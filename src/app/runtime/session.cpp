#include "error.hpp"
#include "field_delivery.hpp"
#include "session_internal.hpp"

namespace voyemu {
using namespace session_detail;

// A marker is a complete console line from its prefix on: the hypervisor writes into the same ring
// in pieces, so a fragment of its output can precede a marker on the line, and a last line without
// its LF may still be in writing.
std::vector<std::string> qnx_startup_markers(std::string_view console,
                                             std::set<std::string> &seen) {
    std::vector<std::string> result;

    for (auto end = console.find('\n'); end != std::string_view::npos; end = console.find('\n')) {
        auto line = console.substr(0, end);
        console.remove_prefix(end + 1);

        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }

        auto start = std::string_view::npos;
        std::size_t prefix = 0;

        for (const std::string_view candidate : {"QMC ", "QIP ", "QC1 ", "QSTORE "}) {
            if (const auto at = line.find(candidate); at < start) {
                start = at;
                prefix = candidate.size();
            }
        }

        if (start == std::string_view::npos) {
            continue;
        }

        const auto marker = line.substr(start);

        if (marker.size() > prefix && marker.size() <= 256 &&
            std::all_of(marker.begin(), marker.end(),
                        [](char c) {
                            return c >= ' ' && c <= '~';
                        }) &&
            seen.size() < 512 && seen.emplace(marker).second) {
            result.emplace_back(marker);
        }
    }

    return result;
}

std::unique_ptr<Session> Session::for_machine_start(std::filesystem::path directory,
                                                    std::string session_id,
                                                    std::filesystem::path current_program) {
    if (!current_program.empty() &&
        (!current_program.is_absolute() || !std::filesystem::is_regular_file(current_program))) {
        throw VoyemuError("LAUNCH_FILE", "current Voyemu executable unavailable");
    }

    auto session = std::unique_ptr<Session>(
            new Session(std::move(directory), std::move(session_id), MachineStart{}));
    session->machine_program_ = std::move(current_program);

    return session;
}

Session::Session(std::filesystem::path directory, std::string session_id, MachineStart)
    : Session(std::move(directory), std::move(session_id), std::filesystem::path{}) {
    machine_start_ = true;
}

Session::Session(std::filesystem::path directory, std::string session_id,
                 std::filesystem::path canonical_state, VehicleCatalog catalog)
    : directory_(std::move(directory)), session_id_(std::move(session_id)),
      guest_auth_token_(random_token()),
      model_(canonical_state.empty() ? directory_ / "vehicle-state.json" : canonical_state,
             catalog) {
    owner_valid(session_id_);

    while (guest_auth_token_ == session_id_) {
        guest_auth_token_ = random_token();
    }

    endpoint_ = session_control_endpoint(directory_, session_id_);
    deliveries_ = {{"android", {{"status", "not_started"}}}, {"qnx", {{"status", "not_started"}}}};
    power_.observe(model_.state());
    deliveries_["power_ecu"] = {{"consumer", "voyemu.power-ecu"},
                                {"policy_id", "voyemu-own-power-v1"},
                                {"state", PowerEcu::name(power_.state())}};
    next_android_ = next_oem_settings_ = next_qnx_ = std::chrono::steady_clock::now();
    write_private_json(directory_ / "vehicle-state.json", model_.state(), vehicle_state_bytes);
    auto creator = os::current_process();
    auto owner_path = directory_ / "session-owner.json";
    Json owner{{"schema", 1},
               {"session_id", session_id_},
               {"directory", std::filesystem::absolute(directory_).lexically_normal().string()},
               {"origin", "voyemu-native-supervisor"},
               {"creator_pid", creator}};

    if (std::filesystem::exists(owner_path)) {
        auto previous = read_private_json(owner_path, 4096);

        if (previous != owner) {
            throw VoyemuError("SESSION_OWNER", "session belongs to another supervisor");
        }
    } else {
        write_private_json(owner_path, owner);
    }
}

