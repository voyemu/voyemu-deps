#include "error.hpp"
#include "session_internal.hpp"

namespace voyemu {
using namespace session_detail;

void Session::configure_cluster(const Json &specification, std::optional<unsigned> seconds) {
    std::lock_guard lifecycle(lifecycle_mutex_);
    std::lock_guard lock(mutex_);

    if (cluster_requested_) {
        throw VoyemuError("QNX_LIFECYCLE", "active cluster already configured");
    }

    if (!specification.is_object() || specification.value("schema", 0) != 1 ||
        !specification.contains("pins") || !specification["pins"].is_array() ||
        !qnx_cluster_seconds_valid(host_qnx_engine, seconds, specification)) {
        throw VoyemuError("QNX_LIFECYCLE", "prepared cluster configuration required");
    }

    // Hash-check and claim guest resources only when the user actually starts QNX.
    cluster_spec_ = specification;
    qnx_checkpoint_disabled_ = false;
    cluster_seconds_ = seconds;
    deliveries_["qnx"] = {{"status", "off"},
                          {"lifecycle_supported", true},
                          {"generation", cluster_generation_},
                          {"bounded", seconds.has_value()},
                          {"bounded_seconds", seconds.value_or(0)}};
}

void Session::publish_graphics_owners() {
    const auto previous = graphics_owner_challenge_;

    do {
        graphics_owner_challenge_ = std::stoull(random_token().substr(0, 16), nullptr, 16);
    } while (!graphics_owner_challenge_ || graphics_owner_challenge_ == previous);

    Json owners = Json::array();

    for (std::size_t i = 0; i < graphics_.size(); ++i) {
        const auto owner = graphics_owners_.at(i);
        owners.push_back({{"nonce", owner.nonce},
                          {"producer_address", producers_.at(i)},
                          {"worker_pid", graphics_[i].pid()},
                          {"guest_pid", owner.pid},
                          {"producer_incarnation", owner.incarnation}});
    }

    write_private_json(directory_ / "active-owners.json", {{"schema", 1},
                                                           {"session_id", session_id_},
                                                           {"generation", cluster_generation_},
                                                           {"challenge", graphics_owner_challenge_},
                                                           {"owners", owners}});
}

void Session::retire_dead_graphics(
        const std::function<std::size_t(std::uint64_t, std::span<std::byte>)> &read_ram) {
    const auto frame_owner = directory_ / "frame-owner.json";

    if (!graphics_owner_challenge_ || !std::filesystem::exists(frame_owner)) {
        return;
    }

    const auto claim = read_private_json(frame_owner, 1024);

    if (claim.value("schema", 0) != 1 ||
        claim.value("source", std::string{}) != "original-OEM-Screen-BMP" ||
        !claim.contains("producer_address")) {
        return;
    }

    const auto observer = std::find(producers_.begin(), producers_.end(),
                                    u64(claim.at("producer_address")));

    if (observer == producers_.end()) {
        return;
    }

    const auto observer_index = std::size_t(observer - producers_.begin());

    if (!graphics_[observer_index].alive()) {
        return;
    }

    const auto observer_address = *observer;
    const auto observer_owner = graphics_owners_.at(observer_index);
    const auto observer_pid = graphics_[observer_index].pid();
    bool changed = false;

    for (std::size_t i = 0; i < graphics_.size();) {
        const auto owner = graphics_owners_.at(i);

        if (!owner.pid || !owner.incarnation || producers_[i] == observer_address) {
            ++i;
            continue;
        }

        const auto path = directory_ / ("owner-death-" + std::to_string(owner.nonce) + ".json");

        if (!std::filesystem::exists(path)) {
            ++i;
            continue;
        }

        const auto receipt = read_private_json(path, 4096);
        const Json expected{{"schema", 1},
                            {"session_id", session_id_},
                            {"generation", cluster_generation_},
                            {"challenge", graphics_owner_challenge_},
                            {"nonce", owner.nonce},
                            {"producer_address", producers_[i]},
                            {"worker_pid", graphics_[i].pid()},
                            {"guest_pid", owner.pid},
                            {"producer_incarnation", owner.incarnation},
                            {"result", -1},
                            {"errno", 3},
                            {"observer_producer_address", observer_address},
                            {"observer_worker_pid", observer_pid},
                            {"observer_guest_pid", observer_owner.pid},
                            {"observer_producer_incarnation", observer_owner.incarnation},
                            {"observer_nonce", observer_owner.nonce}};

        if (receipt != expected) {
            auto stale = expected;
            stale["challenge"] = receipt.value("challenge", std::uint64_t{0});

            if (receipt == stale && receipt.at("challenge") != graphics_owner_challenge_) {
                const auto target = directory_ / "owner-observation-history" /
                                    std::to_string(cluster_generation_);
                ensure_private_directory(target);
                terminal_path_guard(path);
                std::filesystem::rename(path,
                                        target / (std::to_string(owner.nonce) + "-" +
                                                  std::to_string(u64(receipt.at("challenge"))) +
                                                  "-" + random_token() + ".json"));
            }

            ++i;
            continue;
        }

        std::array<std::uint32_t, 2> header{};
        std::array<std::uint64_t, 2> identity{};

        if (read_ram(producers_[i], std::as_writable_bytes(std::span(header))) != sizeof(header) ||
            header[0] != 0x58464751 || header[1] != 3 ||
            read_ram(producers_[i] + 240, std::as_writable_bytes(std::span(identity))) !=
                    sizeof(identity) ||
            identity[0] != owner.pid || identity[1] != owner.incarnation) {
            ++i;
            continue;
        }

        // Only a fresh canonical Screen ESRCH observation authorizes stopping this retained child.
        graphics_[i].terminate(1000);

        if (graphics_[i].alive()) {
            throw VoyemuError("QNX_OWNER_DEATH", "owned graphics worker remained alive");
        }

        retire_graphics_worker(i, true, "guest_pid_esrch", false);
        changed = true;
    }

    if (changed) {
        publish_graphics_owners();
    }
}

void Session::retire_graphics_worker(std::size_t index, bool unclean, const char *reason,
                                     bool republish) {
    const auto address = producers_.at(index);
    const auto owner = graphics_owners_.at(index);
    const auto target = directory_ / "producer-history" / std::to_string(cluster_generation_) /
                        std::to_string(owner.nonce);

    ensure_private_directory(target);
    auto archive = [&](const std::filesystem::path &path) {
        if (!std::filesystem::exists(path)) {
            return;
        }

        terminal_path_guard(path);
        std::filesystem::rename(path, target / path.filename());
    };

    for (const auto *name : {"frame-owner.json", "control-owner.json"}) {
        const auto claim = directory_ / name;

        if (!std::filesystem::exists(claim)) {
            continue;
        }

        const auto value = read_private_json(claim, 1024);

        if (value.value("schema", 0) != 1 || !value.contains("producer_address") ||
            u64(value.at("producer_address")) != address) {
            continue;
        }

        if (std::string_view(name) == "frame-owner.json") {
            frames::invalidate(directory_ / "frame.bgra");
            archive(directory_ / "frame.bgra");
        } else {
            qnx_retired_receiver_ = {
                    {"producer_address", address}, {"generation", cluster_generation_},
                    {"guest_pid", owner.pid},      {"producer_incarnation", owner.incarnation},
                    {"unclean", unclean},          {"retirement", reason}};
        }

        archive(claim);
    }

    std::ostringstream producer;
    producer << "producer-" << std::hex << address;
    frames::invalidate(directory_ / producer.str() / "frame.bgra");
    archive(directory_ / producer.str());
    archive(directory_ / ("graphics-" + std::to_string(address) + ".log"));
    archive(directory_ / ("retired-" + std::to_string(address) + ".json"));
    archive(directory_ / ("owner-death-" + std::to_string(owner.nonce) + ".json"));

    for (const auto *name :
         {"vehicle-ack.json", "wheel-ack.json", "theme-ack.json", "language-ack.json"}) {
        const auto path = directory_ / name;

        if (!std::filesystem::exists(path)) {
            continue;
        }

        const auto value = read_private_json(path, 4096);

        if (value.contains("producer_address") && u64(value.at("producer_address")) == address) {
            archive(path);
        }
    }

    write_private_json(target / "retirement.json", {{"schema", 1},
                                                    {"producer_address", address},
                                                    {"worker_pid", graphics_[index].pid()},
                                                    {"generation", cluster_generation_},
                                                    {"nonce", owner.nonce},
                                                    {"session_id", session_id_},
                                                    {"guest_pid", owner.pid},
                                                    {"producer_incarnation", owner.incarnation},
                                                    {"unclean", unclean},
                                                    {"reason", reason},
                                                    {"exit_code", graphics_[index].exit_code()}});
    retired_graphics_.push_back({{"pid", graphics_[index].pid()},
                                 {"producer_address", address},
                                 {"alive", false},
                                 {"exit_code", graphics_[index].exit_code()},
                                 {"generation", cluster_generation_},
                                 {"nonce", owner.nonce},
                                 {"producer_incarnation", owner.incarnation},
                                 {"unclean", unclean}});
    retired_producers_.insert({address, owner.incarnation});
    retired_producers_.insert({address, 0});
    logging::write(unclean ? logging::Level::error : logging::Level::info, "qnx",
                   unclean ? "producer.unclean_retirement" : "producer.retired",
                   {{"producer_address", address},
                    {"worker_pid", graphics_[index].pid()},
                    {"generation", cluster_generation_},
                    {"nonce", owner.nonce},
                    {"guest_pid", owner.pid},
                    {"producer_incarnation", owner.incarnation}});
    graphics_.erase(graphics_.begin() + index);
    producers_.erase(producers_.begin() + index);
    graphics_owners_.erase(graphics_owners_.begin() + index);
    deliveries_["qnx"]["producers"] = producers_;

    if (!qnx_retired_receiver_.is_null()) {
        deliveries_["qnx"]["vehicle"] = qnx_vehicle_pending();
    }

    if (republish) {
        publish_graphics_owners();
    }
}

void Session::archive_cluster_generation() {
    if (!cluster_generation_) {
        return;
    }

    if (!cluster_finalized_) {
        throw VoyemuError("QNX_GENERATION", "previous owned cluster cleanup incomplete");
    }

    const auto target = directory_ / "cluster-history" / std::to_string(cluster_generation_);
    ensure_private_directory(target);
    std::vector<std::filesystem::path> paths;
    static const std::set<std::string> names{
            "frame.bgra",      "frame-owner.json",   "control-owner.json", "vehicle-ack.json",
            "wheel-ack.json",  "wheel-command.json", "theme-command.json", "language-command.json",
            "qnx-console.log", "qnx-serial.log",     "cluster-ui.log"};

    for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
        const auto name = entry.path().filename().string();

        if (names.contains(name) || (name.starts_with("graphics-") && name.ends_with(".log")) ||
            name.starts_with("producer-")) {
            paths.push_back(entry.path());
        }
    }

