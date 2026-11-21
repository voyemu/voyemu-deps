#include "qnx_prepare.hpp"
#include "check.hpp"
#include "error.hpp"
#include "ordinary_path.hpp"
#include "os/fs.hpp"
#include "process.hpp"
#include "project_files.hpp"
#include "qnx6_file.hpp"
#include "qnx_bootstrap.hpp"
#include "qnx_console.hpp"
#include "qnx_gpt.hpp"
#include "qnx_guest_build.hpp"
#include "qnx_guest_shell.hpp"
#include "qnx_host_adapt.hpp"
#include "qnx_imagefs.hpp"
#include "qnx_interguest_contract.hpp"
#include "qnx_lzo.hpp"
#include "qnx_originals_directory.hpp"
#include "qnx_passion.hpp"
#include "qnx_passion_platform.hpp"
#include "qnx_passion_smem.hpp"
#include "qnx_passion_someip.hpp"
#include "qnx_qinggan11.hpp"
#include "qnx_runtime_layout.hpp"
#include "qnx_startup.hpp"
#include "quickboot.hpp"
#include "toolchain_manifest.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
namespace voyemu {
namespace {
constexpr CodeCheck need{"QNX preparation check failed"};

void ordinary(const std::filesystem::path &path) {
    ordinary_path(path, need, PathForm::Normalized, "QNX_PREPARE_PATH", "QNX_PREPARE_LINK");
}

std::string read(const std::filesystem::path &path, std::uint64_t maximum) {
    ordinary(path);
    need(std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) <= maximum,
         "QNX_PREPARE_INPUT_BOUNDS");
    std::ifstream input(path, std::ios::binary);
    need(bool(input), "QNX_PREPARE_OPEN");
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    need(!input.bad() && bytes.size() <= maximum, "QNX_PREPARE_READ");

    return bytes;
}

// The SHA-256 of a project file of root by its project-relative name.
std::string source_sha256(const std::filesystem::path &root, const std::string &name) {
    return name.starts_with("src/") ? project_file_sha256(root, name, 2 * 1024 * 1024)
                                    : sha256_file(root / name);
}

// Records the SHA-256 of a project file of a QNX build. An installation has no src/ and records
// only the profile files: the sources of its launcher are those of its version.
void add_source(Json &sources, const std::filesystem::path &root, const std::string &name) {
    if (!name.starts_with("src/") || project_source_checkout(root)) {
        sources[name] = source_sha256(root, name);
    }
}

// A sanity bound for reading guest scripts; the ImageFS writer checks the OEM slot capacity.
constexpr std::uint64_t guest_script_bytes = 1024 * 1024;

void write(const std::filesystem::path &path, std::string_view bytes) {
    need(!std::filesystem::exists(path), "QNX_PREPARE_FRESH_FILE");
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), bytes.size());
    output.close();
    need(bool(output), "QNX_PREPARE_WRITE");
}

std::uint32_t word(std::string_view bytes, std::size_t at) {
    need(at + 4 <= bytes.size(), "QNX_PREPARE_FIELD_BOUNDS");
    std::uint32_t value = 0;

    for (unsigned i = 0; i < 4; ++i) {
        value |= std::uint32_t(static_cast<unsigned char>(bytes[at + i])) << (i * 8);
    }

    return value;
}
} // namespace

std::string qnx_native_mcu_async_runtime(std::string_view source) {
    const std::string before =
            "QGFX_NATIVE_MCU=1 LD_PRELOAD=/proc/boot/bin/Snapdragon.rgb /usr/bin/rpcd dummy &\n";
    const std::string after =
            "QGFX_NATIVE_MCU=1 QGFX_NATIVE_MCU_ASYNC=1 LD_PRELOAD=/proc/boot/bin/Snapdragon.rgb /usr/bin/rpcd dummy &\n";
    const auto at = source.find(before);

    need(at != source.npos && source.find(before, at + 1) == source.npos &&
                 source.find("QGFX_NATIVE_MCU_ASYNC=") == source.npos,
         "QNX_MCU_LAUNCH");
    auto runtime = std::string(source);
    runtime.replace(at, before.size(), after);

    return runtime;
}

std::string qnx_qinggan09_runtime(std::string_view source, std::string_view persistent_runtime,
                                  bool cluster_ui) {
    auto runtime = qnx_native_mcu_async_runtime(source);
    const std::string mount =
            "qm_bounded logmount /bin/mount -t qnx6 -o sync=optional /dev/qip/hd1 /log || exit 1\n";
    const auto network = runtime.find(mount);

    need(network != runtime.npos && runtime.find(mount, network + 1) == runtime.npos &&
                 runtime.find("VOYEMU_QNET_RELAY=") == runtime.npos,
         "QNX_QINGGAN09_NETWORK_MOUNT");
    const std::string launch =
            "/usr/sbin/random -t >&2 &\nqm_random=$!\nqm_random_attempt=0\nwhile { [ ! -c /dev/random ] || [ ! -c /dev/urandom ]; } && [ \"$qm_random_attempt\" -lt 160 ]; do\n    if ! kill -0 \"$qm_random\" 2>/dev/null; then echo 'QNET FAIL entropy-parent-exited'; exit 1; fi\n    /bin/sleep 0.1 || exit 1\n    qm_random_attempt=$((qm_random_attempt+1))\ndone\nif ! kill -0 \"$qm_random\" 2>/dev/null || [ ! -c /dev/random ] || [ ! -c /dev/urandom ]; then echo 'QNET FAIL entropy-devices'; exit 1; fi\nqm_bounded random-ready /bin/openssl rand -out /dev/null 32 || exit 1\necho 'QNET entropy-ready=1'\n"
            "[ -e /dev/socket ] || { echo 'QNET FAIL socket'; exit 1; }\nqm_bounded net-create /bin/ifconfig tap0 create || exit 1\nqm_bounded net-up /bin/ifconfig tap0 172.16.100.22 netmask 255.255.255.0 up || exit 1\nVOYEMU_QNET_RELAY=1 LD_PRELOAD=/proc/boot/bin/Snapdragon.rgb /bin/ls >&2 &\nqm_qnet=$!\n/usr/sbin/qconn >&2 &\nqm_qconn=$!\n";
    runtime.insert(network + mount.size(), launch);

    if (!persistent_runtime.empty()) {
        need(persistent_runtime.find('\0') == persistent_runtime.npos,
             "QNX_QINGGAN09_PERSISTENT_RUNTIME");
        const std::string driver = "automount=hd0:/:qnx6:ro,after &\n";
        const auto root = runtime.find(driver);
        need(root != runtime.npos && runtime.find(driver, root + 1) == runtime.npos,
             "QNX_QINGGAN09_ROOT_DRIVER");
        runtime.replace(root, driver.size(),
                        "automount=hd0:/:qnx6:ro,after qnx6 sync=optional &\n");
        const auto begin = runtime.find("qm_bounded vardirs "),
                   end = runtime.find("/bin/pps -m /var/pps ", begin);
        need(begin != runtime.npos && end != runtime.npos && begin < end,
             "QNX_QINGGAN09_USERDATA_STAGE");
        runtime.replace(begin, end - begin, persistent_runtime);
        // Native sysmgr reads the persistent config before starting the graphics owners.
        const std::string background = "qm_background=$!\n",
                          screen = "# Stream the real Screen composition";
        const auto background_pos = runtime.find(background), screen_pos = runtime.find(screen);
        need(background_pos != runtime.npos && screen_pos != runtime.npos &&
                     background_pos + background.size() < screen_pos &&
                     runtime.find(background, background_pos + 1) == runtime.npos &&
                     runtime.find(screen, screen_pos + 1) == runtime.npos,
             "QNX_QINGGAN09_GRAPHICS_START");
        const auto owner_start = background_pos + background.size();
        const auto owner_block = runtime.substr(owner_start, screen_pos - owner_start);
        need(owner_block.find(" /usr/bin/point ") != owner_block.npos &&
                     owner_block.find(" /usr/bin/light ") != owner_block.npos &&
                     owner_block.find(" /usr/bin/gauge ") != owner_block.npos,
             "QNX_QINGGAN09_GRAPHICS_OWNERS");
        runtime.replace(
                owner_start, owner_block.size(),
                "for qm_owner in point light gauge; do\n"
                "[ ! -e /log/run/$qm_owner.pid ] && qm_bounded \"$qm_owner-start\" /usr/bin/sysmgrtool start \"$qm_owner\" || exit 1\n"
                "done\n");
        const std::string exporter = "qm_export=$!\n";
        const auto export_pos = runtime.find(exporter);
        need(export_pos != runtime.npos && export_pos > owner_start &&
                     runtime.find(exporter, export_pos + 1) == runtime.npos,
             "QNX_QINGGAN09_GRAPHICS_EXPORT");
        runtime.insert(
                export_pos + exporter.size(),
                "qm_point=$(qm_owner_pid point) && qm_light=$(qm_owner_pid light) && qm_gauge=$(qm_owner_pid gauge) || exit 1\n");
        const std::string owner_cleanup =
                "kill \"$qm_gauge\" 2>/dev/null\nwait \"$qm_gauge\"\necho \"QMC GAUGE exit=$?\"\n"
                "for qm_service in \"$qm_background\" \"$qm_point\" \"$qm_light\"";
        const auto cleanup = runtime.find(owner_cleanup);
        need(cleanup != runtime.npos && runtime.find(owner_cleanup, cleanup + 1) == runtime.npos,
             "QNX_QINGGAN09_GRAPHICS_STOP");
        runtime.replace(
                cleanup, owner_cleanup.size(),
                "for qm_owner in gauge light point; do qm_bounded \"$qm_owner-stop\" /usr/bin/sysmgrtool stop \"$qm_owner\"; done\n"
                "for qm_service in \"$qm_background\"");
        const auto applications = runtime.find("/bin/pps -m /var/pps ");
        const std::string preload = "LD_PRELOAD=/proc/boot/bin/Snapdragon.rgb";

        for (auto position = applications;
             (position = runtime.find(preload, position)) != runtime.npos;) {
            const std::string composed = "LD_PRELOAD=\"$qm_render_preload\"";
            runtime.replace(position, preload.size(), composed);
            position += composed.size();
        }

        const std::string observer = "LD_PRELOAD=/proc/boot/bin/qditest";

        for (auto position = applications;
             (position = runtime.find(observer, position)) != runtime.npos;) {
            const std::string composed = "LD_PRELOAD=\"$qm_observer_preload\"";
            runtime.replace(position, observer.size(), composed);
            position += composed.size();
        }
    }

    if (cluster_ui && persistent_runtime.empty()) {
        return qnx_kanzi_gauge_runtime(runtime);
    }

    if (cluster_ui) {
        const std::string marker = R"(if(o=="gauge")f=")";
        const auto gauge = runtime.find(marker);
        need(gauge != runtime.npos && runtime.find(marker, gauge + 1) == runtime.npos,
             "QNX_QINGGAN09_KANZI_GAUGE");
        runtime.insert(gauge + marker.size(), "CLUSTER_UI=1 ");
    }

    return runtime;
}