Session::~Session() {
    try {
        stop();
    } catch (const std::exception &error) {
        logging::try_write(logging::Level::error, "session", "terminal.stop_failed", "error",
                           error.what());

        if (qnx_persistent_storage_ && cluster_requested_) {
            if (interguest_link_) {
                interguest_link_->clear_ram();
            }

            qnx_.detach();

            for (auto &process : graphics_) {
                process.detach();
            }
        }
    }

    if (android_adb_ && !android_adb_->shutdown()) {
        logging::try_write(logging::Level::error, "session", "terminal.adb_shutdown_timeout");
    }

    try {
        close_qnx_frame_grpc();
    } catch (const std::exception &error) {
        logging::try_write(logging::Level::error, "session", "terminal.frame_grpc_close_failed",
                           "error", error.what());
    }
}

Json Session::snapshot() const {
    auto wheel_routes = ((passion_presentation(android_spec_) || nxp_presentation(android_spec_)) &&
                         !android_spec_.contains("wheel_adapter"))
                                ? Json(nullptr)
                                : EventQueue::routes();

    if (passion_presentation(android_spec_)) {
        for (auto &[id, route] : wheel_routes.items()) {
            route["qnx_index"] = passion_qnx_wheel_index(id) ? Json(passion_qnx_wheel_index(id))
                                                             : Json(nullptr);
        }
    }

    if (android_spec_.contains("wheel_capability")) {
        for (auto &[id, route] : wheel_routes.items()) {
            route["android"] = "unsupported";
        }
    }

    if (android_spec_.contains("wheel_adapter") &&
        android_spec_["wheel_adapter"].contains("routes")) {
        for (const auto &[id, code] : android_spec_["wheel_adapter"]["routes"].items()) {
            if (wheel_routes.contains(id)) {
                wheel_routes[id]["android"] = code.is_null() ? Json("unmapped")
                                                             : Json{{"key_code", code}};
            }
        }
    }

    if (!passion_presentation(android_spec_) && qinggan11_wheel_unsupported(android_spec_)) {
        for (auto &[id, route] : wheel_routes.items()) {
            route["android"] = "unsupported";
        }
    }

    for (const auto &[id, route] : wheel_routes.items()) {
        if (!nxp_presentation(android_spec_) && AdasEcu::handles(id)) {
            wheel_routes[id]["ecu"] = AdasEcu::policy();
        }
    }

    auto observed_deliveries = deliveries_;

    if (passion_mcu_available(android_spec_)) {
        passion_mcu_expire(observed_deliveries["android"], android_transport_boot_,
                           model_.state()["sequence"].get<std::uint32_t>(), passion_mcu_now_ms());
    }

    add_interguest_delivery(observed_deliveries["android"]);

    return {{"ok", true},
            {"schema", 1},
            {"session_id", session_id_},
            {"revision", model_.state()["sequence"]},
            {"state", model_.state()},
            {"catalog", catalog_json(state_catalog(model_.state()))},
            {"field_delivery", field_delivery_status(field_delivery(cluster_spec_, android_spec_))},
            {"deliveries", observed_deliveries},
            {"wheel_routes", wheel_routes},
            {"wheel_capability",
             (android_spec_.contains("wheel_capability") ? android_spec_.at("wheel_capability")
                                                         : Json(nullptr))},
            {"held_buttons", events_.held()},
            {"screens", android_spec_.contains("screens") ? android_spec_.at("screens") : Json()},
            {"endpoint", endpoint_.string()},
            {"android_pid", android_.pid()},
            {"qnx_pid", qnx_.pid()},
            {"stopped", stopped_}};
}

void Session::apply_adas_outcome(AdasOutcome outcome) {
    if (!outcome.handled) {
        return;
    }

    auto before = model_.state().at("sequence");

    if (!outcome.patch.empty()) {
        model_.patch(outcome.patch);
        write_private_json(directory_ / "vehicle-state.json", model_.state(), vehicle_state_bytes);
        schedule_android_state(before.get<std::uint32_t>());

        if (cluster_requested_) {
            deliveries_["qnx"]["vehicle"] = qnx_vehicle_pending();
        }
    }

    outcome.receipt["before_revision"] = before;
    outcome.receipt["after_revision"] = model_.state().at("sequence");
    deliveries_["adas_ecu"] = std::move(outcome.receipt);
}

