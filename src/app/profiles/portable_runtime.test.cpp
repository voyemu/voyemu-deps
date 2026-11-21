#include "portable_runtime.hpp"
#include "android_engine.hpp"
#include "android_port_lease.hpp"
#include "api30_framework_boot.hpp"
#include "host_refresh.hpp"
#include "image_clone.hpp"
#include "machine_host_ports.hpp"
#include "machine_preparation.hpp"
#include "nxp_presentation.hpp"
#include "os/platform.hpp"
#include "passion_mcu_prepared.hpp"
#include "passion_power_prepared.hpp"
#include "passion_presentation.hpp"
#include "passion_wheel.hpp"
#include "phone_control.hpp"
#include "process.hpp"
#include "profile_catalog.hpp"
#include "profile_runtime.hpp"
#include "qinggan09_owned_packages.hpp"
#include "qinggan09_phone_package.hpp"
#include "qinggan11_startup.hpp"
#include "qnx_host.hpp"
#include "qnx_process_ram.hpp"
#include "qnx_runtime_layout.hpp"
#include "quickboot.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#ifdef _WIN32
#include "wine_native_backend_windows.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
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

namespace {
void check(bool value, const char *text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

// The android object of a registered profile with the clean base pins of its API28 or API30 image.
Json android_base(unsigned api) {
    return embedded_profile(api == 30 ? "free-11-chinese-20260731-7.3.1" : qinggan09_profile())
            .at("definition")
            .at("android");
}

void sparse_copy_checks(const std::filesystem::path &root) {
    const auto directory = root / "sparse-copy";

    ensure_private_directory(directory);
    const auto source = directory / "source.img", target = directory / "copy.img";
    constexpr std::uint64_t size = 64ULL << 20;
    {
        std::ofstream out(source, std::ios::binary);

        for (const auto offset :
             std::array<std::uint64_t, 3>{0, (32ULL << 20) + 17, size - (1ULL << 20) - 5}) {
            out.seekp(offset);
            out << "data";
        }

        out.seekp(size - 1);
        out.put(0);
        check(bool(out), "sparse source fixture write");
    }
    const auto original = sha256_file(source);
    copy_portable_image(source, original, target, 0);
    check(std::filesystem::file_size(target) == size && sha256_file(target) == original,
          "sparse logical extent/bytes differ");
    check(std::filesystem::hard_link_count(source) == 1 &&
                  std::filesystem::hard_link_count(target) == 1 &&
                  cache_file_identity(source) != cache_file_identity(target),
          "sparse copy must be independent");
#ifdef _WIN32
    HANDLE allocation_file = CreateFileW(target.c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
    check(allocation_file != INVALID_HANDLE_VALUE, "sparse allocation file unavailable");
    FILE_STANDARD_INFO allocation{};
    const bool observed = GetFileInformationByHandleEx(allocation_file, FileStandardInfo,
                                                       &allocation, sizeof(allocation)) != 0;
    CloseHandle(allocation_file);
    check(observed && allocation.AllocationSize.QuadPart >= 0,
          "allocated sparse extent unavailable");
    const auto allocated = std::uint64_t(allocation.AllocationSize.QuadPart);
#else
    struct stat status{};
    check(::stat(target.c_str(), &status) == 0, "allocated sparse extent unavailable");
    const auto allocated = std::uint64_t(status.st_blocks) * 512;
#endif
    check(allocated < size / 8, "zero ranges allocated as full logical copy");
    {
        std::fstream out(target, std::ios::in | std::ios::out | std::ios::binary);
        out.seekp(7);
        out.put('X');
    }
    check(sha256_file(source) == original && sha256_file(target) != original,
          "target mutation changed source");
    const auto independent = sha256_file(target);
    {
        std::fstream out(source, std::ios::in | std::ios::out | std::ios::binary);
        out.seekp(9);
        out.put('Y');
    }
    check(sha256_file(target) == independent, "source mutation changed target");
    const auto current = sha256_file(source);
    auto rejected = [&](const auto &input, const std::string &pin, const auto &output,
                        std::uint64_t reserve = 0) {
        bool denied = false;
        try {
            copy_portable_image(input, pin, output, reserve);
        } catch (...) {
            denied = true;
        }

        check(denied, "unsafe sparse copy accepted");
    };
    rejected(source, current, target);
    check(sha256_file(target) == independent, "existing target replaced");
    const auto wrong = directory / "wrong.img";
    rejected(source, std::string(64, '0'), wrong);
    check(!std::filesystem::exists(wrong), "wrong pin created target");
    const auto alias = directory / "alias.img";
    std::filesystem::create_hard_link(source, alias);
    rejected(source, current, wrong);
    std::filesystem::remove(alias);
    check(!std::filesystem::exists(wrong), "hardlinked input created target");
    const auto link = directory / "link.img";
    std::error_code error;
    std::filesystem::create_symlink(source, link, error);

    if (!error) {
        rejected(link, current, wrong);
        std::filesystem::remove(link);
        check(!std::filesystem::exists(wrong), "linked input created target");
    }

    const auto pending = directory / "pending";
    ensure_private_directory(pending);
    const auto partial = pending / "partial.img";
    rejected(source, current, partial, std::numeric_limits<std::uint64_t>::max());
    check(!std::filesystem::exists(partial) || std::filesystem::file_size(partial) == 0,
          "reserve violation wrote image data");
    std::filesystem::remove_all(pending);
    check(!std::filesystem::exists(pending), "owned partial cleanup failed");
#ifdef __APPLE__
    for (const auto error : {ENOTSUP, EXDEV}) {
        const auto fake=error==ENOTSUP?+[](const char*,const char*,uint32_t){errno=ENOTSUP;return -1;}:+[](const char*,const char*,uint32_t){errno=EXDEV;return -1;};
        check(!clone_image(source, wrong, fake),
              "unsupported clone did not select sparse fallback");
    }

    for (const auto fake : {+[](const char *, const char *, uint32_t) {
                                errno = EEXIST;

                                return -1;
                            },
                            +[](const char *, const char *, uint32_t) {
                                errno = EACCES;

                                return -1;
                            },
                            +[](const char *, const char *, uint32_t) {
                                errno = ENOSPC;

                                return -1;
                            },
                            +[](const char *, const char *, uint32_t) {
                                errno = EIO;

                                return -1;
                            },
                            +[](const char *, const char *, uint32_t) {
                                errno = EINVAL;

                                return -1;
                            }}) {
        bool denied = false;
        try {
            clone_image(source, wrong, fake);
        } catch (...) {
            denied = true;
        }

        check(denied, "clone hard error masked as sparse fallback");
    }

    const auto dense = directory / "dense.img", dense_copy = directory / "dense-copy.img";
    {
        std::ofstream out(dense, std::ios::binary);
        const std::string bytes(1 << 20, 'D');
        out << bytes;
        check(bool(out), "dense fixture write");
    }
    const auto dense_sha = sha256_file(dense);
    copy_portable_image(dense, dense_sha, dense_copy, 0);
    check(sha256_file(dense_copy) == dense_sha && std::filesystem::hard_link_count(dense_copy) == 1,
          "dense clone bytes/identity");
    {
        std::fstream out(dense_copy, std::ios::in | std::ios::out | std::ios::binary);
        out.put('X');
    }
    check(sha256_file(dense) == dense_sha, "dense target write changed source");
    struct stat cloned{};
    check(::stat(dense_copy.c_str(), &cloned) == 0 && (cloned.st_mode & 0777) == 0600,
          "clone output mode differs");
#endif
}

void cache(const std::filesystem::path &path) {
    ensure_private_directory(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.seekp(1080);
    out.put(0x53);
    out.put(static_cast<char>(0xef));
    out.seekp(69206015);
    out.put(0);
    out.close();
    check(bool(out), "cache fixture write");
}

void put(const std::filesystem::path &path, const std::string &text) {
    ensure_private_directory(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.close();
    check(bool(out), "fixture write");
}

Json resource_fixture(const std::filesystem::path &seed) {
    Json resources = Json::object();

    for (const auto *key : {"ui/splash.png", "ui/wheel-routes.json", "ui/wheel/wheel-hotspots.json",
                            "ui/wheel/wheel-left.png", "ui/wheel/wheel-right.png"}) {
        const std::string text = std::string(key) == "ui/wheel-routes.json" ? "{\"routes\":[]}"
                                 : std::string(key) == "ui/wheel/wheel-hotspots.json"
                                         ? "{}"
                                         : "synthetic owned UI bytes";
        const auto file = seed / key;
        put(file, text);
        resources[key] = {{"file", key}, {"bytes", text.size()}, {"sha256", sha256_bytes(text)}};
    }

    return resources;
}

void observer_configuration_checks(const std::filesystem::path &stage, bool a9 = false) {
    const auto root = stage / (a9 ? "observer-refresh-a9" : "observer-refresh"),
               seed = root / "machines/current";
    const std::string profile = a9 ? "free-09-chinese-20260106-6.0.3"
                                   : "passion-11-russian-20260403-2.1.4.80",
                      machine = "current";
    const auto selected_machine = machine_identity(profile, machine);

    ensure_private_directory(seed);
    const Json android30 = {
            {"package", "system-images;android-30;default;arm64-v8a"},
            {"revision", "1"},
            {"url",
             "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"},
            {"sha256", "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304"},
            {"clean_base", android_base(30).at("clean_base")}};
    Json definition = {{"id", profile},
                       {"title", "Synthetic Observer 1.0"},
                       {"platform", a9 ? "qinggan09" : "passion"},
                       {"board", "8155"},
                       {"android", android30}};

    if (a9) {
        definition.erase("id");
        definition["android"] = {
                {"package", "system-images;android-28;default;arm64-v8a"},
                {"revision", "1"},
                {"url",
                 "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip"},
                {"sha256", "86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9"},
                {"clean_base", android_base(28).at("clean_base")}};
    }

    write_private_json(seed / "profile.json", definition);
    Json manifest = {{"schema", 1},
                     {"kind", "portable-guest-runtime"},
                     {"profile", profile},
                     {"profile_sha256", sha256_file(seed / "profile.json")},
                     {"android", {{"api", a9 ? 28 : 30}}}};
    write_private_json(seed / "runtime.json", manifest);
    const auto scoped = root / ".local/machines/current";
    const Json activation = {{"schema", 1},
                             {"kind", "runtime-configuration"},
                             {"profile", profile},
                             {"machine", machine},
                             {"seed", seed.string()},
                             {"manifest_sha256", sha256_file(seed / "runtime.json")},
                             {"integrity_audited", false}};
    write_private_json(scoped / "activation.json", activation);
    const auto dex = root / ".local/toolchains/ui-observer/source/classes.dex";
    put(dex, "source DEX fixture");
    const Json descriptor = {{"dex", dex.string()}, {"sha256", sha256_file(dex)}};
    const Json launch = {
            {"api", a9 ? 28 : 30},
            {"frontend", {{"executable", "private/c774"}, {"opaque", true}}},
            {"pins", Json::array({{{"path", "private/c774"}, {"sha256", std::string(64, 'a')}}})},
            {"machine", seed.string()},
            {"args", Json::array({"-memory", "4096"})}};
    write_private_json(scoped / "android-launch.json", launch);
    const auto before = read_private_json_bytes(scoped / "android-launch.json");
    int prepares = 0;
    auto prepare = [&] {
        ++prepares;

        return descriptor;
    };
    auto denied = [&](std::string_view selected = std::string_view{}) {
        const auto old = prepares;
        const auto activation = read_private_json_bytes(scoped / "activation.json");
        bool rejected = false;
        try {
            refresh_machine_ui_observer(root, selected.empty() ? profile : selected,
                                        selected_machine, seed, prepare);
        } catch (...) {
            rejected = true;
        }

        check(rejected && prepares == old &&
                      read_private_json_bytes(scoped / "android-launch.json") == before &&
                      read_private_json_bytes(scoped / "activation.json") == activation,
              "invalid refresh compiled or changed launch");
    };
    denied(profile + "x");
    manifest["android"]["api"] = a9 ? 30 : 28;
    write_private_json(seed / "runtime.json", manifest);
    denied();
    manifest["android"]["api"] = a9 ? 28 : 30;
    write_private_json(seed / "runtime.json", manifest);
    const auto directory = native_session_directory(root, profile, selected_machine, "own");
    write_private_json(native_current_active_directory(root, profile, selected_machine) /
                               (profile + ".json"),
                       {{"session_id", "own"}, {"directory", directory.string()}});
    write_private_json(directory / "runtime.json", {{"session_id", "own"}, {"stopped", false}});
    denied();
    write_private_json(directory / "runtime.json", {{"session_id", "own"}, {"stopped", true}});

    for (const auto *suffix : {".launch.lock", ".supervisor.lock"}) {
        const auto path = native_current_active_directory(root, profile, selected_machine) /
                          (profile + suffix);
#ifdef _WIN32
        HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(lock != INVALID_HANDLE_VALUE, "fixture lock failed");
        denied();
        CloseHandle(lock);
#else
        int lock = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
        check(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0, "fixture lock failed");
        denied();
        ::close(lock);
#endif
    }

    const auto profile_before = read_private_json_bytes(seed / "profile.json");
    put(seed / "profile.json", "{}");
    denied();
    put(seed / "profile.json", profile_before);
    auto wrong_snapshot = read_private_json(seed / "profile.json");
    wrong_snapshot["android"] = {
            {"package", "system-images;android-28;default;arm64-v8a"},
            {"revision", "1"},
            {"url",
             "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip"},
            {"sha256", "86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9"},
            {"clean_base", android_base(28).at("clean_base")}};

    if (a9) {
        wrong_snapshot["android"] = android30;
    }

    write_private_json(seed / "profile.json", wrong_snapshot);
    auto mismatch_manifest = manifest;
    mismatch_manifest["profile_sha256"] = sha256_file(seed / "profile.json");
    write_private_json(seed / "runtime.json", mismatch_manifest);
    auto mismatch_activation = activation;
    mismatch_activation["manifest_sha256"] = sha256_file(seed / "runtime.json");
    write_private_json(scoped / "activation.json", mismatch_activation);
    denied();
    put(seed / "profile.json", profile_before);
    write_private_json(seed / "runtime.json", manifest);
    write_private_json(scoped / "activation.json", activation);

    for (const auto &invalid : {Json{}, Json::object()}) {
        auto bad = launch;
        bad["ui_observer"] = invalid;
        write_private_json(scoped / "android-launch.json", bad);
        const auto bad_bytes = read_private_json_bytes(scoped / "android-launch.json");
        const auto count = prepares;
        bool rejected = false;
        try {
            refresh_machine_ui_observer(root, profile, selected_machine, seed, prepare);
        } catch (...) {
            rejected = true;
        }

        check(rejected && prepares == count &&
                      read_private_json_bytes(scoped / "android-launch.json") == bad_bytes,
              "invalid present observer repaired or accepted");
    }

    put(scoped / "android-launch.json", before);
    auto result = refresh_machine_ui_observer(root, profile, selected_machine, seed, prepare);
    check(result.at("guest_started") == false && prepares == 1,
          "refresh did not prepare exactly once");
    auto expected = launch;
    expected["ui_observer"] = descriptor;

    if (!a9) {
        expected["ui_observer"]["profile"] = profile;
    }

    expected["pins"].push_back({{"path", dex.string()}, {"sha256", sha256_file(dex)}});
    auto expected_manifest = manifest;
    expected_manifest["ui_observer"] = {{"file", "observer/classes.dex"},
                                        {"bytes", std::filesystem::file_size(dex)},
                                        {"sha256", sha256_file(dex)}};
    auto expected_activation = activation;
    expected_activation["manifest_sha256"] = sha256_file(seed / "runtime.json");
    check(read_private_json(scoped / "android-launch.json") == expected &&
                  read_private_json(scoped / "activation.json") == expected_activation &&
                  read_private_json(seed / "runtime.json") == expected_manifest &&
                  sha256_file(seed / "observer/classes.dex") == sha256_file(dex) &&
                  std::filesystem::hard_link_count(seed / "observer/classes.dex") == 1,
          "observer refresh failed portable payload/manifest/configuration transaction");
    const auto embedded = seed / "observer/classes.dex";
    put(embedded, "tampered embedded DEX");
    const auto before_tampered_prepare = prepares;
    bool embedded_rejected = false;
    try {
        refresh_machine_ui_observer(root, profile, selected_machine, seed, prepare);
    } catch (...) {
        embedded_rejected = true;
    }

    check(embedded_rejected && prepares == before_tampered_prepare &&
                  sha256_file(embedded) == sha256_bytes("tampered embedded DEX"),
          "tampered embedded observer repaired or compiled");
    put(embedded, "source DEX fixture");
    const auto refreshed = read_private_json_bytes(scoped / "android-launch.json");
    put(dex, "tampered DEX");
    const auto count = prepares;
    bool rejected = false;
    try {
        refresh_machine_ui_observer(root, profile, selected_machine, seed, prepare);
    } catch (...) {
        rejected = true;
    }

    check(rejected && prepares == count &&
                  read_private_json_bytes(scoped / "android-launch.json") == refreshed,
          "tampered present observer repaired or accepted");
}

void observer_build_snapshot_checks(const std::filesystem::path &stage) {
    for (const auto *profile : {"free-09-chinese-20260106-6.0.3", "free-11-chinese-20260731-7.3.1",
                                "passion-11-russian-20260403-2.1.4.80"}) {
        for (const bool named : {false, true}) {
            const auto root = stage / (std::string("observer-build-") + profile +
                                       (named ? "-named" : "-canonical"));
            const std::string machine = named ? "source-build" : "";
            const auto selected_machine = machine_identity(profile, machine);
            const auto seed = root / "machines" / (named ? machine : profile);
            const bool a9 = std::string_view(profile) == qinggan09_owned_packages_profile;
            const Json android = {
                    {"package", a9 ? "system-images;android-28;default;arm64-v8a"
                                   : "system-images;android-30;default;arm64-v8a"},
                    {"revision", "1"},
                    {"url",
                     a9 ? "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip"
                        : "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"},
                    {"sha256",
                     a9 ? "86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9"
                        : "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304"},
                    {"clean_base", android_base(a9 ? 28 : 30).at("clean_base")}};
            Json definition = {{"platform", a9 ? "qinggan09"
                                            : std::string_view(profile).starts_with("passion")
                                                    ? "passion"
                                                    : "qinggan11"},
                               {"board", "8155"},
                               {"title", "Synthetic Observer Build 1.0"},
                               {"android", android}};

            if (!a9) {
                definition["id"] = profile;
            }

            write_private_json(seed / "profile.json", definition);
            put(seed / "android/userdata-qemu.img", "installed application state");
            put(seed / "qnx/persistent.img", "installed QNX state");
            Json manifest = {{"schema", 1},
                             {"kind", "portable-guest-runtime"},
                             {"profile", profile},
                             {"profile_sha256", sha256_file(seed / "profile.json")},
                             {"android", {{"api", a9 ? 28 : 30}}}};
            write_private_json(seed / "runtime.json", manifest);
            const auto scoped = root / ".local/machines" / (named ? machine : profile);
            ensure_private_directory(scoped);
            const auto dex = root / ".local/toolchains/ui-observer/current/classes.dex";
            put(dex, "new source-built observer");
            const Json observer = {{"dex", dex.string()}, {"sha256", sha256_file(dex)}};
            unsigned prepares = 0;
            auto prepare = [&] {
                ++prepares;

                return observer;
            };
            // The old configuration-only path cannot publish an unactivated portable build.
            bool denied = false;
            try {
                refresh_machine_ui_observer(root, profile, selected_machine, seed, prepare);
            } catch (...) {
                denied = true;
            }

            check(denied && prepares == 0,
                  "configuration refresh accepted an unactivated source build");

            if (!a9) {
                auto no_id = definition;
                no_id.erase("id");
                write_private_json(seed / "profile.json", no_id);
                auto wrong = manifest;
                wrong["profile_sha256"] = sha256_file(seed / "profile.json");
                write_private_json(seed / "runtime.json", wrong);
                bool missing_id_rejected = false;
                try {
                    refresh_machine_ui_observer(root, profile, selected_machine, seed, prepare,
                                                false);
                } catch (...) {
                    missing_id_rejected = true;
                }

                check(missing_id_rejected && prepares == 0,
                      "API30 snapshot identity requirement relaxed");
                write_private_json(seed / "profile.json", definition);
                write_private_json(seed / "runtime.json", manifest);
            }

            std::map<std::filesystem::path, std::pair<Json, std::string>> retained;

            for (const auto *file :
                 {"profile.json", "android/userdata-qemu.img", "qnx/persistent.img"}) {
                retained.emplace(seed / file, std::make_pair(cache_file_identity(seed / file),
                                                             sha256_file(seed / file)));
            }

            auto preserved = [&] {
                for (const auto &[file, state] : retained) {
                    check(cache_file_identity(file) == state.first &&
                                  sha256_file(file) == state.second,
                          "source observer publication changed guest state");
                }
            };
            const auto result = refresh_machine_ui_observer(root, profile, selected_machine, seed,
                                                            prepare, false);
            check(prepares == 1 && result.at("host_refreshed") == false &&
                          result.at("embedded_observer_updated") == true &&
                          result.at("guest_started") == false,
                  "portable source observer publication failed");
            preserved();
            manifest["ui_observer"] = {{"file", "observer/classes.dex"},
                                       {"bytes", std::filesystem::file_size(dex)},
                                       {"sha256", sha256_file(dex)}};
            check(read_private_json(seed / "runtime.json") == manifest &&
                          sha256_file(seed / "observer/classes.dex") == sha256_file(dex) &&
                          std::filesystem::hard_link_count(seed / "observer/classes.dex") == 1,
                  "source build retained its stale embedded observer");
            check(!std::filesystem::exists(scoped / "activation.json") &&
                          !std::filesystem::exists(scoped / "android-launch.json"),
                  "portable build invented private launch configuration");
            // Existing private configuration can be stale after the system commit; preserve it.
            write_private_json(scoped / "activation.json", {{"stale", "private activation"}});
            write_private_json(scoped / "android-launch.json", {{"stale", "private launch"}});
            put(dex, "next source observer");
            const auto next = Json{{"dex", dex.string()}, {"sha256", sha256_file(dex)}};
            refresh_machine_ui_observer(
                    root, profile, selected_machine, seed,
                    [&] {
                        return next;
                    },
                    false);
            preserved();
            check(sha256_file(seed / "observer/classes.dex") == sha256_file(dex) &&
                          read_private_json(scoped / "activation.json") ==
                                  Json{{"stale", "private activation"}} &&
                          read_private_json(scoped / "android-launch.json") ==
                                  Json{{"stale", "private launch"}},
                  "build refresh consumed or rewrote stale private configuration");
            const auto before = read_private_json_bytes(seed / "runtime.json"),
                       payload = sha256_file(seed / "observer/classes.dex");
            bool corrupt = false;
            try {
                auto bad = next;
                bad["sha256"] = std::string(64, '0');
                refresh_machine_ui_observer(
                        root, profile, selected_machine, seed,
                        [&] {
                            return bad;
                        },
                        false);
            } catch (...) {
                corrupt = true;
            }

            check(corrupt && read_private_json_bytes(seed / "runtime.json") == before &&
                          sha256_file(seed / "observer/classes.dex") == payload,
                  "invalid source observer damaged portable build");
            preserved();
        }
    }
}

void qnx_upgrade_checks(const std::filesystem::path &stage, bool firmware_611 = false) {
    const std::string profile = firmware_611 ? "free-11-chinese-20250710-6.11.1"
                                             : "free-11-chinese-20260731-7.3.1",
                      machine = "";
    const auto selected_machine = machine_identity(profile, machine);
    const auto root = stage / (firmware_611 ? "qnx-upgrade611" : "qnx-upgrade"),
               seed = root / "machines" / profile, data = seed / "android",
               scoped = root / ".local/machines" / profile;

    ensure_private_directory(data);
    ensure_private_directory(scoped);
    write_private_json(
            seed / "profile.json",
            {{"id", profile}, {"title", "Synthetic QNX Upgrade 1.0"}, {"android", {{"api", 30}}}});
    write_private_json(seed / "runtime.json",
                       {{"schema", 1},
                        {"kind", "portable-guest-runtime"},
                        {"profile", profile},
                        {"profile_sha256", sha256_file(seed / "profile.json")},
                        {"android", {{"api", 30}}}});
    std::map<std::filesystem::path, std::pair<Json, std::string>> disks;

    for (const auto *name : {"userdata-qemu.img", "userdata-qemu.img.qcow2", "cache.img",
                             "system.img", "vendor.img", "installed-app.bin"}) {
        const auto file = data / name;
        put(file, name);
        disks[file] = {cache_file_identity(file), sha256_file(file)};
    }

    const auto preserved = [&] {
        for (const auto &[path, state] : disks) {
            check(cache_file_identity(path) == state.first && sha256_file(path) == state.second,
                  "QNX upgrade changed installed Android state");
        }
    };
    const auto qnx_source = root / ".local/qnx-upgrade-source";
    ensure_private_directory(qnx_source / "qnx");
    ensure_private_directory(seed / "qnx");
    auto with_qnx = read_private_json(seed / "runtime.json");
    Json old_qnx = {{"guest_arch", "aarch64"}, {"images", Json::object()}}, new_qnx = old_qnx;
    const auto qnx_layout = qnx_runtime_layout(firmware_611 ? "sa8155-qinggan11-611"
                                                            : "sa8155-qinggan11-731");
    qnx_runtime_metadata(old_qnx, qnx_layout);
    qnx_runtime_metadata(new_qnx, qnx_layout);

    for (const auto &name : qnx_layout.images) {
        const auto old_file = seed / "qnx" / name, new_file = qnx_source / "qnx" / name;
        put(old_file, "old " + name);
        put(new_file, "new " + name);
        old_qnx["images"][name] = {{"file", "qnx/" + name},
                                   {"bytes", std::filesystem::file_size(old_file)},
                                   {"sha256", sha256_file(old_file)}};
        new_qnx["images"][name] = {{"file", "qnx/" + name},
                                   {"bytes", std::filesystem::file_size(new_file)},
                                   {"sha256", sha256_file(new_file)}};
    }

    with_qnx["qnx"] = old_qnx;
    write_private_json(seed / "runtime.json", with_qnx);
    const auto qnx_before = read_private_json_bytes(seed / "runtime.json"),
               qnx_pin = sha256_bytes(qnx_before);
    const auto old_payload = sha256_file(seed / "qnx/hyp-hvf.img");
    auto rejects_qnx = [&](const std::string &pin, const Json &qnx,
                           const std::function<void()> &callback) {
        bool denied = false;
        try {
            publish_machine_qnx(root, profile, selected_machine, pin, qnx_source, qnx, callback);
        } catch (...) {
            denied = true;
        }

        check(denied && read_private_json_bytes(seed / "runtime.json") == qnx_before &&
                      sha256_file(seed / "qnx/hyp-hvf.img") == old_payload,
              "failed QNX publication changed accepted machine");
        preserved();
    };
    rejects_qnx(std::string(64, '0'), new_qnx, [] {});
    auto bad_qnx = new_qnx;
    bad_qnx["images"]["hyp-hvf.img"]["sha256"] = std::string(64, '0');
    rejects_qnx(qnx_pin, bad_qnx, [] {});
    unsigned qnx_verifies = 0;
    rejects_qnx(qnx_pin, new_qnx, [&] {
        if (++qnx_verifies == 3) {
            throw std::runtime_error("injected QNX post-swap failure");
        }
    });
    check(qnx_verifies == 3, "QNX rollback did not exercise post-swap failure");
    put(seed / "qnx/unknown-user-file", "keep me");
    rejects_qnx(qnx_pin, new_qnx, [] {});
    check(read_private_json_bytes(seed / "runtime.json") == qnx_before,
          "unknown QNX file rejection changed manifest");
    std::filesystem::remove(seed / "qnx/unknown-user-file");

    bool rollback_failed = false;
    std::filesystem::path retained;
    unsigned fail_at = 0;
    try {
        publish_machine_qnx(root, profile, selected_machine, qnx_pin, qnx_source, new_qnx, [&] {
            if (++fail_at == 3) {
                for (const auto &entry :
                     std::filesystem::directory_iterator(root / ".local/build/tmp")) {
                    if (entry.is_directory() &&
                        entry.path().filename().string().starts_with("qnx-upgrade-")) {
                        retained = entry.path();
                        ensure_private_directory(retained / "new");
                        break;
                    }
                }

                throw std::runtime_error("injected rollback target collision");
            }
        });
    } catch (...) {
        rollback_failed = true;
    }

    check(rollback_failed && !retained.empty() &&
                  sha256_file(retained / "old/hyp-hvf.img") == old_payload &&
                  read_private_json_bytes(seed / "runtime.json") == qnx_before,
          "failed rollback discarded old QNX payload");
    preserved();
    std::filesystem::remove_all(seed / "qnx");
    std::filesystem::rename(retained / "old", seed / "qnx");
    std::filesystem::remove_all(retained);
    const auto qnx_result = publish_machine_qnx(root, profile, selected_machine, qnx_pin,
                                                qnx_source, new_qnx, [] {});
    auto expected_qnx = with_qnx;
    expected_qnx["qnx"] = new_qnx;
    check(qnx_result.at("applications_preserved") == true &&
                  read_private_json(seed / "runtime.json") == expected_qnx &&
                  sha256_file(seed / "qnx/hyp-hvf.img") ==
                          sha256_file(qnx_source / "qnx/hyp-hvf.img"),
          "QNX source publication differs");
    preserved();
}

void qnx_persistent_upgrade_checks(const std::filesystem::path &stage, bool external = false) {
    const std::string profile = "free-09-chinese-20260106-6.0.3";
    const auto root = stage / (external ? "qnx-portable-upgrade" : "qnx-persistent-upgrade"),
               seed = external ? root / ".local/build/portable-machine"
                               : root / "machines" / profile;
    const std::string machine = external ? seed.filename().string() : "";
    const auto selected_machine = machine_identity(profile, machine);
    const auto data = seed / "android",
               scoped = profile_launch_directory(root, profile, selected_machine);

    ensure_private_directory(data);
    ensure_private_directory(scoped);
    write_private_json(
            seed / "profile.json",
            {{"id", profile}, {"title", "Synthetic QNX Upgrade 1.0"}, {"android", {{"api", 28}}}});
    write_private_json(seed / "runtime.json",
                       {{"schema", 1},
                        {"kind", "portable-guest-runtime"},
                        {"profile", profile},
                        {"profile_sha256", sha256_file(seed / "profile.json")},
                        {"android", {{"api", 28}}}});
    std::map<std::filesystem::path, std::pair<Json, std::string>> disks;

    for (const auto *name : {"userdata-qemu.img", "userdata-qemu.img.qcow2", "cache.img",
                             "system.img", "vendor.img", "installed-app.bin"}) {
        const auto file = data / name;
        put(file, name);
        disks[file] = {cache_file_identity(file), sha256_file(file)};
    }

    const auto preserved = [&] {
        for (const auto &[path, state] : disks) {
            check(cache_file_identity(path) == state.first && sha256_file(path) == state.second,
                  "QNX upgrade changed installed Android state");
        }
    };
    const auto qnx_source = root / ".local/qnx-upgrade-source";
    ensure_private_directory(qnx_source / "qnx");
    ensure_private_directory(seed / "qnx");
    auto with_qnx = read_private_json(seed / "runtime.json");
    Json old_qnx = {{"guest_arch", "aarch64"}, {"images", Json::object()}}, new_qnx = old_qnx;
    auto qnx_layout = qnx_runtime_layout("sa8155-qinggan09");
    qnx_layout.persistence = {{"schema", 1},
                              {"system_image", "sysla-gpt-ramdisk-v3.img"},
                              {"userdata_image", "userdata-qnx6.img"}};
    qnx_layout.interguest_network = qnx_interguest_contract();
    qnx_layout.images.push_back("userdata-qnx6.img");
    qnx_runtime_metadata(old_qnx, qnx_layout);
    qnx_runtime_metadata(new_qnx, qnx_layout);

    for (const auto &name : qnx_layout.images) {
        const auto old_file = seed / "qnx" / name, new_file = qnx_source / "qnx" / name;
        put(old_file, "old " + name);
        put(new_file, "new " + name);
        old_qnx["images"][name] = {{"file", "qnx/" + name},
                                   {"bytes", std::filesystem::file_size(old_file)},
                                   {"sha256", sha256_file(old_file)}};
        new_qnx["images"][name] = {{"file", "qnx/" + name},
                                   {"bytes", std::filesystem::file_size(new_file)},
                                   {"sha256", sha256_file(new_file)}};
    }

    for (const auto *name : {"sysla-gpt-ramdisk-v3.img", "userdata-qnx6.img"}) {
        const auto file = seed / "qnx" / name;
        put(file, "app " + std::string(name));
        disks[file] = {cache_file_identity(file), sha256_file(file)};
    }

    with_qnx["qnx"] = old_qnx;
    write_private_json(seed / "runtime.json", with_qnx);
    const auto qnx_before = read_private_json_bytes(seed / "runtime.json"),
               qnx_pin = sha256_bytes(qnx_before);
    const auto old_payload = sha256_file(seed / "qnx/hyp-hvf.img");
    const auto publish = [&](const std::string &pin, const Json &qnx,
                             const std::function<void()> &callback) {
        return external ? publish_machine_qnx_boot_at(root, profile, seed, pin, qnx_source, qnx,
                                                      callback)
                        : publish_machine_qnx_boot(root, profile, selected_machine, pin, qnx_source,
                                                   qnx, callback);
    };

    if (external) {
        bool legacy_denied = false;
        try {
            publish_machine_qnx_boot(root, profile, selected_machine, qnx_pin, qnx_source, new_qnx,
                                     [] {});
        } catch (...) {
            legacy_denied = true;
        }

        check(legacy_denied, "name-only publisher unexpectedly found a portable machine");
        const auto alias = root / ".local/build/alias";
        std::filesystem::create_directory_symlink(seed, alias);
        bool alias_denied = false;
        try {
            publish_machine_qnx_boot_at(root, profile, alias, qnx_pin, qnx_source, new_qnx, [] {});
        } catch (...) {
            alias_denied = true;
        }

        check(alias_denied, "symlink machine alias accepted");
        std::filesystem::remove(alias);
    }

    auto rejects_qnx = [&](const std::string &pin, const Json &qnx,
                           const std::function<void()> &callback) {
        bool denied = false;
        try {
            publish(pin, qnx, callback);
        } catch (...) {
            denied = true;
        }

        check(denied && read_private_json_bytes(seed / "runtime.json") == qnx_before &&
                      sha256_file(seed / "qnx/hyp-hvf.img") == old_payload,
              "failed QNX publication changed accepted machine");
        preserved();
    };
    rejects_qnx(std::string(64, '0'), new_qnx, [] {});
    auto bad_qnx = new_qnx;
    bad_qnx["images"]["hyp-hvf.img"]["sha256"] = std::string(64, '0');
    rejects_qnx(qnx_pin, bad_qnx, [] {});
    unsigned qnx_verifies = 0;
    rejects_qnx(qnx_pin, new_qnx, [&] {
        if (++qnx_verifies == 3) {
            throw std::runtime_error("injected QNX post-swap failure");
        }
    });
    check(qnx_verifies == 3, "QNX rollback did not exercise post-swap failure");
    rejects_qnx(qnx_pin, new_qnx, [] {
        throw std::runtime_error("accepted source changed");
    });

    if (external) {
        const auto session = native_session_directory(root, profile, selected_machine,
                                                      "0123456789abcdef0123456789abcdef");
        ensure_private_directory(session);
        write_private_json(session / "runtime.json",
                           {{"session_id", session.filename().string()}, {"stopped", false}});
        const auto active = native_current_active_directory(root, profile, selected_machine) /
                            (profile + ".json");
        write_private_json(active, {{"session_id", session.filename().string()},
                                    {"directory", session.string()}});
        rejects_qnx(qnx_pin, new_qnx, [] {});
        std::filesystem::remove(active);
        unsigned nested = 0;
        rejects_qnx(qnx_pin, new_qnx, [&] {
            ++nested;
            publish(qnx_pin, new_qnx, [] {});
        });
        check(nested == 1, "concurrent portable publisher bypassed shared seed lock");
    }

    put(seed / "qnx/unknown-user-file", "keep me");
    rejects_qnx(qnx_pin, new_qnx, [] {});
    check(read_private_json_bytes(seed / "runtime.json") == qnx_before,
          "unknown QNX file rejection changed manifest");
    std::filesystem::remove(seed / "qnx/unknown-user-file");

    const auto qnx_result = publish(qnx_pin, new_qnx, [] {});
    auto expected_qnx = with_qnx;
    expected_qnx["qnx"] = new_qnx;

    for (const auto *name : {"sysla-gpt-ramdisk-v3.img", "userdata-qnx6.img"}) {
        expected_qnx["qnx"]["images"][name] = old_qnx.at("images").at(name);
    }

    check(qnx_result.at("persistent_storage_preserved") == true &&
                  qnx_result.at("applications_preserved") == true &&
                  read_private_json(seed / "runtime.json") == expected_qnx &&
                  sha256_file(seed / "qnx/hyp-hvf.img") ==
                          sha256_file(qnx_source / "qnx/hyp-hvf.img"),
          "QNX source publication differs");
    preserved();
}

void system_upgrade_checks(const std::filesystem::path &stage,
                           const std::filesystem::path &executable, bool named = true) {
    const std::string profile = "free-11-chinese-20260731-7.3.1", machine = named ? "current" : "";
    const auto selected_machine = machine_identity(profile, machine);
    const auto root = stage / (named ? "system-upgrade" : "system-upgrade-default"),
               seed = root / "machines" / (named ? machine : profile), data = seed / "android",
               scoped = root / ".local/machines" / (named ? machine : profile);

    ensure_private_directory(data);
    write_private_json(
            seed / "profile.json",
            {{"id", profile},
             {"title", "Synthetic System Upgrade 1.0"},
             {"android",
              {{"package", "system-images;android-30;default;arm64-v8a"},
               {"revision", "1"},
               {"url",
                "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"},
               {"sha256", "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304"},
               {"clean_base", android_base(30).at("clean_base")}}}});
    put(data / "system.img", "old system");
    Json manifest = {{"schema", 1},
                     {"kind", "portable-guest-runtime"},
                     {"profile", profile},
                     {"profile_sha256", sha256_file(seed / "profile.json")},
                     {"android",
                      {{"api", 30},
                       {"images",
                        {{"system.img",
                          {{"file", "android/system.img"},
                           {"bytes", 10},
                           {"sha256", sha256_file(data / "system.img")}}}}}}}};
    write_private_json(seed / "runtime.json", manifest);
    const auto before = read_private_json_bytes(seed / "runtime.json"),
               before_sha = sha256_bytes(before);
    std::map<std::filesystem::path, std::pair<Json, std::string>> disks;

    for (const auto *name :
         {"userdata-qemu.img", "userdata-qemu.img.qcow2", "encryptionkey.img",
          "encryptionkey.img.qcow2", "cache.img", "cache.img.qcow2", "system.img.qcow2",
          "vendor.img", "vendor.img.qcow2", "qnx/runtime.img", "ui/resource.png"}) {
        const auto path = data / name;
        put(path, name);
        disks[path] = {cache_file_identity(path), sha256_file(path)};
    }

    const auto sdk = root / ".local/toolchains/native-sdk";
#ifdef _WIN32
    const auto tool = sdk / "emulator/qemu-img.exe";
#else
    const auto tool = sdk / "emulator/qemu-img";
#endif
    ensure_private_directory(tool.parent_path());
    std::filesystem::copy_file(executable, tool);
    write_private_json(scoped / "android-launch.json",
                       {{"machine", data.string()}, {"android_sdk_root", sdk.string()}});
    const auto source = root / "candidate/system.img";
    put(source, "new verified system");
    const auto sha = sha256_file(source);
    const auto extent = std::filesystem::file_size(source);
    const auto target = data / "system-versions" / sha;
    const Json disk_tool = {{"path", tool.string()}, {"sha256", sha256_file(tool)}};
    const auto preserved = [&] {
        for (const auto &[path, state] : disks) {
            check(cache_file_identity(path) == state.first && sha256_file(path) == state.second,
                  "system upgrade changed app disk identity or bytes");
        }
    };
    std::string rejection;
    auto rejected = [&](const std::string &expected, const std::function<void()> &callback) {
        bool denied = false;
        try {
            publish_machine_android_system(root, profile, selected_machine, expected, source, sha,
                                           extent, callback, disk_tool, Json(), 0);
        } catch (const std::exception &error) {
            denied = true;
            rejection = error.what();
        }

        check(denied, "unsafe system upgrade accepted");
        preserved();
    };
    rejected(std::string(64, '0'), [] {});
    check(read_private_json_bytes(seed / "runtime.json") == before, "stale CAS changed manifest");
    const auto directory = native_session_directory(root, profile, selected_machine, "owned");
    write_private_json(native_current_active_directory(root, profile, selected_machine) /
                               (profile + ".json"),
                       {{"session_id", "owned"}, {"directory", directory.string()}});
    write_private_json(directory / "runtime.json", {{"session_id", "owned"}, {"stopped", false}});
    rejected(before_sha, [] {});
    write_private_json(directory / "runtime.json", {{"session_id", "owned"}, {"stopped", true}});
#ifdef _WIN32
    _putenv_s("VOYEMU_SYSTEM_UPGRADE_FIXTURE", "1");
#else
    setenv("VOYEMU_SYSTEM_UPGRADE_FIXTURE", "1", 1);
#endif
    {
        auto bad_tool = disk_tool;
        bad_tool["sha256"] = std::string(64, '0');
        bool denied = false;
        try {
            publish_machine_android_system(
                    root, profile, selected_machine, before_sha, source, sha, extent, [] {},
                    bad_tool, Json(), 0);
        } catch (...) {
            denied = true;
        }

        check(denied && read_private_json_bytes(seed / "runtime.json") == before &&
                      !std::filesystem::exists(target),
              "unverified disk tool changed accepted generation");
        preserved();
    }
    int verifies = 0;
    rejected(before_sha, [&] {
        if (++verifies == 2) {
            throw std::runtime_error("injected staged candidate failure");
        }
    });

    if (verifies != 2 || read_private_json_bytes(seed / "runtime.json") != before ||
        std::filesystem::exists(target)) {
        throw std::runtime_error("staged failure unexpected gate: " + rejection +
                                 " verifies=" + std::to_string(verifies));
    }

    verifies = 0;
    auto foreign = manifest;
    foreign["foreign_writer"] = true;
    rejected(before_sha, [&] {
        if (++verifies == 2) {
            write_private_json(seed / "runtime.json", foreign);
        }
    });
    check(read_private_json(seed / "runtime.json") == foreign && !std::filesystem::exists(target),
          "system upgrade overwrote foreign manifest CAS change");
    put(seed / "runtime.json", before);
    bool reserve_rejected = false;
    try {
        publish_machine_android_system(
                root, profile, selected_machine, before_sha, source, sha, extent, [] {}, disk_tool,
                Json(), std::numeric_limits<std::uint64_t>::max());
    } catch (...) {
        reserve_rejected = true;
    }

    check(reserve_rejected && read_private_json_bytes(seed / "runtime.json") == before &&
                  !std::filesystem::exists(target),
          "unavailable publication reserve changed accepted machine");
    preserved();
    auto result = publish_machine_android_system(
            root, profile, selected_machine, before_sha, source, sha, extent, [] {}, disk_tool,
            Json(), 0);
    preserved();
    check(result.at("applications_preserved") == true &&
                  result.at("previous_system_sha256") == sha256_bytes("old system") &&
                  sha256_file(target / "system.img") == sha,
          "system-only publication differs");
    const auto updated = read_private_json(seed / "runtime.json");
    auto expected = manifest;
    expected["android"]["images"]["system.img"] = {
            {"file", "android/system-versions/" + sha + "/system.img"},
            {"bytes", extent},
            {"sha256", sha}};
    check(updated == expected, "system-only publication changed unrelated manifest fields");
    check(publish_machine_android_system(
                  root, profile, selected_machine, sha256_file(seed / "runtime.json"), source, sha,
                  extent, [] {}, disk_tool, Json(), 0)
                          .at("cached") == true,
          "idempotent system upgrade rejected");
    preserved();
    const auto apk_source = root / "candidate/provider.apk";
    put(apk_source, "owned provider package");
    const auto apk_sha = sha256_file(apk_source);
    const Json package = {{"package", "com.voyemu.input.wheel"},
                          {"file", apk_source.string()},
                          {"sha256", apk_sha},
                          {"bytes", std::filesystem::file_size(apk_source)}};
    const auto package_target = data / "packages" / (apk_sha + ".apk");
    const auto manifest_before_package = read_private_json_bytes(seed / "runtime.json");
    auto rejects_package = [&](const Json &value, const std::function<void()> &callback) {
        bool denied = false;
        try {
            publish_machine_android_system(root, profile, selected_machine,
                                           sha256_bytes(manifest_before_package), source, sha,
                                           extent, callback, disk_tool, value, 0);
        } catch (...) {
            denied = true;
        }

        check(denied && read_private_json_bytes(seed / "runtime.json") == manifest_before_package &&
                      !std::filesystem::exists(package_target),
              "failed package publication changed accepted manifest");
        preserved();
    };
    auto wrong_package = package;
    wrong_package["package"] = "foreign.package";
    rejects_package(wrong_package, [] {});
    wrong_package = package;
    wrong_package["sha256"] = std::string(64, '0');
    rejects_package(wrong_package, [] {});
    wrong_package = package;
    wrong_package["file"] = (root / "missing.apk").string();
    rejects_package(wrong_package, [] {});
    verifies = 0;
    rejects_package(package, [&] {
        if (++verifies == 2) {
            throw std::runtime_error("injected staged APK failure");
        }
    });
    const auto package_result = publish_machine_android_system(
            root, profile, selected_machine, sha256_bytes(manifest_before_package), source, sha,
            extent, [] {}, disk_tool, package, 0);
    check(package_result.at("cached") == false && sha256_file(package_target) == apk_sha,
          "same-system provider update was skipped");
    auto expected_package = updated;
    auto portable_package = package;
    portable_package["file"] = "android/packages/" + apk_sha + ".apk";
    expected_package["android"]["owned_package"] = portable_package;
    check(read_private_json(seed / "runtime.json") == expected_package,
          "APK publication changed unrelated manifest fields");
    preserved();
    check(publish_machine_android_system(
                  root, profile, selected_machine, sha256_file(seed / "runtime.json"), source, sha,
                  extent, [] {}, disk_tool, package, 0)
                          .at("cached") == true,
          "idempotent owned package publication rejected");
    preserved();

    const auto camera_source = root / "candidate/camera.apk";
    put(camera_source, "prepared camera package");
    const auto camera_sha = sha256_file(camera_source);
    const Json camera = {{"package", "com.qinggan.camera"},
                         {"file", camera_source.string()},
                         {"sha256", camera_sha},
                         {"bytes", std::filesystem::file_size(camera_source)}};
    const auto before_camera = sha256_file(seed / "runtime.json");
    const auto camera_result = publish_machine_android_system(
            root, profile, selected_machine, before_camera, source, sha, extent, [] {}, disk_tool,
            package, 0, Json(), camera);
    check(camera_result.at("cached") == false, "Camera change skipped");
    auto both = expected_package;
    auto portable_camera = camera;
    portable_camera["file"] = "android/packages/" + camera_sha + ".apk";
    both["android"]["camera_package"] = portable_camera;
    check(read_private_json(seed / "runtime.json") == both &&
                  sha256_file(data / "packages" / (camera_sha + ".apk")) == camera_sha,
          "Camera and wheel descriptors must coexist");
    preserved();
    check(publish_machine_android_system(
                  root, profile, selected_machine, sha256_file(seed / "runtime.json"), source, sha,
                  extent, [] {}, disk_tool, package, 0, Json(), camera)
                          .at("cached") == true,
          "current Camera must be idempotent");
    preserved();

    const auto navi_source = root / "candidate/navi.apk";
    put(navi_source, "prepared navi package");
    const auto navi_sha = sha256_file(navi_source);
    const Json navi = {{"package", "com.baidu.naviauto"},
                       {"file", navi_source.string()},
                       {"sha256", navi_sha},
                       {"bytes", std::filesystem::file_size(navi_source)}};
    const Json prepared = Json::array({camera, navi});
    const auto before_list = sha256_file(seed / "runtime.json");
    auto published_list = publish_machine_android_system(
            root, profile, selected_machine, before_list, source, sha, extent, [] {}, disk_tool,
            package, 0, Json(), Json(), prepared);
    check(published_list.at("cached") == false, "prepared list migration skipped");
    auto expected_list = both;
    expected_list["android"].erase("camera_package");
    auto portable_navi = navi;
    portable_navi["file"] = "android/packages/" + navi_sha + ".apk";
    expected_list["android"]["prepared_packages"] = Json::array({portable_camera, portable_navi});
    check(read_private_json(seed / "runtime.json") == expected_list &&
                  sha256_file(data / "packages" / (navi_sha + ".apk")) == navi_sha,
          "prepared list, wheel or migration differs");
    preserved();
    check(publish_machine_android_system(
                  root, profile, selected_machine, sha256_file(seed / "runtime.json"), source, sha,
                  extent, [] {}, disk_tool, package, 0, Json(), Json(), prepared)
                          .at("cached") == true,
          "prepared list repeat must be cached");
    auto invalid_list = prepared;
    invalid_list.erase(1);
    bool rejected_list = false;
    try {
        publish_machine_android_system(
                root, profile, selected_machine, sha256_file(seed / "runtime.json"), source, sha,
                extent, [] {}, disk_tool, package, 0, Json(), Json(), invalid_list);
    } catch (...) {
        rejected_list = true;
    }

    check(rejected_list && read_private_json(seed / "runtime.json") == expected_list,
          "incomplete prepared list accepted");

#ifdef _WIN32
    _putenv_s("VOYEMU_SYSTEM_UPGRADE_FIXTURE", "");
#else
    unsetenv("VOYEMU_SYSTEM_UPGRADE_FIXTURE");
#endif
}

void passion_wheel_refresh_checks(const std::filesystem::path &stage) {
    const std::string profile = "passion-11-russian-20260403-2.1.4.80";
    const auto default_machine = machine_identity(profile, "");
    const auto root = stage / "passion-wheel", seed = root / "machines" / profile;
    Json definition = {
            {"id", profile},
            {"title", "Synthetic Passion 1.0"},
            {"platform", "passion"},
            {"board", "8155"},
            {"status", "incomplete"},
            {"android",
             {{"package", "system-images;android-30;default;arm64-v8a"},
              {"revision", "1"},
              {"url",
               "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"},
              {"sha256", "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304"},
              {"clean_base", android_base(30).at("clean_base")}}},
            {"patches",
             {{"MegaNexusService", {{"steps", Json::array({"passion-api30-nexus-power.json"})}}},
              {"passion-api30-secondary-navigation",
               {{"steps", Json::array({"passion-api30-display-power.json"})}}},
              {"MonzaCarService",
               {{"input", "original:MonzaCarService"},
                {"type", "apk"},
                {"steps", Json::array({"framework-libraries.json", "passion-api30-diagnostics.json",
                                       "passion-api30-mcu-property.json"})}}}}}};
    // Deliberately preserve insertion order different from Json's sorted object order.
    nlohmann::ordered_json producer;

    producer["id"] = profile;

    for (auto it = definition.begin(); it != definition.end(); ++it) {
        if (it.key() != "id") {
            producer[it.key()] = it.value();
        }
    }

    const auto producer_bytes = producer.dump(2) + "\n",
               producer_pin = sha256_bytes(producer_bytes);
    check(producer_pin != sha256_bytes(definition.dump(2) + "\n"),
          "fixture must distinguish ordered source bytes");
    Json images = Json::object();

    for (const auto *role :
         {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img", "cache.img"}) {
        const auto file = seed / "android" / role;

        if (std::string_view(role) == "cache.img") {
            ensure_private_directory(file.parent_path());
            std::ofstream out(file, std::ios::binary);
            out.seekp(1080);
            out.put(char(0x53));
            out.put(char(0xef));
            out.seekp(69206015);
            out.put(0);
        } else {
            put(file, role);
        }

        images[role] = {{"file", std::string("android/") + role},
                        {"bytes", std::filesystem::file_size(file)},
                        {"sha256", sha256_file(file)}};
    }

    Json candidate = {{"schema", 1},
                      {"kind", "android-candidate"},
                      {"profile", profile},
                      {"profile_sha256", producer_pin},
                      {"clean_base_archive_sha256", definition.at("android").at("sha256")},
                      {"artifact_result_sha256", std::string(64, 'd')},
                      {"prepared_VM", true},
                      {"stopped", true},
                      {"cold_boot_verified", true},
                      {"android", {{"images", images}}},
                      {"installed", Json::array()},
                      {"checks", Json::array()}};

    for (const auto &pair : std::vector<std::pair<std::string, std::string>>{
                 {"boot", "1"}, {"api", "30"}, {"abi", "arm64-v8a"}, {"selinux", "Enforcing"}}) {
        candidate["checks"].push_back({{"id", pair.first},
                                       {"actual", pair.second},
                                       {"expected", pair.second},
                                       {"passed", true}});
    }

    Json pins = Json::object();
    const auto targets = passion_power_targets(), csv = passion_power_csv();

    for (auto it = targets.begin(); it != targets.end(); ++it) {
        const auto pin = csv.contains(it.key().substr(1))
                                 ? csv.at(it.key().substr(1)).get<std::string>()
                                 : std::string(64, 'e');
        pins[it.key()] = pin;
        candidate["installed"].push_back({{"artifact", it.value()},
                                          {"target", it.key()},
                                          {"sha256", pin},
                                          {"mutable", false}});
        candidate["checks"].push_back(
                {{"id", "artifact:" + it.value().get<std::string>() + ":" + it.key()},
                 {"expected", pin},
                 {"actual", pin},
                 {"passed", true}});
    }

    const std::string apk(64, 'b');
    candidate["installed"].push_back(
            {{"artifact", "patched:MonzaCarService"},
             {"target", "/system/priv-app/MonzaCarService/MonzaCarService.apk"},
             {"sha256", apk},
             {"mutable", false}});
    candidate["checks"].push_back(
            {{"id",
              "artifact:patched:MonzaCarService:/system/priv-app/MonzaCarService/MonzaCarService.apk"},
             {"expected", apk},
             {"actual", apk},
             {"passed", true}});
    Json helpers = Json::object();

    for (const auto &target :
         {std::string("/system/framework/services.jar"),
          std::string("/system/priv-app/MegaNexusService/MegaNexusService.apk")}) {
        const bool sdk = target == "/system/framework/services.jar";
        helpers[target] = {{"contract", "passion-display-power-helper-v1"},
                           {"artifact", passion_power_targets().at(target)},
                           {"archive_sha256", pins.at(target)},
                           {"entry", sdk ? "classes3.dex" : "classes2.dex"},
                           {"sha256", std::string(64, 'f')},
                           {"bytes", std::uint64_t{128}}};
    }

    candidate["display_power"] = passion_power_contract(definition, candidate, pins, helpers);
    candidate["mcu_property"] =
            passion_mcu_contract(definition, candidate, apk,
                                 {{"contract", "passion-mcu-property-helper-v1"},
                                  {"artifact", "patched:MonzaCarService"},
                                  {"archive_sha256", apk},
                                  {"entry", "classes18.dex"},
                                  {"sha256", std::string(64, 'f')},
                                  {"bytes", std::uint64_t{25}}});
    Json manifest = {{"schema", 1},
                     {"kind", "portable-guest-runtime"},
                     {"profile", profile},
                     {"profile_sha256", producer_pin},
                     {"scope", "android"},
                     {"cluster_available", false},
                     {"candidate_receipt_sha256", sha256_bytes(candidate.dump())},
                     {"android",
                      {{"api", 30},
                       {"abi", "arm64-v8a"},
                       {"base_archive_sha256", definition.at("android").at("sha256")},
                       {"images", images}}}};
    auto bind_candidate = [&](Json &target, const Json &proof) {
        for (const auto *role : {"display_power", "mcu_property"}) {
            const auto relative = std::string(role == std::string_view("display_power")
                                                      ? "display-power"
                                                      : "mcu-property") +
                                  "/candidate.json";
            put(seed / relative, proof.dump());
            target["android"][role] = {{"schema", 1},
                                       {"file", relative},
                                       {"bytes", std::filesystem::file_size(seed / relative)},
                                       {"sha256", sha256_file(seed / relative)},
                                       {"capability", proof.at(role).at("capability")}};
        }

        target["candidate_receipt_sha256"] = sha256_bytes(proof.dump());
    };
    bind_candidate(manifest, candidate);
    put(seed / "profile.json", producer_bytes);
    write_private_json(seed / "runtime.json", manifest);
    check(inspect_portable_runtime(root, profile, default_machine) == manifest,
          "unchanged cold producer rejected");
    auto current = producer;
    current["wheel"] = {{"assets", "profiles/common/wheels/passion-2024"},
                        {"provenance", "Approved photographs"},
                        {"artwork", {{"schema", 1}}}};
    Json provenance = {{"schema", 1},
                       {"kind", "host-wheel-resource-refresh"},
                       {"guest_producer_profile_sha256", producer_pin},
                       {"ui_profile_sha256", sha256_bytes(current.dump(2) + "\n")},
                       {"guest_images_rebuilt", false},
                       {"artwork", current.at("wheel").at("artwork")}};
    auto bind_profile = [&](Json &target, const nlohmann::ordered_json &value, Json &record) {
        put(seed / "profile.json", value.dump(2) + "\n");
        target["profile_sha256"] = sha256_file(seed / "profile.json");
        record["ui_profile_sha256"] = target.at("profile_sha256");
    };
    auto bind_provenance = [&](Json &target, const Json &record) {
        put(seed / "ui/cockpit-provenance.json", record.dump());
        target["resources"]["ui/cockpit-provenance.json"] = {
                {"file", "ui/cockpit-provenance.json"},
                {"bytes", std::filesystem::file_size(seed / "ui/cockpit-provenance.json")},
                {"sha256", sha256_file(seed / "ui/cockpit-provenance.json")}};
    };
    bind_profile(manifest, current, provenance);
    bind_provenance(manifest, provenance);
    write_private_json(seed / "runtime.json", manifest);
    const auto evidence_before = read_private_json_bytes(seed / "display-power/candidate.json");
    check(inspect_portable_runtime(root, profile, default_machine) == manifest,
          "host-only wheel refresh rejected power or MCU cold proof");
    check(portable_profile_definition(root, profile, default_machine) == Json(current),
          "host wheel definition lost");

    for (unsigned mode = 0; mode < 18; ++mode) {
        auto bad = manifest, record = provenance, proof = candidate;
        auto changed = current;

        if (mode == 0) {
            record["guest_producer_profile_sha256"] = std::string(64, '0');
        }

        if (mode == 1) {
            record["kind"] = "untrusted";
        }

        if (mode == 2) {
            record["schema"] = 2;
        }

        if (mode == 3) {
            record["guest_images_rebuilt"] = true;
        }

        if (mode == 4) {
            changed["android"]["sha256"] = std::string(64, '0');
        }

        if (mode == 5) {
            changed["patches"]["MonzaCarService"]["steps"].push_back("foreign.json");
        }

        if (mode == 6) {
            changed["wheel"]["androidRoutes"] = "foreign.json";
        }

        if (mode == 7) {
            changed["wheel"]["assets"] = "profiles/common/wheels/foreign";
        }

        if (mode == 8) {
            changed["wheel"]["artwork"]["schema"] = 2;
        }

        if (mode == 9) {
            changed["wheel"] = nullptr;
        }

        if (mode == 10) {
            proof["profile_sha256"] = std::string(64, '0');
        }

        if (mode == 11) {
            proof["display_power"]["sdk_hook_dex_sha256"] = std::string(64, '0');
        }

        if (mode == 12) {
            proof["mcu_property"]["apk_sha256"] = std::string(64, '0');
        }

        if (mode == 13) {
            proof["android"]["images"]["system.img"]["sha256"] = std::string(64, '0');
        }

        bind_profile(bad, changed, record);

        if (mode == 14) {
            record["ui_profile_sha256"] = std::string(64, '0');
        }

        bind_provenance(bad, record);

        if (mode == 15) {
            bad["resources"]["ui/cockpit-provenance.json"]["bytes"] = 1;
        }

        if (mode == 16) {
            bad["resources"]["ui/cockpit-provenance.json"]["sha256"] = std::string(64, '0');
        }

        if (mode == 17) {
            record["unknown"] = true;
            bind_provenance(bad, record);
        }

        bind_candidate(bad, proof);
        write_private_json(seed / "runtime.json", bad);
        bool denied = false;
        try {
            inspect_portable_runtime(root, profile, default_machine);
        } catch (...) {
            denied = true;
        }

        if (!denied) {
            throw std::runtime_error("unsafe wheel refresh accepted mode=" + std::to_string(mode));
        }
    }

    bind_profile(manifest, current, provenance);
    bind_provenance(manifest, provenance);
    bind_candidate(manifest, candidate);
    write_private_json(seed / "runtime.json", manifest);
    check(inspect_portable_runtime(root, profile, default_machine) == manifest &&
                  read_private_json_bytes(seed / "display-power/candidate.json") == evidence_before,
          "wheel regression changed genuine evidence");
}

void passion_system_upgrade_checks(const std::filesystem::path &stage,
                                   const std::filesystem::path &executable) {
    const std::string profile = "passion-11-russian-20260403-2.1.4.80";
    const auto default_machine = machine_identity(profile, "");
    const auto root = stage / "passion-system-upgrade", seed = root / "machines" / profile,
               data = seed / "android", scoped = root / ".local/machines" / profile;
    Json definition = {
            {"id", profile},
            {"title", "Synthetic Passion 1.0"},
            {"platform", "passion"},
            {"board", "8155"},
            {"status", "incomplete"},
            {"android",
             {{"package", "system-images;android-30;default;arm64-v8a"},
              {"revision", "1"},
              {"url",
               "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"},
              {"sha256", "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304"},
              {"clean_base", android_base(30).at("clean_base")}}},
            {"patches",
             {{"MegaNexusService", {{"steps", Json::array({"passion-api30-nexus-power.json"})}}},
              {"passion-api30-secondary-navigation",
               {{"steps", Json::array({"passion-api30-display-power.json"})}}},
              {"MonzaCarService",
               {{"input", "original:MonzaCarService"},
                {"type", "apk"},
                {"steps", Json::array({"framework-libraries.json", "passion-api30-diagnostics.json",
                                       "passion-api30-mcu-property.json"})}}}}}};

    write_private_json(seed / "profile.json", definition);
    const auto original_profile = sha256_file(seed / "profile.json");
    Json images = Json::object();
    std::map<std::filesystem::path, std::pair<Json, std::string>> retained;

    for (const auto *role :
         {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img", "cache.img"}) {
        const auto file = data / role;
        put(file, role);
        images[role] = {{"file", std::string("android/") + role},
                        {"sha256", sha256_file(file)},
                        {"bytes", std::filesystem::file_size(file)}};
        retained[file] = {cache_file_identity(file), sha256_file(file)};
    }

    for (const auto *role : {"userdata-qemu.img.qcow2", "cache.img.qcow2", "vendor.img.qcow2",
                             "encryptionkey.img.qcow2", "system.img.qcow2"}) {
        const auto file = data / role;
        put(file, role);
        retained[file] = {cache_file_identity(file), sha256_file(file)};
    }

    Json manifest = {{"schema", 1},
                     {"kind", "portable-guest-runtime"},
                     {"profile", profile},
                     {"profile_sha256", original_profile},
                     {"scope", "android"},
                     {"cluster_available", false},
                     {"candidate_receipt_sha256", std::string(64, '1')},
                     {"android",
                      {{"api", 30},
                       {"abi", "arm64-v8a"},
                       {"base_archive_sha256", definition.at("android").at("sha256")},
                       {"images", images}}}};
    put(seed / "observer/classes.dex", "accepted original observer");
    manifest["ui_observer"] = {{"file", "observer/classes.dex"},
                               {"bytes", std::filesystem::file_size(seed / "observer/classes.dex")},
                               {"sha256", sha256_file(seed / "observer/classes.dex")}};
    write_private_json(seed / "runtime.json", manifest);
    const auto before = read_private_json_bytes(seed / "runtime.json");
    definition["components"]["view-observation"] = "components/view-observation";
    definition["install"] = Json::array({{{"artifact", "component:view-observation"},
                                          {"type", "file"},
                                          {"target", "/system/framework/framework.jar"},
                                          {"mode", "0644"}}});
    const auto profile_bytes = definition.dump(2) + "\n";
    const auto source = root / ".local/candidate/system.img";
    put(source, "fresh cold verified system");
    auto fresh = images;
    fresh["system.img"] = {{"file", "android/system.img"},
                           {"sha256", sha256_file(source)},
                           {"bytes", std::filesystem::file_size(source)}};
    fresh["userdata-qemu.img"]["sha256"] = std::string(64, '9');
    Json candidate = {
            {"schema", 1},
            {"kind", "android-candidate"},
            {"profile", profile},
            {"profile_sha256", sha256_bytes(profile_bytes)},
            {"clean_base_archive_sha256", definition.at("android").at("sha256")},
            {"artifact_result_sha256", std::string(64, 'a')},
            {"prepared_VM", true},
            {"stopped", true},
            {"cold_boot_verified", true},
            {"old_machine_modified", false},
            {"android", {{"abi", "arm64-v8a"}, {"images", fresh}}},
            {"installed", Json::array({{{"artifact", "component:view-observation"},
                                        {"target", "/system/framework/framework.jar"},
                                        {"mode", "0644"},
                                        {"mutable", false}}})},
            {"checks", Json::array()},
            {"events", Json::array({{{"phase", "boot-art-compiled"},
                                     {"files", 18},
                                     {"command", api30_framework_boot::command(definition)}}})}};

    for (const auto &path : api30_framework_boot::paths(definition)) {
        candidate["installed"].push_back({{"artifact", "compiled-boot-image"}, {"target", path}});
    }

    for (const auto &pair : std::vector<std::pair<std::string, std::string>>{
                 {"boot", "1"}, {"api", "30"}, {"abi", "arm64-v8a"}, {"selinux", "Enforcing"}}) {
        candidate["checks"].push_back({{"id", pair.first},
                                       {"expected", pair.second},
                                       {"actual", pair.second},
                                       {"passed", true}});
    }

    Json pins = Json::object();
    const auto targets = passion_power_targets(), csv = passion_power_csv();

    for (auto it = targets.begin(); it != targets.end(); ++it) {
        const auto pin = csv.contains(it.key().substr(1))
                                 ? csv.at(it.key().substr(1)).get<std::string>()
                                 : std::string(64, 'e');
        pins[it.key()] = pin;
        candidate["installed"].push_back({{"artifact", it.value()},
                                          {"target", it.key()},
                                          {"sha256", pin},
                                          {"mutable", false}});
        candidate["checks"].push_back(
                {{"id", "artifact:" + it.value().get<std::string>() + ":" + it.key()},
                 {"expected", pin},
                 {"actual", pin},
                 {"passed", true}});
    }

    const std::string apk(64, 'b');
    candidate["installed"].push_back(
            {{"artifact", "patched:MonzaCarService"},
             {"target", "/system/priv-app/MonzaCarService/MonzaCarService.apk"},
             {"sha256", apk},
             {"mutable", false}});
    candidate["checks"].push_back(
            {{"id",
              "artifact:patched:MonzaCarService:/system/priv-app/MonzaCarService/MonzaCarService.apk"},
             {"expected", apk},
             {"actual", apk},
             {"passed", true}});
    Json helpers = Json::object();

    for (const auto &target :
         {std::string("/system/framework/services.jar"),
          std::string("/system/priv-app/MegaNexusService/MegaNexusService.apk")}) {
        const bool sdk = target == "/system/framework/services.jar";
        helpers[target] = {{"contract", "passion-display-power-helper-v1"},
                           {"artifact", passion_power_targets().at(target)},
                           {"archive_sha256", pins.at(target)},
                           {"entry", sdk ? "classes3.dex" : "classes2.dex"},
                           {"sha256", std::string(64, 'f')},
                           {"bytes", std::uint64_t{128}}};
    }

    candidate["display_power"] = passion_power_contract(definition, candidate, pins, helpers);
    candidate["mcu_property"] =
            passion_mcu_contract(definition, candidate, apk,
                                 {{"contract", "passion-mcu-property-helper-v1"},
                                  {"artifact", "patched:MonzaCarService"},
                                  {"archive_sha256", apk},
                                  {"entry", "classes18.dex"},
                                  {"sha256", std::string(64, 'f')},
                                  {"bytes", std::uint64_t{25}}});
    candidate["checks"].push_back(android_candidate_vehicle_observer_policy(definition));
    const auto sdk = root / ".local/toolchains/native-sdk";
#ifdef _WIN32
    const auto tool = sdk / "emulator/qemu-img.exe";
    _putenv_s("VOYEMU_SYSTEM_UPGRADE_FIXTURE", "1");
#else
    const auto tool = sdk / "emulator/qemu-img";
    setenv("VOYEMU_SYSTEM_UPGRADE_FIXTURE", "1", 1);
#endif
    ensure_private_directory(tool.parent_path());
    std::filesystem::copy_file(executable, tool);
    write_private_json(scoped / "android-launch.json",
                       {{"machine", data.string()}, {"android_sdk_root", sdk.string()}});
    const Json disk_tool = {{"path", tool.string()}, {"sha256", sha256_file(tool)}};
    const auto sha = sha256_file(source);
    const auto target = data / "system-versions" / sha;
    auto preserved = [&] {
        check(sha256_file(seed / "profile.json") == original_profile,
              "Passion accepted profile changed");

        for (const auto &[file, state] : retained) {
            check(cache_file_identity(file) == state.first && sha256_file(file) == state.second,
                  "Passion accepted disk changed");
        }
    };
    auto publish = [&](const Json &proof, const std::string &bytes, const std::string &expected) {
        return publish_machine_android_system(
                root, profile, default_machine, expected, source, sha256_file(source),
                std::filesystem::file_size(source), [] {}, disk_tool, Json(), 0,
                {{"profile_bytes", bytes}, {"candidate_bytes", proof.dump()}});
    };

    for (unsigned mode = 0; mode < 7; ++mode) {
        auto bad = candidate;
        auto bytes = profile_bytes;

        if (mode == 0) {
            bad["stopped"] = false;
        }

        if (mode == 1) {
            bad["checks"][0]["passed"] = false;
        }

        if (mode == 2) {
            bad["android"]["images"]["vendor.img"]["sha256"] = std::string(64, 'f');
        }

        if (mode == 3) {
            bytes += ' ';
        }

        if (mode == 4) {
            bad["events"][0]["files"] = 17;
        }

        if (mode == 5) {
            bad["mcu_property"]["apk_sha256"] = std::string(64, '0');
        }

        if (mode == 6) {
            bad["profile"] = "free-11-chinese-20260731-7.3.1";
        }

        bool denied = false;
        try {
            publish(bad, bytes, sha256_bytes(before));
        } catch (...) {
            denied = true;
        }

        check(denied && read_private_json_bytes(seed / "runtime.json") == before &&
                      !std::filesystem::exists(target),
              "unsafe Passion proof published");
        preserved();
    }

    bool denied = false;
    try {
        publish(candidate, profile_bytes, std::string(64, '0'));
    } catch (...) {
        denied = true;
    }

    check(denied, "stale Passion target CAS accepted");
    const auto result = publish(candidate, profile_bytes, sha256_bytes(before));
    check(result.at("applications_preserved") == true, "Passion publication did not preserve apps");
    preserved();
    const auto updated = read_private_json(seed / "runtime.json");
    check(updated.at("profile_sha256") == original_profile &&
                  updated.at("android").at("images").at("userdata-qemu.img") ==
                          images.at("userdata-qemu.img") &&
                  !updated.at("android").contains("owned_package"),
          "Passion identity or retained userdata changed");
    check(portable_profile_definition(root, profile, default_machine) == definition,
          "effective self-contained source snapshot lost");
    check(publish(candidate, profile_bytes, sha256_file(seed / "runtime.json")).at("cached") ==
                  true,
          "Passion cached publication rejected");
    auto another_epoch = candidate;
    another_epoch["events"].push_back({{"phase", "extra-source-epoch"}});
    denied = false;
    try {
        publish(another_epoch, profile_bytes, sha256_file(seed / "runtime.json"));
    } catch (...) {
        denied = true;
    }

    check(denied && read_private_json(seed / "runtime.json") == updated,
          "cached Passion accepted a different candidate epoch");
    put(source, "second genuine cold verified system");
    auto second = candidate;
    second["android"]["images"]["system.img"]["sha256"] = sha256_file(source);
    second["android"]["images"]["system.img"]["bytes"] = std::filesystem::file_size(source);
    const auto second_result = publish(second, profile_bytes, sha256_file(seed / "runtime.json"));
    check(second_result.at("previous_system_sha256") == sha &&
                  portable_profile_definition(root, profile, default_machine) == definition,
          "second Passion generation lineage failed");
    preserved();
    const auto latest = read_private_json(seed / "runtime.json");
    const auto parent_file = seed / latest.at("android")
                                            .at("system_upgrade")
                                            .at("parent_manifest")
                                            .at("file")
                                            .get<std::string>();
    check(read_private_json(parent_file) == updated,
          "second generation lost its exact parent manifest");
    // The observer is a separately published portable component, not a system/app-disk generation.
    const auto observer_dex = root / ".local/toolchains/ui-observer/new/classes.dex";
    put(observer_dex, "fresh source-built observer");
    const Json observer = {{"dex", observer_dex.string()}, {"sha256", sha256_file(observer_dex)}};
    const auto parent_before = read_private_json_bytes(parent_file);
    refresh_machine_ui_observer(
            root, profile, default_machine, seed,
            [&] {
                return observer;
            },
            false);
    check(portable_profile_definition(root, profile, default_machine) == definition,
          "observer-only refresh invalidated accepted system lineage");
    preserved();
    check(read_private_json_bytes(parent_file) == parent_before,
          "observer refresh rewrote pinned system parent");
    const auto observer_manifest = read_private_json(seed / "runtime.json");
    auto observer_rejected = [&](const Json &changed) {
        write_private_json(seed / "runtime.json", changed);
        bool rejected = false;
        try {
            portable_profile_definition(root, profile, default_machine);
        } catch (...) {
            rejected = true;
        }

        check(rejected,
              "observer lifecycle normalization accepted retained disk or component drift");
        write_private_json(seed / "runtime.json", observer_manifest);
    };

    for (const auto *field : {"profile_sha256", "candidate_receipt_sha256"}) {
        auto changed = observer_manifest;
        changed[field] = std::string(64, '4');
        observer_rejected(changed);
    }

    auto changed_disk = observer_manifest;
    changed_disk["android"]["images"]["userdata-qemu.img"]["sha256"] = std::string(64, '5');
    observer_rejected(changed_disk);
    auto removed_observer = observer_manifest;
    removed_observer.erase("ui_observer");
    observer_rejected(removed_observer);
    auto wrong_path = observer_manifest;
    wrong_path["ui_observer"]["file"] = "android/userdata-qemu.img";
    observer_rejected(wrong_path);
    auto wrong_pin = observer_manifest;
    wrong_pin["ui_observer"]["sha256"] = std::string(64, '6');
    observer_rejected(wrong_pin);
    auto wrong_size = observer_manifest;
    wrong_size["ui_observer"]["bytes"] = 0;
    observer_rejected(wrong_size);
    const auto embedded_observer = seed / "observer/classes.dex";
    put(embedded_observer, "tampered observer");
    bool bad_observer = false;
    try {
        portable_profile_definition(root, profile, default_machine);
    } catch (...) {
        bad_observer = true;
    }

    check(bad_observer, "unverified observer payload normalized into lineage");
    put(embedded_observer, "fresh source-built observer");
    preserved();
    const auto proof_file =
            seed /
            latest.at("android").at("system_upgrade").at("candidate").at("file").get<std::string>();
    put(proof_file, "tampered");
    denied = false;
    try {
        portable_profile_definition(root, profile, default_machine);
    } catch (...) {
        denied = true;
    }

    check(denied, "tampered Passion generation proof accepted");
#ifdef _WIN32
    _putenv_s("VOYEMU_SYSTEM_UPGRADE_FIXTURE", "");
#else
    unsetenv("VOYEMU_SYSTEM_UPGRADE_FIXTURE");
#endif
}
} // namespace

// Uses the pinned API30 boot files supplied by the test runner; never mutates them.
void qinggan11_development_activation_checks(const std::filesystem::path &root,
                                             const std::filesystem::path &clean,
                                             const std::filesystem::path &executable) {
    const std::string profile = "free-11-chinese-20250710-6.11.1",
                      archive = "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304";
    const auto default_machine = machine_identity(profile, "");

    check(std::filesystem::is_directory(root) && std::filesystem::is_empty(root),
          "empty owned scratch required");
    ensure_private_directory(root);
    const auto seed = root / "machines" / profile, boot = seed / "android",
               sdk = root / ".local/toolchains/native-sdk";
    ensure_private_directory(boot);

    for (const auto &[name, pin] : portable_clean_boot_pins(android_base(30))) {
        check(sha256_file(clean / name) == pin, "API30 clean input pin differs");
        std::filesystem::copy_file(clean / name, boot / name);
    }

    const auto kernel = boot / "kernel-ranchu";
    put(kernel, "owned Free A11 6.11.1 development kernel fixture");
    const auto dev_sha = sha256_file(kernel);
    Json dev = {{"url", "https://github.com/voyemu/voyemu-deps/releases/download/kernel-fixture-" +
                                dev_sha.substr(0, 8) + "/Image"},
                {"sha256", dev_sha},
                {"abi", "arm64-v8a"},
                {"provenance",
                 {{"source_revision", "fixture"},
                  {"source_content_sha256", std::string(64, '1')},
                  {"embedded_config_sha256", std::string(64, '2')},
                  {"normalized_config_sha256", std::string(64, '3')},
                  {"toolchain", "fixture"},
                  {"patches", Json::array({std::string(64, '4')})},
                  {"configuration_deviations", Json::array()}}}};
    Json def = {{"schema", 1},
                {"id", profile},
                {"title", "Synthetic Free 1.0"},
                {"android",
                 {{"package", "system-images;android-30;default;arm64-v8a"},
                  {"revision", "1"},
                  {"url",
                   "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"},
                  {"sha256", archive},
                  {"clean_base", android_base(30).at("clean_base")},
                  {"development_kernel", dev}}},
                {"emulator", {{"port", 5574}, {"memory", 4096}, {"cores", 2}, {"gpuMode", "auto"}}},
                {"displays", Json::array({{{"role", "primary"},
                                           {"guest", "android"},
                                           {"width", 1920},
                                           {"height", 720},
                                           {"density", 160}},
                                          {{"role", "secondary"},
                                           {"guest", "android"},
                                           {"width", 1920},
                                           {"height", 720},
                                           {"density", 160},
                                           {"flag", 0},
                                           {"xOffset", 1920},
                                           {"yOffset", 0}}})},
                {"configure",
                 {{"home", "com.qinggan.app.launcher"},
                  {"homeActivity",
                   "com.qinggan.app.launcher/com.qinggan.app.launcher.activity.MainActivity"}}}};
    Json images = Json::object();

    for (const auto *name :
         {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img"}) {
        put(boot / name, std::string("fixture ") + name);
        images[name] = {{"file", std::string("android/") + name},
                        {"bytes", std::filesystem::file_size(boot / name)},
                        {"sha256", sha256_file(boot / name)}};
    }

    cache(boot / "cache.img");
    images["cache.img"] = {{"file", "android/cache.img"},
                           {"bytes", 69206016},
                           {"sha256", sha256_file(boot / "cache.img")}};
    Json files = Json::object();

    for (const auto &[name, pin] : portable_clean_boot_pins(android_base(30))) {
        files[name] = {{"file", "android/" + name},
                       {"sha256", sha256_file(boot / name)},
                       {"bytes", std::filesystem::file_size(boot / name)}};
    }

    Json manifest = {{"schema", 1},
                     {"kind", "portable-guest-runtime"},
                     {"profile", profile},
                     {"profile_sha256", ""},
                     {"android",
                      {{"api", 30},
                       {"abi", "arm64-v8a"},
                       {"base_archive_sha256", archive},
                       {"images", images},
                       {"development_kernel", dev},
                       {"boot_files", files}}}};
    manifest["resources"] = resource_fixture(seed);
    const auto publish = [&](const Json &d, Json m) {
        write_private_json(seed / "profile.json", d);
        m["profile_sha256"] = sha256_file(seed / "profile.json");
        write_private_json(seed / "runtime.json", m);

        return m;
    };
    manifest = publish(def, manifest);
    const std::string suffix(os::executable_suffix), backend(os::emulator_host);
    PortableRuntimeHostBindings host;
    host.sdk = sdk;
    host.clean_base = boot;
    host.require_cluster = false;

    for (const auto &f : {sdk / "emulator" / ("emulator" + suffix),
                          sdk / "emulator/qemu" / backend / ("qemu-system-aarch64" + suffix)}) {
        put(f, "host fixture");
        host.pins[f.string()] = sha256_file(f);
    }

    const auto disk = sdk / "emulator" / ("qemu-img" + suffix);
    ensure_private_directory(disk.parent_path());
    std::filesystem::copy_file(executable, disk);
    host.pins[disk.string()] = sha256_file(disk);
    auto reject = [&](const char *why, const std::string &expected) {
        std::string error;
        try {
            (void)activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (const std::exception &e) {
            error = e.what();
        }

        check(error.find(expected) != std::string::npos, why);
        check(!std::filesystem::exists(root / ".local/machines" / profile / "activation.json"),
              "rejected activation published state");
    };
    auto bad = manifest;
    bad["android"]["development_kernel"]["provenance"]["toolchain"] = "foreign";
    write_private_json(seed / "runtime.json", bad);
    reject("different development descriptor accepted", "development kernel manifest differs");
    write_private_json(seed / "runtime.json", manifest);
    bad = manifest;
    bad["android"].erase("development_kernel");
    write_private_json(seed / "runtime.json", bad);
    reject("missing development descriptor accepted", "development kernel declaration differs");
    write_private_json(seed / "runtime.json", manifest);
    bad = manifest;
    bad["android"].erase("boot_files");
    write_private_json(seed / "runtime.json", bad);
    reject("missing development boot files accepted", "development kernel manifest differs");
    write_private_json(seed / "runtime.json", manifest);
    {
        auto duplicate = manifest.dump();
        const std::string key = "\"development_kernel\":";
        const auto at = duplicate.find(key);
        check(at != std::string::npos, "development fixture key missing");
        duplicate.insert(at, key + "null,");
        put(seed / "runtime.json", duplicate);
        reject("duplicate development key accepted", "duplicate development profile key");
        write_private_json(seed / "runtime.json", manifest);
    }
    put(kernel, "wrong kernel");
    reject("different development kernel accepted", "input pin differs");
    put(kernel, "owned Free A11 6.11.1 development kernel fixture");

    for (const auto &[name, pin] : portable_clean_boot_pins(android_base(30))) {
        if (name == "kernel-ranchu") {
            continue;
        }

        const auto file = boot / name, held = boot / (name + ".held");
        std::filesystem::rename(file, held);
        put(file, "foreign boot bytes");
        reject("changed stock boot dependency accepted", "input pin differs");
        std::filesystem::remove(file);
        std::filesystem::rename(held, file);
    }

#ifndef _WIN32
    {
        const auto held = sdk.parent_path() / "native-sdk-held";
        std::filesystem::rename(sdk, held);
        std::filesystem::create_directory_symlink(held, sdk);
        reject("linked SDK parent accepted", "canonical path required: " + sdk.string());
        std::filesystem::remove(sdk);
        std::filesystem::rename(held, sdk);
    }
    {
        const auto held = boot / "kernel.held";
        std::filesystem::rename(kernel, held);
        std::filesystem::create_symlink(held, kernel);
        reject("development symlink accepted", "canonical path required: " + kernel.string());
        std::filesystem::remove(kernel);
        std::filesystem::create_hard_link(held, kernel);
        reject("development hardlink accepted", "bounded unaliased ordinary file required");
        std::filesystem::remove(kernel);
        std::filesystem::rename(held, kernel);
    }
#endif
    const auto original_kernel = cache_file_identity(kernel);
    const auto original_seed = sha256_file(seed / "runtime.json");
#ifdef _WIN32
    _putenv_s("VOYEMU_SYSTEM_UPGRADE_FIXTURE", "1");
#else
    setenv("VOYEMU_SYSTEM_UPGRADE_FIXTURE", "1", 1);
#endif
    host.seed = seed;
    const auto external = root / "external-clean";
    ensure_private_directory(external);

    for (const auto &[name, pin] : portable_clean_boot_pins(android_base(30))) {
        std::filesystem::copy_file(clean / name, external / name);
    }

    host.clean_base = external;
    const auto external_activation = activate_portable_runtime(
            root, profile, machine_identity(profile, "external-stock-base"), host, 0);
    const auto external_kernel =
            std::filesystem::path(external_activation.at("runtime").get<std::string>()) /
            "android-machine/base/kernel-ranchu";
    check(sha256_file(external_kernel) == dev_sha,
          "external stock base did not select development kernel");
    std::filesystem::remove(external / "kernel-ranchu");
    std::filesystem::copy_file(kernel, external / "kernel-ranchu");
    bool external_denied = false;
    try {
        (void)activate_portable_runtime(root, profile,
                                        machine_identity(profile, "foreign-dev-base"), host, 0);
    } catch (const std::exception &e) {
        external_denied = std::string(e.what()).find("input pin differs") != std::string::npos;
    }

    check(external_denied && !std::filesystem::exists(
                                     root / ".local/machines/foreign-dev-base/activation.json"),
          "external development base accepted as stock");
    host.clean_base = boot;
    host.seed.clear();
    const auto activated = activate_portable_runtime(root, profile, default_machine, host, 0);
    const auto runtime = std::filesystem::path(activated.at("runtime").get<std::string>()),
               copied = runtime / "android-machine/base/kernel-ranchu";
    check(sha256_file(copied) == dev_sha && cache_file_identity(copied) != original_kernel,
          "selected kernel not independently copied");
    const auto launch =
            read_private_json(root / ".local/machines" / profile / "android-launch.json");
    unsigned pins = 0;

    for (const auto &item : launch.at("pins")) {
        if (item.at("path") == copied.string() && item.at("sha256") == dev_sha) {
            ++pins;
        }
    }

    check(pins == 1, "selected kernel launch pin differs");
    check(cache_file_identity(kernel) == original_kernel &&
                  sha256_file(seed / "runtime.json") == original_seed,
          "activation changed seed");
    // A nearby Free profile cannot use the 611-only exception for its embedded clean base.
    auto foreign_def = def;
    const std::string foreign_profile = "free-11-chinese-20260731-7.3.1";
    foreign_def["id"] = foreign_profile;
    auto foreign_manifest = manifest;
    foreign_manifest["profile"] = foreign_profile;
    (void)publish(foreign_def, foreign_manifest);
    host.seed = seed;
    bool foreign_denied = false;
    std::string foreign_reason;
    try {
        (void)activate_portable_runtime(root, foreign_profile,
                                        machine_identity(foreign_profile, "foreign-free"), host, 0);
    } catch (const std::exception &e) {
        foreign_reason = e.what();
        foreign_denied = foreign_reason.find("input pin differs") != std::string::npos;
    }

    check(foreign_denied &&
                  !std::filesystem::exists(root / ".local/machines/foreign-free/activation.json"),
          "foreign profile accepted development kernel as stock clean base");
    (void)publish(def, manifest);
    host.seed.clear();
    // With no development declaration the stock API30 kernel remains required.
    auto stock_def = def;
    stock_def["android"].erase("development_kernel");
    auto stock_manifest = manifest;
    stock_manifest["android"].erase("development_kernel");
    stock_manifest["android"].erase("boot_files");
    std::filesystem::remove(kernel);
    std::filesystem::copy_file(clean / "kernel-ranchu", kernel);
    (void)publish(stock_def, stock_manifest);
    host.seed = seed;
    const auto stock_activation = activate_portable_runtime(
            root, profile, machine_identity(profile, "stock-611"), host, 0);
    const auto stock_copy =
            std::filesystem::path(stock_activation.at("runtime").get<std::string>()) /
            "android-machine/base/kernel-ranchu";
    check(sha256_file(stock_copy) == portable_clean_boot_pins(android_base(30)).at("kernel-ranchu"),
          "stock profile did not use stock kernel");
    put(kernel, "owned Free A11 6.11.1 development kernel fixture");
    bool stock_denied = false;
    try {
        (void)activate_portable_runtime(
                root, profile, machine_identity(profile, "stock-with-dev-kernel"), host, 0);
    } catch (const std::exception &e) {
        stock_denied = std::string(e.what()).find("input pin differs") != std::string::npos;
    }

    check(stock_denied && !std::filesystem::exists(
                                  root / ".local/machines/stock-with-dev-kernel/activation.json"),
          "stock profile accepted development kernel");
#ifdef _WIN32
    _putenv_s("VOYEMU_SYSTEM_UPGRADE_FIXTURE", "");
#else
    unsetenv("VOYEMU_SYSTEM_UPGRADE_FIXTURE");
#endif
}

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--passion-wheel-refresh-only") {
            const auto scratch = std::filesystem::canonical(std::filesystem::path(__FILE__))
                                         .parent_path()
                                         .parent_path()
                                         .parent_path()
                                         .parent_path() /
                                 ".local/build/tmp" /
                                 ("passion-wheel-regression-" + random_token());
            ensure_private_directory(scratch);
            struct Cleanup {
                std::filesystem::path path;

                ~Cleanup() {
                    std::error_code error;
                    std::filesystem::remove_all(path, error);
                }
            } cleanup{scratch};
            passion_wheel_refresh_checks(scratch);
            std::cout << "Passion host-only wheel provenance and power/MCU cold binding passed\n";

            return 0;
        }

        if (argc == 2 && std::string_view(argv[1]) == "--passion-system-upgrade-only") {
            const auto scratch = std::filesystem::canonical(std::filesystem::path(__FILE__))
                                         .parent_path()
                                         .parent_path()
                                         .parent_path()
                                         .parent_path() /
                                 ".local/build/tmp" /
                                 ("passion-system-regression-" + random_token());
            ensure_private_directory(scratch);
            struct Cleanup {
                std::filesystem::path path;

                ~Cleanup() {
                    std::error_code error;
                    std::filesystem::remove_all(path, error);
                }
            } cleanup{scratch};
            passion_system_upgrade_checks(scratch, std::filesystem::canonical(argv[0]));
            std::cout
                    << "Passion stopped cold lineage, CAS and preserved application disks passed\n";

            return 0;
        }

        if (argc == 2 && std::string_view(argv[1]) == "--qnx-upgrade-only") {
            const auto project = std::filesystem::canonical(std::filesystem::path(__FILE__))
                                         .parent_path()
                                         .parent_path()
                                         .parent_path()
                                         .parent_path();
            const auto scratch = project / ".local/build/tmp" /
                                 ("qnx-upgrade-regression-" + random_token());
            ensure_private_directory(scratch);
            struct Cleanup {
                std::filesystem::path path;

                ~Cleanup() {
                    std::error_code error;
                    std::filesystem::remove_all(path, error);
                }
            } cleanup{scratch};
            qnx_upgrade_checks(scratch);
            qnx_upgrade_checks(scratch, true);
            qnx_persistent_upgrade_checks(scratch);
            qnx_persistent_upgrade_checks(scratch, true);
            std::cout << "Free A11 QNX publication and rollback regressions passed\n";

            return 0;
        }

        {
            const Json binding = {{"schema", 1},
                                  {"launcher", "Z:/private/launcher"},
                                  {"qnx",
                                   {{"executable", "Z:/private/qemu-system-aarch64"},
                                    {"sha256", std::string(64, 'a')}}}};
            const auto windows_ram = [](unsigned memory_mib) {
                const auto ram = qnx_ram_launch(QnxEngine::tcg_file, memory_mib);
                std::vector<std::string> args{
                        "-machine", ram.machine, "-accel", ram.accelerator,
                        "-cpu",     "max",       "-m",     std::to_string(memory_mib)};
                args.insert(args.end(), ram.options.begin(), ram.options.end());

                return args;
            };
            const Json launch = {
                    {"qemu", {{"executable", "C:/qemu.exe"}, {"args", windows_ram(8192)}}},
                    {"qmp", {{"host", "127.0.0.1"}, {"port", 45997}}},
                    {"ram_backend", "file"},
                    {"pins", Json::array()}};
            check(portable_qnx_diagnostic_launch(launch, Json::object()) == launch,
                  "ordinary QNX launch must remain unchanged");
            const auto native = portable_qnx_diagnostic_launch(launch, binding);
            check(native.at("ram_backend") == "file" &&
                          native.at("qemu").at("executable") == binding.at("qnx").at("executable"),
                  "native diagnostic must select pinned Mac engine and file RAM");
            check(portable_qnx_diagnostic_current(native, binding),
                  "current native diagnostic rejected");
            check(!portable_qnx_diagnostic_current(native, Json::object()),
                  "diagnostic launch reused without explicit binding");
            const auto args = native.at("qemu").at("args").get<std::vector<std::string>>();
            check(std::find(args.begin(), args.end(), "-icount") == args.end() &&
                          std::find(args.begin(), args.end(), "hvf,kernel-irqchip=on") !=
                                  args.end() &&
                          std::count(args.begin(), args.end(), "-object") == 1,
                  "native diagnostic retained TCG clock or accelerator or duplicated RAM");
            auto passion_launch = launch;
            passion_launch["hardware_adapter"] = "sa8155-passion-21480";
            passion_launch["qemu"]["args"] = windows_ram(11713);
            const auto passion_native = portable_qnx_diagnostic_launch(passion_launch, binding);
            const auto passion_args =
                    passion_native.at("qemu").at("args").get<std::vector<std::string>>();
            check(std::count(
                          passion_args.begin(), passion_args.end(),
                          "memory-backend-file,id=qnxmem,size=11713M,mem-path=${SESSION}/qnx-ram.bin,share=on") ==
                                  1 &&
                          portable_qnx_diagnostic_current(passion_native, binding),
                  "Passion native diagnostic must preserve its 11713MiB memory extent");
            auto tampered = native;
            tampered["pins"][0]["sha256"] = std::string(64, 'b');
            check(!portable_qnx_diagnostic_current(tampered, binding),
                  "changed native engine pin accepted");
            auto reject = [&](Json candidate, Json capability) {
                bool denied = false;
                try {
                    portable_qnx_diagnostic_launch(candidate, capability);
                } catch (...) {
                    denied = true;
                }

                check(denied, "invalid native diagnostic accepted");
            };
            auto wrong_passion = passion_launch;
            wrong_passion["qemu"]["args"] = windows_ram(8192);
            reject(wrong_passion, binding);
            wrong_passion = passion_launch;
            wrong_passion["qemu"]["args"].push_back("-m");
            wrong_passion["qemu"]["args"].push_back("11713");
            reject(wrong_passion, binding);
            auto bad = launch;
            bad["persistent_storage"] = Json::object();
            reject(bad, binding);
            bad = launch;
            bad["ram_backend"] = "process";
            reject(bad, binding);
            bad = launch;
            bad["qemu"]["args"].erase(bad["qemu"]["args"].end() - 4, bad["qemu"]["args"].end() - 2);
            reject(bad, binding);
            bad = launch;
            bad["qmp"]["host"] = "0.0.0.0";
            reject(bad, binding);
            auto bad_binding = binding;
            bad_binding["qnx"]["executable"] = "C:/qemu.exe";
            reject(launch, bad_binding);
            auto persistent = launch;
            persistent["ram_backend"] = "file";
            persistent["persistent_storage"] = {{"schema", 1},
                                                {"system", "/machines/car/qnx/system.img"},
                                                {"userdata", "/machines/car/qnx/userdata.img"}};
            auto persistent_args = std::vector<std::string>{
                    "-machine", "virt-11.0", "-accel", "tcg,thread=single", "-cpu", "max"};
            const auto mapped = qnx_persistent_ram_launch(QnxEngine::tcg_file,
                                                          "/machines/car/qnx/system.img",
                                                          "/machines/car/qnx/userdata.img", true);
            persistent_args.insert(persistent_args.end(), mapped.options.begin(),
                                   mapped.options.end());
            persistent["qemu"]["args"] = persistent_args;
            const auto translated = portable_qnx_diagnostic_launch(persistent, binding);
            const auto translated_args =
                    translated.at("qemu").at("args").get<std::vector<std::string>>();
            check(translated.at("persistent_storage") == persistent.at("persistent_storage") &&
                          translated.at("diagnostic_native").at("persistent_backend") ==
                                  "macos-file-numa-v1" &&
                          translated.at("diagnostic_native").at("engine_sha256") ==
                                  binding.at("qnx").at("sha256"),
                  "persistent native translation changed disks or omitted explicit engine capability");
            check(std::count(translated_args.begin(), translated_args.end(), "-object") == 5 &&
                          std::count(translated_args.begin(), translated_args.end(), "-numa") ==
                                  5 &&
                          std::find(translated_args.begin(), translated_args.end(), "-icount") ==
                                  translated_args.end(),
                  "persistent native translation duplicated RAM or retained TCG clock");
            check(portable_qnx_diagnostic_current(translated, binding),
                  "current persistent native diagnostic rejected");
            auto stale = translated;
            stale["diagnostic_native"]["engine_sha256"] = std::string(64, 'b');
            check(!portable_qnx_diagnostic_current(stale, binding),
                  "stale persistent native engine capability accepted");
            auto wrong = persistent;
            wrong["qemu"]["args"][9] =
                    "memory-backend-file,id=user,size=32M,mem-path=/foreign,share=on";
            reject(wrong, binding);
            wrong = persistent;
            wrong["qemu"]["args"].push_back("-object");
            wrong["qemu"]["args"].push_back(
                    "memory-backend-file,id=foreign,size=1M,mem-path=/foreign");
            reject(wrong, binding);
        }
        {
            const std::string sha(64, 'a');
            Json descriptor = {{"file", "android/system.img"}, {"sha256", sha}, {"bytes", 4096}};
            check(portable_system_image_file(descriptor, 28) == "android/system.img",
                  "legacy system role changed");
            descriptor["file"] = "android/system-versions/" + sha + "/system.img";
            check(portable_system_image_file(descriptor, 30) ==
                          descriptor.at("file").get<std::string>(),
                  "versioned system role rejected");
            auto rejected = [&](Json value, unsigned api) {
                bool failed = false;
                try {
                    (void)portable_system_image_file(value, api);
                } catch (...) {
                    failed = true;
                }

                check(failed, "unsafe system generation accepted");
            };
            rejected(descriptor, 28);
            auto wrong = descriptor;
            wrong["sha256"] = std::string(64, 'b');
            rejected(wrong, 30);
            wrong = descriptor;
            wrong["file"] = "android/system-versions/../system.img";
            rejected(wrong, 30);
            wrong = descriptor;
            wrong["file"] = "/android/system-versions/" + sha + "/system.img";
            rejected(wrong, 30);
        }
        {
            const std::string qinggan11_731 = "free-11-chinese-20260731-7.3.1";
            Json binding = {{"dex", "/owned/classes.dex"}, {"sha256", std::string(64, 'a')}};
            check(portable_ui_observer_binding(binding, "") &&
                          portable_ui_observer_binding(binding, qinggan11_731),
                  "legacy observer binding rejected");
            binding["profile"] = "passion-11-russian-20260403-2.1.4.80";
            check(portable_ui_observer_binding(binding, "passion-11-russian-20260403-2.1.4.80"),
                  "exact Passion observer binding rejected");
            binding["profile"] = "free-11-chinese-20250710-6.11.1";
            check(portable_ui_observer_binding(binding, "free-11-chinese-20250710-6.11.1"),
                  "exact Free A11 6.11.1 observer binding rejected");
            check(!portable_ui_observer_binding(binding, "free-11-chinese-20250710-6.11.1x"),
                  "nearby Free A11 6.11.1 observer profile accepted");
            binding["profile"] = qinggan11_731;
            check(portable_ui_observer_binding(binding, qinggan11_731),
                  "exact Free A11 7.3.1 observer binding rejected");
            check(!portable_ui_observer_binding(binding, "free-09-chinese-20260106-6.0.3") &&
                          !portable_ui_observer_binding(binding,
                                                        "passion-11-russian-20260403-2.1.4.80"),
                  "foreign observer profile accepted");
            auto invalid = binding;
            invalid["extra"] = true;
            check(!portable_ui_observer_binding(invalid, qinggan11_731),
                  "open observer schema accepted");
            invalid = binding;
            invalid["profile"] = qinggan11_731 + "x";
            check(!portable_ui_observer_binding(invalid, qinggan11_731),
                  "nearby observer profile accepted");
            invalid = binding;
            invalid["sha256"] = 3;
            check(!portable_ui_observer_binding(invalid, qinggan11_731),
                  "invalid observer pin type accepted");
        }

        const auto invoked = std::filesystem::path(argv[0]).filename().string();

        if (invoked == "mkfs.ext4" || invoked == "mkfs.ext4.exe") {
            return 0;
        }

        if (invoked == "mke2fs.exe") {
            check(argc == 6 && std::string(argv[1]) == "-t" && std::string(argv[2]) == "ext4" &&
                          std::string(argv[3]) == "-F" && std::string(argv[4]) == "-q",
                  "Windows formatter arguments differ");
            const auto config = std::getenv("MKE2FS_CONFIG");
            check(config && std::filesystem::is_regular_file(config),
                  "Windows formatter config missing");

            return 0;
        }

        if (invoked == "qemu-img" || invoked == "qemu-img.exe") {
            check(argc > 2, "fixture disk tool arguments missing");
            const std::string operation = argv[1];

            if (operation == "create") {
                if (const auto replace = std::getenv("VOYEMU_BINDER_FIXTURE_REPLACE")) {
                    put(replace, "{\"foreign\":true}\n");
                }

                std::filesystem::path output = argv[argc - 1];
                const std::string last = argv[argc - 1];

                if (last.find_first_not_of("0123456789") == std::string::npos) {
                    output = argv[argc - 2];
                }

                if (std::getenv("VOYEMU_BINDER_FIXTURE_FAIL") &&
                    output.filename() == "userdata-qemu.img.qcow2") {
                    return 9;
                }

                if (std::getenv("VOYEMU_SYSTEM_UPGRADE_FIXTURE")) {
                    std::string bytes(34, '\0');
                    bytes[0] = 'Q';
                    bytes[1] = 'F';
                    bytes[2] = 'I';
                    bytes[3] = static_cast<char>(0xfb);
                    bytes[15] = 24;
                    bytes[19] = 10;
                    bytes.replace(24, 10, "system.img");
                    put(std::filesystem::absolute(output), bytes);
                } else {
                    put(output, "synthetic-small-disk-tool-output");
                }

                return 0;
            }

            if (operation == "rebase") {
                std::ofstream output(argv[argc - 1], std::ios::binary | std::ios::app);
                output << "\nrelative-backing=" << argv[argc - 2] << '\n';

                return output ? 0 : 10;
            }

            return 11;
        }

        {
            const std::array<HostRefreshRecord, 3> records{
                    {{"launch", "{\n \"old\": 1\n}\n", "{\"new\":1}\n"},
                     {"activation", "{\n \"old\": 2\n}\n", "{\"new\":2}\n"},
                     {"portable-host", "{\n \"old\": 3\n}\n", "{\"new\":3}\n"}}};

            for (const unsigned failure : {1u, 2u, 3u}) {
                std::map<std::filesystem::path, std::string> values;

                for (const auto &record : records) {
                    values[record.path] = record.before;
                }

                unsigned publications = 0;
                bool thrown = false;
                try {
                    publish_host_refresh(
                            records,
                            [&](const auto &path) {
                                return values.at(path);
                            },
                            [&](const auto &path, const std::string &value) {
                                values[path] = value;

                                if (value.find("new") != std::string::npos &&
                                    ++publications == failure) {
                                    throw std::runtime_error("injected post-publication failure");
                                }
                            },
                            [] {});
                } catch (...) {
                    thrown = true;
                }

                check(thrown, "publication failure injection missing");

                for (const auto &record : records) {
                    check(values.at(record.path) == record.before,
                          "host refresh failed to roll back all original records");
                }
            }

            std::map<std::filesystem::path, std::string> raced;

            for (const auto &record : records) {
                raced[record.path] = record.before;
            }

            bool refused = false;
            try {
                publish_host_refresh(
                        records,
                        [&](const auto &path) {
                            return raced.at(path);
                        },
                        [&](const auto &path, const std::string &value) {
                            raced[path] = value;

                            if (path == records[0].path && value == records[0].after) {
                                raced[records[1].path] = "{\"foreign\":true}\n";
                            }
                        },
                        [] {});
            } catch (const std::exception &error) {
                refused = std::string(error.what()).starts_with("HOST_REFRESH_ROLLBACK:");
            }

            check(refused && raced.at(records[0].path) == records[0].before &&
                          raced.at(records[1].path) == "{\"foreign\":true}\n" &&
                          raced.at(records[2].path) == records[2].before,
                  "publication race overwrote foreign record or retained our partial write");

            for (unsigned foreign = 0; foreign < records.size(); ++foreign) {
                std::map<std::filesystem::path, std::string> values;

                for (const auto &record : records) {
                    values[record.path] = record.before;
                }

                values[records[foreign].path] = "foreign metadata";
                bool retained = false, restored = false, denied = false;
                try {
                    rollback_host_refresh_payload(
                            records,
                            [&](const auto &path) {
                                return values.at(path);
                            },
                            [&] {
                                restored = true;
                            },
                            retained);
                } catch (...) {
                    denied = true;
                }

                check(denied && retained && !restored,
                      "incomplete metadata rollback released recovery or moved payload");
            }

            for (const auto failure :
                 {"foreign payload", "missing payload", "destination collision"}) {
                std::map<std::filesystem::path, std::string> values;

                for (const auto &record : records) {
                    values[record.path] = record.before;
                }

                bool retained = false, denied = false;
                try {
                    rollback_host_refresh_payload(
                            records,
                            [&](const auto &path) {
                                return values.at(path);
                            },
                            [&] {
                                throw std::runtime_error(failure);
                            },
                            retained);
                } catch (...) {
                    denied = true;
                }

                check(denied && retained, "failed payload rollback released recovery");
            }

            {
                bool retained = true, restored = false;
                std::map<std::filesystem::path, std::string> values;

                for (const auto &record : records) {
                    values[record.path] = record.before;
                }

                rollback_host_refresh_payload(
                        records,
                        [&](const auto &path) {
                            return values.at(path);
                        },
                        [&] {
                            restored = true;
                        },
                        retained);
                check(restored && !retained, "complete rollback retained processed recovery");
            }
            const std::array<HostRefreshRecord, 4> full_records{
                    {records[0],
                     {"cluster", "old cluster\n", "new cluster\n"},
                     records[1],
                     records[2]}};

            for (unsigned failure = 1; failure <= 4; ++failure) {
                std::map<std::filesystem::path, std::string> values;

                for (const auto &record : full_records) {
                    values[record.path] = record.before;
                }

                unsigned publications = 0;
                bool thrown = false;
                try {
                    publish_host_refresh(
                            full_records,
                            [&](const auto &path) {
                                return values.at(path);
                            },
                            [&](const auto &path, const std::string &value) {
                                values[path] = value;

                                if (value.find("new") != std::string::npos &&
                                    ++publications == failure) {
                                    throw std::runtime_error(
                                            "injected full post-publication failure");
                                }
                            },
                            [] {});
                } catch (...) {
                    thrown = true;
                }

                check(thrown, "full publication fault was not injected");

                for (const auto &record : full_records) {
                    check(values.at(record.path) == record.before,
                          "full host refresh failed record rollback");
                }
            }

            std::map<std::filesystem::path, std::string> full_race;

            for (const auto &record : full_records) {
                full_race[record.path] = record.before;
            }

            refused = false;
            try {
                publish_host_refresh(
                        full_records,
                        [&](const auto &path) {
                            return full_race.at(path);
                        },
                        [&](const auto &path, const auto &value) {
                            full_race[path] = value;

                            if (path == full_records[0].path && value == full_records[0].after) {
                                full_race[full_records[1].path] = "foreign cluster\n";
                            }
                        },
                        [] {});
            } catch (...) {
                refused = true;
            }

            check(refused && full_race.at("launch") == records[0].before &&
                          full_race.at("cluster") == "foreign cluster\n" &&
                          full_race.at("activation") == records[1].before &&
                          full_race.at("portable-host") == records[2].before,
                  "full CAS overwrote foreign cluster or left partial writes");
        }

        if (argc == 4 && std::string_view(argv[1]) == "--qinggan11-611-development-activation") {
            qinggan11_development_activation_checks(std::filesystem::canonical(argv[2]),
                                                    std::filesystem::canonical(argv[3]),
                                                    std::filesystem::canonical(argv[0]));

            return 0;
        }

        if (argc == 7 && std::string(argv[1]) == "--activate") {
            PortableRuntimeHostBindings host;
            host.sdk = std::filesystem::canonical(argv[4]);
            host.clean_base = std::filesystem::canonical(argv[5]);
            const auto pins = read_private_json(std::filesystem::canonical(argv[6]),
                                                8 * 1024 * 1024);

            for (auto at = pins.begin(); at != pins.end(); ++at) {
                host.pins.emplace(at.key(), at.value().get<std::string>());
            }

            std::cout << activate_portable_runtime(std::filesystem::canonical(argv[2]), argv[3],
                                                   machine_identity(argv[3], ""), host, 0)
                                 .dump()
                      << '\n';

            return 0;
        }

        const bool cache_policy = argc == 3 && std::string_view(argv[1]) == "--cache-policy";
        check(argc == 1 ||
                      (argc == 3 && (std::string_view(argv[1]) == "--scratch" || cache_policy)),
              "usage: --scratch OWNED_DIRECTORY, --cache-policy OWNED_DIRECTORY or --activate ROOT PROFILE SDK CLEAN_BASE ABSOLUTE_HOST_PINS.json");
        {
            const auto sha = std::string(64, 'a');
            Json definition = {
                    {"title", "Synthetic Kernel 1.0"},
                    {"android",
                     {{"development_kernel",
                       {{"url",
                         "https://github.com/voyemu/voyemu-deps/releases/download/kernel-fixture-" +
                                 sha.substr(0, 8) + "/Image"},
                        {"sha256", sha},
                        {"abi", "arm64-v8a"},
                        {"provenance",
                         {{"source_revision", "fixture"},
                          {"source_content_sha256", std::string(64, '1')},
                          {"embedded_config_sha256", std::string(64, '2')},
                          {"normalized_config_sha256", std::string(64, '3')},
                          {"toolchain", "fixture"},
                          {"patches", Json::array({std::string(64, '4')})},
                          {"configuration_deviations", Json::array()}}}}}}}};
            check(portable_development_kernel(definition) ==
                          definition.at("android").at("development_kernel"),
                  "development descriptor rejected");

            for (const auto &key : {"sha256", "url", "abi", "provenance"}) {
                auto bad = definition;
                bad["android"]["development_kernel"][key] = "invalid";
                bool denied = false;
                try {
                    (void)portable_development_kernel(bad);
                } catch (...) {
                    denied = true;
                }

                check(denied, "invalid development descriptor accepted");
            }

            auto legacy = definition;
            auto &legacy_kernel = legacy["android"]["development_kernel"];
            legacy_kernel["url"] = "https://github.com/voyemu/voyemu/releases/download/"
                                   "kernel-fixture-aaaaaaaa/Image";
            check(portable_development_kernel(legacy) == legacy_kernel,
                  "old release URL survives in prepared machine descriptors");

            // Only the release asset named by the Image pin is accepted.
            for (const auto *url :
                 {"https://github.com/voyemu/voyemu-deps/releases/download/kernel-fixture-bbbbbbbb/Image",
                  "https://example.com/voyemu/voyemu/releases/download/kernel-fixture-aaaaaaaa/Image",
                  "https://github.com/voyemu/voyemu-deps/releases/download/fixture-aaaaaaaa/Image",
                  "https://github.com/voyemu/voyemu-deps/releases/download/kernel-fixture-aaaaaaaa/Image?x"}) {
                auto bad = definition;
                bad["android"]["development_kernel"]["url"] = url;
                bool denied = false;
                try {
                    (void)portable_development_kernel(bad);
                } catch (...) {
                    denied = true;
                }

                check(denied, "development kernel URL outside its release accepted");
            }

            check(portable_development_kernel_file(
                          "/root", definition.at("android").at("development_kernel")) ==
                          std::filesystem::path("/root/.local/toolchains/downloads/"
                                                "development-kernel") /
                                  (sha + ".blob"),
                  "development kernel download path differs");
            bool duplicate = false;
            try {
                (void)portable_profile_json(
                        R"({"android":{"development_kernel":{},"development_kernel":null}})");
            } catch (...) {
                duplicate = true;
            }

            check(duplicate, "duplicate development descriptor accepted");
            check(portable_profile_json(R"({"android":{},"legacy":1,"legacy":2})").at("legacy") ==
                          2,
                  "legacy duplicate behavior changed");
        }
        const auto boot = portable_clean_boot_pins(android_base(28));
        check(boot.size() == 4 &&
                      boot.at("build.prop") ==
                              "570d38abec9eb8bcb752758a4ba8e25f128a67b6efbac423cf85dbe1085119ee" &&
                      boot.at("advancedFeatures.ini") ==
                              "2d75e2c798d09f8127c254756ca3d290b23bcc011e03918f2a3334caad5b31f7",
              "accepted encrypted boot metadata missing");
        check(portable_clean_boot_pins(android_base(30)).size() == 6 &&
                      portable_clean_boot_pins(android_base(30)).at("kernel-ranchu") ==
                              "08987ba1309fb275565ddc385134d276d8eab6459f81c016b116fb562433e4fa",
              "API30 boot dependency pins missing");
        bool unknown_api = false;
        auto api29 = android_base(30);
        api29["package"] = "system-images;android-29;default;arm64-v8a";
        try {
            portable_clean_boot_pins(api29);
        } catch (...) {
            unknown_api = true;
        }

        check(unknown_api, "unregistered clean boot API accepted");
        const auto project = std::filesystem::canonical(std::filesystem::path(__FILE__))
                                     .parent_path()
                                     .parent_path()
                                     .parent_path()
                                     .parent_path();
        const auto scratch = argc == 3 ? std::filesystem::canonical(argv[2])
                                       : project / ".local/build/tmp";
        const auto fixture = scratch / ("pr-" + random_token().substr(0, 16));
        const auto root = fixture / "project";
        ensure_private_directory(root);
        struct Cleanup {
            std::filesystem::path root;

            ~Cleanup() {
                std::error_code error;
                std::filesystem::remove_all(root, error);
            }
        } cleanup{fixture};

        if (!cache_policy) {
            qnx_upgrade_checks(root);
            qnx_upgrade_checks(root, true);
            qnx_persistent_upgrade_checks(root);
            qnx_persistent_upgrade_checks(root, true);
            observer_configuration_checks(root);
            observer_configuration_checks(root, true);
            observer_build_snapshot_checks(root);
            system_upgrade_checks(root, std::filesystem::canonical(argv[0]));
            system_upgrade_checks(root, std::filesystem::canonical(argv[0]), false);
            passion_wheel_refresh_checks(root);
            passion_system_upgrade_checks(root, std::filesystem::canonical(argv[0]));
            sparse_copy_checks(root);
        }

#ifdef _WIN32
        const std::string engine_name = "qemu-system-aarch64.exe";
#else
        const std::string engine_name = "qemu-system-aarch64";
#endif
        PortableRuntimeHostBindings installed;
        const auto engine = fixture / "installed" / engine_name,
                   unrelated = fixture / "installed/unrelated-library";
        put(engine, "installed engine fixture");
        put(unrelated, "unrelated fixture");
        installed.qnx_qemu = std::filesystem::canonical(engine);
        installed.pins[installed.qnx_qemu.string()] = sha256_file(engine);
        installed.pins[unrelated.string()] = sha256_file(unrelated);
        check(verify_portable_host_tool(root, installed, installed.qnx_qemu, true) ==
                      sha256_file(engine),
              "explicit canonical installed QNX engine pin rejected");
        bool outside_rejected = false;
        try {
            verify_portable_host_tool(root, installed, installed.qnx_qemu);
        } catch (...) {
            outside_rejected = true;
        }

        check(outside_rejected, "generic host dependency escaped project");
        outside_rejected = false;
        try {
            verify_portable_host_tool(root, installed, unrelated, true);
        } catch (...) {
            outside_rejected = true;
        }

        check(outside_rejected, "unrelated outside host pin accepted as QNX engine");
        installed.pins[installed.qnx_qemu.string()] = std::string(64, '0');
        outside_rejected = false;
        try {
            verify_portable_host_tool(root, installed, installed.qnx_qemu, true);
        } catch (...) {
            outside_rejected = true;
        }

        check(outside_rejected, "installed QNX engine wrong pin accepted");
        const std::string profile = "free-09-chinese-20260106-6.0.3";
        const auto default_machine = machine_identity(profile, ""),
                   canonical_machine = machine_identity(profile, profile);
        const auto development_machine = machine_identity(profile, "development-fixture");
        const auto can_model_machine = machine_identity(profile, "can-model-fixture");
        const auto external_machine = machine_identity(profile, "external-first");
        const auto adb_cache_machine = machine_identity(profile, "adb-cache-regression");
        const auto persistent_machine_identity = machine_identity(profile,
                                                                  "qinggan09-qnx-persistent");
        const auto snapshot_machine = machine_identity(profile, "without-recipe-or-source");
        const auto copied_machine = machine_identity(profile, "persistent-copy");
        {
            const auto app = fixture / "runtime-consumer-host",
                       external = fixture / "outside-host/original-machine";
            ensure_private_directory(app);
            ensure_private_directory(external / "android");
#ifdef _WIN32
            const std::string tool_suffix = ".exe";
#else
            const std::string tool_suffix;
#endif
            const auto sdk = app / ".local/toolchains/native-sdk",
                       disk_tool = sdk / "emulator" / ("qemu-img" + tool_suffix),
                       format_tool = android_sdk_ext4_program(sdk, !tool_suffix.empty());
            ensure_private_directory(disk_tool.parent_path());
            ensure_private_directory(format_tool.parent_path());
            std::filesystem::copy_file(std::filesystem::canonical(argv[0]), disk_tool);
            std::filesystem::copy_file(std::filesystem::canonical(argv[0]), format_tool);

            if (const auto config = android_sdk_ext4_config(sdk, !tool_suffix.empty());
                !config.empty()) {
                put(config, "fixed formatter fixture config");
            }

            put(sdk / "emulator" / ("emulator" + tool_suffix), "not launched");
            const auto program = app / ("renamed-current-app" + tool_suffix);
            put(program, "first current binary");
#ifdef _WIN32
            put(sdk / "emulator/qemu/windows-x86_64/qemu-system-aarch64.exe", "not launched");
#endif
            constexpr auto archive =
                    "86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9";

            for (const auto &[name, pin] : portable_clean_boot_pins(android_base(28))) {
                put(external / "android" / name, std::string("installed boot dependency ") + name);
            }

            ensure_private_directory(app / "profiles/common/wheels/test");
            write_private_json(
                    app / "profiles" / profile / "profile.json",
                    {{"title", "Synthetic Free 1.0"},
                     {"wheel", {{"assets", "profiles/common/wheels/test"}}},
                     {"android",
                      {{"package", "system-images;android-28;default;arm64-v8a"},
                       {"revision", "1"},
                       {"url",
                        "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip"},
                       {"sha256", archive},
                       {"clean_base", android_base(28).at("clean_base")}}},
                     {"emulator", {{"port", 5562}}},
                     {"original",
                      {{"BluetoothPhone",
                        {{"sha256",
                          "d5a7417be79e2e4bfd70e643df85ae76e1a8a15332a345f126ccc5781f5ba8a3"}}}}},
                     {"patches",
                      {{"BluetoothPhone",
                        {{"input", "original:BluetoothPhone"},
                         {"steps",
                          Json::array({"systemservice-framework.json", "add-qg-runtime.json",
                                       "phone-provider.json"})}}}}}});
            write_private_json(app / "src/app/android-wheel-routes.json",
                               {{"routes", Json::array()}});
            Json images = Json::object();
            std::map<std::string, Json> original_ids;

            for (const auto *name :
                 {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img"}) {
                const auto file = external / "android" / name;
                put(file, std::string("original fixture ") + name);
                images[name] = {{"file", std::string("android/") + name},
                                {"bytes", std::filesystem::file_size(file)},
                                {"sha256", std::string(64, '0')}};
                original_ids[name] = cache_file_identity(file);
            }

            Json portable{
                    {"schema", 1},
                    {"kind", "portable-guest-runtime"},
                    {"profile", profile},
                    {"profile_sha256", sha256_file(app / "profiles" / profile / "profile.json")},
                    {"android",
                     {{"api", 28},
                      {"abi", "arm64-v8a"},
                      {"base_archive_sha256", archive},
                      {"images", images}}}};
            portable["resources"] = resource_fixture(external);
            std::filesystem::copy_file(app / "profiles" / profile / "profile.json",
                                       external / "profile.json");
            write_private_json(external / "runtime.json", portable);
            // Development kernels are bound from the embedded machine and fenced in cached
            // launches.
            {
                const auto dev = fixture / "outside-host/development-machine";
                std::filesystem::copy(external, dev, std::filesystem::copy_options::recursive);
                auto definition = read_private_json(dev / "profile.json");
                const auto kernel = dev / "android/kernel-ranchu";
                const auto sha = sha256_file(kernel);
                definition["android"] = {
                        {"package", "system-images;android-28;default;arm64-v8a"},
                        {"revision", "1"},
                        {"url",
                         "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip"},
                        {"sha256", archive},
                        {"clean_base", android_base(28).at("clean_base")},
                        {"development_kernel",
                         {{"url",
                           "https://github.com/voyemu/voyemu-deps/releases/download/kernel-fixture-" +
                                   sha.substr(0, 8) + "/Image"},
                          {"sha256", sha},
                          {"abi", "arm64-v8a"},
                          {"provenance",
                           {{"source_revision", "fixture"},
                            {"source_content_sha256", std::string(64, '1')},
                            {"embedded_config_sha256", std::string(64, '2')},
                            {"normalized_config_sha256", std::string(64, '3')},
                            {"toolchain", "fixture"},
                            {"patches", Json::array({std::string(64, '4')})},
                            {"configuration_deviations", Json::array()}}}}}};
                write_private_json(dev / "profile.json", definition);
                auto manifest = portable;
                manifest["profile_sha256"] = sha256_file(dev / "profile.json");
                manifest["android"]["development_kernel"] =
                        definition.at("android").at("development_kernel");
                Json files = Json::object();

                for (const auto &[name, pin] : portable_clean_boot_pins(android_base(28))) {
                    const auto path = dev / "android" / name;
                    files[name] = {{"file", "android/" + name},
                                   {"sha256", sha256_file(path)},
                                   {"bytes", std::filesystem::file_size(path)}};
                }

                manifest["android"]["boot_files"] = files;
                write_private_json(dev / "runtime.json", manifest);
                const auto author = app / "profiles" / profile / "profile.json";
                std::filesystem::rename(author, author.string() + ".hidden");
                const auto scope = bind_machine_configuration(app, profile, development_machine,
                                                              dev, program, true, false);
                auto launch = read_private_json(scope / "android-launch.json");
                auto count_kernel = [&](const Json &value) {
                    unsigned count = 0;

                    for (const auto &pin : value.at("pins")) {
                        if (pin.at("path").get<std::string>() == kernel.string() &&
                            pin.at("sha256").get<std::string>() == sha) {
                            ++count;
                        }
                    }

                    return count;
                };
                check(count_kernel(launch) == 1, "development launch missing exact kernel pin");
                const auto other = dev / "other-android";
                ensure_private_directory(other);
                write_private_json(other / "machine.json", {{"profile", profile}});
                auto wrong = launch;
                wrong["machine"] = other.string();
                wrong["base"] = other.string();
                write_private_json(scope / "android-launch.json", wrong);
                bind_machine_configuration(app, profile, development_machine, dev, program, true,
                                           false);
                check(read_private_json(scope / "android-launch.json").at("base") ==
                              kernel.parent_path().string(),
                      "stale development launch accepted foreign base");
                launch = read_private_json(scope / "android-launch.json");
                wrong = launch;
                wrong["args"].push_back("-kernel");
                wrong["args"].push_back((other / "kernel-ranchu").string());
                write_private_json(scope / "android-launch.json", wrong);
                bind_machine_configuration(app, profile, development_machine, dev, program, true,
                                           false);
                check(read_private_json(scope / "android-launch.json").at("args") ==
                              launch.at("args"),
                      "stale development launch accepted kernel override");
                launch = read_private_json(scope / "android-launch.json");

                for (const auto &mutation : {"sha", "path", "duplicate"}) {
                    wrong = launch;

                    for (auto &pin : wrong["pins"]) {
                        if (pin.at("path").get<std::string>() == kernel.string()) {
                            if (std::string_view(mutation) == "sha") {
                                pin["sha256"] = std::string(64, '0');
                            } else if (std::string_view(mutation) == "path") {
                                pin["path"] = (other / "kernel-ranchu").string();
                            }

                            break;
                        }
                    }

                    if (std::string_view(mutation) == "duplicate") {
                        wrong["pins"].push_back({{"path", kernel.string()}, {"sha256", sha}});
                    }

                    write_private_json(scope / "android-launch.json", wrong);
                    bind_machine_configuration(app, profile, development_machine, dev, program,
                                               true, false);
                    check(count_kernel(read_private_json(scope / "android-launch.json")) == 1,
                          "stale development kernel pin accepted");
                }

                auto mismatch = manifest;
                mismatch["android"]["development_kernel"]["provenance"]["toolchain"] = "different";
                write_private_json(dev / "runtime.json", mismatch);
                bool mismatch_denied = false;
                try {
                    bind_machine_configuration(app, profile, development_machine, dev, program,
                                               true, false);
                } catch (...) {
                    mismatch_denied = true;
                }

                check(mismatch_denied, "different embedded kernel descriptor accepted");
                write_private_json(dev / "runtime.json", manifest);
                launch = read_private_json(scope / "android-launch.json");
                auto &pins = launch["pins"];

                for (auto it = pins.begin(); it != pins.end(); ++it) {
                    if (it->at("path").get<std::string>() == kernel.string()) {
                        pins.erase(it);
                        break;
                    }
                }

                write_private_json(scope / "android-launch.json", launch);
                bind_machine_configuration(app, profile, development_machine, dev, program, true,
                                           false);
                check(count_kernel(read_private_json(scope / "android-launch.json")) == 1,
                      "stale development launch was reused");
                put(kernel, "tampered kernel");
                bool denied = false;
                try {
                    bind_machine_configuration(app, profile, development_machine, dev, program,
                                               true, false);
                } catch (...) {
                    denied = true;
                }

                check(denied, "tampered development kernel accepted");
                std::filesystem::rename(author.string() + ".hidden", author);
            }
            {
                const auto machine = fixture / "outside-host/can-model-machine";
                std::filesystem::copy(external, machine, std::filesystem::copy_options::recursive);
                const std::string artifact = "component:can-model-observer",
                                  target = "/system/bin/voyemu-can-model-observer", helper(64, 'c');
                const Json row = {{"artifact", artifact}, {"target", target}, {"mode", "0755"},
                                  {"mutable", false},     {"package", ""},    {"sha256", helper}};
                const Json cold_check = {{"id", "artifact:" + artifact + ":" + target},
                                         {"passed", true},
                                         {"expected", helper},
                                         {"actual", helper}};
                auto manifest = portable;
                const Json proof = {{"schema", 1},
                                    {"kind", "android-candidate"},
                                    {"profile", profile},
                                    {"profile_sha256", manifest.at("profile_sha256")},
                                    {"stopped", true},
                                    {"prepared_VM", true},
                                    {"cold_boot_verified", true},
                                    {"old_machine_modified", false},
                                    {"android",
                                     {{"images",
                                       {{"system.img",
                                         {{"sha256", manifest.at("android")
                                                             .at("images")
                                                             .at("system.img")
                                                             .at("sha256")}}}}}}},
                                    {"installed", Json::array({row})},
                                    {"checks", Json::array({cold_check})}};
                const auto receipt = machine / "can-model/candidate.json";
                put(receipt, proof.dump());
                manifest["android"]["can_model_receipt"] = {
                        {"file", "can-model/candidate.json"},
                        {"sha256", sha256_file(receipt)},
                        {"bytes", std::filesystem::file_size(receipt)}};
                write_private_json(machine / "runtime.json", manifest);
                const auto author = app / "profiles" / profile / "profile.json";
                std::filesystem::rename(author, author.string() + ".hidden-can-model");
                const auto scope = bind_machine_configuration(app, profile, can_model_machine,
                                                              machine, program, true, false);
                auto launch = read_private_json(scope / "android-launch.json");
                check(launch.at("can_model_observer_sha256") == helper,
                      "CAN hash not passed to launch");
                launch["can_model_observer_sha256"] = std::string(64, 'd');
                write_private_json(scope / "android-launch.json", launch);
                bind_machine_configuration(app, profile, can_model_machine, machine, program, true,
                                           false);
                check(read_private_json(scope / "android-launch.json")
                                      .at("can_model_observer_sha256") == helper,
                      "stale CAN launch reused");
                put(receipt, "tampered");
                bool denied = false;
                try {
                    bind_machine_configuration(app, profile, can_model_machine, machine, program,
                                               true, false);
                } catch (...) {
                    denied = true;
                }

                check(denied, "tampered CAN receipt reused launch");
                std::filesystem::rename(author.string() + ".hidden-can-model", author);
            }

            // Portable startup reads boot files from its own machine, never shared base cache.
            const auto held_boot = external / "android/kernel-ranchu.held";
            std::filesystem::rename(external / "android/kernel-ranchu", held_boot);
            bool missing_base = false;
            try {
                bind_machine_configuration(app, profile,
                                           machine_identity(profile, "external-missing-base"),
                                           external, program, true, false);
            } catch (const std::exception &error) {
                missing_base = std::string(error.what()).find("kernel-ranchu") != std::string::npos;
            }

            std::filesystem::rename(held_boot, external / "android/kernel-ranchu");
            check(missing_base && !std::filesystem::exists(external / "android/system.img.qcow2"),
                  "missing machine boot file must fail before disk creation");
            // CLI creates its active directory before the first configuration bind.
            const auto first_scope = profile_launch_directory(app, profile, external_machine);
            const auto first_lock = first_scope / "active" / (profile + ".launch.lock");
            ensure_private_directory(first_lock.parent_path());
            put(first_lock, "");
#ifndef _WIN32
            check(::chmod(first_lock.c_str(), 0600) == 0, "private first-launch lock fixture");
#endif
            const auto first_lock_identity = cache_file_identity(first_lock);
            const auto retained_placement = first_scope / "state/placement.json";
            write_private_json(retained_placement, {{"schema", 1}, {"scale_percent", 50}});
            const auto placement_identity = cache_file_identity(retained_placement);
            const auto placement_sha = sha256_file(retained_placement);
            const auto first = [&] {
#ifdef _WIN32
                struct HeldLock {
                    HANDLE file = INVALID_HANDLE_VALUE;

                    ~HeldLock() {
                        if (file != INVALID_HANDLE_VALUE) {
                            CloseHandle(file);
                        }
                    }
                } held;
                held.file = CreateFileW(first_lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                check(held.file != INVALID_HANDLE_VALUE, "held CLI lock fixture");
#else
                struct HeldLock {
                    int file = -1;

                    ~HeldLock() {
                        if (file >= 0) {
                            ::close(file);
                        }
                    }
                } held;
                held.file = ::open(first_lock.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
                check(held.file >= 0 && ::flock(held.file, LOCK_EX | LOCK_NB) == 0,
                      "held CLI lock fixture");
#endif

                return bind_machine_configuration(app, profile, external_machine, external, program,
                                                  true, false);
            }();
            write_private_json(external / "profile.json", {{"tampered", true}});

            for (const bool multimedia : {true, false}) {
                bool denied = false;
                try {
                    bind_machine_configuration(app, profile, external_machine, external, program,
                                               multimedia, !multimedia);
                } catch (...) {
                    denied = true;
                }

                check(denied, "cached or cluster-only binding accepted tampered snapshot");
            }

            std::filesystem::copy_file(app / "profiles" / profile / "profile.json",
                                       external / "profile.json",
                                       std::filesystem::copy_options::overwrite_existing);
            const auto launch = read_private_json(first / "android-launch.json");
            check(launch.at("machine") == (external / "android").string() &&
                          launch.at("frontend").at("executable") == program.string() &&
                          launch.at("frontend").at("backend") == "voyemu",
                  "external machine binding did not consume current paths");
            check(!std::filesystem::exists(app / "machines") &&
                          !std::filesystem::exists(app / ".local/machines/retired-portable"),
                  "external binding copied a machine graph");
            check(cache_file_identity(first_lock) == first_lock_identity &&
                          read_private_json(first_scope / "activation.json").at("machine") ==
                                  "external-first",
                  "first configuration replaced its live lock or omitted activation");
            check(cache_file_identity(retained_placement) == placement_identity &&
                          sha256_file(retained_placement) == placement_sha,
                  "first configuration changed retained placement state");
            {
                const std::string move_name = "relocation-machine";
                const auto moved_machine = machine_identity(profile, move_name);
                const auto old_seed = fixture / "relocation-origin/machines" / move_name,
                           new_seed = app / "machines" / move_name;
                const auto prepare_move_seed = [&](const std::filesystem::path &target) {
                    ensure_private_directory(target / "android");

                    for (const auto *name :
                         {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img"}) {
                        put(target / "android" / name, std::string("relocation disk ") + name);
                    }

                    for (const auto &[name, pin] : portable_clean_boot_pins(android_base(28))) {
                        put(target / "android" / name,
                            std::string("installed boot dependency ") + name);
                    }

                    cache(target / "android/cache.img");
                    std::filesystem::copy_file(external / "profile.json", target / "profile.json");
                    auto metadata = portable;
                    metadata["resources"] = resource_fixture(target);
                    write_private_json(target / "runtime.json", metadata);
                };
                prepare_move_seed(old_seed);
                prepare_move_seed(new_seed);
                const auto move_scope = bind_machine_configuration(app, profile, moved_machine,
                                                                   old_seed, program, true, false);
                const auto original_activation =
                        read_private_json_bytes(move_scope / "activation.json");
                bool collision = false;
                try {
                    bind_machine_configuration(app, profile, moved_machine, new_seed, program, true,
                                               false);
                } catch (...) {
                    collision = true;
                }

                check(collision && read_private_json_bytes(move_scope / "activation.json") ==
                                           original_activation,
                      "two existing same-basename machines shared a runtime");
                std::filesystem::remove_all(new_seed);
                std::filesystem::rename(old_seed, new_seed);
#ifndef _WIN32
                std::filesystem::create_directory_symlink(fixture / "absent-relocation-target",
                                                          old_seed);
                bool dangling = false;
                try {
                    bind_machine_configuration(app, profile, moved_machine, new_seed, program, true,
                                               false);
                } catch (...) {
                    dangling = true;
                }

                check(dangling && read_private_json_bytes(move_scope / "activation.json") ==
                                          original_activation,
                      "dangling old seed was accepted as absent");
                std::filesystem::remove(old_seed);
#endif
                const auto move_sid = random_token();
                const auto move_session = native_session_directory(app, profile, moved_machine,
                                                                   move_sid);
                write_private_json(move_session / "runtime.json",
                                   {{"session_id", move_sid}, {"stopped", false}});
                const auto move_owner = native_current_active_directory(app, profile,
                                                                        moved_machine) /
                                        (profile + ".json");
                write_private_json(move_owner, {{"session_id", move_sid},
                                                {"directory", move_session.string()}});
                bool active_move = false;
                try {
                    bind_machine_configuration(app, profile, moved_machine, new_seed, program, true,
                                               false);
                } catch (...) {
                    active_move = true;
                }

                check(active_move && read_private_json_bytes(move_scope / "activation.json") ==
                                             original_activation,
                      "active machine permitted relocation");
                std::filesystem::remove(move_owner);
                check(bind_machine_configuration(app, profile, moved_machine, new_seed, program,
                                                 true, false) == move_scope,
                      "same-basename relocation failed with retained runtime");
                const auto moved_spec = read_private_json(move_scope / "android-launch.json");
                check(moved_spec.at("machine") == (new_seed / "android").string() &&
                              moved_spec.at("previous_machine") == (old_seed / "android").string(),
                      "relocation lost the old owned disk alias scope");
                const auto arbitrary_seed = fixture / "arbitrary-destination" / move_name;
                ensure_private_directory(arbitrary_seed.parent_path());
                std::filesystem::rename(new_seed, arbitrary_seed);
                check(bind_machine_configuration(app, profile, moved_machine, arbitrary_seed,
                                                 program, true, false) == move_scope,
                      "arbitrary same-basename destination was rejected");
                std::filesystem::rename(arbitrary_seed, old_seed);
                check(bind_machine_configuration(app, profile, moved_machine, old_seed, program,
                                                 true, false) == move_scope,
                      "same-basename relocation back failed with retained runtime");
                check(read_private_json(move_scope / "android-launch.json")
                                      .at("previous_machine") ==
                              (arbitrary_seed / "android").string(),
                      "relocation back retained the wrong previous disk scope");
            }

            for (unsigned fault = 0; fault < 5; ++fault) {
                const auto name = "external-partial-" + std::to_string(fault);
                const auto faulty_machine = machine_identity(profile, name);
                const auto scope = profile_launch_directory(app, profile, faulty_machine);
                ensure_private_directory(scope / "active");
                const auto bad = scope / (fault == 0   ? "android-launch.json"
                                          : fault == 1 ? "unexpected.json"
                                          : fault == 2 ? "activation.json"
                                          : fault == 3 ? "active/unexpected.lock"
                                                       : "state/unexpected.json");
                put(bad, fault == 2 ? "{broken" : "retained");
                const auto before = cache_file_identity(bad);
                bool rejected = false;
                try {
                    bind_machine_configuration(app, profile, faulty_machine, external, program,
                                               true, false);
                } catch (...) {
                    rejected = true;
                }

                check(rejected && cache_file_identity(bad) == before &&
                              !std::filesystem::exists(scope / "config-template.ini"),
                      "partial or unknown first scope was repaired or mutated");
            }

            for (const auto &[name, identity] : original_ids) {
                check(cache_file_identity(external / "android" / name) == identity,
                      "binding changed an original image identity");
            }

            check(valid_machine_console_port(launch.at("port").get<unsigned>()) &&
                          launch.at("sdk_control").at("grpc_port") ==
                                  launch.at("port").get<unsigned>() + 30000,
                  "first binding retained reserved or incoherent host ports");
            check(synthetic_phone_supported(launch),
                  "external renamed A9 lost retained phone provider capability");
            check(!read_private_json(external / "profile.json").contains("id"),
                  "A9 portability regression must retain real raw snapshot without id");
            const auto adb_cache = bind_machine_configuration(app, profile, adb_cache_machine,
                                                              external, program, true, false);
            const auto adb_launch_file = adb_cache / "android-launch.json";
            const auto adb_launch = read_private_json(adb_launch_file);
            const auto adb = sdk / "platform-tools" / ("adb" + tool_suffix);
            check(!std::filesystem::exists(adb) && !adb_launch.contains("adb") &&
                          adb_launch.at("adb_transport") == "direct",
                  "ready configuration requires SDK adb");

            for (const auto &entry : adb_launch.at("pins")) {
                check(entry.at("path") != adb.string(), "ready configuration pins SDK adb");
            }

            write_private_json(qnx_host_bindings_path(app), {{"android_port", 5578}});
            const auto override_cache = bind_machine_configuration(
                    app, profile, machine_identity(profile, "explicit-port"), external, program,
                    true, false);
            check(read_private_json(override_cache / "android-launch.json").at("port") == 5578,
                  "explicit isolated host port was ignored");
            std::filesystem::remove(qnx_host_bindings_path(app));

            // Legacy canonical activation may still point at a separate older writable runtime.
            const auto canonical = app / "machines" / profile;
            ensure_private_directory(canonical / "android");

            for (const auto &[name, identity] : original_ids) {
                std::filesystem::copy_file(external / "android" / name,
                                           canonical / "android" / name);
            }

            for (const auto &[name, pin] : portable_clean_boot_pins(android_base(28))) {
                ensure_private_directory(canonical / "android");
                std::filesystem::copy_file(external / "android" / name,
                                           canonical / "android" / name);
            }

            auto canonical_manifest = portable;
            Json canonical_qnx = Json::object();

            for (const auto *name : {"hyp-hvf.img", "ifs2-native.img", "host-native-lzo.bin",
                                     "smem.bin", "qnx-ifs2-restore.bin", "synthetic-gpt.bin",
                                     "sysla-gpt-ramdisk-v3.img", "qnx-ipl-virt.bin"}) {
                put(canonical / "qnx" / name, std::string("fresh canonical QNX ") + name);
                canonical_qnx[name] = {
                        {"file", std::string("qnx/") + name},
                        {"bytes", std::filesystem::file_size(canonical / "qnx" / name)},
                        {"sha256", std::string(64, '0')}};
            }

            canonical_manifest["qnx"] = {{"guest_arch", "aarch64"}, {"images", canonical_qnx}};
            canonical_manifest["resources"] = resource_fixture(canonical);
            std::filesystem::copy_file(external / "profile.json", canonical / "profile.json");
            write_private_json(canonical / "runtime.json", canonical_manifest);
            const auto default_cache = bind_machine_configuration(app, profile, default_machine,
                                                                  canonical, program, true, false);

            // The API names the canonical machine by its profile and the CLI leaves it unnamed;
            // a stale launch refreshes under either spelling whichever one recorded it.
            for (const auto &recorded : {std::string(), profile}) {
                auto activation = read_private_json(default_cache / "activation.json");
                activation["machine"] = recorded;
                write_private_json(default_cache / "activation.json", activation);

                for (const auto &selector : {std::string(), profile}) {
                    const auto selected_machine = machine_identity(profile, selector);
                    auto stale = read_private_json(default_cache / "android-launch.json");
                    stale["frontend"]["executable"] = (app / "retired-launcher").string();
                    write_private_json(default_cache / "android-launch.json", stale);
                    check(bind_machine_configuration(app, profile, selected_machine, canonical,
                                                     program, true, false) == default_cache &&
                                  read_private_json(default_cache / "android-launch.json")
                                                  .at("frontend")
                                                  .at("executable") == program.string(),
                          "canonical machine spelling blocked a stale launch refresh");
                }
            }

#ifndef _WIN32
            // A configuration recorded on the other host matches under either machine spelling.
            auto foreign_seed = "Z:" + canonical.string();
            std::replace(foreign_seed.begin(), foreign_seed.end(), '/', '\\');
            const auto canonical_sha = sha256_file(canonical / "runtime.json");

            for (const auto &recorded : {std::string(), profile}) {
                const Json foreign = {{"schema", 1},          {"kind", "runtime-configuration"},
                                      {"profile", profile},   {"machine", recorded},
                                      {"seed", foreign_seed}, {"manifest_sha256", canonical_sha}};

                for (const auto &selector : {std::string(), profile}) {
                    const auto selected_machine = machine_identity(profile, selector);
                    check(portable_configuration_from_other_host(foreign, app, profile,
                                                                 selected_machine, canonical,
                                                                 canonical_sha),
                          "canonical machine spelling hid a foreign-host configuration");
                }

                check(!portable_configuration_from_other_host(
                              foreign, app, profile, machine_identity(profile, "other-machine"),
                              canonical, canonical_sha),
                      "foreign-host configuration matched another machine");
            }
#endif
            const auto old_runtime = app / ".local/machines/retired-portable" / profile /
                                     "portable-old",
                       old_data = old_runtime / "android-machine";
            ensure_private_directory(old_data);
            put(old_data / "config.ini", "legacy configuration");
            put(old_data / "machine.json", "{\"profile\":\"" + profile + "\"}");
            put(old_data / "userdata-qemu.img.qcow2", "unique legacy userdata bytes");
            const auto legacy_userdata = cache_file_identity(old_data / "userdata-qemu.img.qcow2");
            auto legacy_launch = read_private_json(default_cache / "android-launch.json");
            legacy_launch["machine"] = old_data.string();
            write_private_json(default_cache / "android-launch.json", legacy_launch);
            write_private_json(default_cache / "cluster-launch.json",
                               {{"stale", "legacy QNX launch"}});
            const Json legacy_record = {
                    {"schema", 1},
                    {"activation_version", 4},
                    {"profile", profile},
                    {"manifest_sha256", std::string(64, '1')},
                    {"runtime", old_runtime.string()},
                    {"android_launch_sha256", sha256_file(default_cache / "android-launch.json")}};
            write_private_json(default_cache / "activation.json", legacy_record);
            const auto active_legacy_dir = native_session_directory(app, profile, default_machine,
                                                                    "legacy-session"),
                       active_legacy_file = native_current_active_directory(app, profile,
                                                                            default_machine) /
                                            (profile + ".json");
            ensure_private_directory(active_legacy_dir);
            write_private_json(active_legacy_dir / "runtime.json",
                               {{"session_id", "legacy-session"}, {"stopped", false}});
            write_private_json(active_legacy_file, {{"session_id", "legacy-session"},
                                                    {"directory", active_legacy_dir.string()}});
            bool legacy_live_refused = false;
            try {
                bind_machine_configuration(app, profile, default_machine, canonical, program, true,
                                           false);
            } catch (const std::exception &error) {
                legacy_live_refused = std::string(error.what()).find("stop selected profile") !=
                                      std::string::npos;
            }

            check(legacy_live_refused &&
                          read_private_json(default_cache / "android-launch.json") == legacy_launch,
                  "live legacy activation was reused or changed during seed transition");
            write_private_json(active_legacy_dir / "runtime.json",
                               {{"session_id", "legacy-session"}, {"stopped", true}});
            check(bind_machine_configuration(app, profile, default_machine, canonical, program,
                                             true, false) == default_cache &&
                          read_private_json(default_cache / "android-launch.json").at("machine") ==
                                  (canonical / "android").string(),
                  "canonical start retained the old legacy guest instead of binding the new prepared seed");
            check(read_private_json(default_cache / "activation.json").at("manifest_sha256") ==
                                  sha256_file(canonical / "runtime.json") &&
                          cache_file_identity(old_data / "userdata-qemu.img.qcow2") ==
                                  legacy_userdata,
                  "canonical migration lost manifest identity or changed legacy userdata");
            check(valid_machine_console_port(
                          read_private_json(default_cache / "android-launch.json")
                                  .at("port")
                                  .get<unsigned>()),
                  "canonical refresh retained a reserved profile port");
            bool legacy_history = false;

            for (const auto &entry : std::filesystem::directory_iterator(default_cache)) {
                if (entry.path().filename().string().starts_with("activation-") &&
                    entry.path().extension() == ".json" &&
                    read_private_json(entry.path()) == legacy_record) {
                    legacy_history = true;
                }
            }

            check(legacy_history, "legacy activation provenance was not preserved");
            const auto canonical_engine = app / ".local/toolchains/fixture-qnx" /
                                          ("qemu-system-aarch64" + tool_suffix),
                       canonical_angle = app / ".local/toolchains/fixture-angle";
            put(canonical_engine, "not launched");
            const std::string canonical_library(os::shared_library_suffix);

            for (const auto *name : {"libEGL", "libGLESv2"}) {
                put(canonical_angle / (std::string(name) + canonical_library), "not launched");
            }

            Json canonical_host = {{"qnx_qemu", canonical_engine.string()},
                                   {"angle", canonical_angle.string()}};
#ifdef _WIN32
            // Every Windows cluster launch, persistent or not, needs the verified file backend.
            write_private_json(qnx_host_bindings_path(app), canonical_host);
            bool unverified = false;
            try {
                bind_machine_configuration(app, profile, default_machine, canonical, program, false,
                                           true);
            } catch (const std::runtime_error &error) {
                unverified =
                        std::string_view(error.what()).starts_with("QNX_FILE_BACKEND_UNSUPPORTED:");
            }

            check(unverified, "Windows cluster bound without a verified file backend");
            canonical_host["qnx_file_backend"] = {{"schema", 1},
                                                  {"version", "11.1.2"},
                                                  {"engine_sha256", sha256_file(canonical_engine)}};
#endif
            write_private_json(qnx_host_bindings_path(app), canonical_host);
            check(bind_machine_configuration(app, profile, default_machine, canonical, program,
                                             false, true) == default_cache &&
                          read_private_json(default_cache / "cluster-launch.json").contains("qemu"),
                  "Android-only migration reused the old cluster on a later cluster start");
#ifdef _WIN32
            check(read_private_json(default_cache / "cluster-launch.json").at("qnx_file_backend") ==
                          canonical_host.at("qnx_file_backend"),
                  "nonpersistent Windows cluster launch lost its file backend capability");
#endif
            const auto clock_launch = read_private_json(default_cache / "cluster-launch.json");
            const auto clock_manifest = sha256_file(canonical / "runtime.json");
            const auto clock_identity = cache_file_identity(default_cache / "cluster-launch.json");
            check(bind_machine_configuration(app, profile, default_machine, canonical, program,
                                             false, true) == default_cache &&
                          cache_file_identity(default_cache / "cluster-launch.json") ==
                                  clock_identity,
                  "current QNX clock policy rebuilt its launch cache");
            auto stale_graphics = clock_launch;
            const auto diagnostic_program = app / ".local/build/tmp/diagnostics-epoch/diag-host";
            put(diagnostic_program, "old diagnostic host");
            stale_graphics["graphics"]["executable"] = diagnostic_program.string();
            stale_graphics["pins"].push_back({{"path", diagnostic_program.string()},
                                              {"sha256", sha256_file(diagnostic_program)}});
            const auto guest_pin = canonical / "qnx/hyp-hvf.img";
            stale_graphics["pins"].push_back(
                    {{"path", guest_pin.string()}, {"sha256", sha256_file(guest_pin)}});
            write_private_json(default_cache / "cluster-launch.json", stale_graphics);
            const auto android_identity =
                    cache_file_identity(default_cache / "android-launch.json");
            check(bind_machine_configuration(app, profile, default_machine, canonical, program,
                                             true, true) == default_cache,
                  "graphics rebind changed cache directory");
            auto rebound_graphics = stale_graphics;
            rebound_graphics["graphics"]["executable"] = program.string();
            rebound_graphics["pins"][0] = {{"path", program.string()},
                                           {"sha256", sha256_file(program)}};
            check(read_private_json(default_cache / "cluster-launch.json") == rebound_graphics,
                  "cached diagnostic graphics host was reused or guest configuration changed");
            check(cache_file_identity(default_cache / "android-launch.json") == android_identity &&
                          sha256_file(canonical / "runtime.json") == clock_manifest,
                  "graphics rebind changed current Android launch or guest manifest");
            const auto rebound_identity =
                    cache_file_identity(default_cache / "cluster-launch.json");
            bind_machine_configuration(app, profile, default_machine, canonical, program, true,
                                       true);
            check(cache_file_identity(default_cache / "cluster-launch.json") == rebound_identity,
                  "current graphics host was rebound on cache hit");
            auto unpinned_graphics = clock_launch;
            unpinned_graphics["graphics"]["executable"] = diagnostic_program.string();
            write_private_json(default_cache / "cluster-launch.json", unpinned_graphics);
            write_private_json(active_legacy_dir / "runtime.json",
                               {{"session_id", "legacy-session"}, {"stopped", false}});
            bool live_graphics_refused = false;
            try {
                bind_machine_configuration(app, profile, default_machine, canonical, program, true,
                                           true);
            } catch (const std::exception &error) {
                live_graphics_refused = std::string(error.what()).find("stop selected profile") !=
                                        std::string::npos;
            }

            check(live_graphics_refused &&
                          read_private_json(default_cache / "cluster-launch.json") ==
                                  unpinned_graphics,
                  "live profile allowed graphics host refresh");
            write_private_json(active_legacy_dir / "runtime.json",
                               {{"session_id", "legacy-session"}, {"stopped", true}});
            bind_machine_configuration(app, profile, default_machine, canonical, program, true,
                                       true);
            check(read_private_json(default_cache / "cluster-launch.json") == clock_launch,
                  "unpinned cached diagnostic graphics host was not rebound exactly");
            auto stale_clock = clock_launch;
            auto &stale_args = stale_clock["qemu"]["args"];
#ifndef __APPLE__
            bool clock_removed = false;

            for (std::size_t i = 0; i + 1 < stale_args.size(); ++i) {
                if (stale_args[i] == "-icount") {
                    check(stale_args[i + 1] == "shift=0,align=off,sleep=on",
                          "fresh translated clock policy differs");
                    stale_args.erase(stale_args.begin() + i, stale_args.begin() + i + 2);
                    clock_removed = true;
                    break;
                }
            }

            check(clock_removed, "fresh translated launch lacks fixed0");
#else
            for (const auto &argument : stale_args) {
                check(argument != "-icount", "fresh Mac HVF launch inherited icount");
            }

            stale_args.push_back("-icount");
            stale_args.push_back("shift=0,align=off,sleep=on");
#endif
            write_private_json(default_cache / "cluster-launch.json", stale_clock);
            check(bind_machine_configuration(app, profile, default_machine, canonical, program,
                                             false, true) == default_cache,
                  "clock policy refresh changed cache directory");
            const auto refreshed_clock = read_private_json(default_cache / "cluster-launch.json");

            if (refreshed_clock != clock_launch) {
                std::cerr << "clock cache diff " << Json::diff(clock_launch, refreshed_clock).dump()
                          << '\n';
            }

            check(refreshed_clock == clock_launch, "stale cached QNX clock policy was reused");
            check(sha256_file(canonical / "runtime.json") == clock_manifest,
                  "clock policy refresh changed portable guest identity");
            {
                const auto foreign_path = [](const std::filesystem::path &path) {
#ifdef _WIN32
                    return wine_native_host() ? wine_native_unix_path(path) : std::string{};
#else
                    auto value = path.string();
                    std::replace(value.begin(), value.end(), '/', '\\');

                    return "Z:" + value;
#endif
                };

                if (!foreign_path(canonical).empty()) {
                    const auto activation_path = default_cache / "activation.json",
                               android_path = default_cache / "android-launch.json";
                    const auto original_activation = read_private_json(activation_path),
                               original_android = read_private_json(android_path);
                    const auto original_active = read_private_json(active_legacy_file);
                    auto foreign = original_activation;
                    foreign["machine"] = profile;
                    foreign["seed"] = foreign_path(canonical);
                    const auto foreign_android = original_android;
                    const auto unrelated = fixture / "unrelated" / "machines" / profile;
                    ensure_private_directory(unrelated);
                    auto wrong = foreign;
                    wrong["seed"] = foreign_path(unrelated);
                    write_private_json(activation_path, wrong);
                    bool denied = false;
                    try {
                        bind_machine_configuration(app, profile, canonical_machine, canonical,
                                                   program, true, false);
                    } catch (...) {
                        denied = true;
                    }

                    check(denied && read_private_json(activation_path) == wrong &&
                                  read_private_json(android_path) == foreign_android,
                          "foreign checkout cache was rebound");
                    auto wrong_pin = foreign;
                    wrong_pin["manifest_sha256"] = std::string(64, '0');
                    write_private_json(activation_path, wrong_pin);
                    denied = false;
                    try {
                        bind_machine_configuration(app, profile, canonical_machine, canonical,
                                                   program, true, false);
                    } catch (...) {
                        denied = true;
                    }

                    check(denied && read_private_json(activation_path) == wrong_pin,
                          "foreign cache with mismatched manifest pin was rebound");
                    wrong_pin["machine"] = "";
                    write_private_json(activation_path, wrong_pin);
                    denied = false;
                    try {
                        bind_machine_configuration(app, profile, default_machine, canonical,
                                                   program, true, false);
                    } catch (...) {
                        denied = true;
                    }

                    check(denied && read_private_json(activation_path) == wrong_pin,
                          "default machine rebound an unverified foreign cache");
                    write_private_json(activation_path, foreign);
                    auto foreign_active = original_active;
                    foreign_active["directory"] = foreign_path(active_legacy_dir);
                    write_private_json(active_legacy_file, foreign_active);
                    write_private_json(active_legacy_dir / "runtime.json",
                                       {{"session_id", "legacy-session"}, {"stopped", false}});
                    denied = false;
                    try {
                        bind_machine_configuration(app, profile, canonical_machine, canonical,
                                                   program, true, false);
                    } catch (const std::exception &error) {
                        denied = std::string(error.what())
                                         .find("stop selected profile on the other host") !=
                                 std::string::npos;
                    }

                    check(denied && read_private_json(activation_path) == foreign &&
                                  read_private_json(android_path) == foreign_android,
                          "other host live epoch permitted binding refresh");
                    write_private_json(active_legacy_dir / "runtime.json",
                                       {{"session_id", "legacy-session"}, {"stopped", true}});
                    const auto overlay = canonical / "android/userdata-qemu.img.qcow2",
                               system = canonical / "qnx/sysla-gpt-ramdisk-v3.img";
                    const auto overlay_id = cache_file_identity(overlay),
                               system_id = cache_file_identity(system);
                    check(bind_machine_configuration(app, profile, canonical_machine, canonical,
                                                     program, true, false) == default_cache,
                          "foreign host binding selected another scope");
                    const auto current = read_private_json(activation_path),
                               current_android = read_private_json(android_path);
                    check(current.at("seed") == canonical.string() &&
                                  current.value("cluster_rebind_pending", false) &&
                                  current_android.at("port") == original_android.at("port") &&
                                  !current_android.contains("adb") &&
                                  current_android.at("adb_transport") == "direct",
                          "host switch did not refresh paths and preserve the selected Android port");
                    check(bind_machine_configuration(app, profile, canonical_machine, canonical,
                                                     program, false, true) == default_cache &&
                                  read_private_json(default_cache / "cluster-launch.json")
                                                  .at("graphics")
                                                  .at("executable") == program.string() &&
                                  !read_private_json(activation_path)
                                           .contains("cluster_rebind_pending"),
                          "cluster binding was not refreshed after switching hosts");
                    check(cache_file_identity(overlay) == overlay_id &&
                                  cache_file_identity(system) == system_id &&
                                  sha256_file(canonical / "runtime.json") == clock_manifest,
                          "host switch changed canonical guest storage");
                    write_private_json(activation_path, original_activation);
                    write_private_json(android_path, original_android);
                    write_private_json(default_cache / "cluster-launch.json", clock_launch);
                    write_private_json(active_legacy_file, original_active);
                }
            }
#ifndef _WIN32
            const auto persistent_machine = app / "machines/qinggan09-qnx-persistent";
            ensure_private_directory(persistent_machine / "qnx");

            for (const auto &entry : std::filesystem::directory_iterator(canonical / "qnx")) {
                std::filesystem::copy_file(entry.path(),
                                           persistent_machine / "qnx" / entry.path().filename());
            }

            std::filesystem::copy_file(canonical / "profile.json",
                                       persistent_machine / "profile.json");
            auto persistent_seed = canonical_manifest;
            persistent_seed["qnx"]["persistence"] = {{"schema", 1},
                                                     {"system_image", "sysla-gpt-ramdisk-v3.img"},
                                                     {"userdata_image", "userdata-qnx6.img"}};
            put(persistent_machine / "qnx/userdata-qnx6.img", "initial persistent userdata");
            persistent_seed["qnx"]["images"]["userdata-qnx6.img"] = {
                    {"file", "qnx/userdata-qnx6.img"},
                    {"bytes",
                     std::filesystem::file_size(persistent_machine / "qnx/userdata-qnx6.img")},
                    {"sha256", std::string(64, '0')}};
            write_private_json(persistent_machine / "runtime.json", persistent_seed);
            const auto persistent_bound =
                    bind_machine_configuration(app, profile, persistent_machine_identity,
                                               persistent_machine, program, false, true);
            const auto persistent_launch =
                    read_private_json(persistent_bound / "cluster-launch.json");
            check(persistent_launch.at("persistent_storage") ==
                          Json{{"schema", 1},
                               {"system",
                                (persistent_machine / "qnx/sysla-gpt-ramdisk-v3.img").string()},
                               {"userdata",
                                (persistent_machine / "qnx/userdata-qnx6.img").string()}},
                  "persistent QNX paths are outside the selected machine");

            for (const auto &value : persistent_launch.at("qemu").at("args")) {
                check(value.get<std::string>().find(
                              "loader,file=" +
                              (persistent_machine / "qnx/sysla-gpt-ramdisk-v3.img").string()) ==
                              std::string::npos,
                      "persistent system still uses a boot loader");
            }

            put(persistent_machine / "qnx/userdata-qnx6.img", "guest changed userdata");
            put(persistent_machine / "qnx/sysla-gpt-ramdisk-v3.img", "guest changed system");
            const auto user_identity =
                               cache_file_identity(persistent_machine / "qnx/userdata-qnx6.img"),
                       system_identity = cache_file_identity(persistent_machine /
                                                             "qnx/sysla-gpt-ramdisk-v3.img");
            check(bind_machine_configuration(app, profile, persistent_machine_identity,
                                             persistent_machine, program, false,
                                             true) == persistent_bound &&
                          cache_file_identity(persistent_machine / "qnx/userdata-qnx6.img") ==
                                  user_identity &&
                          cache_file_identity(persistent_machine /
                                              "qnx/sysla-gpt-ramdisk-v3.img") == system_identity,
                  "persistent QNX restart rewrote guest changes");
            auto stale_persistent = persistent_launch;
            stale_persistent["persistent_storage"]["system"] = "/old/project/machines/system.img";
            write_private_json(persistent_bound / "cluster-launch.json", stale_persistent);
            auto relocated_activation = read_private_json(persistent_bound / "activation.json");
            relocated_activation["seed"] =
                    (app / "old-project/machines/qinggan09-qnx-persistent").string();
            write_private_json(persistent_bound / "activation.json", relocated_activation);
            bind_machine_configuration(app, profile, persistent_machine_identity,
                                       persistent_machine, program, false, true);
            check(read_private_json(persistent_bound / "cluster-launch.json")
                                          .at("persistent_storage") ==
                                  persistent_launch.at("persistent_storage") &&
                          cache_file_identity(persistent_machine / "qnx/userdata-qnx6.img") ==
                                  user_identity &&
                          cache_file_identity(persistent_machine /
                                              "qnx/sysla-gpt-ramdisk-v3.img") == system_identity,
                  "persistent QNX relocation refresh rewrote guest storage");
            const auto clone = app / "machines/persistent-clone";
            ensure_private_directory(clone / "qnx");

            for (const auto &entry :
                 std::filesystem::directory_iterator(persistent_machine / "qnx")) {
                std::filesystem::copy_file(entry.path(), clone / "qnx" / entry.path().filename());
            }

            std::filesystem::copy_file(persistent_machine / "profile.json", clone / "profile.json");
            write_private_json(clone / "runtime.json", persistent_seed);
            const auto clone_bound = bind_machine_configuration(
                    app, profile, machine_identity(profile, "persistent-clone"), clone, program,
                    false, true);
            const auto clone_launch = read_private_json(clone_bound / "cluster-launch.json");
            check(clone_launch.at("persistent_storage").at("system") ==
                                  (clone / "qnx/sysla-gpt-ramdisk-v3.img").string() &&
                          clone_launch.at("persistent_storage").at("userdata") ==
                                  (clone / "qnx/userdata-qnx6.img").string(),
                  "persistent QNX clone retained source machine paths");
            put(clone / "qnx/userdata-qnx6.img", "clone-only guest writes");
            check(cache_file_identity(persistent_machine / "qnx/userdata-qnx6.img") ==
                          user_identity,
                  "persistent clone writes changed source machine userdata");
            write_private_json(persistent_machine / "runtime.json", canonical_manifest);
#endif

            if (cache_policy) {
                std::cout
                        << "public binder fresh, current and stale QNX clock cache policy guards passed; no guest launched\n";

                return 0;
            }

            write_private_json(default_cache / "android-launch.json", legacy_launch);
            write_private_json(default_cache / "activation.json", legacy_record);
            write_private_json(active_legacy_dir / "runtime.json",
                               {{"session_id", "legacy-session"}, {"stopped", false}});
            bool cluster_live_refused = false;
            try {
                bind_machine_configuration(app, profile, default_machine, canonical, program, false,
                                           true);
            } catch (const std::exception &error) {
                cluster_live_refused = std::string(error.what()).find("stop selected profile") !=
                                       std::string::npos;
            }

            check(cluster_live_refused &&
                          read_private_json(default_cache / "android-launch.json") ==
                                  legacy_launch &&
                          read_private_json(default_cache / "activation.json") == legacy_record,
                  "cluster-first migration changed a live legacy configuration");
            write_private_json(active_legacy_dir / "runtime.json",
                               {{"session_id", "legacy-session"}, {"stopped", true}});
            check(bind_machine_configuration(app, profile, default_machine, canonical, program,
                                             false, true) == default_cache,
                  "cluster-first legacy migration failed");
            std::filesystem::remove(qnx_host_bindings_path(app));
            check(bind_machine_configuration(app, profile, default_machine, canonical, program,
                                             true, false) == default_cache &&
                          read_private_json(default_cache / "android-launch.json").at("machine") ==
                                  (canonical / "android").string(),
                  "cluster-first migration reused the old Android guest on a later multimedia start");
            check(cache_file_identity(old_data / "userdata-qemu.img.qcow2") == legacy_userdata,
                  "cluster-first migration changed legacy userdata");
            const auto migrated_launch = cache_file_identity(default_cache / "android-launch.json");
            check(bind_machine_configuration(app, profile, default_machine, canonical, program,
                                             true, false) == default_cache &&
                          cache_file_identity(default_cache / "android-launch.json") ==
                                  migrated_launch,
                  "same prepared canonical seed was rebound on every start");
            std::filesystem::remove(active_legacy_file);

            // Refresh only the obsolete implicit-SD template, never while its profile is active.
            const auto launch_before = read_private_json(first / "android-launch.json"),
                       activation_before = read_private_json(first / "activation.json");
            std::map<std::string, Json> overlay_ids;

            for (const auto *name : {"system.img.qcow2", "vendor.img.qcow2",
                                     "userdata-qemu.img.qcow2", "encryptionkey.img.qcow2"}) {
                overlay_ids[name] = cache_file_identity(external / "android" / name);
            }

            const auto launch_identity = cache_file_identity(first / "android-launch.json");
            check(bind_machine_configuration(app, profile, external_machine, external, program,
                                             true, false) == first &&
                          cache_file_identity(first / "android-launch.json") == launch_identity,
                  "exact API28 no-SD configuration was rewritten");
            const std::filesystem::path config_path =
                    launch_before.at("config_template").get<std::string>();
            std::ifstream config_input(config_path);
            std::string old_config{std::istreambuf_iterator<char>(config_input), {}};
            config_input.close();
            const auto sd_line = old_config.find("hw.sdCard=no\n");
            check(sd_line != std::string::npos, "API28 no-SD template missing");
            old_config.erase(sd_line, std::string("hw.sdCard=no\n").size());
            put(config_path, old_config);
            const auto active_dir = native_session_directory(app, profile, external_machine,
                                                             "0123456789abcdef0123456789abcdef");
            ensure_private_directory(active_dir);
            write_private_json(
                    active_dir / "runtime.json",
                    {{"session_id", active_dir.filename().string()}, {"stopped", false}});
            const auto active_file = native_current_active_directory(app, profile,
                                                                     external_machine) /
                                     (profile + ".json");
            write_private_json(active_file, {{"session_id", active_dir.filename().string()},
                                             {"directory", active_dir.string()}});
            bool active_refused = false;
            try {
                bind_machine_configuration(app, profile, external_machine, external, program, true,
                                           false);
            } catch (const std::exception &error) {
                active_refused = std::string(error.what()).find("stop selected profile") !=
                                 std::string::npos;
            }

            check(active_refused &&
                          read_private_json(first / "android-launch.json") == launch_before,
                  "live profile allowed API28 configuration refresh");
            write_private_json(active_dir / "runtime.json",
                               {{"session_id", active_dir.filename().string()}, {"stopped", true}});
            check(bind_machine_configuration(app, profile, external_machine, external, program,
                                             true, false) == first,
                  "stopped API28 no-SD template refresh failed");
            const auto refreshed28 = read_private_json(first / "android-launch.json");
            std::ifstream current_config(refreshed28.at("config_template").get<std::string>());
            const std::string current_text{std::istreambuf_iterator<char>(current_config), {}};
            check(current_text.find("hw.sdCard=no\n") != std::string::npos &&
                          read_private_json(first / "activation.json") == activation_before,
                  "API28 refresh lost no-SD contract or activation provenance");
            std::filesystem::remove(active_file);

            for (const auto &[name, identity] : overlay_ids) {
                check(cache_file_identity(external / "android" / name) == identity,
                      "API28 configuration refresh replaced a writable overlay");
            }

#ifdef _WIN32
            // Windows separator/case spelling may differ while the exact ordinary directory is
            // unchanged.
            const auto activation_path = first / "activation.json";
            auto activation = read_private_json(activation_path);
            const auto original_activation = activation;
            auto spelling = external.string();

            for (auto &c : spelling) {
                if (c == '\\') {
                    c = '/';
                } else if (c >= 'a' && c <= 'z') {
                    c = char(c - 'a' + 'A');
                }
            }

            activation["seed"] = spelling;
            write_private_json(activation_path, activation);
            check(bind_machine_configuration(app, profile, external_machine, external, program,
                                             true, false) == first,
                  "same Windows directory rejected for slash/case spelling");
            const auto other_seed = fixture / "outside-host/other-seed";
            ensure_private_directory(other_seed);
            activation["seed"] = other_seed.string();
            write_private_json(activation_path, activation);
            bool other_seed_rejected = false;
            try {
                bind_machine_configuration(app, profile, external_machine, external, program, true,
                                           false);
            } catch (...) {
                other_seed_rejected = true;
            }

            check(other_seed_rejected, "different recorded seed identity accepted");
            write_private_json(activation_path, original_activation);
#endif
            put(program, "different current binary without version binding");
            check(bind_machine_configuration(app, profile, external_machine, external, program,
                                             true, false) == first,
                  "current binary update invalidated machine configuration");
            const auto moved = fixture / "outside-host/relocated-machine";
            std::filesystem::rename(external, moved);
            const auto second = bind_machine_configuration(
                    app, profile, machine_identity(profile, "external-relocated"), moved, program,
                    true, false);
            const auto moved_launch = read_private_json(second / "android-launch.json");
            check(moved_launch.at("machine") == (moved / "android").string(),
                  "relocated machine retained old data path");
            check(synthetic_phone_supported(moved_launch),
                  "relocated A9 lost embedded phone provider capability");

            for (const auto &[name, identity] : original_ids) {
                check(cache_file_identity(moved / "android" / name) == identity,
                      "relocation copied or replaced an original image");
            }

            const auto held_profiles = app / "profiles.held", held_sources = app / "src.held";
            std::filesystem::rename(app / "profiles", held_profiles);
            std::filesystem::rename(app / "src", held_sources);
            const auto independent = bind_machine_configuration(app, profile, snapshot_machine,
                                                                moved, program, true, false);
            check(synthetic_phone_supported(read_private_json(independent / "android-launch.json")),
                  "A9 without source profiles lost embedded phone capability");
            check(read_private_json(independent / "android-launch.json").at("machine") ==
                          (moved / "android").string(),
                  "external prepared machine requires source tree or shared recipe");
            write_private_json(app / "profiles" / profile / "profile.json", {{"foreign", true}});
            put(app / "src/app/android-wheel-routes.json", "invalid source routes");
            check(bind_machine_configuration(app, profile, snapshot_machine, moved, program, true,
                                             false) == independent,
                  "shared recipe/source mutation changed prepared machine");
            const auto owned_launch = read_private_json(independent / "android-launch.json");
            const auto owned_activation = read_private_json_bytes(independent / "activation.json");

            for (const char *field :
                 {"assets", "splash", "startup", "startup_video", "startup_cluster"}) {
                auto stale_source = owned_launch;
                stale_source["frontend"][field] = (app / "src/obsolete-host-artwork").string();
                write_private_json(independent / "android-launch.json", stale_source);
                check(bind_machine_configuration(app, profile, snapshot_machine, moved, program,
                                                 true, false) == independent,
                      "cached source artwork path changed named scope");
                const auto repaired = read_private_json(independent / "android-launch.json");
                check(repaired.at("frontend").at("assets") == (moved / "ui/wheel").string() &&
                              repaired.at("frontend").at("splash") ==
                                      (moved / "ui/splash.png").string() &&
                              !repaired.at("frontend").contains("startup") &&
                              !repaired.at("frontend").contains("startup_video") &&
                              !repaired.at("frontend").contains("startup_cluster") &&
                              read_private_json_bytes(independent / "activation.json") ==
                                      owned_activation,
                      "cached source artwork path survived machine-owned refresh or changed activation");

                for (const auto &[name, identity] : original_ids) {
                    check(cache_file_identity(moved / "android" / name) == identity,
                          "cached artwork path refresh changed guest image");
                }
            }

            auto stale_routes = owned_launch;
            stale_routes["wheel_adapter"]["routes"] = {{"foreign_action", 999}};
            write_private_json(independent / "android-launch.json", stale_routes);
            check(bind_machine_configuration(app, profile, snapshot_machine, moved, program, true,
                                             false) == independent &&
                          read_private_json(independent / "android-launch.json")
                                          .at("wheel_adapter")
                                          .at("routes") == Json::object(),
                  "cached wheel routes survived machine bundle refresh");

            for (const auto &[name, identity] : original_ids) {
                check(cache_file_identity(moved / "android" / name) == identity,
                      "cached route refresh changed guest image");
            }

            write_private_json(independent / "android-launch.json", stale_routes);
            std::filesystem::remove(independent / "activation.json");
            bool missing_activation = false;
            try {
                bind_machine_configuration(app, profile, snapshot_machine, moved, program, true,
                                           false);
            } catch (...) {
                missing_activation = true;
            }

            check(missing_activation &&
                          read_private_json(independent / "android-launch.json") == stale_routes,
                  "named legacy cache without activation reused source or was silently repaired");
            put(independent / "activation.json", owned_activation);
            write_private_json(independent / "android-launch.json", owned_launch);
            auto resource_refresh = read_private_json(moved / "runtime.json");
            const auto light = moved / "ui/wheel/wheel-left-light.png";
            put(light, "new optional machine artwork");
            resource_refresh["resources"]["ui/wheel/wheel-left-light.png"] = {
                    {"file", "ui/wheel/wheel-left-light.png"},
                    {"bytes", std::filesystem::file_size(light)},
                    {"sha256", sha256_file(light)}};
            write_private_json(moved / "runtime.json", resource_refresh);
            check(bind_machine_configuration(app, profile, snapshot_machine, moved, program, true,
                                             false) == independent &&
                          read_private_json(independent / "activation.json")
                                          .at("manifest_sha256") ==
                                  sha256_file(moved / "runtime.json") &&
                          read_private_json(independent / "android-launch.json").at("machine") ==
                                  (moved / "android").string(),
                  "named prepared resource refresh kept stale manifest or lost seed");

            for (const auto &[name, identity] : original_ids) {
                check(cache_file_identity(moved / "android" / name) == identity,
                      "named resource refresh changed guest image");
            }

            const auto snapshot_bytes = read_private_json_bytes(moved / "profile.json");
            std::filesystem::remove(moved / "profile.json");
            bool absent_snapshot = false;
            try {
                bind_machine_configuration(app, profile,
                                           machine_identity(profile, "missing-own-profile"), moved,
                                           program, true, false);
            } catch (...) {
                absent_snapshot = true;
            }

            check(absent_snapshot &&
                          !std::filesystem::exists(app / ".local/machines/missing-own-profile"),
                  "prepared machine borrowed global profile when own snapshot missing");
            put(moved / "profile.json", snapshot_bytes);
            std::filesystem::remove_all(app / "profiles");
            std::filesystem::remove_all(app / "src");
            std::filesystem::rename(held_profiles, app / "profiles");
            std::filesystem::rename(held_sources, app / "src");

            std::ifstream overlay(moved / "android/system.img.qcow2");
            const std::string overlay_text{std::istreambuf_iterator<char>(overlay), {}};
            check(overlay_text.find("relative-backing=system.img") != std::string::npos &&
                          overlay_text.find(external.string()) == std::string::npos,
                  "overlay retained an absolute original machine backing path");
            bool mismatch = false;
            try {
                bind_machine_configuration(app, profile, external_machine, moved, program, true,
                                           false);
            } catch (...) {
                mismatch = true;
            }

            check(mismatch, "cached configuration attached another machine path");
            const auto failing = fixture / "outside-host/tool-failure";
            ensure_private_directory(failing / "android");

            for (const auto *name :
                 {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img"}) {
                put(failing / "android" / name, std::string("preserve ") + name);
            }

            (void)resource_fixture(failing);

            for (const auto &[name, pin] : portable_clean_boot_pins(android_base(28))) {
                put(failing / "android" / name, std::string("installed boot dependency ") + name);
            }

            std::filesystem::copy_file(moved / "profile.json", failing / "profile.json");
            write_private_json(failing / "runtime.json", portable);
#ifdef _WIN32
            _putenv_s("VOYEMU_BINDER_FIXTURE_FAIL", "1");
#else
            setenv("VOYEMU_BINDER_FIXTURE_FAIL", "1", 1);
#endif
            bool failed = false;
            try {
                bind_machine_configuration(app, profile,
                                           machine_identity(profile, "external-failed"), failing,
                                           program, true, false);
            } catch (...) {
                failed = true;
            }

#ifdef _WIN32
            _putenv_s("VOYEMU_BINDER_FIXTURE_FAIL", "");
#else
            unsetenv("VOYEMU_BINDER_FIXTURE_FAIL");
#endif
            check(failed && !std::filesystem::exists(app / ".local/machines/external-failed") &&
                          !std::filesystem::exists(failing / "android/cache.img"),
                  "failed disk tool left published configuration/new cache");
            const std::string
                    free30 = "free-11-chinese-20260731-7.3.1",
                    archive30 = "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304";
            const Json descriptor30{
                    {"package", "system-images;android-30;default;arm64-v8a"},
                    {"revision", "1"},
                    {"url",
                     "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"},
                    {"sha256", archive30},
                    {"clean_base", android_base(30).at("clean_base")},
                    {"features",
                     {{"MultiDisplay", "on"},
                      {"VirtioInput", "on"},
                      {"VirtioTablet", "off"},
                      {"Vulkan", "off"}}}};
            const auto preferred_port30 = select_machine_console_port(
                    5576,
                    [](unsigned port) {
                        return tcp_port_free(port);
                    },
                    android_port_leasable);
            const Json definition30{
                    {"schema", 1},
                    {"title", "Synthetic Free 1.0"},
                    {"components",
                     {{"canbus", "components/canbus"},
                      {"qg-runtime", "components/qg-runtime"},
                      {"bootstrap", "components/bootstrap"}}},
                    {"install",
                     Json::array({{{"artifact", "component:canbus"}, {"type", "apk"}},
                                  {{"artifact", "component:qg-runtime"}, {"type", "file"}},
                                  {{"artifact", "component:bootstrap"}, {"type", "apk"}}})},
                    {"id", free30},
                    {"status", "incomplete"},
                    {"android", descriptor30},
                    {"wheel", {{"assets", "profiles/common/wheels/test"}}},
                    {"emulator",
                     {{"port", preferred_port30},
                      {"memory", 6144},
                      {"cores", 2},
                      {"gpuMode", "auto"}}},
                    {"displays", Json::array({{{"role", "primary"},
                                               {"guest", "android"},
                                               {"width", 1280},
                                               {"height", 720},
                                               {"density", 240}},
                                              {{"role", "secondary"},
                                               {"guest", "android"},
                                               {"width", 1280},
                                               {"height", 720},
                                               {"density", 240},
                                               {"flag", 1225},
                                               {"xOffset", 1280},
                                               {"yOffset", 0}},
                                              {{"role", "rear"},
                                               {"guest", "android"},
                                               {"width", 1902},
                                               {"height", 540},
                                               {"density", 160},
                                               {"xOffset", 0},
                                               {"yOffset", 720}}})},
                    {"configure",
                     {{"home", "com.qinggan.app.launcher"},
                      {"homeActivity",
                       "com.qinggan.app.launcher/com.qinggan.app.launcher.activity.MainActivity"}}}};
            const auto api30_machine = machine_identity(free30, "api30");
            const auto api30_qnx_machine = machine_identity(free30, "api30-qnx-unified");
            const auto api30_rebuilt_machine = machine_identity(free30, "api30-both-rebuilt");
            const auto profile30 = app / "profiles" / free30 / "profile.json";
            write_private_json(profile30, definition30);
            const auto clean30 = app / ".local/profiles/common/android-base" / archive30;

            for (const auto *name : {"kernel-ranchu", "ramdisk.img", "build.prop",
                                     "advancedFeatures.ini", "source.properties",
                                     "VerifiedBootParams.textproto", "system.img", "vendor.img"}) {
                put(clean30 / name, std::string("API30 dependency ") + name);
            }

            const auto seed30 = fixture / "outside-host/api30-machine";

            for (const auto &[name, pin] : portable_clean_boot_pins(android_base(30))) {
                put(seed30 / "android" / name, std::string("API30 dependency ") + name);
            }

            Json images30 = Json::object();
            std::map<std::string, Json> ids30;

            for (const auto *name :
                 {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img"}) {
                const auto file = seed30 / "android" / name;
                put(file, std::string("API30 original ") + name);
                images30[name] = {{"file", std::string("android/") + name},
                                  {"bytes", std::filesystem::file_size(file)},
                                  {"sha256", sha256_file(file)}};
                ids30[name] = cache_file_identity(file);
            }

            cache(seed30 / "android/cache.img");
            images30["cache.img"] = {{"file", "android/cache.img"},
                                     {"bytes", 69206016},
                                     {"sha256", sha256_file(seed30 / "android/cache.img")}};
            ids30["cache.img"] = cache_file_identity(seed30 / "android/cache.img");
            Json seed_metadata30{{"schema", 1},
                                 {"kind", "portable-guest-runtime"},
                                 {"profile", free30},
                                 {"profile_sha256", sha256_file(profile30)},
                                 {"scope", "android"},
                                 {"cluster_available", false},
                                 {"candidate_receipt_sha256", std::string(64, '1')},
                                 {"android",
                                  {{"api", 30},
                                   {"abi", "arm64-v8a"},
                                   {"base_archive_sha256", archive30},
                                   {"images", images30}}}};
            seed_metadata30["resources"] = resource_fixture(seed30);
            std::filesystem::copy_file(profile30, seed30 / "profile.json");
            write_private_json(seed30 / "runtime.json", seed_metadata30);
            check(inspect_portable_runtime(app, free30, api30_machine, seed30) == seed_metadata30,
                  "published API30 Android-only seed cannot be inspected");
            // Prepared machines carry no OEM control artwork; a ui manifest entry is foreign.
            {
                auto with_ui = seed_metadata30;
                with_ui["ui"] = Json::object();
                write_private_json(seed30 / "runtime.json", with_ui);
                bool denied = false;
                try {
                    inspect_portable_runtime(app, free30, api30_machine, seed30);
                } catch (...) {
                    denied = true;
                }

                check(denied, "a prepared machine with a ui artwork manifest is accepted");
                write_private_json(seed30 / "runtime.json", seed_metadata30);
            }
            // Normal start deliberately does not repeat payload checks performed by explicit check.
            seed_metadata30["android"]["images"]["system.img"]["sha256"] = std::string(64, '0');
            write_private_json(seed30 / "runtime.json", seed_metadata30);
            // Ordinary start consumes only a portable DEX and pin; no source/JDK/compiler cache
            // exists.
            bool missing_observer = false;
            try {
                bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                           false);
            } catch (const std::exception &error) {
                missing_observer =
                        std::string(error.what()).find("prepared Free 7.3.1 UI observer missing") !=
                        std::string::npos;
            }

            check(missing_observer && !std::filesystem::exists(app / ".local/machines/api30"),
                  "normal start compiled or ignored missing observer");
            const auto portable_dex = seed30 / "observer/classes.dex";
            put(portable_dex, "portable source-built observer fixture");
            seed_metadata30["ui_observer"] = {{"file", "observer/classes.dex"},
                                              {"bytes", std::filesystem::file_size(portable_dex)},
                                              {"sha256", sha256_file(portable_dex)}};
            write_private_json(seed30 / "runtime.json", seed_metadata30);
            const auto observer_cache = app / ".local/toolchains/portable-ui-observer" /
                                        sha256_file(portable_dex);
            write_private_json(seed30 / "profile.json", {{"tampered", true}});
            bool snapshot_denied = false;
            try {
                bind_machine_configuration(app, free30,
                                           machine_identity(free30, "api30-snapshot-tampered"),
                                           seed30, program, true, false);
            } catch (...) {
                snapshot_denied = true;
            }

            check(snapshot_denied, "normal binding fell back from tampered snapshot");
            std::filesystem::remove(seed30 / "profile.json");
            std::filesystem::copy_file(profile30, seed30 / "profile.json");
            std::filesystem::rename(profile30, profile30.parent_path() / "moved-profile.json");
            const auto bound30 = bind_machine_configuration(app, free30, api30_machine, seed30,
                                                            program, true, false);
            std::filesystem::rename(profile30.parent_path() / "moved-profile.json", profile30);
            const auto launch30 = read_private_json(bound30 / "android-launch.json");
            const Json observer_binding = {{"dex", (observer_cache / "classes.dex").string()},
                                           {"sha256", sha256_file(observer_cache / "classes.dex")},
                                           {"profile", free30}};
            check(launch30.at("ui_observer") == observer_binding,
                  "normal Free A11 7.3.1 start lost source-built observer binding");
            unsigned observer_pins = 0;

            for (const auto &item : launch30.at("pins")) {
                if (item.at("path") == observer_binding.at("dex")) {
                    check(item.at("sha256") == observer_binding.at("sha256"),
                          "normal observer pin differs");
                    ++observer_pins;
                }
            }

            check(observer_pins == 1, "normal observer DEX pin missing or duplicated");
            {
                auto missing = launch30;
                missing.erase("ui_observer");
                write_private_json(bound30 / "android-launch.json", missing);
                check(bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                                 false) == bound30 &&
                              read_private_json(bound30 / "android-launch.json")
                                              .at("ui_observer") == observer_binding,
                      "normal start reused missing observer cache");
            }
            {
                auto missing_pin = launch30;
                auto &pins = missing_pin["pins"];

                for (auto at = pins.begin(); at != pins.end();) {
                    if (at->at("path") == observer_binding.at("dex")) {
                        at = pins.erase(at);
                    } else {
                        ++at;
                    }
                }

                write_private_json(bound30 / "android-launch.json", missing_pin);
                bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                           false);
                check(read_private_json(bound30 / "android-launch.json").at("pins") ==
                              launch30.at("pins"),
                      "normal start reused missing observer launch pin");
            }
            put(portable_dex, "changed portable observer");
            bool stale_observer = false;
            try {
                bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                           false);
            } catch (...) {
                stale_observer = true;
            }

            check(stale_observer, "normal start silently reused changed portable DEX");
            put(portable_dex, "portable source-built observer fixture");

            for (int mode = 0; mode < 4; ++mode) {
                auto invalid = seed_metadata30;

                if (mode == 0) {
                    invalid["ui_observer"]["file"] = "../classes.dex";
                }

                if (mode == 1) {
                    invalid["ui_observer"]["sha256"] = std::string(64, '0');
                }

                if (mode == 2) {
                    invalid["ui_observer"]["bytes"] = 0;
                }

                if (mode == 3) {
                    invalid["ui_observer"]["extra"] = true;
                }

                write_private_json(seed30 / "runtime.json", invalid);
                bool denied = false;
                try {
                    bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                               false);
                } catch (...) {
                    denied = true;
                }

                check(denied, "invalid portable observer descriptor accepted");
            }

            write_private_json(seed30 / "runtime.json", seed_metadata30);
            put(observer_cache / "classes.dex", "changed host observer");
            bool changed_cache = false;
            try {
                bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                           false);
            } catch (...) {
                changed_cache = true;
            }

            check(changed_cache, "changed host observer DEX accepted");
            put(observer_cache / "classes.dex", "portable source-built observer fixture");
            check(!std::filesystem::exists(app / "src/guest/android/api") &&
                          !std::filesystem::exists(app / ".local/toolchains/ui-observer"),
                  "portable observer start required source/compiler cache");
            check(!launch30.contains("wheel_adapter") &&
                          launch30.at("wheel_capability") ==
                                  Json{{"schema", 1},
                                       {"profile", free30},
                                       {"status", "unsupported"},
                                       {"supported", false},
                                       {"reason", "provider_not_installed"},
                                       {"authority", "com.voyemu.input.wheel"}},
                  "Free graph without wheel provider borrowed A9 wheel bootstrap");
            check(launch30.at("vehicle_adapter").at("authority") == "com.voyemu.output.vehicle",
                  "Free wheel capability disabled genuine vehicle full-state publication");
            check(launch30.at("api") == 30 &&
                          launch30.at("base") == (seed30 / "android").string() &&
                          launch30.at("features") == descriptor30.at("features") &&
                          launch30.at("gpuMode") == "auto" &&
                          launch30.at("home").at("component") ==
                                  definition30.at("configure").at("homeActivity"),
                  "API30 consumer retained API28 base/features/HOME defaults");

            for (const auto *name :
                 {"kernel-ranchu", "ramdisk.img", "build.prop", "advancedFeatures.ini",
                  "source.properties", "VerifiedBootParams.textproto"}) {
                check(sha256_file(seed30 / "android" / name) == sha256_file(clean30 / name),
                      "API30 prepared sysdir lost clean boot metadata");
            }

            {
                auto stale_wheel = launch30;
                stale_wheel.erase("wheel_capability");
                stale_wheel["wheel_adapter"] = {{"authority", "com.voyemu.input.wheel"}};
                write_private_json(bound30 / "android-launch.json", stale_wheel);
                bool denied = false;
                try {
                    bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                               false);
                } catch (...) {
                    denied = true;
                }

                check(denied && read_private_json(bound30 / "android-launch.json") == stale_wheel,
                      "malformed Free cached wheel contract was silently repaired");
                write_private_json(bound30 / "android-launch.json", launch30);
            }
            {
                auto installed = definition30;
                installed["components"]["wheel-input"] = "components/wheel-input";
                installed["components"]["wheel-platform"] = "components/wheel-platform";
                installed["original"]["KeyManagerService"] = {
                        {"file", "KeyManagerService.apk"},
                        {"sha256",
                         "951abb4f86540663fd654138b7e58cd0226d5dfa0470a4f5e85384bcdedbb78e"},
                        {"source",
                         {{"path",
                           "/system/priv-app/KeyManagerService-release/KeyManagerService-release.apk"}}}};
                installed["patches"]["KeyManagerService"] = {
                        {"input", "original:KeyManagerService"},
                        {"type", "apk"},
                        {"steps",
                         Json::array({"api30-keymanager.json", "api30-key-ingress.json"})}};
                installed["install"].push_back(
                        {{"artifact", "component:wheel-input"}, {"type", "apk"}});
                installed["install"].push_back(
                        {{"artifact", "component:wheel-platform"},
                         {"type", "file"},
                         {"target", "/system/framework/voyemu-free-wheel.jar"},
                         {"mode", "0644"}});
                installed["install"].push_back(
                        {{"artifact", "patched:KeyManagerService"},
                         {"type", "apk"},
                         {"target",
                          "/system/priv-app/KeyManagerService-release/KeyManagerService-release.apk"},
                         {"package", "com.qinggan.keymanager.service"}});
                write_private_json(profile30, installed);
                write_private_json(seed30 / "profile.json", installed);
                seed_metadata30["profile_sha256"] = sha256_file(profile30);
                write_private_json(seed30 / "runtime.json", seed_metadata30);
                check(bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                                 false) == bound30,
                      "partial Free cached refresh changed scope");
                const auto partial = read_private_json(bound30 / "android-launch.json");
                const Json adapter{{"authority", "com.voyemu.input.wheel"},
                                   {"routes",
                                    {{"volume_up", 2},
                                     {"volume_down", 1},
                                     {"next", 3},
                                     {"previous", 4},
                                     {"ok", 6},
                                     {"photo", 173},
                                     {"voice", 130},
                                     {"phone", 128}}}};
                check(partial.at("wheel_adapter") == adapter &&
                              partial.at("wheel_capability") ==
                                      Json{{"schema", 1},
                                           {"profile", free30},
                                           {"status", "partial"},
                                           {"supported", true},
                                           {"authority", "com.voyemu.input.wheel"},
                                           {"routes", adapter.at("routes")}},
                      "authenticated Free graph failed to expose exact partial wheel");
                check(!qinggan11_wheel_unsupported(partial) &&
                              partial.at("vehicle_adapter") == launch30.at("vehicle_adapter"),
                      "partial Free wheel damaged genuine vehicle output");

                for (int mode = 0; mode < 5; ++mode) {
                    auto stale = partial;

                    if (mode == 0) {
                        stale.erase("wheel_adapter");
                    }

                    if (mode == 1) {
                        stale["wheel_adapter"]["routes"]["volume_up"] = 1;
                    }

                    if (mode == 2) {
                        stale["wheel_adapter"]["routes"]["menu"] = 3074;
                    }

                    if (mode == 3) {
                        stale["wheel_adapter"]["component"] = "com.voyemu.oem/.RegistrationService";
                    }

                    if (mode == 4) {
                        stale["wheel_capability"]["routes"]["custom"] = 3090;
                    }

                    bool denied = false;
                    try {
                        (void)qinggan11_wheel_unsupported(stale);
                    } catch (...) {
                        denied = true;
                    }

                    check(denied, "malformed or widened partial Free wheel accepted");
                    write_private_json(bound30 / "android-launch.json", stale);
                    denied = false;
                    try {
                        bind_machine_configuration(app, free30, api30_machine, seed30, program,
                                                   true, false);
                    } catch (...) {
                        denied = true;
                    }

                    check(denied && read_private_json(bound30 / "android-launch.json") == stale,
                          "malformed partial Free cache was silently repaired");
                    write_private_json(bound30 / "android-launch.json", partial);
                }

                installed["patches"]["KeyManagerService"]["steps"] =
                        Json::array({"api30-keymanager.json"});
                write_private_json(profile30, installed);
                write_private_json(seed30 / "profile.json", installed);
                seed_metadata30["profile_sha256"] = sha256_file(profile30);
                write_private_json(seed30 / "runtime.json", seed_metadata30);
                bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                           false);
                const auto legacy = read_private_json(bound30 / "android-launch.json");
                check(!legacy.contains("wheel_adapter") &&
                              legacy.at("wheel_capability").at("reason") ==
                                      "oem_delivery_unverified" &&
                              qinggan11_wheel_unsupported(legacy),
                      "historical single-step graph acquired partial wheel support");
                write_private_json(profile30, definition30);
                write_private_json(seed30 / "profile.json", definition30);
                seed_metadata30["profile_sha256"] = sha256_file(profile30);
                write_private_json(seed30 / "runtime.json", seed_metadata30);
                bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                           false);
            }

            // A same-path normal rebuild replaces RAW payloads, not its old host cache.
            for (const auto *name : {"machine.json"}) {
                check(std::filesystem::remove(seed30 / "android" / name),
                      "generated Android metadata fixture absent");
                check(bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                                 false) == bound30 &&
                              std::filesystem::is_regular_file(seed30 / "android" / name),
                      "same-path rebuild reused cache with missing Android metadata");

                for (const auto &[raw, identity] : ids30) {
                    check(cache_file_identity(seed30 / "android" / raw) == identity,
                          "metadata regeneration changed prepared RAW/userdata");
                }
            }

            const auto stable30 = read_private_json(bound30 / "activation.json");
            auto stale30 = launch30;
            stale30["base"] = clean30.string();
            write_private_json(bound30 / "android-launch.json", stale30);
            const auto active30 = native_current_active_directory(app, free30, api30_machine) /
                                  (free30 + ".json"),
                       session30 = native_session_directory(app, free30, api30_machine,
                                                            "live-fixture");
            write_private_json(active30,
                               {{"session_id", "live-fixture"}, {"directory", session30.string()}});
            write_private_json(session30 / "runtime.json",
                               {{"session_id", "live-fixture"}, {"stopped", false}});
            bool live_refresh = false;
            try {
                bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                           false);
            } catch (...) {
                live_refresh = true;
            }

            check(live_refresh && read_private_json(bound30 / "android-launch.json") == stale30,
                  "live API30 configuration was refreshed");
            write_private_json(session30 / "runtime.json",
                               {{"session_id", "live-fixture"}, {"stopped", true}});
            check(bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                             false) == bound30 &&
                          read_private_json(bound30 / "android-launch.json").at("base") ==
                                  (seed30 / "android").string(),
                  "cached API30 duplicate-super route was not regenerated");
            check(read_private_json(bound30 / "activation.json") == stable30,
                  "cached API30 refresh changed activation provenance");

