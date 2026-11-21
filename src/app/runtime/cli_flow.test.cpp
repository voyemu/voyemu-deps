#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#endif
#include "android_engine.hpp"
#include "android_port_lease.hpp"
#include "machine_path.hpp"
#include "passion_presentation.hpp"
#include "profile_catalog.hpp"
#include "profile_runtime.hpp"
#include "qnx_host.hpp"
#include "qnx_process_ram.hpp"
#include "qnx_runtime_layout.hpp"
#include "session.hpp"
#include "start_host_pins.hpp"
#include "supervisor_failure.hpp"
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include "os/process.hpp"
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace voyemu;

// The bundled Android 9 profile on the 8155 board, read from the catalog.
static const std::string &qinggan09_profile() {
    static const auto id =
            embedded_profile_names(ProfilePlatform::qinggan09, ProfileBoard::sa8155).at(0);

    return id;
}

static void require(bool result, const std::string &message) {
    if (!result) {
        throw std::runtime_error(message);
    }
}

// A failed ordinary command shows the code of an uncatalogued error and names its log, which keeps
// the English text: whether output and that log carry "CODE: text".
static bool logged_error(const std::string &output, const std::string &error) {
    std::smatch match;

    if (!std::regex_search(output, match,
                           std::regex(R"((?:Log|Details are in the log): ([^\n]+?\.log))")) ||
        output.find(error.substr(0, error.find(':') + 1)) == std::string::npos) {
        return false;
    }

    std::ifstream stream(match[1].str());

    return std::string(std::istreambuf_iterator<char>(stream), {}).find(error) != std::string::npos;
}

#ifdef _WIN32
// Test-only channel separation: structured stderr diagnostics are never response authority.
static ProcessResult fixture_public_cli(const std::filesystem::path &executable,
                                        const std::vector<std::string> &arguments,
                                        const std::filesystem::path &root) {
    struct Handle {
        HANDLE value = INVALID_HANDLE_VALUE;

        ~Handle() {
            if (value && value != INVALID_HANDLE_VALUE) {
                CloseHandle(value);
            }
        }
    };
    const auto slot = root / ".local/build" / ("split-cli-" + random_token());

    ensure_private_directory(slot);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle output{CreateFileW((slot / "stdout.json").c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, &security, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr)};
    Handle errors{CreateFileW((slot / "stderr.log").c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, &security, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr)};
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(output.value != INVALID_HANDLE_VALUE && errors.value != INVALID_HANDLE_VALUE &&
                    input.value != INVALID_HANDLE_VALUE,
            "private CLI channels unavailable");
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(job.value && SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                                 &limits, sizeof(limits)),
            "private CLI job unavailable");
    std::string command = windows_quote(executable.string());

    for (const auto &argument : arguments) {
        command += " " + windows_quote(argument);
    }

    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.data(),
                                            static_cast<int>(command.size()), nullptr, 0);
    require(length > 0, "private CLI command encoding invalid");
    std::wstring wide(length, L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.data(),
                                static_cast<int>(command.size()), wide.data(), length) == length,
            "private CLI command encoding changed");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.value;
    startup.hStdOutput = output.value;
    startup.hStdError = errors.value;
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(), wide.data(), nullptr, nullptr, TRUE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, root.c_str(), &startup,
                           &process),
            "private CLI spawn failed");
    Handle child{process.hProcess}, thread{process.hThread};

    if (!AssignProcessToJobObject(job.value, child.value)) {
        TerminateProcess(child.value, 126);
        require(WaitForSingleObject(child.value, 3000) == WAIT_OBJECT_0,
                "unassigned own CLI cleanup unverified");
        throw std::runtime_error("private CLI job assignment failed");
    }

    require(ResumeThread(thread.value) != DWORD(-1), "private CLI resume failed");
    const auto waited = WaitForSingleObject(child.value, 30000);
    const bool timeout = waited == WAIT_TIMEOUT;

    if (waited != WAIT_OBJECT_0) {
        require(TerminateJobObject(job.value, 124) &&
                        WaitForSingleObject(child.value, 3000) == WAIT_OBJECT_0,
                "private CLI timeout cleanup unverified");
        require(timeout, "private CLI wait failed");
    }

    DWORD code = 0;
    require(GetExitCodeProcess(child.value, &code) && code != STILL_ACTIVE,
            "private CLI terminal state unavailable");
    CloseHandle(output.value);
    output.value = INVALID_HANDLE_VALUE;
    CloseHandle(errors.value);
    errors.value = INVALID_HANDLE_VALUE;

    if (timeout) {
        return {static_cast<int>(code), {}, true};
    }

    return {static_cast<int>(code), read_private_json(slot / "stdout.json", 512 * 1024).dump(),
            false};
}
#endif

static std::unique_ptr<Session> attach_fixture_presentation(const std::filesystem::path &root,
                                                            bool attach_frontend, bool background) {
    const auto launch_directory = native_machine_directory(
            root, qinggan09_profile(), voyemu::machine_identity(qinggan09_profile(), {}));
    const auto id = random_token();
    auto session = std::make_unique<Session>(launch_directory / "sessions" / id, id);
    auto specification = read_private_json(launch_directory / "android-launch.json");

    specification["env"]["VOYEMU_NATIVE_FIXTURE_ROOT"] = root.string();
    session->attach_android(specification, true);
    const auto cluster_file = launch_directory / "cluster-launch.json";
    const bool prepared = std::filesystem::exists(cluster_file);

    if (prepared) {
        session->configure_cluster(read_private_json(cluster_file));
    }

    if (attach_frontend) {
        session->attach_frontend(specification.at("frontend"), prepared, background);
    }

    return session;
}