// The startup sequence is a transition of the power state: ON or READY waits at ACC.
void Session::apply_startup_ignition(const std::optional<StartupIgnition::Change> &change) {
    if (!change) {
        return;
    }

    apply_power_outcome(
            power_.startup(model_.state(), change->values.at("ign") == 1, change->reason));
    const Json receipt{{"ign", model_.state().at("raw_oem").at("ign")},
                       {"reason", change->reason},
                       {"pending", startup_ignition_.pending()},
                       {"revision", model_.state().at("sequence")}};
    deliveries_["startup_ignition"] = receipt;
    logging::write(logging::Level::info, "vehicle", "startup.ignition", receipt);
}

void Session::apply_power_outcome(PowerEcu::Outcome outcome) {
    auto before = model_.state().at("sequence");

    if (!outcome.patch.empty()) {
        model_.patch(outcome.patch);
        write_private_json(directory_ / "vehicle-state.json", model_.state(), vehicle_state_bytes);

        if (cluster_requested_) {
            deliveries_["qnx"]["vehicle"] = qnx_vehicle_pending();
        }
    }

    // The multimedia ACC rides on the next state delivery, OFF<->ACC changes no field.
    const auto acc = [&](const char *key) {
        return PowerEcu::android_acc(*PowerEcu::parse(outcome.receipt.at(key)));
    };

    if (outcome.patch.empty() && acc("previous") != acc("state")) {
        model_.advance_revision(before.get<std::uint32_t>());
        write_private_json(directory_ / "vehicle-state.json", model_.state(), vehicle_state_bytes);
    }

    schedule_android_state(before.get<std::uint32_t>());

    outcome.receipt["before_revision"] = before;
    outcome.receipt["after_revision"] = model_.state().at("sequence");
    deliveries_["power_ecu"] = std::move(outcome.receipt);
    logging::write(logging::Level::info, "vehicle", "power.state", deliveries_["power_ecu"]);
}

std::vector<std::string> Session::android_power_extra() const {
    return {"--ei", "acc", std::to_string(PowerEcu::android_acc(power_.state()))};
}

bool Session::close_passion_display_power() {
    if (!passion_display_power_) {
        return true;
    }

    try {
        passion_display_power_->close();
        passion_display_power_.reset();
        passion_display_power_boot_.clear();
        deliveries_["android"].erase("display_power");
        deliveries_["android"]["display_power_cleanup"] = {{"verified", true}};

        return true;
    } catch (const std::exception &error) {
        next_passion_display_power_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        deliveries_["android"].erase("display_power");
        deliveries_["android"]["display_power_cleanup"] = {{"verified", false},
                                                           {"error", error.what()}};
        frontend_releases_verified_ = false;

        return false;
    }
}