            for (const auto &[name, identity] : ids30) {
                check(cache_file_identity(seed30 / "android" / name) == identity,
                      "API30 sysdir refresh replaced a prepared disk");
            }

            {
                std::ifstream input(launch30.at("config_template").get<std::string>());
                const std::string text{std::istreambuf_iterator<char>(input), {}};
                check(text.find("hw.sdCard=no\n") != std::string::npos &&
                              text.find("sdcard.path") == std::string::npos,
                      "API30 attached an undeclared SD card/system image");
            }
            {
                std::ifstream input(read_private_json(first / "android-launch.json")
                                            .at("config_template")
                                            .get<std::string>());
                const std::string text{std::istreambuf_iterator<char>(input), {}};
                check(text.find("hw.sdCard=no\n") != std::string::npos &&
                              text.find("sdcard.path") == std::string::npos,
                      "API28 attached an undeclared SD card/system image");
            }

            for (const auto &argument : launch30.at("args")) {
                check(argument != "-system" && argument != "-vendor" &&
                              argument != clean30.string(),
                      "API30 launch added a duplicate clean/prepared system partition");
            }

            check(launch30.at("port") == preferred_port30 &&
                          launch30.at("sdk_control").at("grpc_port") == preferred_port30 + 30000,
                  "API30 selected profile port was ignored");
            {
                std::ifstream source(launch30.at("config_template").get<std::string>());
                const std::string config{std::istreambuf_iterator<char>(source), {}};
                check(config.find("hw.display1.flag=1225\n") != std::string::npos &&
                              config.find("hw.lcd.width=1280\n") != std::string::npos,
                      "API30 profile secondary decoration flags or primary geometry were lost");
                // The rear screen is the third emulator display.
                check(config.find("hw.display2.width=1902\n") != std::string::npos &&
                              config.find("hw.display2.height=540\n") != std::string::npos &&
                              config.find("hw.display2.yOffset=720\n") != std::string::npos,
                      "API30 profile rear screen was not an emulator display");
            }