static int fixture_presentation_probe(const std::filesystem::path &root,
                                      std::string_view mode) try {
    require(mode == "foreground" || mode == "background", "unknown presentation probe mode");
    auto session = attach_fixture_presentation(root, true, mode == "background");
    auto receipt = [&](const char *name) {
        const auto path = root / name;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        while (!std::filesystem::exists(path) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return read_private_json(path);
    };
    const auto arguments = receipt("frontend-arguments.json");
    const auto sdk = receipt("emulator-arguments.json");
    // Keep ticking until the retained raw transport has served this launch.
    // An immediate stop can otherwise spend its deadline waiting for first identity.
    const auto route = root / "adbd-routing.json";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    do {
        session->tick();

        if (std::filesystem::exists(route)) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);

    require(std::filesystem::exists(route),
            "presentation fixture did not serve its owned ADB transport");
    const auto status = session->handle({{"op", "status"}});
    session->stop();
    write_private_json(root / "presentation-probe.json",
                       {{"frontend", status.at("deliveries").at("frontend")},
                        {"arguments", arguments},
                        {"sdk", sdk}});

    return 0;
} catch (const std::exception &error) {
    std::cerr << "PRESENTATION_FIXTURE_ERROR: " << error.what() << '\n';

    return 1;
}

int main(int argc, char **argv) try {
#ifndef _WIN32
    if (argc == 4 && std::string_view(argv[1]) == "--session-resource-holder") {
        write_private_json(argv[3], {{"ready", true}});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

        while (!std::filesystem::exists(argv[2]) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        require(std::filesystem::exists(argv[2]), "resource descendant gate timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        return 0;
    }
#endif

    if (argc == 4 && std::string_view(argv[1]) == "--presentation-probe") {
        return fixture_presentation_probe(argv[2], argv[3]);
    }

    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: cli_flow_test VOYEMU RUNTIME_FIXTURE\n";

        return 2;
    }

    const bool unowned = argc == 4 && std::string(argv[3]) == "--quickboot-unowned";
    const std::string gpu_mode = argc == 4 && std::string(argv[3]) == "--gpu-host" ? "host"
                                 : argc == 4 && std::string(argv[3]) == "--gpu-auto-no-window"
                                         ? "auto-no-window"
                                         : "";
    const bool clock_flow = argc == 4 && std::string(argv[3]) == "--clock-readback";
    const bool control_latency = argc == 4 && std::string(argv[3]) == "--control-latency";
    const bool state_flow = argc == 4 && std::string(argv[3]) == "--state-latency";
    const bool state_restore = argc == 4 && std::string(argv[3]) == "--state-restore";
    const bool boot_trace = argc == 4 && std::string(argv[3]) == "--boot-trace";
    std::optional<std::string> boot_trace_value;
    const bool boot_observation = argc == 4 && std::string(argv[3]) == "--boot-observation";
    const bool slow_startup = argc == 4 && std::string(argv[3]) == "--slow-windows-startup";
    const bool root_restart = argc == 4 && (std::string(argv[3]) == "--root-restart-recovery" ||
                                            std::string(argv[3]) == "--root-restart-unverified");
    const bool root_unverified = root_restart &&
                                 std::string(argv[3]) == "--root-restart-unverified";
    [[maybe_unused]] const bool orphan_recovery = argc == 4 &&
                                                  std::string(argv[3]) == "--orphan-recovery";
    const bool clock_service = argc == 4 && std::string(argv[3]) == "--clock-service";
    const bool clock_observation = argc == 4 && std::string(argv[3]) == "--clock-observation";
    const bool canonical_api = argc == 4 && std::string(argv[3]) == "--canonical-api";
    const bool api_first_bind = argc == 4 && std::string(argv[3]) == "--api-first-bind";
    const bool check_portable = argc == 4 && std::string(argv[3]) == "--check-portable";
    const bool arguments_only = argc == 4 && std::string(argv[3]) == "--arguments-only";
    const bool timings = argc == 4 && std::string(argv[3]) == "--adb-timings";
    [[maybe_unused]] const bool identity_backpressure =
            argc == 4 && std::string(argv[3]) == "--identity-backpressure";
    const bool owned_adb_death = argc == 4 && std::string(argv[3]) == "--owned-adb-death";
    const bool latency = argc == 4 && (std::string(argv[3]) == "--quickboot-latency" || timings);
    const bool runtime_paths = argc == 4 && std::string(argv[3]) == "--runtime-paths";
    const bool reboot_preflight = argc == 4 && std::string(argv[3]) == "--android-reboot-preflight";
    const bool cluster_lifecycle = argc == 4 && std::string(argv[3]) == "--cluster-lifecycle";
    const bool frontend_lifecycle = argc == 4 && std::string(argv[3]) == "--frontend-lifecycle";
    const bool startup_cancel = argc == 4 && std::string(argv[3]) == "--startup-cancel";
    const bool frontend_presentation = argc == 4 &&
                                       std::string(argv[3]) == "--frontend-presentation";
    // API2 privilege bootstrap requires a retained SDK process on every host.
    // The explicit unowned restore branch below still models a detached launcher.
    const bool quickboot = true;
    auto cli = std::filesystem::canonical(argv[1]), fixture = std::filesystem::canonical(argv[2]);
    auto project = cli.parent_path();

    while (!project.empty() && !std::filesystem::is_directory(project / "src/app")) {
        if (project == project.parent_path()) {
            break;
        }

        project = project.parent_path();
    }

    require(std::filesystem::is_directory(project / "src/app"), "fixture project root unavailable");
    // Separate components keep the Windows spelling the CLI derives from its own module path.
    auto fixture_base = project / ".local" / "build";

    if (const auto *scratch = std::getenv("VOYEMU_NATIVE_FIXTURE_BASE")) {
        fixture_base = std::filesystem::path(scratch);
        require(fixture_base.is_absolute() && fixture_base == fixture_base.lexically_normal() &&
                        std::filesystem::is_directory(fixture_base),
                "explicit ordinary fixture scratch required");
    }

    const auto root = fixture_base / ("voyemu-native-cli-fixture-" + random_token());
    ensure_private_directory(root / ".local/machines");
#ifdef _WIN32
    const auto installed_cli = root / "voyemu.exe";
#else
    const auto installed_cli = root / "voyemu";
#endif
    std::filesystem::copy_file(cli, installed_cli);
    cli = installed_cli;
    const auto selected_machine = root / "machines" / qinggan09_profile();
    ensure_private_directory(selected_machine);
    Json fixture_manifest = {
            {"schema", 1}, {"kind", "portable-guest-runtime"}, {"profile", qinggan09_profile()}};

    if (control_latency || cluster_lifecycle) {
        Json qnx = {{"guest_arch", "aarch64"}, {"images", Json::object()}};

        for (const auto &name : qnx_runtime_layout("sa8155-qinggan09").images) {
            qnx["images"][name] = {
                    {"file", "qnx/" + name}, {"bytes", 1}, {"sha256", std::string(64, '0')}};
        }

        qnx_portable_layout(qinggan09_profile(), qnx);
        auto missing = qnx;
        missing["images"].erase("smem.bin");
        bool rejected = false;
        try {
            qnx_portable_layout(qinggan09_profile(), missing);
        } catch (const std::exception &) {
            rejected = true;
        }

        require(rejected, "portable fixture accepted a missing QNX image role");
        auto foreign = qnx;
        foreign["images"]["smem.bin"]["file"] = "qnx/foreign.bin";
        rejected = false;
        try {
            qnx_portable_layout(qinggan09_profile(), foreign);
        } catch (const std::exception &) {
            rejected = true;
        }

        require(rejected, "portable fixture accepted a foreign QNX image path");
        fixture_manifest["qnx"] = qnx;
    }

    write_private_json(selected_machine / "runtime.json", fixture_manifest);

    if (quickboot) {
        std::ofstream(root / "direct-engine") << "retained owned engine fixture";
    }

#ifdef __APPLE__
    const auto linked_bin = root / "linked-bin";

    if (argc == 3) {
        ensure_private_directory(linked_bin);
        std::filesystem::create_symlink(cli, linked_bin / "voyemu");
    }
#endif
    auto run = [&](std::vector<std::string> args, int timeout = 30000) {
        const auto command = args.front();
        const bool internal = command == "_start" || command == "test";

        if (!frontend_presentation && !cluster_lifecycle &&
            (command == "start" || command == "_start" || command == "restart") &&
            std::find(args.begin(), args.end(), "--background") == args.end()) {
            args.push_back("--background");
        }

        if (internal) {
            // The internal start has no default profile; the fixture machine runs the catalog one.
            if (command == "_start" &&
                std::find(args.begin(), args.end(), "--profile") == args.end()) {
                args.insert(args.end(), {"--profile", qinggan09_profile()});
            }

            args.insert(args.end(), {"--root", root.string()});
        } else {
            // Human text assertions read English regardless of the host locale.
            args.emplace_back("--lang=en");

            if (command == "start" || command == "restart" || command == "status" ||
                command == "stop" || command == "check" || command == "patch") {
                if (args.size() < 2 || args[1].starts_with("-")) {
                    args.insert(args.begin() + 1, selected_machine.string());
                }
            }

            if (command == "fetch" || command == "build") {
                args.insert(args.begin() + 1, qinggan09_profile());
            }

            for (auto &argument : args) {
                if (argument == "--no-cluster") {
                    argument = "--cluster=off";
                } else if (argument == "--with-cluster") {
                    argument = "--cluster=on";
                }
            }
        }

        ProcessSpec spec;
        spec.executable = cli;
        spec.arguments = std::move(args);
#ifdef __APPLE__
        // Homebrew invokes the CLI through PATH from outside its installation directory.
        if (argc == 3) {
            spec.executable = "/bin/sh";
            spec.arguments.insert(spec.arguments.begin(), {"-c", "exec voyemu \"$@\"", "voyemu"});
            spec.environment["PATH"] = linked_bin.string() + ":/usr/bin:/bin";
            spec.directory = root;
        }
#endif
        spec.environment["ADB_TRACE"] = "transport,packets,sockets";

        if (boot_trace_value) {
            spec.environment["VOYEMU_ANDROID_BOOT_TRACE"] = *boot_trace_value;
        }

        spec.environment["VOYEMU_NATIVE_FIXTURE_ROOT"] = root.string();
        // Latency flows keep control pipe accept and lock-wait records in retained diagnostics.
        spec.environment["VOYEMU_LOG_LEVEL"] = (timings || boot_observation || control_latency ||
                                                state_flow || slow_startup || clock_observation ||
                                                root_restart)
                                                       ? "debug"
                                                       : "info";

        return execute(spec, timeout);
    };
    auto json = [&](std::vector<std::string> args) {
        const auto command = args.front();
        const int budget = command == "restart" ? 180000 : command == "stop" ? 90000 : 30000;
        Json prior;

        if (command == "stop") {
            prior = read_private_json(
                    native_current_active_directory(
                            root, qinggan09_profile(),
                            voyemu::machine_identity(qinggan09_profile(), std::string_view{})) /
                    (std::string(qinggan09_profile()) + ".json"));
        }

        if (command == "start") {
            args.front() = "_start";
        }

        auto result = run(std::move(args), budget);
        require(!result.timed_out && result.exit_code == 0, "CLI failed: " + result.output);

        if (command == "start") {
            return Json::parse(result.output);
        }

        require(Json::parse(result.output, nullptr, false).is_discarded(),
                "public CLI emitted JSON");

        if (command == "stop") {
            auto terminal =
                    read_private_json(root / ".local/machines" / qinggan09_profile() / "sessions" /
                                      prior.at("session_id").get<std::string>() / "runtime.json");
            terminal["ok"] = true;

            return terminal;
        }

        const auto active = read_private_json(
                native_current_active_directory(
                        root, qinggan09_profile(),
                        voyemu::machine_identity(qinggan09_profile(), std::string_view{})) /
                (std::string(qinggan09_profile()) + ".json"));

        return request(active.at("endpoint").get<std::string>(),
                       {{"op", "status"}, {"session_id", active.at("session_id")}}, 1000);
    };
    try {
#ifdef __APPLE__
        if (argc == 3) {
            const auto portable = root / "portable/voyemu", home = root / "user-home";
            ensure_private_directory(portable.parent_path());
            std::filesystem::copy_file(cli, portable);
            ProcessSpec probe;
            probe.executable = portable;
            probe.arguments = {"check", selected_machine.string()};
            probe.directory = root;
            probe.environment["HOME"] = home.string();
            auto clean = execute(probe);
            require(clean.exit_code != 0 && clean.output.starts_with("Voyemu:"),
                    "standalone check must report an unprepared user profile");

            require(!std::filesystem::exists(home), "read-only standalone check created user data");
            probe.environment["HOME"] = "relative-home";
            auto invalid_home = execute(probe);
            require(invalid_home.exit_code != 0 &&
                            invalid_home.output.find("USER_HOME") == std::string::npos,
                    "valid project ancestry must take precedence over HOME");
            probe.arguments = {"check", selected_machine.string(), "--root", root.string(),
                               "--lang=en"};
            auto explicit_root = execute(probe);
            require(explicit_root.exit_code != 0 &&
                            explicit_root.output.find("--root is internal") != std::string::npos,
                    "public check accepted internal root override");
        }
#endif
        auto profile = root / "profiles" / qinggan09_profile();
        ensure_private_directory(profile);
        ensure_private_directory(root / "profiles/common/wheels/free-2021");
        write_private_json(
                profile / "profile.json",
                {{"schema", 1},
                 {"title", embedded_profile(qinggan09_profile()).at("definition").at("title")},
                 {"platform",
                  embedded_profile(qinggan09_profile()).at("definition").at("platform")},
                 {"board", embedded_profile(qinggan09_profile()).at("definition").at("board")},
                 {"initial_state", embedded_profile(qinggan09_profile())
                                           .at("definition")
                                           .value("initial_state", Json::object())},
                 {"displays",
                  embedded_profile(qinggan09_profile()).at("definition").at("displays")},
                 {"wheel", {{"assets", "profiles/common/wheels/free-2021"}}}});
        auto help = run({"help"});
        require(help.exit_code == 0 && help.output.find("--cluster=on|off") != std::string::npos &&
                        help.output.find("Run and control") != std::string::npos,
                "complete grouped help omitted launch options");
        auto bundled_profiles = embedded_profile_names();
        std::set<std::string> expected_profiles(bundled_profiles.begin(), bundled_profiles.end());
        expected_profiles.insert(qinggan09_profile());
        std::string expected_listing = "Profiles\n";

        for (const auto &name : expected_profiles) {
            expected_listing += "  " + name + "\n";
        }

        auto listed = run({"list"});
        std::string listed_lines;

        for (std::size_t i = 0; i < listed.output.size(); ++i) {
            if (listed.output[i] == '\r' && i + 1 < listed.output.size() &&
                listed.output[i + 1] == '\n') {
                continue;
            }

            listed_lines += listed.output[i];
        }

        require(listed.exit_code == 0 && listed_lines == expected_listing,
                "list lost bundled or local profiles");
        auto checked = run({"check"});
        require(checked.exit_code != 0 && checked.output.starts_with("Voyemu:"),
                "unprepared check changed runtime");
        require(std::filesystem::is_empty(root / ".local/machines") &&
                        !std::filesystem::exists(root / ".local/profiles") &&
                        !std::filesystem::exists(root / ".local/toolchains"),
                "read-only help/list/check created runtime or preparation state");
        auto misplaced_jdk = run({"check", "--jdk", (root / "missing-jdk").string()});
        require(misplaced_jdk.exit_code != 0 &&
                        misplaced_jdk.output.find("--jdk is only valid for deps/build") !=
                                std::string::npos,
                "JDK option accepted outside preparation commands");

        if (arguments_only) {
            const auto root_program = (root / "voyemu").string();
            const Json root_pin{{"path", root_program}, {"sha256", std::string(64, 'a')}};
            const std::vector<Json> pin_launches{
                    {{"pins", Json::array({root_pin,
                                           {{"path", "/absent-payload"},
                                            {"sha256", std::string(64, '0')}}})}}};
            Json legacy_host{{"graphics", root_program}, {"pins", Json::array({root_pin})}};
            verify_start_root_binding(root / "voyemu", pin_launches, legacy_host);
            legacy_host["pins"][0]["sha256"] = std::string(64, 'b');
            bool legacy_rejected = false;
            try {
                verify_start_root_binding(root / "voyemu", pin_launches, legacy_host);
            } catch (const std::exception &e) {
                legacy_rejected = std::string(e.what()).starts_with("HOST_ROOT_LEGACY_BINDING:");
            }

            require(legacy_rejected,
                    "legacy root mismatch did not fail before touching absent payload");
            require(supervisor_failure_code(
                            "private arguments\n{\"ok\":false,\"error\":\"LAUNCH_PIN: immutable input mismatch: /private/file\"}\n") ==
                            "LAUNCH_PIN",
                    "supervisor structured cause hidden");
            require(supervisor_failure_code("{\"ok\":false,\"error\":\"secret payload\"}\n")
                            .empty(),
                    "unstructured private supervisor error exposed");
            require(help.output.find("start MACHINE_PATH") != std::string::npos,
                    "help omits ordinary machine path start");
            const auto external = root / "outside machines" / "arbitrary-seed";
            ensure_private_directory(external);
            write_private_json(external / "runtime.json", {{"schema", 1},
                                                           {"kind", "portable-guest-runtime"},
                                                           {"profile", qinggan09_profile()}});
            const auto selection = inspect_machine_path(external);
            require(selection.profile == qinggan09_profile() && selection.seed == external &&
                            machine_path_name(root, selection) == "arbitrary-seed",
                    "external machine selection lost identity");
            require(machine_path_name(root, {root / "machines" / qinggan09_profile(),
                                             qinggan09_profile()}) == qinggan09_profile(),
                    "canonical seed must retain its descriptive machine name");

            for (const auto &options : std::vector<std::vector<std::string>>{
                         {"start", external.string(), external.string()},
                         {"start", external.string(), "--machine", "other"},
                         {"start", external.string(), "--profile", "other"},
                         {"start", (root / "absent-machine").string()}}) {
                auto denied = run(options);
                require(denied.exit_code != 0 &&
                                (denied.output.find("MACHINE_PATH") != std::string::npos ||
                                 denied.output.find("CLI_ARGUMENT") != std::string::npos),
                        "invalid positional machine selection accepted");
            }

            const auto linked = root / "linked-seed";
            std::filesystem::create_directory_symlink(external, linked);
            bool denied = false;
            try {
                inspect_machine_path(linked);
            } catch (...) {
                denied = true;
            }

            require(denied, "linked machine directory accepted");
            std::filesystem::remove(linked);
            std::filesystem::remove_all(root / "outside machines");
            require(help.output.find("fetch PROFILE --source=FILE") != std::string::npos &&
                            help.output.find("fetch PROFILE --source=adb --serial SERIAL") !=
                                    std::string::npos &&
                            help.output.find("--update") == std::string::npos &&
                            help.output.find("build-finalize") == std::string::npos,
                    "complete help differs from fetch/build options");
            require(run({"--help"}).output == help.output, "--help differs from complete help");
            const auto archive = (root / "missing-update.zip").string();
            const auto source = "--source=" + archive;

            for (const auto &options : std::vector<std::vector<std::string>>{
                         {"check", source},
                         {"fetch", source, "--serial", "physical-fixture"},
                         {"fetch", source, source},
                         {"fetch", "--source=adb"},
                         {"fetch", "--serial", "physical-fixture"},
                         {"fetch", "--update", archive},
                 }) {
                auto denied = run(options);
                require(denied.exit_code != 0 &&
                                denied.output.find("CLI_ARGUMENT") != std::string::npos,
                        "fetch arguments accepted outside their declared scope");
            }

            for (const auto &options : std::vector<std::vector<std::string>>{
                         {"fetch", "--source=adb", "--serial", "emulator-5554"},
                         {"fetch", "--source=adb", "--serial", "invalid serial"},
                         {"fetch", "--offline", "--source=adb", "--serial", "physical-fixture"},
                 }) {
                auto denied = run(options);
                require(denied.exit_code != 0 &&
                                denied.output.find("FETCH_SERIAL") != std::string::npos,
                        "ADB fetch accepted an invalid or offline device");
            }

            auto accepted = run({"fetch", "--offline", source});
            require(accepted.exit_code != 0 &&
                            accepted.output.find("PIN_PATH") != std::string::npos,
                    "local archive source did not reach owner profile validation");
            auto incomplete = read_private_json(profile / "profile.json");
            incomplete["status"] = "incomplete";
            write_private_json(profile / "profile.json", incomplete);
            write_private_json(profile / "capture.json",
                               {{"oversized", std::string(128 * 1024, 'x')}}, 256 * 1024);
            const auto device = run({"fetch", "--source=adb", "--serial", "physical-fixture"});
            require(device.exit_code != 0 &&
                            device.output.find("VEHICLE_CAPTURE_PROFILE") != std::string::npos,
                    "explicit physical serial did not reach capture profile validation");
            // fetch recreates the empty .local roots but imports nothing.
            require(std::filesystem::is_empty(root / ".local/profiles") &&
                            !std::filesystem::exists(root / ".local/machines" /
                                                     qinggan09_profile() / "sessions"),
                    "argument validation changed inputs or started a session");
            std::filesystem::remove_all(root);
            std::cout
                    << "CLI fetch source argument scope passed; no guest or dependency operation\n";

            return 0;
        }

        auto build_parse = run({"build", "--offline", "--jdk", (root / "missing-jdk").string()});
        require(build_parse.exit_code != 0 &&
                        build_parse.output.find("CLI_ARGUMENT") == std::string::npos,
                "offline build/JDK option rejected before source profile validation");
        require(!std::filesystem::exists(root / ".local/machines" / qinggan09_profile() /
                                         "sessions"),
                "invalid source build launched a session");
        auto invalid = run({"check", "--profile", "../outside"});
        require(invalid.exit_code != 0 && invalid.output.find("CLI_ARGUMENT") != std::string::npos,
                "CLI accepted traversal");
        auto machine = root / "machines" / qinggan09_profile();
        ensure_private_directory(machine);
        write_private_json(machine / "machine.json", {{"schema", 1}});
        {
            std::ofstream(machine / "config.ini", std::ios::binary)
                    << "AvdId=VoyemuFixture\nhw.ramSize=1\nhw.sdCard=no\n";
        }

        for (const char *name :
             {"userdata-qemu.img", "cache.img", "encryptionkey.img", "userdata-qemu.img.qcow2",
              "cache.img.qcow2", "encryptionkey.img.qcow2", "system.img.qcow2",
              "vendor.img.qcow2"}) {
            std::ofstream(machine / name) << name;
        }

        for (const char *name : {"system.img", "vendor.img"}) {
            std::ofstream(machine / name) << name;
        }

        { std::ofstream(machine / "build.prop") << "ro.build.version.incremental=9767327\n"; }
        const auto prepared_config_sha256 = sha256_file(machine / "config.ini");
        auto config_template = machine / "config-template.ini";
        std::filesystem::copy_file(machine / "config.ini", config_template);
        std::map<std::filesystem::path, std::string> disk_hashes;

        for (const char *name :
             {"userdata-qemu.img", "cache.img", "encryptionkey.img", "userdata-qemu.img.qcow2",
              "cache.img.qcow2", "encryptionkey.img.qcow2", "system.img.qcow2",
              "vendor.img.qcow2"}) {
            disk_hashes[machine / name] = sha256_file(machine / name);
        }

        for (const char *name : {"system.img", "vendor.img"}) {
            disk_hashes[machine / name] = sha256_file(machine / name);
        }

        auto require_disks = [&] {
            for (const auto &[path, hash] : disk_hashes) {
                require(sha256_file(path) == hash, "config persistence changed disk bytes");
            }
        };
        auto bin = root / "fixture-bin";
        ensure_private_directory(bin);
#ifdef _WIN32
        auto emulator = bin / "emulator.exe";
#else
        auto emulator = bin / "emulator";
#endif
        std::filesystem::copy_file(fixture, emulator);
        write_private_json(qnx_host_bindings_path(root),
                           {{"runtime_sdk", {{"sdk_root", root.string()}}}});
        std::filesystem::permissions(emulator, std::filesystem::perms::owner_all);
        unsigned port = 5660;

        while (port <= 5682 && (!android_port_leasable(port) || !tcp_port_free(port) ||
                                !tcp_port_free(port + 1) || !tcp_port_free(port + 30000))) {
            port += 2;
        }

        require(port <= 5682, "no isolated fixture console port available");
        auto configuration = root / ".local/machines";
        ensure_private_directory(configuration);
        const auto launch_directory = configuration / qinggan09_profile();
        ensure_private_directory(launch_directory);
        const auto write_cluster_fixture = [&](Json specification) {
            // These synthetic QMP-less children never announce a graphics producer.
            // Keep their prepared host-cache identity current without launching ANGLE.
            const auto angle = root / "fixture-angle";
            ensure_private_directory(angle);
            // Every host keeps the guest RAM in the session file, as a prepared launch records.
            specification["ram_backend"] = "file";
            specification["qemu"]["env"]["VOYEMU_FIXTURE_SESSION"] = "${SESSION}";

            for (const auto &option : qnx_ram_launch(host_qnx_engine).options) {
                specification["qemu"]["args"].push_back(option);
            }

#ifdef _WIN32
            const std::array<const char *, 2> libraries{"libEGL.dll", "libGLESv2.dll"};
#else
            const std::array<const char *, 2> libraries{"libEGL.dylib", "libGLESv2.dylib"};
#endif
            specification["graphics"] = {
                    {"executable", cli.string()}, {"backend", "voyemu"}, {"angle", angle.string()}};
            specification["pins"].push_back({{"path", cli.string()}, {"sha256", sha256_file(cli)}});

            for (const auto *name : libraries) {
                const auto file = angle / name;
                {
                    std::ofstream out(file);
                    out << "synthetic unused ANGLE fixture";
                }
                specification["pins"].push_back(
                        {{"path", file.string()}, {"sha256", sha256_file(file)}});
            }

            write_private_json(launch_directory / "cluster-launch.json", specification);
        };
        write_private_json(launch_directory / "android-launch.json",
                           {{"schema", 1},
                            {"executable", emulator.string()},
                            {"adb_transport", "direct"},
                            {"avd_name", "VoyemuFixture"},
                            {"machine", machine.string()},
                            {"base", machine.string()},
                            {"port", port},
                            {"android_sdk_root", root.string()},
                            {"config_template", config_template.string()},
                            {"pins", Json::array({{{"path", emulator.string()},
                                                   {"sha256", sha256_file(emulator)}},
                                                  {{"path", config_template.string()},
                                                   {"sha256", prepared_config_sha256}}})}});
        const auto other_machine = root / "machines/another-car";
        ensure_private_directory(other_machine);
        write_private_json(
                other_machine / "runtime.json",
                {{"schema", 1}, {"kind", "portable-guest-runtime"}, {"profile", "another-car"}});
        auto other_check = run({"check", other_machine.string()});
        require(other_check.exit_code != 0,
                "unprepared profile inherited the first machine launch");
        auto other_start = run({"_start", "--profile", "another-car", "--no-cluster"});
        require(other_start.exit_code != 0,
                "unprepared profile started the wrong machine: " + other_start.output);
        require(!std::filesystem::exists(configuration / "another-car/sessions"),
                "rejected profile created a session");
#ifndef _WIN32
        const auto linked_configuration = configuration / qinggan09_profile();
        std::filesystem::rename(linked_configuration, configuration / "fixture-launch-pending");
        std::filesystem::create_directory_symlink(root, linked_configuration);
        auto status_without_launch = run({"status"});
        require(status_without_launch.output.find("PROFILE_LAUNCH") == std::string::npos,
                "status must remain available when launch configuration is corrupt");
        std::filesystem::remove(linked_configuration);
        std::filesystem::rename(configuration / "fixture-launch-pending", linked_configuration);
#endif
        write_private_json(selected_machine / "profile.json",
                           read_private_json(profile / "profile.json"));
        fixture_manifest["profile_sha256"] = sha256_file(selected_machine / "profile.json");
        ensure_private_directory(selected_machine / "android");
        const auto system_fixture = selected_machine / "android/system.img";
        std::filesystem::copy_file(machine / "system.img", system_fixture);
        fixture_manifest["android"] = {{"api", 28},
                                       {"images",
                                        {{"system.img",
                                          {{"file", "android/system.img"},
                                           {"bytes", std::filesystem::file_size(system_fixture)},
                                           {"sha256", sha256_file(system_fixture)}}}}}};
        Json resources = Json::object();

        for (const auto *name :
             {"ui/splash.png", "ui/wheel-routes.json", "ui/wheel/wheel-hotspots.json",
              "ui/wheel/wheel-left.png", "ui/wheel/wheel-right.png"}) {
            const auto file = selected_machine / name;
            ensure_private_directory(file.parent_path());
            const std::string bytes = std::string(name) == "ui/wheel-routes.json"
                                              ? "{\"routes\":[{\"id\":\"menu\",\"qg_code\":3074}]}"
                                      : std::string(name) == "ui/wheel/wheel-hotspots.json"
                                              ? "{}"
                                              : "owned fixture UI";

            if (std::string_view(name) == "ui/splash.png") {
                // Owned 1920x720 one-bit grayscale PNG; native splash geometry is part of the
                // contract.
                static constexpr char splash[] =
                        "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x07\x80\x00\x00\x02\xd0\x01\x00\x00\x00\x00\x59\x6f\x50"
                        "\x3b\x00\x00\x00\xbf\x49\x44\x41\x54\x78\x9c\xed\xc1\x31\x01\x00\x00\x00\xc2\xa0\xf5\x4f\x6d\x0d\x0f\xa0\x00\x00\x00\x00\x00\x00"
                        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        "\x00\x00\xbe\x0c\xa5\xee\x00\x01\x33\xeb\x16\xe8\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82";
                std::ofstream out(file, std::ios::binary);
                out.write(splash, sizeof(splash) - 1);
            } else if (std::string_view(name).starts_with("ui/wheel/")) {
                std::filesystem::copy_file(
                        project / "profiles/common/wheels/free-2021" / file.filename(), file);
            } else {
                std::ofstream out(file, std::ios::binary);
                out << bytes;
            }

            resources[name] = {{"file", name},
                               {"bytes", std::filesystem::file_size(file)},
                               {"sha256", sha256_file(file)}};
        }

        fixture_manifest["resources"] = resources;
        write_private_json(selected_machine / "runtime.json", fixture_manifest);
        auto launch = read_private_json(launch_directory / "android-launch.json");
        launch["sdk_control"] = {{"grpc_port", port + 30000},
                                 {"discovery_root", (root / "fixture-sdk-discovery").string()}};
        launch["frontend"] = {
                {"schema", 1},
                {"pins", Json::array({{{"path", cli.string()}, {"sha256", sha256_file(cli)}}})},
                {"executable", cli.string()},
                {"source_root", root.string()},
                {"grpc_port", port + 30000},
                {"discovery_root", (root / "fixture-sdk-discovery").string()},
                {"state", (root / "placement.json").string()},
                {"assets", (selected_machine / "ui/wheel").string()},
                {"fonts", (selected_machine / "ui/fonts").string()},
                {"splash", (selected_machine / "ui/splash.png").string()}};

        if (!gpu_mode.empty()) {
            launch["gpuMode"] = gpu_mode;
        }

        // A prepared launch carries the screens of its profile for the window.
        launch["screens"] = read_private_json(profile / "profile.json").at("displays");
        launch["vehicle_adapter"] = {{"authority", "com.voyemu.output.vehicle"}};
        launch["wheel_adapter"] = {{"authority", "com.voyemu.input.wheel"},
                                   {"routes", {{"menu", 3074}}}};

        if (latency || slow_startup || clock_flow) {
            std::ofstream(root / "canvas-fixture") << "host canvas observation fixture";
            launch["env"]["DYLD_INSERT_LIBRARIES"] = "";
            launch["home"] = {{"package", "com.qinggan.app.launcher"}};
            launch["features"] = {{"MultiDisplay", "on"}};
            launch["systemui_adapter"] = {{"component", "com.qinggan.systemui/.SystemUIService"}};
        }

#ifdef _WIN32
        if (slow_startup) {
            launch["sdk_control"] = {{"grpc_port", port + 30000},
                                     {"discovery_root", (root / "fixture-sdk-discovery").string()}};
            launch["args"] = Json::array({"-grpc", std::to_string(port + 30000)});
        }
#endif
        write_private_json(launch_directory / "android-launch.json", launch);

        if (!api_first_bind) {
            write_private_json(launch_directory / "activation.json",
                               {{"schema", 1},
                                {"kind", "runtime-configuration"},
                                {"profile", qinggan09_profile()},
                                {"machine", qinggan09_profile()},
                                {"seed", selected_machine.string()},
                                {"manifest_sha256", sha256_file(selected_machine / "runtime.json")},
                                {"integrity_audited", false}});
        }

#ifndef _WIN32
        if (reboot_preflight) {
            require(::setenv("VOYEMU_NATIVE_FIXTURE_ROOT", root.c_str(), 1) == 0,
                    "fixture environment unavailable");
            const auto token = random_token();
            const auto directory = configuration / qinggan09_profile() / "sessions" / token;
            auto specification = launch;
            specification["env"]["VOYEMU_NATIVE_FIXTURE_ROOT"] = root.string();
            Session session(directory, token);
            session.attach_android(specification, true);
            const auto ownership = random_token();
            write_private_json(directory / "automation-owner.json",
                               {{"session_id", token}, {"ownership_id", ownership}});
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);

            while ((tcp_port_free(port) || tcp_port_free(port + 1)) &&
                   std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            const auto before = session.handle({{"op", "status"}});
            const auto observer_file = directory / "ui-observer.json";
            const Json lease{{"schema", 1},
                             {"session_id", token},
                             {"android_pid", before.at("android_pid")}};
            write_private_json(observer_file, lease);
            { std::ofstream(root / "identity-hold") << "reject initial restart preflight"; }
            const auto refused = session.handle({{"op", "android_reboot"},
                                                 {"session_id", token},
                                                 {"ownership_id", ownership},
                                                 {"mode", "cold"}});
            std::filesystem::remove(root / "identity-hold");
            require(!refused.value("ok", true) &&
                            refused.value("error", std::string{})
                                            .find("ANDROID_IDENTITY_NOT_READY") !=
                                    std::string::npos,
                    "unavailable identity did not reject restart: " + refused.dump());
            require(std::filesystem::exists(observer_file) &&
                            read_private_json(observer_file) == lease,
                    "initial identity failure retired observer metadata");
            const auto after = session.handle({{"op", "status"}});
            require(after.at("android_pid") == before.at("android_pid") &&
                            after["deliveries"]["android"]["generation"] ==
                                    before["deliveries"]["android"]["generation"],
                    "initial identity failure mutated Android generation");
            session.stop();
            require(tcp_port_free(port) && tcp_port_free(port + 1),
                    "preflight fixture left owned SDK listeners");
            std::filesystem::remove_all(root);
            std::cout
                    << "Android initial reboot identity failure preserves observer lease and owned generation\n";

            return 0;
        }
#endif
        auto ready = run({"check"});
        require(ready.exit_code != 0 && ready.output.starts_with("Voyemu:"),
                "explicit check accepted incomplete portable fixture metadata");

        if (api_first_bind || check_portable) {
#ifdef _WIN32
            std::filesystem::remove_all(root);

            return 77;
#else
            constexpr auto archive =
                    "86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9";
            const auto sdk = root / ".local/toolchains/native-sdk",
                       data = selected_machine / "android", clean = data;
            ensure_private_directory(data);
            ensure_private_directory(clean);
            ensure_private_directory(sdk / "emulator");
            write_private_json(
                    qnx_host_bindings_path(root),
                    {{"runtime_sdk", {{"sdk_root", sdk.string()}}}, {"android_port", port}});
            std::filesystem::copy_file(fixture, sdk / "emulator/emulator");
            std::filesystem::permissions(sdk / "emulator/emulator",
                                         std::filesystem::perms::owner_all);

            for (const auto *name : {"kernel-ranchu", "ramdisk.img", "advancedFeatures.ini",
                                     "system.img", "vendor.img"}) {
                std::ofstream(clean / name) << name;
            }

            { std::ofstream(clean / "build.prop") << "ro.build.version.incremental=9767327\n"; }
            Json images = Json::object();

            for (const auto *name : {"system.img", "vendor.img", "userdata-qemu.img",
                                     "encryptionkey.img", "cache.img"}) {
                const auto file = data / name;
                std::ofstream(file) << name;

                if (check_portable && std::string_view(name) == "cache.img") {
                    std::filesystem::resize_file(file, 69206016);
                    std::fstream cache(file, std::ios::in | std::ios::out | std::ios::binary);
                    cache.seekp(1080);
                    cache.put(0x53);
                    cache.put(static_cast<char>(0xef));
                }

                std::ofstream(data / (std::string(name) + ".qcow2"))
                        << "owned tiny overlay fixture";
                images[name] = {{"file", std::string("android/") + name},
                                {"bytes", std::filesystem::file_size(file)},
                                {"sha256", sha256_file(file)}};
                disk_hashes[file] = sha256_file(file);
            }

            write_private_json(selected_machine / "runtime.json",
                               {{"schema", 1},
                                {"kind", "portable-guest-runtime"},
                                {"profile", qinggan09_profile()},
                                {"scope", "android"},
                                {"cluster_available", false},
                                {"android",
                                 {{"api", 28},
                                  {"abi", "arm64-v8a"},
                                  {"base_archive_sha256", archive},
                                  {"images", images}}}});
            auto definition = read_private_json(profile / "profile.json");
            // The registered clean base pins with the boot files of this fixture base.
            auto clean_base = embedded_profile(qinggan09_profile())
                                      .at("definition")
                                      .at("android")
                                      .at("clean_base");

            for (auto &item : clean_base.at("boot_files").items()) {
                item.value() = sha256_file(clean / item.key());
            }

            definition["emulator"] = {{"port", port}};
            definition["android"] = {
                    {"package", "system-images;android-28;default;arm64-v8a"},
                    {"revision", "1"},
                    {"url",
                     "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip"},
                    {"sha256", archive},
                    {"features", Json::object()},
                    {"clean_base", clean_base}};
            write_private_json(profile / "profile.json", definition);
            write_private_json(selected_machine / "profile.json", definition);
            auto portable = read_private_json(selected_machine / "runtime.json");
            portable["profile_sha256"] = sha256_file(selected_machine / "profile.json");
            portable["candidate_receipt_sha256"] = std::string(64, '1');
            portable["resources"] = resources;
            write_private_json(selected_machine / "runtime.json", portable);
            ensure_private_directory(root / "src/app");
            write_private_json(root / "src/app/android-wheel-routes.json",
                               {{"routes", Json::array()}});
            std::filesystem::remove(launch_directory / "android-launch.json");

            if (check_portable) {
                {
                    std::ofstream stale(launch_directory / "cluster-launch.json");
                    stale << "{invalid stale JSON";
                }
                const auto checked = run({"check"});
                require(checked.exit_code == 0 &&
                                checked.output.find("Multimedia launch: available") !=
                                        std::string::npos &&
                                checked.output.find("Cluster launch: missing") != std::string::npos,
                        "Android-only check did not ignore stale cluster launch: " +
                                checked.output);
                const auto pristine_active = native_current_active_directory(
                        root, qinggan09_profile(),
                        voyemu::machine_identity(qinggan09_profile(), qinggan09_profile()));
                const auto pristine_record = pristine_active /
                                             (std::string(qinggan09_profile()) + ".json");
                require(!std::filesystem::exists(pristine_active),
                        "fresh status fixture unexpectedly has active metadata");
                const auto foreign_path = [](const std::filesystem::path &path) {
                    auto value = path.string();
                    std::replace(value.begin(), value.end(), '/', '\\');

                    return "Z:" + value;
                };
                const auto activation_file = launch_directory / "activation.json",
                           android_file = launch_directory / "android-launch.json";
                const auto original_activation = read_private_json(activation_file);
                const auto manifest_sha = sha256_file(selected_machine / "runtime.json");
                auto foreign = original_activation;
                foreign["seed"] = foreign_path(selected_machine);
                foreign["manifest_sha256"] = manifest_sha;
                auto stale = launch;
                stale["pins"] = Json::array({{{"path", foreign_path(sdk / "emulator/emulator")},
                                              {"sha256", std::string(64, '1')}}});
                write_private_json(android_file, stale);
                auto wrong = foreign;
                wrong["seed"] = foreign_path(root / "unrelated" / "machines" / qinggan09_profile());
                write_private_json(activation_file, wrong);
                require(run({"check"}).exit_code != 0,
                        "check accepted another checkout as a host alias");
                wrong = foreign;
                wrong["manifest_sha256"] = std::string(64, '0');
                write_private_json(activation_file, wrong);
                require(run({"check"}).exit_code != 0,
                        "check accepted a foreign cache with a different manifest");
                wrong = foreign;
                wrong["seed"] = selected_machine.string();
                write_private_json(activation_file, wrong);
                require(run({"check"}).exit_code != 0,
                        "check ignored a bad pin in the current host cache");
                write_private_json(activation_file, foreign);
                const auto foreign_activation_sha = sha256_file(activation_file),
                           foreign_launch_sha = sha256_file(android_file);
                require(run({"check"}).exit_code == 0 &&
                                sha256_file(activation_file) == foreign_activation_sha &&
                                sha256_file(android_file) == foreign_launch_sha &&
                                !std::filesystem::exists(pristine_active),
                        "read-only check rejected or rewrote the other host's cache");
                const auto avd_content = launch_directory / "android", system = data / "system.img";
                ensure_private_directory(avd_content);
                std::filesystem::create_hard_link(system, root / "unrelated-system.img");
                require(run({"check"}).exit_code != 0, "check accepted an unknown guest hard link");
                std::filesystem::remove(root / "unrelated-system.img");
                std::filesystem::create_hard_link(system, avd_content / "system.img");
                require(run({"check"}).exit_code == 0 &&
                                std::filesystem::hard_link_count(system) == 2,
                        "read-only check rejected or removed the exact Windows AVD alias");
                std::filesystem::remove(avd_content / "system.img");
                std::filesystem::remove(avd_content);

                for (const auto &[file, sha] : disk_hashes) {
                    require(sha256_file(file) == sha, "read-only host switch changed guest data");
                }

                write_private_json(activation_file, original_activation);
                std::filesystem::remove(android_file);
                const auto header_only = root / "machines/header-only";
                ensure_private_directory(header_only);
                write_private_json(header_only / "runtime.json",
                                   read_private_json(selected_machine / "runtime.json"));
                const auto unprepared_status = run({"status", header_only.string()});
                require(unprepared_status.exit_code == 4 &&
                                unprepared_status.output.find("STATE_EXTENT: invalid file") !=
                                        std::string::npos,
                        "header-only machine incorrectly reported pristine stopped: " +
                                unprepared_status.output);
                const auto pristine_status = run({"status"});
                const auto stopped_row = pristine_status.output.find("\nstopped");
                require(pristine_status.exit_code == 0 && stopped_row != std::string::npos,
                        "fresh prepared machine status must report stopped: " +
                                pristine_status.output);
                std::istringstream stopped_fields(pristine_status.output.substr(stopped_row + 1));
                std::string stopped_state, android_state, qnx_state;
                stopped_fields >> stopped_state >> android_state >> qnx_state;
                require(stopped_state == "stopped" && android_state == "off" && qnx_state == "off",
                        "fresh status must report both guests off");
                require(!std::filesystem::exists(pristine_active),
                        "fresh status created active metadata");
                const auto saved_profile = read_private_json(selected_machine / "profile.json");
                auto changed_profile = saved_profile;
                changed_profile["status_fixture_changed"] = true;
                write_private_json(selected_machine / "profile.json", changed_profile);
                require(run({"status"}).exit_code != 0,
                        "status accepted mismatched prepared profile SHA");
                std::filesystem::remove(selected_machine / "profile.json");
                std::filesystem::create_directory(selected_machine / "profile.json");
                require(run({"status"}).exit_code != 0,
                        "status accepted directory profile snapshot");
                std::filesystem::remove(selected_machine / "profile.json");
                std::filesystem::create_symlink(profile / "profile.json",
                                                selected_machine / "profile.json");
                require(run({"status"}).exit_code != 0, "status accepted linked profile snapshot");
                std::filesystem::remove(selected_machine / "profile.json");
                write_private_json(selected_machine / "profile.json", saved_profile);
                const auto independent_machine = root / "machines/independent-prepared";
                std::filesystem::copy(selected_machine, independent_machine,
                                      std::filesystem::copy_options::recursive);
                const auto independent_status = run({"status", independent_machine.string()});
                require(independent_status.exit_code == 0 &&
                                independent_status.output.find("stopped") != std::string::npos,
                        "prepared same-profile machine did not report its independent stopped status: " +
                                independent_status.output);
                require(!std::filesystem::exists(native_current_active_directory(
                                root, qinggan09_profile(),
                                voyemu::machine_identity(qinggan09_profile(),
                                                         "independent-prepared"))),
                        "independent status created active metadata");

                ensure_private_directory(pristine_active);
                {
                    std::ofstream corrupt(pristine_record);
                    corrupt << "{invalid";
                }
                require(run({"status"}).exit_code != 0, "status hid corrupt active metadata");
                std::filesystem::remove(pristine_record);
                ensure_private_directory(pristine_record);
                require(run({"status"}).exit_code != 0, "status hid directory active metadata");
                std::filesystem::remove(pristine_record);
                std::filesystem::create_symlink(root / "missing-active", pristine_record);
                require(run({"status"}).exit_code != 0, "status hid dangling active metadata");
                std::filesystem::remove(pristine_record);
                const auto unindexed_session = native_session_directory(
                        root, qinggan09_profile(),
                        voyemu::machine_identity(qinggan09_profile(), qinggan09_profile()),
                        random_token());
                ensure_private_directory(unindexed_session);
                require(run({"status"}).exit_code != 0,
                        "status hid a session whose active record is missing");
                std::filesystem::remove_all(unindexed_session.parent_path());
                std::filesystem::remove_all(pristine_active);
                const auto foreign_active = root / "foreign-active";
                ensure_private_directory(foreign_active);
                std::filesystem::create_directory_symlink(foreign_active, pristine_active);
                require(run({"status"}).exit_code != 0,
                        "status accepted linked missing active metadata");
                std::filesystem::remove(pristine_active);
                std::filesystem::remove(foreign_active);
                write_private_json(activation_file, foreign);
                write_private_json(android_file, stale);
                const auto previous_id = random_token();
                const auto previous_directory = native_session_directory(
                        root, qinggan09_profile(),
                        voyemu::machine_identity(qinggan09_profile(), qinggan09_profile()),
                        previous_id);
                const auto previous_endpoint = "\\\\.\\pipe\\voyemu-" + previous_id;
                write_private_json(pristine_record,
                                   {{"schema", 1},
                                    {"profile", qinggan09_profile()},
                                    {"session_id", previous_id},
                                    {"directory", foreign_path(previous_directory)},
                                    {"endpoint", previous_endpoint}});
                write_private_json(previous_directory / "session-owner.json",
                                   {{"schema", 1},
                                    {"session_id", previous_id},
                                    {"directory", foreign_path(previous_directory)},
                                    {"origin", "voyemu-native-supervisor"},
                                    {"creator_pid", 1}});
                write_private_json(previous_directory / "runtime.json",
                                   {{"session_id", previous_id},
                                    {"endpoint", previous_endpoint},
                                    {"stopped", false}});
                const auto busy = run({"start", "--cluster=off"});
                require(busy.exit_code != 0 &&
                                busy.output.find("stop the session on the other host first") !=
                                        std::string::npos &&
                                sha256_file(activation_file) == foreign_activation_sha,
                        "host switch replaced a live Wine session: " + busy.output);
                write_private_json(previous_directory / "runtime.json",
                                   {{"session_id", previous_id},
                                    {"endpoint", previous_endpoint},
                                    {"stopped", true}});
                require(run({"status"}).exit_code == 0,
                        "stopped Wine session status could not be read on macOS");
                const auto started = run({"start"});
                require(started.exit_code == 0,
                        "stopped Wine session did not rebind on macOS: " + started.output);
                const auto stopped = run({"stop"}, 90000);
                require(stopped.exit_code == 0,
                        "could not stop the fixture after host switch: " + stopped.output);
                require(read_private_json(activation_file).at("seed") ==
                                        selected_machine.string() &&
                                read_private_json(android_file).at("machine") == data.string(),
                        "public start kept foreign host bindings");
                require(sha256_file(selected_machine / "runtime.json") == manifest_sha &&
                                sha256_file(data / "userdata-qemu.img") ==
                                        disk_hashes.at(data / "userdata-qemu.img"),
                        "public host switch changed the guest manifest or data image");
                auto invalid = portable;
                invalid["profile_sha256"] = std::string(64, '0');
                write_private_json(selected_machine / "runtime.json", invalid);
                const auto refused = run({"check"});
                require(refused.exit_code != 0, "check accepted invalid portable metadata");
                std::filesystem::remove_all(root);
                std::cout
                        << "Portable check and public start/stop handle an exact Wine alias without changing guest data\n";

                return 0;
            }

            require(!std::filesystem::exists(configuration / qinggan09_profile() /
                                             "activation.json"),
                    "fresh API fixture unexpectedly has a cached activation");
#endif
        }

        if (canonical_api || api_first_bind) {
            try {
                const auto named = configuration / qinggan09_profile();

                if (!api_first_bind) {
                    ensure_private_directory(named);
                    write_private_json(named / "android-launch.json",
                                       read_private_json(launch_directory / "android-launch.json"));
                    write_private_json(
                            named / "activation.json",
                            {{"schema", 1},
                             {"kind", "runtime-configuration"},
                             {"profile", qinggan09_profile()},
                             {"machine", qinggan09_profile()},
                             {"seed", selected_machine.string()},
                             {"manifest_sha256", sha256_file(selected_machine / "runtime.json")},
                             {"integrity_audited", false}});
                }

                const auto input = root / ".local/build/canonical-api-start.json";
                write_private_json(input, {{"schema", 1},
                                           {"request_id", random_token()},
                                           {"op", "session.start"},
                                           {"profile", qinggan09_profile()},
                                           {"machine", qinggan09_profile()},
                                           {"background", true},
                                           {"with_cluster", false}});
                const auto started = run({"test", "--request", input.string()});
                require(started.exit_code == 0, "canonical API machine failed: " + started.output);
                const auto response = Json::parse(started.output);
                require(response.at("ok") == true, "canonical API start not accepted");
                const auto observed = json({"status"});
                require(observed.at("machine") == qinggan09_profile() &&
                                observed.at("machine_seed") == selected_machine.string() &&
                                observed.at("android_pid").get<int>() > 0,
                        "public path did not select the API-owned canonical machine");
                const auto attached = run({"start", "--cluster=off"});
                require(attached.exit_code == 0,
                        "public canonical path could not attach the same API machine: " +
                                attached.output);
                const auto foreign = root / "machines/canonical-other";
                ensure_private_directory(foreign);
                write_private_json(foreign / "runtime.json",
                                   read_private_json(selected_machine / "runtime.json"));
                const auto owner_file = native_current_active_directory(
                                                root, qinggan09_profile(),
                                                voyemu::machine_identity(qinggan09_profile(),
                                                                         qinggan09_profile())) /
                                        (std::string(qinggan09_profile()) + ".json");
                const auto owner_sha = sha256_file(owner_file);
                const auto refused = run({"stop", foreign.string()});
                require(refused.exit_code == 4 &&
                                logged_error(refused.output, "STATE_EXTENT: invalid file"),
                        "foreign scope did not reject its missing active record: exit=" +
                                std::to_string(refused.exit_code) + " output=" + refused.output);
                const auto survived = json({"status"});
                require(survived.at("session_id") == observed.at("session_id") &&
                                survived.at("android_pid") == observed.at("android_pid") &&
                                !survived.at("stopped").get<bool>() &&
                                sha256_file(owner_file) == owner_sha,
                        "foreign scope changed the live canonical SID, PID or owner bytes");
                require(!std::filesystem::exists(
                                native_current_active_directory(
                                        root, qinggan09_profile(),
                                        voyemu::machine_identity(qinggan09_profile(),
                                                                 "canonical-other")) /
                                (std::string(qinggan09_profile()) + ".json")),
                        "foreign scope acquired a canonical active record");
                require_disks();

                if (api_first_bind) {
                    const auto stop_input = root / ".local/build/fresh-api-stop.json";
                    write_private_json(stop_input,
                                       {{"schema", 1},
                                        {"request_id", random_token()},
                                        {"op", "session.stop"},
                                        {"profile", qinggan09_profile()},
                                        {"machine", qinggan09_profile()},
                                        {"session", response.at("result").at("session")}});
                    const auto stopped = run({"test", "--request", stop_input.string()}, 90000);
                    require(stopped.exit_code == 0 && Json::parse(stopped.output).at("ok") == true,
                            "fresh API owned stop failed: " + stopped.output);
                } else {
                    json({"stop"});
                }

                require(tcp_port_free(port) && tcp_port_free(port + 1),
                        "canonical API/public stop left owned SDK listeners");
                require_disks();
                std::filesystem::remove_all(root);
                std::cout
                        << "Canonical API machine and public path share exact seed identity; foreign stop rejected\n";

                return 0;
            } catch (...) {
                try {
                    json({"stop"});
                } catch (...) { /* Ignored: cleanup must not replace the original failure. */
                }

                throw;
            }
        }

#ifndef _WIN32
        if (runtime_paths) {
            const auto token = random_token();
            const auto directory = configuration / qinggan09_profile() / "sessions" / token;
            Session session(directory, token);
            const auto expected = (native_socket_root() / (token + "-qmp.sock")).string();
            session.attach_cluster(
                    {{"schema", 1},
                     {"pins", Json::array({{{"path", fixture.string()},
                                            {"sha256", sha256_file(fixture)}}})},
                     {"ram", "${SESSION}/qnx-ram.bin"},
                     {"qmp", "${RUNTIME}/${TOKEN}-qmp.sock"},
                     {"qemu",
                      {{"executable", fixture.string()},
                       {"args",
                        Json::array({"--runtime-path-fixture", "${RUNTIME}/${TOKEN}-qmp.sock"})},
                       {"env",
                        {{"VOYEMU_NATIVE_FIXTURE_ROOT", root.string()},
                         {"VOYEMU_FIXTURE_RUNTIME_PATH", "${RUNTIME}/${TOKEN}-qmp.sock"}}}}}},
                    2);
            const auto launched = root / "runtime-path-launch.json";
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

            while (!std::filesystem::exists(launched) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            const auto observed = read_private_json(launched, 4096);
            require(observed["argument"] == expected && observed["environment"] == expected,
                    "generic launch did not expand runtime socket root in arguments and environment");
            require(std::filesystem::path(session.endpoint()).parent_path() == native_socket_root(),
                    "Session control and QMP used different runtime roots");
            session.stop();
            require(!read_private_json(directory / "runtime.json")["deliveries"]["qnx"]["cleanup"]
                             .contains("error"),
                    "runtime QMP cleanup rejected its own expanded path");
            std::filesystem::remove_all(root);
            std::cout
                    << "runtime placeholder shared by launch, control and strict QMP cleanup passed\n";

            return 0;
        }
#else
        (void)runtime_paths;
#endif

        if (boot_trace) {
            require(std::getenv("VOYEMU_ANDROID_BOOT_TRACE") == nullptr,
                    "boot trace fixture requires an unset parent opt-in");
            auto receipt = [&] {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);

                while (!std::filesystem::exists(root / "emulator-boot-trace.json") &&
                       std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                return read_private_json(root / "emulator-arguments.json");
            };
            json({"start", "--no-cluster", "--background"});
            const auto normal = receipt();
            require(!read_private_json(root / "emulator-boot-trace.json").at("present").get<bool>(),
                    "default CLI inherited a boot trace opt-in");

            for (const auto &arg : normal) {
                require(arg != "-show-kernel" && arg != "-logcat",
                        "default SDK trace flags changed");
            }

            json({"stop"});
            std::filesystem::remove(root / "emulator-arguments.json");
            std::filesystem::remove(root / "emulator-boot-trace.json");
            boot_trace_value = "1";
            json({"start", "--no-cluster", "--background"});
            const auto traced = receipt();
            require(read_private_json(root / "emulator-boot-trace.json") ==
                            Json{{"present", true}, {"value", "1"}},
                    "public CLI to supervisor to SDK environment inheritance failed");
            const std::vector<std::string> expected{
                    "-show-kernel", "-logcat",
                    "ActivityManager:I SystemServer:I Watchdog:W Zygote:I AndroidRuntime:E libc:F art:I dex2oat:I app_process:I init:I adbd:I *:S"};
            require(traced.size() >= expected.size(), "trace arguments missing");

            for (std::size_t i = 0; i < expected.size(); ++i) {
                require(traced.at(i) == expected[i], "finite trace prefix differs");
            }

            for (const auto *flag : {"-show-kernel", "-logcat"}) {
                require(std::count(traced.begin(), traced.end(), Json(flag)) == 1,
                        "trace option duplicated");
            }

#ifdef _WIN32
            const auto qemu = std::find(traced.begin(), traced.end(), Json("-qemu"));
            require(qemu != traced.end() && std::distance(traced.begin(), qemu) >= 3,
                    "trace options escaped after QEMU boundary");
#endif
            json({"stop"});
            std::filesystem::remove(root / "emulator-arguments.json");
            std::filesystem::remove(root / "emulator-boot-trace.json");
            boot_trace_value = "true";
            const auto denied = run({"start", "--no-cluster", "--background"});
            require(!denied.timed_out && denied.exit_code != 0,
                    "invalid trace value started a guest");
            require(!std::filesystem::exists(root / "emulator-arguments.json") &&
                            !std::filesystem::exists(root / "emulator-boot-trace.json"),
                    "invalid trace value spawned SDK or ADB");
            bool labeled = false;

            for (const auto &epoch : std::filesystem::directory_iterator(
                         configuration / qinggan09_profile() / "sessions")) {
                std::ifstream file(epoch.path() / "supervisor.log");
                std::string log{std::istreambuf_iterator<char>(file), {}};
                labeled |= log.find("ANDROID_BOOT_TRACE:") != std::string::npos;
            }

            require(labeled, "invalid trace value lost its error code");
            boot_trace_value.reset();
            std::filesystem::remove_all(root);
            std::cout << "public CLI boot trace inheritance/default/invalid-spawn guards passed\n";

            return 0;
        }

#ifdef _WIN32
        require(_putenv_s("VOYEMU_NATIVE_FIXTURE_ROOT", root.string().c_str()) == 0,
                "fixture environment unavailable");
#else
        require(::setenv("VOYEMU_NATIVE_FIXTURE_ROOT", root.c_str(), 1) == 0,
                "fixture environment unavailable");
#endif
        // A production supervisor owns one Session and embedded ADB host per process.
        auto presentation_probe = [&](bool background) {
            for (const char *name : {"emulator-arguments.json", "frontend-arguments.json",
                                     "adbd-routing.json", "presentation-probe.json"}) {
                std::filesystem::remove(root / name);
            }

            ProcessSpec probe;
            probe.executable = std::filesystem::canonical(argv[0]);
            probe.arguments = {"--presentation-probe", root.string(),
                               background ? "background" : "foreground"};
            const auto result = execute(probe, 30000);
            require(!result.timed_out && result.exit_code == 0,
                    "isolated presentation probe failed: " + result.output);

            return read_private_json(root / "presentation-probe.json");
        };
#ifndef _WIN32
        std::unique_ptr<Session> presentation_session;
        bool presentation_attach = true;
        auto presentation_request = [&](const Json &operation) {
            require(bool(presentation_session), "presentation session missing");
            presentation_session->tick();

            return presentation_session->handle(operation);
        };
        auto presentation_control = [&](std::vector<std::string> arguments) {
            if (arguments.front() == "status") {
                return presentation_request({{"op", "status"}});
            }

            if (arguments.front() == "stop") {
                presentation_session->stop();

                return presentation_session->handle({{"op", "status"}});
            }

            require(arguments.front() == "start", "unknown presentation fixture command");
            presentation_session = attach_fixture_presentation(root, presentation_attach, false);

            return presentation_session->handle({{"op", "status"}});
        };
#endif

        if (frontend_presentation) {
            auto public_launch = read_private_json(launch_directory / "android-launch.json");
            public_launch["frontend"]["capture"] = true;
            write_private_json(launch_directory / "android-launch.json", public_launch);
            auto android = read_private_json(launch_directory / "android-launch.json");
            android["headless"] = true;
            android["audioOutput"] = true;
            auto receipt = [&](const char *name) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);

                while (!std::filesystem::exists(root / name) &&
                       std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                return read_private_json(root / name);
            };
            const auto background_started = json({"start", "--no-cluster", "--background"});
            const auto headless = json({"status"});
            require(headless["deliveries"].contains("frontend") &&
                            headless["deliveries"]["frontend"]["pid"].get<std::int64_t>() > 0 &&
                            headless["deliveries"]["frontend"]["background"] == true,
                    "public A9 background CLI omitted its mandatory cockpit or requested foreground");
            [[maybe_unused]] const auto background_session =
                    configuration / qinggan09_profile() / "sessions" /
                    background_started.at("session_id").get<std::string>();
#ifdef __APPLE__
            const auto shown_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

            while (!std::filesystem::is_regular_file(background_session /
                                                     "media-native-proof/splash.png") &&
                   std::chrono::steady_clock::now() < shown_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(std::filesystem::is_regular_file(background_session /
                                                     "media-native-proof/splash.png"),
                    "public A9 background cockpit did not render its owned surface");
#endif
            const auto silent = receipt("emulator-arguments.json");
            require(std::find(silent.begin(), silent.end(), Json("-no-audio")) != silent.end(),
                    "public headless CLI enabled audio");
            json({"stop"});
#ifdef __APPLE__
            const auto background_proof =
                    read_private_json(background_session / "media-native-proof/result.json");
            require(background_proof.at("background") == true &&
                            background_proof.at("activation_requested") == false &&
                            background_proof.contains("cluster_ui_inspection"),
                    "public A9 background cockpit omitted controls or requested activation");
#endif
            std::filesystem::remove(root / "emulator-arguments.json");
            std::filesystem::remove(root / "frontend-arguments.json");
#ifdef __APPLE__
            const auto exited_start = json({"start", "--no-cluster", "--background"});
            const auto exited_status = json({"status"});
            const auto exited_pid =
                    exited_status.at("deliveries").at("frontend").at("pid").get<int>();
            require(exited_pid > 0 &&
                            exited_status.at("deliveries").at("frontend").at("status") == "started",
                    "unexpected-exit test has no owned started frontend");
            require(::kill(exited_pid, SIGTERM) == 0,
                    "could not close this fixture's owned frontend");
            const auto exited_directory = configuration / qinggan09_profile() / "sessions" /
                                          exited_start.at("session_id").get<std::string>();
            bool exited_clean = false;
            const auto exited_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);

            while (std::chrono::steady_clock::now() < exited_until) {
                if (std::filesystem::is_regular_file(exited_directory / "runtime.json") &&
                    read_private_json(exited_directory / "runtime.json").value("stopped", false) &&
                    android_port_leasable(port)) {
                    exited_clean = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(exited_clean && tcp_port_free(port) && tcp_port_free(port + 1),
                    "unexpected frontend exit no longer stopped its owned session");
            std::filesystem::remove(root / "emulator-arguments.json");
#endif
#ifdef _WIN32
            constexpr bool native_background = true;
#else
            constexpr bool native_background = false;
#endif
            android["frontend"] = {{"schema", 1},
                                   {"executable", fixture.string()},
                                   {"pins", Json::array({{{"path", fixture.string()},
                                                          {"sha256", sha256_file(fixture)}}})},
                                   {"grpc_port", port + 100},
                                   {"discovery_root", (root / "sdk-running").string()},
                                   {"splash", (root / "splash.png").string()},
                                   {"assets", (root / "assets").string()},
                                   {"fonts", (root / "fonts").string()},
                                   {"state", (root / "placement.json").string()}};
            write_private_json(launch_directory / "android-launch.json", android);
            const auto native = presentation_probe(native_background);
            const auto &arguments = native.at("arguments");
#ifdef _WIN32
            require(std::find(arguments.begin(), arguments.end(), Json("--background")) !=
                                    arguments.end() &&
                            native["frontend"]["background"] == true,
                    "normal Windows frontend must preserve foreground");
#else
            require(std::find(arguments.begin(), arguments.end(), Json("--background")) ==
                                    arguments.end() &&
                            native["frontend"]["background"] == false,
                    "Mac native presentation changed");
#endif
            const auto &audible = native.at("sdk");
            require(std::find(audible.begin(), audible.end(), Json("-no-audio")) == audible.end(),
                    "normal native presentation disabled prepared audio");
#ifndef _WIN32
            const auto free_launch = android;
            android["api"] = 30;
            android["reference_profile"] = "synthetic-passion";
            android["platform"] = "passion";
            android["board"] = "8155";
            android["home"] = {{"package", "com.voyah.os.carlauncher"},
                               {"component", passion_home}};
            android["presentation_adapter"] = passion_presentation_contract("synthetic-passion");
            android.erase("vehicle_adapter");
            android.erase("wheel_adapter");
            android.erase("systemui_adapter");
            android["frontend"]["cluster_controls"] = false;
            {
                std::filesystem::path passion_template = machine / "passion-config-template.ini";
                std::filesystem::copy_file(config_template, passion_template);
                {
                    std::ofstream passion_config(passion_template,
                                                 std::ios::binary | std::ios::app);
                    passion_config << "hw.camera.back=emulated\nhw.camera.front=none\n";
                }
                android["config_template"] = passion_template.string();
                android["pins"].push_back({{"path", passion_template.string()},
                                           {"sha256", sha256_file(passion_template)}});
            }
            write_private_json(launch_directory / "android-launch.json", android);
            const auto passion_native = presentation_probe(true);
            const auto &passion_args = passion_native.at("arguments");
            const auto &passion_sdk = passion_native.at("sdk");
            require(std::find(passion_args.begin(), passion_args.end(), Json("--assets")) !=
                                    passion_args.end() &&
                            std::find(passion_args.begin(), passion_args.end(),
                                      Json("--endpoint")) != passion_args.end(),
                    "legacy false controls flag removed cockpit assets or controller");
            require(std::find(passion_sdk.begin(), passion_sdk.end(), Json("-no-audio")) ==
                            passion_sdk.end(),
                    "Passion background disabled normal prepared audio");
            require(std::find(passion_args.begin(), passion_args.end(), Json("--background")) !=
                                    passion_args.end() &&
                            std::find(passion_args.begin(), passion_args.end(),
                                      Json("--no-cluster")) != passion_args.end() &&
                            passion_native["frontend"]["background"] == true,
                    "Passion background presentation did not retain the native cockpit without focus");
            android.erase("presentation_adapter");
            android.erase("reference_profile");

            // The Free probe returns to the platform and board of the qinggan09 machine.
            for (const auto *key : {"platform", "board"}) {
                if (free_launch.contains(key)) {
                    android[key] = free_launch.at(key);
                } else {
                    android.erase(key);
                }
            }

            android["home"] = {
                    {"package", "com.qinggan.app.launcher"},
                    {"component",
                     "com.qinggan.app.launcher/com.qinggan.app.launcher.activity.MainActivity"}};
            write_private_json(launch_directory / "android-launch.json", android);
            const auto free_status = presentation_probe(true);
            const auto &free_args = free_status.at("arguments");
            const auto &free_sdk = free_status.at("sdk");
            require(std::find(free_sdk.begin(), free_sdk.end(), Json("-no-audio")) ==
                            free_sdk.end(),
                    "Free background disabled normal prepared audio");
            require(std::find(free_args.begin(), free_args.end(), Json("--background")) !=
                                    free_args.end() &&
                            free_status["frontend"]["background"] == true,
                    "Free background native frontend must preserve focus");
#endif
            std::filesystem::remove_all(root);
            std::cout
                    << "Session native presentation and headless automation audio contracts passed\n";

            return 0;
        }

#ifndef _WIN32
        if (cluster_lifecycle || frontend_lifecycle || startup_cancel) {
            write_cluster_fixture(
                    {{"schema", 1},
                     {"pins", Json::array({{{"path", fixture.string()},
                                            {"sha256", sha256_file(fixture)}}})},
                     {"ram", "${SESSION}/qnx-ram.bin"},
                     {"qmp", "${RUNTIME}/${TOKEN}-qmp.sock"},
                     {"qemu",
                      {{"executable", fixture.string()},
                       {"args",
                        Json::array({"--runtime-path-fixture", "${RUNTIME}/${TOKEN}-qmp.sock"})},
                       {"env",
                        {{"VOYEMU_NATIVE_FIXTURE_ROOT", root.string()},
                         {"VOYEMU_FIXTURE_RUNTIME_PATH", "${RUNTIME}/${TOKEN}-qmp.sock"}}}}}});

            if (cluster_lifecycle) {
                auto normal = json({"start", "--background"});
                require(normal["with_cluster"] == true,
                        "prepared cluster did not start by default");
                std::this_thread::sleep_for(std::chrono::milliseconds(1200));
                auto running = json({"status"});
                require(running["deliveries"]["qnx"]["bounded"] == false &&
                                running["deliveries"]["qnx"]["bounded_seconds"] == 0 &&
                                running["deliveries"]["qnx"]["status"] == "booting",
                        "CLI supplied an implicit cluster deadline");
                json({"stop"});
                const auto terminal_status = run({"status"});
                require(terminal_status.exit_code == 0 &&
                                terminal_status.output.find("stopped") != std::string::npos,
                        "stopped session status must survive socket removal: " +
                                terminal_status.output);
                auto metadata = read_private_json(selected_machine / "runtime.json");
                metadata["qnx"] = fixture_manifest.at("qnx");
                write_private_json(selected_machine / "runtime.json", metadata);
                std::filesystem::remove(root / "emulator-arguments.json");
                const auto cluster_only = run({"start", "--multimedia=off", "--cluster=on",
                                               "--seconds", "10", "--background"});
                // start names the cluster window of the new session and how to stop it.
                require(!cluster_only.timed_out && cluster_only.exit_code == 0 &&
                                cluster_only.output.starts_with(
                                        "Session started; the cluster window opens in a few "
                                        "seconds.\nStop it: voyemu stop ") &&
                                cluster_only.output.ends_with(selected_machine.filename().string() +
                                                              "\n") &&
                                std::count(cluster_only.output.begin(), cluster_only.output.end(),
                                           '\n') == 2,
                        "public QNX-only launch failed or emitted unsolicited output: " +
                                cluster_only.output);
                const auto only_status = json({"status"});
                require(only_status.at("android_pid") == 0 &&
                                only_status.at("qnx_pid").get<int>() > 0 &&
                                !std::filesystem::exists(root / "emulator-arguments.json"),
                        "multimedia=off launched Android or lost QNX");
                const auto wrong_scope = run({"start", "--multimedia=on", "--cluster=on"});
                require(wrong_scope.exit_code != 0 &&
                                wrong_scope.output.find("MACHINE_SCOPE") != std::string::npos,
                        "active subsystem flags silently ignored");
                const auto wrong_machine = root / "machines/same-profile-other-machine";
                ensure_private_directory(wrong_machine);
                write_private_json(wrong_machine / "runtime.json", metadata);
                const auto cluster_owner =
                        native_current_active_directory(
                                root, qinggan09_profile(),
                                voyemu::machine_identity(qinggan09_profile(), std::string_view{})) /
                        (std::string(qinggan09_profile()) + ".json");
                const auto cluster_owner_sha = sha256_file(cluster_owner);

                for (const auto *command : {"status", "stop"}) {
                    const auto denied = run({command, wrong_machine.string()});
                    // The read-only status keeps no log and shows the error in full.
                    require(denied.exit_code == 4 &&
                                    (std::string_view(command) == "status"
                                             ? denied.output.find("STATE_EXTENT: invalid file") !=
                                                       std::string::npos
                                             : logged_error(denied.output,
                                                            "STATE_EXTENT: invalid file")),
                            "different machine scope did not reject its absent owner: " +
                                    denied.output);
                }

                const auto surviving_cluster = json({"status"});
                require(surviving_cluster.at("session_id") == only_status.at("session_id") &&
                                surviving_cluster.at("qnx_pid") == only_status.at("qnx_pid") &&
                                !surviving_cluster.at("stopped").get<bool>() &&
                                sha256_file(cluster_owner) == cluster_owner_sha,
                        "foreign scope changed the active QNX SID, PID or owner bytes");
                json({"stop"});
                const auto previous_port = port;
                unsigned bounded_port = port + 2;

                while (bounded_port <= 5682 &&
                       (!tcp_port_free(bounded_port) || !tcp_port_free(bounded_port + 1) ||
                        !tcp_port_free(bounded_port + 30000))) {
                    bounded_port += 2;
                }

                require(bounded_port <= 5682, "no isolated bounded fixture port group available");
                auto bounded_launch = read_private_json(launch_directory / "android-launch.json");
                bounded_launch["port"] = bounded_port;
                bounded_launch["sdk_control"]["grpc_port"] = bounded_port + 30000;

                if (bounded_launch.value("frontend", Json::object()).contains("grpc_port")) {
                    bounded_launch["frontend"]["grpc_port"] = bounded_port + 30000;
                }

                write_private_json(launch_directory / "android-launch.json", bounded_launch);
                port = bounded_port;
                write_private_json(root / "bounded-port-group.json",
                                   {{"previous_console", previous_port},
                                    {"console", port},
                                    {"adbd", port + 1},
                                    {"grpc", port + 30000},
                                    {"all_three_available_before_launch", true}});
                auto bounded = json({"start", "--with-cluster", "--seconds", "1", "--background"});
                require(bounded["session_id"] != normal["session_id"],
                        "bounded start reused a stopped epoch");
                bool complete = false;
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

                while (std::chrono::steady_clock::now() < deadline) {
                    auto value = json({"status"});

                    if (value["deliveries"]["qnx"]["status"] == "bounded_run_complete") {
                        require(value["deliveries"]["android"].value("status", std::string{}) !=
                                        "exited",
                                "bounded fixture Android exited independently: " +
                                        value["deliveries"]["android"].dump());
                        require(!value.value("stopped", true) &&
                                        value["deliveries"]["qnx"]["bounded"] == true,
                                "cluster deadline stopped multimedia or lost its explicit policy");
                        complete = true;
                        break;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                require(complete, "explicit CLI deadline did not complete");
                json({"stop"});
                const auto cli_disabled = json({"start", "--no-cluster", "--background"});
                const auto cli_disabled_status = json({"status"});
                require(cli_disabled["with_cluster"] == false &&
                                cli_disabled_status["qnx_pid"] == 0 &&
                                cli_disabled_status["deliveries"].contains("frontend") &&
                                cli_disabled_status["deliveries"]["frontend"]["pid"]
                                                .get<std::int64_t>() > 0 &&
                                cli_disabled_status["deliveries"]["frontend"]["background"] == true,
                        "public no-cluster background CLI omitted its mandatory cockpit or launched QNX");
                const auto disabled_terminal = json({"stop"});
                const auto &disabled_shutdown =
                        disabled_terminal["deliveries"]["android"]["shutdown"];
                require(disabled_shutdown["identity_verified"] == true &&
                                disabled_shutdown["owned_clean_exit"] == true,
                        "early no-cluster stop did not establish its owned SDK identity and exit cleanly");
                std::filesystem::remove_all(root);
                std::cout << "Public CLI cluster default, scope, deadline and opt-out passed\n";

                return 0;
            }

            auto json = presentation_control;
            auto request = [&](const std::string &, const Json &operation, int = 10000) {
                return presentation_request(operation);
            };
            auto android = read_private_json(launch_directory / "android-launch.json");
            android["headless"] = true;
            android["frontend"] = {{"schema", 1},
                                   {"executable", fixture.string()},
                                   {"pins", Json::array({{{"path", fixture.string()},
                                                          {"sha256", sha256_file(fixture)}}})},
                                   {"grpc_port", port + 100},
                                   {"discovery_root", (root / "sdk-running").string()},
                                   {"splash", (root / "splash.png").string()},
                                   {"assets", (root / "assets").string()},
                                   {"fonts", (root / "fonts").string()},
                                   {"state", (root / "placement.json").string()}};
            write_private_json(launch_directory / "android-launch.json", android);
            auto frontend_args = [&]() {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);

                while (!std::filesystem::exists(root / "frontend-arguments.json") &&
                       std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                return read_private_json(root / "frontend-arguments.json");
            };

            if (startup_cancel) {
                presentation_attach = false;
                json({"start"});
                const auto before = json({"status"});
                require(before["android_pid"].get<std::int64_t>() > 0 && before["qnx_pid"] == 0 &&
                                !before["deliveries"].contains("frontend"),
                        "startup cancel fixture did not retain its Android-only child");
                const auto began = std::chrono::steady_clock::now();
                const auto terminal = json({"stop"});
                require(std::chrono::steady_clock::now() - began < std::chrono::seconds(15),
                        "startup cancellation exceeded its bounded cleanup budget");
                require(terminal.value("stopped", false) && tcp_port_free(port) &&
                                tcp_port_free(port + 1),
                        "startup cancellation left owned SDK listeners");
                std::filesystem::remove_all(root);
                std::cout << "Session immediate startup cancellation and owned cleanup passed\n";

                return 0;
            }

            presentation_attach = true;
            auto disabled = json({"start", "--no-cluster"});
            auto panel_status = json({"status"});
            const auto args = frontend_args();
            require(panel_status["qnx_pid"] == 0, "opt-out launched QNX");
            require(std::find(args.begin(), args.end(), Json("--background")) == args.end() &&
                            panel_status["deliveries"]["frontend"]["background"] == false,
                    "foreground native presentation was not attached");
            require(panel_status["deliveries"]["frontend"]["cluster_panel"] == true,
                    "off-cluster panel receipt lost layout availability");
            require(std::find(args.begin(), args.end(), Json("--with-cluster")) != args.end(),
                    "no-cluster removed the prepared wheel and parameter panel");
            require(panel_status["deliveries"]["qnx"]["lifecycle_supported"] == true,
                    "off cluster lost its prepared power controls");
            const auto endpoint = panel_status.at("endpoint").get<std::string>();
            const auto media_pid = panel_status.at("android_pid").get<std::int64_t>();
            auto enabled = request(endpoint, {{"op", "cluster_power"},
                                              {"session_id", disabled.at("session_id")},
                                              {"enabled", true}});
            require(enabled.value("ok", false) && enabled.at("qnx_pid").get<std::int64_t>() > 0 &&
                            enabled.at("android_pid") == media_pid,
                    "prepared off cluster could not cold-start beside the same multimedia");
            require(enabled["deliveries"]["qnx"]["quickboot"]["reason"] == "explicit_cold_boot",
                    "power from off used a warm snapshot");
            const auto cluster_pid = enabled.at("qnx_pid"),
                       frontend_pid = enabled.at("deliveries").at("frontend").at("pid");
            auto ready_media = [&]() {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);

                while (std::chrono::steady_clock::now() < deadline) {
                    auto current = request(endpoint, {{"op", "status"}});

                    if (current.at("deliveries").at("android").value("status", std::string{}) ==
                        "guest_acknowledged") {
                        return current;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                }

                throw std::runtime_error("multimedia lifecycle fixture did not become ready");
            };
            ready_media();
            const auto checkpoint = launch_directory / "android/voyemu-quickboot.json";
            const auto prepared_key = read_private_json(checkpoint).at("key");
            require(prepared_key.at("prepared").at("sdk_control").at("discovery_root") ==
                            android.at("sdk_control").at("discovery_root"),
                    "initial QuickBoot key lost the prepared SDK discovery root");
            auto media_off = request(endpoint, {{"op", "media_power"}, {"enabled", false}}, 90000);
            require(media_off.value("ok", false) &&
                            media_off["deliveries"]["android"]["status"] == "off" &&
                            !media_off.value("stopped", true),
                    "multimedia power-off stopped the session");
            require(media_off["qnx_pid"] == cluster_pid &&
                            media_off["deliveries"]["frontend"]["pid"] == frontend_pid &&
                            tcp_port_free(port) && tcp_port_free(port + 1),
                    "multimedia power-off changed QNX/window or retained SDK listeners");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            require(request(endpoint, {{"op", "status"}})["deliveries"]["android"]["status"] ==
                            "off",
                    "watchdog overwrote explicit power-off");
            auto media_on = request(endpoint, {{"op", "media_power"}, {"enabled", true}}, 90000);
            require(media_on.value("ok", false) &&
                            media_on["deliveries"]["android"]["generation"] == 2 &&
                            media_on["deliveries"]["android"]["quickboot"]["reason"] ==
                                    "explicit_cold_boot",
                    "power-on did not create a fresh cold Android generation: " +
                            media_on.value("error", std::string{}));
            ready_media();
            require(read_private_json(checkpoint).at("key") == prepared_key,
                    "media power-on changed the prepared QuickBoot key");
            ready_media();
            auto media_restarted = request(endpoint, {{"op", "media_restart"}}, 90000);
            require(media_restarted.value("ok", false) &&
                            media_restarted["deliveries"]["android"]["generation"] == 3 &&
                            media_restarted["qnx_pid"] == cluster_pid &&
                            media_restarted["deliveries"]["frontend"]["pid"] == frontend_pid,
                    "multimedia restart did not preserve QNX and its native window");
            ready_media();
            require(read_private_json(checkpoint).at("key") == prepared_key,
                    "media restart changed the prepared QuickBoot key");
            auto off = request(endpoint, {{"op", "cluster_power"},
                                          {"session_id", disabled.at("session_id")},
                                          {"enabled", false}});
            require(off.value("ok", false) && off["deliveries"]["qnx"]["status"] == "off" &&
                            !off.value("stopped", true),
                    "power-off stopped the multimedia session");
            json({"stop"});
            presentation_session.reset();
            std::filesystem::remove(launch_directory / "cluster-launch.json");
            const auto media_only = presentation_probe(false);
            const auto &media_arguments = media_only.at("arguments");
            require(std::find(media_arguments.begin(), media_arguments.end(),
                              Json("--with-cluster")) == media_arguments.end(),
                    "a profile without QNX acquired a phantom cluster panel");
            require(tcp_port_free(port) && tcp_port_free(port + 1),
                    "lifecycle fixture left owned SDK listeners");
            std::filesystem::remove_all(root);
            std::cout
                    << "Session frontend, multimedia and cluster lifecycle with owned cleanup passed\n";

            return 0;
        }
#else
        (void)cluster_lifecycle;
        (void)frontend_lifecycle;
        (void)startup_cancel;
#endif

        if (control_latency) {
            std::string endpoint = "${RUNTIME}/${TOKEN}-qmp.sock";
            Json qmp = endpoint;
#ifdef _WIN32
            unsigned qmp_port = port + 30000;

            while (qmp_port < 65535 && !tcp_port_free(qmp_port)) {
                ++qmp_port;
            }

            require(tcp_port_free(qmp_port), "no isolated fixture QMP port available");
            endpoint = "tcp:127.0.0.1:" + std::to_string(qmp_port);
            qmp = {{"host", "127.0.0.1"}, {"port", qmp_port}};
#endif
            // This idle host child exercises control publication, not a QMP server or guest.
            write_cluster_fixture({{"schema", 1},
                                   {"pins", Json::array({{{"path", fixture.string()},
                                                          {"sha256", sha256_file(fixture)}}})},
                                   {"ram", "${SESSION}/qnx-ram.bin"},
                                   {"qmp", qmp},
                                   {"qemu",
                                    {{"executable", fixture.string()},
                                     {"args", Json::array({"--runtime-path-fixture", endpoint})},
                                     {"env",
                                      {{"VOYEMU_NATIVE_FIXTURE_ROOT", root.string()},
                                       {"VOYEMU_FIXTURE_RUNTIME_PATH", endpoint}}}}}});
        }

        if (clock_service) {
            write_private_json(root / "clock-state.json", {{"offset_ms", -3600000}});
            std::ofstream(root / "alarm-service-absent") << "not registered";
        }

        if (clock_observation) {
            write_private_json(root / "clock-state.json", {{"offset_ms", -3600000}});
            write_private_json(root / "clock-observation-fixture.json", {{"delay_ms", 3500}});
        }

        if (clock_flow || slow_startup) {
            write_private_json(root / "clock-state.json", {{"offset_ms", -28000}});
            std::ofstream(root / "clock-deny") << "reject time correction until tested";
        }

        if (state_flow) {
            write_private_json(root / "readiness-probes.json", Json::object());
        }

        const auto saved_scenario_path =
                native_current_active_directory(
                        root, qinggan09_profile(),
                        voyemu::machine_identity(qinggan09_profile(), std::string_view{}))
                        .parent_path() /
                "state/vehicle-state.json";
        auto fixture_files = [&]() {
            Json files = Json::object();

            for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
                if (entry.is_regular_file() && !entry.is_symlink()) {
                    files[entry.path().lexically_relative(root).generic_string()] =
                            sha256_file(entry.path());
                }
            }

            return files;
        };
        const auto before_resume = fixture_files();
        auto missing_resume =
                run(control_latency ? std::vector<std::string>{"_start", "--resume", "--cluster=on"}
                                    : std::vector<std::string>{"_start", "--resume"});
        const auto resume_error = Json::parse(missing_resume.output, nullptr, false);
        require(resume_error.is_object(),
                "explicit resume returned no JSON: " + missing_resume.output);
        require(missing_resume.exit_code == 4 && !resume_error.at("ok").get<bool>() &&
                        resume_error.at("error") ==
                                "VEHICLE_STARTUP: saved scenario missing: " +
                                        saved_scenario_path.string() +
                                        "; use fresh scenario or provide a saved vehicle scenario before explicit resume",
                "missing saved scenario did not explain explicit resume recovery: " +
                        missing_resume.output);
        const auto launch_lock =
                native_current_active_directory(
                        root, qinggan09_profile(),
                        voyemu::machine_identity(qinggan09_profile(), std::string_view{})) /
                (std::string(qinggan09_profile()) + ".launch.lock");
        require(std::filesystem::symlink_status(launch_lock).type() ==
                                std::filesystem::file_type::regular &&
                        std::filesystem::file_size(launch_lock) == 0,
                "startup admission lock is not an ordinary empty file");
#ifndef _WIN32
        struct stat launch_lock_status{};
        require(::lstat(launch_lock.c_str(), &launch_lock_status) == 0 &&
                        launch_lock_status.st_uid == ::geteuid() &&
                        (launch_lock_status.st_mode & 0777) == 0600,
                "startup admission lock owner or mode differs");
#endif
        auto after_resume = fixture_files();
        const auto launch_lock_key = launch_lock.lexically_relative(root).generic_string();

        if (!before_resume.contains(launch_lock_key)) {
            after_resume.erase(launch_lock_key);
        }

        require(after_resume == before_resume,
                "missing resume changed checkpoint, inputs or allocated session files: " +
                        Json::diff(before_resume, after_resume).dump());
        require(!std::filesystem::exists(configuration / qinggan09_profile() / "sessions") &&
                        !std::filesystem::exists(root / "identity.json") && tcp_port_free(port) &&
                        tcp_port_free(port + 1),
                "missing resume allocated a session, supervisor or SDK guest");
        require(!std::filesystem::exists(saved_scenario_path.parent_path()),
                "missing resume created saved state directory");
        const auto saved_scenario = apply_patch(initial_state(VehicleCatalog::qinggan09),
                                                {{"speed", 80}, {"gear", 2}, {"tail_gate", 1}});
#ifdef _WIN32
        if (identity_backpressure) {
            ensure_private_directory(root / "identity-probe-clients");
            write_private_json(root / "identity-bootstrap-hold",
                               {{"phase", 1}, {"released", false}});
        }
#endif
#ifdef _WIN32
        if (slow_startup) {
            write_private_json(root / "slow-startup-guest.json", {{"milliseconds", 1800}});
        }
#else
        require(!slow_startup, "slow Windows startup fixture requires Windows");
#endif

        if (boot_observation) {
            std::ofstream(root / "boot-id-fail-once") << "one guarded boot observation failure\n";
        }

        if (root_restart) {
            write_private_json(root / "root-restart-fixture.json",
                               {{"after_uid", root_unverified ? 2000 : 0},
                                {"secret_negative", root_unverified}});
        }

        const auto slow_owner = slow_startup ? random_token() : std::string{};
        std::vector<std::string> first_arguments{"start"};

        if (orphan_recovery) {
            first_arguments.push_back("--background");
        }

        if (control_latency) {
            // This fixture already pins its own QNX child and needs no host engine download.
            first_arguments.emplace_back("--cluster=on");
        }

        auto first = slow_startup ? json({"start", "--exclusive", "--ownership-id", slow_owner})
                                  : json(std::move(first_arguments));
        auto first_id = first.at("session_id").get<std::string>();
        require(first["runtime"] == "native C++" && first["with_cluster"] == control_latency,
                "start selected the wrong cluster scope");
        auto active = json({"status"});
        require(active["session_id"] == first_id && active["stopped"] == false,
                "first native epoch not active");
        require(active["state"]["raw_oem"]["speed"] == 0 &&
                        active["state"]["raw_oem"]["gear"] == 0 &&
                        active["state"]["raw_oem"]["ambient_temp"] == 120,
                "fresh launch restored the saved driving scenario");
        require(!std::filesystem::exists(saved_scenario_path),
                "fresh launch created a saved scenario");
        write_private_json(saved_scenario_path, saved_scenario);
        const auto saved_scenario_sha = sha256_file(saved_scenario_path);

        if (clock_flow) {
            bool ready = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);

            while (std::chrono::steady_clock::now() < deadline) {
                const auto current = json({"status"});
                const auto &android = current.at("deliveries").at("android");

                if (android.value("guest_acknowledged", false) &&
                    android.value("home", Json::object()).value("status", std::string{}) ==
                            "visible_on_primary" &&
                    android.value("displays", Json::object()).value("status", std::string{}) ==
                            "both_guest_displays_ready" &&
                    android.value("systemui", Json::object()).value("status", std::string{}) ==
                            "both_OEM_bars_visible" &&
                    android.value("stage_errors", Json::object()).empty()) {
                    const auto &clock = android.at("clock");
                    require(clock.at("status") == "degraded" && clock.at("synced") == false,
                            "offset clock falsely reported synced");
                    ready = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(ready, "authenticated offset clock blocked actual readiness");

            for (const auto *phase : {"down", "up"}) {
                require(request(active.at("endpoint").get<std::string>(),
                                {{"op", "wheel"},
                                 {"session_id", first_id},
                                 {"id", "menu"},
                                 {"phase", phase},
                                 {"owner", "clock-quality"}})
                                .value("accepted", false),
                        "degraded-clock wheel phase was not queued");
            }

            require(read_private_json(root / "clock-set-receipt.json").at("count") == 1,
                    "clock correction was replayed");
        }

        auto first_directory = configuration / qinggan09_profile() / "sessions" / first_id;

        if (root_restart) {
#ifdef _WIN32
            bool observed = false;
            Json android;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(25);

            while (std::chrono::steady_clock::now() < until) {
                auto value = json({"status"});
                android = value.at("deliveries").at("android");
                const auto error = android.value("last_error", std::string{});

                if (root_unverified
                            ? error == "ANDROID_STARTUP_MUTATION: write attempt cannot be replayed"
                            : android.value("transport", Json::object())
                                              .value("status", std::string{}) == "root_ready") {
                    observed = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(observed,
                    "ROOT_RESTART: existing pipeline did not reach expected guarded outcome");
            const auto observation = read_private_json(root / "root-restart-observation.json");
            require(observation.at("root_calls") == 1 &&
                            observation.at("offline_outer_requests") == 1,
                    "ROOT_RESTART: mutation repeated or offline readback not exercised");
            require(observation.value("uid2000_reads", 0) >= 1,
                    "ROOT_RESTART: initial unprivileged UID not observed");

            if (root_unverified) {
                require(!android.value("guest_acknowledged", false),
                        "ROOT_RESTART: UID2000 falsely acknowledged");
                std::this_thread::sleep_for(std::chrono::milliseconds(350));
                require(read_private_json(root / "root-restart-observation.json")
                                        .at("root_calls") == 1,
                        "ROOT_RESTART: failed root mutation replayed");
            } else {
                require(observation.value("uid0_reads", 0) >= 1,
                        "ROOT_RESTART: same-boot UID0 recovery absent");
            }

            const auto stopped = json({"stop"});
            require(stopped.value("stopped", false) && tcp_port_free(port) &&
                            tcp_port_free(port + 1),
                    "ROOT_RESTART: owned stop/listener proof failed");
            std::ifstream file(first_directory / "supervisor.log");
            const std::string contents{std::istreambuf_iterator<char>(file), {}};
            file.close();
            require(contents.find("fixture-root-private-") == std::string::npos,
                    "ROOT_RESTART: response secret leaked");
            const auto token = read_private_json(root / "identity.json")
                                       .at("token")
                                       .get<std::string>(); // The cold nonce equals the public
                                                            // session ID in this fixture; existing
                                                            // lifecycle IDs are permitted.
            bool diagnostic = false;
            std::istringstream lines(contents);
            std::string line;

            while (std::getline(lines, line)) {
                const auto event = Json::parse(line);
                const auto message = event.value("message", std::string{});

                if (message != "android.started" && message != "session.stopped") {
                    require(event.at("fields").dump().find(token) == std::string::npos,
                            "ROOT_RESTART: guest nonce in non-lifecycle diagnostics");
                }

                if (message != "startup.adb_response") {
                    continue;
                }

                const auto &fields = event.at("fields");

                if (fields.at("phase") != static_cast<unsigned>(WindowsStartupPhase::root_submit)) {
                    continue;
                }

                require(fields.at("outer_exit_code") == int(root_unverified) &&
                                fields.at("timed_out") == false,
                        "ROOT_RESTART: raw root result diagnostics missing");
                require(fields.at("diagnostic_message")
                                        .get<std::string>()
                                        .find("fixture-root-private-") == std::string::npos,
                        "ROOT_RESTART: canonical message retained secret");
                diagnostic = true;
            }

            require(diagnostic, "ROOT_RESTART: production DEBUG classifier event missing");
            write_private_json(root / ".local/build/root-restart-result.json",
                               {{"synthetic_fixture", true},
                                {"real_VM", false},
                                {"UID0_recovery", !root_unverified},
                                {"UID2000_no_replay", root_unverified},
                                {"root_calls", 1},
                                {"offline_outer_requests", 1},
                                {"actual_classifier_event", true},
                                {"private_payload_absent", true},
                                {"owned_stop_verified", true}});
            std::cout
                    << "Raw root restart and offline readback privacy passed; evidence retained at "
                    << root.string() << '\n';

            return 0;
#else
            throw std::runtime_error("root restart fixture requires Windows");
#endif
        }

        if (clock_service) {
#ifdef _WIN32
            bool ready = false;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);

            while (std::chrono::steady_clock::now() < until) {
                const auto current = json({"status"});
                const auto &android = current.at("deliveries").at("android");

                if (android.value("guest_acknowledged", false)) {
                    require(android.at("clock").at("status") == "degraded" &&
                                    android.at("clock").at("synced") == false,
                            "absent alarm service lost degraded quality");
                    ready = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(ready, "absent alarm service blocked readiness");
            require(!std::filesystem::exists(root / "clock-set-receipt.json") &&
                            !std::filesystem::exists(root / "premature-clock-write.json"),
                    "absent service consumed clock mutation");
            const auto stopped = json({"stop"});
            require(stopped.value("ok", false) && tcp_port_free(port) && tcp_port_free(port + 1),
                    "clock service fixture cleanup failed");
            std::cout << "Absent alarm service retained degraded readiness without a write\n";

            return 0;
#else
            throw std::runtime_error("clock service fixture requires Windows");
#endif
        }

        if (clock_observation) {
#ifdef _WIN32
            bool ready = false;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(45);

            while (std::chrono::steady_clock::now() < until) {
                const auto current = request(active.at("endpoint").get<std::string>(),
                                             {{"op", "status"}, {"session_id", first_id}}, 5000);
                const auto &android = current.at("deliveries").at("android");

                if (android.value("guest_acknowledged", false)) {
                    require(android.at("clock").at("status") == "degraded" &&
                                    android.at("clock").at("synced") == false,
                            "delayed clock falsely reported synced");
                    ready = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(ready, "delayed clock mutation blocked actual readiness");
            const auto application = read_private_json(root / "clock-apply-observation.json");
            require(application.at("actual_apply_host_ms").get<std::int64_t>() -
                                    application.at("target_ms").get<std::int64_t>() >=
                            3500,
                    "fixture did not reproduce stale pre-sampled clock target");
            require(read_private_json(root / "clock-set-receipt.json").at("count") == 1,
                    "clock mutation replayed after stale readback");
            const auto stopped = json({"stop"});
            require(stopped.value("ok", false) && tcp_port_free(port) && tcp_port_free(port + 1),
                    "clock observation fixture cleanup failed");
            std::ifstream input(first_directory / "supervisor.log");
            std::string line;
            Json launches = Json::object(), received = Json::object();
            unsigned validated = 0;
            bool stale = false, body255 = false;

            while (std::getline(input, line)) {
                Json event;
                try {
                    event = Json::parse(line);
                } catch (...) {
                    continue;
                }

                if (event.value("message", std::string{}) != "startup.clock") {
                    continue;
                }

                const auto &fields = event.at("fields");
                require(!fields.contains("output") && !fields.contains("nonce") &&
                                !fields.contains("boot_id") && !fields.contains("arguments"),
                        "clock diagnostics exposed guest identity/payload");
                const auto key = fields.at("action_sha256").get<std::string>();
                const auto stage = fields.at("stage").get<std::string>();

                if (stage == "launch") {
                    launches[key] = fields;
                } else if (stage == "receive") {
                    received[key] = fields;
                } else if (stage == "validated") {
                    ++validated;
                    require(launches.contains(key) && received.contains(key),
                            "clock validation lacks actual host interval");
                    const auto &launch = launches.at(key);
                    const auto &receive = received.at(key);
                    require(launch.at("sdk_pid") == fields.at("sdk_pid") &&
                                    receive.at("sdk_pid") == fields.at("sdk_pid") &&
                                    launch.at("generation") == fields.at("generation") &&
                                    receive.at("generation") == fields.at("generation"),
                            "clock observations crossed retained action identity");
                    require(receive.at("host_receive_steady_ns").get<std::int64_t>() >=
                                            launch.at("host_launch_steady_ns")
                                                    .get<std::int64_t>() &&
                                    fields.at("host_owner_boot_nonce_verified") == true,
                            "clock diagnostics used unverified or reversed interval");

                    if (launch.contains("target_utc_ms")) {
                        require(fields.at("body_exit_code") == 255 &&
                                        receive.at("outer_exit_code") == 0,
                                "clock body exit was confused with outer adb status");
                        require(launch.at("target_utc_ms") == application.at("target_ms"),
                                "logged target differs from actual submitted time");
                        body255 = true;
                        stale = receive.at("host_receive_utc_ms").get<std::int64_t>() -
                                        fields.at("guest_tail_utc_seconds").get<std::int64_t>() *
                                                1000 >
                                2000;
                    }
                }
            }

            input.close();
            require(validated >= 2 && body255 && stale,
                    "CLOCK_OBSERVATION: missing validated delayed-clock scalar events");
            write_private_json(root / ".local/build/clock-observation-result.json",
                               {{"schema", 1},
                                {"synthetic_fixture", true},
                                {"actual_VM", false},
                                {"stale_target_proven", true},
                                {"strict_clock_readiness", false},
                                {"clock_writes", 1},
                                {"body_exit", 255},
                                {"outer_exit", 0},
                                {"validated_observations", validated},
                                {"owned_stop", true}});
            std::cout
                    << "Validated delayed clock target/body exit/host intervals retained; degraded readiness and once-only mutation preserved; evidence "
                    << root.string() << '\n';

            return 0;
#else
            throw std::runtime_error("clock observation fixture requires Windows");
#endif
        }

        if (slow_startup) {
            bool ready = false;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);

            while (std::chrono::steady_clock::now() < until) {
                const auto observed = request(active.at("endpoint").get<std::string>(),
                                              {{"op", "status"}, {"session_id", first_id}}, 5000);
                const auto android = observed.at("deliveries").at("android");
                ready = android.value("guest_acknowledged", false) &&
                        android.value("wheel_bootstrap", Json::object())
                                        .value("status", std::string{}) == "session-ready" &&
                        android.value("home", Json::object()).value("status", std::string{}) ==
                                "visible_on_primary" &&
                        android.value("displays", Json::object()).value("status", std::string{}) ==
                                "both_guest_displays_ready" &&
                        android.value("systemui", Json::object()).value("status", std::string{}) ==
                                "both_OEM_bars_visible" &&
                        android.value("stage_errors", Json::object()).empty();

                if (ready) {
                    require(android.at("clock").at("status") == "degraded" &&
                                    android.at("clock").at("synced") == false,
                            "slow offset clock falsely reported synced");
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            require(ready,
                    "slow startup guest commands never reached complete protected state/home/two-display readiness");
            require(std::filesystem::exists(root / "late-clock-injected.json") &&
                            read_private_json(root / "delayed-state-ack.json")
                                    .value("pending_observed", false),
                    "late UTC and delayed ACK were not exercised");
            require(read_private_json(root / "clock-set-receipt.json").at("count") == 1,
                    "slow startup replayed clock correction");
            const auto stable_until = std::chrono::steady_clock::now() + std::chrono::seconds(6);

            while (std::chrono::steady_clock::now() < stable_until) {
                const auto sample = json({"status"});
                const auto &delivery = sample.at("deliveries").at("android");
                require(delivery.value("guest_acknowledged", false) &&
                                delivery.value("startup_observation", Json::object())
                                                .value("age_ms", std::int64_t{64000}) <= 63000,
                        "slow background maintenance lost bounded readiness");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            const auto boot = request(active.at("endpoint").get<std::string>(),
                                      {{"op", "test_boot_identity"}, {"session_id", first_id}},
                                      30000);
            require(boot.value("ok", false) &&
                            android_boot_id_valid(boot.at("boot_id").get<std::string>()),
                    "slow public fresh boot observation failed");
            const auto changed = json({"patch", "--values", Json{{"speed", 43}}.dump()});
            const auto wanted = changed.at("state").at("sequence");
            bool applied = false;
            const auto state_until = std::chrono::steady_clock::now() + std::chrono::seconds(30);

            while (std::chrono::steady_clock::now() < state_until) {
                const auto sample = json({"status"});
                const auto &delivery = sample.at("deliveries").at("android");

                if (delivery.value("guest_acknowledged", false) &&
                    delivery.value("sequence", Json{}) == wanted) {
                    applied = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            require(applied, "slow steady-state full revision never received exact ACK");

            for (const auto *phase : {"down", "up"}) {
                require(request(active.at("endpoint").get<std::string>(),
                                {{"op", "wheel"},
                                 {"session_id", first_id},
                                 {"id", "menu"},
                                 {"phase", phase},
                                 {"owner", "slow-startup-test"}})
                                .value("accepted", false),
                        "slow wheel phase was not queued");
            }

            bool wheel_ack = false;
            const auto wheel_until = std::chrono::steady_clock::now() + std::chrono::seconds(20);

            while (std::chrono::steady_clock::now() < wheel_until) {
                const auto sample = json({"status"});
                const auto wheel =
                        sample.at("deliveries").at("android").value("wheel", Json::object());

                if (wheel.value("receipt", Json::object()).value("phase", std::string{}) == "up" &&
                    wheel.value("guest_acknowledged", false)) {
                    wheel_ack = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            require(wheel_ack, "slow steady-state wheel submit/readback never completed");
            auto public_call = [&](Json input, bool expect_success = true) {
                input["schema"] = 1;
                input["request_id"] = random_token();
                input["profile"] = qinggan09_profile();
                input["session"] = {{"id", first_id}, {"ownership_id", slow_owner}};
                const auto path = root / ".local/build" / ("slow-api-" + random_token() + ".json");
                ensure_private_directory(path.parent_path());
                write_private_json(path, input);
                const auto result = [&] {
#ifdef _WIN32
                    return fixture_public_cli(
                            cli, {"test", "--request", path.string(), "--root", root.string()},
                            root);
#else
                    return run({"test", "--request", path.string()}, 30000);
#endif
                }();
                require(!result.timed_out &&
                                (expect_success ? result.exit_code == 0 : result.exit_code != 0),
                        "slow public API outcome differed: " + result.output);
                const auto envelope = Json::parse(result.output);
                require(envelope.value("schema", 0) == 1 &&
                                envelope.at("request_id") == input.at("request_id") &&
                                envelope.value("ok", false) == expect_success,
                        "slow public API admission differed");

                return expect_success ? envelope.at("result") : envelope;
            };
            const auto before_capture = public_call(
                    {{"op", "session.screenshot"},
                     {"screens", Json::array({"primary", "secondary"})},
                     {"timeout_ms", 10000},
                     {"max_skew_ms", 10000},
                     {"artifact_dir", (root / ".local/build/slow-sdk-before").string()}});

            for (const char *screen : {"primary", "secondary"}) {
                const auto input = public_call(
                        {{"op", "input.tap"}, {"screen", screen}, {"x", 100}, {"y", 100}});
                require(input.value("sdk_completed", false),
                        "slow public pointer lacked real fixture SDK ACK");
            }

            const auto after_capture = public_call(
                    {{"op", "session.screenshot"},
                     {"screens", Json::array({"primary", "secondary"})},
                     {"timeout_ms", 10000},
                     {"max_skew_ms", 10000},
                     {"artifact_dir", (root / ".local/build/slow-sdk-after").string()}});
            require(before_capture.at("accepted") == true && after_capture.at("accepted") == true &&
                            before_capture.at("frames").size() == 2 &&
                            after_capture.at("frames").size() == 2,
                    "slow public capture did not export both synthetic SDK slots");

            for (unsigned slot = 0; slot < 2; ++slot) {
                const auto &before = before_capture.at("frames").at(slot);
                const auto &after = after_capture.at("frames").at(slot);
                require(before.at("source_width") == 1920 && before.at("source_height") == 720 &&
                                after.at("source_width") == 1920 &&
                                after.at("source_height") == 720 &&
                                before.at("sha256") != after.at("sha256"),
                        "fixture SDK input did not change original synthetic pixels");
            }

            require(read_private_json(root / "fixture-sdk-touch.json").at("touch_calls") == 4,
                    "public taps did not release both fixture SDK touches");
            const auto short_started = std::chrono::steady_clock::now();
            const auto short_reply = request(
                    active.at("endpoint").get<std::string>(),
                    {{"op", "test_boot_identity"}, {"session_id", first_id}, {"timeout_ms", 2500}},
                    5000);
            require(!short_reply.value("ok", false) &&
                            std::chrono::steady_clock::now() - short_started <
                                    std::chrono::seconds(4),
                    "short caller budget did not cancel retained delayed identity");
            bool recovered = false;
            const auto recovery_until = std::chrono::steady_clock::now() + std::chrono::seconds(15);

            while (std::chrono::steady_clock::now() < recovery_until) {
                const auto sample = json({"status"});

                if (sample.at("deliveries").at("android").value("guest_acknowledged", false)) {
                    recovered = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            require(recovered, "cancelled short observation prevented readonly steady recovery");
            const auto touches_before =
                    read_private_json(root / "fixture-sdk-touch.json").at("touch_calls");
            {
                std::ofstream marker(root / "stale-identity");
                marker << "fixture-only foreign nonce";
            }
            bool mismatch = false;
            const auto mismatch_until = std::chrono::steady_clock::now() + std::chrono::seconds(15);

            while (std::chrono::steady_clock::now() < mismatch_until) {
                const auto sample = json({"status"});
                const auto &android = sample.at("deliveries").at("android");

                if (android.value("last_error", std::string{})
                            .starts_with("ANDROID_IDENTITY_MISMATCH:")) {
                    require(!android.value("guest_acknowledged", true),
                            "nonce mismatch retained readiness");
                    mismatch = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            require(mismatch, "framed background nonce mismatch was not revoked");
            public_call({{"op", "input.tap"}, {"screen", "primary"}, {"x", 100}, {"y", 100}},
                        false);
            require(read_private_json(root / "fixture-sdk-touch.json").at("touch_calls") ==
                            touches_before,
                    "mismatched identity dispatched SDK mutation");
            const auto rejected_stop = run({"stop"}, 10000);
            require(!rejected_stop.timed_out && rejected_stop.exit_code != 0 &&
                            rejected_stop.output.find("ANDROID_IDENTITY_MISMATCH") !=
                                    std::string::npos,
                    "mismatched stop did not reject before lifecycle effects");
            std::filesystem::remove(root / "stale-identity");
            bool repaired = false;
            const auto repair_until = std::chrono::steady_clock::now() + std::chrono::seconds(45);

            while (std::chrono::steady_clock::now() < repair_until) {
                const auto sample = json({"status"});
                const auto &android = sample.at("deliveries").at("android");

                if (android.value("guest_acknowledged", false) && !android.contains("last_error")) {
                    require(android.at("generation") == 1,
                            "same-boot nonce repair changed generation");
                    repaired = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            require(repaired, "rejected mismatch stop blocked same-generation readonly repair");

            require(std::filesystem::is_directory(root / "slow-startup-receipts") &&
                            !std::filesystem::is_empty(root / "slow-startup-receipts"),
                    "slow startup did not execute delayed guest commands");
            bool framed_fences = false;
            std::set<std::string> categories;

            for (const auto &entry :
                 std::filesystem::directory_iterator(root / "slow-startup-receipts")) {
                const auto sample = read_private_json(entry.path(), 1024);
                require(sample.at("minimum_delay_ms") == 1800 &&
                                sample.at("actual_delay_ms").get<std::int64_t>() >= 1800,
                        "guest delay fixture did not exercise the timeout boundary");
                categories.insert(sample.at("category").get<std::string>());
                framed_fences = framed_fences || sample.value("boot_nonce_fenced", false);
            }

            require(framed_fences,
                    "slow startup did not execute complete boot/nonce fenced transactions");

            for (const char *category :
                 {"getprop", "date", "provider", "dumpsys", "cmd", "state_submission"}) {
                require(categories.contains(category),
                        "slow startup missed representative guest command category");
            }

            const auto stopped =
                    json({"stop"}); // Delay remains active through identity, release and sync.
            std::filesystem::remove(root / "slow-startup-guest.json");
            require(stopped.value("stopped", false) && tcp_port_free(port) &&
                            tcp_port_free(port + 1) && tcp_port_free(port + 30000),
                    "slow startup fixture owned stop incomplete");
            const auto &shutdown = stopped.at("deliveries").at("android").at("shutdown");
            require(shutdown.value("identity_verified", false) &&
                            shutdown.value("guest_sync_completed", false) &&
                            shutdown.value("owned_clean_exit", false) &&
                            stopped.at("deliveries")
                                    .at("android")
                                    .at("quickboot")
                                    .value("checkpoint_saved", false),
                    "slow fixture stop lacked verified sync/owned SDK snapshot/clean exit");
            unsigned launches = 0, terminals = 0, timed_out = 0;
            std::ifstream log(first_directory / "supervisor.log");
            std::string line;

            while (std::getline(log, line)) {
                const auto record = Json::parse(line);

                if (record.value("message", std::string{}) != "startup.action") {
                    continue;
                }

                const auto &fields = record.at("fields");
                const auto stage = fields.at("stage").get<std::string>();
                const std::set<std::string> expected =
                        stage == "launched" ? std::set<std::string>{"stage",
                                                                    "phase",
                                                                    "sdk_pid",
                                                                    "generation",
                                                                    "public_operation",
                                                                    "mutation",
                                                                    "budget_ms"}
                                            : std::set<std::string>{"stage",
                                                                    "phase",
                                                                    "sdk_pid",
                                                                    "generation",
                                                                    "elapsed_ms",
                                                                    "exit_code",
                                                                    "timed_out",
                                                                    "output_bytes",
                                                                    "host_owner_verified",
                                                                    "retained_terminal"};
                require(fields.size() == expected.size(),
                        "startup diagnostics changed bounded scalar schema");

                for (const auto &[key, value] : fields.items()) {
                    require(expected.contains(key) &&
                                    (key == "stage" || value.is_number() || value.is_boolean()),
                            "startup diagnostics leaked arguments or payload");
                }

                if (stage == "launched") {
                    ++launches;
                    require(fields.at("budget_ms").get<std::int64_t>() > 0 &&
                                    fields.at("budget_ms").get<std::int64_t>() <= 60000,
                            "startup stream budget was unbounded");
                } else {
                    require(stage == "stream_terminal" && fields.at("retained_terminal") == true &&
                                    fields.at("host_owner_verified") == true &&
                                    fields.at("output_bytes").get<std::size_t>() <= 512 * 1024,
                            "startup terminal diagnostics lost ownership/extent");
                    ++terminals;

                    if (fields.at("timed_out") == true) {
                        ++timed_out;
                    }
                }
            }

            require(launches > 0 && terminals > 0 && timed_out > 0,
                    "delayed worker/cancellation scalar evidence missing");
            log.close();
            write_private_json(root / ".local/build/slow-startup-result.json",
                               {{"schema", 1},
                                {"synthetic_fixture", true},
                                {"actual_OEM_acceptance", false},
                                {"real_VM", false},
                                {"session_id", first_id},
                                {"functional_assertions_passed", true},
                                {"minimum_guest_delay_ms", 1800},
                                {"state_sequence", wanted},
                                {"before_frames", before_capture.at("frames")},
                                {"after_frames", after_capture.at("frames")},
                                {"sdk_touch_calls", 4},
                                {"identity_verified", shutdown.at("identity_verified")},
                                {"guest_sync_completed", shutdown.at("guest_sync_completed")},
                                {"owned_clean_exit", shutdown.at("owned_clean_exit")},
                                {"checkpoint_saved", stopped.at("deliveries")
                                                             .at("android")
                                                             .at("quickboot")
                                                             .at("checkpoint_saved")},
                                {"worker_launches", launches},
                                {"worker_terminals", terminals},
                                {"worker_timeouts", timed_out}});
            std::cout
                    << "slow Windows startup, synthetic SDK capture/input, state ACK, recovery, snapshot and owned stop passed; no VM; evidence retained at "
                    << root.string() << '\n';

            return 0;
        }

        if (boot_observation) {
            bool ready = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);

            while (std::chrono::steady_clock::now() < deadline) {
                const auto value = json({"status"});
                const auto &android = value["deliveries"]["android"];

                if (android.value("guest_acknowledged", false) && !android.contains("last_error") &&
                    android.value("stage_errors", Json::object()).empty()) {
                    ready = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(ready, "boot observation fixture never established guarded guest readiness");
            json({"stop"});
            require(read_private_json(first_directory / "runtime.json").value("stopped", false) &&
                            tcp_port_free(port) && tcp_port_free(port + 1),
                    "boot observation fixture retained owned listeners");
            std::ifstream file(first_directory / "supervisor.log", std::ios::binary);
            std::string contents{std::istreambuf_iterator<char>(file), {}};
            file.close();
            const auto boot_id = first_id.substr(0, 8) + "-" + first_id.substr(8, 4) + "-" +
                                 first_id.substr(12, 4) + "-" + first_id.substr(16, 4) + "-" +
                                 first_id.substr(20);
            require(contents.find(boot_id) == std::string::npos,
                    "DEBUG observation leaked boot ID");

            for (const char *forbidden :
                 {"qemu.voyemu.token", "content://com.voyemu", "state_json", "raw_oem"}) {
                require(contents.find(forbidden) == std::string::npos,
                        "DEBUG observation leaked arguments or payload");
            }

            const auto calls = read_private_json(root / "content-requests.json");

            for (const auto &call : calls) {
                require(contents.find(call.at("auth").get<std::string>()) == std::string::npos,
                        "DEBUG observation leaked private credential");
            }

            unsigned failed = 0, success = 0, labeled = 0;
            std::istringstream lines(contents);
            std::string line;
#ifndef _WIN32
            constexpr unsigned boot_bytes = 37;
#endif

            while (std::getline(lines, line)) {
                const auto record = Json::parse(line);
                const auto message = record.value("message", std::string{});
                const auto &fields = record.at("fields");
#ifdef _WIN32
                if (message != "startup.action" ||
                    fields.at("phase") != static_cast<unsigned>(WindowsStartupPhase::identity)) {
                    continue;
                }

                if (fields.at("stage") == "launched") {
                    ++labeled;
                    continue;
                }

                require(fields.at("stage") == "stream_terminal" &&
                                fields.at("host_owner_verified") == true &&
                                fields.at("retained_terminal") == true,
                        "framed boot observation lacked retained host proof");
                const std::set<std::string> expected{"stage",
                                                     "phase",
                                                     "sdk_pid",
                                                     "generation",
                                                     "elapsed_ms",
                                                     "exit_code",
                                                     "timed_out",
                                                     "output_bytes",
                                                     "host_owner_verified",
                                                     "retained_terminal"};
                require(fields.size() == expected.size(), "framed boot scalar schema changed");

                for (const auto &[key, value] : fields.items()) {
                    (void)value;
                    require(expected.contains(key), "framed boot observation leaked payload field");
                }

                if (fields.at("exit_code") == 7) {
                    require(fields.at("output_bytes") == 0 && fields.at("timed_out") == false,
                            "failed framed boot outcome changed");
                    ++failed;
                } else {
                    require(fields.at("exit_code") == 0 &&
                                    fields.at("output_bytes").get<std::size_t>() > 38 &&
                                    fields.at("timed_out") == false,
                            "complete framed boot response missing");
                    ++success;
                }
#else
                if (message == "adb.completed" && fields.at("operation") == "shell.boot_id") {
                    ++labeled;
                }

                if (message != "bootstrap.boot_observation") {
                    continue;
                }

                require(record.at("level") == "debug" && record.at("component") == "android",
                        "boot observation category changed");
                const std::set<std::string> expected{"exit_code", "timed_out", "output_bytes",
                                                     "ownership_checked", "owned"};
                require(fields.size() == expected.size(), "boot observation schema changed");

                for (const auto &[key, value] : fields.items()) {
                    (void)value;
                    require(expected.contains(key), "boot observation unapproved field");
                }

                require(fields.at("exit_code").is_number_integer() &&
                                fields.at("output_bytes").is_number_unsigned() &&
                                fields.at("timed_out").is_boolean() &&
                                fields.at("ownership_checked").is_boolean() &&
                                fields.at("owned").is_boolean(),
                        "boot observation scalar types changed");

                if (fields.at("exit_code") == 7) {
                    require(fields.at("output_bytes") == 0 && fields.at("timed_out") == false &&
                                    fields.at("ownership_checked") == false &&
                                    fields.at("owned") == false,
                            "failed boot observation checked ownership");
                    ++failed;
                } else {
                    require(fields.at("exit_code") == 0 &&
                                    fields.at("output_bytes") == boot_bytes &&
                                    fields.at("timed_out") == false &&
                                    fields.at("ownership_checked") == true &&
                                    fields.at("owned") == true,
                            "successful boot observation lost verified ownership");
                    ++success;
                }
#endif
            }

            require(failed == 1 && success > 0 && labeled >= failed + success,
                    "actual guarded boot observation outcomes missing");
            std::filesystem::remove_all(root);
            std::cout
                    << "actual boot observation failure/success DEBUG scalars and payload exclusion passed\n";

            return 0;
        }

#ifdef _WIN32
        if (identity_backpressure) {
            unsigned phase = 1;
            auto count = [&] {
                std::set<std::uint64_t> operations;

                for (const auto &file :
                     std::filesystem::directory_iterator(root / "identity-probe-clients")) {
                    // A producer's .pending file is not a published operation record.
                    if (file.path().extension() == ".json" && file.is_regular_file()) {
                        const auto record = read_private_json(file.path());

                        if (record.at("phase") == phase) {
                            operations.insert(record.at("operation").get<std::uint64_t>());
                        }
                    }
                }

                return operations.size();
            };
            auto rpc = [&](const char *op) {
                const auto begin = std::chrono::steady_clock::now();
                auto value = request(active.at("endpoint").get<std::string>(), {{"op", op}}, 1000);
                require(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(500),
                        "pending identity blocked status");

                return value;
            };
            const auto started = std::chrono::steady_clock::now() + std::chrono::seconds(5);

            while (count() == 0 && std::chrono::steady_clock::now() < started) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            require(count() == 1, "bootstrap identity did not start exactly one retained stream");
            const auto held = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);

            while (std::chrono::steady_clock::now() < held) {
                const auto status = rpc("status");
                require(status.value("stopped", true) == false &&
                                !status["deliveries"]["android"].value("guest_acknowledged", false),
                        "unverified bootstrap became ready");
                require(count() == 1, "bootstrap opened duplicate guest identity streams");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            write_private_json(root / "identity-bootstrap-hold",
                               {{"phase", 1}, {"released", true}});
            bool ready = false;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);

            while (std::chrono::steady_clock::now() < until) {
                auto value = rpc("status");

                if (value["deliveries"]["android"].value("guest_acknowledged", false) &&
                    !value["deliveries"]["android"].contains("last_error") &&
                    value["deliveries"]["android"].value("stage_errors", Json::object()).empty()) {
                    ready = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(ready, "completed bootstrap did not establish normal ready lifecycle");
            {
                std::ofstream stale(root / "stale-identity");
                stale << "wrong nonce\n";
            }
            bool mismatched = false;
            const auto mismatch_deadline = std::chrono::steady_clock::now() +
                                           std::chrono::seconds(10);

            while (std::chrono::steady_clock::now() < mismatch_deadline) {
                auto value = rpc("status");

                if (value["deliveries"]["android"]
                            .value("last_error", std::string{})
                            .starts_with("ANDROID_IDENTITY_MISMATCH:")) {
                    mismatched = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(mismatched, "wrong nonce was not rejected");
            const auto rejected_stop = run({"stop"});
            require(rejected_stop.exit_code != 0 &&
                            rejected_stop.output.find("ANDROID_IDENTITY_MISMATCH") !=
                                    std::string::npos,
                    "mismatched stop was not rejected");
            require(!rpc("status").value("stopped", true),
                    "rejected mismatch stop changed lifecycle");
            std::filesystem::remove(root / "stale-identity");
            ready = false;
            const auto repair_deadline = std::chrono::steady_clock::now() +
                                         std::chrono::seconds(15);

            while (std::chrono::steady_clock::now() < repair_deadline) {
                auto value = rpc("status");

                if (value["deliveries"]["android"].value("guest_acknowledged", false) &&
                    !value["deliveries"]["android"].contains("last_error") &&
                    value["deliveries"]["android"].value("stage_errors", Json::object()).empty()) {
                    ready = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(ready,
                    "same-generation readonly identity repair remained closed after rejected stop");
            phase = 2;
            write_private_json(root / "identity-bootstrap-hold",
                               {{"phase", phase}, {"released", false}});
            const auto recovering = std::chrono::steady_clock::now() + std::chrono::seconds(5);

            while (count() < 1 && std::chrono::steady_clock::now() < recovering) {
                rpc("status");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            require(count() == 1, "steady maintenance did not retain exactly one retained stream");
            const auto quiet = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);

            while (std::chrono::steady_clock::now() < quiet) {
                rpc("status");
                require(count() == 1, "steady maintenance kept flooding guest OPENs");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            write_private_json(root / "adb-gate.json",
                               {{"operation", "name"}, {"milliseconds", 1200}});
            write_private_json(root / "identity-bootstrap-hold",
                               {{"phase", phase}, {"released", true}});
            const auto name_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

            while (!std::filesystem::exists(root / "adb-gate-held") &&
                   std::chrono::steady_clock::now() < name_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            require(std::filesystem::exists(root / "adb-gate-held"),
                    "identity completion AVD check was not held");
            const auto stop_started = std::chrono::steady_clock::now();
            Json stopped;
            std::exception_ptr stop_error;
            std::thread stopping([&] {
                try {
                    stopped = json({"stop"});
                } catch (...) {
                    stop_error = std::current_exception();
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            {
                std::ofstream release(root / "adb-gate-release");
                release << "release held AVD after concurrent stop";
            }
            stopping.join();

            if (stop_error) {
                std::rethrow_exception(stop_error);
            }

            require(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(20),
                    "pending identity blocked lifecycle stop");
            require(stopped.value("stopped", false) &&
                            stopped["deliveries"]["android"]["shutdown"].value("ports_released",
                                                                               false),
                    "pending-stream stop lacked verified owned cleanup");
            const auto final_count = count();
            require(final_count == 1u + (stopped["deliveries"]["android"]["shutdown"].value(
                                                 "identity_verified", false)
                                                 ? 2u
                                                 : 0u),
                    "stop exceeded its fresh identity and framed sync attempts");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            require(count() == final_count, "stopped lifecycle reissued identity probes");
            require(tcp_port_free(port) && tcp_port_free(port + 1),
                    "identity fixture left owned listeners");
            std::filesystem::remove_all(root);
            std::cout
                    << "bootstrap/recovery one-stream backpressure, status responsiveness and owned stop passed; no VM\n";

            return 0;
        }
#endif
        // start acknowledges the supervisor, so the asynchronous SDK receipt may arrive later.
        const auto args_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        while (!std::filesystem::exists(root / "emulator-arguments.json") &&
               std::chrono::steady_clock::now() < args_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        require(std::filesystem::exists(root / "emulator-arguments.json"),
                "SDK argument receipt did not arrive");
        auto sdk_args = read_private_json(root / "emulator-arguments.json");
        Json process_owner;
        const auto process_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        do {
            process_owner = read_private_json(first_directory / "session-owner.json", 16384);

            if (process_owner.value("android_processes", Json::object())
                        .value("phase", std::string{}) == "recorded") {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < process_deadline);

        const auto processes = process_owner.at("android_processes");
        require(processes.at("phase") == "recorded" &&
                        processes.at("sdk_pid").get<std::int64_t>() > 0 &&
                        processes.at("adbd_port") == port + 1 && !processes.contains("adb_pid") &&
                        processes.at("generation").get<unsigned>() == 1,
                "early owned SDK process and adbd port metadata missing before guest readiness");
#ifndef _WIN32
        if (orphan_recovery) {
            const auto supervisor = process_owner.at("creator_pid").get<std::int64_t>();
            const auto start = os::process_start_identity(supervisor);
            const auto arguments = os::process_arguments(supervisor);
            require(start &&
                            std::find(arguments.begin(), arguments.end(),
                                      first_directory.string()) != arguments.end() &&
                            os::process_start_identity(supervisor) == start,
                    "orphan fixture supervisor identity differs");
            require(::kill(static_cast<pid_t>(supervisor), SIGKILL) == 0,
                    "orphan fixture supervisor kill failed");
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);

            while (os::process_start_identity(supervisor) == start &&
                   std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            require(os::process_start_identity(supervisor) != start,
                    "orphan fixture supervisor remains");
            const auto terminal = json({"stop"});
            require(terminal.value("stopped", false) &&
                            !std::filesystem::exists(active.at("endpoint").get<std::string>()) &&
                            !os::process_start_identity(
                                    processes.at("sdk_pid").get<std::int64_t>()),
                    "ordinary orphan stop retained SDK or endpoint");
            require(terminal.at("deliveries").at("qnx") == active.at("deliveries").at("qnx"),
                    "Android-only orphan stop changed an unstarted QNX delivery");

            if (active.at("deliveries").contains("frontend")) {
                require(terminal.at("deliveries").at("frontend").at("status") == "host_terminal" &&
                                terminal.at("deliveries").at("frontend").at("pid") ==
                                        active.at("deliveries").at("frontend").at("pid"),
                        "ordinary orphan terminal retained a running window label or changed its PID");
            }

            const auto public_status = run({"status"});
            require(public_status.exit_code == 0 &&
                            public_status.output.find("host_terminal") == std::string::npos &&
                            public_status.output.find("graphics_running") == std::string::npos,
                    "public stopped status leaked running or internal terminal labels");
            const auto next = json({"start", "--background"});
            require(next.at("session_id") != first_id, "ordinary start reused the stopped epoch");
            const auto next_directory = configuration / qinggan09_profile() / "sessions" /
                                        next.at("session_id").get<std::string>();
            const auto next_owner = read_private_json(next_directory / "session-owner.json");
            const auto next_supervisor = next_owner.at("creator_pid").get<std::int64_t>();
            const auto next_start = os::process_start_identity(next_supervisor);
            const auto gate = root / "descendant-release.json",
                       ready = root / "descendant-ready.json";
            ProcessSpec descendant;
            descendant.executable = std::filesystem::canonical(argv[0]);
            descendant.arguments = {"--session-resource-holder", gate.string(), ready.string()};
            descendant.directory = next_directory;
            descendant.output = next_directory / "android.log";
            auto holder = ChildProcess::launch(descendant);
            const auto ready_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);

            while (!std::filesystem::exists(ready) &&
                   std::chrono::steady_clock::now() < ready_until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            require(std::filesystem::exists(ready) && next_start.has_value(),
                    "ordinary stop descendant fixture not ready");
            std::exception_ptr release_error;
            std::thread release([&] {
                try {
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);

                    while (os::process_start_identity(next_supervisor) == next_start &&
                           std::chrono::steady_clock::now() < until) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }

                    require(os::process_start_identity(next_supervisor) != next_start,
                            "next supervisor did not release its lifetime");
                    write_private_json(gate, {{"release", true}});
                } catch (...) {
                    release_error = std::current_exception();
                }
            });
            Json stopped;
            try {
                stopped = json({"stop"});
            } catch (...) {
                release.join();
                throw;
            }

            release.join();

            if (release_error) {
                std::rethrow_exception(release_error);
            }

            require(stopped.value("stopped", false) && !holder.alive(),
                    "ordinary stop returned before its post-supervisor descendant released resources");
            std::filesystem::remove_all(root);
            std::cout
                    << "ordinary orphan stop, stale endpoint retirement and next start passed; no VM\n";

            return 0;
        }
#endif
#ifdef _WIN32
        const auto trusted_tail = android_sdk_machine_arguments(true);
        require(sdk_args.size() >= trusted_tail.size() &&
                        std::equal(trusted_tail.begin(), trusted_tail.end(),
                                   sdk_args.end() -
                                           static_cast<std::ptrdiff_t>(trusted_tail.size()),
                                   [](const std::string &expected, const Json &actual) {
                                       return actual == Json(expected);
                                   }),
                "Windows ARM64 launch omitted the owned virt/TCG/ICount tail");
#else
        require(std::find(sdk_args.begin(), sdk_args.end(), Json("-qemu")) == sdk_args.end(),
                "Mac launch acquired Windows QEMU board arguments");
#endif

        if (!gpu_mode.empty()) {
            auto option = std::find(sdk_args.begin(), sdk_args.end(), Json("-gpu"));
            require(option != sdk_args.end() && option + 1 != sdk_args.end() &&
                            *(option + 1) == gpu_mode,
                    "prepared GPU mode did not reach the owned SDK process");
        }

        require(std::find(sdk_args.begin(), sdk_args.end(), Json("-fixed-scale")) != sdk_args.end(),
                "native CLI left initial window fitting enabled");
        require(std::find(sdk_args.begin(), sdk_args.end(), Json("-no-boot-anim")) !=
                        sdk_args.end(),
                "guest boot animation would draw the SDK fallback under the window");
        auto path_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        while (!std::filesystem::exists(root / "emulator-paths.json") &&
               std::chrono::steady_clock::now() < path_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        auto sdk_paths = read_private_json(root / "emulator-paths.json", 4096);
        require(sdk_paths["avd_content"] ==
                        (configuration / qinggan09_profile() / "android").string(),
                "SDK content escaped temporary runtime");
        require(sdk_paths["datadir"] == sdk_paths["avd_content"],
                "SDK writable fallback escaped the machine runtime");
        require(sdk_paths["data"] == (machine / "userdata-qemu.img").string() &&
                        sdk_paths["encryption_key"] == (machine / "encryptionkey.img").string() &&
                        std::filesystem::path(sdk_paths["cache"].get<std::string>())
                                        .generic_string()
                                        .find(("/.local/machines/" +
                                               std::string(qinggan09_profile()) + "/android/")) !=
                                std::string::npos,
                "persistent userdata or runtime cache path differs");
        const auto sdk_environment = read_private_json(root / "emulator-environment.json", 4096);

        for (const char *name : {"HOME", "ANDROID_EMULATOR_HOME", "ANDROID_USER_HOME",
                                 "USERPROFILE", "LOCALAPPDATA"}) {
            require(sdk_environment.at(name) == (first_directory / "emulator-home").string(),
                    "SDK home escaped its owned session");
        }

        for (const char *name : {"ANDROID_TMP", "TMPDIR", "TMP", "TEMP", "XDG_RUNTIME_DIR"}) {
            require(sdk_environment.at(name) == (first_directory / "tmp").string(),
                    "SDK temporary directory escaped its owned session");
        }

        require(sha256_file(config_template) == prepared_config_sha256 &&
                        sha256_file(
                                std::filesystem::path(sdk_paths["avd_content"].get<std::string>()) /
                                "config.ini") != prepared_config_sha256,
                "SDK config mutation reached prepared input or fixture did not mutate config");
        require_disks();
#ifndef _WIN32
        auto owner = read_private_json(first_directory / "session-owner.json", 4096);
        auto supervisor_pid = owner.at("creator_pid").get<int>();
        require(owner.at("session_id") == first_id && supervisor_pid > 1,
                "signal regression requires the exact own fixture supervisor");
        require(::kill(supervisor_pid, SIGPIPE) == 0,
                "own supervisor SIGPIPE regression could not deliver signal");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto survived = json({"status"});
        require(survived.at("session_id") == first_id && !survived.value("stopped", true),
                "SIGPIPE killed the native supervisor or replaced its epoch");
#endif

        auto press = request(active.at("endpoint").get<std::string>(), {{"op", "wheel"},
                                                                        {"session_id", first_id},
                                                                        {"id", "menu"},
                                                                        {"phase", "down"},
                                                                        {"owner", "fixture-ui"}});
        require(press.value("accepted", false), "fixture wheel operation rejected");
        auto wheel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        Json received;
        bool acknowledged = false;

        while (std::chrono::steady_clock::now() < wheel_deadline) {
            received = request(
                    active.at("endpoint").get<std::string>(),
                    {{"op", "status"}, {"session_id", first_id}, {"owner", "fixture-ui"}}, 10000);
            auto android = received.at("deliveries").at("android");

            if (android.contains("wheel") && android["wheel"].value("guest_acknowledged", false)) {
                acknowledged = true;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        require(acknowledged, "compiled content fixture never returned a protected receipt");
        require(received["deliveries"]["android"]["guest_acknowledged"] == true &&
                        received["deliveries"]["android"]["fields"] == 579,
                "complete source was not acknowledged independently of wheel receipt");
        const auto route = read_private_json(root / "adbd-routing.json");
        require(route.at("pid") == processes.at("sdk_pid") && route.at("port") == port + 1 &&
                        read_private_json(root / "sdk-adb-trace.json") ==
                                Json{{"empty", true}, {"isolated", true}},
                "Session did not use the retained SDK raw endpoint with ADB_TRACE scrubbed");

        if (owned_adb_death) {
            // Drop only the accepted adbd transport and keep both SDK-owned listeners.
            const auto sdk_pid = route.at("pid").get<std::uint64_t>();
            write_private_json(root / ("adbd-stop-" + std::to_string(sdk_pid)),
                               {{"fixture_owned", true}});
            const auto death_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool rejected = false;

            while (std::chrono::steady_clock::now() < death_deadline) {
                const auto status = json({"status"});
                const auto &android = status.at("deliveries").at("android");

                if (android.value("last_error", std::string{})
                            .starts_with("ANDROID_IDENTITY_NOT_READY:")) {
                    rejected = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(rejected && !tcp_port_free(port) && !tcp_port_free(port + 1),
                    "Session missed the lost adbd transport or lost its SDK listener ownership");
            require(read_private_json(root / "adbd-routing.json") == route,
                    "Session silently attached to another adbd");
            const auto terminal = json({"stop"});
            const auto &android = terminal.at("deliveries").at("android");
            const auto &shutdown = android.at("shutdown");
            require(terminal.value("stopped", false) && shutdown.at("mode") == "host_only" &&
                            !shutdown.at("guest_sync_completed").get<bool>() &&
                            !android.at("quickboot").at("checkpoint_saved").get<bool>() &&
                            tcp_port_free(port) && tcp_port_free(port + 1),
                    "offline adbd stop used guest authority or leaked owned listeners");
            std::filesystem::remove_all(root);
            std::cout << "Lost adbd transport fails closed and retains host-only cleanup\n";

            return 0;
        }

        if (argc == 3) {
            const auto endpoint = active.at("endpoint").get<std::string>();
            auto language = [&](unsigned value) {
                write_private_json(root / "oem-language.json",
                                   {{"output", "Row: 0 value=" + std::to_string(value) + "\n"}});
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);

                while (std::chrono::steady_clock::now() < deadline) {
                    auto current = request(endpoint, {{"op", "status"}}, 10000);
                    const auto &android = current.at("deliveries").at("android");

                    if (current["state"]["raw_oem"]["ivi_languageSet"] == value &&
                        android.value("language", Json::object()).value("value", 99u) == value &&
                        android.value("sequence", 0u) == current.value("revision", 0u)) {
                        return current;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                }

                throw std::runtime_error(
                        "OEM language selection did not synchronize canonical MCU input");
            };
            auto english = language(1);
            const auto revision = english.at("revision");
            std::this_thread::sleep_for(std::chrono::milliseconds(2300));
            require(request(endpoint, {{"op", "status"}})["revision"] == revision,
                    "unchanged OEM language advanced the vehicle revision");
            auto chinese = language(0);
            require(chinese["revision"].get<unsigned>() > revision.get<unsigned>(),
                    "OEM language change lost source revision");
            write_private_json(root / "oem-language.json", {{"output", "Row: 0 value=2\n"}});
            std::this_thread::sleep_for(std::chrono::milliseconds(2300));
            auto unsupported = request(endpoint, {{"op", "status"}});
            require(unsupported["state"]["raw_oem"]["ivi_languageSet"] == 0 &&
                            unsupported["revision"] == chinese["revision"],
                    "unknown OEM language changed input");
            std::filesystem::remove(root / "oem-language.json");
        }

        if (control_latency) {
#ifdef _WIN32
            const auto supervisor_pid = process_owner.at("creator_pid").get<DWORD>();
            std::unique_ptr<void, decltype(&CloseHandle)> supervisor(
                    OpenProcess(SYNCHRONIZE, FALSE, supervisor_pid), &CloseHandle);
            require(bool(supervisor), "open own control fixture supervisor");
#endif

            const auto endpoint = active.at("endpoint").get<std::string>();
            auto rpc = [&](Json value) {
                value["session_id"] = first_id;

                return request(endpoint, value, 3000);
            };
            const auto initial_release = rpc(
                    {{"op", "wheel"}, {"id", "menu"}, {"phase", "up"}, {"owner", "fixture-ui"}});
            require(initial_release.value("accepted", false), "initial release rejected");
            const auto release_sequence = initial_release.at("event_sequence");

            auto arm = [&](const char *operation, int milliseconds = 600) {
                std::filesystem::remove(root / "adb-gate-held");
                std::filesystem::remove(root / "adb-gate-release");
                write_private_json(root / "adb-gate.json",
                                   {{"operation", operation}, {"milliseconds", milliseconds}});
            };
            auto held = [&]() {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);

                while (!std::filesystem::exists(root / "adb-gate-held") &&
                       std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }

                require(std::filesystem::exists(root / "adb-gate-held"),
                        "ADB gate was not reached");
            };
            auto release = [&]() {
                std::ofstream(root / "adb-gate-release") << "release own fixture";
            };
            auto patch = [&](unsigned speed) {
                auto begin = std::chrono::steady_clock::now();
                auto result = rpc({{"op", "patch"}, {"values", {{"speed", speed}}}});
                auto ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - begin)
                                  .count();
                require(result.value("ok", false) && result["state"]["raw_oem"]["speed"] == speed,
                        "concurrent patch lost source");
                std::cout << Json{{"control_reply_ms", ms}, {"target_speed", speed}}.dump() << '\n';
                require(ms < 150, "patch blocked behind bounded ADB: " + std::to_string(ms) + "ms");

                return result["state"]["sequence"].get<unsigned>();
            };
            auto acknowledged = [&](unsigned revision) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);

                while (std::chrono::steady_clock::now() < until) {
                    auto value = rpc({{"op", "status"}});
                    const auto &a = value["deliveries"]["android"];

                    if (a.value("guest_acknowledged", false) &&
                        a.value("sequence", 0u) == revision) {
                        return;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                throw std::runtime_error("current captured state was not acknowledged");
            };
            const auto released_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            bool release_acknowledged = false;

            while (std::chrono::steady_clock::now() < released_until) {
                const auto android = rpc({{"op", "status"}}).at("deliveries").at("android");
                const auto wheel = android.value("wheel", Json::object());

                if (wheel.value("guest_acknowledged", false) &&
                    wheel.at("event_sequence") == release_sequence) {
                    release_acknowledged = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            require(release_acknowledged && read_private_json(root / "held-input.json").empty(),
                    "initial Android release was not acknowledged");

            const auto restarted = rpc({{"op", "cluster_restart"}});
            require(restarted.value("ok", false),
                    "prepared fake QNX did not start: " +
                            restarted.value("error", std::string{"no error detail"}));
            arm("identity");
            held();
            auto revision = patch(43);
            require(rpc({{"op", "status"}})["state"]["sequence"] == revision,
                    "status missed concurrent source");
            auto down = rpc({{"op", "wheel"},
                             {"id", "menu"},
                             {"phase", "down"},
                             {"owner", "fast-fixture"}});
            require(down.value("accepted", false), "concurrent wheel down rejected");
            require(std::filesystem::exists(first_directory / "wheel-command.json"),
                    "accepted QNX wheel phase still waited behind ADB");
            auto issued = read_private_json(first_directory / "wheel-command.json");
            require(issued["sequence"] == down["event_sequence"] && issued["value"] == 1,
                    "QNX command was not published before the input reply");
            require(rpc({{"op", "wheel"},
                         {"id", "menu"},
                         {"phase", "up"},
                         {"owner", "fast-fixture"}})
                            .value("accepted", false),
                    "concurrent wheel up rejected");
            require(read_private_json(first_directory / "wheel-command.json")["sequence"] ==
                            down["event_sequence"],
                    "QNX phase was overwritten before guest ACK");
            release();
            acknowledged(revision);
            arm("state");
            auto older = patch(60);
            held();
            revision = patch(80);
            require(revision > older &&
                            read_private_json(first_directory /
                                              "vehicle-state.json")["raw_oem"]["speed"] == 80,
                    "current QNX source was overwritten");
            release();
            acknowledged(revision);
            require(read_private_json(root / "full-input.json")["raw_oem"]["speed"] == 80,
                    "stale Android publication replaced newer source");
            arm("identity", 1200);
            held();
            std::atomic<bool> stop_returned = false;
            Json terminal;
            std::exception_ptr stop_error;
            std::thread stopping([&] {
                try {
                    terminal = rpc({{"op", "stop"}});
                } catch (...) {
                    stop_error = std::current_exception();
                }

                stop_returned = true;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const bool killed_early = std::filesystem::exists(root / "emu-kill-receipt.json") ||
                                      stop_returned.load();
            release();
            stopping.join();

            if (stop_error) {
                std::rethrow_exception(stop_error);
            }

            require(!killed_early, "lifecycle stop raced an in-flight owned ADB command");
            require(terminal.value("stopped", false) &&
                            terminal["deliveries"]["android"]["shutdown"]["owned_clean_exit"] ==
                                    true,
                    "concurrent lifecycle did not cleanly stop owned Android");
            require(read_private_json(root / "held-input.json").empty(),
                    "concurrent wheel phases left held input");
#ifdef _WIN32
            require(WaitForSingleObject(supervisor.get(), 5000) == WAIT_OBJECT_0,
                    "control fixture supervisor did not release its files");
#endif
            std::filesystem::remove_all(root);
            std::cout
                    << "bounded ADB leaves input/status/wheel responsive; captured state and lifecycle remain guarded\n";

            return 0;
        }

        if (state_flow) {
            std::chrono::steady_clock::duration probe_time{};

            for (unsigned i = 0; i < 16; ++i) {
                const auto probes_started = std::chrono::steady_clock::now();

                for (const auto owned_port : {port, port + 1}) {
                    require(!tcp_port_free(owned_port),
                            "owned fixture listener disappeared during repeated probes");
                }

                probe_time += std::chrono::steady_clock::now() - probes_started;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            const auto probes_ms = std::chrono::duration<double, std::milli>(probe_time).count();
            require(probes_ms < 700, "owned fixture listener backlog delayed repeated probes: " +
                                             std::to_string(probes_ms) + "ms");
            require(json({"status"})["deliveries"]["android"].value("native_listener_ownership",
                                                                    false),
                    "repeated probes lost native SDK listener ownership");
            const int before = read_private_json(root / "readiness-probes.json").value("theme", 0);
            bool polled = false;
            const auto next = std::chrono::steady_clock::now() + std::chrono::seconds(5);

            while (std::chrono::steady_clock::now() < next) {
                if (read_private_json(root / "readiness-probes.json").value("theme", 0) >=
                    before + 2) {
                    polled = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            require(polled, "fixture did not observe a completed ready-state background poll");
            const auto endpoint = active.at("endpoint").get<std::string>();
            const auto started = std::chrono::steady_clock::now();
            auto changed = request(
                    endpoint,
                    {{"op", "patch"}, {"session_id", first_id}, {"values", {{"speed", 43}}}});
            const auto patch_reply_ms = std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - started)
                                                .count();
            const auto revision = changed["state"]["sequence"];
            bool applied = false;
            Json last_status = changed, publication_samples = Json::array();
#ifdef _WIN32
            // Windows uses a framed readiness transaction around the protected receipt. Its
            // three-second maintenance interval is still much longer than this full publication
            // budget.
            constexpr auto delivery_budget = std::chrono::milliseconds(1500);
#else
            constexpr auto delivery_budget = std::chrono::milliseconds(500);
#endif

            while (std::chrono::steady_clock::now() - started < delivery_budget) {
                last_status = request(endpoint, {{"op", "status"}, {"session_id", first_id}});
                const auto &a = last_status["deliveries"]["android"];
                const auto published = read_private_json(root / "full-input.json");
                publication_samples.push_back(
                        {{"elapsed_ms", std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - started)
                                                .count()},
                         {"published_sequence", published.at("sequence")},
                         {"delivery_sequence", a.value("sequence", Json{})},
                         {"guest_acknowledged", a.value("guest_acknowledged", false)}});

                if (a.value("guest_acknowledged", false) && a["sequence"] == revision) {
                    applied = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (!applied) {
                std::cerr << Json{{"fixture_phase", "ready_state_patch"},
                                  {"elapsed_ms", std::chrono::duration<double, std::milli>(
                                                         std::chrono::steady_clock::now() - started)
                                                         .count()},
                                  {"patch_reply_ms", patch_reply_ms},
                                  {"wanted_sequence", revision},
                                  {"publication_samples", publication_samples},
                                  {"android", last_status["deliveries"]["android"]}}
                                     .dump()
                          << '\n';
            }

            require(applied, "ready Android state update missed its delivery budget");
        }

        if (state_restore) {
            auto changed = json({"patch", "--values", Json{{"speed", 43}}.dump()});
            const auto revision = changed["state"]["sequence"];
            bool applied = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

            while (std::chrono::steady_clock::now() < deadline) {
                auto current = json({"status"});

                if (current["deliveries"]["android"].value("guest_acknowledged", false) &&
                    current["deliveries"]["android"]["sequence"] == revision) {
                    applied = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(applied, "restore fixture did not first publish a newer state");

            // Accepted probe connections must not prevent the same owned ports from binding on
            // immediate restore.
            for (const auto owned_port : {port, port + 1}) {
                require(!tcp_port_free(owned_port),
                        "restore fixture lost an owned listener before stop");
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (clock_flow) {
            const auto clock = received["deliveries"]["android"]["clock"];
            require(clock["status"] == "degraded" && clock["synced"] == false &&
                            clock["command_exit_code"] == 255,
                    "persistent offset clock quality was lost");
            const auto count = read_private_json(root / "clock-set-receipt.json").at("count");
            std::this_thread::sleep_for(std::chrono::milliseconds(2100));
            json({"status"});
            require(read_private_json(root / "clock-set-receipt.json")["count"] == count,
                    "ready session repeatedly corrected time");
        }

        auto calls = read_private_json(root / "content-requests.json");
        require(calls.size() >= 3, "provider bootstrap/input/status recovery was not exercised");
        auto private_auth = calls.front().at("auth").get<std::string>();
        require(private_auth.size() == 32 && private_auth != first_id,
                "guest credential equals public epoch id");
        bool bootstrap_seen = false, input_seen = false, status_seen = false;

        for (const auto &call : calls) {
            require(call.at("auth").get<std::string>() == private_auth &&
                            call.at("session_id").get<std::string>() == first_id,
                    "provider credential changed within epoch");
            auto method = call.at("method").get<std::string>();
            bootstrap_seen |= method == "bootstrap";
            input_seen |= method == "input";
            status_seen |= method == "status";
        }

        require(bootstrap_seen && input_seen && status_seen, "provider recovery methods missing");
        require(first.dump().find(private_auth) == std::string::npos &&
                        active.dump().find(private_auth) == std::string::npos &&
                        received.dump().find(private_auth) == std::string::npos,
                "private credential leaked in public replies or receipt");

        for (const char *name : {"runtime.json", "session-owner.json", "supervisor.log"}) {
            require(std::filesystem::is_regular_file(first_directory / name),
                    "public evidence file missing from auth check");
            std::ifstream input(first_directory / name, std::ios::binary);
            require(input.good(), "public evidence file unreadable in auth check");
            std::string contents{std::istreambuf_iterator<char>(input), {}};
            require(contents.find(private_auth) == std::string::npos,
                    "private credential leaked into persisted public evidence or log");
        }

        const auto before_failed_stop = json({"status"});
        require(before_failed_stop.at("android_pid").get<int>() > 0,
                "failed-sync fixture has no owned live guest");
        { std::ofstream(root / "refuse-sync") << "reject only this fixture sync"; }
        auto refused = run({"restart"}, 90000);
        require(refused.exit_code == 4 && refused.output.starts_with("Voyemu:"),
                "restart ignored failed owned stop");
        auto retained = json({"status"});
        require(retained["session_id"] == first_id && !retained.value("stopped", true),
                "failed stop replaced or lost active session");
        require(retained.at("android_pid") == before_failed_stop.at("android_pid") &&
                        retained["deliveries"]["frontend"]["status"] == "stopped",
                "failed sync hid its deliberate frontend close or lost the retained guest");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        require(json({"status"})["session_id"] == first_id,
                "deliberate frontend close asynchronously stopped the retained control service");
        std::filesystem::remove(root / "refuse-sync");
        auto stopped = json({"stop"});
        require(stopped["stopped"] == true, "stop did not report terminal session");
        require(tcp_port_free(port) && tcp_port_free(port + 1),
                "stop returned while SDK child listeners remained");
        auto kill = read_private_json(root / "emu-kill-receipt.json", 4096);
        require(kill.at("session_id").get<std::string>() == first_id &&
                        kill.at("avd").get<std::string>() == "VoyemuFixture",
                "SDK kill was not scoped to exact identity");
        require(stopped["deliveries"]["android"]["shutdown"]["identity_verified"] == true &&
                        stopped["deliveries"]["android"]["shutdown"]["ports_released"] == true &&
                        stopped["deliveries"]["android"]["shutdown"]["guest_sync_completed"] ==
                                true,
                "public stop lacked SDK termination proof");
        require(sha256_file(config_template) == prepared_config_sha256,
                "stop changed immutable prepared template");
        require_disks();
        const auto &stopped_android = stopped.at("deliveries").at("android");
        const auto &stopped_checkpoint = stopped_android.at("quickboot");
        require(stopped_checkpoint.at("checkpoint_saved") == quickboot,
                "unexpected checkpoint result: " +
                        stopped_checkpoint.value("save_status", "unknown") +
                        "; shutdown=" + stopped_android.at("shutdown").dump());
        require(read_private_json(root / "held-input.json").empty(),
                "stop left guest input held before checkpoint");

        if (quickboot) {
            require(read_private_json(root / ".local/machines" / qinggan09_profile() / "android" /
                                      "voyemu-quickboot.json")["state"] == "clean",
                    "clean checkpoint not persisted after owned process exit");
        }

#ifndef _WIN32
        if (unowned) {
            write_private_json(root / "clock-state.json", {{"offset_ms", -3600000}});
            require(!std::filesystem::exists(root / "clock-set-receipt.json"),
                    "fresh unowned fixture unexpectedly corrected time");
            std::filesystem::remove(root / "direct-engine");
            auto rejected = json({"start"});
            auto rejected_id = rejected.at("session_id").get<std::string>();
            auto rejected_directory = configuration / qinggan09_profile() / "sessions" /
                                      rejected_id;
            auto rejection_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool rejected_nonce = false;

            while (std::chrono::steady_clock::now() < rejection_deadline) {
                auto status = request(rejected.at("endpoint").get<std::string>(),
                                      {{"op", "status"}, {"session_id", rejected_id}}, 10000);
                const auto &android = status["deliveries"]["android"];

                if ((android.value("last_error", std::string{}) ==
                             "ANDROID_IDENTITY_NOT_READY: retained SDK listeners unavailable" ||
                     android.value("last_error", std::string{})
                                     .find("cached boot lacks current native SDK listener ownership") !=
                             std::string::npos ||
                     android.value("last_error", std::string{}).find("SDK ownership not ready") !=
                             std::string::npos)) {
                    require(!android.contains("wheel_bootstrap") &&
                                    !android.value("guest_acknowledged", false),
                            "unowned cached nonce reached protected control session");
                    rejected_nonce = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(rejected_nonce, "cached nonce was accepted after retained SDK process exited");
            require(!std::filesystem::exists(root / "clock-set-receipt.json"),
                    "unowned cached SDK reached clock mutation");
            require(read_private_json(root / "provider-session.json")["session_id"] == first_id,
                    "unowned cache replaced guest credential");
            { std::ofstream(root / "window-exit") << "terminate only this compiled fixture leaf"; }
            auto terminal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            bool terminal = false;

            while (std::chrono::steady_clock::now() < terminal_deadline) {
                if (std::filesystem::exists(rejected_directory / "runtime.json") &&
                    read_private_json(rejected_directory / "runtime.json")
                            .value("stopped", false)) {
                    terminal = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(terminal && tcp_port_free(port) && tcp_port_free(port + 1),
                    "unowned fixture leaf did not terminate safely");
            require(read_private_json(root / ".local/machines" / qinggan09_profile() / "android" /
                                      "voyemu-quickboot.json")["state"] == "in_use",
                    "unowned restore produced a clean checkpoint");
            std::filesystem::remove_all(root);
            std::cout << "cached nonce rejected without current retained SDK listener ownership\n";

            return 0;
        }
#endif
        const auto captured_vehicle_state =
                state_restore ? read_private_json(root / "full-input.json") : Json::object();

        if (state_restore) {
            require(captured_vehicle_state["raw_oem"]["speed"] == 43,
                    "state-restore fixture did not capture a distinct driving state");
            auto canonical =
                    native_current_active_directory(
                            root, qinggan09_profile(),
                            voyemu::machine_identity(qinggan09_profile(), std::string_view{}))
                            .parent_path() /
                    "state/vehicle-state.json";
            write_private_json(canonical, initial_state(VehicleCatalog::qinggan09));
        }

        std::uint32_t latest_sequence = 0;

        if (latency) {
            auto canonical =
                    native_current_active_directory(
                            root, qinggan09_profile(),
                            voyemu::machine_identity(qinggan09_profile(), std::string_view{}))
                            .parent_path() /
                    "state/vehicle-state.json";
            auto current = read_private_json(first_directory / "vehicle-state.json");
            write_private_json(root / "stale-full-input.json", current);
            latest_sequence = current.at("sequence").get<std::uint32_t>() + 1;
            current["sequence"] = latest_sequence;
            current["raw_oem"]["speed"] = 43;
            current["raw_oem"]["speed10x"] = 430;
            validate_state(current);
            write_private_json(canonical, current);

            for (const char *name :
                 {"identity-hold", "stale-identity", "display-hold", "stale-state-receipt",
                  "canvas-hold", "reject-duplicate-activation"}) {
                std::ofstream(root / name) << "bounded readiness fixture gate";
            }

            write_private_json(root / "readiness-probes.json", Json::object());
        }

        if (clock_flow) {
            write_private_json(root / "clock-state.json", {{"offset_ms", -3600000}});
        }

        auto second = json(latency ? std::vector<std::string>{"start", "--resume"}
                                   : std::vector<std::string>{"start"});
        auto second_id = second.at("session_id").get<std::string>();
        require(second_id != first_id, "immediate restart reused stopped epoch");

        if (latency) {
#ifdef _WIN32
            // A retained sequence crosses several owned ADB fences before reaching the
            // state provider. It must still complete within one maintenance interval.
            constexpr int readiness_ms = 3000;
            constexpr int recovery_ms = 10000;
            constexpr int debug_readiness_ms = 10000;
            constexpr auto pending_receipt = "submitted_unverified";
            constexpr int boot_before_receipt = 0;
#else
            constexpr int readiness_ms = 1200;
            constexpr int recovery_ms = 5000;
            constexpr int debug_readiness_ms = 5000;
            constexpr auto pending_receipt = "awaiting_state_receipt";
            constexpr int boot_before_receipt = 1;
#endif
            auto status = [&] {
                return request(second.at("endpoint").get<std::string>(),
                               {{"op", "status"}, {"session_id", second_id}}, 10000);
            };
            // Debug timing coverage checks functional readiness; the separate nondebug latency
            // scenario retains its strict budgets.
            auto wait = [&](const auto &predicate, int milliseconds) {
                if (timings && milliseconds < debug_readiness_ms) {
                    milliseconds = debug_readiness_ms;
                }

                const auto started = std::chrono::steady_clock::now();
                auto deadline = started + std::chrono::milliseconds(milliseconds);
                Json last_status, publication_samples = Json::array();

                while (std::chrono::steady_clock::now() < deadline) {
                    last_status = status();
                    const auto published = read_private_json(root / "full-input.json");
                    const auto &android = last_status["deliveries"]["android"];
                    publication_samples.push_back(
                            {{"elapsed_ms", std::chrono::duration<double, std::milli>(
                                                    std::chrono::steady_clock::now() - started)
                                                    .count()},
                             {"published_sequence", published.at("sequence")},
                             {"delivery_sequence", android.value("sequence", Json{})},
                             {"bootstrap", android.contains("wheel_bootstrap")},
                             {"last_error", android.value("last_error", std::string{})}});

                    if (predicate(last_status)) {
                        return true;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }

                Json probes = Json::object();

                for (const char *name :
                     {"readiness-probes.json", "bootstrap-probes.json", "state-probes.json"}) {
                    if (std::filesystem::exists(root / name)) {
                        probes[name] = read_private_json(root / name);
                    }
                }

                std::cerr << Json{{"fixture_phase", "quickboot_readiness_wait"},
                                  {"budget_ms", milliseconds},
                                  {"elapsed_ms", std::chrono::duration<double, std::milli>(
                                                         std::chrono::steady_clock::now() - started)
                                                         .count()},
                                  {"publication_samples", publication_samples},
                                  {"probes", probes},
                                  {"last_status", last_status}}
                                     .dump()
                          << '\n';

                return false;
            };
            require(wait(
                            [](const Json &value) {
                                return value["deliveries"]["android"]
                                        .value("last_error", std::string{})
                                        .starts_with("ANDROID_IDENTITY_NOT_READY");
                            },
                            10000),
                    "delayed identity did not exercise a failed readiness pass");
            std::filesystem::remove(root / "identity-hold");
            require(wait(
                            [](const Json &value) {
                                const auto &android = value["deliveries"]["android"];

                                return android.value("last_error", std::string{})
                                               .starts_with("ANDROID_IDENTITY_MISMATCH") &&
                                       !android.contains("wheel_bootstrap") &&
                                       !android.value("guest_acknowledged", false);
                            }
#ifdef _WIN32
                            ,
                            5000
#else
                            ,
                            1200
#endif
                            ),
                    "startup retry accepted stale identity or exceeded bounded recovery");
            require(read_private_json(root / "provider-session.json")["session_id"] == first_id,
                    "stale identity replaced the protected control credential");
            write_private_json(root / "readiness-probes.json", Json::object());
            std::filesystem::remove(root / "stale-identity");
            // Receipt rejection is published immediately; aggregated stage errors retain the prior
            // pass until display work completes.
            require(wait(
                            [&](const Json &value) {
                                const auto &android = value["deliveries"]["android"];

                                return android.value("wheel_bootstrap", Json::object())
                                               .value("guest_acknowledged", false) &&
                                       android.value("status", std::string{}) == pending_receipt &&
                                       android.value("sequence", 0u) == latest_sequence &&
                                       !android.value("guest_acknowledged", false) &&
                                       read_private_json(root / "readiness-probes.json")
                                                       .value("boot", 0) == boot_before_receipt;
                            },
                            readiness_ms),
                    "fresh bootstrap skipped or stale full-state publication was accepted");
            auto bootstrap_probes = read_private_json(root / "bootstrap-probes.json");
#ifdef _WIN32
            constexpr int expected_bootstrap_identity_pairs = 9;
#else
            // Outer identity + two boot-observation identities + two already-root bootstrap fences
            // + four clock fences.
            constexpr int expected_bootstrap_identity_pairs = 1 + 2 + 2 + 4;
#endif
            require(bootstrap_probes.value("token", 0) == expected_bootstrap_identity_pairs &&
                            bootstrap_probes.value("name", 0) == expected_bootstrap_identity_pairs,
                    "API2 bootstrap ownership/boot fences exceeded the bounded observation count");
            auto state_probes = read_private_json(root / "state-probes.json");
            require(state_probes.value("boot", 0) == 0 &&
                            read_private_json(root / "readiness-probes.json").value("boot", 0) ==
                                    boot_before_receipt,
                    "vehicle publication or secondary display boot observation occurred out of order");
            std::filesystem::remove(root / "stale-state-receipt");
            require(wait(
                            [&](const Json &value) {
                                const auto &android = value["deliveries"]["android"];

#ifdef _WIN32
                                return android.value("status", std::string{}) ==
                                               "protected_state_verified" &&
#else
                                return android.value("guest_acknowledged", false) &&
#endif
                                       android.value("sequence", 0u) == latest_sequence &&
                                       !android.contains("displays");
                            },
                            readiness_ms),
                    "current full579 state did not recover while display readiness remained pending");
            require(wait(
                            [&](const Json &) {
                                return read_private_json(root / "readiness-probes.json")
                                               .value("display", 0) >= 3;
                            },
                            readiness_ms),
                    "secondary display did not receive fast readiness retries");
#ifndef _WIN32
            require(read_private_json(root / "readiness-probes.json").value("theme", 0) == 2,
                    "fast readiness repeated steady-state Binder theme polling");
#endif
            require(read_private_json(root / "readiness-probes.json").value("wake", 0) == 1,
                    "pending readiness repeated successful display activation");
            std::filesystem::remove(root / "display-hold");
#ifdef __APPLE__
            require(wait(
                            [&](const Json &value) {
                                return read_private_json(root / "readiness-probes.json")
                                                       .value("resize", 0) == 2 &&
                                       !value["deliveries"]["android"].contains("displays");
                            },
                            readiness_ms),
                    "late or invalid canvas bypassed readiness guard");
            write_private_json(
                    configuration / qinggan09_profile() / "sessions" / second_id /
                            "media-window.json",
                    {{"schema", 1},
                     {"source", "own-window-instrumentation"},
                     {"bundle_identifier", "com.voyemu.emulator"},
                     {"backing_scale", 2},
                     {"content_points", Json::array({1920, 360})},
                     {"content_subviews",
                      Json::array({{{"class", "EmuGLViewWithMetal"},
                                    {"frame_points", Json::array({0, 0, 1920, 360})},
                                    {"bounds_points", Json::array({0, 0, 1920, 360})}}})}});
#endif
            require(wait(
                            [&](const Json &value) {
                                const auto &android = value["deliveries"]["android"];

                                return android.value("guest_acknowledged", false) &&
                                       android.value("sequence", 0u) == latest_sequence &&
                                       android.value("displays", Json::object())
                                                       .value("status", std::string{}) ==
                                               "both_guest_displays_ready";
                            },
                            readiness_ms),
                    "secondary display readiness retained the two-second delay");
            require(read_private_json(root / "readiness-probes.json").value("wake", 0) == 1,
                    "late canvas triggered duplicate activation before observation");
            const auto ready_probes = read_private_json(root / "readiness-probes.json");
            require(ready_probes.value("boot", 0) == 1,
                    "boot readiness was not cached within the new epoch");
            const auto source_patch = request(
                    second.at("endpoint").get<std::string>(),
                    {{"op", "patch"}, {"session_id", second_id}, {"values", {{"speed", 44}}}},
                    10000);
            latest_sequence = source_patch.at("state").at("sequence").get<std::uint32_t>();
            require(wait(
                            [&](const Json &value) {
                                const auto &android = value["deliveries"]["android"];

                                return android.value("guest_acknowledged", false) &&
                                       android.value("sequence", 0u) == latest_sequence;
                            },
                            readiness_ms),
                    "display watchdog delayed current vehicle state");
            const auto quiet_probes = read_private_json(root / "readiness-probes.json");
#ifdef _WIN32
            require(quiet_probes.value("wake", 0) == ready_probes.value("wake", 0),
                    "state delivery repeated successful display activation");
#else
            require(quiet_probes.value("display", 0) == ready_probes.value("display", 0) &&
                            quiet_probes.value("window", 0) == ready_probes.value("window", 0),
                    "state delivery repeated healthy display observations");
#endif
            std::ofstream(root / "allow-recovery") << "one new activation generation";
            std::ofstream(root / "repair-required")
                    << "confirmed secondary display lost after readiness";
            require(wait(
                            [&](const Json &value) {
                                const auto &android = value["deliveries"]["android"];

                                return read_private_json(root / "readiness-probes.json")
                                                       .value("wake", 0) == 2 &&
                                       android.value("displays", Json::object())
                                                       .value("status", std::string{}) ==
                                               "both_guest_displays_ready" &&
                                       android.value("guest_acknowledged", false) &&
                                       android.value("sequence", 0u) == latest_sequence;
                            },
                            recovery_ms),
                    "ready to lost display failed owned rate-limited repair");
            const auto times =
                    read_private_json(root / "readiness-probes.json").at("wake_times_ms");
            require(times.size() == 2 &&
                            times[1].get<std::int64_t>() - times[0].get<std::int64_t>() >= 1900,
                    "display activation recovery ignored the two-second rate limit");
            std::filesystem::remove(root / "reject-duplicate-activation");
            const int bar_starts =
                    read_private_json(root / "readiness-probes.json").value("systemui_start", 0);
            std::ofstream(root / "systemui-repair-required")
                    << "secondary remains present but OEM bars disappeared";
            require(wait(
                            [&](const Json &value) {
                                const auto &android = value["deliveries"]["android"];

                                return read_private_json(root / "readiness-probes.json")
                                                       .value("systemui_start", 0) > bar_starts &&
                                       android.value("systemui", Json::object())
                                                       .value("status", std::string{}) ==
                                               "both_OEM_bars_visible" &&
                                       android.value("displays", Json::object())
                                                       .value("status", std::string{}) ==
                                               "both_guest_displays_ready";
                            },
                            recovery_ms + 1000),
                    "bounded display watchdog did not recover lost OEM SystemUI");
#ifndef _WIN32
            require(read_private_json(root / "readiness-probes.json").value("boot", 0) == 1,
                    "recovery re-probed immutable epoch boot completion");
#endif

            for (const char *name :
                 {"canvas-hold", "reject-duplicate-activation", "allow-recovery"}) {
                std::filesystem::remove(root / name);
            }

            std::cout
                    << "delayed identity/state/display/canvas retries and ready-lost-repair preserve guards and bounded activation\n";
        }

        auto directory = configuration / qinggan09_profile() / "sessions" / second_id;
        require_disks();
        auto guest_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool own_guest_ready = false;

        while (std::chrono::steady_clock::now() < guest_deadline) {
            auto identity = read_private_json(root / "identity.json", 4096);

            if (identity.value("alive", false) &&
                identity.at("token").get<std::string>() == (quickboot ? first_id : second_id) &&
                !tcp_port_free(port) && !tcp_port_free(port + 1)) {
                auto status = request(second.at("endpoint").get<std::string>(),
                                      {{"op", "status"}, {"session_id", second_id}}, 10000);

                if (status["deliveries"]["android"].value("status", std::string{}) ==
                    "guest_acknowledged") {
                    own_guest_ready = true;
                    break;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        require(own_guest_ready, "second SDK fixture never became an identified live guest");
        require_disks();

        if (state_restore) {
            const auto expected =
                    select_vehicle_startup(VehicleStartup::fresh, VehicleCatalog::qinggan09,
                                           embedded_profile(qinggan09_profile())
                                                   .at("definition")
                                                   .value("initial_state", Json::object()),
                                           nullptr);
            auto current = json({"status"});
            const auto &source = current["state"];
            require(source["sequence"] ==
                                    captured_vehicle_state["sequence"].get<std::uint32_t>() + 1 &&
                            source["raw_oem"] == expected["raw_oem"],
                    "restored guest values replaced fresh profile authority or revision did not advance above captured guest state");
            require(read_private_json(root / "full-input.json") == source,
                    "restored guest did not receive current full host input");
            require(read_private_json(configuration / qinggan09_profile() / "sessions" / second_id /
                                      "vehicle-state.json") == source,
                    "reconciled source revision was not persisted");
        }

        if (clock_flow) {
            auto current = json({"status"});
            require(current["deliveries"]["android"]["clock"]["status"] == "degraded" &&
                            current["deliveries"]["android"]["clock"]["synced"] == false,
                    "restored offset clock quality was lost");
        }

        if (quickboot) {
            auto resumed = request(second.at("endpoint").get<std::string>(),
                                   {{"op", "status"}, {"session_id", second_id}}, 10000);
            require(resumed["deliveries"]["android"]["quickboot"]["mode"] == "restored" &&
                            resumed["deliveries"]["android"]["native_listener_ownership"] == true,
                    "cached nonce accepted without current owned SDK listener proof");
            auto provider = read_private_json(root / "provider-session.json", 4096);
            require(provider["session_id"] == second_id && provider["auth"] != private_auth,
                    "restore reused logical control credential/session");
            require(resumed["deliveries"]["android"]["wheel_bootstrap"]["guest_acknowledged"] ==
                                    true &&
                            resumed["deliveries"]["android"]["guest_acknowledged"] == true,
                    "restore lacks fresh bootstrap/full vehicle state");
            require(read_private_json(root / ".local/machines" / qinggan09_profile() / "android" /
                                      "voyemu-quickboot.json")["state"] == "in_use",
                    "restore failed to invalidate checkpoint before guest launch");
            auto current_args = read_private_json(root / "emulator-arguments.json");
            require(std::find(current_args.begin(), current_args.end(), Json("-snapshot")) !=
                            current_args.end(),
                    "restart did not request named checkpoint");
        }

        require(read_private_json(root / "emulator-paths.json", 4096)["avd_content"] ==
                        (configuration / qinggan09_profile() / "android").string(),
                "restart content escaped temporary runtime");
        const auto restarted_environment = read_private_json(root / "emulator-environment.json",
                                                             4096);

        for (const char *name : {"HOME", "ANDROID_EMULATOR_HOME", "ANDROID_USER_HOME",
                                 "USERPROFILE", "LOCALAPPDATA"}) {
            require(restarted_environment.at(name) == (directory / "emulator-home").string(),
                    "SDK home escaped its owned session");
        }

        for (const char *name : {"ANDROID_TMP", "TMPDIR", "TMP", "TEMP", "XDG_RUNTIME_DIR"}) {
            require(restarted_environment.at(name) == (directory / "tmp").string(),
                    "SDK temporary directory escaped its owned session");
        }

        require(sha256_file(config_template) == prepared_config_sha256 &&
                        sha256_file(
                                std::filesystem::path(sdk_paths["avd_content"].get<std::string>()) /
                                "config.ini") != prepared_config_sha256,
                "restart lost independent SDK config persistence");

#ifndef _WIN32
        auto leaf_status = request(second.at("endpoint").get<std::string>(),
                                   {{"op", "status"}, {"session_id", second_id}}, 10000);
        require(leaf_status["deliveries"]["android"]["wrapper"]["alive"] == quickboot &&
                        leaf_status["stopped"] == false,
                "launcher exit was confused with live SDK guest exit");
#endif
        { std::ofstream(root / "window-exit") << "exit only own compiled fixture\n"; }
        bool terminal = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(directory / "runtime.json")) {
                auto record = read_private_json(directory / "runtime.json");

                if (record.value("stopped", false)) {
                    require(record["deliveries"]["android"]["status"] == "exited",
                            "owned Android exit became a booting zombie");
                    terminal = true;
                    break;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        require(terminal, "supervisor retained stopped:false after fixture window exit");
        auto record = read_private_json(directory / "runtime.json");
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool removed = false;

        while (std::chrono::steady_clock::now() < deadline) {
            try {
                request(record.at("endpoint").get<std::string>(),
                        {{"op", "status"}, {"session_id", second_id}}, 100);
            } catch (const std::exception &) {
                removed = true;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        require(removed, "terminal supervisor kept its control endpoint");
        require(read_private_json(root / ".local/machines" / qinggan09_profile() / "android" /
                                  "voyemu-quickboot.json")["state"] == "in_use",
                "external window exit incorrectly published clean checkpoint");

        if (quickboot) {
            std::filesystem::remove(root / "window-exit");
            auto third = json({"start"});
            auto third_id = third.at("session_id").get<std::string>();
            auto cold_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool cold_ready = false;

            while (std::chrono::steady_clock::now() < cold_deadline) {
                auto status = request(third.at("endpoint").get<std::string>(),
                                      {{"op", "status"}, {"session_id", third_id}}, 10000);

                if (status["deliveries"]["android"].value("status", std::string{}) ==
                    "guest_acknowledged") {
                    require(status["deliveries"]["android"]["quickboot"]["mode"] == "cold",
                            "unclean SDK exit reused old snapshot");
                    cold_ready = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(cold_ready && read_private_json(root / "identity.json")["token"] == third_id,
                    "unclean checkpoint failed safe cold recovery");
            auto fourth = json({"restart"});
            auto fourth_id = fourth.at("session_id").get<std::string>();
            require(fourth_id != third_id, "restart command reused control epoch");
            auto restart_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool restart_ready = false;

            while (std::chrono::steady_clock::now() < restart_deadline) {
                auto status = request(fourth.at("endpoint").get<std::string>(),
                                      {{"op", "status"}, {"session_id", fourth_id}}, 10000);

                if (status["deliveries"]["android"].value("status", std::string{}) ==
                    "guest_acknowledged") {
                    require(status["deliveries"]["android"]["quickboot"]["mode"] == "restored",
                            "restart command failed to use clean saved checkpoint");
                    restart_ready = true;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            require(restart_ready &&
                            read_private_json(root / "provider-session.json")["session_id"] ==
                                    fourth_id,
                    "restart command lacks fresh protected session");
            json({"stop"});
        }

        if (timings) {
            const std::set<std::string> operations{
                    "shell.boot_id",    "shell.getprop", "shell.content", "shell.dumpsys",
                    "shell.cmd",        "shell.am",      "shell.service", "shell.sync",
                    "shell.other",      "push",          "emu.avd",       "emu.kill",
                    "emu.multidisplay", "emu.other",     "unknown"};
            const std::set<std::string> keys{"operation", "elapsed_ms", "timeout_ms",
                                             "status",    "exit_code",  "output_bytes"};
            const std::set<std::string> statuses{"ok", "failed", "timeout", "exception"};
            std::set<std::string> observed;
            unsigned records = 0, failed = 0;
#ifdef _WIN32
            unsigned retained_records = 0;
#endif
            const auto calls = read_private_json(root / "content-requests.json");

            for (const auto &entry : std::filesystem::directory_iterator(
                         configuration / qinggan09_profile() / "sessions")) {
                const auto path = entry.path() / "supervisor.log";
                std::ifstream file(path, std::ios::binary);
                std::string contents{std::istreambuf_iterator<char>(file), {}};

                for (const auto &call : calls) {
                    require(contents.find(call.at("auth").get<std::string>()) == std::string::npos,
                            "DEBUG timing log leaked a private provider credential");
                }

                const auto epoch = entry.path().filename().string();

                if (epoch.size() == 32) {
                    const auto boot_id = epoch.substr(0, 8) + "-" + epoch.substr(8, 4) + "-" +
                                         epoch.substr(12, 4) + "-" + epoch.substr(16, 4) + "-" +
                                         epoch.substr(20);
                    require(contents.find(boot_id) == std::string::npos,
                            "DEBUG boot observation leaked boot ID payload");
                }

                for (const char *text :
                     {"state_json", "raw_oem", "qemu.voyemu.token", "content://com.voyemu"}) {
                    require(contents.find(text) == std::string::npos,
                            "DEBUG timing log leaked arguments, state or provider data");
                }

                std::istringstream lines(contents);
                std::string line;

                while (std::getline(lines, line)) {
                    const auto record = Json::parse(line);

#ifdef _WIN32
                    if (record.value("message", std::string{}) == "startup.action") {
                        const auto &fields = record.at("fields");

                        if (fields.value("stage", std::string{}) == "stream_terminal") {
                            const std::set<std::string> retained_keys{"stage",
                                                                      "phase",
                                                                      "sdk_pid",
                                                                      "generation",
                                                                      "elapsed_ms",
                                                                      "exit_code",
                                                                      "timed_out",
                                                                      "output_bytes",
                                                                      "host_owner_verified",
                                                                      "retained_terminal"};
                            require(record.value("level", std::string{}) == "debug" &&
                                            record.value("component", std::string{}) == "android" &&
                                            fields.size() == retained_keys.size(),
                                    "retained ADB timing schema is invalid");

                            for (const auto &[key, value] : fields.items()) {
                                (void)value;
                                require(retained_keys.contains(key),
                                        "retained ADB timing contains unapproved data");
                            }

                            require(fields.at("elapsed_ms").get<std::int64_t>() >= 0 &&
                                            fields.at("output_bytes").get<std::size_t>() <=
                                                    1024 * 1024 &&
                                            fields.value("host_owner_verified", false) &&
                                            fields.value("retained_terminal", false),
                                    "retained ADB timing lacks bounded terminal ownership");
                            ++retained_records;
                        }
                    }
#endif

                    if (record.value("message", std::string{}) != "adb.completed") {
                        continue;
                    }

                    require(record.value("level", std::string{}) == "debug" &&
                                    record.value("component", std::string{}) == "android",
                            "ADB timings used a non-DEBUG record");
                    const auto &fields = record.at("fields");
                    require(fields.is_object() && fields.size() == keys.size(),
                            "ADB timing schema contains unapproved fields");

                    for (const auto &[key, value] : fields.items()) {
                        (void)value;
                        require(keys.contains(key), "ADB timing schema contains unapproved data");
                    }

                    const auto operation = fields.at("operation").get<std::string>(),
                               status = fields.at("status").get<std::string>();
                    require(operations.contains(operation) && statuses.contains(status),
                            "ADB timing category or status is not allowlisted");

                    for (const char *key :
                         {"elapsed_ms", "timeout_ms", "exit_code", "output_bytes"}) {
                        require(fields.at(key).is_number_integer(),
                                "ADB timing metric is not numeric");
                    }

                    require(fields.at("elapsed_ms").get<std::int64_t>() >= 0 &&
                                    fields.at("timeout_ms").get<int>() > 0 &&
                                    fields.at("output_bytes").get<std::size_t>() <= 1024 * 1024,
                            "ADB timing metric escaped capture bounds");
                    observed.insert(operation);
                    ++records;

                    if (status == "failed") {
                        ++failed;
                    }
                }
            }

#ifdef _WIN32
            require(retained_records > 20,
                    "missing terminal timings for retained Windows ADB actions");

            for (const char *operation : {"emu.avd", "emu.kill"}) {
#else
            for (const char *operation :
                 {"shell.getprop", "shell.content", "shell.dumpsys", "shell.cmd", "shell.am",
                  "shell.service", "emu.avd", "emu.multidisplay", "emu.kill"}) {
#endif
                require(observed.contains(operation),
                        "compiled fixture did not expose required allowlisted ADB timing");
            }

#ifdef _WIN32
            require(records > 0, "ADB timings did not include host control calls");
#else
            require(records > 20 && failed > 0,
                    "ADB timings did not cover successful and failed guarded calls");
#endif
            std::cout << Json{{"adb_timing_records", records},
                              {"failed_guarded_calls", failed},
                              {"allowed_operations_observed", observed},
                              {"secret_and_state_fields_absent", true}}
                                 .dump()
                      << '\n';
        }

        if (argc == 3) {
            std::filesystem::remove(root / "window-exit");
            require(sha256_file(saved_scenario_path) == saved_scenario_sha,
                    "working changes overwrote saved scenario");
            auto resumed = json({"start", "--resume"});
            auto current = json({"status"});
            require(current["state"]["raw_oem"] == saved_scenario["raw_oem"],
                    "explicit resume did not preserve saved driving scenario");
            json({"stop"});
            require(sha256_file(saved_scenario_path) == saved_scenario_sha,
                    "resume changed saved scenario");
        }

        std::filesystem::remove_all(root);
        std::cout
                << "real CLI default/read-only/start/stop/restart/window-exit with owned compiled fixtures passed\n";

        return 0;
    } catch (const std::exception &error) {
        try {
            if (identity_backpressure &&
                std::filesystem::exists(root / "identity-bootstrap-hold")) {
                write_private_json(root / "identity-bootstrap-hold",
                                   {{"phase", 2}, {"released", true}});
            }

            for (const char *name :
                 {"identity-hold", "stale-identity", "display-hold", "stale-state-receipt",
                  "canvas-hold", "reject-duplicate-activation", "allow-recovery", "repair-required",
                  "refuse-sync"}) {
                std::filesystem::remove(root / name);
            }

            run({"stop"});
        } catch (...) { /* Ignored: cleanup must not replace the original failure. */
        }

        std::cerr << "CLI_FIXTURE_ERROR: " << error.what() << "; owned diagnostics retained at "
                  << root.string() << '\n';

        return 1;
    }
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