void Session::poll_passion_display_power() {
    if (!media::PassionPowerOwner::enabled(android_spec_)) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (now < next_passion_display_power_) {
        return;
    }

    next_passion_display_power_ = now + std::chrono::milliseconds(100);
    const bool ready = !stopped_ && android_powered_ && android_identity_ &&
                       android_transport_ready_ && android_displays_ready_ &&
                       android_boot_completed_ && android_boot_id_valid(android_transport_boot_) &&
                       deliveries_["android"].value("clock", Json::object()).value("synced", false);

    if (!ready || (!passion_display_power_boot_.empty() &&
                   passion_display_power_boot_ != android_transport_boot_)) {
        if (!close_passion_display_power()) {
            return;
        }

        if (!ready) {
            deliveries_["android"]["display_power"] = {
                    {"schema", 1},
                    {"protocol", "passion-display-power-v1"},
                    {"session_id", session_id_},
                    {"boot_id", android_transport_boot_},
                    {"watch_id", ""},
                    {"capability", media::PassionPowerOwner::capability()},
                    {"states", Json::array()}};

            return;
        }
    }

    if (!passion_display_power_) {
        const auto pid = android_.pid();
        const auto generation = android_generation_;
        const auto boot = android_transport_boot_;
        const auto port = android_spec_.at("port").get<unsigned>();
        // Retained owner and sameboot event fence; no ADB probe on ordinary ON input or poll.
        auto fence = [this, pid, generation, boot, port] {
            if (stopped_ || !android_powered_ || !android_identity_ || pid != android_.pid() ||
                generation != android_generation_ || boot != android_transport_boot_ ||
                !android_.alive() || !android_.owns_tcp_listener(port) ||
                !android_.owns_tcp_listener(port + 1)) {
                throw VoyemuError("OEM_DISPLAY_POWER", "retained SDK incarnation changed");
            }

            (void)owned_android_adb(android_spec_, pid, generation, false);
        };
        const auto watch_directory = directory_ / "display-power-watch";
        ensure_private_directory(watch_directory);
        auto launch = [this, fence](const std::vector<std::string> &args) {
            struct Watch {
                AndroidEmbeddedAdb::Stream stream;
                bool alive = true;
            };
            fence();
            auto watch =
                    std::make_shared<Watch>(Watch{android_adb_->open_shell(args, 30000, 65536)});
            watch->stream.close_stdin();
            fence();

            return media::PassionWatchStream::Child{
                    [watch] {
                        return watch->alive;
                    },
                    [watch](std::size_t limit) {
                        auto result = watch->stream.poll();

                        if (result.terminal) {
                            watch->alive = false;
                        }

                        if (result.output.size() > limit) {
                            throw VoyemuError("TEST_WATCH_EXTENT", "Bounded watch output exceeded");
                        }

                        return std::move(result.output);
                    },
                    [watch] {
                        watch->stream.cancel();
                        watch->alive = false;
                    },
                    {}};
        };
        auto command = [this, fence, pid, generation](const std::vector<std::string> &args,
                                                      int timeout) {
            fence();
            auto reply = execute_android_adb(android_spec_, args, timeout, 4096, pid, generation);
            fence();

            if (reply.timed_out || reply.exit_code || reply.output.size() > 4096) {
                throw VoyemuError("OEM_DISPLAY_POWER", "original wake command failed");
            }

            return Json::parse(reply.output);
        };
        auto monotonic = []() {
            return static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
        };
        // Conservative host UTC comparison is admitted only after existing sameboot clock
        // verification.
        auto utc = []() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                    .count();
        };
        media::PassionPointerWake::Binding binding{true, session_id_, boot, random_token()};
        // Opt-in bounded causal trace. Environment inheritance is the delivery channel already used
        // by VOYEMU_ANDROID_BOOT_TRACE; the value is a bare marker, never a pathname (a typed path
        // is rejected), so no caller, request or profile can choose a destination. The only
        // writable path is this session's own private fixed file. Bounds and the strict guard are
        // enforced by the owner; the trace callback is best-effort and cannot influence a guard
        // decision.
        media::PassionPowerOwner::Trace clock_trace;

        if (const auto filename =
                    passion_clock_trace_file(std::getenv("VOYEMU_PASSION_CLOCK_TRACE"));
            !filename.empty()) {
            const std::filesystem::path trace_path = watch_directory / filename;
            clock_trace = [trace_path](const Json &record) {
                try {
                    if (auto out = std::ofstream(trace_path, std::ios::app)) {
                        out << record.dump() << '\n';
                    }
                } catch (...) { /* Ignored: trace is best-effort. */
                }
            };
        }

        passion_display_power_ = std::make_unique<media::PassionPowerOwner>(
                std::move(binding), fence, launch, command, monotonic, utc, std::move(clock_trace));
        passion_display_power_boot_ = boot;
    }

    try {
        passion_display_power_->poll();
        deliveries_["android"].erase("display_power_error");
    } catch (const std::exception &error) {
        next_passion_display_power_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        deliveries_["android"]["display_power_error"] = error.what();
    }

    deliveries_["android"]["display_power"] = passion_display_power_->record();
    deliveries_["android"]["display_power"]["capability"] = media::PassionPowerOwner::capability();
}

