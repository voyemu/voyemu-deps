#include "android_java.hpp"
#include "portable_fonts.hpp"
#include "profile_build_internal.hpp"
#include "project_files.hpp"

// Upgrades, finalizes and activates built profiles and their frontend and UI observer bindings.
namespace voyemu {
using namespace profile_build_detail;

Json upgrade_profile_android_system(const std::filesystem::path &root, std::string_view profile,
                                    const MachineIdentity &machine,
                                    const std::filesystem::path &android,
                                    const std::string &receipt_pin,
                                    const std::filesystem::path &artifact_receipt,
                                    const std::string &artifact_receipt_pin) {
    const auto artifact_lease = artifact_receipt.empty() ? candidate_artifact_reader(root, android)
                                                         : artifact_reader(root, artifact_receipt,
                                                                           artifact_receipt_pin);
    const auto image = validate_profile_android_system_candidate(root, profile, android,
                                                                 receipt_pin);

    need(!read_private_json(android / "receipt.json", 1 << 20).contains("original_navi_system"),
         "original Navi requires an explicitly reviewed clean migration; existing userdata upgrade is unsupported");
    const auto target = build_target(root, profile, machine);
    ordinary(target);
    need(std::filesystem::is_directory(target),
         "existing accepted machine required for system-only upgrade");
    const auto expected = hash(target / "runtime.json", 128 * 1024);
    const auto accepted = inspect_portable_runtime(root, profile, machine);
    need(hash(target / "runtime.json", 128 * 1024) == expected,
         "accepted manifest changed during system upgrade preflight");
    install_native_dependencies(root, true);
    const auto sdk = root / ".local/toolchains/native-sdk",
               tool = sdk / "emulator" / ("qemu-img" + exe());
    const auto pins = host_pins(sdk);
    const Json disk_tool = {{"path", tool.string()}, {"sha256", pins.at(tool.string())}};
    const auto candidate = read_private_json(android / "receipt.json", 1 << 20);
    const auto definition = read_private_json(authoring_definition(root, profile), 1 << 20);
    auto package = candidate.value("owned_package", Json());
    auto camera = candidate.value("camera_package", Json());
    auto prepared = candidate.value("prepared_packages", Json());
    auto system_directory = android;

    if (!prepared.is_null()) {
        need(qinggan11_package_delivery(definition) && candidate.at("kind") == "android-candidate",
             "prepared delivery requires exact cold Free A11 candidate");
        android_prepared_packages_descriptor(prepared, true);

        for (auto &item : prepared) {
            item["file"] = (android / item.at("file").get<std::string>()).string();
        }
    }

    if (!camera.is_null()) {
        need(qinggan11_package_delivery(definition) && candidate.at("kind") == "android-candidate",
             "Camera delivery requires exact cold Free A11 candidate");
        android_camera_package_descriptor(camera, true);
        camera["file"] = (android / camera.at("file").get<std::string>()).string();
    }

    if (candidate.at("kind") == "android-owned-package-upgrade") {
        system_directory = candidate.at("base_candidate").at("directory").get<std::string>();
        const auto artifacts = upgrade_pinned_json(root, candidate.at("artifacts"), 16 << 20);
        package["file"] = artifacts.at("artifacts").at("component:wheel-input").at("file");
    } else if (!package.is_null()) {
        package["file"] = (android / package.at("file").get<std::string>()).string();
    }

    const auto media = source_system_media_artifact(root, profile, android, receipt_pin,
                                                    artifact_receipt, artifact_receipt_pin);
    const auto camera_source = source_camera_artifact(root, profile, android, receipt_pin,
                                                      artifact_receipt, artifact_receipt_pin);
    const auto navi_source = source_navi_artifact(root, profile, android, receipt_pin,
                                                  artifact_receipt, artifact_receipt_pin);
    auto verify_media = [&] {
        need(validate_profile_android_system_candidate(root, profile, android, receipt_pin) ==
                     image,
             "candidate changed during system upgrade");
        need(source_system_media_artifact(root, profile, android, receipt_pin, artifact_receipt,
                                          artifact_receipt_pin) == media,
             "system media artifact source epoch changed during upgrade");
        need(source_camera_artifact(root, profile, android, receipt_pin, artifact_receipt,
                                    artifact_receipt_pin) == camera_source,
             "Camera artifact source epoch changed during upgrade");
        need(source_navi_artifact(root, profile, android, receipt_pin, artifact_receipt,
                                  artifact_receipt_pin) == navi_source,
             "Navi artifact source epoch changed during upgrade");
    };
    auto result = publish_machine_android_system(
            root, profile, machine, expected, system_directory / "android/system.img",
            image.at("sha256").get<std::string>(), image.at("bytes").get<std::uint64_t>(),
            verify_media, disk_tool, package, 8ULL << 30,
            record_declares(definition, ProfilePlatform::passion, ProfileBoard::sa8155)
                    ? Json{{"profile_bytes",
                            read_private_json_bytes(android / "profile.json", 1 << 20)},
                           {"candidate_bytes",
                            read_private_json_bytes(android / "receipt.json", 1 << 20)}}
                    : Json(),
            camera, prepared);
    // A source rebuild publishes its observer even when only the Android system changed.
    // Private launch configuration is regenerated from this portable snapshot at start.
    result["ui_observer"] = refresh_machine_ui_observer(
            root, profile, machine, target,
            [&] {
                return prepare_profile_ui_observer(root);
            },
            false);
    const bool cluster = accepted.contains("qnx");
    result["schema"] = 1;
    result["android_ready"] = true;
    result["machine_ready"] = true;
    result["profile_ready"] = cluster;
    result["cluster_available"] = cluster;
    result["cluster_ready"] = cluster;
    result["runtime"] = (target / "runtime.json").string();
    result["runtime_sha256"] = hash(target / "runtime.json", 128 * 1024);

    if (candidate.at("kind") == "android-candidate") {
        try {
            purge_android_candidate_working_disks(root, android, tool);
            result["working_disks_collected"] = true;
        } catch (const std::exception &error) {
            result["working_disks_collected"] = false;
            result["gc_error"] = std::string(error.what());
            try {
                write_private_json(android / "gc-failure.json",
                                   {{"schema", 1},
                                    {"phase", "post-upgrade-publication"},
                                    {"message", std::string(error.what())}});
            } catch (const std::exception &journal_error) {
                result["gc_journal_error"] = std::string(journal_error.what());
            }
        }
    }

    result["candidate_receipt_sha256"] = receipt_pin;

    return result;
}

static Json export_built_ui_observer(const std::filesystem::path &root, std::string_view profile,
                                     const std::filesystem::path &stage) {
    const auto observer = cached_profile_ui_observer(root);

    if (observer.empty()) {
        need(!profile_feature(read_private_json(authoring_definition(root, profile), 1 << 20),
                              "ui_observer_required"),
             "source-built UI observer required before portable publication");

        return Json::object();
    }

    const std::filesystem::path source = observer.at("dex").get<std::string>();
    const auto target = stage / "observer/classes.dex";
    ensure_private_directory(target.parent_path());
    copy_portable_image(source, observer.at("sha256").get<std::string>(), target, 0);

    return {{"file", "observer/classes.dex"},
            {"bytes", std::filesystem::file_size(target)},
            {"sha256", observer.at("sha256")}};
}

static void export_can_model_receipt(const Json &definition, const Json &candidate,
                                     const std::filesystem::path &android,
                                     const std::filesystem::path &stage, Json &runtime,
                                     const std::string &profile, const std::string &profile_pin) {
    if (!definition.value("components", Json::object()).contains("can-model-observer")) {
        return;
    }

    const auto receipt = android / "receipt.json";
    const auto receipt_pin = hash(receipt, 1 << 20);
    (void)can_model_candidate_hash(
            candidate, profile, profile_pin,
            runtime.at("android").at("images").at("system.img").at("sha256").get<std::string>());
    const auto target = stage / "can-model/candidate.json";
    ensure_private_directory(target.parent_path());
    copy_portable_image(receipt, receipt_pin, target, 0);
    need(hash(receipt, 1 << 20) == receipt_pin, "CAN candidate changed");
    runtime["android"]["can_model_receipt"] = {{"file", "can-model/candidate.json"},
                                               {"sha256", receipt_pin},
                                               {"bytes", std::filesystem::file_size(target)}};
}

static void export_built_owned_package(const std::filesystem::path &root, std::string_view profile,
                                       const Json &candidate, const std::filesystem::path &android,
                                       const std::filesystem::path &stage, Json &runtime,
                                       std::uint64_t reserve,
                                       const std::filesystem::path &destination) {
    const auto definition = read_private_json(authoring_definition(root, profile), 1 << 20);
    const auto family = runtime_profile_reference(definition, profile);

    // Both callers publish complete fresh disks. Never migrate a differently signed Navi over
    // existing userdata.
    if (candidate.contains("original_navi_system")) {
        need(original_navi_requested(definition) && family == profile &&
                     !std::filesystem::exists(destination) &&
                     !std::filesystem::is_symlink(destination),
             "original Navi requires an absent clean destination");
        (void)validate_profile_android_system_candidate(root, profile, android,
                                                        hash(android / "receipt.json", 1 << 20));
    }

    if (embedded_profile_declares(family, ProfilePlatform::qinggan09, ProfileBoard::sa8155)) {
        need(candidate.contains("qinggan09_owned_packages"),
             "A9 source candidate requires actual owned APKs");
        const auto &descriptor = candidate.at("qinggan09_owned_packages");
        qinggan09_owned_packages_descriptor(descriptor, true);
        need(descriptor.at("receipt_sha256") == candidate.at("artifact_result_sha256"),
             "A9 source candidate owned APK origin differs");

        for (const auto &package : qinggan09_owned_packages_payloads(descriptor)) {
            const auto source = android / package.at("file").get<std::string>();
            const auto target = stage / package.at("file").get<std::string>();
            const auto sha = package.at("sha256").get<std::string>();
            need(hash(source, 16ULL << 20) == sha &&
                         std::filesystem::file_size(source) == package.at("bytes"),
                 "A9 candidate owned APK changed");
            ensure_private_directory(target.parent_path());
            copy_portable_image(source, sha, target, reserve);
        }

        runtime["android"]["qinggan09_owned_packages"] = descriptor;

        return;
    }

    if (qinggan11_navi_delivery_required(definition)) {
        need(candidate.contains("prepared_packages"),
             "current Free A11 Navi recipe requires cold prepared packages");
    }

    if (qinggan11_camera_delivery_required(definition)) {
        need(candidate.contains("camera_package") || candidate.contains("prepared_packages"),
             "current Free A11 Camera recipe requires cold Camera package");
    }

    if (candidate.contains("prepared_packages")) {
        need(qinggan11_package_delivery(definition) && !candidate.contains("camera_package"),
             "exact Free A11 prepared export required");
        const auto &packages = candidate.at("prepared_packages");
        android_prepared_packages_descriptor(packages, true);
        const auto receipt_pin = hash(android / "receipt.json", 1 << 20);
        need(!source_camera_artifact(root, profile, android, receipt_pin, {}, {}).is_null() &&
                     !source_navi_artifact(root, profile, android, receipt_pin, {}, {}).is_null(),
             "prepared APK origins required");
        unsigned index = 0;

        for (const auto &item : packages) {
            const std::string artifact = index++ == 0 ? "patched:Camera" : "patched:Navi";
            unsigned matches = 0;

            for (const auto &row : candidate.at("installed")) {
                if (row.at("artifact") == artifact) {
                    need(row.at("package") == item.at("package") &&
                                 row.at("sha256") == item.at("sha256") &&
                                 row.at("target").get<std::string>().starts_with("/data/app/") &&
                                 !row.at("mutable").get<bool>(),
                         "cold prepared APK differs");
                    ++matches;
                }
            }

            need(matches == 1, "one cold prepared APK required");
            const auto source = android / item.at("file").get<std::string>(),
                       target = stage / item.at("file").get<std::string>();
            const auto sha = item.at("sha256").get<std::string>();
            need(hash(source, 512ULL << 20) == sha && std::filesystem::file_size(source) ==
                                                              item.at("bytes").get<std::uint64_t>(),
                 "candidate prepared APK export differs");
            ensure_private_directory(target.parent_path());
            copy_portable_image(source, sha, target, reserve);
        }

        runtime["android"]["prepared_packages"] = packages;
    }

    if (candidate.contains("camera_package")) {
        need(qinggan11_package_delivery(definition), "Camera export requires Free A11");
        const auto &camera = candidate.at("camera_package");
        android_camera_package_descriptor(camera, true);
        const auto source = android / camera.at("file").get<std::string>(),
                   target = stage / camera.at("file").get<std::string>();
        const auto sha = camera.at("sha256").get<std::string>();
        need(hash(source, 128ULL << 20) == sha &&
                     std::filesystem::file_size(source) == camera.at("bytes").get<std::uint64_t>(),
             "candidate Camera export differs");
        ensure_private_directory(target.parent_path());
        copy_portable_image(source, sha, target, reserve);
        runtime["android"]["camera_package"] = camera;
    }

    if (!candidate.contains("owned_package")) {
        return;
    }

    need(qinggan11_wheel_provider(definition), "owned provider export requires Free A11");
    const auto &package = candidate.at("owned_package");
    android_owned_package_descriptor(package, true);
    const auto source = android / package.at("file").get<std::string>(),
               target = stage / package.at("file").get<std::string>();
    const auto sha = package.at("sha256").get<std::string>();
    need(hash(source, 16ULL << 20) == sha &&
                 std::filesystem::file_size(source) == package.at("bytes").get<std::uint64_t>(),
         "owned candidate APK export differs");
    ensure_private_directory(target.parent_path());
    copy_portable_image(source, sha, target, reserve);
    runtime["android"]["owned_package"] = package;
}

static Json upgrade_profile_qnx_impl(const std::filesystem::path &root, std::string_view profile,
                                     const MachineIdentity &machine,
                                     const std::filesystem::path &qnx_file,
                                     const std::string &qnx_pin,
                                     const std::filesystem::path &payload_source,
                                     const std::filesystem::path &explicit_seed = {}) {
    const bool declared_persistence =
            read_private_json(root / "profiles" / std::string(profile) / "profile-qnx.json",
                              128 * 1024)
                    .contains("persistence");
    const auto verify_qnx = [&] {
        return validate_accepted_qnx_closure(root, profile, qnx_file, qnx_pin, payload_source,
                                             declared_persistence);
    };

    verify_qnx();
    const auto accepted = read_private_json(qnx_file, 1 << 20);
    const auto layout = qnx_runtime_layout(profile, accepted);
    const bool persistent = !layout.persistence.is_null();
    Json replacement = {{"guest_arch", "aarch64"}, {"images", accepted.at("images")}};
    qnx_runtime_metadata(replacement, layout);
    const auto target = explicit_seed.empty() ? build_target(root, profile, machine)
                                              : explicit_seed;
    const auto expected = hash(target / "runtime.json", 128 * 1024);
    const auto previous = inspect_portable_runtime(root, profile, machine, explicit_seed);
    need(previous.contains("qnx") &&
                 qnx_portable_layout(profile, previous.at("qnx")).persistence == layout.persistence,
         "existing nonpersistent QNX machine required");
    need(hash(target / "runtime.json", 128 * 1024) == expected,
         "machine changed during QNX upgrade preflight");
    const std::filesystem::path source =
            payload_source.empty()
                    ? std::filesystem::path(accepted.at("directory").get<std::string>())
                    : payload_source;
    auto result = !explicit_seed.empty()
                          ? publish_machine_qnx_boot_at(root, profile, explicit_seed, expected,
                                                        source, replacement,
                                                        [&] {
                                                            verify_qnx();
                                                        })
                  : persistent ? publish_machine_qnx_boot(root, profile, machine, expected, source,
                                                          replacement,
                                                          [&] {
                                                              verify_qnx();
                                                          })
                               : publish_machine_qnx(root, profile, machine, expected, source,
                                                     replacement, [&] {
                                                         verify_qnx();
                                                     });
    inspect_portable_runtime(root, profile, machine, explicit_seed);
    result["qnx_acceptance_sha256"] = qnx_pin;
    result["runtime_sha256"] = hash(target / "runtime.json", 128 * 1024);

    return result;
}

Json upgrade_profile_qnx(const std::filesystem::path &root, std::string_view profile,
                         const MachineIdentity &machine, const std::filesystem::path &qnx_file,
                         const std::string &qnx_pin, const std::filesystem::path &payload_source) {
    return upgrade_profile_qnx_impl(root, profile, machine, qnx_file, qnx_pin, payload_source);
}

Json upgrade_profile_qnx_at(const std::filesystem::path &root, std::string_view profile,
                            const std::filesystem::path &seed,
                            const std::filesystem::path &qnx_file, const std::string &qnx_pin,
                            const std::filesystem::path &payload_source) {
    need(read_private_json(root / "profiles" / std::string(profile) / "profile-qnx.json",
                           128 * 1024)
                 .contains("persistence"),
         "explicit QNX boot upgrade requires persistent QNX storage");
    ordinary(seed);
    need(std::filesystem::is_directory(seed) && std::filesystem::canonical(seed) == seed,
         "canonical machine directory required");
    const auto machine = machine_identity(
            profile, seed == root / "machines" / std::string(profile) ? std::string{}
                     : seed.parent_path() == root / "machines"
                             ? seed.filename().string()
                             : "external-" + sha256_bytes(seed.generic_string()).substr(0, 24));

    return upgrade_profile_qnx_impl(root, profile, machine, qnx_file, qnx_pin, payload_source,
                                    seed);
}

static Json upgrade_profile_combined(const std::filesystem::path &root, std::string_view profile,
                                     const MachineIdentity &machine,
                                     const std::filesystem::path &android,
                                     const std::filesystem::path &qnx_file,
                                     const std::string &qnx_pin,
                                     const std::filesystem::path &payload_source) {
    const auto android_pin = hash(android / "receipt.json", 1 << 20);

    validate_profile_android_system_candidate(root, profile, android, android_pin);
    validate_accepted_qnx_reuse(root, profile, qnx_file, qnx_pin, payload_source);
    auto result = upgrade_profile_android_system(root, profile, machine, android, android_pin);
    const auto published = upgrade_profile_qnx(root, profile, machine, qnx_file, qnx_pin,
                                               payload_source);
    result["qnx_acceptance_sha256"] = qnx_pin;
    result["qnx_updated"] = published.at("cluster_ready");
    result["runtime_sha256"] = published.at("runtime_sha256");

    return result;
}

// Checks that candidate is an accepted stopped Android candidate of the authoring definition, with
// its registered clean base, ABI and development kernel.
static void validate_android_candidate(const std::filesystem::path &android, const Json &candidate,
                                       std::string_view profile, const Json &definition) {
    need(candidate.at("schema") == 1 && candidate.at("kind") == "android-candidate" &&
                 candidate.at("profile") == profile && candidate.at("stopped") == true &&
                 candidate.at("cold_boot_verified") == true && candidate.at("prepared_VM") == true,
         "accepted stopped Android candidate required");
    validate_accepted_android_profile(android, candidate, profile, definition);
    need(candidate.at("clean_base_archive_sha256") == definition.at("android").at("sha256") &&
                 candidate.at("android").at("abi") == "arm64-v8a",
         "candidate registered clean base/ABI differs");

    if (const auto development = portable_development_kernel(definition); !development.is_null()) {
        need(candidate.contains("development_kernel") &&
                     candidate.at("development_kernel") == development,
             "candidate development kernel differs from cold boot");
    } else {
        need(!candidate.contains("development_kernel"), "unexpected candidate development kernel");
    }
}

namespace {
// The exclusive pending directory of a new machine beside its final directory, removed unless
// published.
struct MachineStage {
    std::filesystem::path final, stage;
    bool owned = true;