std::string qnx_kanzi_gauge_runtime(std::string_view runtime) {
    const std::string marker =
            "QGFX_WHEEL_RELAY=0 QGFX_MCU_RELAY=0 QGFX_DLT_TAIL=1 QGFX_THEME_REQUEST_ENTRY=1 ";
    const auto gauge = runtime.find(marker);

    need(gauge != runtime.npos && runtime.find(marker, gauge + 1) == runtime.npos &&
                 runtime.find("CLUSTER_UI=") == runtime.npos,
         "QNX_KANZI_GAUGE");
    std::string output(runtime);
    output.insert(gauge + marker.size(), "QGFX_CLUSTER_UI=1 ");

    return output;
}

std::string qnx_profile_adapter(const Json &definition) {
    const auto &adapter = definition.at("hardware_adapter");

    need(adapter.is_string(), "QNX_PREPARE_ADAPTER");

    return adapter.get<std::string>();
}

bool qnx_kanzi_gauge_observer(const Json &definition) {
    const auto observer = definition.contains("gauge_observer") ? definition.at("gauge_observer")
                                                                : Json();

    need(observer.is_null() || observer == "kanzi", "QNX_PREPARE_GAUGE_OBSERVER");

    return observer == "kanzi";
}

QnxDonorInput qnx_donor_input(const std::filesystem::path &root, const Json &definition,
                              std::string_view file) {
    const auto inputs = definition.value("donor_inputs", Json::array());
    const Json *match = nullptr;

    need(inputs.is_array(), "QNX_DONOR_INPUT");

    for (const auto &input : inputs) {
        need(input.is_object() && input.size() == 3 && input.contains("profile") &&
                     input.at("profile").is_string() && input.contains("file") &&
                     input.at("file").is_string() && input.contains("sha256") &&
                     input.at("sha256").is_string(),
             "QNX_DONOR_INPUT");

        if (input.at("file").get<std::string>() == file) {
            need(match == nullptr, "QNX_DONOR_INPUT");
            match = &input;
        }
    }

    need(match != nullptr, "QNX_DONOR_INPUT_MISSING");
    const auto profile = match->at("profile").get<std::string>(),
               pin = match->at("sha256").get<std::string>();
    need(valid_profile_name(profile) && sha256_text(pin), "QNX_DONOR_INPUT");

    return {qnx_originals_directory(root, profile) / std::string(file), pin};
}