bool Session::read_owned_qnx_ram(std::uint64_t physical, std::span<std::byte> output) {
    std::lock_guard lock(mutex_);
    constexpr std::uint64_t base = 0x40000000, extent = 0x200000000ULL;

    if (output.empty() || output.size() > 0x40000 || physical < base || physical - base >= extent ||
        output.size() > extent - (physical - base)) {
        throw VoyemuError("QNX_RAM_READ", "invalid bounded physical extent");
    }

    if (stopped_ || !qnx_.alive()) {
        return false;
    }

    const auto read_file =
            owned_ram_reader(expand_session_path(cluster_spec_.at("ram").get<std::string>(),
                                                 directory_, session_id_),
                             qnx_ram_binding(cluster_spec_).ram_bytes());

    return read_file && read_file(physical - base, output);
}

void Session::tick() {
    std::unique_lock lifecycle(lifecycle_mutex_);
    std::unique_lock lock(mutex_);

    std::optional<AndroidIdentityScope> identity_scope;

    identity_scope.emplace(android_identity_scope_depth_, android_identity_operation_);

    if (stopped_) {
        return;
    }

    poll_frontend();

    if (test_stop_requested_) {
        if (machine_restart_active_) {
            return; // The restart owns test_stop_mutex_; its next observation records the
                    // cancellation.
        }

        identity_scope.reset();
        lock.unlock();
        lifecycle.unlock();
        stop();

        return;
    }

    if (stopped_) {
        return;
    }

    tick_inputs();
    auto now = std::chrono::steady_clock::now();

    if (cluster_requested_ && now >= next_qnx_) {
        next_qnx_ = now + std::chrono::milliseconds(100);
        try {
            poll_qnx();
            deliveries_["qnx"].erase("last_error");
        } catch (const std::exception &error) {
            deliveries_["qnx"]["last_error"] = error.what();
        }
    }

    bool wrapper_alive = android_spec_.empty() ? false : android_.alive();
    bool android_guest_possible = !android_spec_.empty() && android_powered_;

    if (android_guest_possible) {
        auto port = android_spec_.at("port").get<unsigned>();
        bool ports_free = tcp_port_free(port) && tcp_port_free(port + 1);
        deliveries_["android"]["wrapper"] = {{"pid", android_.pid()},
                                             {"alive", wrapper_alive},
                                             {"exit_code", android_.exit_code()}};
        bool launch_finished =
                android_seen_guest_ ||
                (!wrapper_alive &&
                 (android_.exit_code() != 0 || now - android_started_ > std::chrono::seconds(10)));

        if (ports_free && launch_finished) {
            android_.terminate(1000);
            stop_owned_android_adb();
            verify_android_avd_storage_impl(
                    android_home_, android_spec_.at("machine").get<std::string>(),
                    android_spec_.at("avd_name").get<std::string>(), machine_start_,
                    android_spec_.contains("diagnostic_native"), android_spec_);
            android_guest_possible = false;
            android_identity_ = false;
            deliveries_["android"]["status"] = "exited";
            deliveries_["android"]["guest_listeners_closed"] = true;
            deliveries_["android"]["exit_code"] = android_.exit_code();
            flush_android_releases(true);

            if (!cluster_requested_) {
                stop_frontend();
                stopped_ = true;
                write_private_json(directory_ / "runtime.json",
                                   {{"schema", 1},
                                    {"session_id", session_id_},
                                    {"endpoint", endpoint_.string()},
                                    {"android_pid", android_.pid()},
                                    {"qnx_pid", qnx_.pid()},
                                    {"revision", model_.state()["sequence"]},
                                    {"deliveries", deliveries_},
                                    {"stopped", true}});

                return;
            }
        }
    }

    {
        const Restore io_lock(android_io_state_lock_, &lock);
        advance_android(android_guest_possible, now);
    } // Disable cooperative unlock before cleanup bookkeeping.

    if (android_possible_pressed_.empty() && android_events_.empty() &&
        deliveries_["android"].contains("press_cleanup") &&
        deliveries_["android"]["press_cleanup"].value("status", std::string{}) ==
                "cancel_queue_awaiting_private_receipts") {
        deliveries_["android"]["press_cleanup"]["status"] = "private_cancel_receipts_received";
    }

    poll_passion_display_power();
    Json record{{"schema", 1},
                {"session_id", session_id_},
                {"endpoint", endpoint_.string()},
                {"android_pid", android_.pid()},
                {"qnx_pid", qnx_.pid()},
                {"revision", model_.state()["sequence"]},
                {"deliveries", deliveries_},
                {"stopped", stopped_}};

    if (record != runtime_record_) {
        write_private_json(directory_ / "runtime.json", record);
        runtime_record_ = std::move(record);
    }
}