            auto current_definition30 = definition30;
            current_definition30["displays"][1]["flag"] = 1225;
            write_private_json(profile30, current_definition30);
            write_private_json(seed30 / "profile.json", current_definition30);
            seed_metadata30["profile_sha256"] = sha256_file(profile30);
            write_private_json(seed30 / "runtime.json", seed_metadata30);
            check(bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                             false) == bound30 &&
                          read_private_json(bound30 / "activation.json").at("manifest_sha256") ==
                                  sha256_file(seed30 / "runtime.json"),
                  "API30 config-only manifest refresh retained stale activation");
            const auto refreshed30 = read_private_json(bound30 / "android-launch.json");
            check(refreshed30.at("base") == (seed30 / "android").string(),
                  "config refresh lost prepared sysdir");
            auto race_launch30 = refreshed30;
            race_launch30["base"] = clean30.string();
            write_private_json(bound30 / "android-launch.json", race_launch30);
            std::filesystem::remove(seed30 / "android/userdata-qemu.img.qcow2");
            const auto foreign_path = (bound30 / "android-launch.json").string();
#ifdef _WIN32
            _putenv_s("VOYEMU_BINDER_FIXTURE_REPLACE", foreign_path.c_str());
#else
            setenv("VOYEMU_BINDER_FIXTURE_REPLACE", foreign_path.c_str(), 1);