    Json moved = Json::array();

    for (const auto &source : paths) {
        terminal_path_guard(source);
        const auto destination = target / source.filename();

        if (std::filesystem::exists(destination) || std::filesystem::is_symlink(destination)) {
            throw VoyemuError("QNX_GENERATION", "prior archive preserved");
        }

        std::filesystem::rename(source, destination);
        moved.push_back({{"from", source.string()}, {"to", destination.string()}});
    }

    write_private_json(target / "archive.json", {{"schema", 1},
                                                 {"generation", cluster_generation_},
                                                 {"preserved_runtime_artifacts", moved}});
}

void Session::start_qnx_frame_grpc() {
    try {
        const auto discovery =
                std::filesystem::absolute(directory_ / "qnx-frame-grpc.json").lexically_normal();
        terminal_path_guard(discovery);

        if (std::filesystem::exists(discovery)) {
            throw VoyemuError("QNX_FRAME_GRPC", "discovery file already exists");
        }

        auto token = random_token();

        while (token == session_id_ || token == guest_auth_token_) {
            token = random_token();
        }

        auto server = std::make_unique<frames::GrpcServer>(directory_ / "frame.bgra", token);
        const auto port = server->port();
        write_private_json(discovery,
                           {{"schema", 1},
                            {"session_id", session_id_},
                            {"generation", cluster_generation_},
                            {"host", "127.0.0.1"},
                            {"port", port},
                            {"token", token}},
                           4096);
        qnx_frame_grpc_ = std::move(server);
        deliveries_["qnx"]["frame_grpc"] = {{"status", "listening"},
                                            {"host", "127.0.0.1"},
                                            {"port", port},
                                            {"discovery", discovery.string()}};
    } catch (const std::exception &) {
        close_qnx_frame_grpc();
        deliveries_["qnx"]["frame_grpc"] = {{"status", "unavailable"}};
        logging::write(logging::Level::error, "qnx", "frame_grpc.start_failed");
    }
}

void Session::close_qnx_frame_grpc() {
    deliveries_.at("qnx").erase("frame_grpc");

    if (!qnx_frame_grpc_) {
        return;
    }

    const auto port = qnx_frame_grpc_->port();
    qnx_frame_grpc_.reset();
    try {
        const auto discovery = directory_ / "qnx-frame-grpc.json";
        terminal_path_guard(discovery);

        if (!std::filesystem::exists(discovery)) {
            return;
        }

        const auto connection = read_private_json(discovery, 4096);

        if (connection.value("schema", 0) != 1 ||
            connection.value("session_id", std::string{}) != session_id_ ||
            connection.value("generation", std::uint64_t{0}) != cluster_generation_ ||
            connection.value("port", 0u) != port) {
            throw VoyemuError("QNX_FRAME_GRPC", "discovery ownership differs");
        }

        std::filesystem::remove(discovery);
    } catch (const std::exception &) {
        logging::write(logging::Level::error, "qnx", "frame_grpc.discovery_cleanup_failed");
    }
}

void Session::attach_cluster(const Json &specification, std::optional<unsigned> seconds) {
    attach_cluster(specification, seconds, false);
}