void Session::stop() {
    trace_session_stop(session_id_, "entry");
    std::lock_guard stop_lock(test_stop_mutex_);
    std::shared_ptr<voyemu::runtime::SourceCoordinator> sources;
    {
        std::lock_guard lock(mutex_);
        cancel_android_identity_probe();
        android_identity_proof_operation_ = 0;

        if (android_identity_mismatch_) {
            throw VoyemuError("ANDROID_IDENTITY_MISMATCH",
                              "stop rejected before lifecycle side effects");
        }

        test_services_closed_ = true;

        if (test_watch_) {
            test_watch_->close();
        }

        if (test_can_watch_) {
            test_can_watch_->close();
        }

        for (const auto &[selection, projection] : test_source_watches_) {
            projection->close();
        }

        sources = test_sources_;
    }
    // Closed test services and later lifecycle steps cannot be undone; a failure past this point
    // has an uncertain outcome.
    try {
        // Wake long polls and join collectors before taking locks needed by collector fences.
        if (sources) {
            sources->stop();
        }

        trace_session_stop(session_id_, "source_collectors_closed");
        std::shared_ptr<UiObserver> observer;
        {
            std::lock_guard lock(mutex_);
            observer = std::exchange(ui_observer_, {});
        }

        if (observer) {
            observer->stop();
        }

        trace_session_stop(session_id_, "observer_closed");
        std::lock_guard lifecycle(lifecycle_mutex_);
        std::unique_lock lock(mutex_);
        trace_session_stop(session_id_, "lifecycle_acquired");

        if (stopped_) {
            return;
        }

        const auto stop_io = lend_android_request_io(lock);
        const Restore stop_deadline(
                android_request_deadline_,
                tighter_deadline(
                        android_request_deadline_,
                        std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(request_timeout_limit_ms("stop"))));
        trace_session_stop(session_id_, "frontend_begin");
        stop_frontend();
        trace_session_stop(session_id_, "frontend_closed");
        close_cluster(true);

        if (interguest_link_) {
            interguest_link_->stop();
        }

        trace_session_stop(session_id_, "cluster_closed");
        trace_session_stop(session_id_, "android_begin");
        stop_android(lock);
        trace_session_stop(session_id_, "android_closed");
        flush_android_releases(true);
        stopped_ = true;
        deliveries_["android"]["status"] = "stopped";
        deliveries_["qnx"]["status"] = "stopped";
        write_private_json(directory_ / "runtime.json", {{"schema", 1},
                                                         {"session_id", session_id_},
                                                         {"endpoint", endpoint_.string()},
                                                         {"android_pid", android_.pid()},
                                                         {"qnx_pid", qnx_.pid()},
                                                         {"revision", model_.state()["sequence"]},
                                                         {"deliveries", deliveries_},
                                                         {"stopped", true}});
        trace_session_stop(session_id_, "complete");
    } catch (const std::exception &error) {
        throw VoyemuError("SESSION_STOP_UNKNOWN", error.what());
    }
}

void Session::discard() {
    {
        std::lock_guard lock(mutex_);

        if (cluster_requested_ && cluster_spec_.value("source_acceptance", false) != true) {
            throw VoyemuError("SESSION_DISCARD", "only source acceptance images are disposable");
        }

        discard_cluster_ = true;
    }
    stop();
}

bool Session::stopped() const {
    std::lock_guard lock(mutex_);

    return stopped_;
}
} // namespace voyemu