#endif
            bool foreign_refresh = false;
            try {
                bind_machine_configuration(app, free30, api30_machine, seed30, program, true,
                                           false);
            } catch (...) {
                foreign_refresh = true;
            }

#ifdef _WIN32
            _putenv_s("VOYEMU_BINDER_FIXTURE_REPLACE", "");
#else
            unsetenv("VOYEMU_BINDER_FIXTURE_REPLACE");
#endif
            check(foreign_refresh &&
                          read_private_json(bound30 / "android-launch.json") ==
                                  Json{{"foreign", true}} &&
                          !std::filesystem::exists(seed30 / "android/userdata-qemu.img.qcow2"),
                  "API30 work-time foreign configuration was overwritten or owned partial disk retained");

            for (const auto &[name, identity] : ids30) {
                check(cache_file_identity(seed30 / "android" / name) == identity,
                      "failed API30 CAS changed prepared disk");
            }

            write_private_json(bound30 / "android-launch.json", refreshed30);
            const auto explicit_port30 = select_machine_console_port(
                    5580,
                    [&](unsigned port) {
                        return port != preferred_port30 && tcp_port_free(port);
                    },
                    android_port_leasable);
            const auto explicit_bindings = qnx_host_bindings_path(app);
            write_private_json(explicit_bindings, {{"android_port", explicit_port30}});
            const auto explicit_scope = bind_machine_configuration(
                    app, free30, machine_identity(free30, "api30-explicit-port"), seed30, program,
                    true, false);
            const auto explicit_launch30 =
                    read_private_json(explicit_scope / "android-launch.json");
            check(explicit_launch30.at("port") == explicit_port30 &&
                          explicit_launch30.at("sdk_control").at("grpc_port") ==
                                  explicit_port30 + 30000,
                  "explicit native host port lost precedence over profile port");
            std::filesystem::remove(explicit_bindings);
            write_private_json(profile30, definition30);
            write_private_json(seed30 / "profile.json", definition30);
            seed_metadata30["profile_sha256"] = sha256_file(profile30);
            write_private_json(seed30 / "runtime.json", seed_metadata30);
            // Free cluster binding uses the public current program and all ten roles without
            // payload audits.
            auto full30 = seed_metadata30;
            full30.erase("scope");
            full30.erase("cluster_available");
            full30.erase("candidate_receipt_sha256");
            Json cluster30 = {{"guest_arch", "aarch64"}, {"images", Json::object()}};
            const auto free_layout = qnx_runtime_layout("sa8155-qinggan11-731");
            qnx_runtime_metadata(cluster30, free_layout);

            for (const auto &name : free_layout.images) {
                put(seed30 / "qnx" / name, "Free QNX source fixture " + name);
                cluster30["images"][name] = {
                        {"file", "qnx/" + name},
                        {"bytes", std::filesystem::file_size(seed30 / "qnx" / name)},
                        {"sha256", std::string(64, '0')}};
            }

            full30["qnx"] = cluster30;
            write_private_json(seed30 / "runtime.json", full30);
            const auto angle = app / ".local/angle";

            for (const auto *name : {"libEGL", "libGLESv2"}) {
                put(angle / (name + std::string(os::shared_library_suffix)), "ANGLE fixture");
            }

            Json qnx_host = {{"qnx_qemu", installed.qnx_qemu.string()}, {"angle", angle.string()}};
