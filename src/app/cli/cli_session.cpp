#include "cli_commands.hpp"

namespace voyemu::cli {
namespace {
class OperationLock {
public:
    explicit OperationLock(const std::filesystem::path &path) {
        voyemu::ensure_private_directory(path.parent_path());
#ifdef _WIN32
        file_ = voyemu::detail::open_private_state_lock(path, GENERIC_READ | GENERIC_WRITE,
                                                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL);

        if (file_ == INVALID_HANDLE_VALUE) {
            throw voyemu::cli::LocalizedError("SESSION_LOCK", "error.profile_busy");
        }
#else
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);

        if (fd_ < 0 || ::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            if (fd_ >= 0) {
                ::close(fd_);
            }

            throw voyemu::cli::LocalizedError("SESSION_LOCK", "error.profile_busy");
        }
#endif
    }

    ~OperationLock() {
#ifdef _WIN32
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
        }
#else
        if (fd_ >= 0) {
            ::close(fd_);
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

voyemu::Json active_record(const Arguments &args) {
    return voyemu::read_private_json(
            voyemu::native_current_active_directory(args.root, args.profile, args.machine) /
                    (args.profile + ".json"),
            4096);
}

#ifndef _WIN32
void verify_refresh_owner(const Arguments &args) {
    auto token = [](const std::string &value) {
        return hex_text(value, 32);
    };

    if (!args.exclusive || !token(args.refresh_session) || !token(args.ownership_id)) {
        throw VoyemuError("HOST_REFRESH_OWNER", "explicit stopped session lease required");
    }

    const auto active = active_record(args);

    if (active.at("session_id") != args.refresh_session) {
        throw VoyemuError("HOST_REFRESH_OWNER", "active epoch differs");
    }

    const auto directory = voyemu::native_session_directory(args.root, args.profile, args.machine,
                                                            args.refresh_session);
    const auto owner = voyemu::read_private_json(directory / "automation-owner.json");
    const auto terminal = voyemu::read_private_json(directory / "runtime.json");

    if (owner.at("session_id") != args.refresh_session || owner.at("profile") != args.profile ||
        owner.at("ownership_id") != args.ownership_id ||
        terminal.at("session_id") != args.refresh_session || !terminal.value("stopped", false) ||
        terminal.at("endpoint") != active.at("endpoint") ||
        std::filesystem::exists(active.at("endpoint").get<std::string>())) {
        throw VoyemuError("HOST_REFRESH_OWNER", "stopped owned epoch required");
    }

    const auto shutdown = terminal.at("deliveries").at("android").at("shutdown");

    if (!shutdown.value("wrapper_terminal", false) || !shutdown.value("ports_released", false)) {
        throw VoyemuError("HOST_REFRESH_STOP", "verified Android shutdown required");
    }

    const auto launch = voyemu::read_private_json(
            voyemu::profile_launch_directory(args.root, args.profile, args.machine) /
            "android-launch.json");
    const auto port = launch.at("port").get<unsigned>(),
               grpc = launch.at("sdk_control").at("grpc_port").get<unsigned>();
    std::vector<int> sockets;
    struct Cleanup {
        std::vector<int> &sockets;

        ~Cleanup() {
            for (auto fd : sockets) {
                ::close(fd);
            }
        }
    } cleanup{sockets};

    for (auto value : {port, port + 1, grpc}) {
        if (value < 1024 || value > 65535) {
            throw VoyemuError("HOST_REFRESH_STOP", "invalid listener port");
        }

        const auto fd = ::socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0) {
            throw VoyemuError("HOST_REFRESH_STOP", "listener probe failed");
        }

        sockets.push_back(fd);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(value);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
            throw VoyemuError("HOST_REFRESH_STOP", "Android listener remains");
        }
    }
}
#endif

voyemu::Json active_request(const Arguments &args, const voyemu::Json &op) {
    if (op.at("op") == "status" && !args.machine_path.empty()) {
        const auto active = voyemu::native_current_active_directory(args.root, args.profile,
                                                                    args.machine);
        bool ordinary = true;

        for (const auto &path : voyemu::os::ancestors(active)) {
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(path))) {
                ordinary = false;
                break;
            }
        }

        const auto sessions = voyemu::native_machine_directory(args.root, args.profile,
                                                               args.machine) /
                              "sessions";
        const auto history = std::filesystem::symlink_status(sessions);