Json prepare_qnx_images(const std::filesystem::path &root, std::string_view profile,
                        const std::filesystem::path &output, bool offline) {
    ordinary(root);
    ordinary(output);
    need(std::filesystem::canonical(root) == root && valid_profile_name(profile) &&
                 output.parent_path() == root / ".local/build" && !std::filesystem::exists(output),
         "QNX_PREPARE_FRESH_OUTPUT");
    const auto profile_file = root / "profiles" / std::string(profile) / "profile.json",
               qnx_file = root / "profiles" / std::string(profile) / "profile-qnx.json";
    const auto profile_pin = sha256_file(profile_file), qnx_pin = sha256_file(qnx_file);
    const auto config = read_private_json(qnx_file, 128 * 1024);
    const auto adapter = qnx_profile_adapter(config);

    // The adapter paths read installed guest modules; downloads happen here, where offline is
    // known.
    for (const auto &spec : qnx_adapter_registry) {
        if (adapter == spec.name) {
            install_qnx_guest_modules(
                    root, spec.adapter,
                    embedded_toolchain_manifest().at("qnxGuestModules").at(std::string(spec.name)),
                    offline);
        }
    }

    const auto known = std::find_if(qnx_adapter_registry.begin(), qnx_adapter_registry.end(),
                                    [&](const QnxAdapterSpec &spec) {
                                        return spec.name == adapter;
                                    });
    need(known != qnx_adapter_registry.end(), "QNX_PREPARE_ADAPTER");
    const auto platform = known->platform;

    if (platform == QnxGuestPlatform::passion) {
        return prepare_qnx_passion_developer_images(root, profile, output);
    }

    if (platform == QnxGuestPlatform::qinggan11) {
        return prepare_qnx_qinggan11_images(root, profile, output);
    }

    need(config.at("schema") == 1 && platform == QnxGuestPlatform::qinggan09 &&
                 config.at("originals").size() == 4,
         "QNX_PREPARE_ADAPTER");
    const auto input_root = qnx_originals_directory(root, profile);
    Json inputs = Json::object();
    std::map<std::string, std::filesystem::path> files;
    std::map<std::string, Json> epochs;

    for (const auto *name :
         {"hyp_la.img", "ifs2_la.img", "system_la.img", "ifs2_recovery_la.img"}) {
        const auto &definition = config.at("originals").at(name);
        need(definition.at("file") == name, "QNX_PREPARE_ORIGINAL_ROLE");
        const auto file = input_root / name;
        ordinary(file);
        need(std::filesystem::is_regular_file(file) &&
                     std::filesystem::file_size(file) <= 4ULL * 1024 * 1024 * 1024 &&
                     std::filesystem::hard_link_count(file) == 1,
             "QNX_PREPARE_ORIGINAL_BOUNDS");
        const auto epoch = cache_file_identity(file);
        const auto pin = sha256_file(file);
        need(pin == definition.at("sha256").get<std::string>() &&
                     cache_file_identity(file) == epoch,
             "QNX_PREPARE_ORIGINAL_SHA");
        files[name] = file;
        epochs[name] = epoch;
        inputs[name] = {{"sha256", pin}, {"bytes", std::filesystem::file_size(file)}};
    }

    Json source_pins = Json::object();
    auto source_pin = [&](const std::filesystem::path &relative) {
        if (relative.generic_string().starts_with("src/")) {
            add_source(source_pins, root, relative.generic_string());

            return;
        }

        const auto file = root / relative;
        ordinary(file);
        need(std::filesystem::is_regular_file(file) &&
                     std::filesystem::file_size(file) <= 2 * 1024 * 1024,
             "QNX_PREPARE_SOURCE_BOUNDS");
        source_pins[relative.generic_string()] = sha256_file(file);
    };

    for (const auto &item : project_directory_files(root, "src/app/qnx", false)) {
        const std::filesystem::path file = item.name;

        if (file.filename().string().starts_with("qnx_") &&
            file.filename().string().find(".test.") == std::string::npos &&
            (file.extension() == ".cpp" || file.extension() == ".hpp")) {
            source_pin(file);
        }
    }

    for (const auto *file :
         {"src/app/profiles/qnx_originals_directory.hpp", "src/app/profiles/profile_identity.hpp",
          "src/app/core/model.cpp", "src/app/core/model.hpp", "src/app/core/process.cpp",
          "src/app/core/process.hpp", "src/app/runtime/quickboot.cpp",
          "src/app/runtime/quickboot.hpp", "src/app/core/download.cpp", "src/app/core/download.hpp",
          "src/app/CMakeLists.txt", "src/app/dependencies.lock.json",
          "src/guest/qnx/component.json", "src/guest/qnx/qnx-runtime.sh",
          "src/guest/qnx/persistent-runtime.sh", "src/guest/qnx/qnx-screen.conf"}) {
        source_pin(file);
    }

    source_pin(profile_file.lexically_relative(root));
    source_pin(qnx_file.lexically_relative(root));
    need(qnx_qinggan09_source_space_available(std::filesystem::space(root).available),
         "QNX_PREPARE_DISK_RESERVE");
    need(std::filesystem::create_directory(output), "QNX_PREPARE_CREATE");
    ensure_private_directory(output);
    auto phase = [&](const char *name) {
        write_private_json(output / "phase.json",
                           {{"schema", 1}, {"phase", name}, {"guest_started", false}});
    };
    phase("extract-originals");
    const auto hyp = read(files.at("hyp_la.img"), 16 * 1024 * 1024),
               ifs = read(files.at("ifs2_la.img"), 512 * 1024 * 1024),
               recovery = read(files.at("ifs2_recovery_la.img"), 512 * 1024 * 1024);
    const auto end = std::uint64_t(word(hyp, 0x3024)) + 0x3000;
    need(end >= 0x5a108 && end <= hyp.size(), "QNX_PREPARE_HOST_STREAM");
    const auto reference = std::string_view(hyp).substr(0x5a108, end - 0x5a108);
    const auto original_host = decode_qnx_boot_stream(reference);
    const auto libc = read_qnx_imagefs_file(original_host, "lib64/libc.so.4"),
               original_display = read_qnx_imagefs_file(ifs, "ifs_display.img"),
               original_disk = read_qnx_imagefs_file(ifs, "ifs_disk.img"),
               screen = read_qnx_imagefs_file(original_display, "lib64/libscreen.so.1"),
               driver = read_qnx_imagefs_file(read_qnx_imagefs_file(recovery, "ifs_recovery.img"),
                                              "bin/devb-ram");
    const auto &startup_definition = config.at("startup_extension");
    need(startup_definition.size() == 3 &&
                 startup_definition.at("file") == "scripts/startup_cluster.sh" &&
                 startup_definition.at("anchor") == "clusterboot &",
         "QNX_PREPARE_STARTUP_CONTRACT");
    const auto startup_baseline = read_qnx6_file(
            files.at("system_la.img"), inputs.at("system_la.img").at("sha256").get<std::string>(),
            startup_definition.at("file").get<std::string>());
    need(sha256_bytes(startup_baseline) == startup_definition.at("sha256").get<std::string>(),
         "QNX_PREPARE_STARTUP_SHA");
    const auto screenshot = read_qnx6_file(
            files.at("system_la.img"), inputs.at("system_la.img").at("sha256").get<std::string>(),
            "usr/bin/screenshot");
    const auto sysmgrtool_original = read_qnx6_file(
            files.at("system_la.img"), inputs.at("system_la.img").at("sha256").get<std::string>(),
            "usr/bin/sysmgrtool");
    phase("build-guest-sources");
    const auto guest = build_qnx_guest_modules(root, libc, screen, output / "guest", offline,
                                               known->adapter, Json::object(), startup_baseline,
                                               sysmgrtool_original);
    const auto bundle = read(output / "guest/front-bundle.so", 360000),
               observer = read(output / "guest/observer.so", 16928),
               wfd = read(output / "guest/virtual_wfd.so", 97855),
               stub = read(output / "guest/readstub.bin", 4096);
    std::string padded(observer);
    padded.resize(16928, 0);
    auto runtime = prepare_qnx_guest_shell(qnx_qinggan09_runtime(
            project_file_bytes(root, "src/guest/qnx/qnx-runtime.sh", guest_script_bytes),
            project_file_bytes(root, "src/guest/qnx/persistent-runtime.sh", guest_script_bytes),
            qnx_kanzi_gauge_observer(config)));
    need(runtime.starts_with("#!/proc/boot/bin/ksh\n"), "QNX_PREPARE_RUNTIME");
    const std::string old_observer =
                              "2e13e9b8299f882414400969309c501c639314a92f5d0e603806e0f13d37426c",
                      observer_hash = sha256_bytes(padded);
    unsigned replacements = 0;

    for (std::size_t at = 0; (at = runtime.find(old_observer, at)) != std::string::npos; at += 64) {
        runtime.replace(at, 64, observer_hash);
        ++replacements;
    }

    need(replacements == 2, "QNX_PREPARE_OBSERVER_BINDINGS");
    const auto screen_config = project_file_bytes(root, "src/guest/qnx/qnx-screen.conf", 1115);
    phase("assemble-imagefs");
    auto display = original_display;
    auto replace_display = [&](std::string_view name, std::string_view payload,
                               unsigned alignment = 1, bool padded = false) {
        const auto before = read_qnx_imagefs_file(display, name);

        if (name == "bin/ssplash" && payload.size() > before.size()) {
            display = replace_qnx_imagefs_file_in_zero_padding(display, name, sha256_bytes(before),
                                                               payload, 4096);
        } else {
            display = replace_qnx_imagefs_file(display, name, sha256_bytes(before), payload,
                                               alignment, padded);
        }
    };
    replace_display("bin/Snapdragon.rgb", bundle, 4096);
    replace_display("bin/qditest", observer, 1, true);
    replace_display("bin/openwfd_telltale", screenshot, 1, true);
    replace_display("bin/ssplash", runtime, 1, true);
    replace_display("lib64/libopenwfd_qnx.so", wfd, 1, true);
    replace_display("lib64/graphics.conf", screen_config, 1, true);

    for (const auto &[before, after] : std::array<std::pair<std::string_view, std::string_view>, 5>{
                 {{"proc/boot/img_splash.conf", "img_splash.conf"},
                  {"proc/boot/logo.jpg", "logo.jpg"},
                  {"lib64/dll/screen-stdbuf.so", "lib64/screen-stdbuf.so"},
                  {"lib64/dll/img_codec_jpg.so", "lib64/img_codec_jpg.so"},
                  {"lib64/dll/img_codec_png.so", "lib64/img_codec_png.so"}}}) {
        display = rename_qnx_imagefs_file(display, before, after);
    }

    auto disk = replace_qnx_imagefs_file(
            original_disk, "bin/devb-ufs-qualcomm",
            sha256_bytes(read_qnx_imagefs_file(original_disk, "bin/devb-ufs-qualcomm")), driver);
    disk = rename_qnx_imagefs_file(disk, "bin/devb-ufs-qualcomm", "bin/devb-ram");
    auto new_ifs = replace_qnx_imagefs_file(ifs, "ifs_display.img", sha256_bytes(original_display),
                                            display);
    new_ifs = replace_qnx_imagefs_file(new_ifs, "ifs_disk.img", sha256_bytes(original_disk), disk);
    auto host = original_host;
    auto replace_host = [&](std::string_view name, const std::string &payload) {
        host = replace_qnx_imagefs_file(host, name, sha256_bytes(read_qnx_imagefs_file(host, name)),
                                        payload);
    };
    replace_host("proc/boot/.script",
                 adapt_qnx_boot_script(read_qnx_imagefs_file(original_host, "proc/boot/.script")));
    replace_host("proc/boot/procnto-smp-instr",
                 adapt_qnx_procnto_environment(
                         read_qnx_imagefs_file(original_host, "proc/boot/procnto-smp-instr")));
    replace_host("bin/make_symlinks",
                 adapt_qnx_symlinks(read_qnx_imagefs_file(original_host, "bin/make_symlinks")));
    replace_host("bin/startupmgr",
                 adapt_qnx_startupmgr(read_qnx_imagefs_file(original_host, "bin/startupmgr"), stub,
                                      sha256_bytes(display), sha256_bytes(disk)));
    phase("pack-boot-stream");
    const auto packed = encode_qnx_boot_stream(reference, host),
               kernel = assemble_qnx_kernel(
                       hyp, inputs.at("hyp_la.img").at("sha256").get<std::string>(), packed);
    write(output / "hyp-hvf.img", kernel);
    write(output / "ifs2-native.img", new_ifs);
    write(output / "host-native-lzo.bin", packed);
    write(output / "host-native.ifs", host);
    write(output / "smem.bin", qnx_smem_memory());
    write(output / "synthetic-gpt.bin", qnx_synthetic_gpt());

    for (const auto *name : {"qnx-ipl-virt.bin", "qnx-ifs2-restore.bin"}) {
        std::filesystem::copy_file(output / "guest" / name, output / name);
    }

    phase("build-system-disk");
    const auto system = prepare_qnx_system_disk(
            files.at("system_la.img"), inputs.at("system_la.img").at("sha256").get<std::string>(),
            output / "system", qnx_qinggan09_disk_floor);
    std::filesystem::rename(system, output / "sysla-gpt-ramdisk-v3.img");
    std::filesystem::remove(output / "system");
    os::resize_file(output / "sysla-gpt-ramdisk-v3.img", 1025ULL << 20);
    const Json persistence = {{"schema", 1},
                              {"system_image", "sysla-gpt-ramdisk-v3.img"},
                              {"userdata_image", "userdata-qnx6.img"}};
    need(config.at("persistence") == persistence, "QNX_PREPARE_PERSISTENCE_CONTRACT");
    {
        std::ofstream userdata(output / "userdata-qnx6.img", std::ios::binary);
        need(bool(userdata), "QNX_PREPARE_USERDATA_CREATE");
        userdata.seekp((32ULL << 20) - 1);
        userdata.put(0);
        userdata.close();
        need(bool(userdata) &&
                     std::filesystem::file_size(output / "userdata-qnx6.img") == 32ULL << 20,
             "QNX_PREPARE_USERDATA_EXTENT");
    }

    for (const auto *disk : {"sysla-gpt-ramdisk-v3.img", "userdata-qnx6.img"}) {
        std::filesystem::permissions(output / disk, std::filesystem::perms::owner_read |
                                                            std::filesystem::perms::owner_write);
    }

    Json images = Json::object();

    for (const auto *name : {"hyp-hvf.img", "ifs2-native.img", "host-native-lzo.bin", "smem.bin",
                             "qnx-ifs2-restore.bin", "synthetic-gpt.bin",
                             "sysla-gpt-ramdisk-v3.img", "qnx-ipl-virt.bin", "userdata-qnx6.img"}) {
        images[name] = {{"file", name},
                        {"sha256", sha256_file(output / name)},
                        {"bytes", std::filesystem::file_size(output / name)}};
    }

    std::uint64_t image_bytes = 0;

    for (const auto &image : images.items()) {
        image_bytes += image.value().at("bytes").get<std::uint64_t>();
    }

    need(image_bytes <= qnx_qinggan09_image_budget, "QNX_PREPARE_IMAGE_BUDGET");

    for (const auto &[name, file] : files) {
        need(cache_file_identity(file) == epochs.at(name) &&
                     sha256_file(file) == inputs.at(name).at("sha256").get<std::string>(),
             "QNX_PREPARE_ORIGINAL_CHANGED");
    }

    need(sha256_file(profile_file) == profile_pin && sha256_file(qnx_file) == qnx_pin,
         "QNX_PREPARE_PROFILE_CHANGED");

    for (auto at = source_pins.begin(); at != source_pins.end(); ++at) {
        need(source_sha256(root, at.key()) == at.value().get<std::string>(),
             "QNX_PREPARE_SOURCE_CHANGED");
    }

    const Json receipt = {{"schema", 1},
                          {"kind", "raw-qnx-source-build"},
                          {"interguest_network", qnx_interguest_contract()},
                          {"persistence", persistence},
                          {"profile", profile},
                          {"source_generated", true},
                          {"prepared_baseline_input", false},
                          {"originals", inputs},
                          {"guest_build", guest},
                          {"images", images},
                          {"guest_booted", false},
                          {"exported_frame_verified", false}};
    write_private_json(output / "build.json", receipt);
    phase("source-build-complete");

    return receipt;
}

