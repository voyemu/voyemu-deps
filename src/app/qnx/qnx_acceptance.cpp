#include "qnx_acceptance.hpp"
#include "capture_png.hpp"
#include "check.hpp"
#include "error.hpp"
#include "frame_transport.hpp"
#include "ordinary_path.hpp"
#include "os/fs.hpp"
#include "process.hpp"
#include "progress.hpp"
#include "project_files.hpp"
#include "qnx6_file.hpp"
#include "qnx_guest_build.hpp"
#include "qnx_originals_directory.hpp"
#include "qnx_persistent_storage.hpp"
#include "qnx_process_ram.hpp"
#include "qnx_run_budget.hpp"
#include "qnx_runtime_layout.hpp"
#include "quickboot.hpp"
#include "session.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <thread>
namespace voyemu {
namespace {
using Clock = std::chrono::steady_clock;

constexpr Check need{"QNX_PACKAGE_ACCEPTANCE"};

void ordinary(const std::filesystem::path &p) {
    ordinary_path(p, need, PathForm::NormalizedCanonical, "canonical input required",
                  "linked input");
}

void inside(const std::filesystem::path &root, const std::filesystem::path &path) {
    auto rel = path.lexically_relative(root);

    need(!rel.empty() && rel != "." && !rel.is_absolute(), "project descendant required");

    for (const auto &part : rel) {
        need(part != "..", "path leaves project");
    }
}

std::string checked(const std::filesystem::path &file, const std::string &pin,
                    std::uint64_t maximum = 2ULL << 30) {
    ordinary(file);
    need(sha256_text(pin) && std::filesystem::is_regular_file(file) &&
                 std::filesystem::hard_link_count(file) == 1 &&
                 std::filesystem::file_size(file) <= maximum,
         "pinned bounded ordinary input required");
    const auto epoch = cache_file_identity(file);
    need(sha256_file(file) == pin && cache_file_identity(file) == epoch, "input pin/epoch differs");

    return pin;
}

Json run(const std::filesystem::path &root, const std::string &profile,
         const std::filesystem::path &request_file, const std::filesystem::path &output,
         const std::function<bool()> &cancelled) {
    ordinary(root);
    need(valid_profile_name(profile) &&
                 (!project_source_checkout(root) ||
                  std::filesystem::is_regular_file(root / "src/app/qnx/qnx_prepare.cpp")),
         "source project/profile required");
    ordinary(request_file);
    inside(root / ".local", request_file);
    need(std::filesystem::file_size(request_file) < 128 * 1024, "bounded request");
    const auto request_pin = sha256_file(request_file);
    const auto request = read_private_json(request_file, 128 * 1024);
    need(request.at("schema") == 1 && request.at("profile") == profile, "request identity");
    const auto profile_file = root / "profiles" / profile / "profile.json";
    ordinary(profile_file);
    const auto profile_pin = checked(profile_file, sha256_file(profile_file), 1 << 20);
    const auto source_file =
            std::filesystem::path(request.at("source_receipt").at("file").get<std::string>());
    inside(root / ".local", source_file);
    const auto source_pin = checked(
            source_file, request.at("source_receipt").at("sha256").get<std::string>(), 1 << 20);
    const auto source = read_private_json(source_file, 1 << 20);
    need(source.at("schema") == 1 && source.at("kind") == "raw-qnx-source-build" &&
                 source.at("source_generated") == true &&
                 source.at("prepared_baseline_input") == false && source.at("profile") == profile,
         "full raw source build receipt required");
    const auto qnx_profile = root / "profiles" / profile / "profile-qnx.json";
    ordinary(qnx_profile);
    const auto qnx_pin = checked(qnx_profile, sha256_file(qnx_profile), 128 * 1024);
    const auto family = runtime_profile_reference(read_private_json(profile_file, 1 << 20),
                                                  profile);
    need(valid_profile_name(family), "valid QNX guest family required");
    const auto layout = qnx_runtime_layout(family, source);
    const bool passion = layout.adapter == "sa8155-passion-21480";
    const auto qnx_definition = read_private_json(qnx_profile, 128 * 1024);
    need(qnx_definition.at("hardware_adapter") == layout.adapter,
         "source adapter differs from profile");
    const auto original_definitions = qnx_definition.at("originals");
    need(source.at("originals").size() == layout.originals.size() &&
                 original_definitions.size() == layout.originals.size(),
         "exact original firmware roles required");

    if (passion || !layout.projection.is_null()) {
        const auto &guest_build = source.at("guest_build");
        need(guest_build.at("hardware_adapter") == layout.adapter &&
                     (layout.projection.is_null()
                              ? guest_build.at("mcu_projection").is_null()
                              : bool(qnx_mcu_delivery_scope(guest_build, layout.projection))),
             "guest MCU projection differs");
    }

    for (const auto &name : layout.originals) {
        const auto pin = source.at("originals").at(name).at("sha256").get<std::string>();
        need(original_definitions.at(name).at("file") == name &&
                     original_definitions.at(name).at("sha256") == pin,
             "original profile pin differs");
        checked(qnx_originals_directory(root, profile) / name, pin, 4ULL << 30);
    }

    if (passion) {
        const auto &extra = source.at("supplemental_original");
        const std::filesystem::path file = extra.at("file").get<std::string>();
        inside(root / ".local", file);
        need(extra.at("kind") == "owner-captured-qinggan09-recovery-not-passion" &&
                     extra.at("sha256") ==
                             "d7dd2e04b36d760dd8e95a939e194530e66b24b8ffe77a5f7290684145e76930" &&
                     extra.at("driver_sha256") ==
                             "53c732c493495256d16ebe39ab0e0a0f2bbdce3b600b45b5e7d770bd02d4f780",
             "exact Passion supplemental driver source required");
        checked(file, extra.at("sha256").get<std::string>(), 4ULL << 30);
    }

    need(source.at("guest_build").at("modules_digest") == qnx_guest_modules_digest(layout.adapter),
         "guest modules differ from their pins");

    need(output.parent_path() == root / ".local/build" && !output.filename().empty() &&
                 !std::filesystem::exists(output) && !std::filesystem::is_symlink(output),
         "fresh own build output required");
#ifndef _WIN32
    need(std::filesystem::canonical(std::filesystem::temp_directory_path()) ==
                 root / ".local/build/tmp",
         "TMPDIR must be project .local/build/tmp for short owned sockets");
#endif
    need(request.at("images").is_object() && request.at("images").size() == layout.images.size(),
         "exact QNX image roles required");

    for (const auto &name : layout.images) {
        const auto &item = request.at("images").at(name);
        const std::filesystem::path file = item.at("file").get<std::string>();
        inside(root / ".local", file);
        need(file == source_file.parent_path() / name,
             "image must belong to this raw source build");
        const auto pin = item.at("sha256").get<std::string>();
        checked(file, pin);
        need(source.at("images").at(name).at("file") == name &&
                     source.at("images").at(name).at("sha256") == pin &&
                     source.at("images").at(name).at("bytes") == std::filesystem::file_size(file),
             "image differs from raw source receipt");
    }

    const auto &tools = request.at("tools");
    const auto qemu = std::filesystem::path(tools.at("qemu").at("file").get<std::string>()),
               graphics = std::filesystem::path(tools.at("graphics").at("file").get<std::string>()),
               angle = std::filesystem::path(tools.at("angle").get<std::string>());
#ifdef _WIN32
    const std::string executable_suffix = ".exe";
    const std::array<const char *, 2> angle_libraries{"libEGL.dll", "libGLESv2.dll"};
#elif defined(__APPLE__)
    const std::string executable_suffix;
    const std::array<const char *, 2> angle_libraries{"libEGL.dylib", "libGLESv2.dylib"};
#else
    const std::string executable_suffix;
    const std::array<const char *, 2> angle_libraries{"libEGL.so", "libGLESv2.so"};
#endif
    constexpr bool windows = os::host_windows;
    need(qemu.is_absolute() && qemu.filename() == "qemu-system-aarch64" + executable_suffix,
         "explicit pinned installed QNX engine required");
    const auto qemu_pin = checked(qemu, tools.at("qemu").at("sha256").get<std::string>(),
                                  256ULL << 20);
    inside(root, graphics);
    checked(graphics, tools.at("graphics").at("sha256").get<std::string>(), 256ULL << 20);
    inside(root, angle);
    ordinary(angle);
    Json pins = Json::array(
            {{{"path", qemu.string()}, {"sha256", qemu_pin}},
             {{"path", graphics.string()}, {"sha256", tools.at("graphics").at("sha256")}}});

    for (const auto *lib : angle_libraries) {
        const auto file = angle / lib;
        const auto pin = tools.at("libraries").at(lib).get<std::string>();
        checked(file, pin, 256ULL << 20);
        pins.push_back({{"path", file.string()}, {"sha256", pin}});
    }

#ifdef _WIN32
    const unsigned qmp_port = request.value("qmp_port", 5678u);
    need(qmp_port >= 1024 && qmp_port <= 65535 && tcp_port_free(qmp_port),
         "fresh owned QMP port required");
#endif
    const auto capability = tools.at("qemu").value("qnx_file_backend", Json::object());
    const bool verified_file_backend = windows && qnx_file_backend_supported(capability, qemu_pin);
    need(!windows || verified_file_backend,
         "verified selected Windows QNX engine capability required");

    if (cancelled && cancelled()) {
        throw VoyemuError("QNX_PACKAGE_ACCEPTANCE", "cancelled before launch");
    }

    std::vector<std::uint64_t> copy_bytes{std::filesystem::file_size(source_file)};

    for (const auto &name : layout.images) {
        copy_bytes.push_back(std::filesystem::file_size(
                request.at("images").at(name).at("file").get<std::string>()));
    }

    qnx_acceptance_require_disk(std::filesystem::space(root / ".local").available, copy_bytes,
                                layout.memory_mib);
    ensure_private_directory(output.parent_path());
    need(std::filesystem::create_directory(output), "exclusive output creation");
    ensure_private_directory(output / "qnx");
    const auto nonce = random_token();
    write_private_json(output / ".owner.json",
                       {{"nonce", nonce}, {"kind", "qnx-package-acceptance"}});
    Json exported = Json::object();

    for (const auto &name : layout.images) {
        const auto &item = request.at("images").at(name);
        const std::filesystem::path file = item.at("file").get<std::string>();
        const auto target = output / "qnx" / name;
        ensure_private_directory(target.parent_path());
        need(std::filesystem::copy_file(file, target, std::filesystem::copy_options::none),
             "guest image copy failed");
        checked(target, item.at("sha256").get<std::string>());
        exported[name] = {{"file", "qnx/" + std::string(name)},
                          {"sha256", item.at("sha256")},
                          {"bytes", std::filesystem::file_size(target)}};
        pins.push_back({{"path", target.string()}, {"sha256", item.at("sha256")}});
    }

    need(std::filesystem::copy_file(source_file, output / "source-receipt.json",
                                    std::filesystem::copy_options::none),
         "source receipt copy");
    checked(output / "source-receipt.json", source_pin, 1 << 20);
    const auto directory = root / ".local/machines" / ("qnx-acceptance-" + profile) / "sessions" /
                           nonce;
    need(!std::filesystem::exists(directory), "fresh nonce session required");
    ensure_private_directory(directory);
    const auto ram_launch =
            layout.persistence.is_null()
                    ? qnx_ram_launch(host_qnx_engine, layout.memory_mib)
                    : qnx_persistent_ram_launch(
                              host_qnx_engine, (output / "qnx/sysla-gpt-ramdisk-v3.img").string(),
                              (output / "qnx/userdata-qnx6.img").string(), verified_file_backend);
    std::vector<std::string> args{"-machine", ram_launch.machine};
#ifdef _WIN32
    const Json qmp = {{"host", "127.0.0.1"}, {"port", qmp_port}};
    const auto qmp_argument = "tcp:127.0.0.1:" + std::to_string(qmp_port) + ",server=on,wait=off";
#else
    const Json qmp = "${RUNTIME}/${TOKEN}-qmp.sock";
    const std::string qmp_argument = "unix:${RUNTIME}/${TOKEN}-qmp.sock,server=on,wait=off";
#endif
    args.insert(args.end(),
                {"-accel", ram_launch.accelerator, "-cpu", qnx_engine_cpu(host_qnx_engine)});
    args.insert(args.end(), {"-m", std::to_string(layout.memory_mib), "-smp", "8", "-nodefaults",
                             "-display", "none", "-qmp", qmp_argument, "-nic", "none", "-no-reboot",
                             "-kernel", (output / "qnx/hyp-hvf.img").string()});
    const auto &loaders = layout.loaders;

    for (const auto &[name, address] : loaders) {
        if (layout.persistence.is_null() || name != "sysla-gpt-ramdisk-v3.img") {
            args.insert(args.end(), {"-device", "loader,file=" + (output / "qnx" / name).string() +
                                                        ",addr=" + address + ",force-raw=on"});
        }
    }

    args.insert(args.end(), {"-device",
                             "loader,file=" + (output / "qnx/qnx-ipl-virt.bin").string() +
                                     ",addr=0x40200000,force-raw=on,cpu-num=0",
                             "-serial", "file:${SESSION}/qnx-serial.log", "-S"});
    args.insert(args.end(), ram_launch.options.begin(), ram_launch.options.end());
    Json launch = {{"schema", 1},
                   {"pins", pins},
                   {"qemu", {{"executable", qemu.string()}, {"args", args}}},
                   {"graphics", {{"executable", graphics.string()}, {"angle", angle.string()}}},
                   {"ram_backend", ram_launch.backend},
                   {"ram", "${SESSION}/qnx-ram.bin"},
                   {"console", "${SESSION}/qnx-console.log"},
                   {"qmp", qmp},
                   {"guest_arch", "aarch64"},
                   {"ready_ring_address", 1342242816},
                   {"ready_ring_bytes", 262144}};

    if (verified_file_backend) {
        launch["qnx_file_backend"] = capability;
    }

    if (tools.at("graphics").contains("backend")) {
        need(tools.at("graphics").at("backend") == "voyemu", "unknown unified graphics backend");
        launch["graphics"]["backend"] = "voyemu";
    }

    qnx_runtime_launch_metadata(launch, layout);

    if (!layout.persistence.is_null()) {
        launch["persistent_storage"] = {
                {"schema", 1},
                {"system", (output / "qnx/sysla-gpt-ramdisk-v3.img").string()},
                {"userdata", (output / "qnx/userdata-qnx6.img").string()}};
    }

    write_private_json(output / "cold-launch.json", launch);
    Json evidence = {
            {"schema", 1},
            {"profile", profile},
            {"session", directory.string()},
            {"source_receipt_sha256", source_pin},
            {"launch_sha256", sha256_file(output / "cold-launch.json")},
            {"actual_exported_frame", false},
            {"cold_boot_verified", false},
            {"guest_ack", false},
            {"input_note",
             layout.projection.is_null()
                     ? "four original firmware partitions and pinned guest modules; this fresh cold run verifies the resulting images"
                     : "three original firmware partitions and pinned Free A11 guest modules; this fresh cold run verifies the resulting images"}};
    const auto started = Clock::now();
    std::unique_ptr<Session> session;
    try {
        const auto definition = read_private_json(profile_file, 1 << 20);
        const auto catalog = vehicle_catalog(definition.at("platform").get<std::string>());
        session = std::make_unique<Session>(directory, nonce, std::filesystem::path{}, catalog);
        const auto profile_values = definition.value("initial_state", Json::object());

        if (!profile_values.empty()) {
            const auto seeded = session->handle({{"op", "patch"}, {"values", profile_values}});
            need(seeded.value("ok", false), "profile startup state failed");
            evidence["startup_profile_values"] = profile_values;
        }

        auto initial_launch = launch;
        initial_launch["source_acceptance"] = true;

        if (!layout.persistence.is_null()) {
            initial_launch["qemu"]["args"].push_back("-device");
            initial_launch["qemu"]["args"].push_back(
                    "loader,addr=0x867ff000,data=0x315241564d45564f,data-len=8");
            evidence["userdata_initialization"] = {
                    {"kind", "exclusive-zero-seed"},
                    {"seed_sha256", exported.at("userdata-qnx6.img").at("sha256")},
                    {"marker_address", 0x867ff000ULL}};
        }

        // The cold boot has no time limit: it ends with the first frame, with the exit of the
        // owned QNX or graphics process, or with Ctrl+C.
        session->configure_cluster(initial_launch);
        session->attach_cluster(initial_launch);
        frames::Reader reader(directory / "frame.bgra");
        bool captured = false;
        Json stable_owner;
        std::string initial_png_sha;
        std::optional<Clock::time_point> bright_since;
        std::uint64_t first_bright_sequence = 0, probe_revision = 0;
        Clock::time_point probe_sent{};
        progress::Elapsed elapsed("cold_boot_cluster");

        while (!session->stopped()) {
            elapsed.tick();

            if (cancelled && cancelled()) {
                throw VoyemuError("QNX_PACKAGE_ACCEPTANCE", "cancelled during cold boot");
            }

            session->tick();
            const auto status = session->handle({{"op", "status"}});
            const auto &qnx = status.at("deliveries").at("qnx");
            need(qnx.value("status", std::string{}) != "exited" &&
                         qnx.value("status", std::string{}) != "graphics_worker_exited",
                 "owned QNX/graphics exited before frame");
            const bool ack = qnx.contains("vehicle") &&
                             qnx.at("vehicle").value("status", std::string{}) ==
                                     "guest_acknowledged";

            if (ack && std::filesystem::is_regular_file(directory / "frame-owner.json")) {
                const auto owner = read_private_json(directory / "frame-owner.json", 4096);
                need(owner.at("source") == "original-OEM-Screen-BMP",
                     "frame is not original QNX Screen");

                if (!layout.projection.is_null()) {
                    if (stable_owner.is_null()) {
                        stable_owner = owner;
                    } else {
                        need(owner == stable_owner, "Screen producer changed during acceptance");
                    }
                }

                const auto producer = owner.at("producer_address");
                need(qnx.contains("producers") &&
                             std::find(qnx.at("producers").begin(), qnx.at("producers").end(),
                                       producer) != qnx.at("producers").end(),
                     "frame producer not owned by this session");

                if (const auto frame = reader.read_latest()) {
                    if (!layout.projection.is_null() &&
                        !qnx_qinggan11_post_ack_frame(qnx, owner, stable_owner, frame->timestamp_ns,
                                                      probe_revision, layout.projection)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        continue;
                    }

                    need(frame->width == 1920 && frame->height == 720 &&
                                 frame->bgra.size() == frames::pixel_bytes && frame->sequence > 0,
                         "complete native Screen frame geometry");
                    std::size_t bright = 0;

                    for (std::size_t i = 0; i < frame->bgra.size(); i += 4) {
                        if (std::to_integer<unsigned char>(frame->bgra[i]) > 64 ||
                            std::to_integer<unsigned char>(frame->bgra[i + 1]) > 64 ||
                            std::to_integer<unsigned char>(frame->bgra[i + 2]) > 64) {
                            ++bright;
                        }
                    }

                    if (qnx_native_frame_nonblank(frame->bgra, frame->width, frame->height)) {
                        if (!bright_since) {
                            bright_since = Clock::now();
                            first_bright_sequence = frame->sequence;
                        }
                    } else {
                        bright_since.reset();
                    }

                    if (bright_since && frame->sequence > first_bright_sequence &&
                        Clock::now() - *bright_since >= std::chrono::seconds(5)) {
                        if (!probe_revision) {
                            const auto pixels = std::span(
                                    reinterpret_cast<const std::uint8_t *>(frame->bgra.data()),
                                    frame->bgra.size());
                            voyemu::output::write_artifact(
                                    output / "cluster-initial.png",
                                    voyemu::output::png_frame(pixels, 1920, 720, true));
                            initial_png_sha = sha256_file(output / "cluster-initial.png");
                            const bool free = !layout.projection.is_null() && !passion;
                            Json probe = concept_patch(catalog, VehicleConcept::speed,
                                                       free ? 42 : 37);
                            probe.update(
                                    concept_patch(catalog, VehicleConcept::charge, free ? 55 : 65));

                            if (free) {
                                probe.update(
                                        concept_patch(catalog, VehicleConcept::door_front_left, 1));
                            }

                            const auto patched =
                                    session->handle({{"op", "patch"}, {"values", probe}});
                            need(patched.value("ok", false), "input probe patch failed");
                            probe_revision = patched.at("revision").get<std::uint64_t>();
                            probe_sent = Clock::now();
                            evidence["input_probe"] = {{"values", probe},
                                                       {"revision", probe_revision}};
                        }

                        if (qnx.at("vehicle").value(layout.projection.is_null() ? "sequence"
                                                                                : "source_sequence",
                                                    std::uint64_t{}) < probe_revision ||
                            Clock::now() - probe_sent < std::chrono::seconds(5)) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }

                        evidence["input_probe"]["guest_acknowledged"] = true;
                        const auto pixels = std::span(
                                reinterpret_cast<const std::uint8_t *>(frame->bgra.data()),
                                frame->bgra.size());
                        const auto png = voyemu::output::png_frame(pixels, 1920, 720, true);
                        voyemu::output::write_artifact(output / "cluster.png", png);

                        if (!layout.projection.is_null()) {
                            evidence["input_probe"].update(
                                    {{"pixels_changed",
                                      sha256_file(output / "cluster.png") != initial_png_sha},
                                     {"visible_effect_verified", false},
                                     {"scope",
                                      "typed" + layout.projection.at("projected_fields").dump() +
                                              " ACK and owned OEM frame after ACK; visual values require inspection"},
                                     {"frame_timestamp_ns", frame->timestamp_ns}});
                        }

                        evidence["actual_exported_frame"] = true;
                        evidence["guest_ack"] = true;
                        evidence["frame"] = {{"file", (output / "cluster.png").string()},
                                             {"sha256", sha256_file(output / "cluster.png")},
                                             {"sequence", frame->sequence},
                                             {"bright_pixels", bright},
                                             {"stable_bright_ms",
                                              std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      Clock::now() - *bright_since)
                                                      .count()},
                                             {"width", 1920},
                                             {"height", 720},
                                             {"source", owner.at("source")},
                                             {"producer_address", producer}};
                        evidence["live_status"] = {{"qnx", qnx},
                                                   {"qnx_pid", status.at("qnx_pid")},
                                                   {"revision", status.at("revision")}};
                        evidence["cold_boot_verified"] = qnx.at("quickboot").at("mode") == "cold";
                        captured = true;
                        break;
                    }
                }
            }

            need(qnx_acceptance_disk_reserve_available(
                         std::filesystem::space(root / ".local").available),
                 "disk reserve reached");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }

