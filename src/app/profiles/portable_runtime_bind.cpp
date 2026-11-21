#include "portable_runtime_internal.hpp"

// Binds a prepared machine configuration to this host: seed, SDK, ports and launch files.
namespace voyemu {
using namespace portable_detail;

Json audio_diagnostic_activation(const std::filesystem::path &record) {
    // The activation record lives inside the caller-owned configured machine directory; its very
    // existence is the authorization. Ambient environment never selects WAV capture, so a forgotten
    // shell variable cannot silently reroute ordinary guest sound.
    const auto path = record / "audio-diagnostic.json";

    if (!std::filesystem::exists(path)) {
        return Json::object();
    }

    ordinary(path);
    need(std::filesystem::is_regular_file(path) && std::filesystem::hard_link_count(path) == 1 &&
                 std::filesystem::file_size(path) <= 4096,
         "bounded audio diagnostic activation record required");
    const auto value = read_private_json(path, 4096);
    need(value.is_object(), "audio diagnostic activation object required");
    const auto profile = value.value("profile", std::string{});
    const auto bundled = embedded_profile_names();
    need(value.value("schema", 0) == 1 && value.value("route", std::string{}) == "wav" &&
                 std::find(bundled.begin(), bundled.end(), profile) != bundled.end(),
         "audio diagnostic activation must be the exact wav route for an owned run");

    // Only the Passion platform runs without a one-run nonce.
    if (!embedded_profile_declares(profile, ProfilePlatform::passion, ProfileBoard::sa8155)) {
        const auto nonce = value.value("nonce", std::string{});
        need(hex_text(nonce, 32), "Owned audio diagnostic requires a one-run nonce");
        need(value.value("directory", std::string{}) == std::filesystem::canonical(record).string(),
             "Owned audio diagnostic differs from the configuration directory");
    }

    return value;
}

bool same_wine_host_path(std::string_view recorded, const std::filesystem::path &local) {
#ifdef _WIN32
    if (!recorded.starts_with("/") || recorded.starts_with("//") || !wine_native_host()) {
        return false;
    }

    return wine_native_unix_path(local) == recorded;
#else
    if (recorded.size() < 4 || (recorded[0] != 'Z' && recorded[0] != 'z') || recorded[1] != ':') {
        return false;
    }

    auto mapped = std::string(recorded.substr(2));
    std::replace(mapped.begin(), mapped.end(), '\\', '/');

    return local.is_absolute() && local.string() == mapped;
#endif
}

bool portable_configuration_from_other_host(const Json &record, const std::filesystem::path &root,
                                            std::string_view profile,
                                            const MachineIdentity &machine,
                                            const std::filesystem::path &seed,
                                            std::string_view manifest_sha) {
    need(machine.valid_for(profile), "invalid machine identity");
    const auto canonical = root / "machines" / machine.name;

    return seed == canonical && record.value("schema", 0) == 1 &&
           record.value("kind", std::string{}) == "runtime-configuration" &&
           record.value("profile", std::string{}) == profile &&
           canonical_machine_name(profile, record.value("machine", std::string{})) ==
                   machine.name &&
           record.value("manifest_sha256", std::string{}) == manifest_sha &&
           same_wine_host_path(record.value("seed", std::string{}), seed);
}

std::filesystem::path bind_machine_configuration(const std::filesystem::path &root,
                                                 std::string_view profile,
                                                 const MachineIdentity &machine,
                                                 const std::filesystem::path &seed,
                                                 const std::filesystem::path &current_program,
                                                 bool multimedia, bool cluster) {
    need(multimedia || cluster, "select multimedia or cluster");
    ordinary(root);
    ordinary(seed);
    ordinary(current_program);
    need(machine.valid_for(profile), "invalid machine identity");
    const auto text = read_private_json_bytes(seed / "runtime.json", 128 * 1024);
    const auto manifest = portable_profile_json(text);
    need(manifest.value("kind", std::string{}) == "portable-guest-runtime" &&
                 manifest.value("profile", std::string{}) == profile &&
                 manifest.value("schema", 0) == 1,
         "portable machine configuration required");
    const auto retained_definition = portable_profile_definition(root, profile, machine, seed);
    const auto family = runtime_profile_reference(retained_definition, profile);
    const auto development_kernel = checked_development_kernel(seed, retained_definition, manifest);
    const auto can_model_sha = checked_can_model_receipt(seed, manifest);

    if (multimedia) {
        (void)portable_resource_path(seed, "ui/splash.png");
    }

    need(!cluster || (manifest.value("scope", std::string{}) != "android" &&
                      manifest.value("cluster_available", true)),
         "selected machine has no cluster");

    if (cluster) {
        need(manifest.contains("qnx"), "selected machine has no cluster");
        (void)qnx_portable_layout(family, manifest.at("qnx"));
    }

    Json prepared_observer = Json::object();

    // A profile with the ui_observer_required feature always binds its UI observer.
    if (multimedia && (manifest.contains("ui_observer") ||
                       profile_feature(retained_definition, "ui_observer_required"))) {
        const auto descriptor = portable_observer_descriptor(seed, manifest);
        const auto pin = descriptor.at("sha256").get<std::string>();
        const auto dex = root / ".local/toolchains/portable-ui-observer" / pin / "classes.dex";
        ensure_private_directory(dex.parent_path());

        if (!std::filesystem::exists(dex)) {
            copy_portable_image(seed / "observer/classes.dex", pin, dex, 0);
        }

        checked_file(dex, pin, 8 * mib);
        prepared_observer = {{"dex", dex.string()}, {"sha256", pin}};

        if (portable_api30_ui_observer_profile(profile)) {
            prepared_observer["profile"] = std::string(profile);
        }
    }

    Json prepared_power = Json::object();

    if (manifest.at("android").contains("display_power")) {
        const auto definition = portable_profile_definition(root, profile, machine, seed);
        prepared_power = portable_passion_power(seed, manifest, definition);
    }

    Json prepared_mcu = Json::object();

    if (manifest.at("android").contains("mcu_property")) {
        const auto definition = portable_profile_definition(root, profile, machine, seed);
        prepared_mcu = portable_passion_mcu(seed, manifest, definition);
    }

    const auto desired_diagnostic = wine_native_diagnostic_binding();
    // A scoped API30 WAV diagnostic run is bound to one private activation record inside the
    // configured machine directory. It is never derived from ambient state and never persists after
    // the owner removes the record, so ordinary launches keep the normal host audio backend.
    // Ordinary configuration refresh rebuilds the launch specification with the normal host audio
    // backend; a live diagnostic activation record is carried forward so an in-flight bounded
    // capture run is never rerouted underneath itself. Removing the record returns later refreshes
    // to the ordinary backend, and the guest's own output volume is untouched in both cases.
    const auto desired_audio =
            audio_diagnostic_activation(profile_launch_directory(root, profile, machine));
    need(desired_audio.empty() || desired_audio.value("profile", std::string{}) == profile,
         "audio diagnostic differs from the selected machine profile");
    const auto &system_descriptor = manifest.at("android").at("images").at("system.img");
    const auto system_relative = portable_system_image_file(
            system_descriptor, manifest.at("android").at("api").get<unsigned>());
    Json desired_qnx_storage = Json::object(), desired_qnx_file_backend = Json::object();
    std::filesystem::path desired_qnx_engine;

#ifdef _WIN32
    // Every Windows launch maps its guest RAM file through the file backend of the pinned engine;
    // a launch records the verified capability of the installed engine.
    const bool windows_file_backend = cluster && !desired_diagnostic.contains("qnx");

    if (windows_file_backend && std::filesystem::exists(qnx_host_bindings_path(root))) {
        const auto local_host = qnx_host_binding_view(
                root, read_private_json(qnx_host_bindings_path(root), 128 * 1024));
        desired_qnx_file_backend = local_host.value("qnx_file_backend", Json::object());

        if (!desired_qnx_file_backend.empty()) {
            need(local_host.contains("qnx_qemu"), "installed QNX engine configuration required");
            desired_qnx_engine = local_host.at("qnx_qemu").get<std::string>();

            if (!qnx_file_backend_supported(desired_qnx_file_backend,
                                            sha256_file(desired_qnx_engine))) {
                throw VoyemuError("QNX_FILE_BACKEND_UNSUPPORTED",
                                  "Windows file-backed engine capability differs");
            }
        }
    }
#endif

    if (cluster && manifest.at("qnx").contains("persistence")) {
        desired_qnx_storage = {{"schema", 1},
                               {"system", (seed / "qnx/sysla-gpt-ramdisk-v3.img").string()},
                               {"userdata", (seed / "qnx/userdata-qnx6.img").string()}};
#ifdef _WIN32
        if (windows_file_backend) {
            if (desired_qnx_file_backend.empty()) {
                throw VoyemuError("QNX_PERSISTENCE_UNSUPPORTED",
                                  "verified Windows file-backed QEMU engine required");
            }

            (void)qnx_persistent_ram_launch(
                    QnxEngine::tcg_file, desired_qnx_storage.at("system").get<std::string>(),
                    desired_qnx_storage.at("userdata").get<std::string>(), true);
        }
#endif

        for (const auto &role : {"system", "userdata"}) {
            const std::filesystem::path file = desired_qnx_storage.at(role).get<std::string>();
            ordinary(file);
            need(std::filesystem::is_regular_file(file) &&
                         std::filesystem::hard_link_count(file) == 1,
                 "ordinary private persistent QNX image required");
        }
    }

    Json desired_system = Json::object();

    if (system_relative != "android/system.img") {
        const auto raw = seed / system_relative;
        desired_system = {{"raw", raw.string()},
                          {"overlay", (raw.parent_path() / "system.img.qcow2").string()},
                          {"sha256", system_descriptor.at("sha256")}};
        (void)android_system_binding({{"api", 30},
                                      {"machine", (seed / "android").string()},
                                      {"system_binding", desired_system}});
    }

    Json desired_package = Json::object();

    if (manifest.at("android").contains("owned_package")) {
        need(embedded_profile_declares(family, ProfilePlatform::qinggan11, ProfileBoard::sa8155) &&
                     manifest.at("android").at("api") == 30,
             "exact owned provider machine required");
        desired_package = manifest.at("android").at("owned_package");
        android_owned_package_descriptor(desired_package, true);
        desired_package["file"] = (seed / desired_package.at("file").get<std::string>()).string();
        (void)android_owned_package_file({{"api", 30},
                                          {"machine", (seed / "android").string()},
                                          {"owned_package", desired_package}});
    }

    Json desired_qinggan09 = Json::object();
    Json desired_camera = Json::object();

    if (manifest.at("android").contains("camera_package")) {
        need(embedded_profile_declares(family, ProfilePlatform::qinggan11, ProfileBoard::sa8155) &&
                     manifest.at("android").at("api") == 30,
             "exact Camera machine required");
        desired_camera = manifest.at("android").at("camera_package");
        android_camera_package_descriptor(desired_camera, true);
        desired_camera["file"] = (seed / desired_camera.at("file").get<std::string>()).string();
        (void)android_camera_package_file({{"api", 30},
                                           {"machine", (seed / "android").string()},
                                           {"reference_profile", family},
                                           {"camera_package", desired_camera}});
    }

    Json desired_prepared = Json::array();

    if (manifest.at("android").contains("prepared_packages")) {
        need(embedded_profile_declares(family, ProfilePlatform::qinggan11, ProfileBoard::sa8155) &&
                     manifest.at("android").at("api") == 30,
             "exact Free A11 prepared list required");
        desired_prepared = manifest.at("android").at("prepared_packages");
        android_prepared_packages_descriptor(desired_prepared, true);

        for (auto &package : desired_prepared) {
            package["file"] = (seed / package.at("file").get<std::string>()).string();
            const auto name = package.at("package").get<std::string>();
            (void)android_package_detail::file({{"api", 30},
                                                {"machine", (seed / "android").string()},
                                                {"prepared_package", package}},
                                               "prepared_package", name,
                                               name == "com.baidu.naviauto" ? 512ULL << 20
                                                                            : 128ULL << 20);
        }
    }

    if (manifest.at("android").contains("qinggan09_owned_packages")) {
        need(record_declares(retained_definition, ProfilePlatform::qinggan09,
                             ProfileBoard::sa8155) &&
                     manifest.at("android").at("api") == 28,
             "exact A9 package machine required");
        desired_qinggan09 = manifest.at("android").at("qinggan09_owned_packages");
        qinggan09_owned_packages_descriptor(desired_qinggan09, true);

        for (unsigned i = 0; i < qinggan09_owned_packages_payloads(desired_qinggan09).size(); ++i) {
            auto package = qinggan09_owned_packages_payloads(desired_qinggan09).at(i);
            package["file"] = (seed / package.at("file").get<std::string>()).string();
            qinggan09_owned_packages_set_payload(desired_qinggan09, i, package);
        }

        qinggan09_owned_packages_descriptor(desired_qinggan09, false);
        (void)qinggan09_owned_packages_files({{"api", 28},
                                              {"profile", profile},
                                              {"reference_profile", family},
                                              {"machine", (seed / "android").string()},
                                              {"qinggan09_owned_packages", desired_qinggan09}});
    }

    Json desired_phone = Json::object();

    if (manifest.at("android").contains("qinggan09_phone_package")) {
        need(record_declares(retained_definition, ProfilePlatform::qinggan09,
                             ProfileBoard::sa8155) &&
                     manifest.at("android").at("api") == 28,
             "exact A9 Phone machine required");
        desired_phone = manifest.at("android").at("qinggan09_phone_package");
        qinggan09_phone_package_descriptor(desired_phone, true);
        auto &package = desired_phone.at("package");
        package["file"] = (seed / package.at("file").get<std::string>()).string();
        (void)qinggan09_phone_package_file({{"api", 28},
                                            {"profile", profile},
                                            {"reference_profile", family},
                                            {"machine", (seed / "android").string()},
                                            {"qinggan09_phone_package", desired_phone}});
    }

    const Json desired_startup = desired_qinggan09.empty()
                                         ? Json::object()
                                         : Json{{"schema", 1},
                                                {"authority", "com.voyemu.output.audio.startup"}};
    const auto binding_file = qnx_host_bindings_path(root);
    const auto bindings =
            qnx_host_binding_view(root, std::filesystem::exists(binding_file)
                                                ? read_private_json(binding_file, 128 * 1024)
                                                : Json::object());
    const std::filesystem::path bound_runtime_sdk =
            bindings.contains("runtime_sdk")
                    ? std::filesystem::path(
                              bindings.at("runtime_sdk").at("sdk_root").get<std::string>())
                    : std::filesystem::path{};
#ifdef _WIN32
    const bool windows_runtime_sdk = true;
#else
    const bool windows_runtime_sdk = false;
#endif
    const std::filesystem::path runtime_sdk = android_runtime_sdk_root(root, bound_runtime_sdk,
                                                                       windows_runtime_sdk);
    const auto active_session = [&](const Json &owner) {
        const auto id = owner.at("session_id").get<std::string>();
        const auto local = native_session_directory(root, profile, machine, id);
        const auto recorded = owner.at("directory").get<std::string>();

        if (same_wine_host_path(recorded, local)) {
            return local;
        }

        const std::filesystem::path directory(recorded);
        need(directory.is_absolute() && same_directory_tail(directory, local, 3),
             "active machine session directory differs");

        return directory;
    };
    const auto selected_active = [&] {
        const auto active = native_current_active_directory(root, profile, machine) /
                            (std::string(profile) + ".json");

        if (!std::filesystem::exists(active)) {
            return false;
        }

        const auto owner = read_private_json(active, 4096);
        const auto id = owner.at("session_id").get<std::string>();
        const auto directory = active_session(owner);

        // A record whose session directory has been removed names no live epoch, so it reads as
        // inactive instead of as a broken configuration; the CLI fences the same epoch the same
        // way.
        if (!ordinary_native_launch_file(directory / "runtime.json")) {
            return false;
        }

        const auto runtime = read_private_json(directory / "runtime.json", 1024 * 1024);
        need(runtime.at("session_id") == id, "active machine session epoch differs");

        return !runtime.value("stopped", false);
    };
    const auto current_android = [&](const std::filesystem::path &directory) {
        if (!ordinary_native_launch_file(directory / "android-launch.json")) {
            return false;
        }

        if (std::filesystem::exists(directory / "activation.json") &&
            read_private_json(directory / "activation.json")
                    .value("android_rebind_pending", false)) {
            return false;
        }

        const auto launch = read_private_json(directory / "android-launch.json");

        if (launch.value("can_model_observer_sha256", std::string{}) != can_model_sha) {
            return false;
        }

        if (!selected_active()) {
            const auto console = launch.value("port", 0u);

            if (!machine_android_ports_available(
                        console,
                        [](unsigned port) {
                            return tcp_port_free(port);
                        },
                        android_port_leasable) ||
                launch.value("sdk_control", Json::object()).value("grpc_port", 0u) !=
                        console + 30000) {
                return false;
            }
        }

        if (launch.value("phone_adapter", Json::object()) !=
            phone_adapter_for_profile(retained_definition,
                                      manifest.at("profile").get<std::string>())) {
            return false;
        }

        if (record_declares(retained_definition, ProfilePlatform::qinggan09, ProfileBoard::nxp) &&
            !nxp_presentation(launch)) {
            return false;
        }

        if (launch.value("system_binding", Json::object()) != desired_system ||
            launch.value("owned_package", Json::object()) != desired_package ||
            launch.value("camera_package", Json::object()) != desired_camera ||
            launch.value("prepared_packages", Json::array()) != desired_prepared ||
            launch.value("qinggan09_owned_packages", Json::object()) != desired_qinggan09 ||
            launch.value("qinggan09_phone_package", Json::object()) != desired_phone ||
            launch.value("audio_startup", Json::object()) != desired_startup) {
            return false;
        }

        const auto frontend = launch.value("frontend", Json::object());

        if (frontend.value("executable", std::string{}) != current_program.string() ||
            frontend.value("source_root", std::string{}) != root.string()) {
            return false;
        }

        if (frontend.value("assets", std::string{}) != (seed / "ui/wheel").string() ||
            frontend.value("fonts", std::string{}) != (seed / "ui/fonts").string() ||
            frontend.value("splash", std::string{}) != (seed / "ui/splash.png").string() ||
            frontend.value("startup", Json()) != portable_startup_animations(seed) ||
            frontend.value("startup_video", Json()) !=
                    portable_startup_video_frontend(seed).value("startup_video", Json()) ||
            frontend.value("startup_cluster", Json()) !=
                    portable_startup_video_frontend(seed).value("startup_cluster", Json())) {
            return false;
        }

        for (const auto *field : {"startup_screens", "startup_video_screens"}) {
            if (frontend.value(field, Json()) !=
                portable_startup_screens_frontend(seed).value(field, Json())) {
                return false;
            }
        }

        if (!qinggan11_wheel_provider_profile(family) &&
            !record_declares(retained_definition, ProfilePlatform::passion, ProfileBoard::sa8155) &&
            (!record_declares(retained_definition, ProfilePlatform::qinggan09, ProfileBoard::nxp) ||
             retained_definition.value("components", Json::object()).contains("wheel"))) {
            Json routes = Json::object();
            const auto route_metadata =
                    read_private_json(portable_resource_path(seed, "ui/wheel-routes.json"));

            for (const auto &row : route_metadata.at("routes")) {
                routes[row.at("id").get<std::string>()] = row.value("qg_code", Json{});
            }

            if (launch.value("wheel_adapter", Json::object()).value("routes", Json::object()) !=
                routes) {
                return false;
            }
        }

        if (launch.value("diagnostic_native", Json::object()) != desired_diagnostic) {
            return false;
        }

        // WAV capture follows the scoped activation record: cache stays current only while its
        // presence/absence matches the record, so an owned re-bind toggles capture per run.
        if (launch.value("diagnostic_audio", std::string{}) !=
            (desired_audio.empty() ? std::string{} : "wav")) {
            return false;
        }

        if (launch.value("android_sdk_root", std::string{}) != runtime_sdk.string() ||
            !launch.contains("pins") || !launch.at("pins").is_array()) {
            return false;
        }

        if (launch.value("adb_transport", std::string("direct")) != "direct") {
            return false;
        }

        if (!development_kernel.is_null()) {
            if (launch.value("machine", std::string{}) != (seed / "android").string() ||
                launch.value("base", std::string{}) != (seed / "android").string() ||
                !launch.contains("args") || !launch.at("args").is_array()) {
                return false;
            }

            for (const auto &arg : launch.at("args")) {
                if (!arg.is_string()) {
                    return false;
                }

                const auto value = arg.get<std::string>();

                if (value == "-kernel" || value.starts_with("-kernel=") || value == "-sysdir" ||
                    value.starts_with("-sysdir=")) {
                    return false;
                }
            }

            unsigned matches = 0;

            for (const auto &item : launch.at("pins")) {
                if (item.is_object() && item.value("path", std::string{}) ==
                                                (seed / "android/kernel-ranchu").string()) {
                    if (item.value("sha256", std::string{}) !=
                        development_kernel.at("sha256").get<std::string>()) {
                        return false;
                    }

                    ++matches;
                }
            }

            if (matches != 1) {
                return false;
            }
        }

        if (launch.value("display_power_capability", Json::object()) != prepared_power) {
            return false;
        }

        if (launch.value("mcu_property_adapter", Json::object()) != prepared_mcu) {
            return false;
        }

        if (!prepared_observer.empty()) {
            if (launch.value("ui_observer", Json::object()) != prepared_observer ||
                !launch.contains("pins") || !launch.at("pins").is_array()) {
                return false;
            }

            unsigned matches = 0;

            for (const auto &item : launch.at("pins")) {
                if (item.is_object() && item.value("path", std::string{}) ==
                                                prepared_observer.at("dex").get<std::string>()) {
                    if (item.value("sha256", std::string{}) !=
                        prepared_observer.at("sha256").get<std::string>()) {
                        return false;
                    }

                    ++matches;
                }
            }

            if (matches != 1) {
                return false;
            }
        }

        const std::filesystem::path data = launch.at("machine").get<std::string>();

        if (!ordinary_native_launch_file(data / "machine.json")) {
            return false;
        }

        if (manifest.value("android", Json::object()).value("api", 28) != 30) {
            return launch.at("base") == data.string() &&
                   bounded_text(launch.at("config_template").get<std::string>())
                                   .find("hw.sdCard=no\n") != std::string::npos;
        }

        if (launch.value("api", 28) != 30) {
            return false;
        }

        const auto definition = portable_profile_definition(root, profile, machine, seed);

        if (record_declares(definition, ProfilePlatform::passion, ProfileBoard::sa8155)) {
            const auto capability = passion_wheel_capability(definition);

            if (launch.value("wheel_capability", Json{}) != capability) {
                return false;
            }

            if (launch.value("presentation_adapter", Json{}) !=
                passion_presentation_contract(family, !prepared_mcu.empty(),
                                              passion_wheel_partial(capability))) {
                return false;
            }

            passion_wheel_validate(launch);
        }

        if (qinggan11_wheel_provider_profile(family)) {
            need(launch.contains("wheel_capability"), "Free cached wheel capability missing");
            (void)qinggan11_wheel_unsupported(launch);
            const auto capability = qinggan11_wheel_capability(definition);

            if (launch.value("wheel_capability", Json{}) != capability) {
                return false;
            }

            if (capability.at("supported") == true) {
                if (launch.value("wheel_adapter", Json{}) != qinggan11_wheel_adapter()) {
                    return false;
                }
            } else if (launch.contains("wheel_adapter")) {
                return false;
            }

            (void)qinggan11_wheel_unsupported(launch);
        }

        return launch.at("base") == data.string() &&
               bounded_text(launch.at("config_template").get<std::string>()) ==
                       runtime_avd_config(definition, data,
                                          launch.at("avd_name").get<std::string>());
    };
    const auto current_cluster = [&](const std::filesystem::path &directory,
                                     bool check_graphics = true) {
        if (!ordinary_native_launch_file(directory / "cluster-launch.json") ||
            !manifest.contains("qnx")) {
            return false;
        }

        if (std::filesystem::exists(directory / "activation.json") &&
            read_private_json(directory / "activation.json")
                    .value("cluster_rebind_pending", false)) {
            return false;
        }

        const auto layout = qnx_portable_layout(family, manifest.at("qnx"));
        const auto launch = read_private_json(directory / "cluster-launch.json");

        if (!selected_active() && launch.contains("qmp") && launch.at("qmp").is_object() &&
            !tcp_port_free(launch.at("qmp").at("port").get<unsigned>())) {
            return false;
        }

        if (check_graphics &&
            launch.value("graphics", Json::object()).value("executable", std::string{}) !=
                    current_program.string()) {
            return false;
        }

        // A Windows launch from before the shared RAM file kept the guest RAM in QEMU.
        if (launch.value("persistent_storage", Json::object()) != desired_qnx_storage ||
            launch.value("ram_backend", std::string{}) != "file") {
            return false;
        }

        if (launch.value("qnx_file_backend", Json::object()) != desired_qnx_file_backend ||
            (!desired_qnx_file_backend.empty() &&
             launch.at("qemu").at("executable") != desired_qnx_engine.string())) {
            return false;
        }

#ifdef _WIN32
        const auto clock_engine = desired_diagnostic.contains("qnx") ? QnxEngine::hvf
                                                                     : host_qnx_engine;

        if (!portable_qnx_diagnostic_current(launch, desired_diagnostic)) {
            return false;
        }
#else
        constexpr auto clock_engine = host_qnx_engine;
#endif

        if (layout.memory_mib == 11713) {
            if (launch.value("hardware_adapter", std::string{}) != layout.adapter) {
                return false;
            }

            const auto args = launch.at("qemu").at("args").get<std::vector<std::string>>();
            unsigned memory = 0;

            for (std::size_t i = 0; i + 1 < args.size(); ++i) {
                if (args[i] == "-m") {
                    if (args[i + 1] != "11713") {
                        return false;
                    }

                    ++memory;
                }
            }

            if (memory != 1) {
                return false;
            }

            for (const auto &expected : qnx_ram_launch(clock_engine, layout.memory_mib).options) {
                if (std::count(args.begin(), args.end(), expected) != 1) {
                    return false;
                }
            }
        }

        if (!qnx_ram_clock_options_current(
                    clock_engine, launch.at("qemu").at("args").get<std::vector<std::string>>())) {
            return false;
        }

        if (layout.projection.is_null()) {
            return true;
        }

        return launch.value("hardware_adapter", std::string{}) == layout.adapter &&
               qnx_mcu_delivery_scope(
                       Json{{"mcu_projection", launch.value("mcu_projection", Json())}},
                       layout.projection)
                       .has_value() &&
               launch.value("simulated_initial_native_state", Json()) ==
                       layout.simulated_initial_native_state;
    };
    std::unique_ptr<InactiveProfileLease> inactive_profile;
    const auto require_stopped_profile = [&] {
        const auto active = native_current_active_directory(root, profile, machine) /
                            (std::string(profile) + ".json");

        if (std::filesystem::exists(active)) {
            const auto owner = read_private_json(active, 4096);
            const auto directory = active_session(owner);
            const bool foreign_epoch = same_wine_host_path(owner.at("directory").get<std::string>(),
                                                           directory);
            // A record whose session directory has been removed names no epoch to stop, so the
            // refresh proceeds on the same host. The other host needs a verified terminal record.
            need(!foreign_epoch || ordinary_native_launch_file(directory / "runtime.json"),
                 "other host has no terminal session record");

            if (ordinary_native_launch_file(directory / "runtime.json")) {
                const auto runtime = read_private_json(directory / "runtime.json", 1024 * 1024);
                need(runtime.at("session_id") == owner.at("session_id"),
                     "active machine session epoch differs");
                need(!foreign_epoch || runtime.value("stopped", false),
                     "stop selected profile on the other host before refreshing prepared configuration");

                if (!runtime.value("stopped", false) && !inactive_profile) {
                    try {
                        inactive_profile = std::make_unique<InactiveProfileLease>(
                                root, profile, machine, owner, runtime, desired_qnx_storage);
                    } catch (const std::exception
                                     &) { /* Ignored: unverified stale epochs remain protected. */
                    }
                }

                need(runtime.value("stopped", false) ||
                             (inactive_profile && inactive_profile->matches(owner, runtime)),
                     "stop selected profile before refreshing prepared configuration");
            }
        }
    };
    const auto cached = profile_launch_directory(root, profile, machine);
    // A changed manifest, a moved machine or a configuration from another host rebinds the host
    // configuration; the writable disks of the prepared runtime stay.
    bool rebind_configuration = false, foreign_host = false;
    std::filesystem::path relocated_from;
    Json relocation_record, relocated_activation;
    const auto missing_seed = [&](const std::filesystem::path &path) {
        if (!path.is_absolute() || path.lexically_normal() != path) {
            return false;
        }

        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);

        if ((error && error != std::errc::no_such_file_or_directory) ||
            status.type() != std::filesystem::file_type::not_found) {
            return false;
        }

        for (const auto &parent : os::ancestors(path.parent_path())) {
            error.clear();
            const auto state = std::filesystem::symlink_status(parent, error);

            if (error == std::errc::no_such_file_or_directory) {
                continue;
            }

            if (error || std::filesystem::is_symlink(state)) {
                return false;
            }
        }

        return true;
    };