Json prepare_qnx_qinggan11_images(const std::filesystem::path &root, std::string_view profile,
                                  const std::filesystem::path &output) {
    ordinary(root);
    ordinary(output);
    need(std::filesystem::canonical(root) == root && valid_profile_name(profile) &&
                 output.parent_path() == root / ".local/build" && !std::filesystem::exists(output),
         "QNX_QINGGAN11_DEVELOPER_OUTPUT");
    const auto config_file = root / "profiles" / std::string(profile) / "profile-qnx.json",
               profile_file = config_file.parent_path() / "profile.json";
    const auto config_pin = sha256_file(config_file), profile_pin = sha256_file(profile_file);
    const auto config = read_private_json(config_file, 128 * 1024);
    const auto family = runtime_profile_reference(read_private_json(profile_file, 1 << 20),
                                                  profile);
    const auto referenced_config = root / "profiles" / family / "profile-qnx.json";
    // A local copy uses the QNX of its reference profile only when both declare the same QNX.
    need(valid_profile_name(family) &&
                 (family == profile ||
                  (std::filesystem::is_regular_file(referenced_config) &&
                   read_private_json(referenced_config, 128 * 1024) == config)),
         "QNX_QINGGAN11_DEVELOPER_IDENTITY");
    const auto spec = std::find_if(qnx_adapter_registry.begin(), qnx_adapter_registry.end(),
                                   [&](const QnxAdapterSpec &item) {
                                       return item.name == qnx_profile_adapter(config) &&
                                              item.platform == QnxGuestPlatform::qinggan11;
                                   });
    need(config.at("schema") == 1 && spec != qnx_adapter_registry.end() &&
                 qnx_qinggan11_adapter_originals_match(spec->adapter, config.at("originals")),
         "QNX_QINGGAN11_DEVELOPER_ADAPTER");
    const auto adapter = spec->adapter;
    const auto &originals = qnx_qinggan11_adapter_originals(adapter);
    std::map<std::string, std::filesystem::path> files;
    std::map<std::string, Json> epochs;
    Json inputs = Json::object(), sources = Json::object();
    const auto input_root = qnx_originals_directory(root, profile);

    for (const auto &pin : originals) {
        const auto file = input_root / pin.file;
        ordinary(file);
        need(std::filesystem::is_regular_file(file) &&
                     std::filesystem::hard_link_count(file) == 1 &&
                     std::filesystem::file_size(file) == pin.bytes,
             "QNX_QINGGAN11_DEVELOPER_ORIGINAL");
        epochs[pin.file] = cache_file_identity(file);
        need(sha256_file(file) == pin.sha256, "QNX_QINGGAN11_DEVELOPER_SHA");
        files[pin.file] = file;
        inputs[pin.file] = {{"sha256", pin.sha256}, {"bytes", pin.bytes}};
    }

    for (const auto *directory : {"src/app/qnx", "src/guest/qnx"}) {
        for (const auto &item : project_directory_files(root, directory, false)) {
            if (item.bytes < 2 * 1024 * 1024) {
                add_source(sources, root, item.name);
            }
        }
    }

    for (const auto *file : {"src/app/profiles/qnx_originals_directory.hpp",
                             "src/app/profiles/profile_identity.hpp"}) {
        add_source(sources, root, file);
    }

    if (!project_source_checkout(root)) {
        add_source(sources, root, profile_file.lexically_relative(root).generic_string());
        add_source(sources, root, config_file.lexically_relative(root).generic_string());
    }

    if (family != profile) {
        const auto referenced = std::filesystem::path("profiles") / family / "profile-qnx.json";
        sources[referenced.generic_string()] = sha256_file(root / referenced);
    }

    need(qnx_qinggan11_source_space_available(std::filesystem::space(root).available),
         "QNX_QINGGAN11_DEVELOPER_RESERVE");
    const auto hyp = read(files.at("hyp_la.img"), 8 * 1024 * 1024),
               ifs = read(files.at("ifs2_la.img"), 256 * 1024 * 1024);
    const auto host = decode_qnx_qinggan11_host(
            adapter, hyp, inputs.at("hyp_la.img").at("sha256").get<std::string>());
    const auto original_display = read_qnx_imagefs_file(ifs, "ifs_display.img"),
               disk = read_qnx_imagefs_file(ifs, "ifs_disk.img");

    if (spec->ram_driver_paddr != std::array<std::uint8_t, 2>{}) {
        validate_qnx_qinggan11_ram_driver(adapter, read_qnx_imagefs_file(disk, "bin/devb-ram"));
    }

    need(std::filesystem::file_size(files.at("system_la.img")) / 512 + 98 == 2097250,
         "QNX_QINGGAN11_DEVELOPER_SYSTEM_CAPACITY");
    auto runtime = project_file_bytes(root, "src/guest/qnx/qnx-runtime.sh", guest_script_bytes);
    const std::string marker = "\nqm_source_capture=0\n";
    const auto at = runtime.find(marker);
    need(at != std::string::npos && runtime.find(marker, at + 1) == std::string::npos,
         "QNX_QINGGAN11_DEVELOPER_CAPTURE_MARKER");

    if (spec->mcu_layout_embedded) {
        need(!qnx_qinggan11_mcu_layout(
                      project_file_json(root, "src/guest/qnx/" + std::string(spec->mcu_layout_file),
                                        256 * 1024),
                      config.value("initial_native_state", Json::object()))
                      .empty(),
             "QNX_QINGGAN11_MCU_LAYOUT_REQUIRED");
    }

    need(std::filesystem::create_directory(output), "QNX_QINGGAN11_DEVELOPER_CREATE");
    ensure_private_directory(output);
    const auto guest = build_qnx_guest_modules(
            root, read_qnx_imagefs_file(host, "lib64/libc.so.4"),
            read_qnx_imagefs_file(original_display, "lib64/libscreen.so.1"), output / "guest", true,
            adapter, config.value("initial_native_state", Json::object()));
    const auto bundle = read(output / "guest/front-bundle.so", 360000),
               observer = read(output / "guest/observer.so", 21312),
               wfd = read(output / "guest/virtual_wfd.so", 97847),
               stub = read(output / "guest/readstub.bin", 4096),
               screen_config = project_file_bytes(root, "src/guest/qnx/qnx-screen.conf", 1119);
    const auto observer_extent = read_qnx_imagefs_file(original_display, "bin/qditest").size();
    need(observer_extent == 21312 && observer.size() <= observer_extent &&
                 bundle.size() + 4095 <=
                         read_qnx_imagefs_file(original_display, "bin/Snapdragon.rgb").size() &&
                 wfd.size() <=
                         read_qnx_imagefs_file(original_display, "lib64/libopenwfd_qnx.so").size(),
         "QNX_QINGGAN11_DEVELOPER_PAYLOAD_EXTENT");
    auto padded = observer;
    padded.resize(observer_extent, 0);
    runtime = prepare_qnx_qinggan11_runtime(qnx_native_mcu_async_runtime(runtime),
                                            sha256_bytes(padded));
    runtime = prepare_qnx_guest_shell(
            qnx_kanzi_gauge_observer(config) ? qnx_kanzi_gauge_runtime(runtime) : runtime);
    auto display = prepare_qnx_qinggan11_display_config(original_display, screen_config);
    auto replace_display = [&](std::string_view name, std::string_view bytes,
                               unsigned alignment = 1, bool retain = true) {
        display = replace_qnx_imagefs_file(display, name,
                                           sha256_bytes(read_qnx_imagefs_file(display, name)),
                                           bytes, alignment, retain);
    };
    replace_display("bin/Snapdragon.rgb", bundle, 4096, false);
    replace_display("bin/qditest", observer);
    replace_display("bin/ssplash", runtime);
    replace_display("lib64/libopenwfd_qnx.so", wfd);

    for (const auto &[before, after] : std::array<std::pair<std::string_view, std::string_view>, 5>{
                 {{"proc/boot/img_splash.conf", "img_splash.conf"},
                  {"proc/boot/logo.jpg", "logo.jpg"},
                  {"lib64/dll/screen-stdbuf.so", "lib64/screen-stdbuf.so"},
                  {"lib64/dll/img_codec_jpg.so", "lib64/img_codec_jpg.so"},
                  {"lib64/dll/img_codec_png.so", "lib64/img_codec_png.so"}}}) {
        display = rename_qnx_imagefs_file(display, before, after);
    }

    auto new_ifs = replace_qnx_imagefs_file(ifs, "ifs_display.img", sha256_bytes(original_display),
                                            display);
    auto derived_host = host;
    auto replace_host = [&](std::string_view path, std::string_view bytes) {
        derived_host =
                path == "proc/boot/.script"
                        ? replace_qnx_imagefs_file_in_zero_padding(
                                  derived_host, path,
                                  sha256_bytes(read_qnx_imagefs_file(host, path)), bytes, 300)
                        : replace_qnx_imagefs_file(derived_host, path,
                                                   sha256_bytes(read_qnx_imagefs_file(host, path)),
                                                   bytes);
    };
    replace_host("proc/boot/.script",
                 adapt_qnx_qinggan11_boot_script(read_qnx_imagefs_file(host, "proc/boot/.script")));
    replace_host("proc/boot/procnto-smp-instr",
                 adapt_qnx_qinggan11_procnto_environment(
                         read_qnx_imagefs_file(host, "proc/boot/procnto-smp-instr")));
    replace_host("bin/make_symlinks",
                 adapt_qnx_qinggan11_symlinks(read_qnx_imagefs_file(host, "bin/make_symlinks")));
    replace_host("bin/startupmgr", qnx_qinggan11_developer_candidate(
                                           read_qnx_imagefs_file(host, "bin/startupmgr"), stub,
                                           sha256_bytes(display), sha256_bytes(disk)));
    const auto hyp_pin = inputs.at("hyp_la.img").at("sha256").get<std::string>();
    const auto packed = encode_qnx_qinggan11_host(adapter, hyp, hyp_pin, derived_host);
    const auto prepared = assemble_qnx_qinggan11_kernel(adapter, hyp, hyp_pin, packed);
    const auto console = read(output / "guest/qnx-pl011-console.bin", 112);
    const auto kernel = adapt_qnx_pl011_console(adapter, hyp, hyp_pin, prepared, console);
    write(output / "hyp-hvf.img", kernel);
    write(output / "host-native-lzo.bin", packed);
    write(output / "ifs2-native.img",
          qnx_qinggan11_loader_image(adapter, new_ifs, sha256_bytes(new_ifs)));
    write(output / "smem.bin", qnx_smem_memory());
    const auto gpt = qnx_qinggan11_gpt(adapter, new_ifs, sha256_bytes(new_ifs));
    validate_qnx_qinggan11_gpt(adapter, gpt, new_ifs, sha256_bytes(new_ifs));

    write(output / "synthetic-gpt.bin", gpt.cache);
    write(output / "gpt-primary.bin", gpt.primary);
    write(output / "gpt-backup.bin", gpt.backup);

    for (const auto *name : {"qnx-ipl-virt.bin", "qnx-ifs2-restore.bin"}) {
        std::filesystem::copy_file(output / "guest" / name, output / name);
    }

    const auto system = prepare_qnx_system_disk(
            files.at("system_la.img"), inputs.at("system_la.img").at("sha256").get<std::string>(),
            output / "system", qnx_qinggan11_disk_floor);
    const auto theme_manager = read_qnx6_file(
            files.at("system_la.img"), inputs.at("system_la.img").at("sha256").get<std::string>(),
            "usr/bin/themmgr");
    replace_qnx6_file(system, sha256_file(system), "usr/bin/themmgr", sha256_bytes(theme_manager),
                      prepare_qnx_qinggan11_theme_manager(adapter, theme_manager), 64 * 512);
    const auto theme_scene = read_qnx6_file(
            files.at("system_la.img"), inputs.at("system_la.img").at("sha256").get<std::string>(),
            "lib64/libclusterscene_withoutfaceid.so.0.1");
    replace_qnx6_file(system, sha256_file(system), "lib64/libclusterscene_withoutfaceid.so.0.1",
                      sha256_bytes(theme_scene),
                      prepare_qnx_qinggan11_theme_scene(adapter, theme_scene), 64 * 512);
    std::filesystem::rename(system, output / "sysla-gpt-ramdisk-v3.img");
    std::filesystem::remove(output / "system");
    Json images = Json::object();

    for (const auto *name :
         {"hyp-hvf.img", "host-native-lzo.bin", "ifs2-native.img", "smem.bin", "synthetic-gpt.bin",
          "gpt-primary.bin", "gpt-backup.bin", "qnx-ipl-virt.bin", "qnx-ifs2-restore.bin",
          "sysla-gpt-ramdisk-v3.img"}) {
        images[name] = {{"file", name},
                        {"sha256", sha256_file(output / name)},
                        {"bytes", std::filesystem::file_size(output / name)}};
    }

    for (const auto &pin : originals) {
        need(cache_file_identity(files.at(pin.file)) == epochs.at(pin.file) &&
                     sha256_file(files.at(pin.file)) == pin.sha256,
             "QNX_QINGGAN11_DEVELOPER_SOURCE_CHANGED");
    }

    need(sha256_file(config_file) == config_pin && sha256_file(profile_file) == profile_pin,
         "QNX_QINGGAN11_DEVELOPER_PROFILE_CHANGED");

    for (auto i = sources.begin(); i != sources.end(); ++i) {
        need(source_sha256(root, i.key()) == i.value().get<std::string>(),
             "QNX_QINGGAN11_DEVELOPER_CODE_CHANGED");
    }

    const Json receipt = {
            {"schema", 1},
            {"kind", "raw-qnx-source-build"},
            {"hardware_adapter", spec->name},
            {"interguest_network", qnx_interguest_contract()},
            {"mcu_projection", guest.at("mcu_projection")},
            {"simulated_initial_native_state", guest.at("simulated_initial_native_state")},
            {"profile", profile},
            {"source_generated", true},
            {"prepared_baseline_input", false},
            {"originals", inputs},
            {"guest_build", guest},
            {"images", images},
            {"source_capture", 1},
            {"observer_padded_sha256", sha256_bytes(padded)},
            {"display_sha256", sha256_bytes(display)},
            {"disk_sha256", sha256_bytes(disk)},
            {"guest_booted", false},
            {"exported_frame_verified", false}};
    write_private_json(output / "build.json", receipt);

    return receipt;
}

