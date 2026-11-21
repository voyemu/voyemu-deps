#include "error.hpp"
#include "session_internal.hpp"

namespace voyemu {
using namespace session_detail;

Json session_detail::host_lifecycle_receipt() {
    return {{"host_wall_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count()},
            {"host_monotonic_us", std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count()}};
}

void session_detail::trace_session_stop(std::string_view session, std::string_view phase) noexcept {
    try {
        auto fields = host_lifecycle_receipt();
        fields["session_id"] = session;
        fields["phase"] = phase;
        logging::write(logging::Level::info, "lifecycle", "session.stop", std::move(fields));
    } catch (...) { /* Ignored: diagnostics do not alter lifecycle delivery. */
    }
}

Json Session::cluster_ui_sample(bool diagnostics, std::chrono::steady_clock::time_point deadline) {
    Json reply = {{"ok", true},
                  {"session_id", session_id_},
                  {"status", "off"},
                  {"generation", cluster_generation_}};
    const auto unavailable = [&](const char *reason) {
        reply["status"] = "unavailable";
        reply["reason"] = reason;

        return reply;
    };

    if (diagnostics) {
        if (cluster_spec_.empty()) {
            return unavailable("cluster_not_configured");
        }

        if (stopped_ || test_services_closed_ || !cluster_requested_) {
            return unavailable("cluster_powered_off");
        }

        if (!qnx_.alive()) {
            return unavailable("cluster_process_exited");
        }
    } else if (stopped_ || test_services_closed_ || !cluster_requested_ || !qnx_.alive()) {
        return reply;
    }

    if (!qnx_kanzi_ui_adapter(
                cluster_boot_spec_.value("hardware_adapter", std::string("sa8155-qinggan09")))) {
        if (diagnostics) {
            return unavailable("unsupported_cluster_adapter");
        }

        throw VoyemuError("TEST_UNSUPPORTED", "cluster has no compatible Kanzi scene provider");
    }

    reply["status"] = "awaiting";

    if (!qmp_cont_) {
        return reply;
    }

    if (qnx_checkpoint_.resume) {
        if (diagnostics) {
            return unavailable("checkpoint_resume");
        }

        throw VoyemuError("TEST_UNSUPPORTED", "Kanzi scene observation requires a cold QNX boot");
    }

    const auto ram = std::filesystem::path(expand_session_path(
            cluster_spec_.at("ram").get<std::string>(), directory_, session_id_));
    constexpr auto bytes = qnx_kanzi_ram_end - qnx_kanzi_ram_base;
    const auto read_file = owned_ram_reader(ram, bytes);

    if (!read_file) {
        return reply;
    }

    const auto read = [&](std::uint64_t address, std::span<std::byte> output) {
        return read_file(address - qnx_kanzi_ram_base, output);
    };
    std::optional<std::pair<std::uint64_t, QnxKanziUiSample>> observation;

    for (std::size_t i = 0; i < graphics_.size(); ++i) {
        if (!graphics_[i].alive()) {
            continue;
        }

        const auto owner = graphics_owners_[i];

        if (auto sample = read_qnx_kanzi_ui(read, producers_[i], owner.pid, owner.incarnation,
                                            {16ULL * 1024 * 1024, deadline})) {
            if (!graphics_[i].alive()) {
                continue;
            }

            if (observation) {
                throw VoyemuError("CLUSTER_UI_PROTOCOL", "ambiguous Gauge scene producer");
            }

            observation.emplace(owner.nonce, std::move(*sample));
        }
    }

    if (!observation || !qnx_.alive()) {
        return reply;
    }

    const auto &[owner, observed] = *observation;

    if (!observed.page.count) {
        return unavailable(qnx_kanzi_ui_reason(observed.page.reason));
    }

    auto nodes = decode_qnx_kanzi_ui(observed, {256ULL * 1024 * 1024, deadline});
    const auto &sample = observed.page;
    reply = {{"ok", true},
             {"session_id", session_id_},
             {"status", "sample"},
             {"generation", cluster_generation_},
             {"owner", owner},
             {"sequence", sample.sequence},
             {"guest_monotonic_ns", sample.guest_monotonic_ns},
             {"heartbeat_ns", sample.heartbeat_ns},
             {"presented_ns", sample.presented_ns},
             {"window", {{"width", sample.window_width}, {"height", sample.window_height}}},
             {"complete", sample.reason == QGFX_KANZI_UI_COMPLETE},
             {"partial_reason", qnx_kanzi_ui_reason(sample.reason)}};

    if (diagnostics) {
        reply["guest_pid"] = sample.guest_pid;
        reply["producer_incarnation"] = sample.incarnation;
        reply["node_count"] = nodes.size();
    } else {
        reply["nodes"] = std::move(nodes);
    }

    return reply;
}

Json Session::handle(const Json &operation) {
    const auto received = std::chrono::steady_clock::now();
    const std::string name = operation.is_object() && operation.contains("op") &&
                                             operation["op"].is_string()
                                     ? operation["op"].get<std::string>()
                                     : std::string{};

    if (name == "stop") {
        trace_session_stop(session_id_, "request_received");
    }

    const bool input = name == "status" || name == "patch" || name == "wheel" ||
                       name == "wheel_pair" || name == "release_owner" ||
                       name.starts_with("test_watch_") || name == "test_output_state" ||
                       name == "test_output_system" || name == "test_output_logs" ||
                       name == "ui_observer_exchange" || name == "test_output_provider" ||
                       name == "test_cluster_ui" || name == "test_cluster_system";
    std::unique_lock lifecycle(lifecycle_mutex_, std::defer_lock);

    if (!input) {
        lifecycle.lock();
    }

    std::unique_lock lock(mutex_);
    Json reply;
    // Every public request has its own proof epoch, including concurrent readonly requests.
    // A nested status cannot inherit mutation authority from a cooperatively waiting caller.
    ++android_identity_operation_;
    std::optional<AndroidIdentityScope> identity_scope;

    if (lifecycle.owns_lock()) {
        identity_scope.emplace(android_identity_scope_depth_, android_identity_operation_);
    }

    const auto request_io = lend_android_request_io(lock);
    const Restore request_deadline(
            android_request_deadline_,
            received + std::chrono::milliseconds(request_timeout_limit_ms(name)));
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - received)
                                .count();

    if (waited > 5) {
        try {
            logging::write(logging::Level::debug, "control", "lock.wait",
                           {{"operation", input ? name : "lifecycle"}, {"wait_ms", waited}});
        } catch (...) { /* Ignored: diagnostics do not change command delivery. */
        }
    }

    const auto request_id = operation.value("request_id", Json(nullptr));
    try {
        if (!operation.is_object() || operation.value("schema", 1) != 1) {
            throw VoyemuError("IPC_SCHEMA", "operation object required");
        }

        bound_android_request(operation, received);

        if (operation.contains("session_id") &&
            operation["session_id"].get<std::string>() != session_id_) {
            throw VoyemuError("IPC_SESSION", "stale session");
        }

        auto op = operation.at("op").get<std::string>();

        if (operation.contains("ownership_id")) {
            if (!operation.contains("session_id")) {
                throw VoyemuError("TEST_SESSION", "explicit session required");
            }

            const auto owner = read_private_json(directory_ / "automation-owner.json", 4096);

            if (owner.value("session_id", std::string{}) != session_id_ ||
                owner.at("ownership_id") != operation.at("ownership_id")) {
                throw VoyemuError("TEST_OWNERSHIP", "foreign automation owner");
            }
        }

        if (active_can_boundary_ && !can_boundary_operation_allowed(operation)) {
            throw VoyemuError("CAN_BOUNDARY_FROZEN", "only observation or owned stop is permitted");
        }

        if (operation.contains("expected_revision") &&
            (!operation.at("expected_revision").is_number_unsigned() ||
             operation.at("expected_revision") != model_.state().at("sequence"))) {
            throw VoyemuError("TEST_REVISION", "stale state revision");
        }

        if (operation.contains("expected_generation") &&
            (!operation.at("expected_generation").is_number_unsigned() ||
             operation.at("expected_generation").get<std::uint64_t>() != cluster_generation_)) {
            throw VoyemuError("TEST_GENERATION", "stale QNX generation");
        }

        if (op == "status") {
            if (operation.contains("owner")) {
                events_.renew(operation.at("owner").get<std::string>());
            }

            reply = snapshot();
        } else if (op == "test_cluster_ui") {
            reply = cluster_ui_sample(
                    false, received + std::chrono::milliseconds(cluster_ui_timeout(operation)));
        } else if (op == "test_cluster_system") {
            if (!operation.contains("session_id")) {
                throw VoyemuError("TEST_SESSION", "explicit session required");
            }

            reply = cluster_ui_sample(
                    true, received + std::chrono::milliseconds(cluster_ui_timeout(operation)));
        } else if (op == "phone_control") {
            reply = phone_control(operation);
        } else if (op == "patch" && operation.at("values").is_object() &&
                   operation.at("values").contains("power")) {
            if ((passion_presentation(android_spec_) && !passion_mcu_available(android_spec_)) ||
                nxp_presentation(android_spec_)) {
                throw VoyemuError("TEST_UNSUPPORTED",
                                  "selected presentation vehicle state delivery is unavailable");
            }

            // One field selects the power state; PowerEcu sets the ignition and READY from it.
            const auto &values = operation.at("values");
            const auto target = PowerEcu::parse(values.at("power"));

            if (values.size() != 1 || !target) {
                throw VoyemuError("VEHICLE_POWER", "power alone with off, acc, on or ready");
            }

            auto outcome = power_.request(model_.state(), *target);

            if (outcome.receipt.at("outcome") == "applied") {
                startup_ignition_.observe_patch(
                        concept_patch(state_catalog(model_.state()), VehicleConcept::ignition,
                                      *target >= PowerEcu::State::on ? 1 : 0));
            }

            apply_power_outcome(std::move(outcome));
            reply = snapshot();
        } else if (op == "patch") {
            if ((passion_presentation(android_spec_) && !passion_mcu_available(android_spec_)) ||
                nxp_presentation(android_spec_)) {
                throw VoyemuError("TEST_UNSUPPORTED",
                                  "selected presentation vehicle state delivery is unavailable");
            }

            auto values = operation.at("values");
            auto candidate = apply_patch(model_.state(), values);
            auto reconciled = adas_.reconcile(candidate);

            // Explicit raw ADAS status injection remains available for firmware display tests.
            if (!values.contains("acc_mode") && !values.contains("mpc_icaSts")) {
                values.update(reconciled.patch);
            }

            auto before = model_.state().at("sequence");
            startup_ignition_.observe_patch(values);
            model_.patch(values);
            power_.observe(model_.state());
            write_private_json(directory_ / "vehicle-state.json", model_.state(),
                               vehicle_state_bytes);
            schedule_android_state(before.get<std::uint32_t>());

            if (cluster_requested_) {
                deliveries_["qnx"]["vehicle"] = qnx_vehicle_pending();
            }

            queue_cluster_language();

            if (!reconciled.patch.empty() && !operation.at("values").contains("acc_mode") &&
                !operation.at("values").contains("mpc_icaSts")) {
                reconciled.receipt["before_revision"] = before;
                reconciled.receipt["after_revision"] = model_.state().at("sequence");
                deliveries_["adas_ecu"] = reconciled.receipt;
            }

            reply = snapshot();
        } else if (op == "wheel" || op == "wheel_pair") {
            if ((passion_presentation(android_spec_) || nxp_presentation(android_spec_)) &&
                !android_spec_.contains("wheel_adapter")) {
                throw VoyemuError("TEST_UNSUPPORTED",
                                  "selected presentation steering-wheel delivery is unavailable");
            }

            const bool unsupported = !passion_presentation(android_spec_) &&
                                     qinggan11_wheel_unsupported(android_spec_);
            const auto button = operation.at("id").get<std::string>();
            const bool free_route = !unsupported && android_spec_.contains("wheel_adapter") &&
                                    android_spec_.at("wheel_adapter")
                                            .value("routes", Json::object())
                                            .contains(button);

            if (android_spec_.contains("wheel_capability") && !free_route &&
                !(cluster_requested_ &&
                  (passion_presentation(android_spec_) ? passion_qnx_wheel_index(button)
                                                       : route_index(button)))) {
                throw VoyemuError("TEST_UNSUPPORTED",
                                  "steering-wheel button has no verified active route");
            }

            if (op == "wheel_pair") {
                require_short_pair_transport();

                if (!operation.at("hold_ms").is_number_integer()) {
                    throw VoyemuError("WHEEL_PAIR", "integer hold required");
                }

                if (qnx_events_.size() + android_events_.size() > 950) {
                    throw VoyemuError("WHEEL_BACKPRESSURE",
                                      "delivery queues await guest acknowledgement");
                }

                const auto hold = operation.at("hold_ms").get<int>();

                const Json pressed_capability = android_spec_.contains("wheel_capability")
                                                        ? android_spec_.at("wheel_capability")
                                                        : Json(nullptr);

                if ((button != "volume_up" && button != "volume_down") ||
                    !qinggan11_partial_wheel_capability_matches(pressed_capability) || hold < 1 ||
                    hold > 200) {
                    throw VoyemuError("WHEEL_PAIR", "verified Free short press required");
                }

                // Validate both phases on a copy before exposing either acceptance.
                auto pending = events_;
                const auto down = pending.submit(button, "down",
                                                 operation.at("owner").get<std::string>());
                const auto up = pending.submit(button, "up", down.owner);
                events_ = std::move(pending);
                android_short_pairs_[down.sequence] = {hold, false};
                tick_inputs();
                auto accepted = [&](const WheelEvent &event) {
                    return Json{{"ok", true},
                                {"accepted", true},
                                {"event_sequence", event.sequence},
                                {"session_id", session_id_},
                                {"revision", model_.state()["sequence"]},
                                {"deliveries",
                                 {{"qnx", event.qnx_index
                                                  ? (cluster_requested_ ? "queued" : "not_started")
                                                  : "unmapped"},
                                  {"android", "queued"},
                                  {"adas_ecu", "not_a_consumer"}}}};
                };
                reply = {{"ok", true},
                         {"session_id", session_id_},
                         {"down", accepted(down)},
                         {"up", accepted(up)},
                         {"released", true},
                         {"guest_acknowledged", false},
                         {"visible_effect_verified", false}};
            } else {
                auto phase = operation.at("phase").get<std::string>();

                if (phase == "down" && (qnx_events_.size() + android_events_.size() > 950)) {
                    throw VoyemuError("WHEEL_BACKPRESSURE",
                                      "delivery queues await guest acknowledgement");
                }

                auto event = events_.submit(operation.at("id").get<std::string>(), phase,
                                            operation.at("owner").get<std::string>());
                tick_inputs();
                std::string android_route = "not_started";

                if (!android_spec_.empty()) {
                    android_route = "unmapped";

                    if (android_spec_.contains("wheel_adapter") &&
                        android_spec_["wheel_adapter"].contains("routes") &&
                        android_spec_["wheel_adapter"]["routes"].contains(event.id) &&
                        !android_spec_["wheel_adapter"]["routes"][event.id].is_null()) {
                        android_route = "queued";
                    }
                }

                reply = {{"ok", true},
                         {"accepted", true},
                         {"event_sequence", event.sequence},
                         {"session_id", session_id_},
                         {"revision", model_.state()["sequence"]},
                         {"deliveries",
                          {{"qnx", event.qnx_index ? (cluster_requested_ ? "queued" : "not_started")
                                                   : "unmapped"},
                           {"android", android_route},
                           {"adas_ecu",
                            !nxp_presentation(android_spec_) && AdasEcu::handles(event.id)
                                    ? "queued"
                                    : "not_a_consumer"}}}};
            }
        } else if (op == "release_owner") {
            events_.release(operation.at("owner").get<std::string>());
            tick_inputs();
            reply = snapshot();
        } else if (op == "close_cluster") {
            if (operation.contains("owner")) {
                events_.release(operation.at("owner").get<std::string>());
            }

            close_cluster();
            reply = snapshot();
        } else if (op == "cluster_power" || op == "cluster_restart") {
            if (cluster_spec_.empty()) {
                throw VoyemuError("QNX_LIFECYCLE", "no prepared cluster configured");
            }

            const bool enabled = op == "cluster_restart" ? true
                                                         : operation.at("enabled").get<bool>();

            if (cluster_requested_ && (!enabled || op == "cluster_restart")) {
                close_cluster(false, op == "cluster_restart" ? "restart" : "power_off");
            }

            if (enabled && !cluster_requested_) {
                cluster_reset_ = op == "cluster_restart";
                attach_cluster(cluster_spec_, cluster_seconds_, true);
            }

            if (!enabled) {
                deliveries_["qnx"]["status"] = "off";
            }

            reply = snapshot();
        } else if (op == "machine_restart") {
            if (!operation.contains("ownership_id") || !operation.contains("session_id")) {
                throw VoyemuError("TEST_OWNERSHIP", "full restart requires explicit owner");
            }

            identity_scope.reset();
            lock.unlock();

            if (lifecycle.owns_lock()) {
                lifecycle.unlock();
            }

            struct Restore {
                std::unique_lock<std::recursive_mutex> &state;

                ~Restore() {
                    if (!state.owns_lock()) {
                        state.lock();
                    }
                }
            } restore{lock};
            reply = restart_machine(operation);
            lock.lock();
        } else if (op == "android_reboot") {
            if (!operation.contains("ownership_id") || !operation.contains("session_id")) {
                throw VoyemuError("TEST_OWNERSHIP", "reboot requires explicit owned session");
            }

            const auto mode = operation.at("mode").get<std::string>();
            validate_android_reboot_mode(mode);
            identity_scope.reset();
            lock.unlock();

            if (lifecycle.owns_lock()) {
                lifecycle.unlock();
            }

            struct Restore {
                std::unique_lock<std::recursive_mutex> &state;

                ~Restore() {
                    if (!state.owns_lock()) {
                        state.lock();
                    }
                }
            } restore{lock};
            reply = reboot_android(mode);
            lock.lock();
        } else if (op == "media_power" || op == "media_restart" || op == "test_media_power") {
            const bool api_power = op == "test_media_power";

            if (api_power &&
                (!operation.contains("session_id") || !operation.contains("ownership_id"))) {
                throw VoyemuError("TEST_OWNERSHIP",
                                  "multimedia power requires explicit owned session");
            }

            if (api_power && (stopped_ || test_services_closed_)) {
                throw VoyemuError("ANDROID_LIFECYCLE", "session is stopping");
            }

            if (android_spec_.empty()) {
                throw VoyemuError("ANDROID_LIFECYCLE", "no prepared multimedia");
            }

            const bool enabled = op == "media_restart" || operation.at("enabled").get<bool>();
            const bool changed = android_powered_ != enabled;
            const auto qnx_generation = cluster_generation_;
            const auto qnx_pid = qnx_.pid();
            const bool qnx_requested = cluster_requested_, qnx_alive = api_power && qnx_.alive();

            if (android_powered_ && (!enabled || op == "media_restart")) {
                // Hide both old frames before any bounded guest shutdown work.
                deliveries_["android"]["status"] = "stopping";
                deliveries_["android"]["powered"] = false;
                write_private_json(directory_ / "runtime.json", {{"schema", 1},
                                                                 {"session_id", session_id_},
                                                                 {"deliveries", deliveries_},
                                                                 {"stopped", false}});
                stop_android(lock);
                flush_android_releases(true);
                android_powered_ = false;
                android_identity_ = false;
                media_connection_written_ = false;
                std::filesystem::remove(directory_ / "media-connection.json");
            }

            if (enabled && !android_powered_) {
                auto specification = android_spec_;
                attach_android(specification, true);
            }

            if (!enabled) {
                deliveries_["android"]["status"] = "off";
                deliveries_["android"]["powered"] = false;
            }

            if (api_power) {
                const bool alive = android_.alive();

                if (android_powered_ != enabled || alive != enabled ||
                    (enabled && !android_generation_) || (!enabled && android_adb_sink_)) {
                    throw VoyemuError(
                            "ANDROID_POWER_UNKNOWN",
                            "owned multimedia process did not reach the requested power state");
                }

                if (cluster_generation_ != qnx_generation || qnx_.pid() != qnx_pid ||
                    cluster_requested_ != qnx_requested || qnx_.alive() != qnx_alive) {
                    throw VoyemuError("ANDROID_POWER_UNKNOWN",
                                      "QNX lifecycle changed during multimedia power");
                }

                reply = {{"ok", true},
                         {"session_id", session_id_},
                         {"enabled", enabled},
                         {"changed", changed},
                         {"android_generation", android_generation_},
                         {"android_pid", alive ? Json(android_.pid()) : Json()},
                         {"process_alive", alive},
                         {"verified", true},
                         {"qnx_unchanged", true},
                         {"source", "supervisor.android_lifecycle"},
                         {"visible_effect_verified", false}};

                if (changed && !enabled) {
                    const auto &shutdown = deliveries_.at("android").at("shutdown");

                    if (shutdown.at("wrapper_terminal") != true ||
                        shutdown.at("ports_released") != true) {
                        throw VoyemuError("ANDROID_POWER_UNKNOWN",
                                          "owned multimedia shutdown receipt is incomplete");
                    }

                    reply["shutdown"] = shutdown;
                }
            } else {
                reply = snapshot();
            }
        } else if (op == "ui_observer") {
            if (android_spec_.empty() || !android_spec_.contains("ui_observer")) {
                throw VoyemuError("TEST_UNSUPPORTED", "profile UI observer is not prepared");
            }

            if (stopped_ || test_services_closed_ || !android_powered_) {
                throw VoyemuError("TREE_STALE", "Android observation is unavailable");
            }

            const auto budget = operation.value("timeout_ms", 5000);

            if (budget < 1 || budget > 30000) {
                throw VoyemuError("TEST_ARGUMENT", "bounded observer preparation required");
            }

            assert_android_identity();
            auto observer = test_ui_observer_locked();
            const auto generation = android_generation_;
            const auto pid = android_.pid();
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                                      received + std::chrono::milliseconds(budget) -
                                      std::chrono::steady_clock::now())
                                      .count();

            if (left <= 0) {
                throw VoyemuError("TREE_TIMEOUT_UNKNOWN", "observer setup deadline elapsed");
            }

            lock.unlock();
            Json connection;
            try {
                connection = observer->connection(static_cast<int>(left),
                                                  operation.value("recover", false));
            } catch (...) {
                lock.lock();
                throw;
            }

            lock.lock();

            if (observer != ui_observer_ || generation != android_generation_ ||
                pid != android_.pid() || !android_powered_) {
                throw VoyemuError("TREE_STALE", "Android changed during observer setup");
            }

            deliveries_["android"]["ui_observer"] = {
                    {"status", "ready"},
                    {"observer_pid", connection.at("observer_pid")},
                    {"boot_id", connection.at("boot_id")}};
            reply = {{"ok", true},
                     {"session_id", session_id_},
                     {"android_pid", android_.pid()},
                     {"connection", connection}};
            write_private_json(directory_ / "ui-observer.json", {{"schema", 1},
                                                                 {"session_id", session_id_},
                                                                 {"android_pid", android_.pid()},
                                                                 {"connection", connection}});
        } else if (op == "ui_observer_exchange") {
            const auto budget = operation.value("timeout_ms", 5000);

            if (budget < 1 || budget > 30000) {
                throw VoyemuError("TEST_ARGUMENT", "bounded observer exchange required");
            }

            const auto &message = operation.at("message");
            const auto command = message.value("op", std::string{});

            if (command != "hello" && command != "ui") {
                throw VoyemuError("TEST_ARGUMENT", "read-only observer operation required");
            }

            if (!operation.contains("session_id") || !ui_observer_ || stopped_ ||
                test_services_closed_ || !android_powered_ ||
                deliveries_["android"].value("status", std::string{}) == "stopping") {
                throw VoyemuError("TREE_STALE", "owned observer is unavailable");
            }

            const auto observer = ui_observer_;
            const auto generation = android_generation_;
            const auto pid = android_.pid();
            const auto specification = android_spec_;
            (void)owned_android_adb(specification, pid, generation);
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                                      received + std::chrono::milliseconds(budget) -
                                      std::chrono::steady_clock::now())
                                      .count();

            if (left <= 0) {
                throw VoyemuError("TREE_TIMEOUT_UNKNOWN", "observer control deadline elapsed");
            }

            lock.unlock();
            Json observation;
            try {
                observation = observer->exchange(operation.at("connection"), message,
                                                 static_cast<int>(left));
            } catch (...) {
                lock.lock();
                throw;
            }

            lock.lock();

            if (observer != ui_observer_ || generation != android_generation_ ||
                pid != android_.pid() || test_services_closed_ || !android_powered_ ||
                deliveries_["android"].value("status", std::string{}) == "stopping") {
                throw VoyemuError("TREE_STALE", "Android changed during observer exchange");
            }

            (void)owned_android_adb(specification, pid, generation);
            reply = {{"ok", true},
                     {"session_id", session_id_},
                     {"observation", std::move(observation)}};
        } else if (op == "test_output_state" || op == "test_output_system" ||
                   op == "test_output_provider" || op == "test_watch_start" ||
                   op == "test_watch_read" || op == "test_watch_stop") {
            if (!operation.contains("session_id")) {
                throw VoyemuError("TEST_SESSION", "explicit session required");
            }

            const auto timeout = operation.value("timeout_ms", 1000);

            if (timeout < 0 || timeout > 60000) {
                throw VoyemuError("TEST_ARGUMENT", "timeout must be 0..60000 ms");
            }

            if (op == "test_output_provider" &&
                (!test_providers_ ||
                 !test_providers_->contains(operation.at("source").get<std::string>()))) {
                throw VoyemuError("PROVIDER_NOT_FOUND", "source is not registered");
            }

            ensure_test_services();
            auto sources = test_sources_;
            auto hub = test_watch_;
            std::vector<std::string> selected;

            if (op != "test_watch_read" && op != "test_watch_stop") {
                if (operation.contains("sources") && operation.at("sources").is_null()) {
                    throw VoyemuError("TEST_ARGUMENT", "sources must be an array");
                }

                Json selection = operation.value("sources", Json());

                if (op == "test_output_system") {
                    selection = Json::array({"system"});
                }

                if (op == "test_output_provider") {
                    selection = Json::array({operation.at("source")});
                }

                selected = sources->select(selection);
            }

            if (op == "test_watch_start") {
                voyemu::runtime::validate_condition(operation.at("condition"));
                const auto lease = operation.value("lease_ms", 30000);

                if (lease < 1 || lease > 60000) {
                    throw VoyemuError("TEST_ARGUMENT", "lease must be 1..60000 ms");
                }

                auto found = test_source_watches_.find(selected);

                if (found == test_source_watches_.end()) {
                    if (test_source_watches_.size() >= 8) {
                        auto retired = std::find_if(
                                test_source_watches_.begin(), test_source_watches_.end(),
                                [&](const auto &entry) {
                                    return entry.second != test_can_watch_ &&
                                           !entry.second->active(
                                                   voyemu::runtime::SourceCoordinator::now_ms());
                                });

                        if (retired == test_source_watches_.end()) {
                            throw VoyemuError("TEST_BUSY", "selected source watch limit");
                        }

                        retired->second->close();
                        sources->remove_projection(*retired->second);
                        test_source_watches_.erase(retired);
                    }

                    auto projection = std::make_shared<voyemu::runtime::WatchHub>();
                    sources->publish_to(*projection, selected);
                    found = test_source_watches_.emplace(selected, std::move(projection)).first;
                }

                hub = found->second;
            }

            if (op == "test_watch_read" || op == "test_watch_stop") {
                hub.reset();

                for (const auto &[selection, projection] : test_source_watches_) {
                    if (operation.at("epoch").get<std::string>() == projection->epoch()) {
                        hub = projection;
                        break;
                    }
                }

                if (!hub) {
                    throw VoyemuError("TEST_WATCH_EPOCH", "stale watch epoch");
                }
            }

            identity_scope.reset();
            lock.unlock();

            if (lifecycle.owns_lock()) {
                lifecycle.unlock();
            }

            struct Restore {
                std::unique_lock<std::recursive_mutex> &state;

                ~Restore() {
                    if (!state.owns_lock()) {
                        state.lock();
                    }
                }
            } restore{lock};

            if (op == "test_watch_stop") {
                hub->stop(operation.at("watch_id").get<std::string>());
                reply = {{"ok", true}, {"status", "stopped"}, {"epoch", hub->epoch()}};
            } else if (op == "test_watch_read") {
                const auto limit = operation.value("limit", 1);

                if (limit < 1 || limit > 32) {
                    throw VoyemuError("TEST_ARGUMENT", "event limit must be 1..32");
                }

                // Each entry is returned whole. A transport oversize is explicit, never cursor
                // truncation.
                const auto result = hub->read(operation.at("watch_id").get<std::string>(),
                                              operation.at("cursor").get<std::string>(),
                                              std::chrono::milliseconds(timeout), limit,
                                              voyemu::runtime::SourceCoordinator::now_ms());
                Json events = Json::array();

                for (const auto &event : result.events) {
                    events.push_back({{"cursor", event.cursor},
                                      {"state", event.observation->state},
                                      {"epoch", event.observation->epoch},
                                      {"continuous", event.observation->continuous},
                                      {"gap", event.observation->gap}});
                }

                reply = {{"ok", true},
                         {"status", result.status},
                         {"epoch", result.epoch},
                         {"cursor", result.cursor},
                         {"gap", result.gap},
                         {"result", result.result.json()},
                         {"events", events}};

                if (reply.dump().size() > 120 * 1024) {
                    throw VoyemuError("TEST_WATCH_SIZE",
                                      "reduce event limit; cursor was not acknowledged");
                }
            } else {
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(timeout);
                test_demand_until_.store(voyemu::runtime::SourceCoordinator::now_ms() + timeout +
                                         1000);
                auto state = sources->snapshot(std::chrono::milliseconds(timeout), selected);

                if (op == "test_watch_start" && hub == test_can_watch_) {
                    const auto coverage = state.value("sources", Json::object())
                                                  .value("can", Json::object())
                                                  .value("coverage", Json::object());
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   deadline - std::chrono::steady_clock::now())
                                                   .count();

                    // A first journal baseline has no prior coverage. Drain once more inside the
                    // original deadline.
                    if (!coverage.value("continuous", false) && remaining > 0) {
                        state = sources->snapshot(std::chrono::milliseconds(remaining), selected);
                    }
                }

                if (op == "test_watch_start") {
                    const auto lease = operation.value("lease_ms", 30000);

                    if (lease < 1 || lease > 60000) {
                        throw VoyemuError("TEST_ARGUMENT", "lease must be 1..60000 ms");
                    }

                    std::size_t active = 0;
                    std::lock_guard state_lock(mutex_);

                    for (const auto &[selection, projection] : test_source_watches_) {
                        active += projection->active(voyemu::runtime::SourceCoordinator::now_ms());
                    }

                    if (active >= 64) {
                        throw VoyemuError("TEST_BUSY", "session watch limit");
                    }

                    auto ready = hub->start(operation.at("condition"), lease,
                                            voyemu::runtime::SourceCoordinator::now_ms());
                    reply = {{"ok", true},
                             {"status", ready.status},
                             {"watch_id", ready.watch_id},
                             {"epoch", ready.epoch},
                             {"cursor", ready.cursor},
                             {"result", ready.result.json()},
                             {"baseline", state}};
                } else {
                    reply = {{"ok", true},
                             {"state",
                              (op == "test_output_system" || op == "test_output_provider")
                                      ? state.value("sources", Json::object())
                                                .value(op == "test_output_system"
                                                               ? std::string("system")
                                                               : operation.at("source")
                                                                         .get<std::string>(),
                                                       Json::object())
                                      : state}};
                }
            }