        if (ordinary && voyemu::ordinary_native_launch_file(args.machine_path / "profile.json") &&
            std::filesystem::symlink_status(active / (args.profile + ".json")).type() ==
                    std::filesystem::file_type::not_found &&
            (history.type() == std::filesystem::file_type::not_found ||
             (std::filesystem::is_directory(history) && std::filesystem::is_empty(sessions)))) {
            (void)voyemu::portable_profile_definition(args.root, args.profile, args.machine,
                                                      args.machine_path);

            return {{"ok", true},
                    {"profile", args.profile},
                    {"machine", args.machine.selector()},
                    {"machine_seed", args.machine_path.string()},
                    {"stopped", true},
                    {"deliveries",
                     {{"android", {{"status", "not_started"}}},
                      {"qnx", {{"status", "not_started"}}}}}};
        }
    }

    const auto record = active_record(args);
    const auto id = record.at("session_id");

    if (op.at("op") == "status") {
        const auto session_id = id.get<std::string>();

        if (hex_text(session_id, 32)) {
            const auto directory = voyemu::native_session_directory(args.root, args.profile,
                                                                    args.machine, session_id);
            const auto recorded_directory = record.value("directory", std::string{});

            if ((recorded_directory == directory.string() ||
                 voyemu::same_wine_host_path(recorded_directory, directory)) &&
                std::filesystem::exists(directory / "runtime.json")) {
                auto terminal = voyemu::read_private_json(directory / "runtime.json");

                if (terminal.value("stopped", false) &&
                    terminal.value("session_id", std::string{}) == session_id &&
                    terminal.value("endpoint", std::string{}) ==
                            record.at("endpoint").get<std::string>() &&
                    active_record(args) == record) {
                    if (!args.machine_path.empty()) {
                        const auto activation = voyemu::read_private_json(
                                voyemu::native_machine_directory(args.root, args.profile,
                                                                 args.machine) /
                                "activation.json");
                        const bool same_seed = activation.value("seed", std::string{}) ==
                                               args.machine_path.string();

                        if (activation.value("profile", std::string{}) != args.profile ||
                            voyemu::canonical_machine_name(
                                    args.profile, activation.value("machine", std::string{})) !=
                                    args.machine.name ||
                            (!same_seed &&
                             !voyemu::portable_configuration_from_other_host(
                                     activation, args.root, args.profile, args.machine,
                                     args.machine_path,
                                     voyemu::sha256_file(args.machine_path / "runtime.json")))) {
                            throw voyemu::cli::LocalizedError("MACHINE_SOURCE",
                                                              "error.other_machine_stopped");
                        }

                        terminal["machine_seed"] = args.machine_path.string();
                    }

                    terminal["ok"] = true;

                    return terminal;
                }
            }
        }
    }

    auto invoke = [&](voyemu::Json operation) {
        operation["schema"] = 1;
        operation["session_id"] = id;
        auto response = voyemu::request(record.at("endpoint").get<std::string>(), operation,
                                        operation.value("op", std::string{}) == "stop"
                                                ? voyemu::request_timeout_limit_ms("stop")
                                                : 10000);

        if (response.value("session_id", std::string{}) != id.get<std::string>()) {
            throw VoyemuError("SESSION_IDENTITY", "stale supervisor");
        }

        return response;
    };

    if (!args.machine_path.empty()) {
        const auto current = invoke({{"op", "status"}});

        if (!voyemu::machine_session_matches(args.root, {args.machine_path, args.profile},
                                             current)) {
            throw voyemu::cli::LocalizedError("MACHINE_SOURCE", "error.other_machine_active");
        }

        if (op.at("op") == "status") {
            return current;
        }
    }

    return invoke(op);
}