Json prepare_qnx_qinggan11_developer_images(const std::filesystem::path &root,
                                            std::string_view profile,
                                            const std::filesystem::path &output) {
    return prepare_qnx_qinggan11_images(root, profile, output);
}

Json prepare_qnx_passion_developer_images(const std::filesystem::path &root,
                                          std::string_view profile,
                                          const std::filesystem::path &output) {
    ordinary(root);
    ordinary(output);
    need(std::filesystem::canonical(root) == root && valid_profile_name(profile) &&
                 output.parent_path() == root / ".local/build" && !std::filesystem::exists(output),
         "QNX_PASSION_DEVELOPER_PATH");
    const auto profile_file = root / "profiles" / std::string(profile) / "profile.json",
               config_file = root / "profiles" / std::string(profile) / "profile-qnx.json";
    const auto profile_pin = sha256_file(profile_file), config_pin = sha256_file(config_file);
    const auto config = read_private_json(config_file, 128 * 1024);
    need(config.at("schema") == 1 && config.at("status") == "incomplete" &&
                 qnx_adapter(qnx_profile_adapter(config)).platform == QnxGuestPlatform::passion,
         "QNX_PASSION_DEVELOPER_ADAPTER");
    // The supplemental recovery is a qinggan09 original that the profile declares as a donor input.
    const auto donor = qnx_donor_input(root, config, "ifs2_recovery_la.img");
    const auto &supplemental_recovery = donor.file;
    const auto &recovery_pin = donor.sha256;
    ordinary(supplemental_recovery);
    struct Original {
        const char *name;
        std::uint64_t bytes;
        const char *sha;
    };
    constexpr std::array originals{
            Original{"mifs_hyp_la.img", 2254664,
                     "1e7c487aae4f4353621f8d0301b352cc974e9037d114ff3ec2556d048512ad28"},
            Original{"ifs2_la.img", 200925932,
                     "1e942ab83e59ab78ff0096f474d3677ba7e83f6a00f86a1ebe9f0ae755a3e157"},
            Original{"system_la.img", 2684354560ULL,
                     "9e6395255f9bc51d926fd5ab604612d779de93da59ba71832ee580c8534f42ec"},
            Original{"mifs_recovery_hyp_la.img", 2330096,
                     "8376e6340f719d8a4b6e572fee7a675dd7bd6e88e14c589ba2eba27ee60c239b"},
            Original{"ifs2_recovery_la.img", 32080412,
                     "1635c921b73544411a08028005b906e677d029c047d2a6e0a041598c9847709c"}};
    Json inputs = Json::object(), sources = Json::object();
    std::map<std::string, std::filesystem::path> files;
    std::map<std::string, Json> epochs;

    for (const auto &item : originals) {
        const auto file = root / ".local/profiles" / std::string(profile) / "qnx" / item.name;
        ordinary(file);
        need(config.at("originals").at(item.name).at("file") == item.name &&
                     config.at("originals").at(item.name).at("sha256") == item.sha &&
                     std::filesystem::is_regular_file(file) &&
                     std::filesystem::file_size(file) == item.bytes,
             "QNX_PASSION_DEVELOPER_ORIGINAL");
        const auto epoch = cache_file_identity(file);
        need(sha256_file(file) == item.sha && cache_file_identity(file) == epoch,
             "QNX_PASSION_DEVELOPER_INPUT_SHA");
        files[item.name] = file;
        epochs[item.name] = epoch;
        inputs[item.name] = {{"sha256", item.sha}, {"bytes", item.bytes}};
    }

    need(std::filesystem::is_regular_file(supplemental_recovery) &&
                 std::filesystem::file_size(supplemental_recovery) <= 256 * 1024 * 1024 &&
                 sha256_file(supplemental_recovery) == recovery_pin,
         "QNX_PASSION_SUPPLEMENTAL_SHA");
    const auto recovery_epoch = cache_file_identity(supplemental_recovery);

    for (const auto &item : project_directory_files(root, "src/app/qnx", false)) {
        const std::filesystem::path file = item.name;

        if (file.filename().string().find(".test.") == std::string::npos &&
            (file.extension() == ".cpp" || file.extension() == ".hpp")) {
            add_source(sources, root, item.name);
        }
    }

    for (const auto *file : {"src/guest/qnx/component.json", "src/guest/qnx/qnx-passion-runtime.sh",
                             "src/guest/qnx/qnx-screen.conf"}) {
        add_source(sources, root, file);
    }

    if (!project_source_checkout(root)) {
        add_source(sources, root, profile_file.lexically_relative(root).generic_string());
        add_source(sources, root, config_file.lexically_relative(root).generic_string());
    }

    qnx_passion_require_source_disk(std::filesystem::space(root).available, originals[2].bytes);
    const auto hyp = read(files.at("mifs_hyp_la.img"), 8 * 1024 * 1024),
               ifs = read(files.at("ifs2_la.img"), 256 * 1024 * 1024),
               host = decode_qnx_passion_host(hyp);
    const auto display = read_qnx_imagefs_file(ifs, "ifs_display.img"),
               disk = read_qnx_imagefs_file(ifs, "ifs_disk.img"),
               driver = read_qnx_imagefs_file(
                       read_qnx_imagefs_file(read(supplemental_recovery, 256 * 1024 * 1024),
                                             "ifs_recovery.img"),
                       "bin/devb-ram");
    need(driver.size() == 22165, "QNX_PASSION_SUPPLEMENTAL_DRIVER");
    const auto runtime = prepare_qnx_passion_11713_runtime(project_file_bytes(
                       root, "src/guest/qnx/qnx-passion-runtime.sh", guest_script_bytes)),
               screen_config = project_file_bytes(root, "src/guest/qnx/qnx-screen.conf", 1309);
    validate_qnx_passion_runtime(runtime);
    const auto network = qnx_passion_network_script(
            read_qnx6_file(files.at("system_la.img"), originals[2].sha, "scripts/startup.sh"),
            qnx_interguest_contract().at("address").get<std::string>());
    // The vehicle gateway contract: the cluster's SOME/IP clients and the service table the Passion
    // catalogue is generated from.
    const auto service_table =
            read_qnx6_file(files.at("system_la.img"), originals[2].sha,
                           "etc/exd/adapter_config/signaldefine/S32G_8155/8155_qnx/"
                           "H53_SOMEIP_ServiceTable_V6.0.0_OIB_8155_QNX_20240103-142433.json");
    need(sha256_bytes(service_table) == passion_map_sha256, "QNX_PASSION_SERVICE_TABLE");
    const auto gateway = qnx_passion_someip_contract(
            read_qnx6_file(files.at("system_la.img"), originals[2].sha, "usr/bin/ic_service"),
            Json::parse(service_table));
    // Our RAM device carries exactly the extent fs-qnx6 accepts (blocksize*blocks + the 0x3000
    // header and the 0x1000 alternate pair), so the capacity is pinned at this call site rather
    // than in the shared contract.
    need(runtime.find("capacity=2462832,address=0x7c008000") != std::string::npos &&
                 runtime.find("capacity=2470000") == std::string::npos,
         "QNX_PASSION_RUNTIME_DEVICE_CAPACITY");
    need(read_qnx_imagefs_file(display, "etc/dispdump.sh").size() == 5728 &&
                 read_qnx_imagefs_file(display, "bin/ssplash").size() == 21330 &&
                 read_qnx_imagefs_file(display, "bin/qditest").size() == 21313 &&
                 read_qnx_imagefs_file(display, "lib64/libopenwfd_qnx.so").size() == 97848 &&
                 read_qnx_imagefs_file(display, "etc/graphics.conf").size() == 1309 &&
                 read_qnx_imagefs_file(display, "bin/display_debug_tools.sh").size() == 25091 &&
                 read_qnx_imagefs_file(disk, "bin/devb-ufs-qualcomm").size() == 88818,
         "QNX_PASSION_DEVELOPER_SLOT_GEOMETRY");
    const auto placement = qnx_passion_11713_placement();
    need(std::filesystem::create_directory(output), "QNX_PASSION_DEVELOPER_CREATE");
    ensure_private_directory(output);
    write_private_json(output / "someip-gateway.json", gateway, 4 * 1024 * 1024);
    const auto guest = build_qnx_guest_modules(
            root, read_qnx_imagefs_file(host, "lib64/libc.so.4"),
            read_qnx_imagefs_file(display, "lib64/libscreen.so.1"), output / "guest", true,
            qnx_adapter(qnx_profile_adapter(config)).adapter);
    const auto front = read(output / "guest/front.so", 360000),
               bounded = read(output / "guest/bounded.so", 21313),
               ram64 = read(output / "guest/ram64.so", 5728),
               wfd = read(output / "guest/virtual_wfd.so", 97848),
               reader = read(output / "guest/readstub.bin", 176);
    need(front.size() + 4095 <= read_qnx_imagefs_file(display, "bin/Snapdragon.rgb").size(),
         "QNX_PASSION_FRONT_SLOT");
    auto new_display = display;
    auto replace_display = [&](std::string_view name, std::string_view bytes,
                               unsigned alignment = 1) {
        const auto before = read_qnx_imagefs_file(display, name);
        new_display = name == "bin/ssplash" && bytes.size() > before.size()
                              ? replace_qnx_imagefs_file_in_zero_padding(
                                        new_display, name, sha256_bytes(before), bytes, 4096)
                              : replace_qnx_imagefs_file(new_display, name, sha256_bytes(before),
                                                         bytes, alignment, false);
    };
    replace_display("bin/Snapdragon.rgb", front, 4096);
    replace_display("bin/qditest", bounded);
    replace_display("etc/dispdump.sh", ram64);
    need(read_qnx_imagefs_file(new_display, "etc/dispdump.sh") == ram64,
         "QNX_PASSION_RAM64_READBACK");
    replace_display("bin/ssplash", runtime);
    replace_display("lib64/libopenwfd_qnx.so", wfd);
    replace_display("etc/graphics.conf", screen_config);
    // The unused OEM debug script slot carries the generated network script the runtime sources.
    replace_display("bin/display_debug_tools.sh", network);
    auto new_disk = replace_qnx_imagefs_file(
            disk, "bin/devb-ufs-qualcomm",
            sha256_bytes(read_qnx_imagefs_file(disk, "bin/devb-ufs-qualcomm")), driver);
    new_disk = rename_qnx_imagefs_file(new_disk, "bin/devb-ufs-qualcomm", "bin/devb-ram");
    auto new_ifs = replace_qnx_imagefs_file(ifs, "ifs_display.img", sha256_bytes(display),
                                            new_display);
    new_ifs = replace_qnx_imagefs_file(new_ifs, "ifs_disk.img", sha256_bytes(disk), new_disk);
    need(new_ifs.size() == ifs.size(), "QNX_PASSION_IFS_LOGICAL_EXTENT");
    new_ifs.resize((new_ifs.size() + 4095) & ~std::size_t(4095), 0);
    need(new_ifs.size() == 200929280, "QNX_PASSION_IFS_LOGICAL_EXTENT");
    // The guest staging writer clears the final 4 KiB block of the staged image,
    // which carries the first member's checksum trailer; that member grows into a
    // spare page so the manager still finds the trailer. The GPT keeps the logical extent.
    const auto staged_ifs = pad_qnx_imagefs_first_member(new_ifs, 4096);
    need(staged_ifs.size() == 200933376 &&
                 placement.ifs_b + staged_ifs.size() < placement.gpt_backup,
         "QNX_PASSION_IFS_PADDING");
    const auto gpt = qnx_passion_11713_config_gpt(new_ifs.size());
    validate_qnx_passion_11713_config_gpt(gpt, new_ifs.size());
    auto new_host = host;
    auto replace_host = [&](std::string_view path, std::string_view bytes) {
        new_host = replace_qnx_imagefs_file(new_host, path,
                                            sha256_bytes(read_qnx_imagefs_file(host, path)), bytes);
    };
    replace_host("proc/boot/.script",
                 adapt_qnx_passion_boot_script(read_qnx_imagefs_file(host, "proc/boot/.script")));
    replace_host("proc/boot/procnto-smp-instr",
                 adapt_qnx_passion_procnto_environment(
                         read_qnx_imagefs_file(host, "proc/boot/procnto-smp-instr")));
    replace_host("bin/make_symlinks",
                 adapt_qnx_passion_symlinks(read_qnx_imagefs_file(host, "bin/make_symlinks")));
    // The 11713 config manager serves both OEM boot slots from one virtual IFS
    // volume (ifs2 name redirection) and the shared config_data cache at
    // 0xe4fc0000; the plain manager still reads the UFS GPT and fails the
    // ifs2_b partition lookup in the virtual machine.
    auto large_reader = reader;
    large_reader.replace(0x3c, 4, std::string("\x85\x9f\xbc\xd2", 4));
    const auto manager = read_qnx_imagefs_file(host, "bin/startupmgr");
    validate_qnx_passion_11713_ifs_integrity_source(manager);
    auto adapted_manager = adapt_qnx_passion_11713_config_startupmgr(manager, reader, large_reader);
    // do_mount verifies every ifs2 member against the static SHA256 registry the
    // genuine manager carries; the two members this build replaces must be
    // republished there or the verifier rejects them before mounting.
    need(adapted_manager.substr(0x2a5ec, 16) == std::string_view("ifs_display.img\0", 16) &&
                 adapted_manager.substr(0x2a6de, 13) == std::string_view("ifs_disk.img\0", 13) &&
                 adapted_manager.substr(0x2a5fc, 64) == sha256_bytes(display) &&
                 adapted_manager.substr(0x2a6eb, 64) == sha256_bytes(disk),
         "QNX_PASSION_DEVELOPER_IFS_REGISTRY_PREIMAGE");
    adapted_manager.replace(0x2a5fc, 64, sha256_bytes(new_display));
    adapted_manager.replace(0x2a6eb, 64, sha256_bytes(new_disk));
    replace_host("bin/startupmgr", adapted_manager);
    need(read_qnx_imagefs_file(new_host, "bin/startupmgr") == adapted_manager,
         "QNX_PASSION_DEVELOPER_IFS_REGISTRY_READBACK");
    // The adapted 11713 startup publishes the full RAM extent, so every OEM
    // reserved-memory range referenced by proc/boot/secpol_range.txt becomes a
    // syspage ram entry and secpolpush succeeds; the plain kernel keeps the
    // legacy RAM view, drops those ranges and the policy push fails with
    // every subsequent "on -T" launch (ENOTSUP) before startupmgr.
    const auto bootstrap = prepare_qnx_passion_11713_config_bootstrap(hyp);
    const auto packed = encode_qnx_passion_host(hyp, new_host),
               kernel = adapt_qnx_pl011_console(qnx_adapter(qnx_profile_adapter(config)).adapter,
                                                hyp, originals[0].sha,
                                                assemble_qnx_passion_11713_config_kernel(
                                                        hyp, packed, sha256_bytes(new_host)),
                                                read(output / "guest/qnx-pl011-console.bin", 112));
    need(kernel.substr(0x69108) == packed, "QNX_PASSION_PACKED_BINDING");
    const auto smem = qnx_passion_smem_memory();
    // Regression: the produced kernel must carry the adapted 11713 startup;
    // the plain legacy kernel keeps a low RAM top, publishes no OEM reserved
    // ranges and secpolpush fails before startupmgr ever runs.
    need(kernel.substr(205196, 20722) == bootstrap.startup.substr(205196 - 0x3000, 20722),
         "QNX_PASSION_DEVELOPER_11713_STARTUP");
    write(output / "hyp-hvf.img", kernel);
    write(output / "host-native-lzo.bin", packed);
    write(output / "ifs2-native.img", staged_ifs);
    write(output / "smem.bin", smem);
    write(output / "early-l2.bin", bootstrap.early_l2);
    write(output / "config-data-blank.bin", qnx_passion_11713_blank_config());
    write(output / "synthetic-gpt.bin", gpt.cache);
    write(output / "gpt-primary.bin", gpt.primary);
    write(output / "gpt-backup.bin", gpt.backup);

    for (const auto *name : {"qnx-ipl-virt.bin", "qnx-ifs2-restore.bin"}) {
        std::filesystem::copy_file(output / "guest" / name, output / name);
    }

    // Check actual generated output before allocating either system container.
    std::uint64_t generated_bytes = 0;

    for (const auto &entry : std::filesystem::recursive_directory_iterator(output)) {
        const auto status = entry.symlink_status();

        if (std::filesystem::is_directory(status)) {
            continue;
        }

        need(std::filesystem::is_regular_file(status), "QNX_PASSION_GENERATED_FILE");
        const auto bytes = entry.file_size();
        need(bytes <= qnx_passion_source_generated_cap - generated_bytes,
             "QNX_PASSION_GENERATED_CAP");
        generated_bytes += bytes;
    }

    qnx_passion_require_source_disk(std::filesystem::space(root).available + generated_bytes,
                                    originals[2].bytes);
    const auto system = prepare_qnx_system_disk(files.at("system_la.img"), originals[2].sha,
                                                output / "system", qnx_passion_source_reserve);
    const auto whole_sha = sha256_file(system);
    const std::uint64_t whole_bytes = std::filesystem::file_size(system);
    need(whole_bytes == placement.system_sectors * 512, "QNX_PASSION_SYSTEM_EXTENT");
    const auto parts = split_qnx_loader_image(system, whole_sha, output / "system-parts",
                                              1ULL << 30, qnx_passion_source_reserve);
    need(parts.size() == 3, "QNX_PASSION_SYSTEM_PART_COUNT");
    Json system_parts = Json::array(), images = Json::object();

    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto &part = parts[i];
        need(part.offset == i * (1ULL << 30) &&
                     part.bytes == (i < 2 ? (1ULL << 30) : 536921088ULL) &&
                     placement.system + part.offset + part.bytes <= placement.ipl_map_end,
             "QNX_PASSION_SYSTEM_PART_EXTENT");
        const auto name = part.file.lexically_relative(output).generic_string();
        images[name] = {{"sha256", part.sha256}, {"bytes", part.bytes}};
        system_parts.push_back({{"file", name},
                                {"offset", part.offset},
                                {"address", placement.system + part.offset},
                                {"bytes", part.bytes},
                                {"sha256", part.sha256}});
    }

    need(std::filesystem::remove(system) && std::filesystem::remove(output / "system"),
         "QNX_PASSION_SYSTEM_TEMP_REMOVE");

    for (const auto *name :
         {"hyp-hvf.img", "host-native-lzo.bin", "ifs2-native.img", "smem.bin", "early-l2.bin",
          "config-data-blank.bin", "synthetic-gpt.bin", "gpt-primary.bin", "gpt-backup.bin",
          "qnx-ipl-virt.bin", "qnx-ifs2-restore.bin", "someip-gateway.json"}) {
        images[name] = {{"sha256", sha256_file(output / name)},
                        {"bytes", std::filesystem::file_size(output / name)}};
    }

    for (const auto &item : originals) {
        need(cache_file_identity(files.at(item.name)) == epochs.at(item.name) &&
                     sha256_file(files.at(item.name)) == item.sha,
             "QNX_PASSION_DEVELOPER_SOURCE_CHANGED");
    }

    need(cache_file_identity(supplemental_recovery) == recovery_epoch &&
                 sha256_file(supplemental_recovery) == recovery_pin,
         "QNX_PASSION_SUPPLEMENTAL_CHANGED");
    need(sha256_file(profile_file) == profile_pin && sha256_file(config_file) == config_pin,
         "QNX_PASSION_DEVELOPER_PROFILE_CHANGED");

    for (auto i = sources.begin(); i != sources.end(); ++i) {
        need(source_sha256(root, i.key()) == i.value().get<std::string>(),
             "QNX_PASSION_DEVELOPER_CODE_CHANGED");
    }

    for (auto at = images.begin(); at != images.end(); ++at) {
        at.value()["file"] = at.key();
    }

    const Json receipt = {
            {"schema", 1},
            {"kind", "raw-qnx-source-build"},
            {"system_image",
             {{"sha256", whole_sha}, {"bytes", whole_bytes}, {"parts", system_parts}}},
            {"profile", profile},
            {"source_generated", true},
            {"prepared_baseline_input", false},
            {"originals", inputs},
            {"supplemental_original",
             {{"kind", "owner-captured-qinggan09-recovery-not-passion"},
              {"file", supplemental_recovery.string()},
              {"sha256", recovery_pin},
              {"driver_sha256", sha256_bytes(driver)},
              {"runtime_abi_verified", false}}},
            {"guest_build", guest},
            {"images", images},
            {"hardware_adapter", qnx_profile_adapter(config)},
            {"interguest_network", qnx_interguest_contract()},
            {"memory_mib", 11713},
            {"runtime_sha256", sha256_bytes(runtime)},
            {"network_script_sha256", sha256_bytes(network)},
            {"source_capture", 1},
            {"logical_ifs_bytes", ifs.size()},
            {"gpt_block_bytes", 4096},
            {"gpt_blocks", placement.gpt_blocks},
            {"gpt_primary_address", placement.gpt_prefix},
            {"gpt_backup_address", placement.gpt_backup},
            {"gpt_cache_address", placement.gpt_cache},
            {"system_address", placement.system},
            {"system_raw_sectors", placement.system_raw_sectors},
            {"original_android_vm_allowed", false},
            {"guest_booted", false},
            {"exported_frame_verified", false},
            {"mcu_verified", false}};
    write_private_json(output / "build.json", receipt);

    return receipt;
}
} // namespace voyemu