void Session::attach_cluster(const Json &input, std::optional<unsigned> seconds,
                             bool explicit_cold_boot) {
    const bool reset = std::exchange(cluster_reset_, false);
    auto specification = input;

    if (machine_start_ && !machine_program_.empty() && specification.contains("graphics")) {
        auto &graphics = specification["graphics"];

        if (graphics.value("backend", std::string{}) == "voyemu" ||
            builtin_program_name(graphics.at("executable").get<std::string>())) {
            graphics["executable"] = machine_program_.string();
        }
    }

    std::lock_guard lifecycle(lifecycle_mutex_);
    std::lock_guard lock(mutex_);

    if (!qnx_cluster_seconds_valid(host_qnx_engine, seconds, specification)) {
        throw VoyemuError("QNX_BOUND", "verified platform run budget required");
    }

    validate_pins(specification, machine_start_);
    cluster_spec_ = specification;

    if (specification.contains("interguest_network")) {
        (void)qnx_interguest_binding(specification);

        if (!interguest_link_) {
            interguest_link_ = std::make_unique<InterguestLink>();
        }

        if (!interguest_link_->status().port) {
            interguest_link_->start(directory_, session_id_,
                                    android_spec_.contains("diagnostic_native"));
        }
    }

    const auto ram_backend = specification.value("ram_backend", std::string("file"));

    if (ram_backend != "file") {
        throw VoyemuError("QNX_RAM_BACKEND", "unsupported transport");
    }

    auto ram = std::filesystem::path(replace_all(specification.at("ram").get<std::string>(),
                                                 "${SESSION}", directory_.string()));
    qnx_project(directory_, session_id_);
    auto expected = std::filesystem::absolute(directory_).lexically_normal() / "qnx-ram.bin";
    terminal_path_guard(expected);

    if (std::filesystem::absolute(ram).lexically_normal() != expected ||
        std::filesystem::exists(expected)) {
        throw VoyemuError("QNX_RAM_OWNER", "exact fresh session backing required");
    }

    archive_cluster_generation();
    producers_.clear();
    graphics_owners_.clear();
    retired_producers_.clear();
    qnx_retired_receiver_ = nullptr;
    qnx_vehicle_acknowledged_ = nullptr;
    retired_graphics_ = Json::array();
    graphics_owner_challenge_ = 0;
    startup_markers_.clear();
    qnx_serial_ = {};
    qnx_serial_ready_ = {};
    qmp_cont_ = false;
    cluster_finalized_ = false;
    issued_.reset();
    qnx_events_.clear();
    theme_.reset();
    cluster_language_.reset();
    next_qnx_ = next_oem_settings_ = std::chrono::steady_clock::now();
    // Manual lifecycle commands bypass the cache without quarantining a valid checkpoint.
    const auto validation_start = std::chrono::steady_clock::now();
    qnx_checkpoint_ =
            specification.contains("persistent_storage") || explicit_cold_boot ||
                            qnx_checkpoint_disabled_ || specification.contains("diagnostic_native")
                    ? QnxCheckpoint{}
            : machine_start_
                    ? runtime_qnx_checkpoint(
                              specification,
                              directory_.parent_path().parent_path().filename().string())
                    : inspect_qnx_checkpoint(
                              specification,
                              directory_.parent_path().parent_path().filename().string(), true);

    if (!machine_start_ && qnx_checkpoint_.resume &&
        qnx_checkpoint_.verification != "cached_identity") {
        try {
            if (!remember_qnx_checkpoint_verification(qnx_checkpoint_)) {
                qnx_checkpoint_ = inspect_qnx_checkpoint(
                        specification, directory_.parent_path().parent_path().filename().string());
            }
        } catch (const std::exception &) {
            logging::write(logging::Level::debug, "qnx", "checkpoint.verification_write_failed",
                           {{"reason", "cache_write_failed"}});
        }
    }

    const auto validation_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - validation_start)
                                       .count();

    if (specification.contains("persistent_storage")) {
        qnx_checkpoint_.reason = "persistent_storage_cold_only";
    }

    if (explicit_cold_boot) {
        qnx_checkpoint_.reason = "explicit_cold_boot";
    } else if (qnx_checkpoint_disabled_) {
        qnx_checkpoint_.reason = "restore_failed";
    }

    cluster_boot_spec_ = qnx_checkpoint_.resume ? qnx_checkpoint_.launch : specification;
    qnx_checkpoint_ready_ = false;

    if (qnx_checkpoint_.resume) {
        model_.advance_revision(qnx_checkpoint_.state_sequence);
        write_private_json(directory_ / "vehicle-state.json", model_.state(), vehicle_state_bytes);
        theme_sequence_ = std::max(theme_sequence_, qnx_checkpoint_.theme_sequence);
    }

    logging::write(logging::Level::info, "qnx", "checkpoint.selected",
                   {{"mode", qnx_checkpoint_.resume ? "restore" : "cold"},
                    {"reason", qnx_checkpoint_.reason},
                    {"verification", qnx_checkpoint_.verification},
                    {"validation_ms", validation_ms},
                    {"omitted_ram_loaders", qnx_checkpoint_.omitted_loaders},
                    {"omitted_ram_loader_bytes", qnx_checkpoint_.omitted_loader_bytes}});
    qnx_persistent_storage_.reset();
    qnx_persistence_paused_ = false;

    if (specification.contains("persistent_storage")) {
        if (!interguest_link_ || !specification.contains("interguest_network")) {
            throw VoyemuError("QNX_PERSISTENCE", "guest flush relay required");
        }

        qnx_persistent_storage_ = std::make_unique<QnxPersistentStorage>(specification);
    }

    const auto ram_binding = qnx_ram_binding(cluster_boot_spec_);
    const auto owned_ram_geometry = qnx_owned_ram_geometry(ram_binding);
    auto qemu = launch_spec(cluster_boot_spec_.at("qemu"), directory_, session_id_);
    configure_qnx_event_monitor(qemu, ram_backend);
    // Host scratch belongs to this launch even when a prepared checkpoint came from another
    // checkout.
    const auto host_tmp = directory_ / "host-tmp";
    ensure_private_directory(host_tmp);

    for (const auto *name : {"TMPDIR", "TMP", "TEMP"}) {
        qemu.environment[name] = host_tmp.string();
    }

    qemu.output = directory_ / "qnx-console.log";
    Json clone_identity;
    try {
        // The native diagnostic engine creates its own RAM file.
        const bool precreated = host_qnx_ram_precreated &&
                                !cluster_boot_spec_.contains("diagnostic_native");

        if (qnx_checkpoint_.external_ram) {
            clone_identity = clone_qnx_checkpoint_ram(qnx_checkpoint_, ram);
        } else if (qnx_persistent_storage_ || precreated) {
            create_qnx_ram(ram, ram_binding.ram_bytes());
            clone_identity = cache_file_identity(ram);
        }

        if (clone_identity.is_object()) {
            auto owner = read_private_json(directory_ / "session-owner.json", 4096);
            owner["ram_absent_before_owned_qemu"] = true;
            owner["qnx_pid"] = 0;
            owner["ram_geometry"] = owned_ram_geometry;
            owner["ram_clone_identity"] = clone_identity;
            owner["ram_materialization"] = qnx_checkpoint_.external_ram
                                                   ? "verified_checkpoint_clone_before_owned_qemu"
                                                   : "exclusive_sparse_before_owned_qemu";
            write_private_json(directory_ / "session-owner.json", owner);
        }

#ifdef _WIN32
        qnx_ = ChildProcess::launch(qemu);
#else
        qnx_ = launch_session_process(qemu, directory_, session_id_);
#endif
    } catch (...) {
        bool pending_launch = false;
#ifndef _WIN32
        const auto hosts = directory_ / "host-processes.json";
        pending_launch = std::filesystem::exists(hosts) &&
                         read_private_json(hosts, 65536).contains("pending_launch");
#endif

        if (clone_identity.is_object() && !pending_launch) {
            try {
                Json evidence{
                        {"cluster_generation", cluster_generation_ + 1},
                        {"children_terminal", true},
                        {"qemu",
                         {{"pid", 0}, {"alive", false}, {"started", false}, {"exit_code", -1}}},
                        {"graphics", Json::array()},
                        {"qemu_launch_failed_before_child", true},
                        {"ram_clone_identity", clone_identity}};
                deliveries_["qnx"]["cleanup"] = finalize_owned_qnx_session(directory_, session_id_,
                                                                           ram, evidence);
                cluster_finalized_ = true;
            } catch (const std::exception &error) {
                deliveries_["qnx"]["cleanup"] = {{"status", "retained_for_failed_cleanup"},
                                                 {"ram_removed", false},
                                                 {"error", error.what()}};
                logging::try_write(logging::Level::error, "qnx", "cleanup.failed", "error",
                                   error.what());
            }
        }

        throw;
    }

    auto owner = read_private_json(directory_ / "session-owner.json", 4096);
    owner["ram_absent_before_owned_qemu"] = true;
    owner["qnx_pid"] = qnx_.pid();
    owner["ram_geometry"] = owned_ram_geometry;
    write_private_json(directory_ / "session-owner.json", owner);
    cluster_requested_ = true;
    ++cluster_generation_;
    cluster_seconds_ = seconds;
    qnx_started_ = std::chrono::steady_clock::now();
    qnx_deadline_ = seconds ? qnx_started_ + std::chrono::seconds(*seconds)
                            : std::chrono::steady_clock::time_point{};
    deliveries_["qnx"] = {
            {"status", "booting"},
            {"pid", qnx_.pid()},
            {"bounded", seconds.has_value()},
            {"bounded_seconds", seconds.value_or(0)},
            {"lifecycle_supported", true},
            {"generation", cluster_generation_},
            {"vehicle", {{"status", "queued"}, {"sequence", model_.state()["sequence"]}}}};
    deliveries_["qnx"]["presentation"] = {{"status", "awaiting_vehicle_ack"},
                                          {"generation", cluster_generation_}};
    deliveries_["qnx"]["quickboot"] = {
            {"mode", qnx_checkpoint_.resume ? "restore_requested" : "cold"},
            {"reason", qnx_checkpoint_.reason},
            {"verification", qnx_checkpoint_.verification},
            {"validation_ms", validation_ms},
            {"omitted_ram_loaders", qnx_checkpoint_.omitted_loaders},
            {"omitted_ram_loader_bytes", qnx_checkpoint_.omitted_loader_bytes}};

    // A cluster reset keeps the ignition of the running vehicle.
    if (!qnx_checkpoint_.resume && !reset &&
        StartupIgnition::applies(
                cluster_boot_spec_.value("hardware_adapter", std::string("sa8155-qinggan09")))) {
        apply_startup_ignition(startup_ignition_.begin(model_.state(), qnx_started_));
    }

    start_qnx_frame_grpc();
}