#ifdef _WIN32
            qnx_host["qnx_file_backend"] = {{"schema", 1},
                                            {"version", "11.1.2"},
                                            {"engine_sha256", sha256_file(installed.qnx_qemu)}};
#endif
            write_private_json(explicit_bindings, qnx_host);
            const auto qnx_bound = bind_machine_configuration(app, free30, api30_qnx_machine,
                                                              seed30, program, false, true);
            const auto qnx_launch = read_private_json(qnx_bound / "cluster-launch.json");
            check(qnx_launch.at("graphics").at("executable") == program.string() &&
                          qnx_launch.at("graphics").at("backend") == "voyemu" &&
                          qnx_launch.at("mcu_projection") == free_layout.projection,
                  "Free cluster did not use unified graphics/projection");
            unsigned primary = 0, backup = 0, ifs = 0;

            for (const auto &value : qnx_launch.at("qemu").at("args")) {
                const auto arg = value.get<std::string>();

                if (arg.find("gpt-primary.bin,addr=0xaffc0000") != std::string::npos) {
                    ++primary;
                }

                if (arg.find("gpt-backup.bin,addr=0xbffbe000") != std::string::npos) {
                    ++backup;
                }

                if (arg.find("ifs2-native.img,addr=") != std::string::npos) {
                    ++ifs;
                }
            }

            check(primary == 1 && backup == 1 && ifs == 2,
                  "Free exact two GPT loaders and shared IFS A/B missing");
            auto calibration30 = full30;
            const auto initial30 =
                    qnx_qinggan11_simulated_initial_state(Json{{"eps_calibrationFlag", 1}});
            calibration30["qnx"]["simulated_initial_native_state"] = initial30;
            write_private_json(seed30 / "runtime.json", calibration30);
            check(bind_machine_configuration(app, free30, api30_qnx_machine, seed30, program, false,
                                             true) == qnx_bound &&
                          read_private_json(qnx_bound / "cluster-launch.json")
                                          .at("simulated_initial_native_state") == initial30,
                  "cached Free cluster lost optional native fixture refresh");
            const auto both30 = bind_machine_configuration(app, free30, api30_rebuilt_machine,
                                                           seed30, program, true, true);
            std::filesystem::remove(seed30 / "android/machine.json");
            std::filesystem::remove(seed30 / "android/config.ini");
            calibration30["qnx"]["simulated_initial_native_state"] =
                    qnx_qinggan11_simulated_initial_state(Json{{"eps_calibrationFlag", 2}});
            write_private_json(seed30 / "runtime.json", calibration30);
            check(bind_machine_configuration(app, free30, api30_rebuilt_machine, seed30, program,
                                             true, true) == both30 &&
                          std::filesystem::is_regular_file(seed30 / "android/machine.json") &&
                          !std::filesystem::exists(seed30 / "android/config.ini") &&
                          read_private_json(both30 / "cluster-launch.json")
                                          .at("simulated_initial_native_state") ==
                                  calibration30.at("qnx").at("simulated_initial_native_state"),
                  "combined same-path rebuild failed atomic Android/cluster metadata refresh");

            for (const auto &[raw, identity] : ids30) {
                check(cache_file_identity(seed30 / "android" / raw) == identity,
                      "combined metadata refresh changed RAW/userdata");
            }

            write_private_json(seed30 / "runtime.json", full30);
            bind_machine_configuration(app, free30, api30_qnx_machine, seed30, program, false,
                                       true);
            check(!read_private_json(qnx_bound / "cluster-launch.json")
                           .contains("simulated_initial_native_state"),
                  "legacy cluster without optional fixture lost compatibility");
            const auto before_qnx_cache = read_private_json(qnx_bound / "cluster-launch.json");

            for (const bool missing : {true, false}) {
                auto bad = full30;

                if (missing) {
                    bad["qnx"]["images"].erase("gpt-primary.bin");
                } else {
                    bad["qnx"]["images"]["unknown.bin"] = bad["qnx"]["images"].at("smem.bin");
                }

                write_private_json(seed30 / "runtime.json", bad);
                bool rejected = false;
                try {
                    bind_machine_configuration(app, free30, api30_qnx_machine, seed30, program,
                                               false, true);
                } catch (...) {
                    rejected = true;
                }

                check(rejected && read_private_json(qnx_bound / "cluster-launch.json") ==
                                          before_qnx_cache,
                      "cached Free accepted missing/unknown role or rewrote launch");
            }

            write_private_json(seed30 / "runtime.json", full30);
            std::filesystem::remove(explicit_bindings);
            write_private_json(seed30 / "runtime.json", seed_metadata30);
            auto foreign_family = definition30;
            foreign_family["configure"]["home"] = "com.voyah.os.carlauncher";
            bool family_denied = false;
            try {
                portable_android_launch(app, PortableRuntimeHostBindings{}, seed30 / "android",
                                        clean30, bound30 / "config-template.ini", "foreign-family",
                                        Json::array(), foreign_family);
            } catch (const std::exception &error) {
                family_denied =
                        std::string(error.what()).find("own native vehicle/display adapter") !=
                        std::string::npos;
            }

            check(family_denied,
                  "API30 non-Free/Dreamer profile silently inherited Free/Dreamer adapters");
            auto nxp_definition = definition30;
            nxp_definition["id"] = nxp_profile_id;
            nxp_definition["platform"] = "qinggan09";
            nxp_definition["board"] = "nxp";
            nxp_definition["android"] = {
                    {"package", "system-images;android-28;default;arm64-v8a"},
                    {"revision", "1"},
                    {"url",
                     "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip"},
                    {"sha256", "86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9"},
                    {"clean_base", android_base(28).at("clean_base")}};
            nxp_definition["configure"] = {{"home", "com.qinggan.app.launcher"},
                                           {"homeActivity", nxp_home}};
            const auto nxp_launch = portable_android_launch(
                    app, PortableRuntimeHostBindings{}, seed30 / "android", clean30,
                    bound30 / "config-template.ini", "nxp", Json::array(), nxp_definition);
            check(nxp_presentation(nxp_launch) && !passion_presentation(nxp_launch),
                  "NXP inherited another presentation family");
            check(!nxp_launch.contains("vehicle_adapter") &&
                          !nxp_launch.contains("systemui_adapter") &&
                          !nxp_launch.contains("wheel_adapter"),
                  "NXP inherited A9 CAN, wheel or SystemUI support");
            {
                // The declared platform and board select the presentation, not the id.
                auto declared = nxp_definition;
                declared["id"] = "synthetic-nxp";
                declared["platform"] = "qinggan09";
                declared["board"] = "nxp";
                declared["title"] = "Synthetic NXP 1.0";
                const auto launch = portable_android_launch(
                        app, PortableRuntimeHostBindings{}, seed30 / "android", clean30,
                        bound30 / "config-template.ini", "synthetic-nxp", Json::array(), declared);
                check(nxp_presentation(launch) && !passion_presentation(launch) &&
                              launch.at("presentation_adapter").at("family") == "synthetic-nxp" &&
                              launch.at("platform") == "qinggan09" && launch.at("board") == "nxp",
                      "declared NXP platform and board did not select the NXP presentation");
                check(launch.at("profile_title") == "Synthetic NXP 1.0" &&
                              nxp_launch.at("profile_title") == "Synthetic Free 1.0",
                      "the window does not name the machine by the title of its profile");
                auto untitled = declared;
                untitled.erase("title");
                bool denied = false;
                try {
                    portable_android_launch(app, PortableRuntimeHostBindings{}, seed30 / "android",
                                            clean30, bound30 / "config-template.ini",
                                            "synthetic-untitled", Json::array(), untitled);
                } catch (const VoyemuError &error) {
                    denied = std::string(error.what()).starts_with("PROFILE_INVALID: ");
                }

                check(denied, "a profile without a title named the window by its id");
            }
            {
                auto wheel_definition = nxp_definition;
                wheel_definition["components"]["wheel"] = "components/wheel";
                wheel_definition["wheel"]["androidRoutes"] = "nxp-wheel-routes.json";
                Json metadata{{"profile", nxp_profile_id}, {"routes", Json::array()}};
                const auto nxp_routes = nxp_wheel_routes();

                for (const auto &[id, code] : nxp_routes.items()) {
                    metadata["routes"].push_back({{"id", id}, {"qg_code", code}});
                }

                write_private_json(app / "nxp-wheel-routes.json", metadata);
                const auto wheel_launch = portable_android_launch(
                        app, PortableRuntimeHostBindings{}, seed30 / "android", clean30,
                        bound30 / "config-template.ini", "nxp-wheel", Json::array(),
                        wheel_definition);
                check(nxp_presentation(wheel_launch) &&
                              wheel_launch.at("wheel_adapter") == nxp_wheel_adapter(),
                      "NXP wheel component did not bind exact OEM routes");
                metadata["routes"][0]["qg_code"] = 173;
                write_private_json(app / "nxp-wheel-routes.json", metadata);
                bool denied = false;
                try {
                    portable_android_launch(app, PortableRuntimeHostBindings{}, seed30 / "android",
                                            clean30, bound30 / "config-template.ini",
                                            "nxp-bad-wheel", Json::array(), wheel_definition);
                } catch (...) {
                    denied = true;
                }

                check(denied, "NXP accepted foreign A9 wheel metadata");
            }
            auto passion_definition = definition30;
            passion_definition["id"] = passion_profile_id;
            passion_definition["platform"] = "passion";
            passion_definition["board"] = "8155";
            passion_definition["configure"] = {{"home", "com.voyah.os.carlauncher"},
                                               {"homeActivity", passion_home}};
            passion_definition["displays"] = Json::array(
                    {{{"role", "primary"},
                      {"guest", "android"},
                      {"width", 1920},
                      {"height", 720},
                      {"density", 160}},
                     {{"role", "secondary"},
                      {"guest", "android"},
                      {"width", 1920},
                      {"height", 720},
                      {"density", 160}},
                     {{"role", "hud"}, {"guest", "qnx"}, {"width", 1280}, {"height", 720}}});
            passion_definition["emulator"]["cores"] = 4;
