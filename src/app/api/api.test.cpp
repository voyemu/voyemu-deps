#include "api.hpp"
#include "can_restart_boundary.hpp"
#include "frame_transport.hpp"
#include "full_restart.hpp"
#include "guest_android_fake.hpp"
#include "platform_file.hpp"
#include "profile_catalog.hpp"
#include "qinggan11_startup.hpp"
#include "session.hpp"
#include "session_recovery.hpp"
#include "start_machine_identity.hpp"
#include "version.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>

#include "private_state_security.hpp"
#else
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif
using namespace voyemu;

// The bundled Android 9 profile on the 8155 board, read from the catalog.
static const std::string &qinggan09_profile() {
    static const auto id =
            embedded_profile_names(ProfilePlatform::qinggan09, ProfileBoard::sa8155).at(0);

    return id;
}

// Opens path for writing as a new file owned by the current user. An elevated Windows process
// creates files owned by Administrators, while the readers of the SDK descriptor, the supervisor
// log and the supervisor lock require the current user as owner.
static std::ofstream user_owned_file(const std::filesystem::path &path) {
    std::filesystem::remove(path);
#ifdef _WIN32
    detail::PrivateStateSecurity security;
    HANDLE owned = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(owned != INVALID_HANDLE_VALUE && CloseHandle(owned));
#endif

    return std::ofstream(path);
}

void image_dimensions(const std::filesystem::path &path, std::size_t width, std::size_t height) {
#ifdef _WIN32
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    assert(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE);
    IWICImagingFactory *factory = nullptr;
    assert(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IWICImagingFactory,
                                      reinterpret_cast<void **>(&factory))));
    IWICBitmapDecoder *decoder = nullptr;
    assert(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                        WICDecodeMetadataCacheOnLoad, &decoder)));
    IWICBitmapFrameDecode *frame = nullptr;
    assert(SUCCEEDED(decoder->GetFrame(0, &frame)));
    UINT w = 0, h = 0;
    assert(SUCCEEDED(frame->GetSize(&w, &h)) && w == width && h == height);
    frame->Release();
    decoder->Release();
    factory->Release();

    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
#else
    auto name = CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
    auto url = CFURLCreateWithFileSystemPath(nullptr, name, kCFURLPOSIXPathStyle, false);
    CFRelease(name);
    auto source = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    assert(source);
    auto image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    assert(image && CGImageGetWidth(image) == width && CGImageGetHeight(image) == height);
    CGImageRelease(image);
#endif
}

#ifdef _WIN32
void owned_recovery_tests(const std::filesystem::path &base, const std::filesystem::path &self) {
    const auto root = base / ("test-owned-recovery-" + random_token());

    ensure_private_directory(root);
    const std::string profile = qinggan09_profile(), id = random_token(),
                      ownership = random_token();
    const auto directory = root / ".local/machines" / profile / "sessions" / id,
               active = root / ".local/machines" / profile / "active" / (profile + ".json"),
               scoped = root / ".local/machines" / profile;
    ensure_private_directory(directory);
    ensure_private_directory(scoped);
    auto put = [](const auto &file) {
        ensure_private_directory(file.parent_path());
        std::ofstream out(file);
        out << "fixture";
    };
    const auto sdk_root = root / "sdk", sdk = android_sdk_program(sdk_root, true),
               frontend = root / "voyemu.exe", wrapper = sdk_root / "emulator/emulator.exe";
    put(sdk);
    put(frontend);
    put(wrapper);
    const unsigned port = 5626, grpc = 42626;
    const auto endpoint = "\\\\.\\pipe\\voyemu-" + id;
    const Json selection{{"schema", 1},
                         {"session_id", id},
                         {"profile", profile},
                         {"directory", directory.string()},
                         {"endpoint", endpoint}};
    const Json owner{
            {"schema", 1}, {"session_id", id}, {"profile", profile}, {"ownership_id", ownership}};
    const Json supervisor{{"schema", 1},
                          {"session_id", id},
                          {"directory", directory.string()},
                          {"origin", "voyemu-native-supervisor"},
                          {"creator_pid", 0xfffffff0u}};
    const std::string source_pin(64, 'f');
    const auto runtime_root = root / ".local/machines" / profile / "portable" /
                              ("portable-" + source_pin);
    ensure_private_directory(runtime_root / "android-machine/base");
    const Json launch{
            {"machine", (runtime_root / "android-machine").string()},
            {"base", (runtime_root / "android-machine/base").string()},
            {"android_sdk_root", sdk_root.string()},
            {"pins", Json::array({{{"path", sdk.string()}, {"sha256", sha256_file(sdk)}},
                                  {{"path", wrapper.string()}, {"sha256", sha256_file(wrapper)}}})},
            {"port", port},
            {"sdk_control", {{"grpc_port", grpc}}},
            {"executable", wrapper.string()},
            {"adb_transport", "direct"},
            {"frontend", {{"executable", frontend.string()}}}};
    const Json runtime{{"schema", 1},
                       {"session_id", id},
                       {"endpoint", endpoint},
                       {"android_pid", 0},
                       {"qnx_pid", 0},
                       {"stopped", false},
                       {"deliveries",
                        {{"android", {{"generation", 1}, {"status", "booting"}}},
                         {"qnx", {{"status", "not_started"}}}}}};
    write_private_json(active, selection);
    write_private_json(directory / "automation-owner.json", owner);
    write_private_json(directory / "session-owner.json", supervisor);
    write_private_json(directory / "runtime.json", runtime);
    write_private_json(scoped / "android-launch.json", launch);
    const Json activation{{"profile", profile},
                          {"manifest_sha256", source_pin},
                          {"runtime", runtime_root.string()},
                          {"android_launch_sha256", sha256_file(scoped / "android-launch.json")}};
    write_private_json(scoped / "activation.json", activation);
    auto refused = [&] {
        bool rejected = false;
        try {
            api::StoppedAndroidLease lease(root, profile, machine_identity(profile, {}), id,
                                           ownership);
        } catch (const std::exception &) {
            rejected = true;
        }

        assert(rejected && read_private_json(directory / "runtime.json") == runtime);
    };
    {
        auto bad = owner;
        bad["ownership_id"] = random_token();
        write_private_json(directory / "automation-owner.json", bad);
        refused();
        write_private_json(directory / "automation-owner.json", owner);
    }
    {
        auto bad = runtime;
        bad["android_pid"] = GetCurrentProcessId();
        write_private_json(directory / "runtime.json", bad);
        bool rejected = false;
        try {
            api::StoppedAndroidLease lease(root, profile, machine_identity(profile, {}), id,
                                           ownership);
        } catch (const std::exception &) {
            rejected = true;
        }

        assert(rejected && read_private_json(directory / "runtime.json") == bad);
        write_private_json(directory / "runtime.json", runtime);
    }
    {
        auto bad = supervisor;
        bad["android_processes"] = {{"generation", 1}, {"phase", "launch_pending"}};
        write_private_json(directory / "session-owner.json", bad);
        refused();
        write_private_json(directory / "session-owner.json", supervisor);
    }
    {
        auto bad = activation;
        bad["android_launch_sha256"] = std::string(64, '0');
        write_private_json(scoped / "activation.json", bad);
        refused();
        write_private_json(scoped / "activation.json", activation);
    }
    {
        const auto pipe = CreateNamedPipeW(std::filesystem::path(endpoint).c_str(),
                                           PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096,
                                           4096, 0, nullptr);
        assert(pipe != INVALID_HANDLE_VALUE);
        refused();
        CloseHandle(pipe);
    }
    WSADATA data{};
    assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);

    for (const auto family : {AF_INET, AF_INET6}) {
        const auto listener = socket(family, SOCK_STREAM, IPPROTO_TCP);
        assert(listener != INVALID_SOCKET);
        BOOL yes = TRUE;
        assert(setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                          reinterpret_cast<const char *>(&yes), sizeof(yes)) == 0);

        if (family == AF_INET) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            assert(bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) ==
                   0);
        } else {
            assert(setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY,
                              reinterpret_cast<const char *>(&yes), sizeof(yes)) == 0);
            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_port = htons(port);
            address.sin6_addr = in6addr_loopback;
            assert(bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) ==
                   0);
        }

        assert(listen(listener, 1) == 0);
        refused();
        closesocket(listener);
    }

    WSACleanup();
    {
        api::FileLock lock(root / ".local/machines" / profile / "active" /
                           (profile + ".supervisor.lock"));
        refused();
    }
    {
        // The launcher is absent and runtime has no backend PID. A backend-only
        // child with differently cased Windows spelling must still block recovery.
        std::filesystem::remove(sdk);
        const auto upper = sdk.parent_path() / "QEMU-SYSTEM-AARCH64.EXE";
        std::filesystem::copy_file(self, upper);
        auto backend_launch = launch;
        backend_launch["pins"][0]["sha256"] = sha256_file(sdk);
        write_private_json(scoped / "android-launch.json", backend_launch);
        auto backend_activation = activation;
        backend_activation["android_launch_sha256"] = sha256_file(scoped / "android-launch.json");
        write_private_json(scoped / "activation.json", backend_activation);
        ProcessSpec child;
        child.executable = upper;
        child.arguments = {"--recovery-child"};
        child.directory = root;
        child.output = root / "backend-child.log";
        auto retained = ChildProcess::launch(child);
        assert(retained.alive());
        refused();
        retained.terminate(100);
        assert(!retained.alive());
        std::filesystem::remove(upper);
        put(sdk);
        write_private_json(scoped / "android-launch.json", launch);
        write_private_json(scoped / "activation.json", activation);
    }
    {
        api::StoppedAndroidLease lease(root, profile, machine_identity(profile, {}), id, ownership);
        auto changed = runtime;
        changed["revision"] = 99;
        write_private_json(directory / "runtime.json", changed);
        bool rejected = false;
        try {
            lease.publish_terminal();
        } catch (const std::exception &) {
            rejected = true;
        }

        assert(rejected && read_private_json(directory / "runtime.json") == changed);
        write_private_json(directory / "runtime.json", runtime);
    }
    {
        api::StoppedAndroidLease lease(root, profile, machine_identity(profile, {}), id, ownership);

        for (const auto value : {port, port + 1, grpc}) {
            assert(!tcp_port_free(value));
        }

        const auto terminal = lease.publish_terminal();
        assert(terminal.at("stopped") == true &&
               terminal.at("deliveries")
                               .at("android")
                               .at("shutdown")
                               .at("guest_shutdown_unverified") == true);
    }
    {
        write_private_json(directory / "runtime.json", runtime);
        unsigned dispatched = 0;
        api::Transports absent_transport;
        absent_transport.control = [&](const auto &, const Json &, int) -> Json {
            ++dispatched;
            throw std::runtime_error(
                    "TEST_CONTROL_DISPATCHED: absent endpoint must recover before IPC");
        };
        api::Api absent_api(root, self, absent_transport);
        const Json request{{"schema", 1},
                           {"request_id", random_token()},
                           {"op", "session.stop"},
                           {"profile", profile},
                           {"session", {{"id", id}, {"ownership_id", ownership}}}};
        const auto recovered = absent_api.call(request);
        assert(recovered.at("ok") == true && dispatched == 0);
        const auto &result = recovered.at("result");
        assert(result.at("accepted") == true && result.at("stopped") == true &&
               result.at("cleanup").at("guest_shutdown_unverified") == true &&
               result.at("host_recovery").at("guest_shutdown_unverified") == true);
    }

    for (const auto value : {port, port + 1, grpc}) {
        assert(tcp_port_free(value));
    }

    {
        // A recorded process receipt names the adbd port and no adb server.
        auto direct_owner = supervisor;
        direct_owner["android_processes"] = {{"phase", "recorded"},  {"generation", 1},
                                             {"sdk_pid", 0},         {"frontend_pid", 0},
                                             {"console_port", port}, {"grpc_port", grpc},
                                             {"adbd_port", port + 1}};
        write_private_json(directory / "session-owner.json", direct_owner);
        write_private_json(directory / "runtime.json", runtime);
        {
            api::StoppedAndroidLease lease(root, profile, machine_identity(profile, {}), id,
                                           ownership);
            lease.verify();

            for (const auto value : {port, port + 1, grpc}) {
                assert(!tcp_port_free(value));
            }
        }
        auto inconsistent = direct_owner;
        inconsistent["android_processes"]["adb_pid"] = 0;
        write_private_json(directory / "session-owner.json", inconsistent);
        refused();
        write_private_json(directory / "session-owner.json", supervisor);
    }
    {
        // Ordinary runtime configurations intentionally have no persisted host
        // executable hashes. Maintenance still requires the exact seed and all
        // existing process, owner, endpoint and listener fences.
        write_private_json(directory / "runtime.json", runtime);
        const auto seed = root / "machines" / profile;
        ensure_private_directory(seed / "android");
        const Json manifest{
                {"schema", 1},
                {"kind", "portable-guest-runtime"},
                {"profile", profile},
                {"android",
                 {{"api", 28}, {"abi", "arm64-v8a"}, {"base_archive_sha256", source_pin}}}};
        write_private_json(seed / "runtime.json", manifest);
        auto configured_launch = launch;
        configured_launch["machine"] = (seed / "android").string();
        configured_launch["base"] = (seed / "android").string();
        configured_launch["executable"] = sdk.string();
        configured_launch["pins"] = Json::array();
        write_private_json(scoped / "android-launch.json", configured_launch);
        const Json configured{{"schema", 1},
                              {"kind", "runtime-configuration"},
                              {"profile", profile},
                              {"machine", ""},
                              {"seed", seed.string()},
                              {"manifest_sha256", sha256_file(seed / "runtime.json")},
                              {"integrity_audited", false}};
        write_private_json(scoped / "activation.json", configured);
        {
            api::StoppedAndroidLease lease(root, profile, machine_identity(profile, {}), id,
                                           ownership);
            lease.verify();

            for (const auto value : {port, port + 1, grpc}) {
                assert(!tcp_port_free(value));
            }
        }
        {
            auto bad = configured;
            const auto foreign = root / "machines/foreign";
            ensure_private_directory(foreign);
            bad["seed"] = foreign.string();
            write_private_json(scoped / "activation.json", bad);
            refused();
            write_private_json(scoped / "activation.json", configured);
        }
        {
            auto bad = configured;
            bad["manifest_sha256"] = std::string(64, '0');
            write_private_json(scoped / "activation.json", bad);
            refused();
            write_private_json(scoped / "activation.json", configured);
        }
        {
            auto bad = runtime;
            bad["android_pid"] = GetCurrentProcessId();
            write_private_json(directory / "runtime.json", bad);
            bool rejected = false;
            try {
                api::StoppedAndroidLease lease(root, profile, machine_identity(profile, {}), id,
                                               ownership);
            } catch (const std::exception &) {
                rejected = true;
            }

            assert(rejected);
            write_private_json(directory / "runtime.json", runtime);
        }
        {
            api::StoppedAndroidLease lease(root, profile, machine_identity(profile, {}), id,
                                           ownership);
            auto updated = configured_launch;
            updated["config_template"] = "source-refresh";
            write_private_json(scoped / "android-launch.json", updated);
            lease.verify_host_absence();
            bool rejected = false;
            try {
                lease.verify();
            } catch (const std::exception &) {
                rejected = true;
            }

            assert(rejected);
        }
        // A portable positional machine outside root/machines keeps its exact
        // registered seed/selector; recovery must not invent a root-relative seed.
        const auto external = root.parent_path() / ("external-recovery-" + random_token());
        ensure_private_directory(external / "android");
        write_private_json(external / "runtime.json", manifest);
        const auto external_name = machine_path_name(root, {external, profile});
        const auto external_scoped = profile_launch_directory(
                root, profile, voyemu::machine_identity(profile, external_name));
        ensure_private_directory(external_scoped);
        auto external_launch = configured_launch;
        external_launch["machine"] = (external / "android").string();
        external_launch["base"] = (external / "android").string();
        write_private_json(external_scoped / "android-launch.json", external_launch);
        auto external_activation = configured;
        external_activation["machine"] = external_name;
        external_activation["seed"] = external.string();
        external_activation["manifest_sha256"] = sha256_file(external / "runtime.json");
        write_private_json(external_scoped / "activation.json", external_activation);
        const Json external_selection{
                {"machine", external_name},
                {"seed", external.string()},
                {"manifest_sha256", external_activation.at("manifest_sha256")}};
        const auto external_directory = native_session_directory(
                root, profile, voyemu::machine_identity(profile, external_name), id);
        ensure_private_directory(external_directory);

        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::filesystem::copy_file(entry.path(),
                                           external_directory / entry.path().filename());
            }
        }

        write_private_json(external_directory / "machine-selection.json", external_selection);
        auto external_active = selection;
        external_active["directory"] = external_directory.string();
        const auto external_active_path =
                native_current_active_directory(root, profile,
                                                voyemu::machine_identity(profile, external_name)) /
                (profile + ".json");
        write_private_json(external_active_path, external_active);
        auto external_supervisor = supervisor;
        external_supervisor["directory"] = external_directory.string();
        write_private_json(external_directory / "session-owner.json", external_supervisor);
        {
            api::StoppedAndroidLease lease(root, profile, machine_identity(profile, external_name),
                                           id, ownership);
            lease.verify();
        }
        auto external_refused = [&] {
            bool rejected = false;
            try {
                api::StoppedAndroidLease lease(
                        root, profile, machine_identity(profile, external_name), id, ownership);
            } catch (const std::exception &) {
                rejected = true;
            }

            assert(rejected);
        };
        {
            auto bad = external_selection;
            bad["machine"] = "external-wrong";
            write_private_json(external_directory / "machine-selection.json", bad);
            external_refused();
            write_private_json(external_directory / "machine-selection.json", external_selection);
        }
        {
            auto bad = external_selection;
            bad["manifest_sha256"] = std::string(64, '0');
            write_private_json(external_directory / "machine-selection.json", bad);
            external_refused();
            write_private_json(external_directory / "machine-selection.json", external_selection);
        }
        {
            auto bad = external_activation;
            bad["seed"] = seed.string();
            write_private_json(external_scoped / "activation.json", bad);
            external_refused();
            write_private_json(external_scoped / "activation.json", external_activation);
        }
        std::filesystem::remove(external_active_path);
        std::filesystem::remove(external_directory / "machine-selection.json");
        std::filesystem::remove_all(external);
        write_private_json(scoped / "android-launch.json", launch);
        write_private_json(scoped / "activation.json", activation);
    }
    {
        api::Transports offline;
        offline.control = [](const auto &, const Json &, int) -> Json {
            throw std::runtime_error("IPC_CONNECT: exact fixture endpoint is absent");
        };
        api::Api recovery_api(root, frontend, offline);
        Json stop{{"schema", 1},
                  {"request_id", random_token()},
                  {"op", "session.stop"},
                  {"profile", profile},
                  {"session", {{"id", id}, {"ownership_id", ownership}}}};
        const auto stopped = recovery_api.call(stop);
        assert(stopped.at("ok") == true &&
               stopped.at("result").at("cleanup").at("verified") == true &&
               stopped.at("result").at("cleanup").at("guest_shutdown_unverified") == true);
        const auto present = CreateNamedPipeW(std::filesystem::path(endpoint).c_str(),
                                              PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1,
                                              4096, 4096, 0, nullptr);
        assert(present != INVALID_HANDLE_VALUE);
        offline.control = [](const auto &, const Json &, int) -> Json {
            throw std::runtime_error(
                    "TEST_SESSION: ownership rejection is not a transport failure");
        };
        api::Api rejected_api(root, frontend, offline);
        stop["request_id"] = random_token();
        const auto rejected = rejected_api.call(stop);
        assert(rejected.at("ok") == false && rejected.at("error").at("code") == "TEST_SESSION");
        CloseHandle(present);
    }
}
#endif
#ifndef _WIN32
static void wheel_release_admission_tests(const std::filesystem::path &base) {
    const auto root = base / ("test-wheel-cancel-admission-" + random_token());

    ensure_private_directory(root);
    const std::string name = qinggan11_731_profile_id, id = random_token(), owner = random_token();
    const auto directory = root / ".local/machines" / name / "sessions" / id;
    ensure_private_directory(directory);
    Session session(directory, id);
    auto select = [&](const std::string &selected, const std::string &epoch) {
        const auto path = root / ".local/machines" / selected / "sessions" / epoch;
        ensure_private_directory(path);
        write_private_json(root / ".local/machines" / selected / "active" / (selected + ".json"),
                           {{"schema", 1},
                            {"session_id", epoch},
                            {"directory", path.string()},
                            {"endpoint", "owned-cancel-fixture"}});
        write_private_json(path / "automation-owner.json",
                           {{"schema", 1}, {"session_id", epoch}, {"ownership_id", owner}});
    };
    select(name, id);
    const std::string other = qinggan09_profile(), other_id = random_token();
    select(other, other_id);
    auto request = [&](const char *op) {
        return Json{{"schema", 1},
                    {"request_id", random_token()},
                    {"op", op},
                    {"profile", name},
                    {"session", {{"id", id}, {"ownership_id", owner}}}};
    };
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false, resume = false;
    unsigned releases = 0, accepted_ups = 0;
    api::Transports transport;
    transport.control = [&](const auto &, const Json &command, int) -> Json {
        const auto op = command.at("op").get<std::string>();

        if (op == "status") {
            auto reply = session.handle(command);
            reply["wheel_capability"] =
                    qinggan11_partial_wheel_capability(qinggan11_731_profile_id);

            return reply;
        }

        if (op == "wheel_pair") {
            {
                std::unique_lock lock(mutex);
                started = true;
                condition.notify_all();
                condition.wait(lock, [&] {
                    return resume;
                });
            }

            return Json{{"ok", true},
                        {"session_id", id},
                        {"down", {{"accepted", true}}},
                        {"up", {{"accepted", true}}},
                        {"released", true},
                        {"guest_acknowledged", false},
                        {"visible_effect_verified", false}};
        }

        auto reply = session.handle(command);

        if (op == "wheel" && command.at("phase") == "down") {
            std::unique_lock lock(mutex);
            started = true;
            condition.notify_all();
            condition.wait(lock, [&] {
                return resume;
            });
        }

        if (op == "release_owner") {
            ++releases;
        }

        if (op == "wheel" && command.at("phase") == "up" && reply.value("ok", false)) {
            ++accepted_ups;
        }

        return reply;
    };
    api::Api pressing(root, "unused-cli", transport), canceling(root, "unused-cli", transport);
    auto busy = [&](const Json &value) {
        const auto result = canceling.call(value);
        assert(!result.at("ok").get<bool>() && result.at("error").at("code") == "TEST_BUSY");
    };
    Json press_result;
    auto press = request("input.wheel.press");
    press["button"] = "volume_down";
    press["hold_ms"] = 201;
    auto unowned = press;
    unowned["request_id"] = random_token();
    unowned["session"]["ownership_id"] = random_token();
    assert(pressing.call(unowned).at("ok") == false && !started &&
           !std::filesystem::exists(directory / "automation/wheel-writer.json"));
    std::thread held([&] {
        press_result = pressing.call(press);
    });
    {
        std::unique_lock lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return started;
        }));
    }
    auto capture = request("session.screenshot");
    busy(capture);
    auto foreign = request("input.wheel.release");
    foreign["session"]["ownership_id"] = random_token();
    const auto rejected = canceling.call(foreign);
    assert(!rejected.at("ok").get<bool>() && releases == 0);
    auto other_session = request("input.wheel.release");
    other_session["profile"] = other;
    other_session["session"]["id"] = other_id;
    const auto independent = canceling.call(other_session);
    assert(independent.value("error", Json::object()).value("code", std::string{}) != "TEST_BUSY");
    const auto marker = directory / "automation/wheel-writer.json",
               lease = directory / "automation/wheel-writer.lock";
    const auto valid_marker = read_private_json(marker);
    auto denied = [&] {
        const auto before = releases;
        assert(canceling.call(request("input.wheel.release")).at("ok") == false &&
               releases == before);
    };

    for (const auto &patch : Json::array({Json{{"schema", 1.0}}, Json{{"unexpected", true}},
                                          Json{{"operation_id", "../outside"}},
                                          Json{{"operation_id", std::string(32, 'A')}},
                                          Json{{"ownership_sha256", std::string(64, 'f')}},
                                          Json{{"session_id", random_token()}}})) {
        auto invalid = valid_marker;
        invalid.update(patch);
        write_private_json(marker, invalid);
        denied();
        write_private_json(marker, valid_marker);
    }

    for (const auto mode :
         {std::filesystem::perms::owner_read,
          std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                  std::filesystem::perms::group_read | std::filesystem::perms::others_read}) {
        for (const auto &path : {marker, lease}) {
            std::filesystem::permissions(path, mode);
            denied();
            std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                                       std::filesystem::perms::owner_write);
        }
    }

    for (const auto &path : {marker, lease}) {
        const auto saved = path.string() + ".saved";
        std::filesystem::rename(path, saved);
        std::filesystem::create_symlink(saved, path);
        denied();
        std::filesystem::remove(path);
        std::filesystem::rename(saved, path);
    }

    const auto pending = root / ".local/build/automation/operations" /
                         (valid_marker.at("operation_id").get<std::string>() + ".json");
    const auto original_pending = read_private_json(pending);
    auto terminal = original_pending;
    terminal["status"] = "terminal";
    write_private_json(pending, terminal);
    denied();
    write_private_json(pending, original_pending);
    const auto released = canceling.call(request("input.wheel.release"));
    {
        std::lock_guard lock(mutex);
        resume = true;
        condition.notify_all();
    }
    held.join();
    // Real admission/FileLock overlap, rather than direct transport calls, must permit the owner's
    // cancellation.
    assert(released.at("ok") == true && releases >= 1 && accepted_ups == 0);
    assert(!press_result.at("ok").get<bool>() &&
           press_result.at("error").at("code") == "TEST_INPUT_CANCELLED_UNKNOWN" &&
           press_result.at("error").at("outcome") == "unknown");
    // A retained marker plus an old client/capture writer without the live wheel lease grants no
    // bypass.
    {
        api::FileLock capture_writer(directory / "automation/writer.lock");
        const auto before = releases;
        busy(request("input.wheel.release"));
        assert(releases == before);
    }
    started = false;
    resume = false;
    press = request("input.wheel.press");
    press["button"] = "volume_down";
    press["hold_ms"] = 100;
    std::thread short_press([&] {
        press_result = pressing.call(press);
    });
    {
        std::unique_lock lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return started;
        }));
    }
    const auto before = releases;
    busy(request("input.wheel.release"));
    assert(releases == before);
    {
        std::lock_guard lock(mutex);
        resume = true;
        condition.notify_all();
    }
    short_press.join();
    assert(press_result.at("ok") == true);
    session.stop();
    std::filesystem::remove_all(root);
}
#endif