    if (std::filesystem::exists(cached / "activation.json")) {
        const auto record = read_private_json(cached / "activation.json");
        need(record.at("profile") == profile, "cached profile identity differs");
        const std::filesystem::path prior_seed = record.value("seed", std::string{});
        const auto selected_name = machine.name;
        foreign_host = portable_configuration_from_other_host(record, root, profile, machine, seed,
                                                              sha256_bytes(text));
        need(prior_seed.empty() || prior_seed.is_absolute() || foreign_host,
             "cached seed is not a verified path on either host");
        const bool relocated = record.value("kind", std::string{}) == "runtime-configuration" &&
                               canonical_machine_name(profile,
                                                      record.value("machine", std::string{})) ==
                                       machine.name &&
                               !prior_seed.empty() && prior_seed != seed &&
                               prior_seed.filename() == selected_name &&
                               seed.filename() == selected_name && missing_seed(prior_seed);

        if (relocated) {
            relocated_from = prior_seed;
        }

        if (relocated || foreign_host) {
            relocation_record = record;
        }

        rebind_configuration = relocated || foreign_host ||
                               record.value("manifest_sha256", std::string{}) != sha256_bytes(text);

        if (machine.selected) {
            need(canonical_machine_name(profile, record.value("machine", std::string{})) ==
                                 machine.name &&
                         record.contains("seed") &&
                         (relocated || foreign_host ||
                          (prior_seed.is_absolute() &&
                           std::filesystem::equivalent(prior_seed, seed))),
                 "cached named machine identity differs");
        }

        if (rebind_configuration && !machine.selected && record.contains("runtime")) {
            need(seed == root / "machines" / std::string(profile),
                 "default cached configuration requires its canonical machine");
            const std::filesystem::path old_runtime = record.at("runtime").get<std::string>();
            inside(root, old_runtime);
            ordinary(old_runtime);
        }
    }