            lock.lock();
            reply["session_id"] = session_id_;
        } else if (op == "test_output_logs") {
            if (!operation.contains("session_id") || android_spec_.empty() || !android_powered_) {
                throw VoyemuError("TEST_LOGS_UNAVAILABLE", "owned Android guest required");
            }

            const int timeout = operation.value("timeout_ms", 5000);

            if (timeout < 1 || timeout > 10000) {
                throw VoyemuError("TEST_ARGUMENT", "log timeout must be 1..10000 ms");
            }

            const auto source = operation.value("source", std::string("android"));
            const bool retained = source == "anr" || source == "native";

            if (source != "android" && !retained) {
                throw VoyemuError("TEST_ARGUMENT",
                                  "diagnostic source must be android, anr or native");
            }

            const auto tombstone = diagnostic_tombstone(operation, source);
            const auto specification = android_spec_;
            const auto port = specification.at("port").get<unsigned>();
            const auto pid = android_.pid();
            const auto generation = android_generation_;
            const auto nonce = android_boot_nonce_, guest_auth = guest_auth_token_,
                       bearer = sdk_descriptor_.bearer();
            const auto resumed = android_resume_;

            if (!android_.owns_tcp_listener(port) || !android_.owns_tcp_listener(port + 1)) {
                throw VoyemuError("TEST_LOGS_STALE", "owned SDK listeners required");
            }

            struct Relock {
                std::unique_lock<std::recursive_mutex> &value;

                ~Relock() {
                    if (!value.owns_lock()) {
                        value.lock();
                    }
                }
            } relock{lock};
            lock.unlock();
            const auto deadline = received + std::chrono::milliseconds(timeout);
            auto read = [&](std::vector<std::string> arguments) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                                               deadline - std::chrono::steady_clock::now())
                                               .count();

                if (remaining <= 0) {
                    throw VoyemuError("TEST_LOGS_TIMEOUT", "fixed log read deadline expired");
                }

                constexpr std::size_t limit = 128 * 1024;
                ProcessResult result;

                if (retained && arguments.front() == "shell") {
                    const auto transport = owned_android_adb(specification, pid, generation);
                    result = transport->run(arguments, static_cast<int>(remaining), limit);
                    (void)owned_android_adb(specification, pid, generation);
                } else {
                    result = execute_android_adb(specification, arguments,
                                                 static_cast<int>(remaining), limit, pid,
                                                 generation);
                }

                if (result.exit_code || result.timed_out) {
                    throw VoyemuError("TEST_LOGS_UNAVAILABLE", "fixed Android read failed");
                }

                return result.output;
            };
            auto line = [](std::string value) {
                auto end = value.find_first_of("\r\n");

                if (end != std::string::npos) {
                    value.resize(end);
                }

                return value;
            };
            const auto token = line(read({"shell", "getprop", "qemu.voyemu.token"}));

            if (retained ? (nonce.empty() || token != nonce)
                         : (token != session_id_ && (!resumed || token != nonce))) {
                throw VoyemuError("TEST_LOGS_STALE", "foreign SDK guest");
            }

            if (line(read({"emu", "avd", "name"})) !=
                specification.at("avd_name").get<std::string>()) {
                throw VoyemuError("TEST_LOGS_STALE", "foreign AVD");
            }

            const auto boot = line(read({"shell", "cat", "/proc/sys/kernel/random/boot_id"}));

            if (!android_boot_id_valid(boot)) {
                throw VoyemuError("TEST_LOGS_STALE", "complete guest boot required");
            }

            if (retained && (operation.value("android_pid", std::int64_t{}) != pid ||
                             operation.value("boot_id", std::string{}) != boot)) {
                throw VoyemuError("TEST_LOGS_STALE", "diagnostic epoch differs");
            }

            auto logs = retained ? collect_retained_android_diagnostics(
                                           source, tombstone,
                                           {guest_auth, nonce, session_id_, bearer}, read)
                                 : parse_android_logs(read({"shell",
                                                            "logcat",
                                                            "-d",
                                                            "-b",
                                                            "main",
                                                            "-b",
                                                            "system",
                                                            "-b",
                                                            "crash",
                                                            "-v",
                                                            "epoch",
                                                            "-t",
                                                            "128",
                                                            "ActivityManager:W",
                                                            "AndroidRuntime:E",
                                                            "libc:F",
                                                            "art:F",
                                                            "DEBUG:F",
                                                            "dex2oat:E",
                                                            "app_process:F",
                                                            "*:S"}),
                                                      operation.value("details", false),
                                                      {guest_auth});

            if (line(read({"shell", "cat", "/proc/sys/kernel/random/boot_id"})) != boot ||
                (retained && line(read({"shell", "getprop", "qemu.voyemu.token"})) != token)) {
                throw VoyemuError("TEST_LOGS_STALE", "guest identity changed during snapshot");
            }

            lock.lock();

            if (generation != android_generation_ || pid != android_.pid() || !android_powered_ ||
                !android_.owns_tcp_listener(port) || !android_.owns_tcp_listener(port + 1)) {
                throw VoyemuError("TEST_LOGS_STALE", "Android ownership changed");
            }

            logs["boot_id"] = boot;

            if (retained) {
                if (source == "native") {
                    logs["handler_diagnostics"]["collection_epoch"] =
                            session_id_ + ":" + std::to_string(pid) + ":" + boot;
                }

                logs["android_pid"] = pid;
                logs["session_id"] = session_id_;
                logs["collector"] = "owned_host_client";
                logs["revision"] = sha256_bytes(logs.dump());
            } else {
                logs["epoch"] = session_id_ + ":" + std::to_string(generation) + ":" + boot;
                logs["revision"] = sha256_bytes(logs.at("events").dump());
            }

            reply = {{"ok", true}, {"session_id", session_id_}, {"logs", logs}};
        } else if (op == "test_can_observation" || op == "test_ipk_history") {
            if (!operation.contains("session_id")) {
                throw VoyemuError("TEST_SESSION", "explicit session required");
            }

            const bool ipk = op == "test_ipk_history";
            const auto query = operation.at("query");

            if (ipk) {
                validate_ipk_request(query);
            } else {
                validate_can_request(query);
            }

            const int timeout = operation.value("timeout_ms", 5000);

            if (android_spec_.empty() || !android_powered_) {
                reply = {{"ok", true},
                         {"session_id", session_id_},
                         {"observation",
                          {{"schema", 1},
                           {"source", ipk ? "android:qg.canbus:ipk" : "android:qg.canbus"},
                           {"status", "unavailable"},
                           {"reason", "owned Android guest is not powered"}}}};
            } else {
                const auto port = android_spec_.at("port").get<unsigned>();

                if (!android_.owns_tcp_listener(port) || !android_.owns_tcp_listener(port + 1)) {
                    throw VoyemuError("ANDROID_IDENTITY_MISMATCH",
                                      "CAN observation requires current owned SDK listeners");
                }

                const auto pid = android_.pid();
                const auto generation = android_generation_;
                const CanGuestIdentity identity{session_id_,
                                                android_spec_.at("avd_name").get<std::string>(),
                                                android_boot_nonce_, android_resume_};
                require_unborrowed_state_lock();
                // Lifecycle still fences Android replacement. Input/state access remains
                // independent.
                struct Relock {
                    std::unique_lock<std::recursive_mutex> &value;

                    ~Relock() {
                        if (!value.owns_lock()) {
                            value.lock();
                        }
                    }
                } relock{lock};
                lock.unlock();
                const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - received)
                                           .count();

                if (spent >= timeout) {
                    throw VoyemuError("CAN_TIMEOUT", "source deadline expired before collection");
                }

                const auto runner = [this](const auto &args, int left) {
                    return adb(args, left);
                };
                const auto left = timeout - static_cast<int>(spent);
                auto observed = ipk ? collect_ipk_history(query, identity, left, runner)
                                    : collect_can_observation(query, identity, left, runner);
                lock.lock();

                if (generation != android_generation_ || pid != android_.pid() ||
                    !android_powered_ || !android_.owns_tcp_listener(port) ||
                    !android_.owns_tcp_listener(port + 1)) {
                    throw VoyemuError("CAN_STALE", "Android ownership changed during collection");
                }

                reply = {{"ok", true},
                         {"session_id", session_id_},
                         {"android_pid", pid},
                         {"observation", std::move(observed)}};
            }
        } else if (op == "test_can_history") {
            if (!operation.contains("session_id")) {
                throw VoyemuError("TEST_SESSION", "explicit session required");
            }

            const int timeout = operation.value("timeout_ms", 5000);

            if (timeout < 1 || timeout > 15000) {
                throw VoyemuError("CAN_BOUNDARY_ARGUMENT", "history timeout 1..15000");
            }

            const auto unavailable = [&](const std::string &reason) {
                return Json{{"schema", 1},
                            {"producer", "closed_ecu_boundary"},
                            {"capability", "closed_ecu_boundary_v1"},
                            {"session_id", session_id_},
                            {"status", "unavailable"},
                            {"reason", reason}};
            };
            Json observed;
            observed = observe_can_boundary(unavailable, lock, received, timeout);
            reply = {{"ok", true},
                     {"session_id", session_id_},
                     {"observation", std::move(observed)}};
        } else if (op == "test_can_model_observation") {
            if (!operation.contains("session_id")) {
                throw VoyemuError("TEST_SESSION", "explicit session required");
            }

            const auto query = operation.at("query");
            validate_can_model_request(query);
            const int timeout = operation.value("timeout_ms", 5000);

            if (android_spec_.empty() || !android_powered_) {
                reply = {{"ok", true},
                         {"session_id", session_id_},
                         {"observation",
                          {{"schema", 1},
                           {"source", "android:can_model"},
                           {"status", "unavailable"},
                           {"reason", "owned Android guest is not powered"}}}};
            } else {
                if (!android_spec_.contains("can_model_observer_sha256")) {
                    throw VoyemuError("CAN_MODEL_UNAVAILABLE", "installed helper proof missing");
                }

                const auto helper_sha =
                        android_spec_.at("can_model_observer_sha256").get<std::string>();
                const auto port = android_spec_.at("port").get<unsigned>();

                if (!android_.owns_tcp_listener(port) || !android_.owns_tcp_listener(port + 1)) {
                    throw VoyemuError("ANDROID_IDENTITY_MISMATCH",
                                      "CAN observation requires current owned SDK listeners");
                }

                const auto pid = android_.pid();
                const auto generation = android_generation_;
                const CanGuestIdentity identity{session_id_,
                                                android_spec_.at("avd_name").get<std::string>(),
                                                android_boot_nonce_, android_resume_};
                require_unborrowed_state_lock();
                // Lifecycle still fences Android replacement. Input/state access remains
                // independent.
                struct Relock {
                    std::unique_lock<std::recursive_mutex> &value;

                    ~Relock() {
                        if (!value.owns_lock()) {
                            value.lock();
                        }
                    }
                } relock{lock};
                lock.unlock();
                const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - received)
                                           .count();

                if (spent >= timeout) {
                    throw VoyemuError("CAN_TIMEOUT", "source deadline expired before collection");
                }

                auto observed = collect_can_model_observation(query, identity, helper_sha,
                                                              timeout - static_cast<int>(spent),
                                                              [this](const auto &args, int left) {
                                                                  return adb(args, left);
                                                              });
                lock.lock();

                if (generation != android_generation_ || pid != android_.pid() ||
                    !android_powered_ || !android_.owns_tcp_listener(port) ||
                    !android_.owns_tcp_listener(port + 1)) {
                    throw VoyemuError("CAN_STALE", "Android ownership changed during collection");
                }

                reply = {{"ok", true},
                         {"session_id", session_id_},
                         {"android_pid", pid},
                         {"observation", std::move(observed)}};
            }
        } else if (op == "test_boot_identity") {
            if (!operation.contains("session_id") || android_spec_.empty() || !android_identity_) {
                throw VoyemuError("TEST_BOOT_UNAVAILABLE", "owned Android identity is not ready");
            }

            assert_android_identity();
            const auto port = android_spec_.at("port").get<unsigned>();

            if (!android_.owns_tcp_listener(port) || !android_.owns_tcp_listener(port + 1)) {
                throw VoyemuError("ANDROID_IDENTITY_MISMATCH",
                                  "boot observation requires current owned SDK listeners");
            }

            // On Windows assert_android_identity consumed a complete fresh boot/nonce frame in this
            // operation; android_boot_id reuses that exact proof, never a prior sample.
            const auto observed = android_boot_id();
            const auto id = observed.value_or(std::string{});

            if (!observed || id.size() != 36 || id[8] != '-' || id[13] != '-' || id[18] != '-' ||
                id[23] != '-' || id.find_first_not_of("0123456789abcdef-") != std::string::npos) {
                throw VoyemuError("TEST_BOOT_UNAVAILABLE",
                                  "complete Linux boot identity unavailable");
            }

            reply = {{"ok", true},
                     {"session_id", session_id_},
                     {"android_pid", android_.pid()},
                     {"boot_id", id},
                     {"guest_nonce_sha256", sha256_bytes(android_boot_nonce_)},
                     {"source", "owned Linux /proc/sys/kernel/random/boot_id"}};
            reply["owned_adb"] = android_spec_.at("owned_adb");
        } else if (op == "test_app_launch") {
            if (!operation.contains("ownership_id") || !operation.contains("session_id")) {
                throw VoyemuError("TEST_OWNERSHIP", "app launch requires explicit owned session");
            }

            const auto package = operation.at("package").get<std::string>(),
                       screen = operation.at("screen").get<std::string>(),
                       boot_id = operation.at("boot_id").get<std::string>();
            validate_app_launch(package, screen, boot_id);

            if (stopped_ || !android_powered_ || android_spec_.empty() || !android_identity_) {
                throw VoyemuError("APP_UNAVAILABLE", "owned Android guest is not ready");
            }

            const auto pid = android_.pid();
            const auto generation = android_generation_;
            const auto port = android_spec_.at("port").get<unsigned>();
            unsigned display = 0;

            if (screen == "secondary") {
                const auto mapping = deliveries_.at("android").value("displays", Json::object());

                if (!android_displays_ready_ ||
                    mapping.value("status", std::string{}) != "both_guest_displays_ready" ||
                    !mapping.contains("secondary_display_id")) {
                    throw VoyemuError("APP_DISPLAY", "secondary display is not ready");
                }

                display = mapping.at("secondary_display_id").get<unsigned>();
            }

            auto fence = [&]() {
                if (pid != android_.pid() || generation != android_generation_ ||
                    !android_powered_ || !android_.alive() || !android_.owns_tcp_listener(port) ||
                    !android_.owns_tcp_listener(port + 1)) {
                    throw VoyemuError("APP_STALE", "owned Android process or listeners changed");
                }

                assert_android_identity();
                const auto observed = android_boot_id();
                const auto id = observed.value_or(std::string{});

                if (!observed || !android_boot_id_valid(id)) {
                    throw VoyemuError("APP_UNAVAILABLE", "owned Linux boot identity unavailable");
                }

                return id;
            };
            reply = launch_android_app(package, screen, boot_id,
                                       operation.value("component", std::string{}), display, fence,
                                       [this](const auto &args, int timeout) {
                                           return adb(args, timeout);
                                       });
            reply["session_id"] = session_id_;
            reply["android_pid"] = pid;
        } else if (op == "media_screen") {
            if (!passion_display_power_ || !media::PassionPowerOwner::enabled(android_spec_)) {
                throw VoyemuError("OEM_DISPLAY_POWER", "prepared service unavailable");
            }

            reply = passion_display_power_->handle(operation);
        } else if (op == "media_input") {
            if (!operation.contains("ownership_id") || !media_input_) {
                throw VoyemuError("TEST_UNSUPPORTED", "owned media input adapter unavailable");
            }

            prepare_android_media_input();
            const auto &input = operation.at("input");
            std::optional<Json> consumed;

            if (media::PassionPowerOwner::enabled(android_spec_) &&
                input.value("kind", std::string{}) == "pointer") {
                if (!passion_display_power_) {
                    throw VoyemuError("OEM_DISPLAY_POWER", "asynchronous initial state required");
                }

                consumed = passion_display_power_->gesture(input);
            }

            reply = consumed ? *consumed : media_input_(input);
            reply["session_id"] = session_id_;
        } else if (op == "stop") {
            identity_scope.reset();
            lock.unlock();

            if (lifecycle.owns_lock()) {
                lifecycle.unlock();
            }

            stop();
            lock.lock();
            reply = snapshot();
        } else {
            throw VoyemuError("IPC_OPERATION", "unknown closed operation");
        }

        if (name == "test_cluster_ui" && reply.contains("nodes")) {
            reply["request_id"] = request_id;
            bound_qnx_kanzi_ui_reply(
                    reply, qnx_kanzi_ui_response_bytes,
                    received + std::chrono::milliseconds(cluster_ui_timeout(operation)));
        }
    } catch (const std::exception &error) {
        reply = {{"ok", false},
                 {"session_id", session_id_},
                 {"error", error.what()},
                 {"revision", model_.state()["sequence"]}};
    }

    reply["request_id"] = request_id;

    return reply;
}
} // namespace voyemu