static void phone_api_contract_tests(const std::filesystem::path &base) {
    const auto root = base / ("test-phone-api-" + random_token());

    ensure_private_directory(root);
    const std::string profile = qinggan09_profile(), id = random_token(), owner = random_token();
    const auto directory = root / ".local/machines" / profile / "sessions" / id;
    ensure_private_directory(directory);
    write_private_json(root / ".local/machines" / profile / "active" / (profile + ".json"),
                       {{"schema", 1},
                        {"session_id", id},
                        {"directory", directory.string()},
                        {"endpoint", "owned-phone-fixture"}});
    write_private_json(directory / "automation-owner.json",
                       {{"schema", 1}, {"session_id", id}, {"ownership_id", owner}});
    std::vector<Json> calls;
    api::Transports transport;
    transport.control = [&](const auto &endpoint, const Json &command, int budget) {
        assert(budget == 15000 && command.at("op") == "phone_control");

        if (endpoint == "unsupported-phone-fixture") {
            return Json{{"ok", false},
                        {"session_id", id},
                        {"error",
                         "TEST_UNSUPPORTED: prepared machine has no synthetic OEM phone adapter"}};
        }

        calls.push_back(command);

        return Json{{"ok", true},
                    {"session_id", id},
                    {"synthetic", true},
                    {"data", {{"outgoing", Json::array({"+70000000001"})}}}};
    };
    api::Api instance(root, "unused", transport);
    auto request = [&](const char *op) {
        return Json{{"schema", 1},
                    {"request_id", random_token()},
                    {"op", op},
                    {"profile", profile},
                    {"session", {{"id", id}, {"ownership_id", owner}}}};
    };
    auto configure = request("multimedia.phone.configure");
    configure["payload"] = {
            {"contacts", Json::array({{{"name", "Иван Иванов"}, {"number", "+70000000001"}},
                                      {{"name", "Анна Петрова"}, {"number", "+70000000002"}}})}};
    const auto accepted = instance.call(configure);
    assert(accepted.at("ok") == true && calls.size() == 1 &&
           calls.back().at("method") == "configure" && calls.back().at("ownership_id") == owner &&
           calls.back().at("payload") == configure.at("payload"));
    assert(instance.call(configure) == accepted &&
           calls.size() == 1); // Durable admission prevents a second mutation.

    for (const char *operation : {"multimedia.phone.connect", "multimedia.phone.disconnect",
                                  "multimedia.phone.answer", "multimedia.phone.hangup"}) {
        const auto value = instance.call(request(operation));
        assert(value.at("ok") == true);
    }

    auto incoming = request("multimedia.phone.incoming");
    incoming["payload"] = {{"number", "+70000000001"}};
    assert(instance.call(incoming).at("ok") == true &&
           calls.back().at("payload").at("number") == "+70000000001");
    const auto before = calls.size();
    auto invalid = configure;
    invalid["request_id"] = random_token();
    invalid["payload"]["contacts"][1]["number"] = "70000000002";
    assert(instance.call(invalid).at("error").at("code") == "TEST_ARGUMENT" &&
           calls.size() == before);
    auto foreign = request("multimedia.phone.connect");
    foreign["session"]["ownership_id"] = random_token();
    assert(instance.call(foreign).at("error").at("code") == "TEST_OWNERSHIP" &&
           calls.size() == before);
    auto stale = request("multimedia.phone.connect");
    stale["session"]["id"] = random_token();
    assert(instance.call(stale).at("error").at("code") == "TEST_SESSION" && calls.size() == before);
    auto reading = request("multimedia.phone.state");
    reading["session"].erase("ownership_id");
    assert(instance.call(reading).at("ok") == true && !calls.back().contains("ownership_id"));
#ifndef _WIN32
    {
        api::FileLock writer(directory / "automation/writer.lock");
        const auto count = calls.size();
        const auto busy = instance.call(request("multimedia.phone.connect"));
        assert(busy.at("ok") == false && busy.at("error").at("code") == "TEST_BUSY" &&
               calls.size() == count);
    }
#endif
    const std::string other_profile = "free-11-chinese-20260731-7.3.1";
    const auto other_directory = root / ".local/machines" / other_profile / "sessions" / id;
    ensure_private_directory(other_directory);
    write_private_json(root / ".local/machines" / other_profile / "active" /
                               (other_profile + ".json"),
                       {{"schema", 1},
                        {"session_id", id},
                        {"directory", other_directory.string()},
                        {"endpoint", "unsupported-phone-fixture"}});
    write_private_json(other_directory / "automation-owner.json",
                       {{"schema", 1}, {"session_id", id}, {"ownership_id", owner}});
    auto unsupported = request("multimedia.phone.connect");
    unsupported["profile"] = other_profile;
    const auto count = calls.size();
    assert(instance.call(unsupported).at("error").at("code") == "TEST_UNSUPPORTED" &&
           calls.size() == count);
#ifndef _WIN32
    {
        api::FileLock writer(directory / "automation/writer.lock");
        unsupported["request_id"] = random_token();
        // Another machine reaches its own adapter instead of failing on this machine's writer.
        assert(instance.call(unsupported).at("error").at("code") == "TEST_UNSUPPORTED" &&
               calls.size() == count);
    }
#endif

    // The _UNKNOWN suffix alone classifies an unknown outcome.
    for (const char *code :
         {"PHONE_PROVIDER_UNKNOWN", "PHONE_DELIVERY_UNKNOWN", "PHONE_PROTOCOL_UNKNOWN",
          "PHONE_PROTOCOL", "PHONE_PROVIDER_REJECTED", "IPC_TIMEOUT_UNKNOWN", "IPC_CONNECT"}) {
        unsigned failed_calls = 0;
        api::Transports failure_transport;
        failure_transport.control = [&](const auto &, const Json &, int) {
            ++failed_calls;

            return Json{{"ok", false},
                        {"session_id", id},
                        {"error", std::string(code) + ": bounded phone rejection"}};
        };
        api::Api failure_api(root, "unused", failure_transport);
        const auto action = request("multimedia.phone.connect"), failed = failure_api.call(action);
        assert(failed.at("ok") == false && failed.at("error").at("code") == code &&
               failed.at("error").at("retryable") == false &&
               failed.at("error").at("outcome") ==
                       (std::string_view(code).ends_with("_UNKNOWN") ? "unknown" : "not_started"));
        assert(failure_api.call(action) == failed && failed_calls == 1);
    }
}

