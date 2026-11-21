#include "error.hpp"
#include "session_internal.hpp"

namespace voyemu {
using namespace session_detail;

void Session::attach_frontend(const Json &input, bool cluster_panel, bool background) {
    auto specification = input;

    if (machine_start_ && !machine_program_.empty() &&
        (specification.value("backend", std::string{}) == "voyemu" ||
         builtin_program_name(specification.at("executable").get<std::string>()))) {
        specification["executable"] = machine_program_.string();
    }

    std::lock_guard lifecycle(lifecycle_mutex_);
    std::lock_guard lock(mutex_);
    validate_pins(specification, machine_start_);

    if (frontend_.pid() || android_spec_.empty() || !android_spec_.value("headless", false)) {
        throw VoyemuError("FRONTEND_OWNER", "a fresh owned headless Android is required");
    }

    if (android_spec_.contains("sdk_control")) {
        specification["discovery_root"] =
                session_sdk_registration_directory(directory_,
                                                   android_spec_.contains("diagnostic_native"))
                        .string();
    }

    const auto grpc = specification.at("grpc_port").get<unsigned>();

    if (grpc < 1024 || grpc > 65535) {
        throw VoyemuError("FRONTEND_GRPC", "invalid prepared port");
    }

    auto runtime = std::filesystem::path(specification.at("discovery_root").get<std::string>());

    if (!runtime.is_absolute()) {
        throw VoyemuError("FRONTEND_DISCOVERY", "absolute SDK registration source required");
    }

    ensure_private_directory(directory_ / "sdk-metadata");
    frontend_spec_ = specification;
    frontend_releases_verified_ = false;
    write_private_json(directory_ / "media-view-owner.json",
                       media::readiness_owner(session_id_, android_spec_));
    ProcessSpec process;
    process.executable = specification.at("executable").get<std::string>();
    process.directory = directory_;
    process.output = directory_ / "native-window.log";
    process.arguments = {
            "--session",
            directory_.string(),
            "--endpoint",
            endpoint_.string(),
            "--profile",
            android_spec_.value("profile_title",
                                directory_.parent_path().parent_path().filename().string()),
            "--splash",
            specification.at("splash").get<std::string>(),
            "--state",
            specification.at("state").get<std::string>(),
            "--assets",
            specification.at("assets").get<std::string>(),
            "--fonts",
            specification.at("fonts").get<std::string>()};

    if (specification.contains("startup")) {
        const auto &startup = specification.at("startup");

        if (!startup.is_array() || startup.size() != 2) {
            throw VoyemuError("FRONTEND_STARTUP", "two screen animations required");
        }

        for (unsigned display = 0; display < 2; ++display) {
            process.arguments.push_back("--startup" + std::to_string(display));
            process.arguments.push_back(startup.at(display).is_null()
                                                ? "black"
                                                : startup.at(display).get<std::string>());
        }
    }

    if (specification.contains("startup_video")) {
        const auto &videos = specification.at("startup_video");

        if (!videos.is_array() || videos.size() != 2) {
            throw VoyemuError("FRONTEND_STARTUP", "two screen startup videos required");
        }

        for (unsigned display = 0; display < 2; ++display) {
            if (videos.at(display).is_string()) {
                process.arguments.push_back("--startup-video" + std::to_string(display));
                process.arguments.push_back(videos.at(display).get<std::string>());
            }
        }
    }

    if (specification.contains("startup_cluster")) {
        process.arguments.push_back("--startup-cluster");
        process.arguments.push_back(specification.at("startup_cluster").get<std::string>());
    }

    // The screens window: the boot animation and startup video of each screen, by role.
    for (const auto *field : {"startup_screens", "startup_video_screens"}) {
        // items() keeps a reference, so the screens outlive the loop in a named value.
        const auto screens = specification.value(field, Json::object());

        for (const auto &[role, path] : screens.items()) {
            process.arguments.push_back(std::string_view(field) == "startup_screens"
                                                ? "--startup-screen"
                                                : "--startup-video-screen");
            process.arguments.push_back(role);
            process.arguments.push_back(path.get<std::string>());
        }
    }

    if (background) {
        process.arguments.push_back("--background");
    }

    process.arguments.push_back(cluster_panel ? "--with-cluster" : "--no-cluster");

    if (specification.value("capture", false)) {
        process.arguments.push_back("--capture");
    }

    if (specification.value("input_smoke", false)) {
        process.arguments.push_back("--input-smoke");
    }

    program_mode(process.executable, "_window", process.arguments,
                 machine_start_ && !machine_program_.empty() &&
                         process.executable == machine_program_);
    record_android_processes(true);
#ifdef _WIN32
    frontend_ = ChildProcess::launch(process);
#else
    frontend_ = launch_session_process(process, directory_, session_id_);
#endif
    record_android_processes(false);
    deliveries_["frontend"] = {{"status", "started"},
                               {"pid", frontend_.pid()},
                               {"with_cluster", cluster_panel},
                               {"cluster_panel", cluster_panel},
                               {"background", background}};
}

void Session::poll_frontend() {
    // A failed later stop phase can retain Android after we deliberately closed this frontend.
    // Its explicit terminal receipt is not a new user/window-exit request. A newly attached window
    // is started again.
    if (frontend_.pid() && !frontend_.alive() &&
        deliveries_.value("frontend", Json::object()).value("status", std::string{}) != "stopped") {
        logging::write(logging::Level::info, "frontend", "window.exited",
                       {{"exit_code", frontend_.exit_code()}});
        test_stop_requested_ = true;

        return;
    }

    if (media_connection_written_ || !android_.alive()) {
        return;
    }

    const auto control = android_spec_.contains("sdk_control") ? android_spec_.at("sdk_control")
                                                               : frontend_spec_;

    if (control.empty()) {
        return;
    }

    const auto grpc = control.at("grpc_port").get<unsigned>();

    if (!android_.owns_tcp_listener(grpc)) {
        deliveries_["android"]["sdk_control"] = "awaiting_owned_grpc_listener";

        return;
    }

    const auto serial = android_spec_.at("port").get<unsigned>();

    if (sdk_descriptor_.authenticated() &&
        (!android_identity_ || !android_.owns_tcp_listener(serial) ||
         !android_.owns_tcp_listener(serial + 1))) {
        deliveries_["android"]["sdk_control"] = "awaiting_owned_guest_identity";

        return;
    }

    ensure_private_directory(directory_ / "sdk-metadata");
    const auto discovery =
            android_spec_.contains("sdk_control")
                    ? session_sdk_registration_directory(
                              directory_, android_spec_.contains("diagnostic_native"))
                    : std::filesystem::path(control.at("discovery_root").get<std::string>());
    const auto copied = sdk_descriptor_.resolve(
            directory_ / "android.log", discovery, directory_ / "sdk-metadata",
            static_cast<unsigned>(android_.pid()), serial, grpc, android_identity_,
            android_.pid_namespace() == "macos-diagnostic");

    if (!copied) {
        deliveries_["android"]["sdk_control"] = "awaiting_sdk_descriptor";

        return;
    }

    write_private_json(directory_ / "media-connection.json",
                       {{"schema", 1},
                        {"session_id", session_id_},
                        {"vm_pid", android_.pid()},
                        {"discovery", copied->string()},
                        {"grpc_port", grpc},
                        {"serial_port", android_spec_.at("port")}});
    media_connection_written_ = true;
    deliveries_["android"]["sdk_control"] = "owned_SDK_descriptor";

    if (frontend_.pid()) {
        deliveries_["frontend"]["connection"] = "owned_SDK_descriptor";
    }
}

void Session::stop_frontend() {
    if (!frontend_.pid()) {
        return;
    }

    request_frontend_stop();
    frontend_.terminate(5000);
    bool released = false;
    try {
        const auto proof = read_private_json(directory_ / "media-native-proof/result.json",
                                             256 * 1024);
        released = proof.value("touches_released", false);
    } catch (const std::exception
                     &) { /* Ignored: a missing terminal receipt makes the checkpoint ineligible. */
    }

    frontend_releases_verified_ = !frontend_.alive() && frontend_.exit_code() == 0 && released;
    deliveries_["frontend"]["status"] = "stopped";
    deliveries_["frontend"]["touches_released"] = frontend_releases_verified_;

    if (media_connection_written_) {
        const auto copied = directory_ / "sdk-metadata" /
                            ("pid_" + std::to_string(android_.pid()) + ".ini");

        if (std::filesystem::is_regular_file(std::filesystem::symlink_status(copied)) &&
            std::filesystem::hard_link_count(copied) == 1) {
            std::filesystem::remove(copied);
        }
    }
}

void Session::set_ui(const std::filesystem::path &executable, const std::filesystem::path &assets,
                     std::string profile_title) {
    std::lock_guard lifecycle(lifecycle_mutex_);
    std::lock_guard lock(mutex_);

    ui_executable_ = executable;
    assets_ = assets;
    ui_profile_title_ = std::move(profile_title);
}

bool Session::fallback_cluster(std::string_view reason) {
    if (!qnx_checkpoint_.resume || qnx_checkpoint_disabled_) {
        return false;
    }

    auto cold = cluster_spec_;
    auto seconds = cluster_seconds_;

    if (seconds) {
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                                       qnx_deadline_ - std::chrono::steady_clock::now())
                                       .count();

        if (remaining < 1) {
            return false;
        }

        seconds = static_cast<unsigned>(remaining);
    }

    logging::write(logging::Level::error, "qnx",
                   qnx_checkpoint_ready_ ? "checkpoint.runtime_failed"
                                         : "checkpoint.restore_failed",
                   {{"reason", reason}, {"action", "cold_start"}});
    try {
        reject_qnx_checkpoint(qnx_checkpoint_, reason, qnx_checkpoint_ready_);
    } catch (const std::exception &) {
        logging::write(logging::Level::error, "qnx", "checkpoint.quarantine_failed",
                       {{"reason", "cache_write_failed"}});
    }

    qnx_checkpoint_disabled_ = true;
    close_cluster(false, "checkpoint_fallback");

    if (!cluster_finalized_) {
        throw VoyemuError("QNX_CHECKPOINT", "previous cleanup incomplete");
    }

    attach_cluster(cold, seconds);

    return true;
}
} // namespace voyemu