void Session::poll_qnx() {
    if (qnx_checkpoint_.resume && !qnx_checkpoint_ready_ &&
        std::chrono::steady_clock::now() - qnx_started_ > std::chrono::seconds(20)) {
        if (fallback_cluster("restore_timeout")) {
            return;
        }
    }

    if (!qnx_.alive()) {
        // Drain after waitpid: the terminal event can arrive after the preceding poll.
        if (qnx_shutdown_monitor_) {
            qnx_shutdown_monitor_->drain();
        }

        const bool guest_reset = qnx_shutdown_monitor_ && qmp_cont_ && cluster_requested_ &&
                                 !stopped_ && qnx_.exit_code() == 0 &&
                                 qnx_shutdown_monitor_->guest_reset(qnx_.pid(),
                                                                    cluster_generation_);

        if (guest_reset) {
            const auto proof = qnx_shutdown_monitor_->evidence(), specification = cluster_spec_;
            const auto seconds = cluster_seconds_;
            const auto deadline = qnx_deadline_;
            const auto previous = cluster_generation_;
            deliveries_["qnx"]["status"] = "exited";
            deliveries_["qnx"]["shutdown"] = proof;
            close_cluster(false, "guest_reset");

            if (!cluster_finalized_) {
                throw VoyemuError("QNX_REBOOT", "previous generation cleanup incomplete");
            }

            if (!seconds || std::chrono::steady_clock::now() < deadline) {
                cluster_reset_ = true;
                attach_cluster(specification, seconds, true);

                if (seconds) {
                    qnx_deadline_ = deadline;
                }

                deliveries_["qnx"]["reboot"] = {{"status", "booting"},
                                                {"from_generation", previous},
                                                {"to_generation", cluster_generation_},
                                                {"shutdown", proof}};
            }

            return;
        }

        if (qnx_shutdown_monitor_) {
            deliveries_["qnx"]["shutdown"] = qnx_shutdown_monitor_->evidence();
        }

        if (fallback_cluster("qemu_exit")) {
            return;
        }

        deliveries_["qnx"]["status"] = "exited";
        close_cluster(false, "qemu_exit");

        return;
    }

    if (qnx_shutdown_monitor_) {
        qnx_shutdown_monitor_->drain();
    }

    // The Free A11 and Passion runtimes write their console to the session UART file on every host.
    const auto serial = qnx_serial_.read(directory_ / "qnx-serial.log");
    qnx_serial_ready_.add(serial);

    for (const auto &marker : qnx_startup_markers(serial, startup_markers_)) {
        logging::write(logging::Level::info, "qnx", "guest.startup",
                       {{"marker", marker},
                        {"host_elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now() - qnx_started_)
                                                    .count()}});
        apply_startup_ignition(startup_ignition_.marker(marker, std::chrono::steady_clock::now()));
    }

    apply_startup_ignition(startup_ignition_.expire(std::chrono::steady_clock::now()));

    for (std::size_t i = 0; i < graphics_.size();) {
        if (graphics_[i].alive()) {
            ++i;
            continue;
        }

        const auto address = producers_.at(i);
        const auto owner = graphics_owners_.at(i);
        const auto retired = directory_ / ("retired-" + std::to_string(address) + ".json");
        bool clean = false, invalidated = false;

        if (graphics_[i].exit_code() == 0 && std::filesystem::exists(retired)) {
            clean = read_private_json(retired, 1024) == Json{{"schema", 1},
                                                             {"producer_address", address},
                                                             {"worker_pid", graphics_[i].pid()},
                                                             {"acknowledged", true}};
        }

        const auto disposition = directory_ /
                                 ("worker-disposition-" + std::to_string(owner.nonce) + ".json");

        if (graphics_[i].exit_code() == 70 && std::filesystem::exists(disposition)) {
            invalidated = read_private_json(disposition, 2048) ==
                          Json{{"schema", 1},
                               {"reason", "rpc_channel_invalidated"},
                               {"producer_address", address},
                               {"worker_pid", graphics_[i].pid()},
                               {"generation", cluster_generation_},
                               {"nonce", owner.nonce},
                               {"session_id", session_id_},
                               {"guest_pid", owner.pid},
                               {"producer_incarnation", owner.incarnation}};
        }

        if (clean || invalidated) {
            retire_graphics_worker(i, invalidated);
            continue;
        }

        if (fallback_cluster("gpu_worker_exit")) {
            return;
        }

        deliveries_["qnx"]["status"] = "graphics_worker_exited";
        deliveries_["qnx"]["failed_producer"] = address;
        close_cluster(false, "graphics_worker_exit");

        return;
    }

    if (cluster_seconds_ && std::chrono::steady_clock::now() >= qnx_deadline_) {
        deliveries_["qnx"]["status"] = "bounded_run_complete";
        close_cluster(false, "time_limit");

        return;
    }

    if (!qmp_cont_) {
        // No guest instruction runs until the generation's shutdown cause stream is subscribed.
        if (!continue_qnx_guest()) {
            return;
        }
    }

    auto ram = std::filesystem::path(replace_all(cluster_spec_.at("ram").get<std::string>(),
                                                 "${SESSION}", directory_.string()));

    if (std::filesystem::is_symlink(ram) || !std::filesystem::is_regular_file(ram)) {
        return;
    }

    std::ifstream input(ram, std::ios::binary);

    constexpr std::uint64_t base = 0x40000000;
    auto address = cluster_spec_.value("ready_ring_address", std::uint64_t{0x50010000});
    auto extent = cluster_spec_.value("ready_ring_bytes", std::size_t{0x40000});

    if (address < base || extent < 32 || extent > 0x40000 ||
        address - base > 0x200000000ULL - extent) {
        throw VoyemuError("QNX_RING", "invalid bounded ring");
    }

    const auto free_adapter = cluster_boot_spec_.value("hardware_adapter", std::string{});
    const auto free_spec = std::find_if(qnx_adapter_registry.begin(), qnx_adapter_registry.end(),
                                        [&](const QnxAdapterSpec &spec) {
                                            return spec.name == free_adapter &&
                                                   spec.platform == QnxGuestPlatform::qinggan11;
                                        });
    const bool free_uart = free_spec != qnx_adapter_registry.end();

    if (free_uart) {
        const auto projection = cluster_boot_spec_.value("mcu_projection", Json());

        if (!qnx_mcu_delivery_scope(Json{{"mcu_projection", projection}},
                                    qnx_mcu_projection(free_spec->adapter))) {
            throw VoyemuError("QNX_READY", "exact Free projection required for UART discovery");
        }
    }

    const auto binding = qnx_ram_binding(cluster_spec_);
    const bool uart_ready = free_uart || binding.passion;
    const std::uint64_t mailbox_end = binding.passion ? 0x180000000ULL : 0x240000000ULL;

    if (address > mailbox_end - extent) {
        throw VoyemuError("QNX_RING", "invalid bounded ring");
    }

    if (interguest_link_ && cluster_spec_.contains("interguest_network")) {
        if (interguest_qnx_generation_ != cluster_generation_) {
            interguest_link_->bind_ram(ram, address,
                                       uart_ready ? directory_ / "qnx-serial.log"
                                                  : std::filesystem::path{},
                                       binding.geometry);
            interguest_qnx_generation_ = cluster_generation_;
        }

        const auto link = interguest_link_->status();
        const auto guest_network = interguest_link_->guest_counters();
        const bool utc_verified = link.utc_complete && link.utc_status == 0 &&
                                  link.utc_expected >= 946684800ULL &&
                                  link.utc_actual >= link.utc_expected &&
                                  link.utc_actual - link.utc_expected <= 2 && link.fault.empty();
        deliveries_["qnx"]["interguest_network"] = {
                {"ready", link.ready},
                {"connected", link.connected},
                {"generation", link.generation},
                {"received_frames", link.received},
                {"sent_frames", link.sent},
                {"guest_received_frames", guest_network.received},
                {"guest_sent_frames", guest_network.sent},
                {"utc_expected", link.utc_expected},
                {"utc_actual", link.utc_actual},
                {"utc_status", link.utc_status},
                {"utc_complete", link.utc_complete},
                {"utc_verified", utc_verified},
                {"fault", link.fault}};
        interguest_link_->check();
    }

    auto read_ram = [&](std::uint64_t physical, std::span<std::byte> output) -> std::size_t {
        input.clear();
        input.seekg(static_cast<std::streamoff>(physical - base));
        input.read(reinterpret_cast<char *>(output.data()),
                   static_cast<std::streamsize>(output.size()));

        return std::size_t(input.gcount());
    };
    retire_dead_graphics(read_ram);
    std::string console;

    if (!uart_ready) {
        start_qnx_ring_collector(ram, address, extent, mailbox_end);
        std::vector<char> bytes(extent);

        if (read_ram(address, std::as_writable_bytes(std::span(bytes))) < 32) {
            return;
        }

        auto word = [&](std::size_t offset) {
            return std::uint32_t(static_cast<unsigned char>(bytes[offset])) |
                   (std::uint32_t(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
                   (std::uint32_t(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
                   (std::uint32_t(static_cast<unsigned char>(bytes[offset + 3])) << 24);
        };

        if (word(0) != 0xbeefeeee) {
            return;
        }

        auto capacity = word(4), cursor = word(8);

        if (capacity < 1 || capacity > bytes.size() - 32 || cursor >= capacity) {
            throw VoyemuError("QNX_RING", "corrupt ring header");
        }

        console.assign(bytes.data() + 32 + cursor, bytes.data() + 32 + capacity);
        console.append(bytes.data() + 32, bytes.data() + 32 + cursor);
        console.erase(std::remove(console.begin(), console.end(), '\0'), console.end());
    }

    for (const auto &marker : qnx_startup_markers(console, startup_markers_)) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - qnx_started_)
                               .count();
        logging::write(logging::Level::info, "qnx", "guest.startup",
                       {{"marker", marker}, {"host_elapsed_ms", elapsed}});
        apply_startup_ignition(startup_ignition_.marker(marker, std::chrono::steady_clock::now()));
    }

    static const std::regex ready(
            R"(QG3 READY paddr=(0x[0-9a-f]+) length=4096(?: pid=([1-9][0-9]*) incarnation=(0x[0-9a-f]+))?)");
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> incarnations;
    std::vector<std::uint64_t> ready_producers =
            qnx_checkpoint_.resume ? qnx_checkpoint_.producers : std::vector<std::uint64_t>{};
    std::map<std::uint64_t, Json> producer_origins;
    auto origin = [&](std::uint64_t producer, const char *source) {
        qnx_producer_origin(producer_origins, producer, source);
    };

    for (auto producer : ready_producers) {
        origin(producer, "checkpoint");
    }

    add_retained_qnx_producers(ready_producers, incarnations, origin);

    if (uart_ready) {
        for (const auto &producer : qnx_serial_ready_.ready(read_ram, mailbox_end)) {
            ready_producers.push_back(producer.address);
            incarnations[producer.address] = {producer.pid, producer.incarnation};
            origin(producer.address, "uart");
        }
    } else {
        qnx_serial_ready_ = {}; // Only the Free A11 and Passion runtimes announce pages there.
    }

    for (auto it = std::sregex_iterator(console.begin(), console.end(), ready);
         it != std::sregex_iterator(); ++it) {
        const auto producer = std::stoull((*it)[1].str(), nullptr, 16);

        if ((*it)[2].matched) {
            if (producer < base || producer > mailbox_end - 4096 || producer % 4096) {
                continue;
            }

            const auto pid = std::stoull((*it)[2].str()),
                       incarnation = std::stoull((*it)[3].str(), nullptr, 16);
            std::array<std::uint64_t, 2> owner{};

            if (!incarnation ||
                read_ram(producer + 240, std::as_writable_bytes(std::span(owner))) !=
                        sizeof(owner) ||
                owner[0] != pid || owner[1] != incarnation) {
                continue;
            }

            incarnations[producer] = {pid, incarnation};
        }

        ready_producers.push_back(producer);
        origin(producer, "current_ring");
    }

    for (auto producer : ready_producers) {
        if (producer < base || producer > mailbox_end - 4096 || producer % 4096 ||
            std::find(producers_.begin(), producers_.end(), producer) != producers_.end()) {
            continue;
        }

        const auto [guest_pid, incarnation] = incarnations[producer];

        if (retired_producers_.contains({producer, incarnation})) {
            continue;
        }

        if (incarnation) {
            std::array<std::uint64_t, 2> owner{};

            if (read_ram(producer + 240, std::as_writable_bytes(std::span(owner))) !=
                        sizeof(owner) ||
                owner[0] != guest_pid || owner[1] != incarnation) {
                continue;
            }
        }

        if (producers_.size() >= 8) {
            throw VoyemuError("QNX_PRODUCERS", "more than eight bounded workers");
        }

        std::array<std::uint32_t, 2> page{};
        const auto read_count = read_ram(producer, std::as_writable_bytes(std::span(page)));
        qnx_producer_identity(
                producer, page, read_count,
                {{"origins", producer_origins.at(producer)},
                 {"generation", cluster_generation_},
                 {"session_directory", directory_.string()},
                 {"session_id", session_id_},
                 {"qnx_pid", qnx_.pid()},
                 {"host_elapsed_ms", std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - qnx_started_)
                                             .count()}},
                [&](const Json &evidence) {
                    try {
                        const auto witness = directory_ /
                                             ("producer-identity-failure-" +
                                              std::to_string(cluster_generation_) + ".json");

                        if (!std::filesystem::exists(witness) &&
                            !std::filesystem::is_symlink(witness)) {
                            write_private_json(witness, evidence);
                        }
                    } catch (...) { /* Ignored: preserve the original failure even when the witness
                                       cannot be written. */
                    }

                    logging::write(logging::Level::error, "qnx", "producer.identity_failure",
                                   evidence);
                });
        const auto &graphics = cluster_boot_spec_.at("graphics");
        ProcessSpec spec;
        spec.executable = graphics.at("executable").get<std::string>();
        spec.directory = directory_;
        spec.output = directory_ / ("graphics-" + std::to_string(producer) + ".log");
        spec.arguments = {"--ram",       ram.string(),
                          "--ram-bytes", std::to_string(binding.ram_bytes()),
                          "--address",   std::to_string(producer),
                          "--session",   directory_.string(),
                          "--angle",     graphics.at("angle").get<std::string>()};

        if (!binding.passion && !cluster_boot_spec_.value("mcu_projection", Json()).is_null()) {
            spec.arguments.push_back("--mcu-adapter");
            spec.arguments.push_back(cluster_boot_spec_.at("hardware_adapter").get<std::string>());
        }

        if (cluster_seconds_) {
            spec.arguments.push_back("--seconds");
            spec.arguments.push_back(std::to_string(*cluster_seconds_));
        }

        auto worker_nonce = std::stoull(random_token().substr(0, 16), nullptr, 16);

        while (!worker_nonce) {
            worker_nonce = std::stoull(random_token().substr(0, 16), nullptr, 16);
        }

        spec.arguments.insert(
                spec.arguments.end(),
                {"--session-id", session_id_, "--generation", std::to_string(cluster_generation_),
                 "--worker-nonce", std::to_string(worker_nonce), "--guest-pid",
                 std::to_string(guest_pid), "--producer-incarnation", std::to_string(incarnation)});
        program_mode(spec.executable, "_graphics", spec.arguments,
                     graphics.value("backend", std::string{}) == "voyemu" ||
                             (machine_start_ && !machine_program_.empty() &&
                              spec.executable == machine_program_));

        if (qnx_checkpoint_.resume) {
            spec.arguments.push_back("--checkpoint-replay");
            spec.arguments.push_back(graphics.at("replay").get<std::string>());
        }

        for (const auto *name : {"TMPDIR", "TMP", "TEMP"}) {
            spec.environment[name] = (directory_ / "host-tmp").string();
        }

        configure_graphics_environment(spec, graphics);
#ifdef _WIN32
        graphics_.push_back(ChildProcess::launch(spec));
#else
        graphics_.push_back(launch_session_process(spec, directory_, session_id_, worker_nonce));
#endif
        producers_.push_back(producer);
        graphics_owners_.push_back({worker_nonce, guest_pid, incarnation});
        publish_graphics_owners();

        if (qnx_checkpoint_.resume) {
            logging::write(
                    logging::Level::info, "qnx", "checkpoint.worker_started",
                    {{"producer_address", producer},
                     {"host_elapsed_ms", std::chrono::duration<double, std::milli>(
                                                 std::chrono::steady_clock::now() - qnx_started_)
                                                 .count()}});
        }

        deliveries_["qnx"]["status"] = "graphics_running";
        deliveries_["qnx"]["producers"] = producers_;
        apply_startup_ignition(startup_ignition_.graphics(std::chrono::steady_clock::now()));
    }

    if (!ui_executable_.empty() && !ui_.pid() && !producers_.empty()) {
        ProcessSpec spec;
        spec.executable = ui_executable_;
        spec.directory = directory_;
        spec.output = directory_ / "cluster-ui.log";
        spec.environment["VOYEMU_MEDIA_WINDOW_RECORD"] =
                (directory_ / "media-window.json").string();
        spec.arguments = {"--endpoint", endpoint_.string(),
                          "--frame",    (directory_ / "frame.bgra").string(),
                          "--assets",   assets_.string()};

        if (!ui_profile_.empty()) {
            spec.arguments.push_back("--profile");
            spec.arguments.push_back(ui_profile_);
        }

#ifdef _WIN32
        ui_ = ChildProcess::launch(spec);
#else
        ui_ = launch_session_process(spec, directory_, session_id_);
#endif
    }

    if (ui_.pid() && !ui_.alive()) {
        close_cluster(false, "window_exit");

        return;
    }

    observe_qnx_vehicle();

    if (theme_sequence_ && deliveries_["qnx"].contains("theme") &&
        deliveries_["qnx"]["theme"].value("status", std::string{}) != "guest_acknowledged") {
        const auto adapter = cluster_boot_spec_.value("hardware_adapter", std::string{});
        const bool skin_receipt =
                std::any_of(qnx_adapter_registry.begin(), qnx_adapter_registry.end(),
                            [&](const QnxAdapterSpec &spec) {
                                return spec.name == adapter && spec.theme_receipt_skin;
                            });

        if (auto receipt = find_qnx_theme_receipt(
                    directory_, producers_, theme_sequence_,
                    deliveries_["qnx"]["theme"].at("theme_id").get<unsigned>(),
                    skin_receipt ? deliveries_["qnx"]["theme"].value("theme_skin", 0u) : 0u)) {
            deliveries_["qnx"]["theme"].update(*receipt);
        }
    }

    if (language_sequence_ && deliveries_["qnx"].contains("language") &&
        deliveries_["qnx"]["language"].value("status", std::string{}) != "guest_acknowledged") {
        if (auto receipt = find_qnx_language_receipt(
                    directory_, producers_, language_sequence_,
                    deliveries_["qnx"]["language"].at("value").get<unsigned>())) {
            deliveries_["qnx"]["language"].update(*receipt);
        }
    }

    if (qnx_checkpoint_.resume && !qnx_checkpoint_ready_ &&
        deliveries_["qnx"]["vehicle"].value("status", std::string{}) == "guest_acknowledged" &&
        std::filesystem::is_regular_file(directory_ / "frame.bgra")) {
        qnx_checkpoint_ready_ = true;
        deliveries_["qnx"]["quickboot"]["mode"] = "restored";
        deliveries_["qnx"]["quickboot"]["reason"] = qnx_checkpoint_.reason;
    }

    publish_qnx_input();
}

Json Session::qnx_vehicle_pending() const {
    const auto sequence = model_.state().at("sequence");

    if (qnx_retired_receiver_.is_null()) {
        return {{"status", "queued"}, {"sequence", sequence}};
    }

    return {{"status", "unavailable"},
            {"reason", "receiver_retired"},
            {"sequence", sequence},
            {"receiver", qnx_retired_receiver_},
            {"last_acknowledged", qnx_vehicle_acknowledged_}};
}

void Session::observe_qnx_vehicle() {
    auto control_path = directory_ / "control-owner.json";

    if (!std::filesystem::exists(control_path)) {
        return;
    }

    auto control = read_private_json(control_path, 4096);
    auto address = u64(control.at("producer_address"));
    const auto owned = std::find(producers_.begin(), producers_.end(), address);

    if (control.value("schema", 0) != 1 ||
        control.value("source", std::string{}) != "original-MCU-input" ||
        owned == producers_.end()) {
        throw VoyemuError("QNX_OWNER", "control producer is not owned");
    }

    // A successor receiver owes a receipt of its own for the current revision.
    if (!qnx_retired_receiver_.is_null()) {
        qnx_retired_receiver_ = nullptr;
        deliveries_["qnx"]["vehicle"] = qnx_vehicle_pending();
    }

    auto vehicle_path = directory_ / "vehicle-ack.json";

    if (!std::filesystem::exists(vehicle_path) || body_.state().is_null() ||
        body_.source_revision() != model_.state().at("sequence").get<std::uint32_t>()) {
        return;
    }

    const auto ack = read_private_json(vehicle_path, 4096);
    const auto scope = qnx_mcu_delivery_scope(ack,
                                              cluster_boot_spec_.value("mcu_projection", Json()));
    const auto owner = graphics_owners_.at(std::size_t(owned - producers_.begin()));
    const QnxVehicleReceiver receiver{address, cluster_generation_, owner.nonce, owner.pid,
                                      owner.incarnation};

    if (!scope ||
        !qnx_vehicle_receipt(ack, receiver, body_.state().at("sequence").get<std::uint32_t>())) {
        return;
    }

    deliveries_["qnx"]["vehicle"] = {{"status", "guest_acknowledged"},
                                     {"sequence", ack.at("sequence")},
                                     {"source_sequence", body_.source_revision()},
                                     {"visible_effect_verified", false}};
    deliveries_["qnx"]["vehicle"].update(*scope);
    qnx_vehicle_acknowledged_ = {{"sequence", ack.at("sequence")},
                                 {"source_sequence", body_.source_revision()},
                                 {"producer_address", address}};
    auto &presentation = deliveries_["qnx"]["presentation"];

    if (!presentation.contains("vehicle_revision") ||
        presentation.at("vehicle_revision") != body_.source_revision()) {
        const bool first = !presentation.contains("vehicle_revision");
        const auto after = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
        presentation = {{"status", "vehicle_acknowledged"},
                        {"generation", cluster_generation_},
                        {"vehicle_revision", body_.source_revision()},
                        {"after_timestamp_ns", after},
                        {"source", "host_observed_original_MCU_ACK"}};

        if (first) {
            logging::write(logging::Level::info, "qnx", "presentation.acknowledged",
                           {{"generation", cluster_generation_},
                            {"vehicle_revision", body_.source_revision()}});
        }
    }
}

void Session::publish_qnx_input() {
    if (!cluster_requested_) {
        return;
    }

    if (body_.update(model_.state(), std::chrono::steady_clock::now())) {
        write_private_json(directory_ / "mcu-state.json", body_.state(), vehicle_state_bytes);
    }

    auto ack_path = directory_ / "wheel-ack.json";

    if (issued_ && std::filesystem::exists(ack_path)) {
        auto ack = read_private_json(ack_path, 4096);
        auto owner_path = directory_ / "control-owner.json";

        if (std::filesystem::exists(owner_path)) {
            auto owner = read_private_json(owner_path, 4096);
            auto address = u64(owner.at("producer_address"));

            if (owner.value("schema", 0) != 1 ||
                owner.value("source", std::string{}) != "original-MCU-input" ||
                std::find(producers_.begin(), producers_.end(), address) == producers_.end()) {
                throw VoyemuError("QNX_OWNER", "wheel ACK producer is not owned");
            }

            if (ack.value("schema", 0) == 1 && ack.value("guest_acknowledged", false) &&
                ack.value("sequence", 0u) == issued_->sequence &&
                u64(ack.at("producer_address")) == u64(owner.at("producer_address"))) {
                deliveries_["qnx"]["wheel"] = {{"status", "guest_acknowledged"},
                                               {"event_sequence", issued_->sequence},
                                               {"button", issued_->id},
                                               {"phase", issued_->phase},
                                               {"visible_effect_verified", false}};
                issued_.reset();
            }
        }
    }

    if (!issued_ && !qnx_events_.empty()) {
        issued_ = qnx_events_.front();
        qnx_events_.pop_front();
        write_private_json(directory_ / "wheel-command.json", {{"sequence", issued_->sequence},
                                                               {"index", issued_->qnx_index},
                                                               {"value", issued_->value}});
        deliveries_["qnx"]["wheel"] = {{"status", "issued"},
                                       {"event_sequence", issued_->sequence},
                                       {"button", issued_->id},
                                       {"phase", issued_->phase}};
    }
}

void Session::tick_inputs() {
    std::lock_guard lock(mutex_);

    if (stopped_) {
        return;
    }

    events_.expire();

    // Accepted phases are drained in order; QNX owns a separate acknowledgement queue.
    while (auto event = events_.pop()) {
        if (passion_presentation(android_spec_)) {
            event->qnx_index = passion_qnx_wheel_index(event->id);
        }

        if (nxp_presentation(android_spec_)) {
            const auto routes = android_spec_.value("wheel_adapter", Json::object())
                                        .value("routes", Json::object());

            if (!routes.contains(event->id) || routes.at(event->id).is_null()) {
                deliveries_["android"]["wheel"] = {{"event_sequence", event->sequence},
                                                   {"button", event->id},
                                                   {"status", "unmapped"}};
                continue;
            }
        }

        auto outcome = adas_.input(model_.state(), event->sequence, event->id, event->phase,
                                   event->owner, event->accepted_at);

        if (outcome.handled) {
            apply_adas_outcome(std::move(outcome));
            continue;
        }

        if (event->qnx_index && cluster_requested_) {
            if (qnx_events_.size() >= 1024) {
                throw VoyemuError("WHEEL_QUEUE", "QNX delivery stalled");
            }

            qnx_events_.push_back(*event);
        } else {
            deliveries_["qnx"]["wheel"] = {
                    {"event_sequence", event->sequence},
                    {"button", event->id},
                    {"status", event->qnx_index ? "not_started" : "unmapped"}};
        }

        if (!android_spec_.empty()) {
            if (android_events_.size() >= 1024) {
                throw VoyemuError("WHEEL_QUEUE", "Android delivery stalled");
            }

            android_events_.push_back(*event);
        } else {
            deliveries_["android"]["wheel"] = {{"event_sequence", event->sequence},
                                               {"status", "not_started"}};
        }
    }

    if (cluster_requested_) {
        try {
            publish_qnx_input();
        } catch (const std::exception &error) {
            deliveries_["qnx"]["last_error"] = error.what();
        }
    }
}

void Session::close_cluster(bool stopping_all, std::string reason) {
    if (cluster_requested_) {
        apply_startup_ignition(startup_ignition_.finish());
        logging::write(logging::Level::info, "qnx", "cluster.close",
                       {{"reason", stopping_all ? "session_stop" : reason},
                        {"bounded", cluster_seconds_.has_value()},
                        {"bounded_seconds", cluster_seconds_.value_or(0)}});
    }

    std::lock_guard lifecycle(lifecycle_mutex_);
    std::lock_guard lock(mutex_);

    if (cluster_requested_ && qnx_persistent_storage_ && discard_cluster_) {
        deliveries_["qnx"]["persistence"] = {{"status", "disposable_images_discarded"},
                                             {"guest_sync_acknowledged", false}};
    } else if (cluster_requested_ && qnx_persistent_storage_) {
        try {
            if (!qnx_.alive()) {
                // -no-reboot turns a guest reboot into a terminal owned QEMU.
                // Its shared disk mappings can be flushed without inventing a guest sync ACK.
                if (qnx_.pid() < 2 || qnx_.exit_code() != 0) {
                    throw VoyemuError("QNX_PERSISTENCE",
                                      "abnormal or unverified guest exit; runtime retained");
                }

                qnx_persistent_storage_->flush();
                deliveries_["qnx"]["persistence"] = {{"status", "owned_qemu_exited_host_flushed"},
                                                     {"guest_sync_acknowledged", false},
                                                     {"qemu_exit_code", qnx_.exit_code()}};
            } else {
                if (!interguest_link_) {
                    throw VoyemuError("QNX_PERSISTENCE", "guest flush relay unavailable");
                }

                const auto relay = interguest_link_->status();

                if (!relay.ready && relay.fault.empty() && startup_markers_.empty()) {
                    // No guest incarnation ever reached the relay, so no unflushed guest state
                    // exists: pause the owned QEMU, flush the host mappings without inventing a
                    // guest sync ACK, and fall through so close_cluster terminates it instead of
                    // retaining a trap.
                    set_qnx_persistence_paused(true);
                    qnx_persistent_storage_->flush();
                    deliveries_["qnx"]["persistence"] = {
                            {"status", "guest_never_attached_qemu_paused_host_flushed"}};
                } else {
                    if (qnx_persistence_paused_) {
                        set_qnx_persistence_paused(false);
                    }

                    interguest_link_->flush(static_cast<int>(qnx_guest_sync().count()));
                    set_qnx_persistence_paused(true);
                    qnx_persistent_storage_->flush();
                    deliveries_["qnx"]["persistence"] = {
                            {"status", "guest_quiesced_qemu_paused_host_flushed"}};
                }
            }
        } catch (const std::exception &error) {
            deliveries_["qnx"]["persistence"] = {
                    {"status", qnx_persistence_paused_ ? "flush_failed_runtime_retained_paused"
                                                       : "flush_failed_runtime_retained"},
                    {"error", error.what()}};
            throw;
        }
    }

    close_qnx_frame_grpc();

    if (!stopping_all && (deliveries_["qnx"].value("status", std::string{}) == "graphics_running" ||
                          deliveries_["qnx"].value("status", std::string{}) == "booting")) {
        deliveries_["qnx"]["status"] = "closed";
    }

    events_.release_all();
    adas_.cancel_all();
    ui_.terminate(1000);

    if (interguest_link_) {
        interguest_link_->clear_ram();
        interguest_qnx_generation_ = 0;
    }

    stop_qnx_ready_collector(); // Join the generation's reader before QEMU/RAM cleanup.
    Json graphics = retired_graphics_;

    for (std::size_t i = 0; i < graphics_.size(); ++i) {
        auto &process = graphics_[i];
        process.terminate(1000);
        graphics.push_back({{"pid", process.pid()},
                            {"producer_address", producers_.at(i)},
                            {"alive", process.alive()},
                            {"exit_code", process.exit_code()}});
    }

    graphics_.clear();
    qnx_.terminate(3000);
    cluster_requested_ = false;
    qnx_events_.clear();
    issued_.reset();
    flush_android_releases(false);

    if (qnx_shutdown_monitor_) {
        qnx_shutdown_monitor_->finish_owned_exit(qnx_.pid(), cluster_generation_);
        qnx_shutdown_monitor_.reset();
    }

    if (cluster_spec_.empty() || cluster_finalized_) {
        return;
    }

    Json evidence{{"cluster_generation", cluster_generation_},
                  {"children_terminal", !qnx_.alive()},
                  {"qemu",
                   {{"pid", qnx_.pid()}, {"alive", qnx_.alive()}, {"exit_code", qnx_.exit_code()}}},
                  {"graphics", graphics},
                  {"canonical_revision", model_.state()["sequence"]},
                  {"deliveries", deliveries_},
                  {"Android_continues",
                   !stopping_all && android_seen_guest_ && !android_spec_.empty() &&
                           !(tcp_port_free(android_spec_.at("port").get<unsigned>()) &&
                             tcp_port_free(android_spec_.at("port").get<unsigned>() + 1))},
                  {"process_scope",
                   "retained native handles/direct child identity; no unrelated runtime stopped"}};
    try {
        remove_qmp_socket(evidence);
        auto ram = std::filesystem::path(expand_session_path(
                cluster_spec_.at("ram").get<std::string>(), directory_, session_id_));
        deliveries_["qnx"]["cleanup"] = finalize_owned_qnx_session(directory_, session_id_, ram,
                                                                   evidence);
        cluster_finalized_ = true;
    } catch (const std::exception &error) {
        deliveries_["qnx"]["cleanup"] = {{"status", "retained_for_failed_cleanup"},
                                         {"ram_removed", false},
                                         {"error", error.what()}};
        logging::try_write(logging::Level::error, "qnx", "cleanup.failed", "error", error.what());
    }
}
} // namespace voyemu