#ifndef _WIN32
            auto passion_host = PortableRuntimeHostBindings{};
            passion_host.android_port = 5590;
            const auto passion_launch = portable_android_launch(
                    app, passion_host, seed30 / "android", clean30, bound30 / "config-template.ini",
                    "passion", Json::array(), passion_definition);
            check(passion_presentation(passion_launch) &&
                          passion_launch.at("sdk_control").at("grpc_port") == 35590,
                  "Passion presentation routing lost machine isolation");
            check(!passion_launch.contains("wheel_adapter") &&
                          !passion_launch.contains("vehicle_adapter") &&
                          !passion_launch.contains("systemui_adapter"),
                  "Passion borrowed Free/Dreamer contracts");

            for (const auto key : {"api", "home", "presentation_adapter"}) {
                auto invalid = passion_launch;
                invalid[key] = nullptr;
                bool denied = false;
                try {
                    passion_presentation(invalid);
                } catch (...) {
                    denied = true;
                }

                check(denied, "invalid Passion presentation contract accepted");
            }

            {
                auto invalid = passion_launch;
                invalid["wheel_adapter"] = Json::object();
                bool denied = false;
                try {
                    passion_presentation(invalid);
                } catch (...) {
                    denied = true;
                }

                check(denied, "Passion acquired fabricated wheel support");
            }
            {
                auto partial = passion_launch;
                partial["wheel_capability"] = passion_wheel_partial_capability();
                partial["wheel_adapter"] = {{"authority", "com.voyemu.input.wheel"},
                                            {"routes", passion_wheel_routes()}};
                partial["presentation_adapter"] = passion_presentation_contract(
                        passion_launch.at("reference_profile").get<std::string>(), false, true);
                check(passion_presentation(partial),
                      "Passion exact prepared partial wheel capability rejected");

                for (int mode = 0; mode < 4; ++mode) {
                    auto bad = partial;

                    if (mode == 0) {
                        bad["wheel_capability"]["profile"] = "foreign";
                    }

                    if (mode == 1) {
                        bad["wheel_capability"]["routes"]["ok"] = 6;
                    }

                    if (mode == 2) {
                        bad["wheel_adapter"]["authority"] = "foreign";
                    }

                    if (mode == 3) {
                        bad["presentation_adapter"]["wheel"] = "unsupported";
                    }

                    bool denied = false;
                    try {
                        passion_presentation(bad);
                    } catch (...) {
                        denied = true;
                    }

                    check(denied, "Passion accepted foreign or contradictory wheel capability");
                }

                auto legacy = passion_launch;
                legacy["presentation_adapter"]["wheel"] = "android-media-keys";
                bool denied = false;
                try {
                    passion_presentation(legacy);
                } catch (...) {
                    denied = true;
                }

                check(denied, "Legacy Passion advertised absent Android wheel transport");
            }
            {
                auto partial = passion_launch;
                partial["wheel_capability"] = passion_wheel_partial_capability();
                partial["wheel_adapter"] = {{"authority", "com.voyemu.input.wheel"},
                                            {"routes", passion_wheel_routes()}};
                partial["mcu_property_adapter"] = passion_mcu_capability();
                partial["presentation_adapter"] = passion_presentation_contract(
                        passion_launch.at("reference_profile").get<std::string>(), true, true);
                check(passion_presentation(partial),
                      "Passion MCU and Android wheel capabilities must coexist");
            }

            for (const auto *field : {"width", "height", "density"}) {
                auto invalid = passion_definition;
                invalid["displays"][0][field] = 1;
                bool denied = false;
                try {
                    portable_android_launch(app, passion_host, seed30 / "android", clean30,
                                            bound30 / "config-template.ini", "bad-geometry",
                                            Json::array(), invalid);
                } catch (...) {
                    denied = true;
                }

                check(denied, "wrong Passion geometry accepted");
            }