int main(int argc, char **argv) try {
    if (argc > 1 &&
        (std::string_view(argv[1]) == "_start" || std::string_view(argv[1]) == "start")) {
        if (std::string_view(argv[1]) != "_start") {
            std::cerr << "Public start is not the API bridge\n";

            return 2;
        }

        std::filesystem::path root, explicit_seed;
        std::string machine;
        Json arguments = Json::array();

        for (int i = 1; i < argc; ++i) {
            arguments.push_back(argv[i]);
        }

        for (int i = 2; i < argc; ++i) {
            const std::string_view option = argv[i];

            if (option == "--root" && i + 1 < argc) {
                root = argv[++i];
            } else if (option == "--machine" && i + 1 < argc) {
                machine = argv[++i];
            } else if ((option == "--profile" || option == "--ownership-id" ||
                        option == "--provider" || option == "--refresh-session" ||
                        option == "--seconds") &&
                       i + 1 < argc) {
                ++i;
            } else if (!option.starts_with("-")) {
                assert(explicit_seed.empty());
                explicit_seed = argv[i];
            }
        }

        write_private_json(root / ".local/build/tmp/api-private-start-arguments.json", arguments);
        const auto seed = explicit_seed.empty() ? root / "machines" / machine
                                                : inspect_machine_path(explicit_seed).seed;

        if (!explicit_seed.empty()) {
            assert(machine.empty());
            machine = machine_path_name(root, inspect_machine_path(seed));
        }

        Json output{{"ok", true}, {"session_id", std::string(32, 'a')}};
        Json status = Json::object();

        if (!machine.empty()) {
            status = {{"machine", machine},
                      {"machine_seed", seed.string()},
                      {"machine_manifest_sha256", sha256_file(seed / "runtime.json")}};
        }

        append_start_machine_identity(
                output, status, machine_identity(qinggan09_profile(), machine), explicit_seed);
        std::cout << output.dump() << '\n';

        return 0;
    }

#ifdef _WIN32
    if (argc == 2 && std::string_view(argv[1]) == "--recovery-child") {
        Sleep(10000);

        return 0;
    }
#endif
    {
        ProcessSpec source;
        source.executable = "owned-source-cli";
        source.arguments = {"_start"};

        for (bool windows : {false, true}) {
            unsigned calls = 0;
            int observed_timeout = 0;
            const auto result = api::capture_source_start(
                    source, windows, [&](const ProcessSpec &process, int timeout) {
                        ++calls;
                        observed_timeout = timeout;
                        assert(process.executable == source.executable &&
                               process.arguments == source.arguments && timeout > 0 &&
                               timeout <= 300000);

                        return ProcessResult{0, R"({"ok":true,"session_id":"fixture"})", false};
                    });
            assert(result.at("session_id") == "fixture" && calls == 1 &&
                   observed_timeout == (windows ? 300000 : 100000));
            calls = 0;
            bool unknown = false;
            try {
                api::capture_source_start(source, windows, [&](const ProcessSpec &, int timeout) {
                    ++calls;
                    assert(timeout == observed_timeout);

                    return ProcessResult{0, R"({"ok":true})", true};
                });
            } catch (const std::runtime_error &error) {
                unknown = std::string_view(error.what()).starts_with("TEST_CREATE_UNKNOWN:");
            }

            assert(unknown && calls == 1);
        }
    }
    {
        const Json selection{{"machine", "free-copy"},
                             {"seed", "/project/machines/free-copy"},
                             {"manifest_sha256", std::string(64, 'a')}};
        const Json selected_status{{"machine", "free-copy"},
                                   {"machine_seed", selection.at("seed")},
                                   {"machine_manifest_sha256", selection.at("manifest_sha256")}};
        assert(api::diagnostic_machine_name(selected_status, selection) == "free-copy");
        assert(api::diagnostic_machine_name(Json::object(), nullptr).empty());

        for (const Json changed :
             {Json{{"machine", "other"}},
              Json{{"machine", "free-copy"}, {"machine_seed", "foreign"}}, Json::object()}) {
            bool rejected = false;
            try {
                api::diagnostic_machine_name(changed, selection);
            } catch (const std::runtime_error &) {
                rejected = true;
            }

            assert(rejected);
        }
    }
    const auto project =
            std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    const auto base = argc > 2 ? std::filesystem::path(argv[2]) : project / ".local/build/tmp";
    const auto root = base / ("test-api-" + random_token());
    ensure_private_directory(root);
#ifdef _WIN32
    owned_recovery_tests(base, std::filesystem::canonical(argv[0]));
#endif
    const auto id = random_token(), owner = random_token();
    const std::string name = qinggan09_profile();
    const auto directory = root / ".local/machines" / name / "sessions" / id;
    ensure_private_directory(directory);
    Session session(directory, id);
    write_private_json(root / ".local/machines" / name / "active" / (name + ".json"),
                       {{"schema", 1},
                        {"session_id", id},
                        {"directory", directory.string()},
                        {"endpoint", "unused-owned-fixture"}});
    write_private_json(directory / "automation-owner.json",
                       {{"schema", 1}, {"session_id", id}, {"ownership_id", owner}});
    assert(argc == 2 || argc == 3);
    const auto cli_request = root / "request.json";
    write_private_json(cli_request, {{"schema", 1}, {"request_id", "cli-host"}, {"op", "info"}});
    ProcessSpec cli;
    cli.executable = argv[1];
    cli.arguments = {"test", "--root", root.string(), "--request", cli_request.string()};
    auto cli_result = execute(cli);
    assert(cli_result.exit_code == 0 &&
           Json::parse(cli_result.output).at("result").at("api") == "voyemu.test");
    {
        std::ofstream file(cli_request);
        file << "malformed-json";
    }
    auto malformed = execute(cli);
    assert(malformed.exit_code == 4 &&
           Json::parse(malformed.output).at("error").at("code") == "TEST_ARGUMENT");
    bool ready = true, changed = false, change_epoch = false, slow = false, boot_changed = false,
         change_boot = false;
    unsigned calls = 0, creates = 0;
    std::atomic<unsigned> captures{0};
    // The screens list of the status; null keeps the two multimedia screens.
    Json status_screens;
    api::Transports transport;
    std::function<Json(const Json &)> fixture_media_input;
    transport.control = [&](const auto &, const Json &op, int) {
        ++calls;
#ifdef _WIN32
        // This API unit has no SDK. The retained Android guard is exercised by
        // the compiled CLI fixture; here the explicit transport mock supplies input.
        if (op.at("op") == "media_input" && fixture_media_input) {
            auto reply = fixture_media_input(op.at("input"));
            reply["session_id"] = id;

            return reply;
        }
#endif

        if (op.at("op") == "test_boot_identity") {
            return Json{{"ok", true},
                        {"session_id", id},
                        {"android_pid", changed ? 124 : 123},
                        {"boot_id", boot_changed ? "bbbbbbbb-1111-2222-3333-444444444444"
                                                 : "aaaaaaaa-1111-2222-3333-444444444444"}};
        }

        auto reply = session.handle(op);

        if (op.at("op") == "status") {
            reply["android_pid"] = changed ? 124 : 123;
            reply["deliveries"]["android"] = {
                    {"guest_acknowledged", ready},
                    {"sequence", reply.at("revision")},
                    {"home", {{"status", "visible_on_primary"}}},
                    {"displays", {{"status", "both_guest_displays_ready"}}}};

            if (!status_screens.is_null()) {
                reply["screens"] = status_screens;
            }
        }

        return reply;
    };
    bool created_background = false;
    transport.create = [&](const Json &value) {
        created_background = value.at("background").get<bool>();
        ++creates;

        return Json{{"ok", true}, {"session_id", id}};
    };
    // Screenshots read SDK frames and boot identity from a scripted guest.
    auto shot = std::make_shared<api::ScriptedGuestAndroid>();
    shot->identity = [&](int) {
        ++calls;

        return Json{{"ok", true},
                    {"session_id", id},
                    {"android_pid", changed ? 124 : 123},
                    {"boot_id", boot_changed ? "bbbbbbbb-1111-2222-3333-444444444444"
                                             : "aaaaaaaa-1111-2222-3333-444444444444"}};
    };
    shot->capture = [&](const std::string &epoch, unsigned display, int) {
        assert(epoch == id);
        ++captures;

        if (slow) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }

        media::Image image;
        image.display = display;
        image.width = 1920;
        image.height = 720;
        image.sequence = 0;
        image.timestamp_us = static_cast<std::uint64_t>(captures) * 100;
        image.storage.resize(image.pixels().size());

        for (std::size_t i = 0; i < image.storage.size(); i += 4) {
            image.storage[i] = display ? 10 : 250;
            image.storage[i + 3] = 255;
        }

        if (change_epoch && display == 1) {
            changed = true;
        }

        if (change_boot && display == 1) {
            boot_changed = true;
        }

        return image;
    };
    api::Api api(root, "unused-cli", transport);
    auto shot_transport = transport;
    shot_transport.android = shot;
    api::Api shots(root, "unused-cli", shot_transport);
    auto request = [&](const std::string &op) {
        return Json{{"schema", 1},
                    {"request_id", random_token()},
                    {"op", op},
                    {"profile", name},
                    {"session", {{"id", id}, {"ownership_id", owner}}}};
    };
    {
        unsigned reads = 0;
        api::Transports model_transport = transport;
        model_transport.control = [&](const auto &, const Json &op, int) {
            ++reads;
            assert(op.at("op") == "test_can_model_observation");
            assert(op.at("query") ==
                   Json({{"schema", 1},
                         {"mode", "drain"},
                         {"limit", 2},
                         {"cursor", {{"session", "18446744073709551615"}, {"next_cursor", "1"}}}}));

            return Json{
                    {"ok", true},
                    {"session_id", id},
                    {"observation",
                     {{"schema", 1}, {"source", "android:can_model"}, {"status", "unavailable"}}}};
        };
        api::Api model_api(root, "unused-cli", model_transport);
        auto q = request("vehicle.can.model");
        q["mode"] = "drain";
        q["limit"] = 2;
        q["cursor"] = {{"session", "18446744073709551615"}, {"next_cursor", "1"}};
        const auto result = model_api.call(q);
        assert(result.at("ok") == true && result.at("result").at("source") == "android:can_model" &&
               reads == 1);
        auto invalid = q;
        invalid["request_id"] = random_token();
        invalid["limit"] = 33;
        assert(model_api.call(invalid).at("ok") == false && reads == 1);
        invalid = q;
        invalid["request_id"] = random_token();
        invalid["schema"] = 2;
        assert(model_api.call(invalid).at("ok") == false && reads == 1);
        // Every host answers through the same session path; an unpowered guest is unavailable.
        const auto unpowered = session.handle({{"op", "test_can_model_observation"},
                                               {"session_id", id},
                                               {"query", {{"schema", 1}}}});
        assert(unpowered.at("ok") == true &&
               unpowered.at("observation").at("source") == "android:can_model" &&
               unpowered.at("observation").at("status") == "unavailable");
        const auto invalid_query =
                session.handle({{"op", "test_can_model_observation"},
                                {"session_id", id},
                                {"query", {{"schema", 1}, {"mode", "drain"}, {"limit", 33}}}});
        assert(invalid_query.at("ok") == false &&
               invalid_query.at("error").get<std::string>().starts_with("CAN_MODEL_ARGUMENT:"));
    }
    {
        // A session without guests reports that every field stays in the model.
        const auto status = session.handle({{"op", "status"}, {"session_id", id}});
        assert(status.at("field_delivery") ==
               Json({{"cluster", Json::array()}, {"multimedia", Json::array()}}));
        unsigned reads = 0;
        api::Transports fields_transport = transport;
        fields_transport.control = [&](const auto &, const Json &op, int) {
            ++reads;
            assert(op.at("op") == "status");

            return Json{{"ok", true},
                        {"session_id", id},
                        {"state", initial_state(VehicleCatalog::qinggan09)},
                        {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                        {"field_delivery",
                         {{"cluster", Json::array({"soc", "speed", "spd_valid"})},
                          {"multimedia", Json::array({"soc"})}}}};
        };
        api::Api fields_api(root, "unused-cli", fields_transport);
        const auto described = fields_api.call(request("vehicle.fields"));
        assert(described.at("ok") == true && reads == 1);
        const auto &fields = described.at("result").at("fields");
        assert(fields.size() == 579);

        // A bound field names its concept and encoding; the concept range limits its primary.
        const auto field_named = [](const Json &list, std::string_view name) {
            return *std::find_if(list.begin(), list.end(), [&](const Json &field) {
                return field.at("name") == name;
            });
        };
        assert(field_named(fields, "speed").at("unit") == "km/h" &&
               field_named(fields, "speed10x").at("unit") == "0.1 km/h" &&
               field_named(fields, "soc").at("unit") == "percent" &&
               field_named(fields, "soc").at("maximum") == 100 &&
               field_named(fields, "odo").at("unit") == "raw_oem" &&
               !field_named(fields, "odo").contains("concept") &&
               field_named(fields, "speed10x").at("concept").at("role") == "mirror" &&
               field_named(fields, "door_fl").at("concept").at("name") == "door_front_left" &&
               described.at("result").at("concepts").size() ==
                       concept_bindings(VehicleCatalog::qinggan09).size());

        for (const auto &field : fields) {
            const auto name = field.at("name").get<std::string>();
            assert(field.at("delivery") == (name == "soc" ? Json::array({"cluster", "multimedia"})
                                            : name == "speed" || name == "spd_valid"
                                                    ? Json::array({"cluster"})
                                                    : Json::array()));
        }

        // Without a session the catalog of the platform of a named profile describes the model
        // alone; a request that names neither is rejected.
        const auto bare = fields_api.call(
                {{"schema", 1}, {"request_id", random_token()}, {"op", "vehicle.fields"}});
        assert(bare.at("ok") == false && reads == 1);

        for (const auto &bundled : embedded_profile_catalog()) {
            const auto platform = bundled.at("definition").at("platform").get<std::string>();
            const auto described_profile = fields_api.call({{"schema", 1},
                                                            {"request_id", random_token()},
                                                            {"op", "vehicle.fields"},
                                                            {"profile", bundled.at("id")}});
            const auto &result = described_profile.at("result");
            assert(described_profile.at("ok") == true && reads == 1 &&
                   result.at("fields").size() == catalog(vehicle_catalog(platform)).size() &&
                   result.at("map_sha256") == catalog_map_sha256(vehicle_catalog(platform)) &&
                   !result.at("fields").at(0).contains("delivery"));

            // Passion fields carry the declaration of the OEM SOME/IP service table.
            if (platform == "passion") {
                const auto speed =
                        field_named(result.at("fields"),
                                    concept_field(VehicleCatalog::passion, VehicleConcept::speed));
                assert(speed.at("signal").at("id") == 212 &&
                       speed.at("signal").at("resolution") == 0.01 &&
                       speed.at("signal").at("offset") == -500.0 && speed.at("unit") == "kmph" &&
                       speed.at("minimum") == 50000 && speed.at("maximum") == 75500 &&
                       speed.at("concept").at("name") == "speed");
                const auto ready =
                        field_named(result.at("fields"),
                                    concept_field(VehicleCatalog::passion, VehicleConcept::ready));
                assert(ready.at("signal").at("values").at("2") == "OnOffSts_ON" &&
                       ready.at("concept").at("encoding").at("invalid") == 255);
            }
        }
    }
    {
        // Same-profile machines remain addressable by their retained SID alone.
        const auto scoped_root = root / "same-profile-routing";
        ensure_private_directory(scoped_root);
        const std::string first = random_token(), second = random_token(),
                          first_owner = random_token(), second_owner = random_token();
        std::map<std::string, std::string> endpoints{
                {first, (scoped_root / "scope-first").string()},
                {second, (scoped_root / "scope-second").string()}};
        auto publish = [&](const std::string &sid, const std::string &ownership,
                           const std::string &machine) {
            const auto path = scoped_root / ".local/machines" / machine / "sessions" / sid;
            ensure_private_directory(path);
            write_private_json(path / "machine-selection.json",
                               {{"machine", machine},
                                {"seed", (scoped_root / "machines" / machine).string()},
                                {"manifest_sha256", std::string(64, 'a')}});
            write_private_json(path / "automation-owner.json",
                               {{"schema", 1}, {"session_id", sid}, {"ownership_id", ownership}});
            write_private_json(scoped_root / ".local/machines" / machine / "active" /
                                       (name + ".json"),
                               {{"schema", 1},
                                {"session_id", sid},
                                {"directory", path.string()},
                                {"endpoint", endpoints.at(sid)}});
        };
        publish(first, first_owner, "machine-a");
        publish(second, second_owner, "machine-b");
        // A conflicting legacy record must never redirect a machine-scoped request.
        write_private_json(
                scoped_root / ".local/machines" / name / "active" / (name + ".json"),
                {{"schema", 1},
                 {"session_id", second},
                 {"directory",
                  (scoped_root / ".local/machines" / name / "sessions" / second).string()},
                 {"endpoint", "foreign-legacy"}});
        unsigned dispatches = 0;
        api::Transports scoped_transport;
        scoped_transport.control = [&](const std::filesystem::path &endpoint, const Json &op, int) {
            const auto sid = op.at("session_id").get<std::string>();

            assert(endpoint == endpoints.at(sid));
            ++dispatches;

            return Json{{"ok", true}, {"session_id", sid}, {"accepted", true}};
        };
        api::Api scoped_api(scoped_root, "unused-cli", scoped_transport);
        auto invoke = [&](const std::string &sid, const std::string &ownership,
                          const std::string &op) {
            return Json{{"schema", 1},
                        {"request_id", random_token()},
                        {"op", op},
                        {"profile", name},
                        {"session", {{"id", sid}, {"ownership_id", ownership}}}};
        };

        for (const auto &sid : {first, second}) {
            assert(scoped_api
                           .call(invoke(sid, sid == first ? first_owner : second_owner,
                                        "session.status"))
                           .at("ok") == true);
            auto attach = invoke(sid, "unused", "session.attach");
            attach["session"].erase("ownership_id");
            assert(scoped_api.call(attach).at("ok") == true);
            auto patch = invoke(sid, sid == first ? first_owner : second_owner, "vehicle.set");
            patch["expected_revision"] = 0;
            patch["values"] = {{"speed", 1}};
            assert(scoped_api.call(patch).at("ok") == true);
        }

        auto wrong = invoke(first, second_owner, "vehicle.set");
        wrong["expected_revision"] = 0;
        wrong["values"] = {{"speed", 1}};
        const auto before = dispatches;
        assert(scoped_api.call(wrong).at("ok") == false && dispatches == before);
        auto mismatched = invoke(first, first_owner, "session.status");
        mismatched["machine"] = "machine-b";
        assert(scoped_api.call(mismatched).at("ok") == false && dispatches == before);
#ifndef _WIN32
        {
            // Retain the actual released lock file left by our terminal supervisor.
            const auto own_lock = scoped_root / ".local/machines/machine-a/active" /
                                  (name + ".supervisor.lock");
            { api::FileLock supervisor(own_lock); }
            assert(api::file_lock_released(own_lock));
            // A foreign legacy supervisor remains locked throughout our scoped stop.
            api::FileLock foreign(scoped_root / ".local/machines" / name / "active" /
                                  (name + ".supervisor.lock"));
            // The live supervisor serves its endpoint until it stops; an absent endpoint with a
            // released lock would be an orphaned session.
            { std::ofstream(endpoints.at(first)) << "live supervisor endpoint"; }
            const auto prior = scoped_transport.control;
            scoped_transport.control = [&](const std::filesystem::path &endpoint, const Json &op,
                                           int timeout) {
                auto reply = prior(endpoint, op, timeout);

                if (op.at("op") == "stop") {
                    write_private_json(scoped_root / ".local/machines/machine-a/sessions" / first /
                                               "runtime.json",
                                       {{"session_id", first},
                                        {"stopped", true},
                                        {"deliveries", {{"qnx", {{"status", "not_started"}}}}}});
                    std::filesystem::remove(endpoint);
                }

                return reply;
            };
            api::Api stop_api(scoped_root, "unused-cli", scoped_transport);
            const auto stopped = stop_api.call(invoke(first, first_owner, "session.stop"));
            assert(stopped.at("ok") == true &&
                   stopped.at("result").at("cleanup").at("supervisor_lock_released") == true);
            assert(!api::file_lock_released(scoped_root / ".local/machines" / name / "active" /
                                            (name + ".supervisor.lock")));
        }
#endif
        const auto active = scoped_root / ".local/machines/machine-a/active" / (name + ".json");
        auto replacement = read_private_json(active);
        replacement["session_id"] = random_token();
        write_private_json(active, replacement);
        assert(scoped_api.call(invoke(first, first_owner, "session.status")).at("ok") == false);
        assert(scoped_api.call(invoke(second, second_owner, "session.status")).at("ok") == true);
        // A copied SID cannot silently choose another machine's runtime scope.
        publish(second, second_owner, "machine-a");
        assert(scoped_api.call(invoke(second, second_owner, "session.status"))
                       .at("error")
                       .at("code") == "TEST_SESSION");
        auto explicit_scope = invoke(second, second_owner, "session.status");
        explicit_scope["machine"] = "machine-b";
        assert(scoped_api.call(explicit_scope).at("ok") == true);
    }
#ifdef _WIN32
    {
        // A live connected Windows endpoint is not a filesystem object. The
        // status preflight must still dispatch while its only instance is busy.
        const auto active = root / ".local/machines" / name / "active" / (name + ".json");
        const auto original = read_private_json(active, 4096);
        auto record = original;
        const auto endpoint = std::filesystem::path("\\\\.\\pipe\\voyemu-" + id);
        HANDLE server = CreateNamedPipeW(endpoint.c_str(), PIPE_ACCESS_DUPLEX,
                                         PIPE_TYPE_BYTE | PIPE_NOWAIT, 1, 4096, 4096, 0, nullptr);
        assert(server != INVALID_HANDLE_VALUE);
        HANDLE client = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        assert(client != INVALID_HANDLE_VALUE);
        assert(!api::StoppedAndroidLease::endpoint_absent(endpoint));
        record["endpoint"] = endpoint.string();
        write_private_json(active, record);
        const auto before = calls;
        const auto response = api.call(request("session.status"));
        CloseHandle(client);
        CloseHandle(server);
        write_private_json(active, original);

        if (!response.value("ok", false)) {
            std::cerr << "live named-pipe status rejected: " << response.dump() << std::endl;
        }

        assert(response.at("ok") == true && calls == before + 1);
    }
#endif
    {
        // The real CLI canonicalizes its session argument before publishing it.
        // On Windows this changes mixed source separators into backslashes.
        const auto active = root / ".local/machines" / name / "active" / (name + ".json");
        auto record = read_private_json(active, 4096);
        record["directory"] = std::filesystem::canonical(directory).string();
        write_private_json(active, record);
        const auto canonical_status = api.call(request("session.status"));

        if (!canonical_status.value("ok", false)) {
            std::cerr << "canonical CLI session record rejected: " << canonical_status.dump()
                      << std::endl;
        }

        assert(canonical_status.at("ok") == true);

        for (const auto &foreign :
             {directory.parent_path() / random_token(), directory / ".", directory / ".." / id}) {
            auto changed_record = record;
            changed_record["directory"] = foreign.string();
            write_private_json(active, changed_record);
            const auto before = calls;
            const auto rejected = api.call(request("session.status"));
            assert(rejected.at("ok") == false &&
                   rejected.at("error").at("code") == "TEST_SESSION" && calls == before);
        }

        const auto alias = root / "session-directory-alias";
        std::filesystem::create_directory_symlink(directory, alias);
        auto linked_record = record;
        linked_record["directory"] = alias.string();
        write_private_json(active, linked_record);
        auto before = calls;
        auto rejected = api.call(request("session.status"));
        assert(rejected.at("ok") == false && rejected.at("error").at("code") == "TEST_SESSION" &&
               calls == before);
        std::filesystem::remove(alias);
        auto stale_record = record;
        stale_record["session_id"] = random_token();
        write_private_json(active, stale_record);
        before = calls;
        rejected = api.call(request("session.status"));
        assert(rejected.at("ok") == false && rejected.at("error").at("code") == "TEST_SESSION" &&
               calls == before);
        write_private_json(active, record);
    }
    auto verify_terminal = [&](api::Api &reopened, const Json &command, const Json &response,
                               const char *outcome) {
        for (const char *key : {"operation_id", "request_id"}) {
            auto lookup = request("operation.get");
            lookup["target"] = {{key, response.at(key)}};
            const auto record = reopened.call(lookup).at("result");
            assert(record.at("status") == "terminal" && record.at("outcome") == outcome &&
                   record.at("response") == response);
        }

        assert(response.at("request_id") == command.at("request_id"));
    };
    {
        // Exercise the public API error path, not a standalone string sanitizer.
        const auto metadata = directory / "ui-observer.json";
        write_private_json(metadata,
                           {{"schema", 1}, {"session_id", id}, {"connection", Json::object()}});
        Json rejection{{"schema", 1},
                       {"ok", false},
                       {"reason", "display_unavailable"},
                       {"error_type", "IllegalStateException"},
                       {"cause_type", "InvocationTargetException"}};
        unsigned observations = 0;
        api::Transports ui_transport = transport;
        auto ui_transport_guest = std::make_shared<api::ScriptedGuestAndroid>();
        ui_transport.android = ui_transport_guest;
        ui_transport_guest->exchange = [&](const Json &, const Json &operation, int) {
            ++observations;
            assert(operation.at("op") == "ui");

            return rejection;
        };
        api::Api ui_api(root, "unused-cli", ui_transport);
        auto observe = [&]() {
            auto query = request("session.ui");
            query["screen"] = "secondary";
            auto response = ui_api.call(query);
            assert(response.at("ok") == true &&
                   response.at("result").at("multimedia")[1].at("error_code") == "TREE_REJECTED");

            return response.dump();
        };
        const auto diagnostic = observe();
        assert(diagnostic.find("reason=display_unavailable") != std::string::npos &&
               diagnostic.find("error_type=IllegalStateException") != std::string::npos &&
               diagnostic.find("cause_type=InvocationTargetException") != std::string::npos &&
               observations == 1);
        rejection = {{"schema", 1},
                     {"ok", false},
                     {"reason", "wm_display_conflict"},
                     {"error_type", "IllegalStateException"},
                     {"message", "must-not-leak"}};
        const auto conflict = observe();
        assert(conflict.find("reason=wm_display_conflict") != std::string::npos &&
               conflict.find("must-not-leak") == std::string::npos);

        for (const auto &unsafe : std::vector<std::string>{
                     "wm_display_conflict_secret", "token_abc123", "private user text",
                     "token=must-not-leak", "line\nsecret", "tab\tsecret", std::string(161, 'x')}) {
            for (const auto *field : {"reason", "error_type", "cause_type"}) {
                rejection = {{"schema", 1}, {"ok", false}, {field, unsafe}};
                const auto sanitized = observe();
                assert(sanitized.find(std::string(field) + "=") == std::string::npos);
            }
        }

        rejection = {{"schema", 1},
                     {"ok", false},
                     {"reason", Json::array({"private"})},
                     {"error_type", 42},
                     {"cause_type", nullptr},
                     {"message", "must-not-leak"},
                     {"stack", "must-not-leak"}};
        const auto malformed = observe();
        assert(malformed.find("must-not-leak") == std::string::npos &&
               malformed.find("reason=") == std::string::npos &&
               malformed.find("error_type=") == std::string::npos &&
               malformed.find("cause_type=") == std::string::npos);
        rejection = {
                {"schema", 1}, {"ok", false}, {"reason", "deadline"}, {"message", "must-not-leak"}};
        auto deadline = request("session.ui");
        deadline["screen"] = "secondary";
        auto timed_out = ui_api.call(deadline);
        assert(timed_out.at("result").at("multimedia")[1].at("error_code") ==
                       "TREE_TIMEOUT_UNKNOWN" &&
               timed_out.dump().find("must-not-leak") == std::string::npos);
        std::filesystem::remove(metadata);
    }
    {
        const auto metadata = directory / "ui-observer.json";
        const Json connection = {
                {"boot_id", "retry-boot"}, {"observer_pid", 777}, {"auth", "private-retry-token"}};
        write_private_json(metadata, {{"schema", 1},
                                      {"session_id", id},
                                      {"android_pid", 123},
                                      {"connection", connection}});
        unsigned ui_reads = 0, hellos = 0;
        bool continuous = false, wrong_boot = false, slow_read = false;
        std::string reason = "stale_child";
        api::Transports retries = transport;
        auto retries_guest = std::make_shared<api::ScriptedGuestAndroid>();
        retries.android = retries_guest;
        retries_guest->exchange = [&](const Json &, const Json &op, int budget) {
            assert(budget > 0);

            if (op.at("op") == "hello") {
                ++hellos;

                return Json{{"ok", true},
                            {"boot_id", wrong_boot ? "another-boot" : "retry-boot"},
                            {"observer_pid", 777}};
            }

            assert(op.at("op") == "ui");
            ++ui_reads;

            if (slow_read) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }

            if (continuous || ui_reads == 1) {
                return Json{{"schema", 1}, {"ok", false}, {"reason", reason}};
            }

            return Json{{"schema", 1},
                        {"ok", true},
                        {"boot_id", "retry-boot"},
                        {"observer_pid", 777},
                        {"surfaces", Json::array({{{"surface", "media_primary"},
                                                   {"display_id", 0},
                                                   {"status", "ok"},
                                                   {"complete", true},
                                                   {"nodes", Json::array()},
                                                   {"windows", Json::array()}}})}};
        };
        api::Api retry_api(root, "unused-cli", retries);
        auto query = [&](int timeout) {
            auto value = request("session.ui");
            value["screen"] = "primary";
            value["timeout_ms"] = timeout;

            return retry_api.call(value).at("result").at("multimedia")[0];
        };

        for (const auto *transient : {"stale_child", "stale_node", "window_changed",
                                      "node_window_changed", "snapshot_busy"}) {
            reason = transient;
            ui_reads = hellos = 0;
            auto response = query(500);
            assert(response.at("status") == "ok" && ui_reads == 2 && hellos == 2);
            assert(response.at("read_attempts") == 2 &&
                   response.at("transient_rejections").at(reason) == 1);
        }

        ui_reads = hellos = 0;
        reason = "display_unavailable";
        auto unknown = query(500);
        assert(unknown.at("error").at("code") == "TREE_REJECTED" && ui_reads == 1 && hellos == 0);
        ui_reads = hellos = 0;
        reason = "application_window_unverified";
        auto omitted = query(500);
        assert(omitted.at("error").at("code") == "TREE_REJECTED" && ui_reads == 1 && hellos == 0);
        assert(omitted.at("error").at("message").get<std::string>().find(
                       "reason=application_window_unverified") != std::string::npos);
        ui_reads = hellos = 0;
        reason = "stale_child";
        wrong_boot = true;
        auto stale = query(500);
        assert(stale.at("error").at("code") == "TREE_STALE" && ui_reads == 1);
        wrong_boot = false;
        ui_reads = hellos = 0;
        continuous = true;
        auto timeout = query(20);
        assert(timeout.at("error").at("code") == "TREE_TIMEOUT_UNKNOWN" && ui_reads > 1 &&
               timeout.dump().find("cause=TREE_REJECTED") != std::string::npos);
        // Leave time for owned metadata discovery; the observer itself must exhaust the budget.
        ui_reads = hellos = 0;
        slow_read = true;
        auto expired = query(200);

        if (expired.at("error").at("code") != "TREE_TIMEOUT_UNKNOWN" || ui_reads != 1) {
            std::cerr << "slow observer deadline: reads=" << ui_reads
                      << " response=" << expired.dump() << std::endl;
        }

        assert(expired.at("error").at("code") == "TREE_TIMEOUT_UNKNOWN" && ui_reads == 1);
        std::filesystem::remove(metadata);
    }
    auto capabilities = api.call(request("info"));
    assert(capabilities.at("schema") == 1 && capabilities.at("ok") == true &&
           capabilities.at("result").at("mcp") == false &&
           capabilities.at("result").at("version") == std::string(version()) &&
           std::regex_match(std::string(version()),
                            std::regex("[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.]+)?")) &&
           capabilities.at("result").at("release_date") == std::string(release_date()) &&
           std::regex_match(std::string(release_date()), std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}")));
    const auto &surfaces = capabilities.at("result").at("screens");
    assert(surfaces.at(0).at("positive_after_sequence_supported") == false &&
           surfaces.at(1).at("positive_after_sequence_supported") == false &&
           surfaces.at(2).at("positive_after_sequence_supported") == true);
    assert(!std::filesystem::exists(root / ".local/build/automation"));
#if defined(_WIN32)
    const char *expected_platform = "Windows";
    const bool expected_windows_runtime = true;
#elif defined(__APPLE__)
    const char *expected_platform = "macOS";
    const bool expected_windows_runtime = false;
#else
    const char *expected_platform = "Linux";
    const bool expected_windows_runtime = false;
#endif
    assert(capabilities.at("result").at("platform") == expected_platform &&
           capabilities.at("result").at("windows_runtime_supported") == expected_windows_runtime);
#ifndef _WIN32
    {
        bool partial = true;
        std::vector<Json> sent;
        auto paired_transport = transport;
        paired_transport.control = [&](const auto &endpoint, const Json &operation,
                                       int timeout) -> Json {
            if (operation.at("op") == "status") {
                auto reply = transport.control(endpoint, operation, timeout);
                reply["wheel_capability"] =
                        partial ? qinggan11_partial_wheel_capability(qinggan11_731_profile_id)
                                : Json(nullptr);

                return reply;
            }

            if (operation.at("op") == "wheel_pair") {
                sent.push_back(operation);
                assert(operation.at("ownership_id") == owner && operation.at("owner") == owner);

                return Json{{"ok", true},
                            {"session_id", id},
                            {"down", {{"accepted", true}}},
                            {"up", {{"accepted", true}}},
                            {"released", true},
                            {"guest_acknowledged", false},
                            {"visible_effect_verified", false}};
            }

            if (operation.at("op") == "wheel") {
                sent.push_back(operation);
            }

            return transport.control(endpoint, operation, timeout);
        };
        api::Api paired_api(root, "unused-cli", paired_transport);
        auto press = [&](int hold) {
            auto value = request("input.wheel.press");
            value["button"] = "volume_down";
            value["hold_ms"] = hold;

            return paired_api.call(value);
        };

        for (int hold : {1, 100, 200}) {
            sent.clear();
            const auto reply = press(hold);
            assert(reply.at("ok") == true && sent.size() == 1 && sent[0].at("op") == "wheel_pair" &&
                   sent[0].at("hold_ms") == hold && reply.at("result").at("released") == true &&
                   reply.at("result").at("guest_acknowledged") == false);
        }

        for (int hold : {201, 1300}) {
            sent.clear();
            assert(press(hold).at("ok") == true && sent.size() == 2 &&
                   sent[0].at("phase") == "down" && sent[1].at("phase") == "up");
        }

        for (const char *button : {"next", "previous", "ok", "photo", "voice", "phone"}) {
            sent.clear();
            auto value = request("input.wheel.press");
            value["button"] = button;
            value["hold_ms"] = 100;
            const auto reply = paired_api.call(value);
            assert(reply.at("ok") == true && sent.size() == 2 && sent[0].at("op") == "wheel" &&
                   sent[0].at("id") == button && sent[0].at("phase") == "down" &&
                   sent[1].at("phase") == "up");
        }

        partial = false;
        sent.clear();
        assert(press(1).at("ok") == true && sent.size() == 2 && sent[0].at("op") == "wheel" &&
               sent[1].at("op") == "wheel");
    }
#endif
#ifndef _WIN32
    wheel_release_admission_tests(base);
#endif
    phone_api_contract_tests(base);
    auto invalid = request("session.status");
    invalid["schema"] = 2;
    const auto schema_calls = calls;
    auto rejected_schema = api.call(invalid);
    assert(rejected_schema.at("schema") == 1 &&
           rejected_schema.at("error").at("code") == "TEST_SCHEMA" && calls == schema_calls);
    invalid = request("session.status");
    invalid["session"]["id"] = random_token();
    const auto before_calls = calls;
    assert(api.call(invalid).at("error").at("code") == "TEST_SESSION" && calls == before_calls);
    {
        unsigned launch_calls = 0;
        api::Transports launches = transport;
        launches.control = [&](const auto &, const Json &command, int) {
            ++launch_calls;
            assert(command.at("op") == "test_app_launch" && command.at("ownership_id") == owner &&
                   command.at("screen") == "primary");

            return Json{{"ok", true},
                        {"session_id", id},
                        {"accepted", true},
                        {"visible_effect_verified", false}};
        };
        api::Api launch_api(root, "unused-cli", launches);
        auto launch = request("multimedia.launch");
        launch["package"] = "org.example.app";
        launch["boot_id"] = "01234567-89ab-cdef-0123-456789abcdef";
        const auto accepted = launch_api.call(launch);
        assert(accepted.at("ok") == true && launch_calls == 1);
        assert(launch_api.call(launch) == accepted && launch_calls == 1);
        auto conflict = launch;
        conflict["package"] = "org.example.other";
        assert(launch_api.call(conflict).at("error").at("code") == "REQUEST_CONFLICT" &&
               launch_calls == 1);
        launch["request_id"] = random_token();
        launch["package"] = "org.example;id";
        assert(launch_api.call(launch).at("error").at("code") == "TEST_ARGUMENT" &&
               launch_calls == 1);
        launch["request_id"] = random_token();
        launch["package"] = "org.example.app";
        launch["screen"] = "cluster";
        assert(launch_api.call(launch).at("error").at("code") == "TEST_ARGUMENT" &&
               launch_calls == 1);
        launch["request_id"] = random_token();
        launch["screen"] = "primary";
        launch["session"].erase("ownership_id");
        assert(launch_api.call(launch).at("error").at("code") == "TEST_OWNERSHIP" &&
               launch_calls == 1);
        launch["session"]["ownership_id"] = owner;
        launch["request_id"] = random_token();
        launches.control = [&](const auto &, const Json &, int) -> Json {
            ++launch_calls;
            throw std::runtime_error("APP_LAUNCH_UNKNOWN: dispatch interrupted");
        };
        api::Api unknown_api(root, "unused-cli", launches);
        const auto uncertain = unknown_api.call(launch);
        assert(uncertain.at("error").at("outcome") == "unknown" &&
               uncertain.at("error").at("retryable") == false && launch_calls == 2);
        assert(unknown_api.call(launch) == uncertain && launch_calls == 2);
    }
    auto patch = request("vehicle.set");
    patch["expected_revision"] = session.handle({{"op", "status"}}).at("revision");
    patch["values"] = {{"speed", 87}, {"soc", 52}};
    auto patched = api.call(patch);
    assert(patched.at("ok") == true &&
           patched.at("result").at("state").at("raw_oem").at("speed10x") == 870);
    const auto replay_calls = calls;
    auto duplicate = api.call(patch);
    assert(duplicate == patched && calls == replay_calls);
    auto conflict = patch;
    conflict["values"]["speed"] = 88;
    assert(api.call(conflict).at("error").at("code") == "REQUEST_CONFLICT" &&
           calls == replay_calls);
    patch["request_id"] = random_token();
    auto stale = api.call(patch);
    assert(stale.at("ok") == false && stale.at("error").at("code") == "TEST_REVISION");
    assert(session.handle({{"op", "status"}}).at("state") == patched.at("result").at("state"));
    patch["request_id"] = random_token();
    patch["session"].erase("ownership_id");
    assert(api.call(patch).at("error").at("code") == "TEST_OWNERSHIP");
    patch["request_id"] = random_token();
    patch["session"]["ownership_id"] = random_token();
    assert(api.call(patch).at("error").at("code") == "TEST_OWNERSHIP");
    unsigned input_calls = 0;
    Json last_input;
    fixture_media_input = [&](const Json &input) {
        ++input_calls;
        last_input = input;

        return Json{{"ok", true},
                    {"sdk_completed", true},
                    {"pointer_released", true},
                    {"display_id", input.value("display_id", 0)},
                    {"visible_effect_verified", false}};
    };
    session.set_media_input(fixture_media_input);
    {
        const auto metadata = directory / "ui-observer.json";
        write_private_json(metadata, {{"schema", 1},
                                      {"session_id", id},
                                      {"android_pid", 123},
                                      {"connection",
                                       {{"boot_id", "fence-boot"},
                                        {"observer_pid", 777},
                                        {"auth", "private-fence-token"}}}});
        unsigned observations = 0, dispatches = 0;
        api::Transports fenced_transport = transport;
        fenced_transport.control = [&](const auto &path, const Json &operation, int timeout) {
            if (operation.at("op") == "media_input") {
                ++dispatches;
            }

            return transport.control(path, operation, timeout);
        };
        auto fenced_transport_guest = std::make_shared<api::ScriptedGuestAndroid>();
        fenced_transport.android = fenced_transport_guest;
        fenced_transport_guest->exchange = [&](const Json &, const Json &operation, int) {
            if (operation.at("op") == "hello") {
                return Json{{"ok", true}, {"boot_id", "fence-boot"}, {"observer_pid", 777}};
            }

            assert(operation.at("op") == "ui");
            ++observations;

            return Json{{"schema", 1},
                        {"ok", true},
                        {"boot_id", "fence-boot"},
                        {"observer_pid", 777},
                        {"observer_epoch", "fence-source"},
                        {"observation_id", "fresh"},
                        {"surfaces", Json::array({{{"surface", "media_primary"},
                                                   {"display_id", 0},
                                                   {"status", "ok"},
                                                   {"complete", true},
                                                   {"width", 1920},
                                                   {"height", 720},
                                                   {"nodes", Json::array()},
                                                   {"windows", Json::array()}}})}};
        };
        api::Api fenced_api(root, "unused-cli", fenced_transport);
        auto fenced = request("input.tap");
        fenced["screen"] = "primary";
        fenced["x"] = 30;
        fenced["y"] = 40;
        const Json bounds = {{"x", 10}, {"y", 20}, {"width", 100}, {"height", 40}};
        fenced["expected"] = {{"boot_id", "fence-boot"},
                              {"source_instance", "fence-source"},
                              {"revision", "old"},
                              {"window",
                               {{"id", 38},
                                {"type", 1},
                                {"bounds", bounds},
                                {"title", "private-title"},
                                {"package", "private-package"}}},
                              {"target",
                               {{"window_id", 38},
                                {"resource_id", "private-resource"},
                                {"package", "private-package"},
                                {"class", "Button"},
                                {"bounds", bounds},
                                {"clipped_bounds", bounds}}}};
        const auto rejected = fenced_api.call(fenced);
        assert(rejected.at("ok") == false);
        const auto &failure = rejected.at("error");
        assert(failure.at("code") == "TEST_INPUT_STALE" &&
               failure.at("message") ==
                       "TEST_INPUT_STALE: target identity or geometry changed before dispatch" &&
               failure.at("outcome") == "not_started" && failure.at("retryable") == false);
        assert(failure.at("details").at("reason") == "window_missing" &&
               failure.at("details").at("expected_window").at("id") == 38);
        assert(rejected.dump().find("private-") == std::string::npos && observations == 1 &&
               dispatches == 0 && input_calls == 0);
        assert(fenced_api.call(fenced) == rejected && observations == 1 && dispatches == 0);
        api::Api reopened(root, "unused-cli", fenced_transport);
        verify_terminal(reopened, fenced, rejected, "not_started");
        auto lookup = request("operation.get");
        lookup["target"] = {{"operation_id", rejected.at("operation_id")}};
        assert(reopened.call(lookup).at("result").at("response") == rejected);
        lookup["request_id"] = random_token();
        lookup["target"] = {{"request_id", fenced.at("request_id")}};
        assert(reopened.call(lookup).at("result").at("response") == rejected && observations == 1 &&
               dispatches == 0);
        std::filesystem::remove(metadata);
    }
    {
        auto fenced = request("input.tap");
        fenced["screen"] = "secondary";
        fenced["scope"] = "interactive";
        fenced["selector"] = {{"resource_id", "example:id/button"}};
        const auto before = input_calls;
        const auto response = api.call(fenced);
        assert(response.at("error").at("code") == "TEST_UNSUPPORTED" && input_calls == before);
    }
    {
        // Regression: the forced all_attached pre-dispatch read for secondary coordinate
        // drags marks the screen "partial" when a wallpaper window has no view hierarchy,
        // while the same guest still reports valid drag dimensions. Geometry, not tree
        // coverage, gates coordinate drags.
        const auto metadata = directory / "ui-observer.json";
        write_private_json(metadata, {{"schema", 1},
                                      {"session_id", id},
                                      {"android_pid", 123},
                                      {"connection",
                                       {{"boot_id", "drag-boot"},
                                        {"observer_pid", 777},
                                        {"auth", "private-drag-token"}}}});
        unsigned drag_observations = 0, drag_dispatches = 0;
        api::Transports drag_transport = transport;
        drag_transport.control = [&](const auto &path, const Json &operation, int timeout) {
            if (operation.at("op") == "media_input") {
                ++drag_dispatches;

                return Json{{"ok", true},
                            {"session_id", id},
                            {"sdk_completed", true},
                            {"pointer_released", true},
                            {"display_id", operation.at("input").value("display_id", 0)},
                            {"visible_effect_verified", false}};
            }

            return transport.control(path, operation, timeout);
        };
        auto drag_transport_guest = std::make_shared<api::ScriptedGuestAndroid>();
        drag_transport.android = drag_transport_guest;
        drag_transport_guest->exchange = [&](const Json &, const Json &operation, int) {
            if (operation.at("op") == "hello") {
                return Json{{"ok", true}, {"boot_id", "drag-boot"}, {"observer_pid", 777}};
            }

            assert(operation.at("op") == "ui");
            ++drag_observations;

            return Json{{"schema", 1},
                        {"ok", true},
                        {"boot_id", "drag-boot"},
                        {"observer_pid", 777},
                        {"observer_epoch", "drag-source"},
                        {"observation_id", "fresh-drag"},
                        {"surfaces", Json::array({{{"surface", "media_secondary"},
                                                   {"display_id", 1},
                                                   {"status", "partial"},
                                                   {"complete", false},
                                                   {"width", 1920},
                                                   {"height", 720},
                                                   {"nodes", Json::array()},
                                                   {"windows", Json::array()}}})}};
        };
        api::Api drag_api(root, "unused-cli", drag_transport);
        auto drag = request("input.drag");
        drag["screen"] = "secondary";
        drag["from"] = {{"x", 300}, {"y", 200}};
        drag["to"] = {{"x", 900}, {"y", 200}};
        drag["duration_ms"] = 500;
        const auto dragged = drag_api.call(drag);
        assert(dragged.at("ok") == true && drag_observations == 1 && drag_dispatches == 1);
        assert(dragged.at("result").at("screen") == "secondary" &&
               dragged.at("result").at("sdk_display_id") == 1);
        // Missing dimensions must still fail closed for drags.
        api::Transports geometry_transport = transport;
        geometry_transport.control = [&](const auto &path, const Json &operation, int timeout) {
            if (operation.at("op") == "media_input") {
                ++drag_dispatches;

                return Json{{"ok", true},
                            {"session_id", id},
                            {"sdk_completed", true},
                            {"pointer_released", true},
                            {"display_id", 1},
                            {"visible_effect_verified", false}};
            }

            return transport.control(path, operation, timeout);
        };
        auto geometry_transport_guest = std::make_shared<api::ScriptedGuestAndroid>();
        geometry_transport.android = geometry_transport_guest;
        geometry_transport_guest->exchange = [&](const Json &, const Json &operation, int) {
            if (operation.at("op") == "hello") {
                return Json{{"ok", true}, {"boot_id", "drag-boot"}, {"observer_pid", 777}};
            }

            return Json{{"schema", 1},
                        {"ok", true},
                        {"boot_id", "drag-boot"},
                        {"observer_pid", 777},
                        {"observer_epoch", "drag-source"},
                        {"observation_id", "fresh-drag"},
                        {"surfaces", Json::array({{{"surface", "media_secondary"},
                                                   {"display_id", 1},
                                                   {"status", "partial"},
                                                   {"complete", false},
                                                   {"nodes", Json::array()},
                                                   {"windows", Json::array()}}})}};
        };
        api::Api geometry_api(root, "unused-cli", geometry_transport);
        drag["request_id"] = random_token();
        const auto before_dispatches = drag_dispatches;
        const auto rejected = geometry_api.call(drag);
        assert(rejected.at("ok") == false &&
               rejected.at("error").at("code") == "TEST_INPUT_UNAVAILABLE" &&
               rejected.at("error").at("outcome") == "not_started" &&
               drag_dispatches == before_dispatches);

        if (rejected.dump().find("private-") != std::string::npos) {
            std::cerr << "privacy regression" << std::endl;
        }

        // Status unsupported, fractional, oversized, zero and out-of-range dimensions must all
        // fail closed before dispatch even when the numeric fields are present and positive.
        auto surface_with = [](Json changes) {
            Json surface{{"surface", "media_secondary"},
                         {"display_id", 1},
                         {"status", "partial"},
                         {"complete", false},
                         {"width", 1920},
                         {"height", 720},
                         {"nodes", Json::array()},
                         {"windows", Json::array()}};

            for (const auto &[key, value] : changes.items()) {
                surface[key] = value;
            }

            return Json{{"schema", 1},
                        {"ok", true},
                        {"boot_id", "drag-boot"},
                        {"observer_pid", 777},
                        {"observer_epoch", "drag-source"},
                        {"observation_id", "fresh-drag"},
                        {"surfaces", Json::array({surface})}};
        };

        for (const Json &rejection :
             {Json{{"status", "unsupported"}}, Json{{"width", 1920.5}}, Json{{"height", 720.5}},
              Json{{"width", 3841}}, Json{{"height", 2161}}, Json{{"width", 0}},
              Json{{"height", 0}}, Json{{"width", -1920}}, Json{{"height", -720}}}) {
            api::Transports reject_transport = transport;
            reject_transport.control = [&](const auto &path, const Json &operation, int timeout) {
                if (operation.at("op") == "media_input") {
                    ++drag_dispatches;
                }

                return transport.control(path, operation, timeout);
            };
            auto reject_transport_guest = std::make_shared<api::ScriptedGuestAndroid>();
            reject_transport.android = reject_transport_guest;
            reject_transport_guest->exchange = [&](const Json &, const Json &operation, int) {
                if (operation.at("op") == "hello") {
                    return Json{{"ok", true}, {"boot_id", "drag-boot"}, {"observer_pid", 777}};
                }

                return surface_with(rejection);
            };
            api::Api reject_api(root, "unused-cli", reject_transport);
            drag["request_id"] = random_token();
            const auto closed = reject_api.call(drag);
            assert(closed.at("ok") == false &&
                   closed.at("error").at("code") == "TEST_INPUT_UNAVAILABLE" &&
                   closed.at("error").at("outcome") == "not_started");
            assert(drag_dispatches == before_dispatches);
        }

        std::filesystem::remove(metadata);
    }
    auto pointer = request("input.gesture");
    pointer["screen"] = "secondary";
    pointer["points"] = Json::array(
            {{{"x", 100}, {"y", 200}, {"at_ms", 0}}, {{"x", 200}, {"y", 200}, {"at_ms", 100}}});
    auto input = api.call(pointer);
    assert(input.at("ok") == true && input_calls == 1 &&
           input.at("result").at("sdk_display_id") == 1 &&
           input.at("result").at("display_id").is_null() &&
           input.at("result").at("screen") == "secondary" &&
           input.at("result").at("visible_effect_verified") == false);
    {
        api::Api reopened(root, "unused-cli", transport);
        verify_terminal(reopened, pointer, input, "completed");
        assert(input.at("result").at("visible_effect_verified") == false);
        const auto receipt = root / ".local/build/automation/operations" /
                             (input.at("operation_id").get<std::string>() + ".json");
        auto old_mirror = read_private_json(receipt);
        assert(old_mirror.at("outcome") == "completed");
        old_mirror["outcome"] = "unknown";
        write_private_json(receipt, old_mirror);
        verify_terminal(reopened, pointer, input, "completed");
    }
    pointer["request_id"] = random_token();
    pointer["points"][1]["x"] = 1920;
    assert(api.call(pointer).at("error").at("code") == "TEST_ARGUMENT" && input_calls == 1);
    pointer["points"][1]["x"] = 200;
    pointer["request_id"] = random_token();
    pointer["points"][0]["at_ms"] = 1;
    assert(api.call(pointer).at("error").at("code") == "TEST_ARGUMENT" && input_calls == 1);
    auto tap = request("input.tap");
    tap["screen"] = "primary";
    tap["x"] = 30;
    tap["y"] = 40;
    auto tapped = api.call(tap);
    assert(tapped.at("ok") == true && input_calls == 2 &&
           tapped.at("result").at("sdk_display_id") == 0 &&
           tapped.at("result").at("display_id").is_null());
    {
        auto legacy = request("input.tap");
        legacy["surface"] = "media_primary";
        legacy["x"] = 30;
        legacy["y"] = 40;
        const auto old_calls = calls;
        assert(api.call(legacy).at("error").at("code") == "TEST_ARGUMENT" && calls == old_calls);
    }
    assert(last_input.at("display_width") == 1920 && last_input.at("display_height") == 720);
    tap["request_id"] = random_token();
    tap["x"] = 1920;
    const auto tap_calls = calls;
    auto invalid_tap = api.call(tap);
    // The status read gives the screen size; the coordinate outside it never reaches the guest.
    assert(invalid_tap.at("ok") == false && invalid_tap.at("request_id") == tap.at("request_id") &&
           invalid_tap.at("error").at("code") == "TEST_ARGUMENT" && calls == tap_calls + 1 &&
           input_calls == 2);
    {
        // A tap on the rear screen takes its emulator display and size from the screens list.
        status_screens = Json::array({{{"role", "primary"},
                                       {"guest", "android"},
                                       {"width", 1920},
                                       {"height", 720},
                                       {"density", 160}},
                                      {{"role", "secondary"},
                                       {"guest", "android"},
                                       {"width", 1920},
                                       {"height", 720},
                                       {"density", 160}},
                                      {{"role", "rear"},
                                       {"guest", "android"},
                                       {"width", 1902},
                                       {"height", 540},
                                       {"density", 160}}});
        auto rear = request("input.tap");
        rear["screen"] = "rear";
        rear["x"] = 1901;
        rear["y"] = 539;
        const auto rear_tap = api.call(rear);
        assert(rear_tap.at("ok") == true && input_calls == 3 &&
               rear_tap.at("result").at("sdk_display_id") == 2 &&
               last_input.at("display_id") == 2 && last_input.at("display_width") == 1902 &&
               last_input.at("display_height") == 540);
        rear["request_id"] = random_token();
        rear["y"] = 540;
        assert(api.call(rear).at("error").at("code") == "TEST_ARGUMENT" && input_calls == 3);
        auto hud = request("input.tap");
        hud["screen"] = "hud";
        hud["x"] = 1;
        hud["y"] = 1;
        assert(api.call(hud).at("error").at("code") == "TEST_UNSUPPORTED" && input_calls == 3);
        status_screens = nullptr;
    }
    {
        const auto writer_path = directory / "automation/writer.lock";
        auto writer = std::make_unique<api::FileLock>(writer_path);
        auto busy_input = request("input.tap");
        busy_input["screen"] = "primary";
        busy_input["x"] = 30;
        busy_input["y"] = 40;
        const auto busy_calls = calls, busy_inputs = input_calls;
        auto busy = api.call(busy_input);
        assert(busy.at("ok") == false && busy.at("error").at("code") == "TEST_BUSY" &&
               busy.at("error").at("outcome") == "not_started" && calls == busy_calls &&
               input_calls == busy_inputs);
        writer.reset();
        auto busy_lookup = request("operation.get");
        busy_lookup["target"] = {{"operation_id", busy.at("operation_id")}};
        auto by_operation = api.call(busy_lookup);
        assert(by_operation.at("ok") == true &&
               by_operation.at("result").at("status") == "terminal" &&
               by_operation.at("result").at("response") == busy);
        busy_lookup["target"] = {{"request_id", busy_input.at("request_id")}};
        auto by_request = api.call(busy_lookup);
        assert(by_request.at("ok") == true && by_request.at("result").at("status") == "terminal" &&
               by_request.at("result").at("response") == busy);
        assert(api.call(busy_input) == busy && calls == busy_calls && input_calls == busy_inputs);
    }
    {
        auto state_request = request("session.observe");
        state_request["timeout_ms"] = 1000;
        auto state = api.call(state_request);
        assert(state.at("ok") == true && state.at("result").at("complete") == false &&
               state.at("result").at("sources").at("can").at("status") == "unavailable" &&
               state.at("result").at("sources").at("system").at("value").is_null());
        auto selected_request = state_request;
        selected_request["sources"] = Json::array({"can"});
        const auto selected_state = api.call(selected_request);
        assert(selected_state.at("ok") == true &&
               selected_state.at("result").at("sources").size() == 1 &&
               selected_state.at("result").at("sources").contains("can"));

        for (const auto &selection :
             std::vector<Json>{Json::array(), Json::array({"can", "can"}), Json::array({7}),
                               Json("can"), Json(nullptr)}) {
            auto invalid = state_request;
            invalid["sources"] = selection;
            const auto prior = calls;
            assert(api.call(invalid).at("error").at("code") == "TEST_ARGUMENT" && calls == prior);
        }

        selected_request["sources"] = Json::array({"unknown"});
        assert(api.call(selected_request).at("error").at("code") == "TEST_ARGUMENT");
        auto system_request = request("multimedia.system");
        system_request["timeout_ms"] = 1000;
        auto system = api.call(system_request);
        assert(system.at("ok") == true && system.at("result").at("status") == "unavailable" &&
               system.at("result").at("value").is_null());
        auto watch = request("session.watch.start");
        watch["condition"] = {{"op", "present"}, {"path", "/sources/can/value"}};
        watch["timeout_ms"] = 1000;
        watch["lease_ms"] = 1000;
        auto invalid_watch = watch;
        invalid_watch["condition"] = {{"op", "not-a-condition"}};
        const auto validation_calls = calls;
        assert(api.call(invalid_watch).at("error").at("code") == "TEST_ARGUMENT" &&
               calls == validation_calls);

        for (const auto lease : {0, 60001}) {
            invalid_watch = watch;
            invalid_watch["lease_ms"] = lease;
            assert(api.call(invalid_watch).at("error").at("code") == "TEST_ARGUMENT" &&
                   calls == validation_calls);
        }

        auto subset_watch = watch;
        subset_watch["sources"] = Json::array({"system", "can"});
        const auto subset = api.call(subset_watch);
        assert(subset.at("ok") == true &&
               subset.at("result").at("baseline").at("sources").size() == 2 &&
               !subset.at("result").at("baseline").at("sources").contains("ui"));
        auto reordered = subset_watch;
        reordered["sources"] = Json::array({"can", "system"});
        assert(api.call(reordered).at("result").at("epoch") == subset.at("result").at("epoch"));
        auto baseline = api.call(watch);
        assert(baseline.at("ok") == true && baseline.at("result").at("status") == "unavailable" &&
               baseline.at("result").at("watch_id") == "" &&
               baseline.at("result").at("baseline").at("complete") == false);
        const auto epoch = baseline.at("result").at("epoch"),
                   cursor = baseline.at("result").at("cursor");
        assert(epoch.is_string() && cursor.is_string());
        auto read = request("session.watch.read");
        read["watch_id"] = "absent-watch";
        read["epoch"] = epoch;
        read["cursor"] = cursor;
        read["timeout_ms"] = 0;
        const auto limit_calls = calls;

        for (const auto limit : {0, 33}) {
            read["limit"] = limit;
            assert(api.call(read).at("error").at("code") == "TEST_ARGUMENT" &&
                   calls == limit_calls);
        }

        read["limit"] = 1;
        auto stale = read;
        stale["epoch"] = "stale-epoch";
        assert(api.call(stale).at("error").at("code") == "TEST_WATCH_EPOCH");
        auto ended = api.call(read);
        assert(ended.at("ok") == true && ended.at("result").at("status") == "ended" &&
               ended.at("result").at("cursor") == cursor &&
               ended.at("result").at("events").empty());
        auto stop = request("session.watch.stop");
        stop["watch_id"] = read.at("watch_id");
        stop["epoch"] = epoch;
        auto stopped_watch = api.call(stop);
        assert(stopped_watch.at("ok") == true &&
               stopped_watch.at("result").at("status") == "stopped" &&
               stopped_watch.at("result").at("epoch") == epoch);
        stop["epoch"] = "stale-epoch";
        assert(api.call(stop).at("error").at("code") == "TEST_WATCH_EPOCH");
    }
    {
        auto condition_wait = request("session.wait");
        condition_wait["condition"] = {{"op", "present"}, {"path", "/sources/can/value"}};
        condition_wait["timeout_ms"] = 1000;
        unsigned source_starts = 0, source_reads = 0, source_stops = 0;
        api::Transports absent_transport = transport;
        absent_transport.control = [&](const auto &endpoint, const Json &op, int timeout) {
            const auto name = op.at("op");

            if (name == "test_watch_start") {
                ++source_starts;
            }

            if (name == "test_watch_read") {
                ++source_reads;
            }

            if (name == "test_watch_stop") {
                ++source_stops;
            }

            return transport.control(endpoint, op, timeout);
        };
        api::Api absent_api(root, "unused-cli", absent_transport);
        auto absent = absent_api.call(condition_wait);
        assert(absent.at("ok") == true && absent.at("result").at("status") == "unavailable" &&
               !absent.at("result").contains("cleanup") && source_starts == 1 &&
               source_reads == 0 && source_stops == 0);
        auto invalid_wait = condition_wait;
        invalid_wait["condition"] = {{"op", "not-a-condition"}};
        const auto invalid_calls = calls;
        assert(absent_api.call(invalid_wait).at("error").at("code") == "TEST_ARGUMENT" &&
               calls == invalid_calls && source_starts == 1 && source_reads == 0 &&
               source_stops == 0);

        for (const std::string mode : {"passed", "gap", "timeout"}) {
            unsigned starts = 0, reads = 0, stops = 0;
            std::string opaque_cursor = "opaque-baseline";
            api::Transports injected = transport;
            injected.control = [&](const auto &, const Json &op, int) {
                const auto name = op.at("op");
                Json response{{"ok", true}, {"session_id", id}};

                if (name == "test_watch_start") {
                    ++starts;
                    assert(op.at("condition") == condition_wait.at("condition"));
                    response.update({{"status", "ready"},
                                     {"watch_id", "injected-watch"},
                                     {"epoch", "injected-epoch"},
                                     {"cursor", opaque_cursor},
                                     {"result", {{"status", "pending"}}}});
                } else if (name == "test_watch_read") {
                    ++reads;
                    assert(op.at("watch_id") == "injected-watch" &&
                           op.at("epoch") == "injected-epoch" && op.at("cursor") == opaque_cursor &&
                           op.at("limit") == 1);
                    response.update(
                            {{"status", "ready"},
                             {"cursor", "opaque-next"},
                             {"gap", mode == "gap"},
                             {"result", {{"status", mode == "passed" ? "passed" : "pending"}}}});
                    opaque_cursor = "opaque-next";

                    if (mode == "timeout") {
                        std::mutex timeout_mutex;
                        std::condition_variable timeout_changed;
                        std::unique_lock timeout_lock(timeout_mutex);
                        timeout_changed.wait_for(
                                timeout_lock,
                                std::chrono::milliseconds(op.at("timeout_ms").get<int>() + 1), [] {
                                    return false;
                                });
                    }
                } else {
                    assert(name == "test_watch_stop");
                    ++stops;
                    assert(op.at("watch_id") == "injected-watch" &&
                           op.at("epoch") == "injected-epoch");
                    response.update({{"status", "stopped"}});
                }

                return response;
            };
            api::Api injected_api(root, "unused-cli", injected);
            auto bounded_wait = condition_wait;
            bounded_wait["request_id"] = random_token();
            bounded_wait["timeout_ms"] = 1000;
            auto result = injected_api.call(bounded_wait);
            assert(result.at("ok") == true && starts == 1 && reads == 1 && stops == 1 &&
                   result.at("result").at("cleanup").at("status") == "stopped");

            if (mode == "passed") {
                assert(result.at("result").at("result").at("status") == "passed");
            } else if (mode == "gap") {
                assert(result.at("result").at("gap") == true);
            } else {
                assert(result.at("result").at("timed_out") == true);
            }
        }
    }
    {
        const Json unavailable_provider{{"status", "unavailable"},
                                        {"value", nullptr},
                                        {"coverage", {{"continuous", false}}}};
        const Json state{{"complete", false},
                         {"epoch", "provider-epoch"},
                         {"atomic", false},
                         {"sources", {{"custom-id", unavailable_provider}}}};
        unsigned provider_calls = 0;
        api::Transports provider_transport = transport;
        provider_transport.control = [&](const auto &, const Json &op, int timeout) {
            ++provider_calls;
            assert(op.at("op") == "test_output_state" &&
                   op.at("sources") == Json::array({"custom-id"}) && op.at("timeout_ms") == 37 &&
                   timeout == 537);

            return Json{{"ok", true}, {"session_id", id}, {"state", state}};
        };
        api::Api provider_api(root, "unused-cli", provider_transport);
        auto provider_request = request("session.observe");
        provider_request["sources"] = Json::array({"custom-id"});
        provider_request["timeout_ms"] = 37;
        auto provider = provider_api.call(provider_request);
        assert(provider.at("ok") == true && provider.at("result") == state && provider_calls == 1);
        assert(provider_api.call(request("output.custom-id")).at("error").at("code") ==
                       "TEST_UNSUPPORTED" &&
               provider_calls == 1);
    }
    {
        const std::string boot = "11111111-1111-4111-8111-111111111111",
                          new_boot = "22222222-2222-4222-8222-222222222222";
        const Json expected_boot = {{"android", boot}, {"qnx_generation", "4"}, {"qnx_pid", 200}};
        const Json sealed = {{"schema", 1},
                             {"producer", "closed_ecu_boundary"},
                             {"status", 0},
                             {"boot_uuid", boot},
                             {"first_open", true},
                             {"valid", true},
                             {"all_writers_quiesced", true},
                             {"stage", "frozen"},
                             {"mode", 2},
                             {"first_mode", 2},
                             {"session", "11"},
                             {"epoch", "12"},
                             {"nonce", "13"},
                             {"kernel_open_seq", "1"},
                             {"commit_seq", "4"},
                             {"on_transition_count", "2"},
                             {"first_commit_boottime_ns", "100"},
                             {"fence_seq", "4"},
                             {"fence_boottime_ns", "200000000"},
                             {"expires_boottime_ns", "900000000"},
                             {"capture_boottime_ns", "300000000"},
                             {"raw_admission_seq", "5"},
                             {"raw_queued", "0"},
                             {"raw_inflight_seq", "0"},
                             {"loss_count", "0"},
                             {"generated_rx_seq", "6"},
                             {"fence_generated_rx_seq", "6"},
                             {"semantic_queue", "0"}};
        auto anchor = sealed;
        anchor.update({{"capture_boottime_ns", "150000000"},
                       {"stage", "none"},
                       {"nonce", "0"},
                       {"all_writers_quiesced", false},
                       {"fence_seq", "0"},
                       {"fence_boottime_ns", "0"},
                       {"expires_boottime_ns", "0"},
                       {"fence_generated_rx_seq", "0"},
                       {"session_id", id},
                       {"owned_android_pid", 100},
                       {"guest_boot_id", boot},
                       {"capability", "closed_ecu_boundary_v1"}});
        const Json expected_can = {{"anchor", anchor}, {"native_terminal_boottime_ms", "151"}};
        auto fresh = sealed;
        fresh.update({{"boot_uuid", new_boot},
                      {"stage", "none"},
                      {"nonce", "0"},
                      {"session", "21"},
                      {"epoch", "1"},
                      {"on_transition_count", "0"},
                      {"all_writers_quiesced", false},
                      {"fence_seq", "0"},
                      {"fence_boottime_ns", "0"},
                      {"expires_boottime_ns", "0"},
                      {"fence_generated_rx_seq", "0"}});
        const Json unavailable = {{"schema", 1},
                                  {"producer", "closed_ecu_boundary"},
                                  {"capability", "closed_ecu_boundary_v1"},
                                  {"session_id", id},
                                  {"status", "unavailable"},
                                  {"reason", "sealed helper unavailable"}};
        const auto absent = session.handle({{"op", "test_can_history"}, {"session_id", id}});
        assert(absent.at("ok") == true && absent.at("observation").at("status") == "unavailable" &&
               !absent.at("observation").contains("loss_count"));
        unsigned reads = 0, restarts = 0;
        bool missing = false, lost = false;
        api::Transports history_transport = transport;
        history_transport.control = [&](const auto &, const Json &command, int timeout) -> Json {
            if (command.at("op") == "test_can_history") {
                assert(!command.contains("ownership_id") && command.at("timeout_ms") == 5000 &&
                       timeout == 5500);
                ++reads;

                return {{"ok", true},
                        {"session_id", id},
                        {"observation", missing ? unavailable : anchor}};
            }

            assert(command.at("op") == "machine_restart" &&
                   command.at("expected_can") == expected_can &&
                   command.at("ownership_id") == owner);
            ++restarts;
            auto before = restart_boot_identity(boot, 4, 200, true);
            before["android_pid"] = 100;
            Json proof = expected_can;
            proof.update({{"prepared", sealed},
                          {"before_qnx_stop", sealed},
                          {"before_android_stop", sealed},
                          {"after", fresh},
                          {"old_kernel_stopped", true}});
            Json receipt = {{"schema", 1},
                            {"ok", true},
                            {"session_id", id},
                            {"operation_id", command.at("operation_id")},
                            {"request_id", command.at("request_id")},
                            {"status", "completed"},
                            {"accepted", true},
                            {"mode", "cold"},
                            {"scope", "android_qnx_cold_boot_changed"},
                            {"before", before},
                            {"after", restart_boot_identity(new_boot, 5, 201, true)},
                            {"guest_acknowledged", true},
                            {"old_processes_stopped", true},
                            {"authority_retained", true},
                            {"can_boundary", proof}};
            write_private_json(directory /
                                       ("full-restart-" +
                                        command.at("operation_id").get<std::string>() + ".json"),
                               receipt);

            if (lost) {
                throw std::runtime_error(
                        "SESSION_RESTART_UNKNOWN: IPC response lost after actual restart");
            }

            return receipt;
        };
        api::Api history_api(root, "unused-cli", history_transport);
        auto inspect = request("vehicle.can.history");
        inspect["session"].erase("ownership_id");
        const auto available = history_api.call(inspect);
        assert(available.at("ok") == true && available.at("result") == anchor &&
               api::result_exit_code(available) == 0 && reads == 1 && restarts == 0);
        missing = true;
        const auto unavailable_result = history_api.call(inspect);
        assert(unavailable_result.at("ok") == true &&
               unavailable_result.at("result") == unavailable && reads == 2);
        inspect["nonce"] = "13";
        assert(history_api.call(inspect).at("error").at("code") == "TEST_ARGUMENT" && reads == 2);
        unsigned ipk_reads = 0;
        api::Transports ipk_transport = transport;
        ipk_transport.control = [&](const auto &, const Json &command, int timeout) -> Json {
            assert(command.at("op") == "test_ipk_history" && !command.contains("ownership_id") &&
                   command.at("query") == Json({{"schema", 1}, {"limit", 2}}) &&
                   command.at("timeout_ms") == 5000 && timeout == 5250);
            ++ipk_reads;

            return {{"ok", true},
                    {"session_id", id},
                    {"observation",
                     {{"schema", 1},
                      {"source", "android:qg.canbus:ipk"},
                      {"status", "not-ready"}}}};
        };
        api::Api ipk_api(root, "unused-cli", ipk_transport);
        auto ipk = request("cluster.ipk.history");
        ipk["session"].erase("ownership_id");
        ipk["limit"] = 2;
        const auto ipk_result = ipk_api.call(ipk);
        assert(ipk_result.at("ok") == true && ipk_result.at("result").at("status") == "not-ready" &&
               ipk_reads == 1);

        for (const auto &[key, value, code] :
             {std::tuple<const char *, Json, const char *>{"nonce", "1", "TEST_ARGUMENT"},
              {"limit", 257, "IPK_ARGUMENT"},
              {"cursor", Json{{"source_instance", "x"}}, "IPK_ARGUMENT"},
              {"timeout_ms", 10001, "TEST_ARGUMENT"}}) {
            auto bad = ipk;
            bad[key] = value;
            assert(ipk_api.call(bad).at("error").at("code") == code && ipk_reads == 1);
        }

        for (const char *removed :
             {"output.can_boundary", "session.restart.prepare", "session.restart.abort"}) {
            assert(history_api.call(request(removed)).at("error").at("code") ==
                           "TEST_UNSUPPORTED" &&
                   restarts == 0);
        }

        auto restart = request("session.restart");
        restart["expected_boot"] = expected_boot;
        restart["expected_can"] = expected_can;
        auto bad = restart;
        bad["request_id"] = random_token();
        bad["expected_can"]["native_terminal_boottime_ms"] = "150";
        assert(history_api.call(bad).at("error").at("code") == "CAN_BOUNDARY_UNVERIFIED" &&
               restarts == 0);
        const auto complete = history_api.call(restart);
        assert(complete.at("ok") == true &&
               complete.at("result").at("can_boundary").at("anchor") == anchor && restarts == 1);
        lost = true;
        restart["request_id"] = random_token();
        const auto unknown = history_api.call(restart);
        assert(unknown.at("error").at("outcome") == "unknown" && restarts == 2);
        auto lookup = request("operation.get");
        lookup["target"] = {{"request_id", restart.at("request_id")}};
        const auto recovered = history_api.call(lookup);
        assert(recovered.at("ok") == true &&
               recovered.at("result").at("runtime_outcome") == "completed" && restarts == 2);
    }
    {
        const std::string old_boot = "11111111-1111-4111-8111-111111111111",
                          new_boot = "22222222-2222-4222-8222-222222222222";
        const Json expected{{"android", old_boot}, {"qnx_generation", "4"}, {"qnx_pid", 200}};
        const Json unavailable{{"schema", 1},
                               {"op", "machine_restart"},
                               {"session_id", id},
                               {"ownership_id", owner},
                               {"operation_id", random_token()},
                               {"request_id", "unavailable-full-restart"},
                               {"expected_boot", expected},
                               {"timeout_ms", 180000}};
        const auto refused = session.handle(unavailable);
        assert(refused.at("ok") == false &&
               refused.at("error").get<std::string>().starts_with("SESSION_RESTART_UNAVAILABLE:") &&
               !session.stopped() &&
               !std::filesystem::exists(
                       directory / ("full-restart-" +
                                    unavailable.at("operation_id").get<std::string>() + ".json")));
        unsigned restarts = 0;
        bool lose_response = false, partial = false;
        int restart_timeout = 180000;
        api::Transports restart_transport = transport;
        restart_transport.control = [&](const auto &, const Json &operation, int timeout) {
            assert(operation.at("op") == "machine_restart" && operation.at("schema") == 1 &&
                   operation.at("session_id") == id && operation.at("ownership_id") == owner &&
                   operation.at("expected_boot") == expected && timeout == restart_timeout);
            ++restarts;
            Json receipt{{"schema", 1},
                         {"ok", true},
                         {"session_id", id},
                         {"operation_id", operation.at("operation_id")},
                         {"request_id", operation.at("request_id")},
                         {"status", "completed"},
                         {"accepted", true},
                         {"mode", "cold"},
                         {"scope", "android_qnx_cold_boot_changed"},
                         {"before", restart_boot_identity(old_boot, 4, 200, true)},
                         {"after", restart_boot_identity(new_boot, partial ? 4 : 5, 201, true)},
                         {"guest_acknowledged", true},
                         {"old_processes_stopped", true},
                         {"authority_retained", true},
                         {"application_ready", false}};
            write_private_json(directory /
                                       ("full-restart-" +
                                        operation.at("operation_id").get<std::string>() + ".json"),
                               receipt);

            if (lose_response) {
                throw std::runtime_error(
                        "SESSION_RESTART_UNKNOWN: IPC response lost after actual restart");
            }

            return receipt;
        };
        api::Api restart_api(root, "unused-cli", restart_transport);
        auto restart = request("session.restart");
        restart["expected_boot"] = expected;
        const auto completed = restart_api.call(restart);
        assert(completed.at("ok") == true &&
               completed.at("result").at("authority_retained") == true && restarts == 1);
        assert(restart_api.call(restart) == completed && restarts == 1);
        auto lookup = request("operation.get");
        lookup["target"] = {{"operation_id", completed.at("operation_id")}};
        const auto complete_lookup = restart_api.call(lookup);
        assert(complete_lookup.at("ok") == true &&
               complete_lookup.at("result").at("runtime_outcome") == "completed" && restarts == 1);
        const auto no_calls = restarts;

        for (const char *key : {"mode", "with_cluster", "shell", "machine_path"}) {
            auto bad = restart;
            bad["request_id"] = random_token();
            bad[key] = "foreign";
            assert(restart_api.call(bad).at("error").at("code") == "TEST_ARGUMENT" &&
                   restarts == no_calls);
        }

        for (const Json value : std::vector<Json>{0, 180001, "1000", 1.5}) {
            auto bad = restart;
            bad["request_id"] = random_token();
            bad["timeout_ms"] = value;
            assert(restart_api.call(bad).at("error").at("code") == "TEST_ARGUMENT" &&
                   restarts == no_calls);
        }

        for (const Json value : std::vector<Json>{0, "0", "04", "18446744073709551616"}) {
            auto bad = restart;
            bad["request_id"] = random_token();
            bad["expected_boot"]["qnx_generation"] = value;
            assert(restart_api.call(bad).at("error").at("code") == "TEST_ARGUMENT" &&
                   restarts == no_calls);
        }

        auto readonly = restart;
        readonly["request_id"] = random_token();
        readonly["session"].erase("ownership_id");
        assert(restart_api.call(readonly).at("error").at("code") == "TEST_OWNERSHIP" &&
               restarts == no_calls);
        readonly["request_id"] = random_token();
        readonly["session"]["ownership_id"] = random_token();
        assert(restart_api.call(readonly).at("error").at("code") == "TEST_OWNERSHIP" &&
               restarts == no_calls);
        partial = true;
        auto half = restart;
        half["request_id"] = random_token();
        auto rejected = restart_api.call(half);
        assert(!rejected.at("ok").get<bool>() &&
               rejected.at("error").at("code") == "SESSION_RESTART_UNKNOWN" &&
               rejected.at("error").at("outcome") == "unknown");
        partial = false;
        lose_response = true;
        auto lost = restart;
        lost["request_id"] = random_token();
        const auto unknown = restart_api.call(lost);
        const auto dispatched = restarts;
        assert(unknown.at("error").at("outcome") == "unknown" &&
               unknown.at("error").at("retryable") == false);
        assert(restart_api.call(lost) == unknown && restarts == dispatched);
        api::Api resumed(root, "unused-cli", restart_transport);
        lookup["target"] = {{"request_id", lost.at("request_id")}};
        const auto recovered = resumed.call(lookup);
        assert(recovered.at("ok") == true &&
               recovered.at("result").at("runtime_outcome") == "completed" &&
               recovered.at("result").at("runtime_receipt").at("operation_id") ==
                       unknown.at("operation_id") &&
               restarts == dispatched);
        auto saved_owner = read_private_json(directory / "automation-owner.json");
        auto changed_owner = saved_owner;
        changed_owner["ownership_id"] = random_token();
        write_private_json(directory / "automation-owner.json", changed_owner);
        lookup["session"]["ownership_id"] = changed_owner.at("ownership_id");
        assert(resumed.call(lookup).at("error").at("code") == "TEST_SESSION" &&
               restarts == dispatched);
        write_private_json(directory / "automation-owner.json", saved_owner);
    }
    {
        unsigned reboot_calls = 0;
        int expected_reboot_timeout = 180000;
        bool reboot_unknown = false;
        api::Transports reboot_transport = transport;
        reboot_transport.control = [&](const auto &, const Json &op, int timeout) {
            ++reboot_calls;
            assert(op.at("op") == "android_reboot" && op.at("schema") == 1 &&
                   op.at("session_id") == id && op.at("ownership_id") == owner &&
                   timeout == expected_reboot_timeout);

            if (reboot_unknown) {
                throw std::runtime_error("ANDROID_REBOOT_UNKNOWN: dispatch outcome is unknown");
            }

            return Json{{"ok", true},
                        {"session_id", id},
                        {"mode", op.at("mode")},
                        {"accepted", true},
                        {"visible_effect_verified", false}};
        };
        api::Api reboot_api(root, "unused-cli", reboot_transport);

        for (const std::string mode : {"warm", "cold"}) {
            auto reboot = request("multimedia.reboot");
            reboot["mode"] = mode;
            auto result = reboot_api.call(reboot);
            assert(result.at("ok") == true && result.at("result").at("mode") == mode &&
                   result.at("result").at("visible_effect_verified") == false);
            const auto replay_calls = reboot_calls;
            assert(reboot_api.call(reboot) == result && reboot_calls == replay_calls);
        }

        auto custom_deadline = request("multimedia.reboot");
        custom_deadline["mode"] = "warm";
        custom_deadline["timeout_ms"] = 1234;
        expected_reboot_timeout = 1234;
        assert(reboot_api.call(custom_deadline).at("ok") == true);
        expected_reboot_timeout = 180000;
        const auto deadline_calls = reboot_calls;

        for (const Json invalid_deadline : std::vector<Json>{0, 180001, 1.5, "1000"}) {
            auto invalid = request("multimedia.reboot");
            invalid["mode"] = "warm";
            invalid["timeout_ms"] = invalid_deadline;
            assert(reboot_api.call(invalid).at("error").at("code") == "TEST_ARGUMENT" &&
                   reboot_calls == deadline_calls);
        }

        const auto before_invalid = reboot_calls;
        auto invalid_reboot = request("multimedia.reboot");
        invalid_reboot["mode"] = "restore";
        assert(reboot_api.call(invalid_reboot).at("error").at("code") == "TEST_ARGUMENT" &&
               reboot_calls == before_invalid);
        invalid_reboot["request_id"] = random_token();
        invalid_reboot["mode"] = "warm";
        invalid_reboot["session"].erase("ownership_id");
        assert(reboot_api.call(invalid_reboot).at("error").at("code") == "TEST_OWNERSHIP" &&
               reboot_calls == before_invalid);
        invalid_reboot["request_id"] = random_token();
        invalid_reboot["session"]["ownership_id"] = random_token();
        assert(reboot_api.call(invalid_reboot).at("error").at("code") == "TEST_OWNERSHIP" &&
               reboot_calls == before_invalid);
        reboot_unknown = true;
        auto uncertain_reboot = request("multimedia.reboot");
        uncertain_reboot["mode"] = "cold";
        auto uncertain = reboot_api.call(uncertain_reboot);
        assert(uncertain.at("ok") == false &&
               uncertain.at("error").at("code") == "ANDROID_REBOOT_UNKNOWN" &&
               uncertain.at("error").at("outcome") == "unknown" &&
               uncertain.at("error").at("retryable") == false &&
               reboot_calls == before_invalid + 1);
        assert(reboot_api.call(uncertain_reboot) == uncertain &&
               reboot_calls == before_invalid + 1);
        api::Api reopened(root, "unused-cli", reboot_transport);
        verify_terminal(reopened, uncertain_reboot, uncertain, "unknown");
        auto lookup = request("operation.get");
        lookup["target"] = {{"operation_id", uncertain.at("operation_id")}};
        const auto receipt = reboot_api.call(lookup).at("result");
        assert(receipt.at("op") == "multimedia.reboot" && receipt.at("response") == uncertain);
        lookup["target"] = {{"request_id", uncertain_reboot.at("request_id")}};
        assert(reboot_api.call(lookup).at("result").at("response") == uncertain);
    }
    auto grouped = api.call(request("info"));
    const auto &operations = grouped.at("result").at("operations");
    const std::set<std::string> expected_operations = {"info",
                                                       "profile.list",
                                                       "profile.get",
                                                       "session.start",
                                                       "session.attach",
                                                       "session.status",
                                                       "session.stop",
                                                       "session.restart",
                                                       "session.wait",
                                                       "session.watch.start",
                                                       "session.watch.read",
                                                       "session.watch.stop",
                                                       "session.ui",
                                                       "session.screenshot",
                                                       "session.logs",
                                                       "session.observe",
                                                       "multimedia.power",
                                                       "multimedia.reboot",
                                                       "multimedia.launch",
                                                       "multimedia.system",
                                                       "multimedia.phone.configure",
                                                       "multimedia.phone.connect",
                                                       "multimedia.phone.disconnect",
                                                       "multimedia.phone.incoming",
                                                       "multimedia.phone.answer",
                                                       "multimedia.phone.hangup",
                                                       "multimedia.phone.state",
                                                       "cluster.power",
                                                       "cluster.reboot",
                                                       "cluster.system",
                                                       "cluster.ipk.history",
                                                       "vehicle.fields",
                                                       "vehicle.get",
                                                       "vehicle.set",
                                                       "vehicle.can.get",
                                                       "vehicle.can.model",
                                                       "vehicle.can.history",
                                                       "input.tap",
                                                       "input.drag",
                                                       "input.gesture",
                                                       "input.key.press",
                                                       "input.wheel.buttons",
                                                       "input.wheel.press",
                                                       "input.wheel.release",
                                                       "input.voice.trigger",
                                                       "operation.get",
                                                       "test.run"};
    assert(operations.size() == expected_operations.size() &&
           operations.get<std::set<std::string>>() == expected_operations);
    assert(!grouped.at("result").contains("dynamic_operations"));
    assert(api.call(request("vehicle.get")).at("ok") == true);
    const auto voice_calls = calls;
    assert(api.call(request("input.voice.text")).at("error").at("code") == "TEST_UNSUPPORTED" &&
           calls == voice_calls);
    auto wait = request("session.wait");
    wait["predicate"] = "android.ready";
    wait["timeout_ms"] = 1000;
    assert(api.call(wait).at("result").at("ready") == true);
    wait["timeout_ms"] = 1;
    ready = false;
    auto waiting = api.call(wait);
    assert(waiting.at("result").at("timed_out") == true && api::result_exit_code(waiting) == 4);
    assert(waiting.at("result").at("exported_frame_verified") == false);
    ready = true;
    {
        // Read-only status transport failures consume the outer wait, not its first RPC.
        auto pending = wait;
        pending["timeout_ms"] = 1000;

        for (const char *code :
             {"IPC_TIMEOUT_UNKNOWN", "IPC_CONNECT", "IPC_READ_UNKNOWN", "IPC_WRITE_UNKNOWN"}) {
            unsigned attempts = 0;
            auto delayed = transport;
            delayed.control = [&](const auto &endpoint, const Json &command, int timeout) -> Json {
                ++attempts;
                assert(command.at("op") == "status" && !command.contains("ownership_id") &&
                       timeout > 0 && timeout <= 1000);

                if (attempts == 1) {
                    throw std::runtime_error(std::string(code) + ": bounded fixture interruption");
                }

                return transport.control(endpoint, command, timeout);
            };
            api::Api delayed_api(root, "unused-cli", delayed);
            pending["request_id"] = random_token();
            const auto result = delayed_api.call(pending);
            assert(result.at("ok") == true && result.at("result").at("ready") == true &&
                   attempts == 2 && result.at("result").at("transport_failures") == 1);
        }

        unsigned attempts = 0;
        auto unavailable = transport;
        unavailable.control = [&](const auto &, const Json &command, int) -> Json {
            ++attempts;
            assert(command.at("op") == "status");
            throw std::runtime_error("IPC_TIMEOUT_UNKNOWN: unavailable fixture");
        };
        api::Api unavailable_api(root, "unused-cli", unavailable);
        pending["request_id"] = random_token();
        pending["timeout_ms"] = 500;
        const auto started = std::chrono::steady_clock::now();
        const auto timed = unavailable_api.call(pending);
        assert(timed.at("ok") == true && timed.at("result").at("ready") == false &&
               timed.at("result").at("timed_out") == true &&
               timed.at("result").at("observation_status") == "unavailable" &&
               timed.at("result").at("outcome") == "unknown" && attempts > 1 &&
               api::result_exit_code(timed) == 4);
        assert(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(450));

        for (const char *code : {"TEST_SESSION", "TEST_CONTROL", "IPC_PROTOCOL"}) {
            attempts = 0;
            auto fatal = transport;
            fatal.control = [&](const auto &, const Json &, int) -> Json {
                ++attempts;
                throw std::runtime_error(std::string(code) + ": fixture refusal");
            };
            api::Api fatal_api(root, "unused-cli", fatal);
            pending["request_id"] = random_token();
            const auto rejected = fatal_api.call(pending);
            assert(rejected.at("ok") == false && rejected.at("error").at("code") == code &&
                   attempts == 1);
        }

        attempts = 0;
        auto terminal = transport;
        terminal.control = [&](const auto &, const Json &command, int) -> Json {
            ++attempts;
            assert(command.at("op") == "status");

            return Json{{"ok", true}, {"session_id", id}, {"stopped", true}};
        };
        api::Api terminal_api(root, "unused-cli", terminal);
        pending["request_id"] = random_token();
        const auto stopped = terminal_api.call(pending);
        assert(stopped.at("ok") == true && stopped.at("result").at("ready") == false &&
               stopped.at("result").at("terminal") == true && attempts == 1 &&
               api::result_exit_code(stopped) == 4);
        auto late = transport;
        late.control = [&](const auto &endpoint, const Json &command, int timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout + 10));

            return transport.control(endpoint, command, timeout);
        };
        api::Api late_api(root, "unused-cli", late);
        pending["request_id"] = random_token();
        pending["timeout_ms"] = 10;
        const auto expired = late_api.call(pending);
        assert(expired.at("ok") == true && expired.at("result").at("ready") == false &&
               expired.at("result").at("timed_out") == true);
        pending["timeout_ms"] = 80;
        const auto active = root / ".local/machines" / name / "active" / (name + ".json");
        const auto before = read_private_json(active);
        attempts = 0;
        auto stale = transport;
        stale.control = [&](const auto &, const Json &, int) -> Json {
            ++attempts;
            auto changed = before;
            changed["session_id"] = random_token();
            write_private_json(active, changed);
            throw std::runtime_error("IPC_TIMEOUT_UNKNOWN: epoch changed during observation");
        };
        api::Api stale_api(root, "unused-cli", stale);
        pending["request_id"] = random_token();
        const auto changed = stale_api.call(pending);
        write_private_json(active, before);
        assert(changed.at("ok") == false && changed.at("error").at("code") == "TEST_SESSION" &&
               attempts == 1);
        attempts = 0;
        stale.control = [&](const auto &endpoint, const Json &command, int timeout) {
            ++attempts;
            const auto reply = transport.control(endpoint, command, timeout);
            auto replaced = before;
            replaced["session_id"] = random_token();
            write_private_json(active, replaced);

            return reply;
        };
        api::Api replaced_api(root, "unused-cli", stale);
        pending["request_id"] = random_token();
        const auto replaced = replaced_api.call(pending);
        write_private_json(active, before);
        assert(replaced.at("ok") == false && replaced.at("error").at("code") == "TEST_SESSION" &&
               attempts == 1);
    }
    const auto unsupported_output = root / ".local/build/tmp/android-sequence-unsupported";
    {
        auto filtered = request("session.screenshot");
        filtered["after_sequence"] = 1;
        filtered["artifact_dir"] = unsupported_output.string();
        const auto old_calls = calls, old_captures = captures.load();
        auto unsupported = shots.call(filtered);
        assert(unsupported.at("ok") == false &&
               unsupported.at("error").at("code") == "TEST_UNSUPPORTED" &&
               unsupported.at("error").at("outcome") == "not_started" &&
               api::result_exit_code(unsupported) == 3);
        assert(calls == old_calls && captures == old_captures &&
               !std::filesystem::exists(unsupported_output));
    }

    for (const char *old_name : {"control.info",
                                 "control.profile.list",
                                 "control.profile.get",
                                 "control.session.start",
                                 "control.session.attach",
                                 "control.session.stop",
                                 "control.vehicle.fields",
                                 "control.vehicle.get",
                                 "control.vehicle.set",
                                 "control.cluster.power",
                                 "control.cluster.restart",
                                 "control.wait",
                                 "input.pointer.tap",
                                 "input.pointer.gesture",
                                 "output.tree",
                                 "output.status",
                                 "test.status",
                                 "control.reachable",
                                 "input.key",
                                 "api.info",
                                 "api.capabilities",
                                 "capabilities",
                                 "profiles",
                                 "session.create",
                                 "capture",
                                 "capture.pair",
                                 "output.screen.capture",
                                 "output.logs.read",
                                 "diagnostics",
                                 "operation.status",
                                 "run",
                                 "wait",
                                 "watch.start",
                                 "watch.read",
                                 "watch.stop",
                                 "cluster.ui",
                                 "multimedia.ui",
                                 "cluster.restart",
                                 "session.restart.prepare",
                                 "session.restart.abort",
                                 "output.capture",
                                 "output.logs",
                                 "output.can",
                                 "output.can_model",
                                 "output.can_boundary",
                                 "output.system",
                                 "output.state",
                                 "output.phone",
                                 "output.custom-id",
                                 "input.phone.configure",
                                 "input.phone.connect",
                                 "input.phone.disconnect",
                                 "input.phone.incoming",
                                 "input.phone.answer",
                                 "input.phone.hangup"}) {
        const auto old_calls = calls;
        const auto rejected = api.call(request(old_name));
        assert(rejected.at("ok") == false &&
               rejected.at("error").at("code") == "TEST_UNSUPPORTED" && calls == old_calls);
    }