void wait_stopped_epoch(const Arguments &args, const voyemu::Json &reply, bool foreign_host = false,
                        bool launch_held = false) {
    auto id = reply.at("session_id").get<std::string>();

    if (!hex_text(id, 32)) {
        throw VoyemuError("SESSION_IDENTITY", "invalid stopped epoch");
    }

    auto directory = voyemu::native_session_directory(args.root, args.profile, args.machine, id);
    auto endpoint = std::filesystem::path(reply.at("endpoint").get<std::string>());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (std::chrono::steady_clock::now() < deadline) {
        auto terminal = voyemu::read_private_json(directory / "runtime.json");

        if (terminal.value("session_id", std::string{}) != id ||
            terminal.value("endpoint", std::string{}) != endpoint.string()) {
            throw VoyemuError("SESSION_IDENTITY", "stopped epoch record changed");
        }

        // The two hosts use different IPC mechanisms. A verified terminal epoch
        // from the other host is fenced by the shared supervisor byte-range lock.
        bool gone = foreign_host;

        if (!foreign_host) {
#ifdef _WIN32
            try {
                auto result = voyemu::request(endpoint, {{"op", "status"}, {"session_id", id}},
                                              100);
                gone = result.value("session_id", std::string{}) != id;
            } catch (const std::exception &) {
                gone = true;
            }
#else
            gone = !std::filesystem::exists(endpoint) && !std::filesystem::is_symlink(endpoint);
#endif
        }

        if (terminal.value("stopped", false)) {
            auto active = active_record(args);

            if (active.value("session_id", std::string{}) != id) {
                return;
            }

            std::optional<OperationLock> launch, supervisor;
            bool exclusive = false;

            try {
                const auto active_directory = voyemu::native_current_active_directory(
                        args.root, args.profile, args.machine);

                if (!launch_held) {
                    launch.emplace(active_directory / (args.profile + ".launch.lock"));
                }

                supervisor.emplace(active_directory / (args.profile + ".supervisor.lock"));
                exclusive = true;
            } catch (const std::exception &) {
                // A contended lease is retried until the deadline.
                exclusive = false;
            }

            if (exclusive) {
                if (active_record(args).value("session_id", std::string{}) != id) {
                    return;
                }

#ifndef _WIN32
                if (!gone && !foreign_host) {
                    (void)voyemu::stop_orphaned_session(directory, id, endpoint.string());
                    gone = true;
                }
#endif

                if (gone) {
#ifndef _WIN32
                    if (!foreign_host) {
                        voyemu::await_session_resources(directory, deadline);
                    }
#endif

                    return;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    throw VoyemuError("SESSION_STOP_TIMEOUT_UNKNOWN",
                      "own stopped epoch still holds supervisor lock");
}

voyemu::Json profile_startup_definition(const Arguments &args) {
    if (args.profile == voyemu::qinggan11_731_profile_id ||
        args.profile == voyemu::passion_profile_id ||
        voyemu::portable_runtime_available(args.root, args.profile, args.machine,
                                           args.machine_path)) {
        return voyemu::portable_profile_definition(args.root, args.profile, args.machine,
                                                   args.machine_path);
    }

    return voyemu::embedded_profile(args.profile).at("definition");
}

voyemu::Json profile_startup_values(const Arguments &args, const voyemu::Json &definition) {
    if (args.profile == voyemu::qinggan11_731_profile_id) {
        return voyemu::qinggan11_startup_values(definition);
    }

    if (args.profile == voyemu::passion_profile_id) {
        return voyemu::passion_startup_values(definition);
    }

    return definition.value("initial_state", voyemu::Json::object());
}

voyemu::Json startup_vehicle_state(const Arguments &args) {
    const auto definition = profile_startup_definition(args);
    auto profile_values = profile_startup_values(args, definition);
    voyemu::Json saved;

    if (args.resume) {
        const auto path = voyemu::native_current_active_directory(args.root, args.profile,
                                                                  args.machine)
                                  .parent_path() /
                          "state/vehicle-state.json";

        if (std::filesystem::symlink_status(path).type() == std::filesystem::file_type::not_found) {
            throw VoyemuError(
                    "VEHICLE_STARTUP",
                    "saved scenario missing: " + path.string() +
                            "; use fresh scenario or provide a saved vehicle scenario before explicit resume");
        }

        saved = voyemu::read_private_json(path, voyemu::vehicle_state_bytes);
    }

    return voyemu::select_vehicle_startup(
            args.resume ? voyemu::VehicleStartup::resume : voyemu::VehicleStartup::fresh,
            voyemu::vehicle_catalog(definition.at("platform").get<std::string>()), profile_values,
            args.resume ? &saved : nullptr);
}
} // namespace

int supervise(Arguments args, const std::filesystem::path &current_program) {
    if (args.root.empty() || args.session.empty() || args.token.size() != 32) {
        throw VoyemuError("SUPERVISOR_ARGUMENT", "private invocation incomplete");
    }

    const auto active_directory = voyemu::native_current_active_directory(args.root, args.profile,
                                                                          args.machine);
    voyemu::ensure_private_directory(active_directory);
    OperationLock ownership(active_directory / (args.profile + ".supervisor.lock"));

    if (args.machine.selected) {
        args.machine_path = voyemu::read_private_json(args.session / "machine-selection.json")
                                    .at("seed")
                                    .get<std::string>();
    }

    // Guest snapshots never choose the host scenario. Fresh launches use a session-owned
    // working file; an explicit resume copies the legacy scenario without modifying it.
    auto selected = startup_vehicle_state(args);
    const auto working = args.session / "vehicle-state.json";

    if (std::filesystem::exists(working) || std::filesystem::is_symlink(working)) {
        throw VoyemuError("VEHICLE_STARTUP", "fresh session working path required");
    }

    voyemu::write_private_json(working, selected, voyemu::vehicle_state_bytes);
    auto owned_session = voyemu::Session::for_machine_start(args.session, args.token,
                                                            current_program);
    auto &session = *owned_session;

    if (std::filesystem::exists(args.session / "providers.json")) {
        session.set_test_providers(
                voyemu::read_private_json(args.session / "providers.json", 65536), args.profile);
    }

    session.set_media_input(voyemu::api::media_input(args.session, args.token));

    voyemu::ControlServer server(
            session.endpoint(),
            [&](const auto &op) {
                auto result = session.handle(op);

                if (args.machine.selected) {
                    const auto selected =
                            voyemu::read_private_json(args.session / "machine-selection.json");
                    result["machine"] = selected.at("machine");
                    result["machine_seed"] = selected.at("seed");
                    result["machine_manifest_sha256"] = selected.at("manifest_sha256");
                }

                return result;
            },
            [&] {
                session.tick_inputs();
            });
#ifndef _WIN32
    voyemu::record_session_endpoint(args.session, args.token, session.endpoint());
#endif
    auto prepared = voyemu::profile_launch_directory(args.root, args.profile, args.machine);
    const auto activation_file = prepared / "activation.json";
    const auto activation = args.machine.selected ||
                                            voyemu::ordinary_native_launch_file(activation_file)
                                    ? voyemu::read_private_json(activation_file)
                                    : voyemu::Json::object();
    bool cluster_available = true;

    if (args.machine.selected) {
        const auto pin = voyemu::read_private_json(args.session / "machine-selection.json");
        const auto selected = voyemu::inspect_machine_path(pin.at("seed").get<std::string>());

        if (selected.profile != args.profile ||
            !voyemu::machine_selector_matches(args.root, selected, args.machine.selector())) {
            throw VoyemuError("MACHINE_SOURCE", "selected seed identity differs");
        }

        if (voyemu::canonical_machine_name(args.profile,
                                           activation.at("machine").get<std::string>()) !=
                    args.machine.name ||
            activation.at("manifest_sha256") != pin.at("manifest_sha256")) {
            throw VoyemuError("MACHINE_SOURCE", "selected seed changed before supervisor launch");
        }

        cluster_available = voyemu::read_private_json(selected.seed / "runtime.json", 128 * 1024)
                                    .contains("qnx");
    }

    const auto cluster_launch = prepared / "cluster-launch.json";
    const bool cluster_prepared = cluster_available &&
                                  !activation.value("cluster_rebind_pending", false) &&
                                  voyemu::ordinary_native_launch_file(cluster_launch);

    if (args.cluster && !cluster_prepared) {
        throw voyemu::cli::LocalizedError("MACHINE_NOT_PREPARED", "error.qnx_not_prepared");
    }

    voyemu::Json android_spec = voyemu::Json::object();

    if (args.multimedia) {
        android_spec = voyemu::read_private_json(prepared / "android-launch.json");
        // Diagnostic WAV follows the same trust boundary as -allow-host-audio: the canonical
        // machine/profile launch specification decides. No ambient environment opt-in exists.
#ifdef __APPLE__
        android_spec["env"]["VOYEMU_HOST_WINDOW_RECORD"] =
                (args.session / "media-window.json").string();
        android_spec["env"]["VOYEMU_HOST_PIXEL_SIZE"] = "3840x720";
        android_spec["env"]["VOYEMU_HOST_LAYOUT_ROLE"] = "multimedia";
#endif

        if (args.background) {
            android_spec["headless"] = true;
            auto &launch_args = android_spec["args"];

            if (std::find(launch_args.begin(), launch_args.end(), voyemu::Json("-no-window")) ==
                launch_args.end()) {
                launch_args.push_back("-no-window");
            }
        }

        // --network=yes attaches the owner-approved outbound NAT SLIRP interface; the
        // canonical launch specification never carries ambient host connectivity.
        if (args.network) {
            android_spec["network"] = voyemu::android_network_contract();
        }

        session.attach_android(android_spec);
    }

    const bool unified = args.multimedia && android_spec.contains("frontend");

    if (cluster_prepared) {
        session.configure_cluster(voyemu::read_private_json(cluster_launch), args.seconds);
    }

    if (unified) {
#ifdef _WIN32
        session.attach_frontend(android_spec.at("frontend"), args.cluster || cluster_prepared,
                                true);
#else
        session.attach_frontend(android_spec.at("frontend"), args.cluster || cluster_prepared,
                                args.background);
#endif
    }

    voyemu::logging::write(voyemu::logging::Level::info, "supervisor", "android.started",
                           {{"session_id", args.token}, {"with_cluster", args.cluster}});

    if (args.cluster) {
        session.attach_cluster(voyemu::read_private_json(cluster_launch), args.seconds);
        auto executable = voyemu::host_payload_directory(args.root) / "voyemu-cluster";
#ifdef _WIN32
        executable += ".exe";
#elif defined(__APPLE__)
        executable = voyemu::host_payload_directory(args.root) /
                     "voyemu-cluster.app/Contents/MacOS/voyemu-cluster";
#endif

        if (!unified && !args.background) {
            const auto seed = args.machine_path.empty() ? args.root / "machines" / args.machine.name
                                                        : args.machine_path;
            const auto assets = voyemu::portable_resource_path(seed, "ui/wheel/wheel-hotspots.json")
                                        .parent_path();
            session.set_ui(executable, assets, args.profile);
        }
    }

    voyemu::write_private_json(
            voyemu::native_current_active_directory(args.root, args.profile, args.machine) /
                    (args.profile + ".json"),
            {{"schema", 1},
             {"session_id", args.token},
             {"profile", args.profile},
             {"endpoint", session.endpoint().string()},
             {"directory", args.session.string()}});
    std::atomic<bool> server_stop = false;
    std::exception_ptr error;
    std::thread thread([&] {
        try {
            server.serve(server_stop);
        } catch (...) {
            error = std::current_exception();
            server_stop = true;
        }
    });
    try {
        while (!interrupted && !server_stop && !session.stopped() &&
               !voyemu::supervisor_stop_requested(args.session)) {
            session.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    } catch (...) {
        server_stop = true;
        thread.join();
        session.stop();
        throw;
    }

    server_stop = true;
    thread.join();
    session.stop();
    voyemu::logging::write(voyemu::logging::Level::info, "supervisor", "session.stopped",
                           {{"session_id", args.token}});

    if (error) {
        std::rethrow_exception(error);
    }

    return 0;
}

std::optional<int> restart_command(Arguments &args, const Emit &emit) {
    {
        OperationLock preflight(
                voyemu::native_current_active_directory(args.root, args.profile, args.machine) /
                (args.profile + ".launch.lock"));

        if (!args.machine_path.empty()) {
            voyemu::bind_machine_configuration(args.root, args.profile, args.machine,
                                               args.machine_path, voyemu::os::executable_path(),
                                               args.multimedia, args.cluster);
        }
    }
    const auto prepared = voyemu::profile_launch_directory(args.root, args.profile, args.machine);

    if ((args.multimedia &&
         !voyemu::ordinary_native_launch_file(prepared / "android-launch.json")) ||
        (args.cluster && !voyemu::ordinary_native_launch_file(prepared / "cluster-launch.json")) ||
        (!args.multimedia && !args.cluster)) {
        throw voyemu::cli::LocalizedError("MACHINE_NOT_PREPARED", "error.restart_not_prepared");
    }

    auto response = active_request(args, {{"op", "stop"}});

    if (!response.value("ok", false)) {
        emit(response);

        return 4;
    }

    wait_stopped_epoch(args, response);
    args.command = "start";

    return std::nullopt;
}

#ifndef _WIN32
// The launch and supervisor locks of a machine are free only while no start or supervisor of it
// runs; the receipts of its active session then decide what a stop has left to do.
std::optional<Json> stop_without_supervisor(const Arguments &args) {
    const auto active = voyemu::native_current_active_directory(args.root, args.profile,
                                                                args.machine);

    if (!voyemu::ordinary_native_launch_file(active / (args.profile + ".json"))) {
        return std::nullopt;
    }

    std::optional<OperationLock> launch, supervisor;
    try {
        launch.emplace(active / (args.profile + ".launch.lock"));
        supervisor.emplace(active / (args.profile + ".supervisor.lock"));
    } catch (const std::exception &) {
        return std::nullopt; // A running start or supervisor answers on its endpoint.
    }

    const auto record = active_record(args);
    const auto id = record.at("session_id").get<std::string>();

    if (!hex_text(id, 32)) {
        throw VoyemuError("SESSION_IDENTITY", "invalid active epoch");
    }

    const auto directory = voyemu::native_session_directory(args.root, args.profile, args.machine,
                                                            id);

    if (record.value("directory", std::string{}) != directory.string()) {
        throw VoyemuError("SESSION_IDENTITY", "active session directory differs");
    }

    auto terminal = voyemu::stop_orphaned_session(directory, id,
                                                  record.at("endpoint").get<std::string>());
    terminal["ok"] = true;

    return terminal;
}
#endif

int session_command(const Arguments &args, const Emit &emit) {
#ifdef _WIN32
    if (args.command == "stop" && !args.ownership_id.empty()) {
        const auto active = active_record(args);

        if (voyemu::api::StoppedAndroidLease::endpoint_absent(
                    active.at("endpoint").get<std::string>())) {
            voyemu::api::StoppedAndroidLease lease(args.root, args.profile, args.machine,
                                                   active.at("session_id").get<std::string>(),
                                                   args.ownership_id);
            auto terminal = lease.publish_terminal();
            terminal["ok"] = true;
            emit(terminal);

            return 0;
        }
    }
#else
    if (args.command == "stop") {
        if (const auto terminal = stop_without_supervisor(args)) {
            emit(*terminal);

            return 0;
        }
    }
#endif
    Json operation{{"op", args.command}};

    if (args.command == "patch") {
        operation["values"] = args.values;
    }

    auto response = active_request(args, operation);

    if (args.command == "stop" && response.value("ok", false)) {
        wait_stopped_epoch(args, response);
    }

    emit(response);

    return response.value("ok", false) ? 0 : 4;
}

int start_command(Arguments &args, const Emit &emit) {
    OperationLock lock(
            voyemu::native_current_active_directory(args.root, args.profile, args.machine) /
            (args.profile + ".launch.lock"));
    auto record = voyemu::native_current_active_directory(args.root, args.profile, args.machine) /
                  (args.profile + ".json");
    Json existing = Json::object();

    if (std::filesystem::exists(record)) {
        try {
            existing = active_request(args, {{"op", "status"}});
        } catch (const std::exception
                         &) { /* Ignored: an unreachable session has no status to report. */
        }
    }

    if (existing.value("ok", false) && !existing.value("stopped", true)) {
        if ((args.cluster_explicit && (existing.value("qnx_pid", 0) > 0) != args.cluster) ||
            (args.multimedia_explicit &&
             (existing.value("android_pid", 0) > 0) != args.multimedia)) {
            throw voyemu::cli::LocalizedError("MACHINE_SCOPE", "error.scope_differs");
        }

        if (args.exclusive) {
            throw VoyemuError("TEST_SESSION_ACTIVE",
                              "selected profile already has a live session; create never attaches");
        }

        if (!args.machine_path.empty() &&
            !voyemu::machine_session_matches(args.root, {args.machine_path, args.profile},
                                             existing)) {
            throw VoyemuError("MACHINE_SOURCE", "live session uses another machine");
        }

        emit(existing);

        return 0;
    }

    // Spontaneous guest/window exit can publish terminal state before the supervisor
    // releases its lock. Fence that exact owned epoch just as stop/restart do before
    // starting the next one.
    if (std::filesystem::exists(record)) {
        const auto prior = active_record(args);
        const auto id = prior.at("session_id").get<std::string>();

        if (!hex_text(id, 32)) {
            throw VoyemuError("SESSION_IDENTITY", "invalid previous epoch");
        }

        const auto previous_directory = voyemu::native_session_directory(args.root, args.profile,
                                                                         args.machine, id);
        const auto terminal_file = previous_directory / "runtime.json";
        const auto recorded_directory = prior.at("directory").get<std::string>();
        const bool foreign_host = voyemu::same_wine_host_path(recorded_directory,
                                                              previous_directory);
        const std::filesystem::path prior_directory(recorded_directory);

        if (!foreign_host &&
            (!prior_directory.is_absolute() ||
             !voyemu::same_directory_tail(prior_directory, previous_directory, 3))) {
            throw VoyemuError("SESSION_IDENTITY", "previous session directory differs");
        }

        if (foreign_host && !voyemu::ordinary_native_launch_file(terminal_file)) {
            throw VoyemuError("SESSION_IDENTITY", "other host has no terminal session record");
        }

        if (voyemu::ordinary_native_launch_file(terminal_file)) {
            const auto terminal = voyemu::read_private_json(terminal_file);

            if (foreign_host && !terminal.value("stopped", false)) {
                throw voyemu::cli::LocalizedError("SESSION_LOCK", "error.other_host_session");
            }

            if (terminal.value("stopped", false)) {
                const auto owner =
                        voyemu::read_private_json(previous_directory / "session-owner.json", 4096);
                // The supervisor and the CLI can spell the same session directory with
                // either separator, so the terminal epoch is fenced on path identity rather
                // than on the text of one spelling. A record prepared on another host can
                // also carry its own root, so compare the trailing components.
                const auto owner_path = owner.at("directory").get<std::string>();
                const std::filesystem::path owner_directory(owner_path);
                const bool same_directory =
                        foreign_host ? voyemu::same_wine_host_path(owner_path, previous_directory)
                                     : owner_directory.is_absolute() &&
                                               voyemu::same_directory_tail(owner_directory,
                                                                           previous_directory, 3);

                if (terminal.at("session_id") != id ||
                    terminal.at("endpoint") != prior.at("endpoint") ||
                    owner.at("session_id") != id || !same_directory ||
                    owner.value("origin", std::string{}) != "voyemu-native-supervisor" ||
                    owner.value("creator_pid", 0) <= 0) {
                    throw VoyemuError("SESSION_IDENTITY",
                                      "previous terminal epoch ownership differs");
                }

                wait_stopped_epoch(args, terminal, foreign_host, true);
            }
        }
    }

    if (args.private_start && args.machine_path.empty()) {
        const auto seed = args.root / "machines" / args.machine.name;

        if (voyemu::ordinary_native_launch_file(seed / "runtime.json")) {
            const auto selected = voyemu::inspect_machine_path(seed);

            if (selected.profile != args.profile ||
                !voyemu::machine_selector_matches(args.root, selected, args.machine.selector())) {
                throw VoyemuError("MACHINE_SOURCE", "selected machine differs from API identity");
            }

            args.machine_path = selected.seed;
        }
    }

    if (!args.machine_path.empty()) {
        voyemu::bind_machine_configuration(args.root, args.profile, args.machine, args.machine_path,
                                           voyemu::os::executable_path(), args.multimedia,
                                           args.cluster);
    }

    const auto scoped = voyemu::profile_launch_directory(args.root, args.profile, args.machine);
    const bool require_cluster = !(args.cluster_explicit && !args.cluster);

    if (args.multimedia && !voyemu::ordinary_native_launch_file(scoped / "android-launch.json")) {
        throw voyemu::cli::LocalizedError("MACHINE_NOT_PREPARED", "error.not_built");
    }

    if (!args.cluster && !args.multimedia) {
        throw voyemu::cli::LocalizedError("CLI_ARGUMENT", "error.no_subsystem");
    }

    auto prepared = voyemu::profile_launch_directory(args.root, args.profile, args.machine);

    if (args.multimedia && !voyemu::ordinary_native_launch_file(prepared / "android-launch.json")) {
        throw voyemu::cli::LocalizedError("PROFILE_NOT_PREPARED", "error.unprepared_native");
    }

    if (args.cluster && !voyemu::ordinary_native_launch_file(prepared / "cluster-launch.json")) {
        throw voyemu::cli::LocalizedError("PROFILE_NOT_PREPARED", "error.unprepared_qnx");
    }

#ifdef _WIN32
    if (args.refresh_host) {
        if (!args.exclusive || voyemu::os::executable_path() != args.root / "voyemu.exe") {
            throw VoyemuError("HOST_REFRESH_OWNER",
                              "explicit exclusive current root program required");
        }

        voyemu::api::StoppedAndroidLease lease(args.root, args.profile, args.machine,
                                               args.refresh_session, args.ownership_id);

        if (!lease.original_runtime().value("stopped", false)) {
            lease.publish_terminal();
        }

        if (!voyemu::portable_runtime_available(args.root, args.profile, args.machine,
                                                args.machine_path)) {
            throw VoyemuError("HOST_REFRESH_SOURCE", "portable factory required");
        }

        lease.verify();
        voyemu::activate_built_profile(args.root, args.profile, args.machine, require_cluster, true,
                                       args.machine_path);
        lease.verify_host_absence();
    }
#endif
#ifndef _WIN32
    if (args.refresh_host) {
        OperationLock supervisor(
                voyemu::native_current_active_directory(args.root, args.profile, args.machine) /
                (args.profile + ".supervisor.lock"));
        verify_refresh_owner(args);

        if (!voyemu::portable_runtime_available(args.root, args.profile, args.machine,
                                                args.machine_path)) {
            throw VoyemuError("HOST_REFRESH_SOURCE", "portable factory required");
        }

        voyemu::activate_built_profile(args.root, args.profile, args.machine, require_cluster, true,
                                       args.machine_path);
    }
#endif
    // Validate the selected source before spawning a supervisor or guest.
    (void)startup_vehicle_state(args);

    record = voyemu::native_current_active_directory(args.root, args.profile, args.machine) /
             (args.profile + ".json");

    // A concurrent start of another machine can lease the selected Android tuple first; its
    // supervisor then exits with ANDROID_PORT_BUSY and binding selects another tuple.
    for (unsigned attempt = 1;; ++attempt) {
        auto token = voyemu::random_token();
        auto session = voyemu::native_session_directory(args.root, args.profile, args.machine,
                                                        token);
        voyemu::ensure_private_directory(session);

        if (args.machine.selected) {
            const auto activation = voyemu::read_private_json(prepared / "activation.json");
            voyemu::write_private_json(
                    session / "machine-selection.json",
                    {{"machine", args.machine.selector()},
                     {"seed",
                      (args.machine_path.empty() ? args.root / "machines" / args.machine.name
                                                 : args.machine_path)
                              .string()},
                     {"manifest_sha256", activation.at("manifest_sha256")}});
        }

        if (!args.providers.empty()) {
            Json manifests = Json::array();

            for (const auto &path : args.providers) {
                manifests.push_back(voyemu::read_private_json(path, 16384));
            }

            voyemu::write_private_json(session / "providers.json", manifests);
        }

        if (args.exclusive) {
            voyemu::write_private_json(
                    session / "automation-owner.json",
                    {{"schema", 1},
                     {"session_id", token},
                     {"profile", args.profile},
                     {"ownership_id",
                      args.ownership_id.empty() ? voyemu::random_token() : args.ownership_id}});
        }

        voyemu::ProcessSpec spec;
        spec.executable = voyemu::os::executable_path();
        spec.arguments = {"_supervise",     "--root",     args.root.string(),
                          "--profile",      args.profile, "--session",
                          session.string(), "--token",    token};

        if (args.machine.selected) {
            spec.arguments.push_back("--machine");
            spec.arguments.push_back(args.machine.selector());
        }

        if (args.background) {
            spec.arguments.push_back("--background");
        }

        if (args.resume) {
            spec.arguments.push_back("--resume");
        }

        if (args.network_explicit) {
            spec.arguments.push_back(args.network ? "--network=yes" : "--network=no");
        }

        if (args.seconds) {
            spec.arguments.push_back("--seconds");
            spec.arguments.push_back(std::to_string(*args.seconds));
        }

        spec.arguments.push_back(args.cluster ? "--cluster=on" : "--cluster=off");
        spec.arguments.push_back(args.multimedia ? "--multimedia=on" : "--multimedia=off");
        spec.directory = args.root;
        spec.output = session / "supervisor.log";
        spec.detached = true;
        auto child = voyemu::ChildProcess::launch(spec);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        bool leased = false;

        while (std::chrono::steady_clock::now() < deadline && !interrupted) {
            if (!child.alive()) {
                const auto log = session / "supervisor.log";

                if (attempt < 8 && args.multimedia && !args.machine_path.empty() &&
                    voyemu::supervisor_log_code(log) == "ANDROID_PORT_BUSY") {
                    leased = true;
                    break;
                }

                throw voyemu::supervisor_exit_error(log, child.exit_code());
            }

            if (std::filesystem::exists(record)) {
                try {
                    auto active = active_record(args);

                    if (active.at("session_id").get<std::string>() == token) {
                        auto response = voyemu::request(active.at("endpoint").get<std::string>(),
                                                        {{"op", "status"}, {"session_id", token}},
                                                        500);

                        if (response.value("ok", false)) {
                            child.detach();
                            Json output{{"ok", true},
                                        {"session_id", token},
                                        {"endpoint", active.at("endpoint")},
                                        {"with_cluster", args.cluster},
                                        {"with_multimedia", args.multimedia},
                                        {"runtime", "native C++"}};
                            voyemu::append_start_machine_identity(
                                    output, response, args.machine,
                                    args.private_start && args.exclusive ? args.machine_path
                                                                         : std::filesystem::path{});
                            emit(output);

                            return 0;
                        }
                    }
                } catch (const std::exception &) { /* Ignored: an unreachable session is retried
                                                      until the deadline. */
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Ctrl+C stops the starting supervisor, which stops its guests, instead of reporting a
        // timeout; a supervisor that does not exit in time is killed with its process.
        if (interrupted) {
            voyemu::stop_starting_supervisor(child, session, std::chrono::seconds(60));

            throw VoyemuError("INTERRUPTED", "start interrupted");
        }

        if (!leased) {
            throw LocalizedError(
                    "SUPERVISOR_TIMEOUT", "error.start_timeout",
                    {{"log", std::filesystem::absolute(session / "supervisor.log").string()}});
        }

        voyemu::bind_machine_configuration(args.root, args.profile, args.machine, args.machine_path,
                                           voyemu::os::executable_path(), args.multimedia,
                                           args.cluster);
    }
}
} // namespace voyemu::cli