    MachineStage(std::filesystem::path target, const std::string &machine)
        : final(std::move(target)) {
        need(!std::filesystem::exists(final) && !std::filesystem::is_symlink(final),
             "final profile already exists; never replaced");
        ensure_private_directory(final.parent_path());
        stage = final.parent_path() / (".pending-" + machine + "-" + random_token());
        need(std::filesystem::create_directory(stage), "exclusive prepared stage required");

        try {
            ensure_private_directory(stage);
        } catch (...) {
            remove();
            throw;
        }
    }

    ~MachineStage() {
        if (owned) {
            remove();
        }
    }

    void remove() const {
        std::error_code e;
        std::filesystem::remove_all(stage, e);
    }

    MachineStage(const MachineStage &) = delete;
    MachineStage &operator=(const MachineStage &) = delete;
};
} // namespace

// Exports the Android-side parts of a new machine from the accepted candidate into stage and
// records them in runtime.
static void export_android_machine_parts(const std::filesystem::path &root,
                                         std::string_view profile, const Json &definition,
                                         const Json &candidate,
                                         const std::filesystem::path &android,
                                         const MachineStage &machine,
                                         const std::string &profile_pin, Json &runtime) {
    const auto &stage = machine.stage;

    export_built_owned_package(root, profile, candidate, android, stage, runtime,
                               android_candidate_reserve_bytes(definition, profile), machine.final);
    export_can_model_receipt(definition, candidate, android, stage, runtime, std::string(profile),
                             profile_pin);

    if (const auto observer = export_built_ui_observer(root, profile, stage); !observer.empty()) {
        runtime["ui_observer"] = observer;
    }

    if (passion_power_requested(definition)) {
        runtime["android"]["display_power"] = export_passion_power(definition, candidate, stage,
                                                                   profile_pin);
    }

    if (passion_mcu_requested(definition)) {
        runtime["android"]["mcu_property"] = export_passion_mcu(definition, candidate, stage,
                                                                profile_pin);
    }

    runtime["android"]["boot_files"] = export_portable_boot_files(
            root, definition, stage, android_candidate_reserve_bytes(definition, profile));

    if (const auto development = portable_development_kernel(definition); !development.is_null()) {
        runtime["android"]["development_kernel"] = development;
    }

    runtime["resources"] = export_portable_resources(root, definition, profile, stage);
    runtime["resources"].update(export_portable_fonts(root, definition, profile, stage));
}

Json finalize_profile_build(const std::filesystem::path &root, std::string_view profile,
                            const MachineIdentity &machine, const std::filesystem::path &android,
                            const std::filesystem::path &qnx_file, const std::string &qnx_pin,
                            bool reuse_qnx, const std::filesystem::path &reuse_qnx_source) {
    const auto artifact_lease = candidate_artifact_reader(root, android);

    if (std::filesystem::exists(build_target(root, profile, machine)) &&
        qinggan11_package_delivery(
                read_private_json(authoring_definition(root, profile), 1 << 20))) {
        return upgrade_profile_combined(root, profile, machine, android, qnx_file, qnx_pin,
                                        reuse_qnx_source);
    }

    root_profile(root, profile);
    ordinary(android);
    inside(root / ".local", android);
    inside(root / ".local", qnx_file);
    need(hash(qnx_file, 1 << 20) == qnx_pin, "QNX acceptance pin mismatch");
    const auto qnx = read_private_json(qnx_file, 1 << 20),
               candidate = read_private_json(android / "receipt.json", 1 << 20);
    const auto authoring_pin = hash(authoring_definition(root, profile), 1 << 20);
    need(qnx.at("schema") == 1 && qnx.at("kind") == "accepted-qnx-portable-seed" &&
                 qnx.at("profile") == profile && qnx.at("source_built") == true &&
                 qnx.at("cold_boot_verified") == true,
         "accepted source-built QNX seed required");

    for (const auto *key : {"source_receipt", "cold_boot_evidence"}) {
        const auto &proof = qnx.at(key);
        const std::filesystem::path file = proof.at("file").get<std::string>();
        inside(root / ".local", file);
        need(hash(file, 16 << 20) == proof.at("sha256").get<std::string>(),
             "QNX acceptance evidence differs");
    }

    const auto definition = read_private_json(authoring_definition(root, profile), 1 << 20);
    validate_android_candidate(android, candidate, profile, definition);
    const auto profile_pin = candidate.at("profile_sha256").get<std::string>();
    const auto reused_qnx = reuse_qnx ? validate_accepted_qnx_reuse(root, profile, qnx_file,
                                                                    qnx_pin, reuse_qnx_source)
                                      : Json();
    const auto api = native_base_api(definition.at("android"));
    MachineStage built(build_target(root, profile, machine), machine.name);
    const auto &final = built.final;
    const auto &stage = built.stage;
    // The machine assembly shows the bytes of the guest images it copies.
    progress::block();
    progress::Meter assembling("build_machine", {{"machine", progress::path_text(final)}});
    assembling.length(image_bytes(candidate.at("android").at("images")) +
                      image_bytes(qnx.at("images")));
    copy_portable_image(android / "profile.json", profile_pin, stage / "profile.json", 0);
    const auto android_images = copy_images(
            root, android, candidate.at("android").at("images"), stage, "android",
            {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img", "cache.img"},
            android_candidate_reserve_bytes(definition, profile), &assembling);
    const std::filesystem::path qnx_source =
            reuse_qnx && !reuse_qnx_source.empty()
                    ? reuse_qnx_source
                    : std::filesystem::path(qnx.at("directory").get<std::string>());
    ordinary(qnx_source);

    if (qnx_source != root / "machines" / std::string(profile)) {
        inside(root / ".local", qnx_source);
    }

    const auto family = runtime_profile_reference(definition, profile);
    need(valid_profile_name(family), "valid QNX guest family required");
    const auto layout = qnx_runtime_layout(family, qnx);
    // The accepted closure runs the hardware adapter that the profile declares.
    need(read_private_json(root / "profiles" / std::string(profile) / "profile-qnx.json",
                           128 * 1024)
                         .at("hardware_adapter") == layout.adapter,
         "accepted QNX adapter differs from the profile");

    if (!layout.projection.is_null()) {
        need(qnx.value("stopped", false), "stopped Free QNX seed required");
    }

    const auto qnx_images =
            copy_images(root, qnx_source, qnx.at("images"), stage, "qnx", qnx_image_roles(layout),
                        android_candidate_reserve_bytes(definition, profile), &assembling);
    Json runtime = {{"schema", 1},
                    {"kind", "portable-guest-runtime"},
                    {"profile", profile},
                    {"profile_sha256", profile_pin},
                    {"android",
                     {{"api", api},
                      {"abi", "arm64-v8a"},
                      {"base_archive_sha256", candidate.at("clean_base_archive_sha256")},
                      {"images", android_images}}},
                    {"qnx", {{"guest_arch", "aarch64"}, {"images", qnx_images}}}};
    qnx_runtime_metadata(runtime["qnx"], layout);
    export_android_machine_parts(root, profile, definition, candidate, android, built, profile_pin,
                                 runtime);

    if (!reused_qnx.is_null()) {
        write_private_json(stage / "build-provenance.json", {{"schema", 1}, {"qnx", reused_qnx}});
    }

    write_private_json(stage / "runtime.json", runtime);
    need(hash(authoring_definition(root, profile), 1 << 20) == authoring_pin &&
                 hash(qnx_file, 1 << 20) == qnx_pin,
         "finalization inputs changed");
    inspect_portable_runtime(root, profile, machine, stage);
    publish(stage, final);
    built.owned = false;
    assembling.finish("");
    const auto verified = inspect_portable_runtime(root, profile, machine);
    Json provenance = Json::object();

    if (!reused_qnx.is_null()) {
        provenance = {{"file", (final / "build-provenance.json").string()},
                      {"sha256", hash(final / "build-provenance.json", 16384)}};
    }

    return {{"build_provenance", provenance},
            {"schema", 1},
            {"profile", profile},
            {"profile_ready", true},
            {"runtime", (final / "runtime.json").string()},
            {"runtime_sha256", hash(final / "runtime.json", 128 * 1024)},
            {"images_verified", true},
            {"guest_started", false},
            {"qnx_acceptance_sha256", qnx_pin}};
}

Json finalize_android_profile_build(const std::filesystem::path &root, std::string_view profile,
                                    const MachineIdentity &machine,
                                    const std::filesystem::path &android,
                                    const std::string &receipt_pin) {
    const auto artifact_lease = candidate_artifact_reader(root, android);

    if (std::filesystem::exists(build_target(root, profile, machine)) &&
        profile_feature(read_private_json(authoring_definition(root, profile), 1 << 20),
                        "system_upgrade")) {
        return upgrade_profile_android_system(root, profile, machine, android, receipt_pin);
    }

    root_profile(root, profile);
    ordinary(android);
    inside(root / ".local", android);
    const auto receipt = android / "receipt.json";
    need(hash(receipt, 1 << 20) == receipt_pin, "Android candidate receipt pin mismatch");
    const auto candidate = read_private_json(receipt, 1 << 20);
    const auto profile_file = authoring_definition(root, profile);
    const auto authoring_pin = hash(profile_file, 1 << 20);
    const auto definition = read_private_json(profile_file, 1 << 20);
    validate_android_candidate(android, candidate, profile, definition);
    const auto profile_pin = candidate.at("profile_sha256").get<std::string>();
    const auto api = native_base_api(definition.at("android"));
    MachineStage built(build_target(root, profile, machine), machine.name);
    const auto &final = built.final;
    const auto &stage = built.stage;
    progress::block();
    progress::Meter assembling("build_machine", {{"machine", progress::path_text(final)}});
    assembling.length(image_bytes(candidate.at("android").at("images")));
    copy_portable_image(android / "profile.json", profile_pin, stage / "profile.json", 0);
    const auto images = copy_images(
            root, android, candidate.at("android").at("images"), stage, "android",
            {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img", "cache.img"},
            android_candidate_reserve_bytes(definition, profile), &assembling);
    {
        const auto disk_tool = root / ".local/toolchains/native-sdk" / "emulator" /
                               ("qemu-img" + exe());
        purge_android_candidate_working_disks(root, android, disk_tool);
    }
    Json runtime = {{"schema", 1},
                    {"kind", "portable-guest-runtime"},
                    {"profile", profile},
                    {"profile_sha256", profile_pin},
                    {"scope", "android"},
                    {"cluster_available", false},
                    {"candidate_receipt_sha256", receipt_pin},
                    {"android",
                     {{"api", api},
                      {"abi", "arm64-v8a"},
                      {"base_archive_sha256", candidate.at("clean_base_archive_sha256")},
                      {"images", images}}}};
    export_android_machine_parts(root, profile, definition, candidate, android, built, profile_pin,
                                 runtime);
    write_private_json(stage / "runtime.json", runtime);
    need(hash(profile_file, 1 << 20) == authoring_pin && hash(receipt, 1 << 20) == receipt_pin,
         "finalization inputs changed");
    publish(stage, final);
    built.owned = false;
    assembling.finish("");

    return {{"schema", 1},
            {"profile", profile},
            {"scope", "android"},
            {"android_ready", true},
            {"machine_ready", true},
            {"profile_ready", false},
            {"cluster_available", false},
            {"runtime", (final / "runtime.json").string()},
            {"runtime_sha256", hash(final / "runtime.json", 128 * 1024)},
            {"images_verified", true},
            {"guest_started", false},
            {"candidate_receipt_sha256", receipt_pin}};
}

Json profile_frontend_bindings(const std::filesystem::path &root, std::string_view profile,
                               const Json &frontend, std::map<std::string, std::string> &pins) {
    ordinary(root);
    need(valid_profile_name(profile) && frontend.is_object() && frontend.value("schema", 0) == 1 &&
                 frontend.at("pins").is_array() && frontend.at("pins").size() > 0 &&
                 frontend.at("pins").size() <= 512,
         "bounded native frontend schema required");

    if (frontend.contains("source_root")) {
        need(frontend.at("source_root") == root.string(),
             "frontend source root differs from selected project");
    }

    std::map<std::string, std::string> verified;

    for (const auto &item : frontend.at("pins")) {
        need(item.is_object() && item.size() == 2, "exact frontend path/SHA pin required");
        const std::filesystem::path file = item.at("path").get<std::string>();
        inside(root, file);
        need(std::filesystem::canonical(file) == file, "canonical frontend input required");
        const auto actual = hash(file, 512ULL << 20);
        need(item.at("sha256") == actual && verified.emplace(file.string(), actual).second,
             "frontend pin differs or duplicates another input");
    }

    const std::filesystem::path executable = frontend.at("executable").get<std::string>(),
                                splash = frontend.at("splash").get<std::string>(),
                                assets = frontend.at("assets").get<std::string>(),
                                state = frontend.at("state").get<std::string>(),
                                discovery = frontend.at("discovery_root").get<std::string>();
    need(executable.filename() == "voyemu" + exe() && verified.contains(executable.string()) &&
                 verified.contains(splash.string()),
         "exact native frontend executable/splash pins required");
    ordinary(assets);
    inside(root, assets);
    need(std::filesystem::is_directory(assets) && std::filesystem::canonical(assets) == assets,
         "native frontend asset root required");

    for (const auto *file : {"wheel-left.png", "wheel-right.png", "wheel-hotspots.json"}) {
        need(verified.contains((assets / file).string()), "wheel asset pin missing");
    }

    ordinary(state);
    ordinary(discovery);
    inside(root / ".local", state);
    inside(root / ".local", discovery);
    need(state.parent_path().filename() == "state" &&
                 state.parent_path().parent_path().parent_path() == root / ".local/machines",
         "profile-scoped frontend placement state required");
    const auto grpc = frontend.at("grpc_port").get<unsigned>();
    need(grpc >= 1024 && grpc <= 65535, "native frontend gRPC port required");

    if (frontend.contains("capture")) {
        need(frontend.at("capture").is_boolean(), "frontend capture flag must be boolean");
    }

    if (frontend.contains("input_smoke")) {
        need(frontend.at("input_smoke").is_boolean(), "frontend input smoke flag must be boolean");
    }

    if (frontend.contains("startup")) {
        const auto &startup = frontend.at("startup");
        need(startup.is_array() && startup.size() == 2, "two frontend startup screens required");

        for (const auto &animation : startup) {
            need(animation.is_null() ||
                         (animation.is_string() && verified.contains(animation.get<std::string>())),
                 "pinned frontend startup animation required");
        }
    }

    if (frontend.contains("startup_video")) {
        const auto &videos = frontend.at("startup_video");
        need(videos.is_array() && videos.size() == 2, "two frontend startup videos required");

        for (const auto &video : videos) {
            need(video.is_null() ||
                         (video.is_string() && verified.contains(video.get<std::string>())),
                 "pinned frontend startup video required");
        }
    }

    if (frontend.contains("startup_cluster")) {
        const auto &video = frontend.at("startup_cluster");
        need(video.is_string() && verified.contains(video.get<std::string>()),
             "pinned frontend startup video required");
    }

    // The screens window: by role, a pinned boot animation and startup video.
    for (const auto *field : {"startup_screens", "startup_video_screens"}) {
        if (frontend.contains(field)) {
            need(frontend.at(field).is_object() && frontend.at(field).size() <= 4,
                 "frontend screens by role required");

            for (const auto &[screen, path] : frontend.at(field).items()) {
                need(path.is_string() && verified.contains(path.get<std::string>()),
                     "pinned frontend startup screen required");
            }
        }
    }

    for (const auto &[file, pin] : verified) {
        const auto existing = pins.find(file);
        need(existing == pins.end() || existing->second == pin,
             "frontend pin conflicts with host tool binding");
    }

    pins.insert(verified.begin(), verified.end());

    return frontend;
}

namespace {
// The observer sources and host tests of a source checkout by name and SHA-256. An installation
// has no sources: it takes the observer sources that the index of its android-java asset names,
// since the androidJava pin identifies that observer, and no host tests.
std::pair<Json, Json> observer_inventory(const std::filesystem::path &root) {
    Json sources = Json::object(), tests = Json::object();

    if (!project_source_checkout(root)) {
        const auto &pin = embedded_toolchain_manifest().at("androidJava");
        const auto asset = install_android_java(root, pin, true, 0);

        if (asset.empty()) {
            throw VoyemuError("ANDROID_JAVA_ASSET_MISSING",
                              "the android-java asset with the UI observer is not installed; run "
                              "deps");
        }

        const auto index = Json::parse(read_pinned_zip_entry(
                asset, pin.at("sha256").get<std::string>(), "index.json", 16 << 20));
        need(index.contains("observer") && index.at("observer").contains("sources"),
             "android-java asset has no UI observer");
        sources = index.at("observer").at("sources");
        need(sources.is_object() && !sources.empty() && sources.size() <= 64,
             "bounded observer sources required");

        return {sources, tests};
    }

    std::size_t count = 0;

    for (const auto &entry : std::filesystem::directory_iterator(root / "src/guest/android/api")) {
        if (entry.path().extension() != ".java") {
            continue;
        }

        need(++count <= 96, "bounded observer source inventory required");
        auto &group = entry.path().filename().string().ends_with("Test.java") ? tests : sources;
        group[entry.path().filename().string()] = hash(entry.path(), 1 << 20);
    }

    need(!sources.empty() && sources.size() <= 64 && !tests.empty() && tests.size() <= 32,
         "bounded observer sources and host tests required");

    return {sources, tests};
}
} // namespace

Json cached_profile_ui_observer(const std::filesystem::path &root) {
    ordinary(root);
    const auto cache = root / ".local/toolchains/ui-observer";

    if (!std::filesystem::exists(cache)) {
        return Json::object();
    }

    ordinary(cache);
    const auto [sources, tests] = observer_inventory(root);
    Json selected = Json::object();
    std::size_t count = 0;
    auto digest = [](const Json &value) {
        return value.is_string() && sha256_text(value.get_ref<const std::string &>());
    };

    for (const auto &entry : std::filesystem::directory_iterator(cache)) {
        need(++count <= 64, "bounded observer artifact cache required");
        const auto name = entry.path().filename().string();

        if (name.starts_with(".pending-")) {
            continue;
        }

        ordinary(entry.path());
        need(std::filesystem::is_directory(entry.path()) && digest(Json(name)),
             "invalid observer cache directory");
        const auto receipt = entry.path() / "observer.json", dex = entry.path() / "classes.dex";
        hash(receipt, 128 * 1024);
        const auto record = read_private_json(receipt, 128 * 1024);
        need(record.is_object() && record.size() == 4 && record.value("schema", 0) == 1 &&
                     record.value("kind", std::string{}) == "owned-api28-ui-observer" &&
                     digest(record.at("sha256")),
             "observer runtime receipt schema differs");
        const auto &inputs = record.at("inputs");
        // A compiled observer names its compilers; one from android-java names that asset.
        const bool asset = inputs.is_object() &&
                           inputs.value("host_test_policy", Json()) == "android-java-v1";
        need(inputs.is_object() && inputs.size() == (asset ? 8U : 9U) && inputs.at("schema") == 1 &&
                     inputs.at("min_api") == 28 &&
                     (asset || inputs.at("host_test_policy") == "assertions-enabled-v1") &&
                     sha256_bytes(inputs.dump()) == name,
             "observer compiler receipt fingerprint differs");

        for (const auto *field :
             asset ? std::vector<const char *>{"java", "android_java", "platform"}
                   : std::vector<const char *>{"java", "javac", "platform", "dexer"}) {
            need(digest(inputs.at(field)), "observer compiler provenance pin missing");
        }

        if (inputs.at("sources") != sources || inputs.at("test_sources") != tests) {
            continue;
        }

        const auto pin = hash(dex, 8 << 20);
        need(pin == record.at("sha256").get<std::string>(), "observer runtime artifact differs");
        need(selected.empty() || selected.at("sha256") == pin,
             "ambiguous source-matched observer artifacts");
        selected = {{"dex", dex.string()}, {"sha256", pin}};
    }

    return selected;
}

Json prepare_profile_ui_observer(const std::filesystem::path &root,
                                 const std::filesystem::path &jdk) {
    ordinary(root);
    const auto cached = cached_profile_ui_observer(root);

    if (!cached.empty()) {
        return cached;
    }

    const auto tools = ensure_java_preparation_tools(root, jdk, true);
    const auto java = tools.runtime.jdk / ("bin/java" + exe()),
               javac = tools.runtime.jdk / ("bin/javac" + exe()),
               dexer = tools.runtime.helper.parent_path() / "lib/d8.jar";
    // Without javac, as on the Java runtime of preparation, or without the observer sources of a
    // source checkout, the observer comes from android-java.
    const bool compiler = std::filesystem::is_regular_file(javac) && project_source_checkout(root);
    std::filesystem::path guest_java;

    if (!compiler) {
        guest_java = install_android_java(root, embedded_toolchain_manifest().at("androidJava"),
                                          true, 0);

        if (guest_java.empty()) {
            throw VoyemuError("ANDROID_JAVA_ASSET_MISSING",
                              "the android-java asset is not installed and the Java runtime has no "
                              "javac for the UI observer; run deps or build with --jdk");
        }
    }

    Json inputs = compiler ? Json{{"schema", 1},
                                  {"min_api", 28},
                                  {"java", hash(java, 512ULL << 20)},
                                  {"javac", hash(javac, 512ULL << 20)},
                                  {"platform", tools.platform_sha256},
                                  {"dexer", java_toolchain_input_sha256(dexer, 128ULL << 20)},
                                  {"sources", Json::object()}}
                           : Json{{"schema", 1},
                                  {"min_api", 28},
                                  {"java", hash(java, 512ULL << 20)},
                                  {"android_java", hash(guest_java, 256ULL << 20)},
                                  {"platform", tools.platform_sha256},
                                  {"sources", Json::object()}};
    const auto [observer_sources, observer_tests] = observer_inventory(root);
    std::vector<std::filesystem::path> sources, host_tests;
    inputs["sources"] = observer_sources;
    inputs["test_sources"] = observer_tests;
    // The android-java workflow does not run the host tests; CTest guest_android_api_java does.
    inputs["host_test_policy"] = compiler ? "assertions-enabled-v1" : "android-java-v1";

    for (auto at = observer_sources.begin(); at != observer_sources.end(); ++at) {
        sources.push_back(root / "src/guest/android/api" / at.key());
    }

    for (auto at = observer_tests.begin(); at != observer_tests.end(); ++at) {
        host_tests.push_back(root / "src/guest/android/api" / at.key());
    }

    const auto fingerprint = sha256_bytes(inputs.dump());
    const auto folder = root / ".local/toolchains/ui-observer" / fingerprint,
               receipt = folder / "observer.json", dex = folder / "classes.dex";
    auto verify = [&]() {
        hash(receipt, 128 * 1024);
        const auto record = read_private_json(receipt, 128 * 1024);
        need(record.at("inputs") == inputs &&
                     record.at("sha256").get<std::string>() == hash(dex, 8 << 20),
             "observer source/artifact cache mismatch");

        return Json{{"dex", dex.string()}, {"sha256", record.at("sha256")}};
    };

    if (std::filesystem::exists(folder)) {
        ordinary(folder);

        return verify();
    }

    ensure_private_directory(folder.parent_path());
    const auto stage = folder.parent_path() / (".pending-" + random_token());
    ensure_private_directory(stage);
    struct Cleanup {
        std::filesystem::path p;

        ~Cleanup() {
            std::error_code e;
            std::filesystem::remove_all(p, e);
        }
    } cleanup{stage};

    if (!compiler) {
        const auto archive = inputs.at("android_java").get<std::string>();
        const auto index =
                Json::parse(read_pinned_zip_entry(guest_java, archive, "index.json", 16 << 20));

        if (!index.contains("observer") ||
            index.at("observer").value("sources", Json()) != inputs.at("sources")) {
            throw VoyemuError("ANDROID_JAVA_MISSING",
                              "the android-java asset has no UI observer of these sources; "
                              "install the asset built from them or build with --jdk");
        }

        const auto dex = read_pinned_zip_entry(guest_java, archive, "observer/classes.dex",
                                               8 << 20);
        {
            std::ofstream out(stage / "classes.dex", std::ios::binary);
            out.write(dex.data(), static_cast<std::streamsize>(dex.size()));
            out.close();
            need(bool(out), "observer DEX write failed");
        }
        write_private_json(stage / "observer.json",
                           {{"schema", 1},
                            {"kind", "owned-api28-ui-observer"},
                            {"inputs", inputs},
                            {"sha256", hash(stage / "classes.dex", 8 << 20)}});
        publish(stage, folder);

        return verify();
    }

    ensure_private_directory(stage / "classes");
    ensure_private_directory(stage / "dex");
    ensure_private_directory(root / ".local/build/tmp");
    auto run = [&](const std::filesystem::path &tool, std::vector<std::string> args) {
        ProcessSpec command;
        command.executable = tool;
        command.arguments = std::move(args);
        command.directory = root;
        command.environment = {{"JAVA_HOME", tools.runtime.jdk.string()},
                               {"TMPDIR", (root / ".local/build/tmp").string()},
                               {"TMP", (root / ".local/build/tmp").string()},
                               {"TEMP", (root / ".local/build/tmp").string()}};
        const auto result = execute(command, 60000, 128 * 1024);
        need(!result.timed_out && result.exit_code == 0,
             ("observer compiler failed: " + result.output).c_str());
    };
    std::vector<std::string> args = {"-J-Djava.io.tmpdir=" + (root / ".local/build/tmp").string(),
                                     "--release",
                                     "8",
                                     "-classpath",
                                     tools.platform.string(),
                                     "-d",
                                     (stage / "classes").string()};

    for (const auto &file : sources) {
        args.push_back(file.string());
    }

    run(javac, args);
    ensure_private_directory(stage / "test-classes");
#ifdef _WIN32
    const std::string class_separator = ";";
#else
    const std::string class_separator = ":";
#endif
    args = {"-J-Djava.io.tmpdir=" + (root / ".local/build/tmp").string(),
            "--release",
            "8",
            "-classpath",
            (stage / "classes").string() + class_separator + tools.platform.string(),
            "-d",
            (stage / "test-classes").string()};

    for (const auto &file : host_tests) {
        args.push_back(file.string());
    }

    run(javac, args);

    for (const auto &file : host_tests) {
        run(java,
            {"-ea", "-Djava.io.tmpdir=" + (root / ".local/build/tmp").string(), "-cp",
             (stage / "test-classes").string() + class_separator + (stage / "classes").string(),
             "com.voyemu.api." + file.stem().string()});
    }

    args = {"-Djava.io.tmpdir=" + (root / ".local/build/tmp").string(),
            "-cp",
            dexer.string(),
            "com.android.tools.r8.D8",
            "--min-api",
            "28",
            "--lib",
            tools.platform.string(),
            "--output",
            (stage / "dex").string()};
    std::vector<std::string> classes;

    for (const auto &entry : std::filesystem::recursive_directory_iterator(stage / "classes")) {
        ordinary(entry.path());

        if (entry.is_regular_file() && entry.path().extension() == ".class") {
            classes.push_back(entry.path().string());
        }
    }

    std::sort(classes.begin(), classes.end());
    need(!classes.empty() && classes.size() <= 512, "bounded observer classes required");
    args.insert(args.end(), classes.begin(), classes.end());
    run(java, args);
    need(hash(java, 512ULL << 20) == inputs.at("java").get<std::string>() &&
                 hash(javac, 512ULL << 20) == inputs.at("javac").get<std::string>() &&
                 java_toolchain_input_sha256(tools.platform, 128ULL << 20) ==
                         inputs.at("platform").get<std::string>() &&
                 java_toolchain_input_sha256(dexer, 128ULL << 20) ==
                         inputs.at("dexer").get<std::string>(),
         "observer compiler inputs changed");

    for (const auto &file : sources) {
        need(hash(file, 1 << 20) ==
                     inputs.at("sources").at(file.filename().string()).get<std::string>(),
             "observer source changed during compilation");
    }

    for (const auto &file : host_tests) {
        need(hash(file, 1 << 20) ==
                     inputs.at("test_sources").at(file.filename().string()).get<std::string>(),
             "observer host test changed during preparation");
    }

    std::size_t source_count = 0;

    for (const auto &entry : std::filesystem::directory_iterator(root / "src/guest/android/api")) {
        if (entry.path().extension() == ".java" &&
            !entry.path().filename().string().ends_with("Test.java")) {
            ++source_count;
            need(inputs.at("sources").contains(entry.path().filename().string()),
                 "observer source inventory changed");
        }
    }

    need(source_count == sources.size(), "observer source inventory changed");
    std::filesystem::rename(stage / "dex/classes.dex", stage / "classes.dex");
    const auto pin = hash(stage / "classes.dex", 8 << 20);
    std::filesystem::remove_all(stage / "classes");
    std::filesystem::remove_all(stage / "dex");
    std::filesystem::remove_all(stage / "test-classes");
    write_private_json(stage / "observer.json", {{"schema", 1},
                                                 {"kind", "owned-api28-ui-observer"},
                                                 {"inputs", inputs},
                                                 {"sha256", pin}});
    publish(stage, folder);

    return verify();
}

Json profile_built_frontend_bindings(const std::filesystem::path &root, std::string_view profile,
                                     const MachineIdentity &machine,
                                     std::map<std::string, std::string> &pins,
                                     const std::filesystem::path &seed) {
    auto frontend = bundled_frontend_record(root);
    const auto definition = portable_profile_definition(root, profile, machine, seed);
    const bool passion = record_declares(definition, ProfilePlatform::passion,
                                         ProfileBoard::sa8155);
    const auto source = seed.empty() ? root / "machines" / machine.name : seed;
    const auto assets = source / "ui/wheel";

    ordinary(assets);
    const auto splash = portable_resource_path(source, "ui/splash.png");
    (void)portable_resource_path(source, "ui/wheel/wheel-hotspots.json");
    frontend["assets"] = assets.string();
    frontend["fonts"] = (source / "ui/fonts").string();
    frontend["splash"] = splash.string();

    if (const auto startup = portable_startup_animations(source); !startup.is_null()) {
        frontend["startup"] = startup;

        for (const auto &animation : startup) {
            if (animation.is_string()) {
                frontend["pins"].push_back(
                        {{"path", animation},
                         {"sha256", hash(animation.get<std::string>(), 64ULL << 20)}});
            }
        }
    }

    if (const auto videos = portable_startup_video_paths(source); !videos.is_null()) {
        frontend.update(portable_startup_video_frontend(source));

        for (const auto &video : videos) {
            if (video.is_string()) {
                frontend["pins"].push_back(
                        {{"path", video},
                         {"sha256", hash(video.get<std::string>(), 256ULL << 20)}});
            }
        }
    }

    // The boot animations of the screens window; their videos are pinned with the others above.
    frontend.update(portable_startup_screens_frontend(source));

    // items() keeps a reference, so the screens outlive the loop in a named value.
    const auto screens = frontend.value("startup_screens", Json::object());

    for (const auto &[screen, animation] : screens.items()) {
        frontend["pins"].push_back(
                {{"path", animation}, {"sha256", hash(animation.get<std::string>(), 64ULL << 20)}});
    }

    frontend["state"] =
            (native_machine_directory(root, profile, machine) / "state/placement.json").string();
    frontend["discovery_root"] =
            (native_machine_directory(root, profile, machine) / "active/avd/running").string();
    frontend["grpc_port"] = passion ? 35590 : 45997;
    frontend["capture"] = false;
    frontend["pins"].push_back({{"path", splash.string()}, {"sha256", hash(splash, 64ULL << 20)}});

    for (const auto &item : std::filesystem::recursive_directory_iterator(assets)) {
        need(!item.is_symlink(), "linked wheel asset rejected");

        if (item.is_regular_file()) {
            frontend["pins"].push_back(
                    {{"path", item.path().string()}, {"sha256", hash(item.path(), 64ULL << 20)}});
        }
    }

    // The car fonts of a machine whose profile declares them.
    if (std::filesystem::is_directory(source / "ui/fonts")) {
        for (const auto &item : std::filesystem::directory_iterator(source / "ui/fonts")) {
            need(!item.is_symlink() && item.is_regular_file(), "ordinary machine font required");
            frontend["pins"].push_back(
                    {{"path", item.path().string()}, {"sha256", hash(item.path(), 64ULL << 20)}});
        }
    }

    return profile_frontend_bindings(root, profile, frontend, pins);
}

Json refresh_built_profile_ui_observer(const std::filesystem::path &root, std::string_view profile,
                                       const MachineIdentity &machine,
                                       const std::filesystem::path &seed) {
    return refresh_machine_ui_observer(root, profile, machine, seed, [&] {
        return prepare_profile_ui_observer(root);
    });
}

static std::map<std::string, std::string>
verified_activation_sdk_pins(const std::filesystem::path &root, const std::filesystem::path &sdk,
                             const std::filesystem::path &scoped) {
    const auto &toolchain = embedded_toolchain_manifest().at("sdk");
    const auto native_host = native_dependency_host();
    const auto compiled =
            native_host == "darwin/arm64"
                    ? toolchain.at("runtimeFilePins").at("darwin").at("arm64").at("files")
                    : Json::object();
    Json retained = Json::object();

    if (std::filesystem::exists(scoped / "activation.json")) {
        const auto record = read_private_json(scoped / "activation.json");

        if (record.value("host", std::string{}) == native_host &&
            record.contains("host_file_pins")) {
            const auto bytes = read_private_json_bytes(scoped / "android-launch.json");
            need(record.at("android_launch_sha256") == sha256_bytes(bytes),
                 "retained Android launch pin differs");

            if (Json::parse(bytes).at("android_sdk_root") == sdk.string()) {
                retained = record.at("host_file_pins");
            }
        }
    }

    const auto recipes = native_runtime_dependencies(native_host);
    const auto archive_dir = root / ".local/toolchains/downloads";
    std::map<std::string, std::string> pins;
    const auto add = [&](const char *name, const std::filesystem::path &file) {
        const auto relative = file.lexically_relative(sdk).generic_string();
        auto expected = compiled.value(relative, std::string{});

        if (expected.empty()) {
            expected = retained.value(name, std::string{});
        }

        if (expected.empty()) {
            const auto &recipe = relative.starts_with("emulator/") ? recipes.at(1) : recipes.at(0);
            const auto archive = archive_dir / "native" / (recipe.pin.sha256 + ".blob");
            need(std::filesystem::is_regular_file(archive),
                 "verified runtime SDK pin or cached archive missing for " + relative);
            ordinary(archive);
            need(std::filesystem::hard_link_count(archive) == 1 &&
                         std::filesystem::file_size(archive) <= recipe.pin.maximum_bytes,
                 "ordinary bounded runtime SDK archive required");
            expected = sha256_bytes(read_pinned_zip_entry(archive, recipe.pin.sha256, relative,
                                                          64ULL << 20, ZipNamePolicy::exact));
        }

        need(hash(file, 512ULL << 20) == expected, "runtime SDK pin differs for " + relative);
        pins.emplace(file.string(), expected);
    };
    add("emulator", sdk / "emulator" / ("emulator" + exe()));
    add("qemu_img", sdk / "emulator" / ("qemu-img" + exe()));
    add("qemu_backend",
        sdk / "emulator" / "qemu" / backend_host() / ("qemu-system-aarch64" + exe()));

    return pins;
}

Json activate_built_profile(const std::filesystem::path &root, std::string_view profile,
                            const MachineIdentity &machine, bool require_cluster, bool refresh_host,
                            const std::filesystem::path &seed, bool root_program_only) {
    const auto manifest = inspect_portable_runtime(root, profile, machine, seed);

    need(!require_cluster || manifest.contains("qnx"),
         "cluster unavailable in Android-only profile");
    const auto binding_file = qnx_host_bindings_path(root);
    auto read_bindings = [&]() {
        Json value = Json::object();

        if (std::filesystem::exists(binding_file) || std::filesystem::is_symlink(binding_file)) {
            hash(binding_file, 128 * 1024);
            value = read_private_json(binding_file, 128 * 1024);
            need(value.at("schema") == 1, "explicit portable host binding schema required");
        }

        return value;
    };
    const auto shared_bindings = read_bindings();
    Json bindings = qnx_host_binding_view(root, shared_bindings);
    PortableRuntimeHostBindings host;
    host.seed = seed;
    const auto scoped = native_machine_directory(root, profile, machine);

    if (bindings.contains("runtime_sdk")) {
#ifdef _WIN32
        host.sdk = bindings.at("runtime_sdk").at("sdk_root").get<std::string>();
        inside(root, host.sdk);
        const auto runtime = verify_native_runtime_dependencies(
                host.sdk, bindings.at("runtime_sdk"), {}, RuntimeSdkUse::launch);
        host.pins = runtime.at("pins").get<std::map<std::string, std::string>>();
#else
        throw VoyemuError("PROFILE_BUILD", "Windows runtime SDK binding requires its native host");
#endif
    } else {
        host.sdk = android_runtime_sdk_root(root, {}, !exe().empty());
        host.pins = verified_activation_sdk_pins(root, host.sdk, scoped);
    }

    host.clean_base = (seed.empty() ? build_target(root, profile, machine) : seed) / "android";
    host.android_port = 5574;
    // The refresh parses the shared document and CASes its platform view, so the snapshot is that
    // same view of the shared document.
    host.require_cluster = require_cluster;
    host.refresh_host = refresh_host;

    if (refresh_host) {
        host.refresh_source = qnx_host_binding_view(root, shared_bindings);
    }

    if (root_program_only) {
        need(refresh_host, "root program refresh requires stopped host migration");
        const auto scoped = profile_launch_directory(root, profile, machine);
        const auto prior = read_private_json(scoped / "android-launch.json");
        host.ui_observer = prior.at("ui_observer");
        need(hash(host.ui_observer.at("dex").get<std::string>(), 8ULL << 20) ==
                     host.ui_observer.at("sha256").get<std::string>(),
             "retained observer pin mismatch");
    } else if (manifest.contains("ui_observer")) {
        const auto source = seed.empty() ? root / "machines" / machine.name : seed;
        const auto &observer = manifest.at("ui_observer");
        host.ui_observer = {{"dex", (source / "observer/classes.dex").string()},
                            {"sha256", observer.at("sha256")}};

        if (portable_api30_ui_observer_profile(profile)) {
            host.ui_observer["profile"] = std::string(profile);
        }
    }

    if (!host.ui_observer.empty()) {
        host.pins[host.ui_observer.at("dex").get<std::string>()] =
                host.ui_observer.at("sha256").get<std::string>();
    }

#if defined(_WIN32) || defined(__APPLE__)
    if (require_cluster && manifest.contains("qnx") && !std::filesystem::exists(binding_file)) {
        ensure_qnx_host_bindings(root, true);
        bindings = qnx_host_binding_view(root, read_bindings());
    }
#endif
    const bool passion = record_declares(portable_profile_definition(root, profile, machine, seed),
                                         ProfilePlatform::passion, ProfileBoard::sa8155);
    host.android_port = passion ? 5590u : bindings.value("android_port", 5574u);
    host.qmp_port = bindings.value("qmp_port", 45995u);

    if (bindings.contains("frontend") && !passion) {
        auto frontend = bindings.at("frontend");
#if defined(_WIN32) || defined(__APPLE__)
        if (refresh_host) {
            const auto program = root / ("voyemu" + exe());
            need(std::filesystem::path(frontend.at("executable").get<std::string>()) == program,
                 "host refresh requires exact root frontend program");
            unsigned count = 0;

            for (auto &item : frontend["pins"]) {
                if (std::filesystem::path(item.at("path").get<std::string>()) == program) {
                    item["sha256"] = hash(program, 128ULL << 20);
                    ++count;
                }
            }

            need(count == 1, "exact root frontend pin required for host refresh");
        }
#endif
        host.frontend = profile_frontend_bindings(root, profile, frontend, host.pins);
    }
#if (defined(__APPLE__) || defined(_WIN32)) && defined(VOYEMU_HAS_WINDOW)
    else {
        host.frontend = profile_built_frontend_bindings(root, profile, machine, host.pins, seed);
    }
#endif
    bool refresh_cluster = false;
#if defined(_WIN32) || defined(__APPLE__)
    if (refresh_host) {
        const auto scoped = profile_launch_directory(root, profile, machine);

        if (std::filesystem::exists(scoped / "activation.json")) {
            refresh_cluster =
                    read_private_json(scoped / "activation.json").contains("cluster_launch_sha256");
        }
    }
#endif

    if ((require_cluster || refresh_cluster) && manifest.contains("qnx")) {
        need(!bindings.empty(), "explicit local QNX host bindings required");
        host.qnx_qemu = bindings.at("qnx_qemu").get<std::string>();
        host.graphics = bindings.at("graphics").get<std::string>();
        host.angle = bindings.at("angle").get<std::string>();

        for (const auto &item : bindings.at("pins")) {
            const std::filesystem::path file = item.at("path").get<std::string>();

            if (file == host.qnx_qemu) {
                ordinary(file);
                need(std::filesystem::canonical(file) == file &&
                             file.filename() == "qemu-system-aarch64" + exe(),
                     "explicit canonical installed QNX engine required");
            } else {
                inside(root, file);
            }

            auto expected = item.at("sha256").get<std::string>();
#if defined(_WIN32) || defined(__APPLE__)
            if (refresh_cluster && file == root / ("voyemu" + exe()) && host.graphics == file &&
                host.frontend.value("executable", std::string{}) == file.string()) {
                expected = host.pins.at(file.string());
            }
#endif
            need(hash(file, 512ULL << 20) == expected, "QNX host pin mismatch");
            host.pins[file.string()] = expected;
        }

        need(!host.qnx_qemu.empty() && !host.graphics.empty() && !host.angle.empty(),
             "QNX host engine/graphics bindings missing");
    }

    return activate_portable_runtime(root, profile, machine, host, 20ULL << 30);
}
} // namespace voyemu