#ifdef _WIN32
    {
        // Status fences use control IPC; boot identity and the frame use the guest. Every call
        // receives the remaining part of one caller budget, which never grows; the budget leaves
        // room for host PNG encoding on slow runners.
        unsigned control_calls = 0, guest_calls = 0;
        int budget = 4000;
        auto bounded_budget = [&](int timeout) {
            assert(timeout > 0 && timeout <= budget);
            budget = timeout;
        };
        auto bounded_transport = shot_transport;
        bounded_transport.control = [&](const auto &endpoint, const Json &command, int timeout) {
            ++control_calls;
            bounded_budget(timeout);
            assert(command.at("timeout_ms").get<unsigned>() <= static_cast<unsigned>(timeout));

            return transport.control(endpoint, command, timeout);
        };
        auto bounded_guest = std::make_shared<api::ScriptedGuestAndroid>();
        bounded_guest->identity = [&](int timeout) {
            ++guest_calls;
            bounded_budget(timeout);

            return shot->identity(timeout);
        };
        bounded_guest->capture = [&](const std::string &epoch, unsigned display, int timeout) {
            ++guest_calls;
            bounded_budget(timeout);

            return shot->capture(epoch, display, timeout);
        };
        bounded_transport.android = bounded_guest;
        api::Api bounded(root, "unused-cli", bounded_transport);
        auto short_capture = request("session.screenshot");
        short_capture["screen"] = "primary";
        short_capture["timeout_ms"] = 4000;
        short_capture["artifact_dir"] = (root / ".local/build/tmp/bounded-control-budget").string();
        const auto response = bounded.call(short_capture);
        assert(response.at("ok") == true && response.at("result").at("accepted") == true &&
               control_calls == 2 && guest_calls == 3);
    }