    const auto verify_relocation = [&] {
        if (foreign_host || !relocated_from.empty()) {
            const auto current = read_private_json(cached / "activation.json");
            need((current == relocation_record ||
                  (!relocated_activation.is_null() && current == relocated_activation)) &&
                         (foreign_host || missing_seed(relocated_from)),
                 "configuration origin changed during rebind");
            require_stopped_profile();
        }
    };

    if (rebind_configuration) {
        require_stopped_profile();
    }

    if (!rebind_configuration && (!multimedia || current_android(cached)) &&
        (!cluster || current_cluster(cached))) {
        if (!machine.selected) {
            need(seed == root / "machines" / std::string(profile),
                 "default cached configuration requires its canonical machine");
        }

        if (std::filesystem::exists(cached / "activation.json")) {
            const auto record = read_private_json(cached / "activation.json");
            need(record.at("profile") == profile, "cached profile identity differs");

            if (machine.selected) {
                const std::filesystem::path recorded_seed = record.at("seed").get<std::string>();
                ordinary(recorded_seed, true);
                need(canonical_machine_name(profile, record.at("machine").get<std::string>()) ==
                                     machine.name &&
                             std::filesystem::equivalent(recorded_seed, seed),
                     "cached machine path identity differs");
            }
        } else {
            need(!machine.selected, "named machine configuration identity missing");
        }

        return cached;
    }