        need(captured && evidence.at("cold_boot_verified") == true,
             "cold boot did not export an actual Screen frame");
        elapsed.done("cold_boot_cluster_done");
        session->stop();
        const auto stopped = session->handle({{"op", "status"}});
        evidence["terminal_status"] = {{"qnx", stopped.at("deliveries").at("qnx")},
                                       {"qnx_pid", stopped.at("qnx_pid")},
                                       {"stopped", stopped.at("stopped")}};
        need(stopped.at("stopped") == true && !std::filesystem::exists(directory / "qnx-ram.bin"),
             "owned QNX shutdown/RAM cleanup incomplete");

        for (const auto &name : layout.images) {
            auto &item = exported.at(name);
            const auto file = output / item.at("file").get<std::string>();

            if (!layout.persistence.is_null() && name == "userdata-qnx6.img") {
                need(std::filesystem::file_size(file) == (32ULL << 20),
                     "formatted userdata extent differs");
                validate_qnx6_image(file);
                const auto pin = sha256_file(file);
                item["sha256"] = pin;
                evidence["userdata_initialization"]["formatted_sha256"] = pin;

                for (auto &payload : launch["pins"]) {
                    if (payload.at("path") == file.string()) {
                        payload["sha256"] = pin;
                    }
                }
            } else if (qnx_adapter(layout.adapter).platform == QnxGuestPlatform::qinggan09 &&
                       !layout.persistence.is_null() && name == "sysla-gpt-ramdisk-v3.img") {
                need(std::filesystem::file_size(file) == (1025ULL << 20),
                     "initialized system extent differs");
                const auto &seed = request.at("images").at(name);
                const auto pin = sha256_file(file), seed_pin = seed.at("sha256").get<std::string>();
                const auto xml = validate_qnx6_system_initialization(
                        seed.at("file").get<std::string>(), seed_pin, file, pin);
                evidence["system_initialization"] = {
                        {"kind", "once-created-graphics-supervisor-override"},
                        {"seed_sha256", seed_pin},
                        {"stopped_sha256", pin},
                        {"outside_partition_unchanged", true},
                        {"config",
                         {{"path", "etc/clsboot/boot_cfg.xml"},
                          {"sha256", sha256_bytes(xml)},
                          {"bytes", xml.size()},
                          {"mode", "100600"},
                          {"regular", true}}},
                        {"scope",
                         "stopped owned QNX6 filesystem; directory/journal metadata may change"}};
                item["sha256"] = pin;

                for (auto &payload : launch["pins"]) {
                    if (payload.at("path") == file.string()) {
                        payload["sha256"] = pin;
                    }
                }
            } else {
                checked(file, item.at("sha256").get<std::string>());
            }
        }