#endif
    {
        api::Transports roi_transport = transport;
        unsigned reads = 0;
        const std::string boot = "aaaaaaaa-1111-2222-3333-444444444444";
        write_private_json(
                directory / "ui-observer.json",
                {{"schema", 1},
                 {"session_id", id},
                 {"android_pid", 123},
                 {"connection", {{"boot_id", boot}, {"observer_pid", 777}, {"api_level", 30}}}});
        auto roi_transport_guest = std::make_shared<api::ScriptedGuestAndroid>();
        roi_transport.android = roi_transport_guest;
        roi_transport_guest->identity = shot->identity;
        roi_transport_guest->capture = shot->capture;
        roi_transport_guest->exchange = [&](const Json &, const Json &operation, int) {
            if (operation.at("op") == "hello") {
                return Json{{"ok", true},
                            {"boot_id", boot},
                            {"observer_pid", 777},
                            {"ui_scopes", Json::array({"all_attached", "interactive"})}};
            }

            ++reads;
            assert(operation.at("scope").at("windows") == "interactive");

            return Json{{"schema", 1}, {"ok", false}, {"reason", "deadline"}};
        };
        api::Api roi_api(root, "unused-cli", roi_transport);
        auto roi = request("session.screenshot");
        roi["screen"] = "secondary";
        roi["scope"] = "interactive";
        roi["selector"] = {{"resource_id", "example:id/switch"}};
        roi["artifact_dir"] = (root / ".local/build/tmp/roi-forwarding").string();
        assert(roi_api.call(roi).at("error").at("code") == "TREE_TIMEOUT_UNKNOWN" && reads == 1);
        std::filesystem::remove(directory / "ui-observer.json");
    }
    const auto output = root / ".local/build/tmp/bundle";
    auto capture = request("session.screenshot");
    capture["artifact_dir"] = output.string();
    capture["max_skew_ms"] = 10000;
    capture["after_sequence"] = 0;
    auto pair = shots.call(capture);
    assert(pair.at("ok") == true && pair.at("result").at("accepted") == true &&
           pair.at("result").at("atomic") == false);
    assert(pair.at("result").at("frames").size() == 2 &&
           !std::filesystem::exists(output / "cluster.png") &&
           pair.at("result").at("qnx_frame_verified") == false);

    for (const auto &frame : pair.at("result").at("frames")) {
        assert(frame.at("width") == 1920 && frame.at("height") == 720 &&
               frame.at("sequence") == "0" && !frame.contains("surface"));
        assert(sha256_file(output / frame.at("file").get<std::string>()) ==
               frame.at("sha256").get<std::string>());
        assert(frame.at("host_interval").at("start_ns").is_string() &&
               frame.at("source_timestamp_ns").is_string() &&
               frame.at("freshness").at("newer_guest_frame_proven") == false);
    }

    assert(pair.at("result").at("frames")[0].at("sha256") !=
           pair.at("result").at("frames")[1].at("sha256"));
    {
        auto selected = request("session.screenshot");
        selected["screen"] = "primary";
        selected["rect"] = {{"x", 10}, {"y", 20}, {"width", 30}, {"height", 40}};
        selected["artifact_dir"] = (root / ".local/build/tmp/roi").string();
        const auto before_captures = captures.load();
        auto cropped = shots.call(selected);
        assert(cropped.at("ok") == true && captures == before_captures + 1);
        const auto &frame = cropped.at("result").at("frames")[0];
        assert(cropped.at("result").at("frames").size() == 1 && frame.at("screen") == "primary" &&
               frame.at("rect") == selected.at("rect") && frame.at("derived") == true);
        image_dimensions(root / ".local/build/tmp/roi/primary.png", 30, 40);
        assert(sha256_file(root / ".local/build/tmp/roi/primary.png") ==
               frame.at("sha256").get<std::string>());
        assert(!std::filesystem::exists(root / ".local/build/tmp/roi/secondary.png") &&
               !std::filesystem::exists(root / ".local/build/tmp/roi/cluster.png"));

        for (const auto &invalid :
             {Json{{"surface", "media_primary"}}, Json{{"screens", {"media_primary"}}},
              Json{{"screens", {"primary", "primary"}}},
              Json{{"screen", "primary"}, {"screens", {"primary"}}},
              Json{{"screen", "primary"},
                   {"rect", {{"x", 1910}, {"y", 0}, {"width", 20}, {"height", 1}}}},
              Json{{"screen", "primary"},
                   {"rect", {{"x", 0}, {"y", 0}, {"width", 0}, {"height", 1}}}},
              Json{{"screen", "primary"},
                   {"rect", {{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}, {"extra", 0}}}},
              Json{{"rect", {{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}}}) {
            auto malformed = request("session.screenshot");
            malformed.update(invalid);
            malformed["artifact_dir"] = (root / ".local/build/tmp/invalid-roi").string();
            const auto old_calls = calls, old_captures = captures.load();
            auto rejected = shots.call(malformed);
            assert(rejected.at("ok") == false &&
                   rejected.at("error").at("code") == "TEST_ARGUMENT" && calls == old_calls &&
                   captures == old_captures &&
                   !std::filesystem::exists(root / ".local/build/tmp/invalid-roi"));
        }

        auto unavailable = request("session.screenshot");
        unavailable["screen"] = "cluster";
        unavailable["artifact_dir"] = (root / ".local/build/tmp/cluster-absent").string();
        const auto old_captures = captures.load();
        assert(shots.call(unavailable).at("error").at("code") == "TEST_UNSUPPORTED" &&
               captures == old_captures &&
               !std::filesystem::exists(root / ".local/build/tmp/cluster-absent"));
    }
    const auto repeated_capture_calls = calls, repeated_captures = captures.load();
    assert(shots.call(capture) == pair && calls == repeated_capture_calls &&
           captures == repeated_captures);
    capture["request_id"] = random_token();
    assert(shots.call(capture).at("error").at("code") == "TEST_ARTIFACT_EXISTS");
    capture["request_id"] = random_token();
    capture["artifact_dir"] = (root / ".local/build/tmp/rejected-pair").string();
    capture["max_skew_ms"] = 1;
    slow = true;
    auto skewed = shots.call(capture);
    assert(skewed.at("ok") == true && skewed.at("result").at("accepted") == false &&
           api::result_exit_code(skewed) == 4);
    assert(skewed.at("result").at("validation_error").at("code") == "TEST_FRAME_SKEW");
    slow = false;
    capture["request_id"] = random_token();
    capture["artifact_dir"] = (root / ".local/build/tmp/changed-pair").string();
    capture["max_skew_ms"] = 10000;
    change_epoch = true;
    auto epoch = shots.call(capture);
    assert(epoch.at("result").at("accepted") == false &&
           epoch.at("result").at("validation_error").at("code") == "TEST_FRAME_EPOCH");
    changed = false;
    change_epoch = false;
    capture["request_id"] = random_token();
    capture["artifact_dir"] = (root / ".local/build/tmp/rebooted-pair").string();
    change_boot = true;
    auto rebooted = shots.call(capture);
    assert(rebooted.at("result").at("accepted") == false &&
           rebooted.at("result").at("validation_error").at("code") == "TEST_FRAME_EPOCH");
    change_boot = false;
    boot_changed = false;

    {
        frames::Writer writer(directory / "frame.bgra");
        std::vector<std::byte> pixels(frames::pixel_bytes);
        writer.publish(pixels, 7, 1000);
        std::mutex barrier_mutex;
        std::condition_variable barrier;
        unsigned entered = 0;
        api::Transports concurrent = transport;
        concurrent.control = [&](const auto &endpoint, const Json &op, int timeout) {
            auto response = transport.control(endpoint, op, timeout);

            if (op.at("op") == "status") {
                response["qnx_pid"] = 456;
            }

            return response;
        };
        auto overlapping = std::make_shared<api::ScriptedGuestAndroid>();
        overlapping->identity = shot->identity;
        concurrent.android = overlapping;
        overlapping->capture = [&](const std::string &epoch, unsigned display, int) {
            assert(epoch == id);
            {
                std::unique_lock lock(barrier_mutex);
                ++entered;
                barrier.notify_all();

                if (!barrier.wait_for(lock, std::chrono::milliseconds(500), [&] {
                        return entered == 2;
                    })) {
                    throw std::runtime_error(
                            "TEST_FRAME_TIMEOUT: Android captures did not overlap");
                }
            }
            media::Image image;
            image.display = display;
            image.width = 1920;
            image.height = 720;
            image.sequence = 0;
            image.timestamp_us = 100 + display;
            image.storage.resize(image.pixels().size());

            for (std::size_t i = 0; i < image.storage.size(); i += 4) {
                image.storage[i] = display ? 50 : 100;
                image.storage[i + 3] = 255;
            }

            return image;
        };
        api::Api simultaneous(root, "unused-cli", concurrent);
        auto all = request("session.screenshot");
        all["artifact_dir"] = (root / ".local/build/tmp/three-screens").string();
        all["max_skew_ms"] = 10000;
        all["screens"] = {"primary", "secondary", "cluster"};
        auto result = simultaneous.call(all);
        assert(result.at("ok") == true && result.at("result").at("accepted") == true);
        const auto &bundle = result.at("result");
        assert(bundle.at("frames").size() == 3 && bundle.at("capture_mode") == "parallel_fenced" &&
               bundle.at("atomic") == false && bundle.at("frames_consistent") == true &&
               entered == 2);
        std::set<std::string> surfaces_seen;

        for (const auto &frame : bundle.at("frames")) {
            surfaces_seen.insert(frame.at("screen"));
            assert(frame.at("width") == 1920 && frame.at("height") == 720);
            assert(sha256_file(std::filesystem::path(bundle.at("artifact_dir").get<std::string>()) /
                               frame.at("file").get<std::string>()) ==
                   frame.at("sha256").get<std::string>());
        }

        assert(surfaces_seen == std::set<std::string>({"cluster", "primary", "secondary"}));
    }
    {
        // The canonical machine named like the profile is one identity whether the request names
        // it and whether the supervisor reports it unnamed or by name; another name differs.
        const auto canonical = root / "machines" / name / "runtime.json";
        write_private_json(canonical,
                           {{"schema", 1}, {"kind", "portable-guest-runtime"}, {"profile", name}});

        for (const std::string reported : {std::string(), name, std::string("other-machine")}) {
            api::Transports spelled = transport;
            spelled.create = [&](const Json &value) {
                assert(value.at("machine") == name);

                return Json{{"ok", true},
                            {"session_id", id},
                            {"machine", reported},
                            {"machine_seed", canonical.parent_path().string()},
                            {"machine_manifest_sha256", sha256_file(canonical)}};
            };
            api::Api spelled_api(root, "unused-cli", spelled);
            auto start = request("session.start");
            start.erase("session");
            start["machine"] = name;
            const auto started = spelled_api.call(start);

            if (reported == "other-machine") {
                assert(started.at("ok") == false &&
                       started.at("error").at("code") == "MACHINE_IDENTITY_UNKNOWN");
            } else {
                assert(started.at("ok") == true && started.at("result").at("machine") == reported &&
                       started.at("result").at("machine_seed") == canonical.parent_path().string());
            }
        }

        std::filesystem::remove(canonical);
    }
    {
        const std::string machine = name + "-copy";
        const auto selected = root / "machines" / machine / "runtime.json";
        write_private_json(selected,
                           {{"schema", 1}, {"kind", "portable-guest-runtime"}, {"profile", name}});
        unsigned selected_creates = 0;
        api::Transports selected_transport = transport;
        selected_transport.create = [&](const Json &value) {
            ++selected_creates;
            assert(value.at("machine") == machine && value.at("profile") == name);

            return Json{{"ok", true},
                        {"session_id", id},
                        {"machine", machine},
                        {"machine_seed", selected.parent_path().string()},
                        {"machine_manifest_sha256", sha256_file(selected)}};
        };
        api::Api selected_api(root, "unused-cli", selected_transport);
        auto start = request("session.start");
        start.erase("session");
        start["machine"] = machine;
        auto started = selected_api.call(start);
        assert(started.at("ok") == true && started.at("result").at("machine") == machine &&
               started.at("result").at("machine_manifest_sha256") == sha256_file(selected) &&
               selected_creates == 1);
        api::Transports stale_transport = selected_transport;
        stale_transport.create = [&](const Json &value) {
            auto result = selected_transport.create(value);

            result["machine_manifest_sha256"] = std::string(64, "0"[0]);

            return result;
        };
        api::Api stale_api(root, "unused-cli", stale_transport);
        auto stale_start = start;
        stale_start["request_id"] = random_token();
        const auto stale_result = stale_api.call(stale_start);
        assert(stale_result.at("ok") == false &&
               stale_result.at("error").at("code") == "MACHINE_IDENTITY_UNKNOWN" &&
               stale_result.at("error").at("outcome") == "unknown" &&
               stale_result.at("error").at("retryable") == false && selected_creates == 2);
        const auto &reconcile = stale_result.at("error").at("details").at("session");
        assert(reconcile.at("id") == id && reconcile.at("ownership_id").is_string());
        const auto stale_receipt =
                read_private_json(root / ".local/build/automation/operations" /
                                  (stale_result.at("operation_id").get<std::string>() + ".json"));
        assert(stale_receipt.at("ownership_id") == reconcile.at("ownership_id") &&
               stale_receipt.at("outcome") == "unknown");
        assert(stale_api.call(stale_start) == stale_result && selected_creates == 2);
        api::Api subprocess_api(root, std::filesystem::canonical(argv[0]));
        auto subprocess_start = start;
        subprocess_start["request_id"] = random_token();
        const auto subprocess_result = subprocess_api.call(subprocess_start);
        assert(subprocess_result.at("ok") == true);
        const auto subprocess_arguments =
                read_private_json(root / ".local/build/tmp/api-private-start-arguments.json");
        assert(subprocess_arguments.at(0) == "_start");
        assert(std::find(subprocess_arguments.begin(), subprocess_arguments.end(),
                         Json("--exclusive")) != subprocess_arguments.end());
        assert(std::find(subprocess_arguments.begin(), subprocess_arguments.end(),
                         Json("--ownership-id")) != subprocess_arguments.end());
        assert(std::find(subprocess_arguments.begin(), subprocess_arguments.end(), Json(machine)) !=
               subprocess_arguments.end());

        for (const auto &invalid :
             std::vector<std::string>{"", "../outside", "missing-machine", "/absolute"}) {
            start["request_id"] = random_token();
            start["machine"] = invalid;
            assert(selected_api.call(start).at("ok") == false && selected_creates == 2);
        }

        start["machine"] = machine;
        start["request_id"] = random_token();
        write_private_json(
                selected,
                {{"schema", 1}, {"kind", "portable-guest-runtime"}, {"profile", "another-car"}});
        assert(selected_api.call(start).at("ok") == false && selected_creates == 2);
        std::filesystem::remove_all(selected.parent_path());
    }
    {
        // A selected seed is lexically normal, with backslashes on Windows, so the expected paths
        // are normalized too.
        const auto external = (root / ".local/build/tmp/external-machine").lexically_normal();
        write_private_json(external / "runtime.json",
                           {{"schema", 1}, {"kind", "portable-guest-runtime"}, {"profile", name}});
        api::Api owner_api(root, std::filesystem::canonical(argv[0]));
        auto start = request("session.start");
        start.erase("session");

        std::vector<std::string> paths{external.string()};
        const auto relative = external.lexically_relative(std::filesystem::current_path());

        // A Windows scratch root on another drive than the working directory has no relative form.
        if (!relative.empty()) {
            paths.push_back(relative.string());
        }

        for (const auto &path : paths) {
            start["request_id"] = random_token();
            start["machine_path"] = path;
            const auto result = owner_api.call(start);
            assert(result.at("ok") == true);
            assert(result.at("result").at("machine_seed") == external.string() &&
                   result.at("result").at("machine") == machine_path_name(root, {external, name}) &&
                   result.at("result").at("machine_manifest_sha256") ==
                           sha256_file(external / "runtime.json"));
            const auto arguments =
                    read_private_json(root / ".local/build/tmp/api-private-start-arguments.json");
            assert(arguments.at(0) == "_start" &&
                   std::find(arguments.begin(), arguments.end(), Json(external.string())) !=
                           arguments.end());
            assert(std::find(arguments.begin(), arguments.end(), Json("--machine")) ==
                           arguments.end() &&
                   std::find(arguments.begin(), arguments.end(), Json("--exclusive")) !=
                           arguments.end() &&
                   std::find(arguments.begin(), arguments.end(), Json("--ownership-id")) !=
                           arguments.end());
        }

        start["request_id"] = random_token();
        start["machine"] = name;
        assert(owner_api.call(start).at("error").at("code") == "MACHINE_ARGUMENT");
        start.erase("machine");
        start["request_id"] = random_token();
        start["machine_path"] = (external / "missing").string();
        assert(owner_api.call(start).at("ok") == false);
        start["machine_path"] = external.string();
        start["request_id"] = random_token();
        write_private_json(
                external / "runtime.json",
                {{"schema", 1}, {"kind", "portable-guest-runtime"}, {"profile", "another-car"}});
        assert(owner_api.call(start).at("error").at("code") == "MACHINE_SOURCE");
        write_private_json(external / "runtime.json",
                           {{"schema", 1}, {"kind", "portable-guest-runtime"}, {"profile", name}});
        api::Transports mismatched = transport;
        mismatched.create = [&](const Json &value) {
            assert(value.at("machine_path") == external.string() && !value.contains("machine"));

            return Json{{"session_id", id},
                        {"machine", machine_path_name(root, {external, name})},
                        {"machine_seed", external.string()},
                        {"machine_manifest_sha256", std::string(64, '0')}};
        };
        api::Api mismatched_api(root, "unused-cli", mismatched);
        start["request_id"] = random_token();
        const auto mismatch = mismatched_api.call(start);
        assert(mismatch.at("ok") == false &&
               mismatch.at("error").at("code") == "MACHINE_IDENTITY_UNKNOWN" &&
               mismatch.at("error").at("outcome") == "unknown" &&
               mismatch.at("error").at("details").at("session").at("id") == id);
        const auto canonical = (root / "machines" / name).lexically_normal();
        assert(!std::filesystem::exists(canonical / "runtime.json"));
        write_private_json(canonical / "runtime.json",
                           {{"schema", 1}, {"kind", "portable-guest-runtime"}, {"profile", name}});
        start["request_id"] = random_token();
        start["machine_path"] = canonical.string();
        const auto canonical_result = owner_api.call(start);
        assert(canonical_result.at("ok") == true &&
               canonical_result.at("result").at("machine") == name &&
               canonical_result.at("result").at("machine_seed") == canonical.string());
        std::filesystem::remove(canonical / "runtime.json");
        std::filesystem::remove_all(external);
    }
    auto create = request("session.start");
    create.erase("session");
    assert(api.call(create).at("error").at("code") == "PROFILE_NOT_PREPARED" && creates == 0);
    {
        auto file = user_owned_file(directory / "supervisor.log");
        file << R"({"schemaVersion":1,"sequence":1,"level":"info","component":"fixture","message":"ready","fields":{"bearer":"must-not-leak"}})"
             << '\n'
             << "unstructured-secret-must-not-leak\n";
    }
    {
        unsigned reads = 0;
        api::Transports log_transport = transport;
        log_transport.control = [&](const auto &, const Json &op, int) {
            ++reads;
            assert(op.at("op") == "test_output_logs" && op.at("details") == true);

            return Json{{"ok", true},
                        {"session_id", id},
                        {"logs", {{"status", "ready"}, {"source", "android"}}}};
        };
        api::Api log_api(root, "unused-cli", log_transport);
        auto detailed = request("session.logs");
        detailed["source"] = "android";
        detailed["details"] = true;
        assert(log_api.call(detailed).at("ok") == true && reads == 1);
        auto mismatched_transport = transport;
        mismatched_transport.control = [&](const auto &, const Json &, int) {
            return Json{{"ok", true},
                        {"session_id", id},
                        {"logs", {{"status", "ready"}, {"source", "anr"}}}};
        };
        api::Api mismatched_api(root, "unused-cli", mismatched_transport);
        auto mismatch = request("session.logs");
        mismatch["source"] = "android";
        assert(mismatched_api.call(mismatch).at("error").at("code") == "TEST_LOGS_PROTOCOL");
        auto invalid_anr = request("session.logs");
        invalid_anr["source"] = "anr";
        invalid_anr["details"] = true;
        assert(log_api.call(invalid_anr).at("error").at("code") == "TEST_ARGUMENT" && reads == 1);
        auto invalid_native = request("session.logs");
        invalid_native["source"] = "native";
        invalid_native["details"] = true;
        assert(log_api.call(invalid_native).at("error").at("code") == "TEST_ARGUMENT" &&
               reads == 1);
        invalid_native.erase("details");

        for (const char *option : {"tags", "buffers", "command"}) {
            invalid_native[option] = "arbitrary";
            assert(log_api.call(invalid_native).at("error").at("code") == "TEST_ARGUMENT" &&
                   reads == 1);
            invalid_native.erase(option);
        }

        invalid_native["tombstone"] = "../../secret";
        assert(log_api.call(invalid_native).at("error").at("code") == "TEST_ARGUMENT" &&
               reads == 1);
        detailed["details"] = "yes";
        assert(log_api.call(detailed).at("error").at("code") == "TEST_ARGUMENT" && reads == 1);
        detailed["details"] = true;
        detailed["source"] = "supervisor";
        assert(log_api.call(detailed).at("error").at("code") == "TEST_ARGUMENT" && reads == 1);
    }
    {
        const auto descriptor = directory / "pid_123.ini";
        {
            auto file = user_owned_file(descriptor);
            file << "grpc.port=35554\nport.serial=5554\ngrpc.token=private-fixture-bearer\n";
        }
#ifndef _WIN32
        std::filesystem::permissions(descriptor, std::filesystem::perms::owner_read |
                                                         std::filesystem::perms::owner_write);
#endif
        const auto connection = directory / "media-connection.json",
                   launch = root / ".local/machines" / name / "android-launch.json";
        write_private_json(connection, {{"schema", 1},
                                        {"session_id", id},
                                        {"vm_pid", 123},
                                        {"serial_port", 5554},
                                        {"grpc_port", 35554},
                                        {"discovery", descriptor.string()}});
        write_private_json(launch, {{"schema", 1}, {"port", 5554}, {"adb_transport", "direct"}});
        bool reboot = false;
        unsigned snapshots = 0;
        api::Transports retained = transport;
        retained.control = [&](const auto &endpoint, const Json &op, int budget) {
            if (op.at("op") != "test_output_logs") {
                return transport.control(endpoint, op, budget);
            }

            ++snapshots;
            assert(op.at("android_pid") == 123 &&
                   op.at("boot_id") == "aaaaaaaa-1111-2222-3333-444444444444");
            assert(op.at("timeout_ms") == budget && !op.contains("command") &&
                   !op.contains("args") &&
                   op.dump().find("private-fixture-bearer") == std::string::npos);

            if (op.at("source") == "native") {
                assert(op.at("tombstone") == "tombstone_00");
            } else {
                assert(op.at("source") == "anr" && !op.contains("tombstone"));
            }

            boot_changed = reboot;

            return Json{{"ok", true},
                        {"session_id", id},
                        {"logs",
                         {{"status", "ready"},
                          {"source", op.at("source")},
                          {"session_id", id},
                          {"android_pid", 123},
                          {"boot_id", op.at("boot_id")}}}};
        };
        api::Api retained_api(root, "unused-cli", retained);
        auto anr = request("session.logs");
        anr["source"] = "anr";
        assert(retained_api.call(anr).at("ok") == true && snapshots == 1);
        auto native = request("session.logs");
        native["source"] = "native";
        native["tombstone"] = "tombstone_00";
        assert(retained_api.call(native).at("ok") == true && snapshots == 2);
        reboot = true;
        anr["request_id"] = random_token();
        assert(retained_api.call(anr).at("error").at("code") == "TEST_LOGS_STALE" &&
               snapshots == 3);
        boot_changed = false;
        std::filesystem::remove(connection);
        std::filesystem::remove(descriptor);
        std::filesystem::remove(launch);
    }
    auto logs = api.call(request("session.logs"));
    assert(logs.at("ok") == true && logs.dump().find("must-not-leak") == std::string::npos &&
           logs.at("result").at("unstructured_lines_skipped") == 1);
    auto diagnostic = api.call(request("session.status"));
    assert(diagnostic.at("ok") == true && diagnostic.at("result").at("session_id") == id &&
           diagnostic.at("result").contains("deliveries") &&
           diagnostic.dump().find("must-not-leak") == std::string::npos);
    auto cursor = request("session.logs");
    cursor["cursor"] = logs.at("result").at("cursor");
    assert(api.call(cursor).at("result").at("events").empty());
    cursor["cursor"]["file_id"] = "rotated";
    assert(api.call(cursor).at("result").at("gap") == true);
    auto run = request("test.run");
    run["artifact_dir"] = (root / ".local/build/tmp/failed-run").string();
    run["cases"] = Json::array({{{"id", "preflight-capture"}, {"op", "session.screenshot"}},
                                {{"id", "input"}, {"op", "input.key.press"}, {"key", "HOME"}}});
    auto failed = api.call(run);
    assert(failed.at("ok") == true && api::result_exit_code(failed) == 4);
    const auto report = read_private_json(failed.at("result").at("report").get<std::string>());
    assert(report.at("preflight").at("status") == "failed" && report.at("cases").size() == 2 &&
           report.at("cases")[0].at("status") == "not-run");
    assert(report.at("cleanup").at("status") == "not-requested");
    auto create_run = run;
    create_run.erase("session");
    create_run.erase("profile");
    create_run["op"] = "test.run";
    create_run["request_id"] = random_token();
    create_run["start"] = {{"profile", "not-prepared-json-fixture"}, {"with_cluster", false}};
    create_run["cleanup"] = "stop";
    create_run["artifact_dir"] = (root / ".local/build/tmp/unprepared-create-run").string();
    auto create_report = api.call(create_run);
    assert(create_report.at("result").at("cleanup_status") == "not-required" && creates == 0);
    {
        const auto portable_manifest = root / "machines" / name / "runtime.json";
        const Json portable{{"schema", 1}, {"kind", "portable-guest-runtime"}, {"profile", name}};
        unsigned portable_creates = 0;
        api::Transports portable_transport = transport;
        portable_transport.create = [&](const Json &launch) {
            ++portable_creates;
            assert(launch.at("profile") == name && launch.at("with_cluster") == false);

            return Json{{"ok", true}, {"session_id", id}};
        };
        api::Api portable_api(root, "unused-cli", portable_transport);

        {
            write_private_json(portable_manifest, portable);
            unsigned refresh_creates = 0;
            api::Transports refresh_transport = transport;
            refresh_transport.create = [&](const Json &launch) {
                ++refresh_creates;
                assert(launch.at("refresh_host") == true && launch.at("ownership_id") == owner &&
                       launch.at("session").at("id") == id);

                return Json{{"ok", true}, {"session_id", id}};
            };
            api::Api refresh_api(root, "unused-cli", refresh_transport);
            auto refresh = request("session.start");
            refresh["refresh_host"] = true;
            refresh["with_cluster"] = false;
            const auto refreshed = refresh_api.call(refresh);
            assert(refreshed.at("ok") == true &&
                   refreshed.at("result").at("session").at("ownership_id") == owner &&
                   refresh_creates == 1);
            assert(refresh_api.call(refresh) == refreshed && refresh_creates == 1);
            refresh["request_id"] = random_token();
            refresh["refresh_host"] = "yes";
            assert(refresh_api.call(refresh).at("ok") == false && refresh_creates == 1);
            refresh["request_id"] = random_token();
            refresh["refresh_host"] = true;
            refresh["session"].erase("ownership_id");
            assert(refresh_api.call(refresh).at("ok") == false && refresh_creates == 1);
        }
        auto portable_start = create;
        portable_start["with_cluster"] = false;
        write_private_json(portable_manifest, portable);
        portable_start["request_id"] = random_token();
        assert(portable_api.call(portable_start).at("ok") == true && portable_creates == 1 &&
               creates == 0);

        for (const auto &field : std::vector<std::string>{"profile", "schema", "kind"}) {
            auto invalid_manifest = portable;

            if (field == "schema") {
                invalid_manifest[field] = 2;
            } else {
                invalid_manifest[field] = "different";
            }

            write_private_json(portable_manifest, invalid_manifest);
            portable_start["request_id"] = random_token();
            assert(portable_api.call(portable_start).at("error").at("code") ==
                           "PROFILE_NOT_PREPARED" &&
                   portable_creates == 1 && creates == 0);
        }

        write_private_json(portable_manifest, portable);
        portable_start["request_id"] = random_token();
        portable_start["with_cluster"] = true;
        portable_start["seconds"] = 1;
        assert(portable_api.call(portable_start).at("error").at("code") == "PROFILE_NOT_PREPARED" &&
               portable_creates == 1 && creates == 0);
        std::filesystem::remove(portable_manifest);
    }
    // Prepared launches belong to this machine, independently of automation records.
    write_private_json(root / ".local/machines" / name / "android-launch.json", {{"schema", 1}});
    {
        const auto prepared = root / ".local/machines" / name / "cluster-launch.json";
        write_private_json(prepared, {{"schema", 1}});
        api::Api launcher(root, std::filesystem::canonical(argv[0]));
        auto start = create;
        start["request_id"] = random_token();
        start["with_cluster"] = true;
        assert(launcher.call(start).at("ok") == true);
        const auto recorded_args = root / ".local/build/tmp/api-private-start-arguments.json";
        auto args = read_private_json(recorded_args);
        assert(std::find(args.begin(), args.end(), Json("--with-cluster")) != args.end());
        assert(std::find(args.begin(), args.end(), Json("--seconds")) == args.end());

        for (const auto &seconds : std::vector<Json>{0, 3601, -1, 1.5, "1", nullptr}) {
            start["request_id"] = random_token();
            start["seconds"] = seconds;
            assert(launcher.call(start).at("error").at("code") == "TEST_ARGUMENT");
            assert(read_private_json(recorded_args) == args);
        }

        start["request_id"] = random_token();
        start["seconds"] = 3600;
        assert(launcher.call(start).at("ok") == true);
        args = read_private_json(recorded_args);
        const auto seconds = std::find(args.begin(), args.end(), Json("--seconds"));
        assert(seconds != args.end() && std::next(seconds) != args.end() &&
               *std::next(seconds) == "3600");

        bool configured = true, bounded = false;
        unsigned mutations = 0;
        std::string active_owner = owner;
        api::Transports lifecycle_transport = transport;
        lifecycle_transport.create = [&](const Json &launch) {
            assert(launch.at("with_cluster") == true && !launch.contains("seconds"));
            active_owner = launch.at("ownership_id").get<std::string>();
            write_private_json(directory / "automation-owner.json",
                               {{"schema", 1}, {"session_id", id}, {"ownership_id", active_owner}});

            return Json{{"ok", true}, {"session_id", id}};
        };
        lifecycle_transport.control = [&](const auto &, const Json &op, int timeout) {
            if (op.at("op") != "status") {
                assert(op.at("op") == "cluster_power" || op.at("op") == "cluster_restart");
                // The bound includes the guest sync of the host QNX engine, longer under TCG.
                assert(op.at("ownership_id") == active_owner && op.at("expected_generation") == 1 &&
                       timeout == request_timeout_limit_ms(op.at("op").get<std::string>()));
                ++mutations;
            }

            return Json{{"ok", true},
                        {"session_id", id},
                        {"deliveries",
                         {{"qnx", {{"lifecycle_supported", configured}, {"bounded", bounded}}}}}};
        };
        api::Api lifecycle(root, "unused-cli", lifecycle_transport);

        for (bool cap : {false, true}) {
            bounded = cap;
            auto power = request("cluster.power");
            power["expected_generation"] = 1;
            power["enabled"] = true;
            assert(lifecycle.call(power).at("ok") == true);
            auto restart = request("cluster.reboot");
            restart["expected_generation"] = 1;
            assert(lifecycle.call(restart).at("ok") == true);
        }

        assert(mutations == 4);
        auto restart = request("cluster.reboot");
        assert(lifecycle.call(restart).at("error").at("code") == "TEST_ARGUMENT" && mutations == 4);
        restart["request_id"] = random_token();
        restart["expected_generation"] = 1;
        restart["session"]["ownership_id"] = random_token();
        assert(lifecycle.call(restart).at("error").at("code") == "TEST_OWNERSHIP" &&
               mutations == 4);
        configured = false;
        restart = request("cluster.reboot");
        restart["expected_generation"] = 1;
        assert(lifecycle.call(restart).at("error").at("code") == "TEST_ARGUMENT" && mutations == 4);

        configured = true;
        bounded = false;
        auto scenario = create_run;
        scenario["request_id"] = random_token();
        scenario["start"] = {{"profile", name}, {"with_cluster", true}};
        scenario["cleanup"] = "leave_running";
        scenario["artifact_dir"] = (root / ".local/build/tmp/unbounded-cluster-run").string();
        scenario["cases"] = Json::array({{{"id", "restart-cluster"},
                                          {"op", "cluster.reboot"},
                                          {"expected_generation", 1}}});
        const auto result = lifecycle.call(scenario);
        assert(result.at("ok") == true && api::result_exit_code(result) == 0 && mutations == 5);
        assert(result.at("result").at("preflight_status") == "passed");
        write_private_json(directory / "automation-owner.json",
                           {{"schema", 1}, {"session_id", id}, {"ownership_id", owner}});
        std::filesystem::remove(prepared);
    }
    {
        const auto manifest = (root / "provider.json").string();
        const auto before_providers = creates;
        const std::vector<Json> invalid_providers = {
                nullptr,
                "provider.json",
                Json::object(),
                Json::array({1}),
                Json::array({"provider.json"}),
                Json::array({manifest, manifest}),
                Json::array({(root / "nested/../provider.json").string()}),
                Json::array({manifest, manifest + "1", manifest + "2", manifest + "3",
                             manifest + "4"})};

        for (const auto &providers : invalid_providers) {
            auto invalid_start = create;
            invalid_start["request_id"] = random_token();
            invalid_start["providers"] = providers;
            assert(api.call(invalid_start).at("error").at("code") == "TEST_ARGUMENT" &&
                   creates == before_providers);
        }

        bool forwarded = false;
        api::Transports manifest_transport = transport;
        manifest_transport.create = [&](const Json &start) {
            forwarded = start.at("providers") == Json::array({manifest});

            return Json{{"ok", true}, {"session_id", id}};
        };
        api::Api manifest_api(root, "unused-cli", manifest_transport);
        auto valid_start = create;
        valid_start["request_id"] = random_token();
        valid_start["providers"] = Json::array({manifest});
        assert(manifest_api.call(valid_start).at("ok") == true && forwarded &&
               creates == before_providers);
    }
    create["request_id"] = random_token();
    auto background_start = api.call(create);
    assert(background_start.at("ok") == true && created_background &&
           background_start.at("result").at("background") == true);
    create["request_id"] = random_token();
    create["background"] = false;
    auto foreground_start = api.call(create);
    assert(foreground_start.at("ok") == true && !created_background &&
           foreground_start.at("result").at("background") == false);
    const auto before_invalid_background = creates;
    create["request_id"] = random_token();
    create["background"] = "false";
    assert(api.call(create).at("error").at("code") == "TEST_ARGUMENT" &&
           creates == before_invalid_background);
    {
        std::mutex pending_mutex;
        std::condition_variable pending_changed;
        bool create_entered = false, release_create = false;
        std::atomic<unsigned> pending_creates{0};
        api::Transports pending_transport = transport;
        pending_transport.create = [&](const Json &) {
            ++pending_creates;
            std::unique_lock pending_lock(pending_mutex);
            create_entered = true;
            pending_changed.notify_all();

            if (!pending_changed.wait_for(pending_lock, std::chrono::seconds(10), [&] {
                    return release_create;
                })) {
                throw std::runtime_error("TEST_TIMEOUT: pending create was not released");
            }

            return Json{{"ok", true}, {"session_id", id}};
        };
        api::Api pending_api(root, "unused-cli", pending_transport);
        auto pending_start = request("session.start");
        pending_start.erase("session");
        Json completed_start;
        std::thread creator([&] {
            completed_start = pending_api.call(pending_start);
        });
        {
            std::unique_lock pending_lock(pending_mutex);
            assert(pending_changed.wait_for(pending_lock, std::chrono::seconds(5), [&] {
                return create_entered;
            }));
        }
        auto pending_duplicate = pending_api.call(pending_start);
        assert(pending_duplicate.at("ok") == false &&
               pending_duplicate.at("error").at("code") == "OPERATION_PENDING_UNKNOWN" &&
               pending_duplicate.at("error").at("outcome") == "unknown" && pending_creates == 1);
        auto pending_lookup = request("operation.get");
        pending_lookup.erase("session");
        pending_lookup["target"] = {{"request_id", pending_start.at("request_id")}};
        auto pending_receipt = pending_api.call(pending_lookup);
        assert(pending_receipt.at("ok") == true &&
               pending_receipt.at("result").at("status") == "pending" &&
               pending_receipt.at("result").at("operation_id") ==
                       pending_duplicate.at("operation_id"));
        {
            std::lock_guard pending_lock(pending_mutex);
            release_create = true;
        }
        pending_changed.notify_all();
        creator.join();
        assert(completed_start.at("ok") == true &&
               completed_start.at("operation_id") == pending_duplicate.at("operation_id") &&
               pending_api.call(pending_start) == completed_start && pending_creates == 1);
        auto terminal_receipt = pending_api.call(pending_lookup);
        assert(terminal_receipt.at("result").at("status") == "terminal" &&
               terminal_receipt.at("result").at("response") == completed_start);
    }
    {
        const std::string large_report(256 * 1024, 'r');
        api::Transports large_transport = transport;
        large_transport.control = [&](const auto &endpoint, const Json &op, int timeout) {
            auto response = transport.control(endpoint, op, timeout);

            if (op.at("op") == "patch") {
                response["report"] = large_report;
            }

            return response;
        };
        api::Api large_api(root, "unused-cli", large_transport);
        auto large_patch = request("vehicle.set");
        large_patch["expected_revision"] = session.handle({{"op", "status"}}).at("revision");
        large_patch["values"] = {{"speed", 87}};
        auto large_response = large_api.call(large_patch);
        assert(large_response.at("ok") == true &&
               large_response.at("result").at("report") == large_report);
        auto large_lookup = request("operation.get");
        large_lookup["target"] = {{"operation_id", large_response.at("operation_id")}};
        auto by_operation = large_api.call(large_lookup);
        assert(by_operation.at("ok") == true &&
               by_operation.at("result").at("status") == "terminal" &&
               by_operation.at("result").at("response") == large_response);
        large_lookup["target"] = {{"request_id", large_patch.at("request_id")}};
        auto by_request = large_api.call(large_lookup);
        assert(by_request.at("ok") == true && by_request.at("result").at("status") == "terminal" &&
               by_request.at("result").at("response") == large_response);
        const auto large_calls = calls;
        assert(large_api.call(large_patch) == large_response && calls == large_calls);
    }
    {
        // A failure without its own CODE after the action was dispatched has an unknown outcome,
        // and a malformed supervisor response is a protocol failure, not a client argument.
        for (const std::string failure : {"filesystem", "malformed", "malformed-read"}) {
            api::Transports failing = transport;
            failing.control = [&](const auto &endpoint, const Json &op, int timeout) -> Json {
                if (op.at("op") != (failure == "malformed-read" ? "status" : "patch")) {
                    return transport.control(endpoint, op, timeout);
                }

                if (failure == "filesystem") {
                    throw std::filesystem::filesystem_error(
                            "resize_file", std::filesystem::path("vehicle-state.json"),
                            std::make_error_code(std::errc::no_space_on_device));
                }

                return Json{{"ok", true}, {"session_id", 7}};
            };
            api::Api failing_api(root, "unused-cli", failing);
            auto failing_request = request(failure == "malformed-read" ? "vehicle.get"
                                                                       : "vehicle.set");

            if (failure != "malformed-read") {
                failing_request["expected_revision"] =
                        session.handle({{"op", "status"}}).at("revision");
                failing_request["values"] = {{"speed", 87}};
            }

            const auto failed = failing_api.call(failing_request).at("error");
            const auto expected = failure == "filesystem"  ? "FILESYSTEM_UNKNOWN"
                                  : failure == "malformed" ? "IPC_PROTOCOL_UNKNOWN"
                                                           : "IPC_PROTOCOL";
            assert(failed.at("code") == expected && failed.at("retryable") == false &&
                   failed.at("outcome") ==
                           (failure == "malformed-read" ? "not_started" : "unknown"));
        }
    }
    run["request_id"] = random_token();
    run["artifact_dir"] = (root / ".local/build/tmp/pass-run").string();
    run["cases"] =
            Json::array({{{"id", "ready"},
                          {"op", "session.wait"},
                          {"predicate", "android.ready"},
                          {"timeout_ms", 1000}},
                         {{"id", "pair"}, {"op", "session.screenshot"}, {"max_skew_ms", 10000}}});
    auto passed = shots.call(run);
    assert(passed.at("ok") == true && passed.at("result").at("status") == "passed");
    auto operation = request("operation.get");
    operation["target"] = {{"operation_id", passed.at("operation_id")}};
    assert(api.call(operation).at("result").at("status") == "terminal");
    operation["target"] = {{"request_id", run.at("request_id")}};
    assert(api.call(operation).at("result").at("status") == "terminal");
    run["request_id"] = random_token();
    run["artifact_dir"] = (root / ".local/build/tmp/quota-run").string();
    run["artifact_budget_bytes"] = 16 * 1024 * 1024;
    run["cases"] = Json::array(
            {{{"id", "pair-one"}, {"op", "session.screenshot"}, {"max_skew_ms", 10000}},
             {{"id", "pair-two"}, {"op", "session.screenshot"}, {"max_skew_ms", 10000}}});
    auto quota = shots.call(run);
    assert(quota.at("result").at("status") == "failed");
    const auto quota_report = read_private_json(quota.at("result").at("report").get<std::string>());
    assert(quota_report.at("cases")[0].at("status") == "passed" &&
           quota_report.at("cases")[1].at("error").at("code") == "TEST_ARTIFACT_BUDGET");
    auto active = read_private_json(root / ".local/machines" / name / "active" / (name + ".json"));
    active["endpoint"] = session.endpoint().string();
    write_private_json(root / ".local/machines" / name / "active" / (name + ".json"), active);
    std::atomic<bool> server_stop = false;
    {
        ControlServer server(session.endpoint(), [&](const Json &op) {
            return session.handle(op);
        });
        std::thread thread([&] {
            server.serve(server_stop);
        });
        ProcessSpec exclusive;
        exclusive.executable = argv[1];
        exclusive.arguments = {"_start", "--root",      root.string(), "--profile",
                               name,     "--exclusive", "--no-cluster"};
        auto refused = execute(exclusive);
        assert(refused.exit_code == 4 &&
               refused.output.find("TEST_SESSION_ACTIVE:") != std::string::npos);
        assert(session.handle({{"op", "status"}}).at("stopped") == false);
        server_stop = true;
        thread.join();
    }
    {
        auto file = user_owned_file(root / ".local/machines" / name / "active" /
                                    (name + ".supervisor.lock"));
        file << "owned-lock-fixture";
    }
    Json stopped;
#ifdef _WIN32
    {
        // A terminal runtime record cannot authorize cleanup while the exact
        // named pipe still has a connected client. Wait for its actual closure.
        const auto endpoint = session.endpoint();
        HANDLE server = CreateNamedPipeW(endpoint.c_str(), PIPE_ACCESS_DUPLEX,
                                         PIPE_TYPE_BYTE | PIPE_NOWAIT, 1, 4096, 4096, 0, nullptr);
        assert(server != INVALID_HANDLE_VALUE);
        HANDLE client = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        assert(client != INVALID_HANDLE_VALUE &&
               !api::StoppedAndroidLease::endpoint_absent(endpoint));
        std::atomic<bool> stop_dispatched = false;
        api::Transports stopping = transport;
        stopping.control = [&](const auto &address, const Json &operation, int timeout) {
            auto response = transport.control(address, operation, timeout);

            if (operation.at("op") == "stop") {
                stop_dispatched = true;
            }

            return response;
        };
        std::thread close_pipe([&] {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);

            while (!stop_dispatched && std::chrono::steady_clock::now() < end) {
                Sleep(1);
            }

            Sleep(100);
            CloseHandle(client);
            CloseHandle(server);
        });
        api::Api stopping_api(root, "unused-cli", stopping);
        stopped = stopping_api.call(request("session.stop"));
        close_pipe.join();
        assert(stop_dispatched);
    }
#else
    stopped = api.call(request("session.stop"));
#endif
    assert(stopped.at("ok") == true &&
           stopped.at("result").at("cleanup").at("supervisor_lock_released") == true);
    auto terminal = api.call(request("session.status"));
    assert(terminal.at("ok") == true && terminal.at("result").at("stopped") == true &&
           terminal.at("result").at("observation_source") == "owned terminal runtime record");
    session.stop();
    std::filesystem::remove_all(root);
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