    need(!cluster || manifest.contains("qnx"), "selected machine has no cluster");
    const auto scoped = native_machine_directory(root, profile, machine);
    const bool android_ready = !rebind_configuration && current_android(scoped),
               cluster_ready = !rebind_configuration && current_cluster(scoped);

    if ((!multimedia || android_ready) && (!cluster || cluster_ready)) {
        return scoped;
    }

    PortableRuntimeHostBindings host;
    host.seed = seed;
    host.sdk = runtime_sdk;
    host.android_port = bindings.value("android_port", 5574u);
    host.qmp_port = bindings.value("qmp_port", 45995u);
    host.ui_observer = prepared_observer;

    if (!prepared_observer.empty()) {
        host.pins[prepared_observer.at("dex").get<std::string>()] =
                prepared_observer.at("sha256").get<std::string>();
    }

#ifdef _WIN32
    const std::string suffix = ".exe", library = ".dll";
#elif defined(__APPLE__)
    const std::string suffix, library = ".dylib";
#else
    const std::string suffix, library = ".so";
#endif
    const auto require_file = [&](const std::filesystem::path &file) {
        std::error_code error;
        const bool present = std::filesystem::is_regular_file(file, error);
        need(!error && present,
             ("installed dependency or machine file missing: " + file.filename().string()).c_str());
        ordinary(file);
    };
    const auto image = [&](const Json &group, const std::string &name) {
        const std::filesystem::path relative = group.at(name).at("file").get<std::string>();
        need(!relative.empty() && !relative.is_absolute() &&
                     relative == relative.lexically_normal(),
             "relative machine image path required");

        for (const auto &part : relative) {
            need(part != "..", "image path leaves selected machine");
        }

        const auto file = seed / relative;
        require_file(file);

        return file;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    ensure_private_directory(scoped.parent_path());
    auto lock = owned_directory(scoped.parent_path() /
                                (scoped.filename().string() + ".configuration.lock"));
    auto stage =
            owned_directory(scoped.parent_path() / (".configuration-pending-" + random_token()));
    verify_relocation();
    Json previous = Json::object();
    std::map<std::filesystem::path, Json> refresh_identities;
    std::array<HostRefreshRecord, 3> refresh_records;
    // The CLI lock creates scope/active before any configuration exists. Accept only
    // that pristine lifecycle shell; incomplete launch payloads must remain errors.
    const auto pristine_lifecycle = [&] {
        ordinary(scoped);
        need(std::filesystem::is_directory(scoped), "initial configuration directory required");
#ifndef _WIN32
        const auto owned = [&](const std::filesystem::path &path, bool directory) {
            struct stat info{};
            need(::lstat(path.c_str(), &info) == 0 && info.st_uid == ::geteuid() &&
                         (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode)) &&
                         (path == scoped || !(info.st_mode & 0077)),
                 "private owned lifecycle path required");
        };
        owned(scoped, true);
#endif
        std::map<std::filesystem::path, Json> locks;

        for (const auto &entry : std::filesystem::directory_iterator(scoped)) {
            if (entry.path().filename() == "state") {
                ordinary(entry.path());
                need(entry.is_directory(), "retained state directory required");
#ifndef _WIN32
                struct stat info{};
                need(::lstat(entry.path().c_str(), &info) == 0 && info.st_uid == ::geteuid() &&
                             S_ISDIR(info.st_mode) && !(info.st_mode & 0022),
                     "owned non-writable retained state directory required");
#endif

                for (const auto &state : std::filesystem::directory_iterator(entry.path())) {
                    const auto name = state.path().filename().string();
                    need(name == "placement.json" || name == "vehicle-state.json",
                         "initial lifecycle contains unknown state");
                    ordinary(state.path());
                    need(state.is_regular_file() &&
                                 std::filesystem::hard_link_count(state.path()) == 1 &&
                                 std::filesystem::file_size(state.path()) <= 65536,
                         "bounded unaliased retained state required");
#ifndef _WIN32
                    owned(state.path(), false);
#endif
                    need(read_private_json(state.path(), 65536).is_object(),
                         "retained state object required");
                    locks.emplace(state.path(),
                                  Json{{"identity", cache_file_identity(state.path())},
                                       {"sha256", sha256_file(state.path())}});
                }

                continue;
            }

            need(entry.path().filename() == "active",
                 "initial configuration contains unowned payload");
            ordinary(entry.path());
            need(entry.is_directory(), "initial lifecycle directory required");
#ifndef _WIN32
            owned(entry.path(), true);
#endif

            for (const auto &lock : std::filesystem::directory_iterator(entry.path())) {
                const auto name = lock.path().filename().string();
                need(name == std::string(profile) + ".launch.lock" ||
                             name == std::string(profile) + ".supervisor.lock",
                     "initial lifecycle contains unknown file");
                ordinary(lock.path());
#ifdef _WIN32
                // The CLI holds this lock with share=0. Metadata-only access remains
                // compatible; generic read access would reject every Windows start.
                struct Metadata {
                    HANDLE file = INVALID_HANDLE_VALUE;

                    ~Metadata() {
                        if (file != INVALID_HANDLE_VALUE) {
                            CloseHandle(file);
                        }
                    }
                } metadata;
                metadata.file = CreateFileW(lock.path().c_str(), 0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                                            nullptr);
                BY_HANDLE_FILE_INFORMATION info{};
                need(metadata.file != INVALID_HANDLE_VALUE &&
                             GetFileType(metadata.file) == FILE_TYPE_DISK &&
                             GetFileInformationByHandle(metadata.file, &info) &&
                             !(info.dwFileAttributes &
                               (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                             info.nNumberOfLinks == 1 && !info.nFileSizeHigh && !info.nFileSizeLow,
                     "ordinary empty lifecycle lock required");
                locks.emplace(lock.path(), Json{{"volume", info.dwVolumeSerialNumber},
                                                {"high", info.nFileIndexHigh},
                                                {"low", info.nFileIndexLow},
                                                {"write_high", info.ftLastWriteTime.dwHighDateTime},
                                                {"write_low", info.ftLastWriteTime.dwLowDateTime}});
#else
                need(lock.is_regular_file() && std::filesystem::hard_link_count(lock.path()) == 1 &&
                             std::filesystem::file_size(lock.path()) == 0,
                     "ordinary empty lifecycle lock required");
                owned(lock.path(), false);
                locks.emplace(lock.path(), cache_file_identity(lock.path()));
#endif
            }
        }

        return locks;
    };
    const bool initial_lifecycle = std::filesystem::exists(scoped) &&
                                   !std::filesystem::exists(scoped / "activation.json") &&
                                   !std::filesystem::is_symlink(scoped / "activation.json");
    const auto initial_locks = initial_lifecycle ? pristine_lifecycle()
                                                 : std::map<std::filesystem::path, Json>{};

    if (std::filesystem::exists(scoped) && !initial_lifecycle) {
        // The recorded seed may be spelled on the preparing host, so match its trailing components
        // instead: seed already passed ordinary() above, and equivalent() would throw on a foreign
        // spelling it cannot resolve.
        ordinary(scoped);
        previous = read_private_json(scoped / "activation.json");

        if (!rebind_configuration) {
            need(previous.value("kind", std::string{}) == "runtime-configuration" &&
                         previous.at("profile") == profile &&
                         canonical_machine_name(profile,
                                                previous.at("machine").get<std::string>()) ==
                                 machine.name &&
                         same_directory_tail(
                                 std::filesystem::path(previous.at("seed").get<std::string>()),
                                 seed, 2),
                 "existing configuration belongs to another machine");
        }

        if (multimedia && !android_ready &&
            ordinary_native_launch_file(scoped / "android-launch.json")) {
            unsigned index = 0;

            for (const auto *name : {"android-launch.json", "activation.json"}) {
                const auto file = scoped / name;
                ordinary(file);
                const auto epoch = cache_file_identity(file);
                const auto bytes = read_private_json_bytes(file, 128 * 1024);
                need(cache_file_identity(file) == epoch, "cached configuration snapshot changed");
                refresh_identities[file] = epoch;
                refresh_records[index++] = {file, bytes, {}};
            }

            need(Json::parse(refresh_records[1].before) == previous,
                 "cached activation changed before snapshot");
        }

        if (cluster && !cluster_ready &&
            ordinary_native_launch_file(scoped / "cluster-launch.json")) {
            for (const auto index : {1u, 2u}) {
                if (!refresh_records[index].path.empty()) {
                    continue;
                }

                const auto file = scoped / (index == 1 ? "activation.json" : "cluster-launch.json");
                ordinary(file);
                const auto epoch = cache_file_identity(file);
                const auto bytes = read_private_json_bytes(file, 128 * 1024);
                need(cache_file_identity(file) == epoch,
                     "cached cluster configuration snapshot changed");
                refresh_identities[file] = epoch;
                refresh_records[index] = {file, bytes, {}};
            }

            need(Json::parse(refresh_records[1].before) == previous,
                 "cached activation changed before cluster snapshot");
        }
    }

    std::vector<std::pair<std::filesystem::path, Json>> created;
    const auto publish = [&](const std::filesystem::path &from, const std::filesystem::path &to) {
        rename_exclusive(from, to);
        created.emplace_back(to, cache_file_identity(to));
    };
    const auto run = [&](const std::filesystem::path &tool, const std::vector<std::string> &args,
                         const std::filesystem::path &directory,
                         const std::map<std::string, std::string> &environment =
                                 std::map<std::string, std::string>{}) {
        require_file(tool);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
        need(remaining > 0, "configuration tool deadline exceeded");
        ProcessSpec command;
        command.executable = tool;
        command.arguments = args;
        command.directory = directory;
        command.environment = {{"TMPDIR", stage.path.string()},
                               {"TMP", stage.path.string()},
                               {"TEMP", stage.path.string()}};
        command.environment.insert(environment.begin(), environment.end());
#ifdef _WIN32
        if (!desired_diagnostic.empty()) {
            need(remaining >= 1000, "diagnostic disk tool deadline reached");
            const auto limit = std::min<std::int64_t>(remaining, 10000);
            const auto task = stage.path / ("disk-tool-" + random_token());
            command.output = stage.path / (task.filename().string() + ".log");
            command.wine_native_diagnostic =
                    WineNativeDiagnostic{desired_diagnostic.at("launcher").get<std::string>(),
                                         task,
                                         {},
                                         static_cast<std::uint64_t>(limit)};
            auto child = ChildProcess::launch(command);
            const auto finish = std::chrono::steady_clock::now() + std::chrono::milliseconds(limit);

            while (child.alive() && std::chrono::steady_clock::now() < finish) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            const bool timed_out = child.alive();

            if (timed_out) {
                child.terminate(1000);
            }

            need(!timed_out && child.exit_code() == 0,
                 "native disk tool could not prepare writable overlay");

            return;
        }
#endif
        const auto result =
                execute(command, static_cast<int>(std::min<std::int64_t>(remaining, 10000)), 16384);
        need(!result.timed_out && result.exit_code == 0,
             "installed disk tool could not prepare writable overlay");
    };
    try {
        if (multimedia && !android_ready) {
            ordinary(host.sdk);
            const auto &android = manifest.at("android");
            const auto definition = portable_profile_definition(root, profile, machine, seed);
            const auto api = android.value("api", 0);
            const auto archive = android.at("base_archive_sha256").get<std::string>();
            need(android.value("abi", std::string{}) == "arm64-v8a" && (api == 28 || api == 30) &&
                         sha256_text(archive),
                 "API28/API30 arm64 configuration required");

            if (record_declares(definition, ProfilePlatform::passion, ProfileBoard::sa8155)) {
                host.android_port = 5590;
            } else if (!bindings.contains("android_port")) {
                host.android_port =
                        definition.value("emulator", Json::object()).value("port", 5574u);

                if (embedded_profile_declares(family, ProfilePlatform::qinggan09,
                                              ProfileBoard::sa8155) &&
                    host.android_port == 5562) {
                    host.android_port = 5574;
                }
            }

            need(host.android_port >= 5554 && host.android_port <= 5682 &&
                         host.android_port % 2 == 0 && host.android_port != 5562,
                 "isolated profile Android port required");

            if (ordinary_native_launch_file(scoped / "android-launch.json")) {
                const auto previous_console =
                        read_private_json(scoped / "android-launch.json").value("port", 0u);

                if (machine_android_ports_available(
                            previous_console,
                            [](unsigned port) {
                                return tcp_port_free(port);
                            },
                            android_port_leasable)) {
                    host.android_port = previous_console;
                }
            }

            host.android_port = select_machine_console_port(
                    host.android_port,
                    [](unsigned port) {
                        return tcp_port_free(port);
                    },
                    android_port_leasable);

            need(definition.contains("android"), "profile Android base descriptor required");
            need(native_base_api(definition.at("android")) == static_cast<unsigned>(api) &&
                         definition.at("android").at("sha256") == archive,
                 "machine base/API differs from selected profile");

            host.clean_base =
                    desired_diagnostic.empty()
                            ? seed / "android"
                            : std::filesystem::path(
                                      desired_diagnostic.at("base_root").get<std::string>());
            ordinary(host.clean_base);

            if (!desired_diagnostic.empty()) {
                need(host.clean_base.filename() == archive,
                     "diagnostic clean base archive differs");

                for (const auto &[name, sha] : portable_clean_boot_pins(definition.at("android"))) {
                    checked_file(host.clean_base / name, sha, 64 * mib);
                }
            }

            const auto &images = android.at("images");
            const auto system = image(images, "system.img"), vendor = image(images, "vendor.img"),
                       userdata = image(images, "userdata-qemu.img"),
                       encryption = image(images, "encryptionkey.img");
            const auto data = userdata.parent_path();
            need(encryption.parent_path() == data && vendor.parent_path() == data &&
                         (system.parent_path() == data || !desired_system.empty()),
                 "portable Android images must share their data directory");

            if (api == 30) {
                need((system == data / "system.img" || !desired_system.empty()) &&
                             vendor == data / "vendor.img",
                     "API30 prepared sysdir requires canonical system/vendor names");
            }

            require_stopped_profile();
            require_file(android_sdk_program(host.sdk, !suffix.empty()));

            for (const auto &[name, sha] : portable_clean_boot_pins(definition.at("android"))) {
                require_file(host.clean_base / name);
            }

            const auto disk_sdk =
                    desired_diagnostic.empty()
                            ? host.sdk
                            : std::filesystem::path(
                                      desired_diagnostic.at("sdk_root").get<std::string>());
            const auto disk_suffix = desired_diagnostic.empty() ? suffix : std::string{};
            const bool windows_disk = !disk_suffix.empty();
            const auto qemu_image = disk_sdk / "emulator" / ("qemu-img" + disk_suffix),
                       mkfs = android_sdk_ext4_program(disk_sdk, windows_disk),
                       mkfs_config = android_sdk_ext4_config(disk_sdk, windows_disk);
            auto disk_stage = owned_directory(stage.path / (".runtime-pending-" + random_token()));
            const auto cache = data / "cache.img";

            if (!std::filesystem::exists(cache)) {
                run(qemu_image,
                    {"create", "-f", "raw", (disk_stage.path / "cache.img").string(), "69206016"},
                    disk_stage.path);

                if (!mkfs_config.empty()) {
                    require_file(mkfs_config);
                }

                run(mkfs, android_sdk_ext4_arguments(disk_stage.path / "cache.img", windows_disk),
                    disk_stage.path, android_sdk_ext4_environment(disk_sdk, windows_disk));
                publish(disk_stage.path / "cache.img", cache);
            } else {
                require_file(cache);
            }

            for (const auto &backing : {userdata, encryption, system, vendor}) {
                const auto output = backing.parent_path() /
                                    (backing.filename().string() + ".qcow2");

                if (std::filesystem::exists(output)) {
                    require_file(output);
                    continue;
                }

                const auto pending = disk_stage.path / output.filename();
                run(qemu_image,
                    {"create", "-f", "qcow2", "-F", "raw", "-b", backing.string(),
                     pending.string()},
                    disk_stage.path);
                need(std::filesystem::is_regular_file(pending) &&
                             std::filesystem::file_size(pending) < 16 * mib,
                     "disk tool output is not a small overlay");
                run(qemu_image,
                    {"rebase", "-u", "-f", "qcow2", "-F", "raw", "-b", backing.filename().string(),
                     pending.string()},
                    disk_stage.path);
                publish(pending, output);
            }

            const auto assets = seed / "ui/wheel";
            (void)portable_resource_path(seed, "ui/wheel/wheel-hotspots.json");
            ordinary(assets);
            const auto splash = portable_resource_path(seed, "ui/splash.png");
            const auto control = portable_sdk_control_binding(root, host);
            host.frontend = {
                    {"schema", 1},
                    {"backend", "voyemu"},
                    {"executable", current_program.string()},
                    {"source_root", root.string()},
                    {"assets", assets.string()},
                    {"fonts", (seed / "ui/fonts").string()},
                    {"splash", splash.string()},
                    {"state",
                     (native_current_active_directory(root, profile, machine).parent_path() /
                      "state/placement.json")
                             .string()},
                    {"grpc_port", control.at("grpc_port")},
                    {"discovery_root", control.at("discovery_root")},
                    {"capture", false},
                    {"pins", Json::array()}};

            if (const auto startup = portable_startup_animations(seed); !startup.is_null()) {
                host.frontend["startup"] = startup;
            }

            host.frontend.update(portable_startup_video_frontend(seed));
            host.frontend.update(portable_startup_screens_frontend(seed));

            const auto launch_base = data;
            const auto avd = "voyemu-" + machine.name;
            const auto config = runtime_avd_config(definition, launch_base, avd);
            private_text(stage.path / "config-template.ini", config);

            if (!std::filesystem::exists(data / "machine.json")) {
                write_private_json(disk_stage.path / "machine.json",
                                   {{"schema", 1},
                                    {"kind", "portable-machine-runtime"},
                                    {"profile", profile}});
                publish(disk_stage.path / "machine.json", data / "machine.json");
            } else {
                need(read_private_json(data / "machine.json").value("profile", std::string{}) ==
                             profile,
                     "data directory belongs to another profile");
            }

            auto launch = portable_android_launch(root, host, data, launch_base,
                                                  scoped / "config-template.ini", avd,
                                                  Json::array(), definition, desired_audio,
                                                  manifest.at("profile").get<std::string>());

            if (!relocated_from.empty()) {
                launch["previous_machine"] = (relocated_from / "android").string();
            }

            if (!development_kernel.is_null()) {
                add_pin(launch["pins"], seed / "android/kernel-ranchu",
                        development_kernel.at("sha256").get<std::string>());
            }

            if (!prepared_power.empty()) {
                launch["display_power_capability"] = prepared_power;
            }

            if (!prepared_mcu.empty()) {
                launch["mcu_property_adapter"] = prepared_mcu;
                launch["presentation_adapter"] = passion_presentation_contract(
                        family, true,
                        passion_wheel_partial(launch.value("wheel_capability", Json())));
            }

            if (api == 28) {
                launch["args"].insert(launch["args"].end(),
                                      {"-system", system.string(), "-vendor", vendor.string()});
            }

            if (manifest.contains("qnx") && manifest.at("qnx").contains("interguest_network")) {
                launch["interguest_network"] = manifest.at("qnx").at("interguest_network");
            }

            if (!can_model_sha.empty()) {
                launch["can_model_observer_sha256"] = can_model_sha;
            }

            if (!desired_system.empty()) {
                launch["system_binding"] = desired_system;
            }

            if (!desired_package.empty()) {
                launch["owned_package"] = desired_package;
            }

            if (!desired_camera.empty()) {
                launch["camera_package"] = desired_camera;
            }

            if (!desired_prepared.empty()) {
                launch["prepared_packages"] = desired_prepared;
            }

            if (!desired_qinggan09.empty()) {
                launch["qinggan09_owned_packages"] = desired_qinggan09;
                launch["audio_startup"] = desired_startup;
                launch["profile"] = profile;
            }

            if (!desired_phone.empty()) {
                launch["qinggan09_phone_package"] = desired_phone;
                launch["profile"] = profile;
            }

            write_private_json(stage.path / "android-launch.json", launch);
        }

        if (cluster && !cluster_ready) {
            require_stopped_profile();

            if (!rebind_configuration && current_cluster(scoped, false)) {
                auto launch = Json::parse(refresh_records[2].before);
                const auto old_program = launch.at("graphics").at("executable").get<std::string>();
                launch["graphics"]["executable"] = current_program.string();

                for (auto &entry : launch.at("pins")) {
                    if (entry.at("path") == old_program) {
                        entry["path"] = current_program.string();
                        entry["sha256"] = sha256_file(current_program);
                    }
                }

                write_private_json(stage.path / "cluster-launch.json", launch);
            } else {
                need(bindings.contains("qnx_qemu") && bindings.contains("angle"),
                     "installed QNX engine and ANGLE configuration required");
                // The cluster renderer is this PE worker, so under the Wine native diagnostic the
                // QNX guest engine is the pinned Mach-O host binary from the diagnostic binding
                // while ANGLE is the pinned Windows emulator library set, not the mac dylibs named
                // by the shared native binding file.
                const bool diagnostic_qnx = desired_diagnostic.contains("qnx");
                host.qnx_qemu =
                        diagnostic_qnx
                                ? desired_diagnostic.at("qnx").at("executable").get<std::string>()
                                : bindings.at("qnx_qemu").get<std::string>();
                host.angle = diagnostic_qnx ? (runtime_sdk / "emulator/lib64/gles_angle").string()
                                            : bindings.at("angle").get<std::string>();
                require_file(host.qnx_qemu);

                for (const auto *name : {"libEGL", "libGLESv2"}) {
                    require_file(host.angle / (std::string(name) + library));
                }

#ifdef _WIN32
                if (windows_file_backend && desired_qnx_file_backend.empty()) {
                    throw VoyemuError("QNX_FILE_BACKEND_UNSUPPORTED",
                                      "verified Windows file-backed QEMU engine required");
                }
#endif

                if (!desired_qnx_file_backend.empty()) {
                    need(host.qnx_qemu == desired_qnx_engine &&
                                 bindings.value("qnx_file_backend", Json::object()) ==
                                         desired_qnx_file_backend,
                         "verified QNX file-backed host binding changed");
                }

                const auto &qnx = manifest.at("qnx");
                const auto layout = qnx_portable_layout(family, qnx);
                need(qnx.value("guest_arch", std::string{}) == "aarch64",
                     "unsupported cluster guest architecture");
                std::map<std::string, std::filesystem::path> images;

                for (const auto &name : layout.images) {
                    images[name] = image(qnx.at("images"), name);
                }

                const auto ram =
                        desired_qnx_storage.empty()
                                ? qnx_ram_launch(host_qnx_engine, layout.memory_mib)
                                : qnx_persistent_ram_launch(
                                          desired_diagnostic.contains("qnx") ? QnxEngine::hvf
                                                                             : host_qnx_engine,
                                          desired_qnx_storage.at("system").get<std::string>(),
                                          desired_qnx_storage.at("userdata").get<std::string>(),
                                          !desired_qnx_file_backend.empty());
                std::vector<std::string> args{"-machine",    ram.machine,
                                              "-accel",      ram.accelerator,
                                              "-cpu",        qnx_engine_cpu(host_qnx_engine),
                                              "-m",          std::to_string(layout.memory_mib),
                                              "-smp",        "8",
                                              "-nodefaults", "-display",
                                              "none"};
                Json qmp;
#ifdef _WIN32
                if (android_ready) {
                    host.android_port = read_private_json(scoped / "android-launch.json")
                                                .at("port")
                                                .get<unsigned>();
                }

                if (ordinary_native_launch_file(scoped / "cluster-launch.json")) {
                    const auto old_qmp =
                            read_private_json(scoped / "cluster-launch.json").value("qmp", Json{});

                    if (old_qmp.is_object()) {
                        host.qmp_port = old_qmp.at("port").get<unsigned>();
                    }
                }

                host.qmp_port = select_machine_qmp_port(host.qmp_port, host.android_port,
                                                        [](unsigned port) {
                                                            return tcp_port_free(port);
                                                        });
                qmp = {{"host", "127.0.0.1"}, {"port", host.qmp_port}};
                args.insert(args.end(), {"-qmp", "tcp:127.0.0.1:" + std::to_string(host.qmp_port) +
                                                         ",server=on,wait=off"});
#else
                qmp = "${RUNTIME}/${TOKEN}-qmp.sock";
                args.insert(args.end(),
                            {"-qmp", "unix:${RUNTIME}/${TOKEN}-qmp.sock,server=on,wait=off"});
#endif
                args.insert(args.end(), {"-nic", "none", "-no-reboot", "-kernel",
                                         images.at("hyp-hvf.img").string()});

                for (const auto &[name, address] : layout.loaders) {
                    if (desired_qnx_storage.empty() || name != "sysla-gpt-ramdisk-v3.img") {
                        args.insert(args.end(),
                                    {"-device", "loader,file=" + images.at(name).string() +
                                                        ",addr=" + address + ",force-raw=on"});
                    }
                }

                args.insert(args.end(), {"-device",
                                         "loader,file=" + images.at("qnx-ipl-virt.bin").string() +
                                                 ",addr=0x40200000,force-raw=on,cpu-num=0",
                                         "-serial", "file:${SESSION}/qnx-serial.log", "-S"});
                args.insert(args.end(), ram.options.begin(), ram.options.end());
                Json cluster_launch = {
                        {"schema", 1},
                        {"qemu", {{"executable", host.qnx_qemu.string()}, {"args", args}}},
                        {"graphics",
                         {{"backend", "voyemu"},
                          {"executable", current_program.string()},
                          {"angle", host.angle.string()}}},
                        {"ram_backend", ram.backend},
                        {"ram", "${SESSION}/qnx-ram.bin"},
                        {"console", "${SESSION}/qnx-console.log"},
                        {"qmp", qmp},
                        {"pins", Json::array()},
                        {"ready_ring_address", 1342242816},
                        {"ready_ring_bytes", 262144},
                        {"guest_arch", "aarch64"},
                        {"owner_local_oem_inputs", true}};
                qnx_runtime_launch_metadata(cluster_launch, layout);

                if (!desired_qnx_storage.empty()) {
                    cluster_launch["persistent_storage"] = desired_qnx_storage;
                }

                if (!desired_qnx_file_backend.empty()) {
                    cluster_launch["qnx_file_backend"] = desired_qnx_file_backend;
                }

                cluster_launch = portable_qnx_diagnostic_launch(std::move(cluster_launch),
                                                                desired_diagnostic);
                write_private_json(stage.path / "cluster-launch.json", cluster_launch);
            }
        }

        verify_relocation();
        auto record = (previous.empty() || rebind_configuration)
                              ? Json{{"schema", 1},
                                     {"kind", "runtime-configuration"},
                                     {"profile", profile},
                                     {"machine", machine.selector()},
                                     {"seed", seed.string()},
                                     {"manifest_sha256", sha256_bytes(text)},
                                     {"integrity_audited", false}}
                              : previous;

        if (rebind_configuration && !cluster && manifest.contains("qnx") &&
            ordinary_native_launch_file(scoped / "cluster-launch.json")) {
            record["cluster_rebind_pending"] = true;
        }

        if (rebind_configuration && !multimedia &&
            ordinary_native_launch_file(scoped / "android-launch.json")) {
            record["android_rebind_pending"] = true;
        }

        if (multimedia) {
            record.erase("android_rebind_pending");
        }

        if (cluster) {
            record.erase("cluster_rebind_pending");
        }

        if (foreign_host || !relocated_from.empty()) {
            relocated_activation = record;
        }

        write_private_json(stage.path / "activation.json", record);

        if (!std::filesystem::exists(scoped)) {
            rename_exclusive(stage.path, scoped);
            stage.retained = true;
        } else {
            if (initial_lifecycle) {
                need(pristine_lifecycle() == initial_locks,
                     "initial lifecycle changed during configuration");

                // Publish activation last, retaining the held CLI lock incarnation.
                for (const auto *name : {"config-template.ini", "android-launch.json",
                                         "cluster-launch.json", "activation.json"}) {
                    if (std::filesystem::exists(stage.path / name)) {
                        publish(stage.path / name, scoped / name);
                    }
                }
            } else if (!refresh_records[0].path.empty() || !refresh_records[2].path.empty()) {
                if (!refresh_records[0].path.empty()) {
                    auto launch = read_private_json(stage.path / "android-launch.json");
                    const auto template_name =
                            "config-template-api" +
                            std::to_string(manifest.at("android").at("api").get<unsigned>()) + "-" +
                            sha256_file(stage.path / "config-template.ini") + ".ini";
                    const auto template_file = scoped / template_name;

                    if (std::filesystem::exists(template_file)) {
                        need(bounded_text(template_file) ==
                                     bounded_text(stage.path / "config-template.ini"),
                             "immutable prepared template differs");
                    } else {
                        publish(stage.path / "config-template.ini", template_file);
                    }

                    launch["config_template"] = template_file.string();
                    refresh_records[0].after = launch.dump() + "\n";
                }

                const auto history = scoped / ("activation-" +
                                               sha256_bytes(refresh_records[1].before) + ".json");

                if (std::filesystem::exists(history)) {
                    need(read_private_json_bytes(history, 128 * 1024) == refresh_records[1].before,
                         "historical activation differs");
                } else {
                    write_private_json_bytes(stage.path / "activation-history.json",
                                             refresh_records[1].before);
                    publish(stage.path / "activation-history.json", history);
                }

                record["manifest_sha256"] = sha256_bytes(text);
                refresh_records[1].after = record.dump() + "\n";

                if (!refresh_records[2].path.empty()) {
                    refresh_records[2].after =
                            read_private_json_bytes(stage.path / "cluster-launch.json", 128 * 1024);
                }

                const auto read = [&](const auto &file) {
                    need(cache_file_identity(file) == refresh_identities.at(file),
                         "cached configuration changed");

                    return read_private_json_bytes(file, 128 * 1024);
                };
                const auto write = [&](const auto &file, const auto &bytes) {
                    write_private_json_bytes(file, bytes);
                    refresh_identities[file] = cache_file_identity(file);
                };
                publish_host_refresh(refresh_records, read, write, [&] {
                    verify_relocation();
                    need(read_private_json_bytes(seed / "runtime.json", 128 * 1024) == text,
                         "machine manifest changed during configuration refresh");
                });

                if (refresh_records[2].path.empty() &&
                    std::filesystem::exists(stage.path / "cluster-launch.json")) {
                    publish(stage.path / "cluster-launch.json", scoped / "cluster-launch.json");
                }
            } else {
                for (const auto *name :
                     {"config-template.ini", "android-launch.json", "cluster-launch.json"}) {
                    if (std::filesystem::exists(stage.path / name)) {
                        publish(stage.path / name, scoped / name);
                    }
                }
            }
        }

        return scoped;
    } catch (...) {
        for (auto it = created.rbegin(); it != created.rend(); ++it) {
            try {
                if (cache_file_identity(it->first) == it->second) {
                    std::filesystem::remove(it->first);
                }
            } catch (...) { /* Ignored: cleanup of owned temporary state is best-effort. */
            }
        }

        throw;
    }
}
} // namespace voyemu