        if (!layout.persistence.is_null()) {
            write_private_json(output / "cold-launch.json", launch);
            evidence["launch_sha256"] = sha256_file(output / "cold-launch.json");
        }

        checked(request_file, request_pin, 128 * 1024);
        checked(profile_file, profile_pin, 1 << 20);
        checked(qnx_profile, qnx_pin, 128 * 1024);
        checked(source_file, source_pin, 1 << 20);
        evidence["elapsed_ms"] =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started)
                        .count();
        write_private_json(output / "cold-boot-evidence.json", evidence);
        Json accepted = {
                {"schema", 1},
                {"kind", "accepted-qnx-portable-seed"},
                {"profile", profile},
                {"directory", output.string()},
                {"source_built", true},
                {"source_build_version", "raw-qnx-source-build"},
                {"source_bundle_sha256", source.at("guest_build").at("bundle_sha256")},
                {"current_sources_rebuilt", true},
                {"cold_boot_verified", true},
                {"stopped", true},
                {"images", exported},
                {"source_receipt",
                 {{"file", (output / "source-receipt.json").string()}, {"sha256", source_pin}}},
                {"cold_boot_evidence",
                 {{"file", (output / "cold-boot-evidence.json").string()},
                  {"sha256", sha256_file(output / "cold-boot-evidence.json")}}},
                {"tcg_verified", host_qnx_engine != QnxEngine::hvf}};
        qnx_runtime_metadata(accepted, layout);
        write_private_json(output / "acceptance.json", accepted);

        return accepted;
    } catch (const std::exception &error) {
        evidence["error"] = error.what();

        // The acceptance images are disposable: a failed run, including an unknown guest sync,
        // ends its owned QEMU and graphics and removes their RAM and image copies.
        if (session) {
            try {
                session->discard();
                const auto failed_status = session->handle({{"op", "status"}});
                evidence["terminal_status"] = {{"qnx", failed_status.at("deliveries").at("qnx")},
                                               {"stopped", failed_status.at("stopped")}};
            } catch (const std::exception &cleanup) {
                evidence["cleanup_error"] = cleanup.what();
            }
        }

        std::error_code removal;
        std::filesystem::remove_all(output / "qnx", removal);
        evidence["disposable_images_removed"] = !removal;
        evidence["accepted"] = false;
        write_private_json(output / "cold-boot-evidence.json", evidence);
        throw;
    }
}
} // namespace

Json accept_qnx_images(const std::filesystem::path &root, const std::string &profile,
                       const std::filesystem::path &request, const std::filesystem::path &output,
                       std::function<bool()> cancelled) {
    return run(root, profile, request, output, cancelled);
}
} // namespace voyemu