#else
            {
                bool denied = false;
                try {
                    portable_android_launch(app, PortableRuntimeHostBindings{}, seed30 / "android",
                                            clean30, bound30 / "config-template.ini",
                                            "passion-windows", Json::array(), passion_definition);
                } catch (const std::exception &e) {
                    denied = std::string(e.what()).find("Windows presentation startup") !=
                             std::string::npos;
                }

                check(denied, "unsupported Passion Windows FSM silently reused Free/Dreamer");
            }
#endif
            check(!passion_presentation(launch30), "Free API30 behavior changed");
            {
                auto permanent = definition30;
                permanent["configure"]["diagnosticAudio"] = "wav";
                bool permanent_denied = false;
                std::string permanent_reason;
                try {
                    portable_android_launch(app, PortableRuntimeHostBindings{}, seed30 / "android",
                                            clean30, bound30 / "config-template.ini",
                                            "permanent-wav", Json::array(), permanent);
                } catch (const std::exception &error) {
                    permanent_reason = error.what();
                    permanent_denied =
                            permanent_reason.find(
                                    "permanent profile diagnosticAudio policy rejected") !=
                            std::string::npos;
                }

                check(permanent_denied, "permanent configure.diagnosticAudio must fail closed");
                (void)permanent_reason;
                check(!launch30.contains("diagnostic_audio"),
                      "default launch never carries diagnostic_audio");
#ifdef __APPLE__
                {
                    const auto activation_directory = clean30 / "audio-scope";
                    std::filesystem::create_directories(activation_directory);
                    const auto activation = activation_directory / "audio-diagnostic.json";
                    std::filesystem::remove(activation);
                    write_private_json(activation,
                                       Json{{"schema", 1},
                                            {"route", "wav"},
                                            {"profile", "passion-11-russian-20260403-2.1.4.80"}});
                    check(audio_diagnostic_activation(activation_directory).at("route") == "wav",
                          "record must authorize the exact wav route");
                    write_private_json(
                            activation,
                            Json{{"schema", 1},
                                 {"route", "wav"},
                                 {"profile", "free-11-chinese-20260731-7.3.1"},
                                 {"nonce", std::string(32, 'a')},
                                 {"directory",
                                  std::filesystem::canonical(activation_directory).string()}});
                    {
                        Json old = {{"schema", 1},
                                    {"route", "wav"},
                                    {"profile", "free-11-chinese-20250710-6.11.1"},
                                    {"nonce", std::string(32, 'c')},
                                    {"directory",
                                     std::filesystem::canonical(activation_directory).string()}};
                        write_private_json(activation, old);
                        check(audio_diagnostic_activation(activation_directory) == old,
                              "611 scoped audio activation required");
                        old["nonce"] = "bad";
                        write_private_json(activation, old);
                        bool denied = false;
                        try {
                            audio_diagnostic_activation(activation_directory);
                        } catch (...) {
                            denied = true;
                        }

                        check(denied, "611 invalid nonce must be rejected");
                    }
                    {
                        const Json nxp_audio = {
                                {"schema", 1},
                                {"route", "wav"},
                                {"profile", nxp_profile_id},
                                {"nonce", std::string(32, 'b')},
                                {"directory",
                                 std::filesystem::canonical(activation_directory).string()}};
                        write_private_json(activation, nxp_audio);
                        const auto nxp_scoped = portable_android_launch(
                                app, PortableRuntimeHostBindings{}, seed30 / "android", clean30,
                                bound30 / "config-template.ini", "nxp-scoped-wav", Json::array(),
                                nxp_definition, audio_diagnostic_activation(activation_directory));
                        check(nxp_scoped.at("diagnostic_audio") == "wav" &&
                                      nxp_scoped.at("api") == 28,
                              "NXP scoped WAV must preserve its API28 runtime");

                        for (const auto &field : {"nonce", "directory"}) {
                            auto bad = nxp_audio;
                            bad[field] = "invalid";
                            write_private_json(activation, bad);
                            bool denied = false;
                            try {
                                audio_diagnostic_activation(activation_directory);
                            } catch (...) {
                                denied = true;
                            }

                            check(denied, "NXP WAV requires owned directory and nonce");
                        }

                        check(!nxp_launch.contains("diagnostic_audio"),
                              "ordinary NXP launch must preserve its playback route");
                        write_private_json(
                                activation,
                                Json{{"schema", 1},
                                     {"route", "wav"},
                                     {"profile", qinggan11_731_profile_id},
                                     {"nonce", std::string(32, 'a')},
                                     {"directory",
                                      std::filesystem::canonical(activation_directory).string()}});
                    }
                    // The launch assembly honors exactly the record the caller proved it owns; the
                    // assembly never reads ambient state.
                    {
                        const auto qinggan09_definition = read_private_json(
                                project / "profiles" / qinggan09_profile() / "profile.json");
                        check(!qinggan09_definition.contains("id"),
                              "canonical A9 fixture unexpectedly has an id");
                        const Json activation_qinggan09 = {
                                {"schema", 1},
                                {"route", "wav"},
                                {"profile", qinggan09_profile()},
                                {"nonce", std::string(32, 'c')},
                                {"directory",
                                 std::filesystem::canonical(activation_directory).string()}};
                        write_private_json(activation, activation_qinggan09);
                        const auto activated = audio_diagnostic_activation(activation_directory);
                        const auto captured = portable_android_launch(
                                project, PortableRuntimeHostBindings{}, seed30 / "android", clean30,
                                bound30 / "config-template.ini", "qinggan09-owned-wav",
                                Json::array(), qinggan09_definition, activated,
                                qinggan09_profile());
                        check(captured.at("diagnostic_audio") == "wav" &&
                                      captured.at("api") == 28 &&
                                      captured.at("audioOutput") == true,
                              "canonical NOid A9 owned WAV route rejected");
                        check(!portable_android_launch(
                                       project, PortableRuntimeHostBindings{}, seed30 / "android",
                                       clean30, bound30 / "config-template.ini",
                                       "qinggan09-ordinary", Json::array(), qinggan09_definition,
                                       Json::object(), qinggan09_profile())
                                       .contains("diagnostic_audio"),
                              "ordinary A9 launch acquired WAV route");

                        for (int variant = 0; variant < 4; ++variant) {
                            auto selected = qinggan09_definition;
                            auto capability = activated;
                            std::string verified(qinggan09_profile());

                            if (variant == 0) {
                                verified = "foreign-profile";
                            }

                            if (variant == 1) {
                                selected["id"] = "foreign-profile";
                            }

                            if (variant == 2) {
                                capability["profile"] = nxp_profile_id;
                            }

                            if (variant == 3) {
                                verified.clear();
                            }

                            bool denied = false;
                            try {
                                portable_android_launch(project, PortableRuntimeHostBindings{},
                                                        seed30 / "android", clean30,
                                                        bound30 / "config-template.ini",
                                                        "qinggan09-denied", Json::array(), selected,
                                                        capability, verified);
                            } catch (...) {
                                denied = true;
                            }

                            check(denied,
                                  "A9 WAV accepted unpaired profile or missing verified identity");
                        }

                        for (const auto &field : {"nonce", "directory"}) {
                            auto bad = activation_qinggan09;
                            bad[field] = "invalid";
                            write_private_json(activation, bad);
                            bool denied = false;
                            try {
                                audio_diagnostic_activation(activation_directory);
                            } catch (...) {
                                denied = true;
                            }

                            check(denied, "A9 WAV accepted invalid owned activation");
                        }

                        write_private_json(
                                activation,
                                Json{{"schema", 1},
                                     {"route", "wav"},
                                     {"profile", qinggan11_731_profile_id},
                                     {"nonce", std::string(32, 'a')},
                                     {"directory",
                                      std::filesystem::canonical(activation_directory).string()}});
                    }
                    const auto scoped = portable_android_launch(
                            app, PortableRuntimeHostBindings{}, seed30 / "android", clean30,
                            bound30 / "config-template.ini", "scoped-wav", Json::array(),
                            definition30, audio_diagnostic_activation(activation_directory));
                    check(scoped.at("diagnostic_audio") == "wav",
                          "scoped owned activation must reach the launch specification");
                    {
                        auto wrong = audio_diagnostic_activation(activation_directory);
                        wrong["profile"] = "foreign-api30";
                        bool denied = false;
                        try {
                            portable_android_launch(app, PortableRuntimeHostBindings{},
                                                    seed30 / "android", clean30,
                                                    bound30 / "config-template.ini", "foreign-wav",
                                                    Json::array(), definition30, wrong);
                        } catch (...) {
                            denied = true;
                        }

                        check(denied, "audio activation must match selected profile");
                    }
                    {
                        const auto good = audio_diagnostic_activation(activation_directory);

                        for (const auto &field : {"nonce", "directory"}) {
                            auto bad = good;
                            bad[field] = "invalid";
                            write_private_json(activation, bad);
                            bool denied = false;
                            try {
                                audio_diagnostic_activation(activation_directory);
                            } catch (...) {
                                denied = true;
                            }

                            check(denied, "Free WAV requires owned directory and nonce");
                        }

                        write_private_json(activation, good);
                    }
                    // Capture is a per-run choice: with no activation record the identical
                    // profile/seed emits no diagnostic_audio field at all, so ordinary playback
                    // keeps the normal host backend.
                    const auto ordinary_launch = portable_android_launch(
                            app, PortableRuntimeHostBindings{}, seed30 / "android", clean30,
                            bound30 / "config-template.ini", "ordinary-after", Json::array(),
                            definition30);
                    check(!ordinary_launch.contains("diagnostic_audio"),
                          "capture must not persist into ordinary launches");
                    // A foreign profile or non-wav route in the record can never authorize capture.
                    write_private_json(
                            activation,
                            Json{{"schema", 1}, {"route", "wav"}, {"profile", "foreign-api30"}});
                    bool foreign_denied = false;
                    try {
                        audio_diagnostic_activation(activation_directory);
                    } catch (const std::exception &error) {
                        foreign_denied = std::string(error.what()).find("exact wav route") !=
                                         std::string::npos;
                    }

                    check(foreign_denied, "foreign activation record must be refused");
                    write_private_json(activation,
                                       Json{{"schema", 1},
                                            {"route", "dsound"},
                                            {"profile", "passion-11-russian-20260403-2.1.4.80"}});
                    bool route_denied = false;
                    try {
                        audio_diagnostic_activation(activation_directory);
                    } catch (const std::exception &error) {
                        route_denied = std::string(error.what()).find("exact wav route") !=
                                       std::string::npos;
                    }

                    check(route_denied, "non-wav activation route must be refused");
                    write_private_json(activation,
                                       Json{{"schema", 2},
                                            {"route", "wav"},
                                            {"profile", "passion-11-russian-20260403-2.1.4.80"}});
                    bool schema_denied = false;
                    try {
                        audio_diagnostic_activation(activation_directory);
                    } catch (const std::exception &error) {
                        schema_denied = true;
                    }

                    check(schema_denied, "wrong schema activation record must be refused");
                    std::filesystem::remove(activation);
                    check(audio_diagnostic_activation(activation_directory).empty(),
                          "removed record must authorize nothing");
                    check(audio_diagnostic_activation(clean30).empty(),
                          "ordinary configured scope without a record authorizes nothing");
                }
#endif
            }
            auto missing_home = definition30;
            missing_home.erase("configure");
            bool home_denied = false;
            try {
                portable_android_launch(app, PortableRuntimeHostBindings{}, seed30 / "android",
                                        clean30, bound30 / "config-template.ini", "missing-home",
                                        Json::array(), missing_home);
            } catch (const std::exception &error) {
                home_denied =
                        std::string(error.what()).find("HOME package and activity metadata") !=
                        std::string::npos;
            }

            check(home_denied, "API30 missing HOME metadata silently inherited A9 defaults");
            std::ifstream config30(bound30 / "config-template.ini");
            const std::string config_text30{std::istreambuf_iterator<char>(config30), {}};

            for (const auto *expected :
                 {"hw.ramSize=6144\n", "hw.cpu.ncore=2\n", "hw.lcd.width=1280\n",
                  "hw.display1.xOffset=1280\n", "hw.display1.density=240\n"}) {
                check(config_text30.find(expected) != std::string::npos,
                      "API30 geometry/CPU configuration ignored selected profile");
            }

            for (const auto &[name, identity] : ids30) {
                check(cache_file_identity(seed30 / "android" / name) == identity,
                      "API30 binding copied or modified an original");
            }

            bool denied30 = false;
            try {
                inspect_portable_runtime(app, free30, api30_machine, seed30);
            } catch (...) {
                denied30 = true;
            }

            check(denied30, "explicit API30 check skipped altered payload pin");
            denied30 = false;
            try {
                bind_machine_configuration(app, free30, machine_identity(free30, "api30-cluster"),
                                           seed30, program, true, true);
            } catch (...) {
                denied30 = true;
            }

            check(denied30 && !std::filesystem::exists(app / ".local/machines/api30-cluster"),
                  "Android-only API30 seed acquired a cluster");
            auto wrong_names30 = seed_metadata30;
            put(seed30 / "android/other-system.img", "foreign automatic-system-name fixture");
            wrong_names30["android"]["images"]["system.img"]["file"] = "android/other-system.img";
            write_private_json(seed30 / "runtime.json", wrong_names30);
            bool wrong_name30 = false;
            try {
                bind_machine_configuration(app, free30,
                                           machine_identity(free30, "api30-wrong-name"), seed30,
                                           program, true, false);
            } catch (...) {
                wrong_name30 = true;
            }

            check(wrong_name30 &&
                          !std::filesystem::exists(app / ".local/machines/api30-wrong-name"),
                  "API30 sysdir accepted a different automatic system filename");
            write_private_json(seed30 / "runtime.json", seed_metadata30);
            seed_metadata30["android"]["api"] = 28;
            write_private_json(seed30 / "runtime.json", seed_metadata30);
            denied30 = false;
            try {
                bind_machine_configuration(app, free30, machine_identity(free30, "api30-wrong-api"),
                                           seed30, program, true, false);
            } catch (...) {
                denied30 = true;
            }

            check(denied30 && !std::filesystem::exists(app / ".local/machines/api30-wrong-api"),
                  "API/base metadata mismatch was accepted");
            seed_metadata30["android"]["api"] = 30;
            seed_metadata30["android"]["base_archive_sha256"] = archive;
            write_private_json(seed30 / "runtime.json", seed_metadata30);
            denied30 = false;
            try {
                bind_machine_configuration(app, free30,
                                           machine_identity(free30, "api30-wrong-base"), seed30,
                                           program, true, false);
            } catch (...) {
                denied30 = true;
            }

            check(denied30, "API30 seed silently used the A9 dependency");
        }
        check(!portable_runtime_available(root, profile, default_machine),
              "missing manifest discovery must be read-only");
        check(!std::filesystem::exists(root / ".local"), "discovery created directories");
        bool rejected = false;
        try {
            portable_runtime_available(root, "../foreign", machine_identity("../foreign", ""));
        } catch (...) {
            rejected = true;
        }

        check(rejected, "unsafe profile rejected");
        PortableRuntimeHostBindings sdk_only;
        const auto sdk_control = portable_sdk_control_binding(root, sdk_only);
        check(sdk_control.at("grpc_port") == 35574 &&
                      std::filesystem::path(sdk_control.at("discovery_root").get<std::string>())
                              .is_absolute(),
              "SDK control incorrectly requires a frontend");
        sdk_only.frontend = {{"grpc_port", 45992},
                             {"discovery_root", (root / ".local/sdk-discovery").string()}};
        check(portable_sdk_control_binding(root, sdk_only).at("grpc_port") == 45992,
              "frontend and test media control diverge");

        for (const auto port : {0, 5574, 5575, 65536}) {
            sdk_only.frontend["grpc_port"] = port;
            bool refused = false;
            try {
                portable_sdk_control_binding(root, sdk_only);
            } catch (...) {
                refused = true;
            }

            check(refused, "invalid/colliding SDK control port accepted");
        }

        write_private_json(root / "src/app/android-wheel-routes.json",
                           {{"routes", Json::array({{{"id", "volume-up"}, {"qg_code", 7}}})}});
        PortableRuntimeHostBindings launch_host;
        launch_host.sdk = root / "sdk";
        launch_host.frontend = {{"grpc_port", 45992}};
        const auto launch_machine = root / "writable/machine",
                   launch_base = root / "immutable/base",
                   launch_config = launch_machine / "config-template.ini";
        const Json launch_pins =
                Json::array({{{"path", launch_config.string()}, {"sha256", std::string(64, 'a')}}});
        const auto assembled = portable_android_launch(
                root, launch_host, launch_machine, launch_base, launch_config, "voyemu-fixture",
                launch_pins, Json{{"title", "Synthetic Free 1.0"}});
        check(assembled.at("machine") == launch_machine.string() &&
                      assembled.at("base") == launch_base.string() &&
                      assembled.at("config_template") == launch_config.string() &&
                      assembled.at("pins") == launch_pins,
              "shared launch builder changed owned paths or pins");
        check(assembled.at("audioOutput") == true && assembled.at("headless") == true &&
                      assembled.at("sdk_control").at("grpc_port") == 45992 &&
                      assembled.at("wheel_adapter").at("routes").at("volume-up") == 7,
              "shared launch builder lost production audio/frontend/control policy");
#ifdef _WIN32
        check(assembled.at("rendererPolicy") == "windows-angle-36.3.10" &&
                      assembled.at("args").at(0) == "-accel" && assembled.at("args").at(1) == "off",
              "shared launch builder lost Windows timer/renderer policy");
#endif
        check(!std::filesystem::exists(launch_machine) && !std::filesystem::exists(launch_base),
              "launch assembly mutated guest images");
        const auto definition = root / "profiles" / profile / "profile.json";
        write_private_json(
                definition,
                {{"title", "Synthetic Free 1.0"},
                 {"platform", "qinggan09"},
                 {"board", "8155"},
                 {"android",
                  {{"package", "system-images;android-28;default;arm64-v8a"},
                   {"revision", "1"},
                   {"url",
                    "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip"},
                   {"sha256", "86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9"},
                   {"clean_base", android_base(28).at("clean_base")}}}});
        const auto seed = root / "machines" / profile;
        Json images = Json::object();

        // Tiny synthetic bytes test the transport/pin contract, not a bootable raw disk.
        for (const auto *name :
             {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img"}) {
            const auto relative = "android/" + std::string(name);
            const auto bytes = std::string("fixture:") + name;
            put(seed / relative, bytes);
            images[name] = {
                    {"file", relative}, {"bytes", bytes.size()}, {"sha256", sha256_bytes(bytes)}};
        }

        cache(seed / "android/cache.img");
        images["cache.img"] = {{"file", "android/cache.img"},
                               {"bytes", 69206016},
                               {"sha256", sha256_file(seed / "android/cache.img")}};
        Json valid = {{"schema", 1},
                      {"kind", "portable-guest-runtime"},
                      {"profile", profile},
                      {"profile_sha256", sha256_file(definition)},
                      {"android",
                       {{"base_archive_sha256",
                         "86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9"},
                        {"api", 28},
                        {"abi", "arm64-v8a"},
                        {"images", images}}}};
        valid["resources"] = resource_fixture(seed);
        std::filesystem::copy_file(definition, seed / "profile.json");
        write_private_json(seed / "runtime.json", valid);
        check(portable_runtime_available(root, profile, default_machine),
              "ordinary manifest discovery");
        check(inspect_portable_runtime(root, profile, default_machine) == valid,
              "valid pinned relative seed inspection");
        {
            const std::string artifact = "component:can-model-observer",
                              target = "/system/bin/voyemu-can-model-observer", helper(64, 'c');
            const Json row = {{"artifact", artifact}, {"target", target}, {"mode", "0755"},
                              {"mutable", false},     {"package", ""},    {"sha256", helper}};
            const Json cold_check = {{"id", "artifact:" + artifact + ":" + target},
                                     {"passed", true},
                                     {"expected", helper},
                                     {"actual", helper}};
            const Json proof = {
                    {"schema", 1},
                    {"kind", "android-candidate"},
                    {"profile", profile},
                    {"profile_sha256", valid.at("profile_sha256")},
                    {"stopped", true},
                    {"prepared_VM", true},
                    {"cold_boot_verified", true},
                    {"old_machine_modified", false},
                    {"android",
                     {{"images",
                       {{"system.img", {{"sha256", images.at("system.img").at("sha256")}}}}}}},
                    {"installed", Json::array({row})},
                    {"checks", Json::array({cold_check})}};
            const auto file = seed / "can-model/candidate.json";
            put(file, proof.dump());
            auto bound = valid;
            bound["android"]["can_model_receipt"] = {{"file", "can-model/candidate.json"},
                                                     {"sha256", sha256_file(file)},
                                                     {"bytes", std::filesystem::file_size(file)}};
            write_private_json(seed / "runtime.json", bound);
            check(inspect_portable_runtime(root, profile, default_machine) == bound,
                  "sealed CAN receipt rejected");
            const auto author = root / "profiles" / profile / "profile.json",
                       hidden = author.parent_path() / "can-model-hidden.json";
            std::filesystem::rename(author, hidden);
            check(inspect_portable_runtime(root, profile, default_machine) == bound,
                  "CAN receipt depends on authoring profile");
            std::filesystem::rename(hidden, author);
            const auto reject_binding = [&](const Json &changed) {
                write_private_json(seed / "runtime.json", changed);
                bool denied = false;
                try {
                    (void)inspect_portable_runtime(root, profile, default_machine);
                } catch (...) {
                    denied = true;
                }

                check(denied, "invalid CAN receipt binding accepted");
            };
            auto bad = bound;
            bad["android"]["can_model_receipt"]["sha256"] = std::string(64, '0');
            reject_binding(bad);
            bad = bound;
            bad["android"]["can_model_receipt"]["bytes"] = std::filesystem::file_size(file) + 1;
            reject_binding(bad);
            bad = bound;
            bad["android"]["can_model_receipt"]["file"] = "other/candidate.json";
            reject_binding(bad);
            auto wrong = proof;
            wrong["android"]["images"]["system.img"]["sha256"] = std::string(64, 'd');
            put(file, wrong.dump());
            bad = bound;
            bad["android"]["can_model_receipt"]["sha256"] = sha256_file(file);
            bad["android"]["can_model_receipt"]["bytes"] = std::filesystem::file_size(file);
            reject_binding(bad);
            put(file, proof.dump());
            write_private_json(seed / "runtime.json", bound);
            put(file, "tampered");
            bool denied = false;
            try {
                (void)inspect_portable_runtime(root, profile, default_machine);
            } catch (...) {
                denied = true;
            }

            check(denied, "tampered sealed CAN receipt accepted");
            std::filesystem::remove(file);
            write_private_json(seed / "runtime.json", valid);
        }

        const auto source_profile = root / "profiles" / profile / "profile.json";
        const auto saved_profile = read_private_json(source_profile, 1 << 20);
        write_private_json(source_profile, {{"changed", true}});
        check(inspect_portable_runtime(root, profile, default_machine) == valid,
              "prepared snapshot depends on changed source profile");
        std::filesystem::rename(source_profile,
                                source_profile.parent_path() / "moved-profile.json");
        check(inspect_portable_runtime(root, profile, default_machine) == valid,
              "prepared snapshot depends on moved source profile");
        write_private_json(source_profile, saved_profile);
        write_private_json(seed / "profile.json", {{"tampered", true}});
        rejected = false;
        try {
            inspect_portable_runtime(root, profile, default_machine);
        } catch (...) {
            rejected = true;
        }

        check(rejected, "tampered snapshot fell back to matching source profile");
        std::filesystem::remove(seed / "profile.json");
        std::filesystem::remove(source_profile.parent_path() / "moved-profile.json");
        rejected = false;
        try {
            inspect_portable_runtime(root, profile, default_machine);
        } catch (...) {
            rejected = true;
        }

        check(rejected, "missing machine snapshot fell back to global recipe");
        write_private_json(seed / "profile.json", saved_profile);

        auto uncached = valid;
        uncached["android"]["images"].erase("cache.img");
        write_private_json(seed / "runtime.json", uncached);
        rejected = false;
        try {
            inspect_portable_runtime(root, profile, default_machine);
        } catch (...) {
            rejected = true;
        }

        check(rejected, "seed without the prepared cache accepted");
        write_private_json(seed / "runtime.json", valid);
        check(!std::filesystem::exists(root / ".local/machines"),
              "inspection must not activate or launch");
        const std::string variant = profile + "-copy";
        const auto variant_machine = machine_identity(profile, variant);
        const auto variant_seed = root / "machines" / variant;

        for (const auto *name :
             {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img"}) {
            put(variant_seed / "android" / name, std::string("fixture:") + name);
        }

        cache(variant_seed / "android/cache.img");
        (void)resource_fixture(variant_seed);
        write_private_json(variant_seed / "profile.json", saved_profile);
        write_private_json(variant_seed / "runtime.json", valid);
        check(inspect_portable_runtime(root, profile, variant_machine) == valid,
              "explicit machine preserves canonical profile");
        check(!portable_runtime_available(root, profile,
                                          machine_identity(profile, "missing-variant")),
              "missing explicit machine never falls back");
        rejected = false;
        try {
            inspect_portable_runtime(root, "another-car", variant_machine);
        } catch (...) {
            rejected = true;
        }

        check(rejected, "wrong canonical profile rejected");
        rejected = false;
        try {
            inspect_portable_runtime(root, profile, machine_identity(profile, "../foreign"));
        } catch (...) {
            rejected = true;
        }

        check(rejected, "machine traversal rejected");
        auto variant_changed = valid;
        variant_changed["profile"] = "another-car";
        write_private_json(variant_seed / "runtime.json", variant_changed);
        rejected = false;
        try {
            inspect_portable_runtime(root, profile, variant_machine);
        } catch (...) {
            rejected = true;
        }

        check(rejected, "changed selected manifest refused");
        write_private_json(variant_seed / "runtime.json", valid);
#ifndef _WIN32
        std::filesystem::create_directory_symlink(variant_seed, root / "machines/linked-variant");
        check(!portable_runtime_available(root, profile,
                                          machine_identity(profile, "linked-variant")),
              "linked selected machine refused");
#endif

        const auto runtime_inventory = [&] {
            std::map<std::string, Json> entries;
            const auto runtime = root / ".local/machines";

            if (!std::filesystem::exists(runtime)) {
                return entries;
            }

            entries["."] = {{"directory", true}};

            for (const auto &entry : std::filesystem::recursive_directory_iterator(runtime)) {
                auto value = Json{{"directory", entry.is_directory()}};

                if (entry.is_regular_file()) {
                    value["identity"] = cache_file_identity(entry.path());
                    value["sha256"] = sha256_file(entry.path());
                }

                entries[entry.path().lexically_relative(runtime).generic_string()] = value;
            }

            return entries;
        };
        auto refuse = [&](const Json &input) {
            write_private_json(seed / "runtime.json", input);
            const auto before = runtime_inventory();
            bool failed = false;
            try {
                inspect_portable_runtime(root, profile, default_machine);
            } catch (...) {
                failed = true;
            }

            check(failed, "invalid portable manifest accepted");
            check(runtime_inventory() == before, "bad manifest mutated runtime");
        };
        auto bad = valid;
        bad["profile_sha256"] = std::string(64, '0');
        refuse(bad);
        bad = valid;
        bad["profile"] = "another-car";
        refuse(bad);
        bad = valid;
        bad["android"]["images"]["system.img"]["file"] = "/foreign/system.img";
        refuse(bad);
        bad = valid;
        bad["android"]["images"]["system.img"]["file"] = "android/../system.img";
        refuse(bad);
        bad = valid;
        bad["android"]["images"]["system.img"]["bytes"] = 999;
        refuse(bad);
        bad = valid;
        bad["android"]["images"]["cache.img"] = {
                {"file", "android/cache.img"}, {"sha256", std::string(64, '0')}, {"bytes", 1}};
        refuse(bad);
        bad = valid;
        bad["android"]["images"].erase("cache.img");
        refuse(bad);
        {
            std::fstream output(seed / "android/cache.img",
                                std::ios::in | std::ios::out | std::ios::binary);
            output.seekp(1080);
            output.put(0);
        }
        bad = valid;
        bad["android"]["images"]["cache.img"]["sha256"] = sha256_file(seed / "android/cache.img");
        refuse(bad);
        cache(seed / "android/cache.img");
        bad = valid;
        bad["host_args"] = Json::array({"-accel", "hvf"});
        refuse(bad);
        bad = valid;
        bad["snapshots"] = Json::object();
        refuse(bad);
        bad = valid;
        bad["android"]["base_archive_sha256"] = std::string(64, '0');
        refuse(bad);
        bad = valid;
        bad["qnx"] = {{"guest_arch", "aarch64"}, {"images", Json::object()}};
        refuse(bad);
        write_private_json(seed / "runtime.json", valid);
        put(seed / "android/system.img", "changed");
        refuse(valid);
        put(seed / "android/system.img", "fixture:system.img");
        write_private_json(seed / "runtime.json", valid);
        PortableRuntimeHostBindings host;
        host.sdk = root / "missing-sdk";
        host.clean_base = root / "missing-base";
        rejected = false;
        try {
            activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (...) {
            rejected = true;
        }

        check(rejected && !std::filesystem::exists(root / ".local/machines"),
              "missing dependencies cannot create activation");
        auto explicit_android = valid;
        explicit_android["scope"] = "android";
        explicit_android["cluster_available"] = false;
        explicit_android["candidate_receipt_sha256"] = std::string(64, '1');
        write_private_json(seed / "runtime.json", explicit_android);
        check(inspect_portable_runtime(root, profile, default_machine) == explicit_android,
              "explicit Android-only seed rejected");
        rejected = false;
        try {
            activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (const std::exception &e) {
            rejected = std::string(e.what()).find("cluster unavailable") != std::string::npos;
        }

        check(rejected && !std::filesystem::exists(root / ".local/machines"),
              "Android-only cluster request mutated runtime");
        bad = explicit_android;
        bad["cluster_available"] = true;
        refuse(bad);
        bad = explicit_android;
        bad["scope"] = "full";
        refuse(bad);
        bad = explicit_android;
        bad["candidate_receipt_sha256"] = "bad";
        refuse(bad);
        write_private_json(seed / "runtime.json", valid);
        Json cached_launch = {{"schema", 1}};
#ifdef _WIN32
        cached_launch["rendererPolicy"] = "windows-angle-36.3.10";
#endif
        const auto cached_bytes = cached_launch.dump();
        const auto scoped = root / ".local/machines" / profile;
        ensure_private_directory(scoped);
        put(scoped / "android-launch.json", cached_bytes);
        rejected = false;
        try {
            activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (...) {
            rejected = true;
        }

        check(rejected && sha256_file(scoped / "android-launch.json") == sha256_bytes(cached_bytes),
              "existing foreign scoped launch preserved");
        auto full = valid;
        Json qnx_images = Json::object();

        for (const auto *name : {"hyp-hvf.img", "ifs2-native.img", "host-native-lzo.bin",
                                 "smem.bin", "qnx-ifs2-restore.bin", "synthetic-gpt.bin",
                                 "sysla-gpt-ramdisk-v3.img", "qnx-ipl-virt.bin"}) {
            const auto relative = "qnx/" + std::string(name);
            const auto bytes = std::string("fixture:") + name;
            put(seed / relative, bytes);
            qnx_images[name] = {
                    {"file", relative}, {"bytes", bytes.size()}, {"sha256", sha256_bytes(bytes)}};
        }

        full["qnx"] = {{"guest_arch", "aarch64"}, {"images", qnx_images}};
        write_private_json(seed / "runtime.json", full);
        {
            auto installed = full;
            installed["qnx"]["persistence"] = {{"schema", 1},
                                               {"system_image", "sysla-gpt-ramdisk-v3.img"},
                                               {"userdata_image", "userdata-qnx6.img"}};
            put(seed / "qnx/userdata-qnx6.img", "factory userdata state");
            installed["qnx"]["images"]["userdata-qnx6.img"] = {
                    {"file", "qnx/userdata-qnx6.img"},
                    {"bytes", std::filesystem::file_size(seed / "qnx/userdata-qnx6.img")},
                    {"sha256", sha256_file(seed / "qnx/userdata-qnx6.img")}};
            write_private_json(seed / "runtime.json", installed);

            for (const auto *name : {"sysla-gpt-ramdisk-v3.img", "userdata-qnx6.img"}) {
                std::fstream file(seed / "qnx" / name,
                                  std::ios::in | std::ios::out | std::ios::binary);
                file.put('G');
                check(bool(file), "persistent guest mutation fixture failed");
            }

            check(inspect_portable_runtime(root, profile, default_machine) == installed,
                  "installed persistence check compared mutable disks with factory hashes");
            const auto source_system = cache_file_identity(seed / "qnx/sysla-gpt-ramdisk-v3.img"),
                       source_userdata = cache_file_identity(seed / "qnx/userdata-qnx6.img");
            const auto system_sha = sha256_file(seed / "qnx/sysla-gpt-ramdisk-v3.img"),
                       userdata_sha = sha256_file(seed / "qnx/userdata-qnx6.img");
            const auto copied = root / "machines/persistent-copy";
            std::filesystem::copy(seed, copied, std::filesystem::copy_options::recursive);
            check(inspect_portable_runtime(root, profile, copied_machine) == installed,
                  "copied installed machine failed mutable-state check");

            for (const auto *name : {"sysla-gpt-ramdisk-v3.img", "userdata-qnx6.img"}) {
                check(!std::filesystem::equivalent(seed / "qnx" / name, copied / "qnx" / name) &&
                              std::filesystem::hard_link_count(copied / "qnx" / name) == 1 &&
                              sha256_file(copied / "qnx" / name) ==
                                      sha256_file(seed / "qnx" / name),
                      "machine directory copy reset persistent bytes or shared writable inode");
            }

            for (const auto *name : {"sysla-gpt-ramdisk-v3.img", "userdata-qnx6.img"}) {
                std::fstream file(copied / "qnx" / name,
                                  std::ios::in | std::ios::out | std::ios::binary);
                file.put('C');
                check(bool(file), "cloned guest mutation fixture failed");
            }

            check(inspect_portable_runtime(root, profile, copied_machine) == installed &&
                          cache_file_identity(seed / "qnx/sysla-gpt-ramdisk-v3.img") ==
                                  source_system &&
                          cache_file_identity(seed / "qnx/userdata-qnx6.img") == source_userdata &&
                          sha256_file(seed / "qnx/sysla-gpt-ramdisk-v3.img") == system_sha &&
                          sha256_file(seed / "qnx/userdata-qnx6.img") == userdata_sha,
                  "copied persistent guest writes changed source machine");
            {
                std::fstream file(seed / "qnx/smem.bin",
                                  std::ios::in | std::ios::out | std::ios::binary);
                file.put('X');
            }
            refuse(installed);
            put(seed / "qnx/smem.bin", "fixture:smem.bin");
            std::filesystem::resize_file(seed / "qnx/userdata-qnx6.img", 1);
            refuse(installed);
            put(seed / "qnx/sysla-gpt-ramdisk-v3.img", "fixture:sysla-gpt-ramdisk-v3.img");
            write_private_json(seed / "runtime.json", full);
        }
        const std::string host_name(os::dependency_host);
        const auto recorded_runtime = root / ".local/machines/retired-portable/fixture";
        ensure_private_directory(recorded_runtime);
        write_private_json(scoped / "activation.json",
                           {{"schema", 1},
                            {"activation_version", 5},
                            {"manifest_sha256", sha256_file(seed / "runtime.json")},
                            {"profile", profile},
                            {"host", host_name},
                            {"runtime", recorded_runtime.string()},
                            {"android_launch_sha256", sha256_file(scoped / "android-launch.json")},
                            {"cluster_configured", false}});
        const auto current_record = read_private_json(scoped / "activation.json");
        auto old_record = current_record;
        old_record.erase("activation_version");
        write_private_json(scoped / "activation.json", old_record);
        host.require_cluster = false;
        rejected = false;
        try {
            activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (const std::exception &e) {
            rejected = std::string(e.what()).find("activation version") != std::string::npos;
        }

        check(rejected && sha256_file(scoped / "android-launch.json") == sha256_bytes(cached_bytes),
              "obsolete activation silently reused or changed");
        write_private_json(scoped / "activation.json", current_record);
#ifdef _WIN32
        for (const Json policy : {Json{}, Json("tampered")}) {
            auto altered = cached_launch;

            if (policy.is_null()) {
                altered.erase("rendererPolicy");
            } else {
                altered["rendererPolicy"] = policy;
            }

            write_private_json(scoped / "android-launch.json", altered);
            auto altered_record = current_record;
            altered_record["android_launch_sha256"] = sha256_file(scoped / "android-launch.json");
            write_private_json(scoped / "activation.json", altered_record);
            bool denied = false;
            try {
                activate_portable_runtime(root, profile, default_machine, host, 0);
            } catch (const std::exception &error) {
                denied = std::string(error.what()).find("ANDROID_RENDERER_POLICY") !=
                         std::string::npos;
            }

            check(denied, "missing/tampered normal renderer policy accepted");
        }

        put(scoped / "android-launch.json", cached_bytes);
        write_private_json(scoped / "activation.json", current_record);
#endif
        host.require_cluster = false;
        const auto android_only = activate_portable_runtime(root, profile, default_machine, host,
                                                            0);
        check(android_only.at("cached") == true && android_only.at("cluster_requested") == false &&
                      android_only.at("cluster_seed_present") == true,
              "explicit Android-only activation lost full QNX seed/status");

        const auto variant_scoped = root / ".local/machines" / variant;
        put(variant_scoped / "android-launch.json", cached_bytes);
        auto variant_record = current_record;
        variant_record["machine"] = variant;
        variant_record["seed"] = variant_seed.string();
        variant_record["manifest_sha256"] = sha256_file(variant_seed / "runtime.json");
        variant_record["android_launch_sha256"] =
                sha256_file(variant_scoped / "android-launch.json");
        write_private_json(variant_scoped / "activation.json", variant_record);
        check(activate_portable_runtime(root, profile, variant_machine, host, 0).at("machine") ==
                      variant,
              "selected cached activation does not reuse canonical launch");
        auto bad_selection = variant_record;
        bad_selection["machine"] = "another-machine";
        write_private_json(variant_scoped / "activation.json", bad_selection);
        rejected = false;
        try {
            activate_portable_runtime(root, profile, variant_machine, host, 0);
        } catch (...) {
            rejected = true;
        }

        check(rejected, "wrong cached selected machine identity accepted");
        write_private_json(variant_scoped / "activation.json", variant_record);
        auto changed_manifest = valid;
        changed_manifest["profile_sha256"] = std::string(64, '0');
        write_private_json(variant_seed / "runtime.json", changed_manifest);
        rejected = false;
        try {
            activate_portable_runtime(root, profile, variant_machine, host, 0);
        } catch (...) {
            rejected = true;
        }

        check(rejected, "changed source bypassed selected cache preflight");
        write_private_json(variant_seed / "runtime.json", valid);
        check(sha256_file(scoped / "android-launch.json") == sha256_bytes(cached_bytes),
              "selected activation changed base launch");
        write_private_json(scoped / "android-launch.json", cached_launch);
        const auto observer_launch_sha = sha256_file(scoped / "android-launch.json");
        auto observer_record = current_record;
        observer_record["android_launch_sha256"] = observer_launch_sha;
        write_private_json(scoped / "activation.json", observer_record);
        host.ui_observer = {{"dex", (root / ".local/missing-observer.dex").string()},
                            {"sha256", std::string(64, '1')}};
        rejected = false;
        std::string observer_error;
        try {
            activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (const std::exception &e) {
            observer_error = e.what();
            rejected = observer_error.find("observer binding differs") != std::string::npos;
        }

        if (!rejected) {
            throw std::runtime_error("stale observer launch refusal missing; actual exception: " +
                                     observer_error);
        }

        check(sha256_file(scoped / "android-launch.json") == observer_launch_sha,
              "stale observer launch rewritten");
        host.ui_observer = Json::object();
        put(scoped / "android-launch.json", cached_bytes);
        write_private_json(scoped / "activation.json", current_record);

        const auto old_dex = root / ".local/observer-old.dex",
                   new_dex = root / ".local/observer-new.dex";
        put(old_dex, "old observer");
        put(new_dex, "new observer");
        const auto writable = recorded_runtime / "userdata-qemu.img";
        put(writable, "installed user data");
        {
            const std::string qinggan11_731 = "free-11-chinese-20260731-7.3.1";
            const auto qinggan11_machine = machine_identity(qinggan11_731, "");
            const auto free_definition = root / "profiles" / qinggan11_731 / "profile.json",
                       free_scoped = root / ".local/machines" / qinggan11_731;
            const Json free_android = {
                    {"package", "system-images;android-30;default;arm64-v8a"},
                    {"revision", "1"},
                    {"url",
                     "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"},
                    {"sha256", "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304"},
                    {"clean_base", android_base(30).at("clean_base")}};
            write_private_json(free_definition, {{"id", qinggan11_731},
                                                 {"title", "Synthetic Free 1.0"},
                                                 {"android", free_android}});
            auto free_manifest = valid;
            free_manifest["profile"] = qinggan11_731;
            free_manifest["profile_sha256"] = sha256_file(free_definition);
            free_manifest["android"]["api"] = 30;
            free_manifest["android"]["base_archive_sha256"] = free_android.at("sha256");
            write_private_json(seed / "profile.json", read_private_json(free_definition));
            write_private_json(seed / "runtime.json", free_manifest);
            Json missing = {{"schema", 1},
                            {"pins", Json::array({{{"path", old_dex.string()},
                                                   {"sha256", sha256_file(old_dex)}}})}};
#ifdef _WIN32
            missing["rendererPolicy"] = "windows-angle-36.3.10";
#endif
            write_private_json(free_scoped / "android-launch.json", missing);
            auto free_record = current_record;
            free_record["profile"] = qinggan11_731;
            free_record["manifest_sha256"] = sha256_file(seed / "runtime.json");
            free_record["android_launch_sha256"] = sha256_file(free_scoped / "android-launch.json");
            write_private_json(free_scoped / "activation.json", free_record);
            const auto saved_seed = host.seed;
            host.seed = seed;
            host.refresh_host = true;
            host.ui_observer = {{"dex", new_dex.string()}, {"sha256", sha256_file(new_dex)}};
            host.pins[new_dex.string()] = sha256_file(new_dex);
            auto initial_refused = [&] {
                bool denied = false;
                try {
                    activate_portable_runtime(root, qinggan11_731, qinggan11_machine, host, 0);
                } catch (...) {
                    denied = true;
                }

                check(denied && read_private_json(free_scoped / "activation.json") == free_record,
                      "initial observer rejection mutated activation");
            };
            host.ui_observer["profile"] = qinggan11_731;
            initial_refused(); // An existing launch without an observer never gains one.
            host.seed = saved_seed;
            host.refresh_host = false;
            host.ui_observer = Json::object();
            write_private_json(seed / "profile.json", saved_profile);
            write_private_json(seed / "runtime.json", full);
        }

        Json refresh_launch = {
                {"schema", 1},
                {"ui_observer", {{"dex", old_dex.string()}, {"sha256", sha256_file(old_dex)}}},
                {"pins",
                 Json::array({{{"path", old_dex.string()}, {"sha256", sha256_file(old_dex)}}})}};
#ifdef _WIN32
        refresh_launch["rendererPolicy"] = "windows-angle-36.3.10";
#endif
        write_private_json(scoped / "android-launch.json", refresh_launch);
        auto refresh_record = current_record;
        refresh_record["android_launch_sha256"] = sha256_file(scoped / "android-launch.json");
        write_private_json(scoped / "activation.json", refresh_record);
        host.ui_observer = {{"dex", new_dex.string()}, {"sha256", sha256_file(new_dex)}};
        host.pins[new_dex.string()] = sha256_file(new_dex);
        rejected = false;
        try {
            activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (...) {
            rejected = true;
        }

        check(rejected, "refresh was silently enabled");
        host.refresh_host = true;
        put(old_dex, "changed old observer");
        rejected = false;
        try {
            activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (...) {
            rejected = true;
        }

        check(rejected && read_private_json(scoped / "activation.json") == refresh_record,
              "changed previous observer accepted");
        put(old_dex, "old observer");
        host.pins[new_dex.string()] = std::string(64, '0');
        rejected = false;
        try {
            activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (...) {
            rejected = true;
        }

        check(rejected && read_private_json(scoped / "activation.json") == refresh_record,
              "wrong new observer pin accepted");
        host.pins[new_dex.string()] = sha256_file(new_dex);

        const auto refresh_lock = scoped.parent_path() / (profile + ".activation.lock");
        ensure_private_directory(refresh_lock);
        rejected = false;
        try {
            activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (...) {
            rejected = true;
        }

        check(rejected && read_private_json(scoped / "activation.json") == refresh_record,
              "concurrent activation lock accepted");
        std::filesystem::remove(refresh_lock);
        const auto foreign_dex = fixture / "foreign-observer.dex";
        put(foreign_dex, "foreign");
        host.ui_observer = {{"dex", foreign_dex.string()}, {"sha256", sha256_file(foreign_dex)}};
        host.pins[foreign_dex.string()] = sha256_file(foreign_dex);
        rejected = false;
        try {
            activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (...) {
            rejected = true;
        }

        check(rejected && read_private_json(scoped / "activation.json") == refresh_record,
              "outside project observer accepted");
        host.ui_observer = {{"dex", new_dex.string()}, {"sha256", sha256_file(new_dex)}};
        const auto refreshed = activate_portable_runtime(root, profile, default_machine, host, 0);
        check(refreshed.at("host_refreshed") == true &&
                      read_private_json(scoped / "android-launch.json").at("ui_observer") ==
                              host.ui_observer,
              "explicit observer refresh failed");
        check(sha256_file(writable) == sha256_bytes("installed user data"),
              "refresh modified writable disk");
        check(read_private_json(scoped / "activation.json").at("android_launch_sha256") ==
                      sha256_file(scoped / "android-launch.json"),
              "refresh record hash not committed");
        check(activate_portable_runtime(root, profile, default_machine, host, 0).at("cached") ==
                      true,
              "refreshed binding not reusable");
#if defined(_WIN32) || defined(__APPLE__)
        {
#ifdef _WIN32
            const std::string program_suffix = ".exe", library_suffix = ".dll";
#else
            const std::string program_suffix, library_suffix = ".dylib";
#endif
            const auto observer_only_launch = read_private_json(scoped / "android-launch.json"),
                       observer_only_record = read_private_json(scoped / "activation.json");
            const auto program = root / ("voyemu" + program_suffix),
                       asset = root / "frontend-asset.png",
                       machine = recorded_runtime / "android-machine",
                       disk = machine / "userdata-qemu.img";
            put(program, "old root program");
            put(asset, "unchanged asset");
            put(disk, "retained writable user data");
            const Json old_frontend{
                    {"schema", 1},
                    {"executable", program.string()},
                    {"grpc_port", 45997},
                    {"assets", root.string()},
                    {"pins",
                     Json::array({{{"path", program.string()}, {"sha256", sha256_file(program)}},
                                  {{"path", asset.string()}, {"sha256", sha256_file(asset)}}})}};
            const Json windows_marker{{"schema", 1}, {"qnx_qemu", "windows-shared-engine"}};
            const Json old_host{{"schema", 1},
                                {"frontend", old_frontend},
                                {"immutable_sdk_marker", "unchanged"}};
            write_private_json(qnx_host_bindings_path(root), old_host);
            host.refresh_source = qnx_host_binding_view(root, old_host);
            const auto sdk_pin = root / "sdk-unchanged.exe";
            put(sdk_pin, "unchanged SDK bytes");
            auto previous = observer_only_launch;
            previous["pins"].push_back(
                    {{"path", sdk_pin.string()}, {"sha256", sha256_file(sdk_pin)}});
            previous["frontend"] = old_frontend;
            previous["machine"] = machine.string();
            write_private_json(scoped / "android-launch.json", previous);
            write_private_json_bytes(scoped / "android-launch.json", previous.dump(2) + "\n");
            write_private_json_bytes(qnx_host_bindings_path(root), old_host.dump(2) + "\n");
            auto previous_record = observer_only_record;
            previous_record["android_launch_sha256"] = sha256_file(scoped / "android-launch.json");
            write_private_json(scoped / "activation.json", previous_record);
            put(program, "new current root program");
            host.frontend = old_frontend;
            host.frontend["pins"][0]["sha256"] = sha256_file(program);
            host.pins[program.string()] = sha256_file(program);
            const auto expected_frontend = host.frontend;
            const auto disk_identity = cache_file_identity(disk);
            const auto disk_sha = sha256_file(disk);
            auto refuse_refresh = [&] {
                bool denied = false;
                try {
                    activate_portable_runtime(root, profile, default_machine, host, 0);
                } catch (...) {
                    denied = true;
                }

                check(denied && read_private_json(scoped / "activation.json") == previous_record &&
                              read_private_json(scoped / "android-launch.json") == previous,
                      "frontend refresh refusal changed activation");
            };
            host.refresh_host = false;
            refuse_refresh();
            host.refresh_host = true;
            host.frontend["grpc_port"] = 45998;
            refuse_refresh();
            host.frontend = expected_frontend;
            host.frontend["executable"] = (root / "foreign-voyemu.exe").string();
            refuse_refresh();
            host.frontend = expected_frontend;
            put(sdk_pin, "changed SDK bytes");
            refuse_refresh();
            put(sdk_pin, "unchanged SDK bytes");
            host.frontend["pins"][1]["sha256"] = std::string(64, '0');
            refuse_refresh();
            host.frontend = expected_frontend;
            host.pins[program.string()] = std::string(64, '0');
            refuse_refresh();
            host.pins[program.string()] = sha256_file(program);
            put(new_dex, "tampered old observer");
            refuse_refresh();
            put(new_dex, "new observer");
            const auto refreshed_frontend = activate_portable_runtime(root, profile,
                                                                      default_machine, host, 0);
            check(refreshed_frontend.at("host_refreshed") == true &&
                          read_private_json(scoped / "android-launch.json").at("frontend") ==
                                  expected_frontend,
                  "explicit current frontend pin refresh missing");
            check(cache_file_identity(disk) == disk_identity && sha256_file(disk) == disk_sha,
                  "frontend refresh changed writable image identity/bytes");
            check(activate_portable_runtime(root, profile, default_machine, host, 0).at("cached") ==
                          true,
                  "frontend refresh not reusable");
            auto expected_host = old_host;
            expected_host["frontend"] = expected_frontend;
            const auto refreshed_host = read_private_json(qnx_host_bindings_path(root));
            check(qnx_host_binding_view(root, refreshed_host) == expected_host &&
                          refreshed_host.at("immutable_sdk_marker") == "unchanged",
                  "explicit host source pin not refreshed atomically");
            // The refresh edits the platform view, so a host-specific section is covered by the
            // sectioned cases below.
            {
                // The portable top-level frontend is optional: a shared document that carries only
                // a host section still refreshes as its own CAS subject.
                const Json section_only{{"schema", 1}, {"windows", windows_marker}};
                host.refresh_source = qnx_host_binding_view(root, section_only);
                write_private_json(qnx_host_bindings_path(root), section_only);
                write_private_json(scoped / "android-launch.json", previous);
                auto section_record = observer_only_record;
                section_record["android_launch_sha256"] =
                        sha256_file(scoped / "android-launch.json");
                write_private_json(scoped / "activation.json", section_record);
                check(activate_portable_runtime(root, profile, default_machine, host, 0)
                                      .value("host_refreshed", false) &&
                              read_private_json(qnx_host_bindings_path(root)) == section_only,
                      "host section without a portable frontend was not refreshed");
                write_private_json(scoped / "android-launch.json", previous);
                write_private_json(scoped / "activation.json", previous_record);
            }
            {
                const auto qemu = root / ("qemu-system-aarch64" + program_suffix),
                           angle = root / "angle",
                           qnx_disk = recorded_runtime / "qnx/host-native-lzo.bin";
                put(qemu, "unchanged QNX engine");
                put(angle / ("libEGL" + library_suffix), "unchanged EGL");
                put(angle / ("libGLESv2" + library_suffix), "unchanged GLES");
                put(qnx_disk, "unchanged QNX guest image");
                Json cluster_pins = Json::array({old_frontend.at("pins").at(0)});

                for (const auto &file : {qemu, angle / ("libEGL" + library_suffix),
                                         angle / ("libGLESv2" + library_suffix), qnx_disk}) {
                    cluster_pins.push_back(
                            {{"path", file.string()}, {"sha256", sha256_file(file)}});
                    host.pins[file.string()] = sha256_file(file);
                }

                const Json old_cluster{
                        {"schema", 1},
                        {"qemu",
                         {{"executable", qemu.string()},
                          {"args", Json::array({"unchanged-owned-args"})}}},
                        {"graphics", {{"executable", program.string()}, {"angle", angle.string()}}},
                        {"pins", cluster_pins}};
                auto full_host = old_host;
                full_host["graphics"] = program.string();
                full_host["qnx_qemu"] = qemu.string();
                full_host["angle"] = angle.string();
                full_host["pins"] = cluster_pins;
                full_host["pins"].erase(full_host["pins"].size() - 1);
                write_private_json(qnx_host_bindings_path(root), full_host);
                host.refresh_source = qnx_host_binding_view(root, full_host);
                host.graphics = program;
                host.qnx_qemu = qemu;
                host.angle = angle;
                host.require_cluster = true;
                write_private_json(scoped / "android-launch.json", previous);
                write_private_json(scoped / "cluster-launch.json", old_cluster);
                auto full_record = previous_record;
                full_record["android_launch_sha256"] = sha256_file(scoped / "android-launch.json");
                full_record["cluster_configured"] = true;
                full_record["cluster_launch_sha256"] = sha256_file(scoped / "cluster-launch.json");
                write_private_json(scoped / "activation.json", full_record);
                const auto reject_full = [&] {
                    const auto before_launch =
                                       read_private_json_bytes(scoped / "android-launch.json"),
                               before_cluster =
                                       read_private_json_bytes(scoped / "cluster-launch.json"),
                               before_activation =
                                       read_private_json_bytes(scoped / "activation.json"),
                               before_host = read_private_json_bytes(qnx_host_bindings_path(root));
                    bool denied = false;
                    try {
                        activate_portable_runtime(root, profile, default_machine, host, 0);
                    } catch (...) {
                        denied = true;
                    }

                    check(denied &&
                                  read_private_json_bytes(scoped / "android-launch.json") ==
                                          before_launch &&
                                  read_private_json_bytes(scoped / "cluster-launch.json") ==
                                          before_cluster &&
                                  read_private_json_bytes(scoped / "activation.json") ==
                                          before_activation &&
                                  read_private_json_bytes(qnx_host_bindings_path(root)) ==
                                          before_host,
                          "full refresh refusal changed host records");
                };
                host.refresh_host = false;
                reject_full();
                host.refresh_host = true;
                put(qemu, "changed QNX engine");
                reject_full();
                put(qemu, "unchanged QNX engine");
                put(angle / ("libEGL" + library_suffix), "changed EGL");
                reject_full();
                put(angle / ("libEGL" + library_suffix), "unchanged EGL");
                put(qnx_disk, "changed QNX guest image");
                reject_full();
                put(qnx_disk, "unchanged QNX guest image");
                auto foreign_cluster = old_cluster;
                foreign_cluster["graphics"]["executable"] =
                        (root / "foreign-graphics.exe").string();
                write_private_json(scoped / "cluster-launch.json", foreign_cluster);
                auto foreign_record = full_record;
                foreign_record["cluster_launch_sha256"] =
                        sha256_file(scoped / "cluster-launch.json");
                write_private_json(scoped / "activation.json", foreign_record);
                reject_full();
                write_private_json(scoped / "cluster-launch.json", old_cluster);
                write_private_json(scoped / "activation.json", full_record);
                auto foreign_host = full_host;
                foreign_host["pins"][0]["sha256"] = std::string(64, 'f');
                write_private_json(qnx_host_bindings_path(root), foreign_host);
                host.refresh_source = qnx_host_binding_view(root, foreign_host);
                reject_full();
                write_private_json(qnx_host_bindings_path(root), full_host);
                host.refresh_source = qnx_host_binding_view(root, full_host);
                const auto qnx_identity = cache_file_identity(qnx_disk);
                const auto qnx_sha = sha256_file(qnx_disk);
                const auto full_refreshed = activate_portable_runtime(root, profile,
                                                                      default_machine, host, 0);
                auto expected_cluster = old_cluster;
                expected_cluster["pins"][0]["sha256"] = sha256_file(program);
                auto expected_full_host = full_host;
                expected_full_host["frontend"] = expected_frontend;
                expected_full_host["pins"][0]["sha256"] = sha256_file(program);
                check(full_refreshed.at("host_refreshed") == true &&
                              read_private_json(scoped / "cluster-launch.json") == expected_cluster,
                      "full cluster graphics pin was not refreshed");
                check(qnx_host_binding_view(root, read_private_json(qnx_host_bindings_path(
                                                          root))) == expected_full_host &&
                              read_private_json(qnx_host_bindings_path(root))
                                              .at("immutable_sdk_marker") == "unchanged",
                      "full host graphics source pin was not refreshed");
                const auto committed = read_private_json(scoped / "activation.json");
                check(committed.at("android_launch_sha256") ==
                                      sha256_file(scoped / "android-launch.json") &&
                              committed.at("cluster_launch_sha256") ==
                                      sha256_file(scoped / "cluster-launch.json"),
                      "full refresh did not commit both launch hashes");
                check(cache_file_identity(disk) == disk_identity && sha256_file(disk) == disk_sha &&
                              cache_file_identity(qnx_disk) == qnx_identity &&
                              sha256_file(qnx_disk) == qnx_sha,
                      "full refresh changed Android or QNX guest identity/bytes");
                check(activate_portable_runtime(root, profile, default_machine, host, 0)
                                      .at("cached") == true,
                      "full refresh was not reusable");
                auto split_host = full_host;
                split_host["frontend"] = expected_frontend;
                write_private_json(qnx_host_bindings_path(root), split_host);
                host.refresh_source = qnx_host_binding_view(root, split_host);
                write_private_json(scoped / "cluster-launch.json", old_cluster);
                auto split_record = committed;
                split_record["cluster_launch_sha256"] = sha256_file(scoped / "cluster-launch.json");
                write_private_json(scoped / "activation.json", split_record);
                host.refresh_host = false;
                reject_full();
                host.refresh_host = true;
                auto split_foreign = split_host;
                split_foreign["pins"][0]["sha256"] = std::string(64, 'f');
                write_private_json(qnx_host_bindings_path(root), split_foreign);
                host.refresh_source = qnx_host_binding_view(root, split_foreign);
                reject_full();
                write_private_json(qnx_host_bindings_path(root), split_host);
                host.refresh_source = qnx_host_binding_view(root, split_host);
                const auto split_refreshed = activate_portable_runtime(root, profile,
                                                                       default_machine, host, 0);
                check(split_refreshed.value("host_refreshed", false) &&
                              read_private_json(scoped / "cluster-launch.json") ==
                                      expected_cluster &&
                              qnx_host_binding_view(root, read_private_json(qnx_host_bindings_path(
                                                                  root))) == expected_full_host,
                      "historical split graphics pins were not refreshed");
                check(cache_file_identity(disk) == disk_identity && sha256_file(disk) == disk_sha &&
                              cache_file_identity(qnx_disk) == qnx_identity &&
                              sha256_file(qnx_disk) == qnx_sha,
                      "split refresh changed guest identity/bytes");
                std::filesystem::remove(scoped / "cluster-launch.json");
                host.require_cluster = false;
            }
#ifdef _WIN32
            {
                // A current Windows host document carries its working keys only in the "windows"
                // section, so the cluster refresh must read and rewrite that section and leave the
                // rest of the document alone.
                const auto qemu = root / ("qemu-system-aarch64" + program_suffix),
                           angle = root / "angle",
                           qnx_disk = recorded_runtime / "qnx/host-native-lzo.bin";
                put(qemu, "unchanged QNX engine");
                put(angle / ("libEGL" + library_suffix), "unchanged EGL");
                put(angle / ("libGLESv2" + library_suffix), "unchanged GLES");
                put(qnx_disk, "unchanged QNX guest image");
                Json cluster_pins = Json::array({old_frontend.at("pins").at(0)});

                for (const auto &file : {qemu, angle / ("libEGL" + library_suffix),
                                         angle / ("libGLESv2" + library_suffix), qnx_disk}) {
                    cluster_pins.push_back(
                            {{"path", file.string()}, {"sha256", sha256_file(file)}});
                    host.pins[file.string()] = sha256_file(file);
                }

                const Json old_cluster{
                        {"schema", 1},
                        {"qemu",
                         {{"executable", qemu.string()},
                          {"args", Json::array({"unchanged-owned-args"})}}},
                        {"graphics", {{"executable", program.string()}, {"angle", angle.string()}}},
                        {"pins", cluster_pins}};
                auto section_pins = cluster_pins;
                section_pins.erase(section_pins.size() - 1);
                const Json section{{"schema", 1},
                                   {"qnx_qemu", qemu.string()},
                                   {"graphics", program.string()},
                                   {"angle", angle.string()},
                                   {"pins", section_pins},
                                   {"qnx_file_backend",
                                    {{"schema", 1},
                                     {"version", "11.1.2"},
                                     {"engine_sha256", sha256_file(qemu)}}}};
                const Json shared_host{
                        {"schema", 1}, {"immutable_sdk_marker", "unchanged"}, {"windows", section}};
                write_private_json(qnx_host_bindings_path(root), shared_host);
                host.refresh_source = qnx_host_binding_view(root, shared_host);
                host.graphics = program;
                host.qnx_qemu = qemu;
                host.angle = angle;
                host.require_cluster = true;
                write_private_json(scoped / "android-launch.json", previous);
                write_private_json(scoped / "cluster-launch.json", old_cluster);
                auto shared_record = previous_record;
                shared_record["android_launch_sha256"] =
                        sha256_file(scoped / "android-launch.json");
                shared_record["cluster_configured"] = true;
                shared_record["cluster_launch_sha256"] =
                        sha256_file(scoped / "cluster-launch.json");
                write_private_json(scoped / "activation.json", shared_record);
                const auto shared_refreshed = activate_portable_runtime(root, profile,
                                                                        default_machine, host, 0);
                const auto shared_written = read_private_json(qnx_host_bindings_path(root));
                check(shared_refreshed.value("host_refreshed", false) &&
                              read_private_json(scoped / "cluster-launch.json")
                                              .at("pins")
                                              .at(0)
                                              .at("sha256") == sha256_file(program),
                      "Windows host section cluster pin was not refreshed");
                check(qnx_host_binding_view(root, shared_written).at("pins").at(0).at("sha256") ==
                                      sha256_file(program) &&
                              qnx_host_binding_view(root, shared_written).at("qnx_qemu") ==
                                      qemu.string(),
                      "Windows host section graphics source was not refreshed");
                check(shared_written.at("immutable_sdk_marker") == "unchanged" &&
                              shared_written.at("windows").at("qnx_file_backend") ==
                                      section.at("qnx_file_backend"),
                      "Windows host section refresh changed the rest of the shared document");
                std::filesystem::remove(scoped / "cluster-launch.json");
                host.require_cluster = false;
            }
#endif
            host.frontend = Json::object();
            write_private_json(scoped / "android-launch.json", observer_only_launch);
            write_private_json(scoped / "activation.json", observer_only_record);
        }
#endif
        host.refresh_host = false;
        host.ui_observer = Json::object();
        put(scoped / "android-launch.json", cached_bytes);
        write_private_json(scoped / "activation.json", current_record);
        host.require_cluster = true;
        rejected = false;
        try {
            activate_portable_runtime(root, profile, default_machine, host, 0);
        } catch (const std::exception &e) {
            rejected = std::string(e.what()).find("cannot be upgraded in place") !=
                       std::string::npos;
        }

        check(rejected && sha256_file(scoped / "android-launch.json") == sha256_bytes(cached_bytes),
              "cluster upgrade overwrote existing Android runtime");
        std::cout
                << "portable seed/profile/pin/path/discovery and system-only preservation guards passed; no guest launched\n";

        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
