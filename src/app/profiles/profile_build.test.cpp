#include "profile_build.hpp"
#include "api30_framework_boot.hpp"
#include "base_image.hpp"
#include "cli_arguments.hpp"
#include "java_toolchain.hpp"
#include "machine_preparation.hpp"
#include "model.hpp"
#include "os/fs.hpp"
#include "portable_resources.hpp"
#include "portable_runtime.hpp"
#include "process.hpp"
#include "profile_build_internal.hpp"
#include "profile_catalog.hpp"
#include "qinggan09_owned_packages.hpp"
#include "qinggan09_phone_package.hpp"
#include "qnx_guest_build.hpp"
#include "qnx_host.hpp"
#include "qnx_runtime_layout.hpp"
#include "quickboot.hpp"
#include "startup_video_fixture.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include "os/stat_time.hpp"
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

// The QNX reuse flows still pass through profile build gates keyed by this id.
static constexpr std::string_view qnx_reuse_profile = "free-11-chinese-20260731-7.3.1";

static Json qinggan09_signature_input_identity(const std::filesystem::path &file) {
#ifdef _WIN32
    const auto handle = CreateFileW(file.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("QINGGAN09_OWNED_PACKAGES: signature input unavailable");
    }

    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    const bool valid =
            GetFileInformationByHandle(handle, &info) &&
            GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof basic) &&
            info.nNumberOfLinks > 0 &&
            !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    CloseHandle(handle);

    if (!valid) {
        throw std::runtime_error("QINGGAN09_OWNED_PACKAGES: signature input identity differs");
    }

    return {{"volume", info.dwVolumeSerialNumber},
            {"index_high", info.nFileIndexHigh},
            {"index_low", info.nFileIndexLow},
            {"links", info.nNumberOfLinks},
            {"size_high", info.nFileSizeHigh},
            {"size_low", info.nFileSizeLow},
            {"mtime_high", info.ftLastWriteTime.dwHighDateTime},
            {"mtime_low", info.ftLastWriteTime.dwLowDateTime},
            {"change_time", basic.ChangeTime.QuadPart}};
#else
    struct stat info{};

    if (::lstat(file.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink < 1) {
        throw std::runtime_error("QINGGAN09_OWNED_PACKAGES: signature input identity differs");
    }

    return {{"device", info.st_dev},
            {"inode", info.st_ino},
            {"links", info.st_nlink},
            {"size", info.st_size},
            {"mtime_seconds", os::modified_time(info).tv_sec},
            {"mtime_ns", os::modified_time(info).tv_nsec},
            {"ctime_seconds", os::changed_time(info).tv_sec},
            {"ctime_ns", os::changed_time(info).tv_nsec}};
#endif
}

static std::filesystem::path qinggan09_signature_java() {
#ifdef _WIN32
    const std::string executable = "java.exe";
    const char separator = ';';
#else
    const std::string executable = "java";
    const char separator = ':';
#endif

    if (const char *home = std::getenv("JAVA_HOME"); home && *home) {
        const std::filesystem::path directory = home;

        if (!directory.is_absolute()) {
            throw std::runtime_error("QINGGAN09_OWNED_PACKAGES: absolute JAVA_HOME required");
        }

        return std::filesystem::canonical(directory / "bin" / executable);
    }

    if (const char *value = std::getenv("PATH")) {
        const std::string paths = value;
        std::size_t at = 0;

        while (at <= paths.size()) {
            auto end = paths.find(separator, at);

            if (end == std::string::npos) {
                end = paths.size();
            }

            const std::filesystem::path folder = paths.substr(at, end - at);

            if (folder.is_absolute() && std::filesystem::is_regular_file(folder / executable)) {
                return std::filesystem::canonical(folder / executable);
            }

            if (end == paths.size()) {
                break;
            }

            at = end + 1;
        }
    }

    throw std::runtime_error(
            "QINGGAN09_OWNED_PACKAGES: installed Java required for signature verification");
}

static void checked_qinggan09_phone_source_file(const std::filesystem::path &root,
                                                const std::filesystem::path &file,
                                                const std::string &pin, std::uint64_t bound,
                                                bool shared_sdk_platform = false) {
    const auto require = [](bool value) {
        if (!value) {
            throw std::runtime_error(
                    "QINGGAN09_PHONE_PACKAGE: pinned canonical source input differs");
        }
    };

    require(file.is_absolute() && file.lexically_normal() == file &&
            std::filesystem::canonical(file) == file && qinggan09_phone_package_hash(pin));

    for (auto at = file; !at.empty(); at = at.parent_path()) {
        require(!std::filesystem::is_symlink(std::filesystem::symlink_status(at)));

        if (at == at.parent_path()) {
            break;
        }
    }

    const auto relative = file.lexically_relative(root);
    require(!relative.empty() && !relative.is_absolute() && *relative.begin() != "..");
    // The canonical SDK platform JAR is an immutable, hash-fenced build input.
    // Shared cache aliases are allowed only here, never for a payload or receipt.
    require(!shared_sdk_platform ||
            file == root / ".local/toolchains/java-sdk/platforms/android-34/android.jar");
    require(std::filesystem::is_regular_file(file) &&
            (shared_sdk_platform || std::filesystem::hard_link_count(file) == 1) &&
            std::filesystem::file_size(file) <= bound);
    const auto identity = [&]() -> Json {
        if (!shared_sdk_platform) {
            return cache_file_identity(file);
        }

#ifdef _WIN32
        const auto handle = CreateFileW(
                file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        require(handle != INVALID_HANDLE_VALUE);
        BY_HANDLE_FILE_INFORMATION info{};
        FILE_BASIC_INFO basic{};
        const bool valid = GetFileInformationByHandle(handle, &info) &&
                           GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                                        sizeof basic) &&
                           info.nNumberOfLinks > 0 &&
                           !(info.dwFileAttributes &
                             (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
        CloseHandle(handle);
        require(valid);

        return {{"volume", info.dwVolumeSerialNumber},
                {"index_high", info.nFileIndexHigh},
                {"index_low", info.nFileIndexLow},
                {"links", info.nNumberOfLinks},
                {"size_high", info.nFileSizeHigh},
                {"size_low", info.nFileSizeLow},
                {"mtime_high", info.ftLastWriteTime.dwHighDateTime},
                {"mtime_low", info.ftLastWriteTime.dwLowDateTime},
                {"change_time", basic.ChangeTime.QuadPart}};
#else
        struct stat info{};
        require(::lstat(file.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                info.st_uid == ::geteuid() && info.st_nlink > 0);

        return {{"device", info.st_dev},
                {"inode", info.st_ino},
                {"links", info.st_nlink},
                {"size", info.st_size},
                {"mtime_seconds", os::modified_time(info).tv_sec},
                {"mtime_ns", os::modified_time(info).tv_nsec},
                {"ctime_seconds", os::changed_time(info).tv_sec},
                {"ctime_ns", os::changed_time(info).tv_nsec}};
#endif
    };
    const auto before = identity();
    require(sha256_file(file) == pin && identity() == before);
}

static void artifact_resume_checks(const std::filesystem::path &root, bool equipment = false) {
    const auto put = [](const std::filesystem::path &file, const std::string &bytes) {
        ensure_private_directory(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };
    const std::string profile = equipment ? "test-qinggan09" : "artifact-resume-fixture";
    const auto definition = root / "profiles" / profile / "profile.json";

    put(definition, "{\"schema\":1}\n");

    if (equipment) {
        write_private_json(
                definition,
                {{"schema", 1},
                 {"platform", "qinggan09"},
                 {"board", "8155"},
                 {"android", {{"package", "system-images;android-28;default;arm64-v8a"}}},
                 {"files",
                  {{"hybrid-nvram",
                    {{"file", "nvram.txt"},
                     {"simulation",
                      {{"kind", "qinggan09-equipment-item51-v1"},
                       {"powertrain", "hybrid"},
                       {"otherOptions",
                        "Preserved synthetic item51 options; not a physical vehicle capture"}}}}},
                   {"nvram", {{"file", "nvram.txt"}}}}}});
        put(definition.parent_path() / "files/nvram.txt", "51=1911F08F808000300000000001\n");
    }

    const auto original_definition = read_private_json_bytes(definition, 1 << 20);
    const auto base = root / ".local/profiles/common/base",
               sdk = root / ".local/toolchains/native-sdk", work = root / ".local/build/work",
               transport = root / ".local/build/tmp/artifact-transport",
               source = root / "src/prepare/java/com/voyemu/prepare/PreparationMain.java";

    for (const auto &directory : {base, sdk, work, transport}) {
        ensure_private_directory(directory);
    }

    put(base / "base.json", "{}");
    put(source, "producer");

    for (const auto &[name, text] : std::array<std::pair<const char *, const char *>, 4>{
                 {{"src/prepare/JavaHelperBuild.java",
                   "package com.voyemu.prepare;\n\npublic final class JavaHelperBuild {}\n"},
                  {"src/prepare/test/fixtures/com/voyemu/prepare/fixture/BuildMarker.java",
                   "class BuildMarker {}\n"},
                  {"src/guest/android/oem/audio/OemAudioPolicy.java", "class Policy {}\n"},
                  {"src/guest/android/oem/audio/OemAudioPolicyUnavailable.java",
                   "class Unavailable {}\n"}}}) {
        const auto target = root / name;
        ensure_private_directory(target.parent_path());
        put(target, text);
    }

    for (const auto *name :
         {"src/prepare/libraries.json", "src/prepare/toolchain.json", "src/app/toolchain.json"}) {
        put(root / name, "{}");
    }

    const auto dependency_sha = sha256_bytes("dependency");
    write_private_json(root / "src/prepare/libraries.json",
                       Json::array({{{"filename", "fixture.jar"}, {"sha256", dependency_sha}}}));
    write_private_json(root / "src/prepare/toolchain.json",
                       {{"libraries",
                         {{"d8", {{"sha256", dependency_sha}}},
                          {"apksigner", {{"sha256", dependency_sha}}}}}});

    const auto fingerprint = sha256_bytes("artifact-resume-helper-generation");
    const auto helper = root / ".local/toolchains/java-helper" / fingerprint /
                        "voyemu-java-helper.jar";
    put(helper, "helper");
    const auto dependency = helper.parent_path() / "lib/fixture.jar";
    put(dependency, "dependency");
    Json producer = {{"schema", 1}, {"kind", "java-preparation-helper"}};
    producer["dependencies"] = Json::array();

    for (const auto *name : {"fixture.jar", "d8.jar", "apksigner.jar"}) {
        const auto relative = std::string("lib/") + name;
        put(helper.parent_path() / relative, "dependency");
        producer["dependencies"].push_back(
                {{"file", relative}, {"sha256", dependency_sha}, {"bytes", 10}});
    }

    write_private_json(helper.parent_path() / "build-receipt.json", producer);
    write_private_json(
            helper.parent_path() / "toolchain-receipt.json",
            {{"schema", 1},
             {"kind", "java-preparation-toolchain"},
             {"fingerprint", fingerprint},
             {"helper_sha256", sha256_file(helper)},
             {"build_receipt_sha256", sha256_file(helper.parent_path() / "build-receipt.json")}});
    Json files = Json::object();
    files["profile.json"] = sha256_file(definition);
    const auto output = work / "fixture.apk";
    put(output, "built output");
    Json result = {
            {"schema", 1},
            {"operation", "build-artifacts"},
            {"artifacts_built", true},
            {"machine_ready", false},
            {"guest_started", false},
            {"artifacts",
             {{"component:fixture",
               {{"file", output.string()}, {"sha256", sha256_file(output)}, {"bytes", 12}}}}},
            {"profile", profile}};

    if (equipment) {
        std::string bytes(4 * 1024 * 1024, char(0));
        bytes[0] = 1;

        for (const auto offset : {0x860, 0x20860}) {
            bytes.replace(offset, 26, "1911F08F808000300000000001");
        }

        if (sha256_bytes(bytes) !=
            "8faa88acffee103b6d6a9ebb47c59e6fcc2eca9469084abf4e76154364bec079") {
            throw std::runtime_error("Java/C++ equipment byte parity differs");
        }

        const auto raw = work / "qinggan09-equipment-nvram.img";
        put(raw, bytes);
        result["artifacts"]["files:hybrid-nvram"] = {
                {"file", raw.string()}, {"sha256", sha256_file(raw)}, {"bytes", bytes.size()}};
    }

    write_private_json(transport / "result.json", result);
    const auto receipt_file = transport / "profile-artifacts.json";
    Json receipt = {{"schema", 1},
                    {"kind", "native-profile-artifacts"},
                    {"profile", profile},
                    {"machine_ready", false},
                    {"guest_started", false},
                    {"base", base.string()},
                    {"base_receipt_sha256", sha256_file(base / "base.json")},
                    {"sdk", sdk.string()},
                    {"work", work.string()},
                    {"transport", transport.string()},
                    {"result_sha256", sha256_file(transport / "result.json")},
                    {"java_tools",
                     {{"fingerprint", fingerprint},
                      {"helper", helper.string()},
                      {"helper_sha256", sha256_file(helper)},
                      {"helper_receipt_sha256",
                       sha256_file(helper.parent_path() / "build-receipt.json")}}}};
    const auto extracted = root / ".local/extracted", input = extracted / "system/test",
               extraction = extracted / "receipt.json",
               native = root / ".local/native/receipt.json",
               native_output = native.parent_path() / "fixture.output";
    put(input, "extracted input");
    write_private_json(
            extraction,
            {{"files", {{"system/test", {{"sha256", sha256_file(input)}, {"bytes", 15}}}}}});
    put(native_output, "native output");
    const Json provenance = {{"profile", profile}};
    write_private_json(native,
                       {{"profile_provenance", provenance},
                        {"files", Json::array({{{"input", input.string()},
                                                {"input_sha256", sha256_file(input)},
                                                {"output", "fixture.output"},
                                                {"output_sha256", sha256_file(native_output)}}})}});
    receipt["android_inputs"] = Json::array({"/system/test"});
    receipt["native_files"] = {{"receipt", native.string()},
                               {"receipt_sha256", sha256_file(native)},
                               {"provenance", provenance}};
    const auto patch = transport / "patch.json", key = transport / "key",
               certificate = transport / "certificate", sdk_tool = sdk / "fixture";
    put(patch, "{}");
    put(key, "key");
    put(certificate, "cert");
    put(sdk_tool, "tool");
    Json request = {
            {"patch_data", patch.string()},
            {"patch_data_sha256", sha256_file(patch)},
            {"tools",
             {{"sdk", sdk.string()},
              {"manifest", "{}"},
              {"manifest_sha256", sha256_file(root / "src/app/toolchain.json")},
              {"signing",
               {{"key", key.string()},
                {"key_sha256", sha256_file(key)},
                {"certificate", certificate.string()},
                {"certificate_sha256", sha256_file(certificate)}}},
              {"files", {{"fixture", sha256_file(sdk_tool)}}}}},
            {"root", root.string()},
            {"profile", profile},
            {"profile_sha256", sha256_file(definition)},
            {"work", work.string()},
            {"android_inputs",
             {{"/system/test",
               {{"receipt", extraction.string()}, {"receipt_sha256", sha256_file(extraction)}}}}},
            {"native_files",
             {{"receipt", native.string()}, {"receipt_sha256", sha256_file(native)}}}};
    write_private_json(transport / "request.json", request);
    write_private_json(
            transport / "receipt.json",
            {{"schema", 1},
             {"kind", "java-preparation-invocation"},
             {"profile", profile},
             {"operation", "build-artifacts"},
             {"guest_started", false},
             {"result_sha256", receipt.at("result_sha256")},
             {"helper_sha256", receipt.at("java_tools").at("helper_sha256")},
             {"helper_receipt_sha256", receipt.at("java_tools").at("helper_receipt_sha256")},
             {"request_sha256", sha256_file(transport / "request.json")}});
    receipt["invocation_sha256"] = sha256_file(transport / "receipt.json");
    receipt["request_sha256"] = sha256_file(transport / "request.json");
    write_private_json(receipt_file, receipt);
    const auto pin = sha256_file(receipt_file);

    if (validate_profile_artifact_resume(root, profile, receipt_file, pin) != receipt) {
        throw std::runtime_error("valid artifact resume refused");
    }

    put(definition, original_definition + "\n");

    if (validate_profile_artifact_resume(root, profile, receipt_file, pin) != receipt) {
        throw std::runtime_error("authoring profile whitespace invalidated prepared artifacts");
    }

    put(definition, original_definition);

    const auto refuse = [&](const std::string &expected_pin) {
        bool denied = false;
        try {
            validate_profile_artifact_resume(root, profile, receipt_file, expected_pin);
        } catch (...) {
            denied = true;
        }

        if (!denied) {
            throw std::runtime_error("changed artifact epoch accepted");
        }
    };

    if (equipment) {
        const auto raw = work / "qinggan09-equipment-nvram.img";
        std::ifstream in(raw, std::ios::binary);
        const std::string original((std::istreambuf_iterator<char>(in)), {});
        in.close();
        const auto original_result = result, original_receipt = receipt,
                   original_invocation = read_private_json(transport / "receipt.json");
        const auto publish = [&]() {
            write_private_json(transport / "result.json", result);
            receipt["result_sha256"] = sha256_file(transport / "result.json");
            auto invocation = original_invocation;
            invocation["result_sha256"] = receipt.at("result_sha256");
            write_private_json(transport / "receipt.json", invocation);
            receipt["invocation_sha256"] = sha256_file(transport / "receipt.json");
            write_private_json(receipt_file, receipt);

            return sha256_file(receipt_file);
        };

        for (const auto offset : {0, 0x860, 0x20860, 0x11e0, 0x211e0, 0x87f}) {
            auto changed = original;
            changed[offset] = offset == 0 ? 0 : char(0x7f);
            put(raw, changed);
            result["artifacts"]["files:hybrid-nvram"]["sha256"] = sha256_file(raw);
            refuse(publish());
            put(raw, original);
            result = original_result;
            receipt = original_receipt;
        }

        const auto foreign = work / "foreign.img";
        put(foreign, original);
        result["artifacts"]["files:hybrid-nvram"]["file"] = foreign.string();
        refuse(publish());
        result = original_result;
        receipt = original_receipt;
        result["artifacts"]["files:unrelated"] = {
                {"file", raw.string()}, {"sha256", sha256_file(raw)}, {"bytes", original.size()}};
        refuse(publish());
        result = original_result;
        receipt = original_receipt;
        write_private_json(transport / "result.json", result);
        write_private_json(transport / "receipt.json", original_invocation);
        write_private_json(receipt_file, receipt);
    }

    refuse(std::string(64, '0'));
    const auto invocation = read_private_json(transport / "receipt.json");
    auto bad_invocation = invocation;
    bad_invocation["request_sha256"] = std::string(64, '0');
    write_private_json(transport / "receipt.json", bad_invocation);
    refuse(pin);
    write_private_json(transport / "receipt.json", invocation);
    auto bad_request = request;
    bad_request["patch_data_sha256"] = std::string(64, '0');
    write_private_json(transport / "request.json", bad_request);
    refuse(pin);
    write_private_json(transport / "request.json", request);
    put(source, "changed producer");

    if (validate_profile_artifact_resume(root, profile, receipt_file, pin) != receipt) {
        throw std::runtime_error("unrelated producer edit refused");
    }

    put(source, "producer");
    const auto added = source.parent_path() / "NewProducer.java";
    put(added, "new producer");

    if (validate_profile_artifact_resume(root, profile, receipt_file, pin) != receipt) {
        throw std::runtime_error("unrelated producer addition refused");
    }

    std::filesystem::remove(added);
    const auto fixture = root /
                         "src/prepare/test/fixtures/com/voyemu/prepare/fixture/BuildMarker.java";
    std::ifstream marker_file(fixture, std::ios::binary);
    const std::string marker((std::istreambuf_iterator<char>(marker_file)), {});
    // Windows refuses to delete a file that is still open for reading.
    marker_file.close();
    std::filesystem::remove(fixture);

    if (validate_profile_artifact_resume(root, profile, receipt_file, pin) != receipt) {
        throw std::runtime_error("later fixture edit refused");
    }

    put(fixture, marker);

    for (const auto *name :
         {"src/prepare/libraries.json", "src/prepare/toolchain.json", "src/app/toolchain.json"}) {
        const auto original = read_private_json_bytes(root / name, 1 << 20);
        put(root / name, original + "\n");

        if (validate_profile_artifact_resume(root, profile, receipt_file, pin) != receipt) {
            throw std::runtime_error("metadata formatting invalidated prepared artifacts");
        }

        put(root / name, "changed metadata");
        refuse(pin);
        put(root / name, original);
    }

    const auto builder = root / "src/prepare/JavaHelperBuild.java";
    std::ifstream builder_file(builder, std::ios::binary);
    const std::string builder_text((std::istreambuf_iterator<char>(builder_file)), {});
    put(builder, builder_text.substr(0, builder_text.size() - 2) + "// changed builder\n}\n");

    if (validate_profile_artifact_resume(root, profile, receipt_file, pin) != receipt) {
        throw std::runtime_error("later builder edit refused");
    }

    put(builder, builder_text);
    put(dependency, "corrupt");
    refuse(pin);
    put(dependency, "dependency");
    put(output, "corrupt");
    refuse(pin);
    put(output, "built output");
    put(input, "corrupt");
    refuse(pin);
    put(input, "extracted input");
    put(native_output, "corrupt");
    refuse(pin);
    put(native_output, "native output");
    put(sdk_tool, "corrupt");
    refuse(pin);
    put(sdk_tool, "tool");

    put(definition, "{\"schema\":1,\"changed\":true}\n");

    if (equipment) {
        // Removing the declared simulation changes the artifact contract itself.
        refuse(pin);
    } else if (validate_profile_artifact_resume(root, profile, receipt_file, pin) != receipt) {
        throw std::runtime_error("unrelated authoring metadata invalidated prepared artifacts");
    }

    put(definition, original_definition);
    put(base / "base.json", "changed base");
    refuse(pin);
    put(base / "base.json", "{}");
    auto bad = receipt;
    bad["guest_started"] = true;
    write_private_json(receipt_file, bad);
    refuse(sha256_file(receipt_file));
    write_private_json(receipt_file, receipt);
    bad = receipt;
    bad["sdk"] = work.string();
    write_private_json(receipt_file, bad);
    refuse(sha256_file(receipt_file));
    write_private_json(receipt_file, receipt);
    put(transport / "result.json", "{}");
    refuse(pin);
    write_private_json(transport / "result.json", result);

    if (validate_profile_artifact_resume(root, profile, receipt_file, pin) != receipt) {
        throw std::runtime_error("restored artifact epoch refused");
    }

    {
        const os::FileLock releasing(work / "build-output.lock", true);

        if (!releasing.held()) {
            throw std::runtime_error("artifact release fixture lock refused");
        }

        refuse(pin);
    }
    {
        const os::FileLock reader(work / "build-output.lock", false),
                independent(transport / "build-output.lock", true);

        if (!reader.held() || !independent.held() ||
            validate_profile_artifact_resume(root, profile, receipt_file, pin) != receipt) {
            throw std::runtime_error("shared or independent artifact reader refused");
        }
    }
    receipt.erase("android_inputs");
    receipt.erase("native_files");
    receipt.erase("invocation_sha256");
    receipt.erase("request_sha256");
    write_private_json(receipt_file, receipt);
}

// One-step build generation release: the published machine and evidence stay, owned payloads go;
// a leased directory, a payload that changed since its receipt, shared inputs, reused QNX and an
// unverified publication are kept and reported.
static void build_output_release_checks(const std::filesystem::path &stage) {
    const auto put = [](const std::filesystem::path &file, const std::string &bytes) {
        ensure_private_directory(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };
    const auto descriptor = [](const std::filesystem::path &file, const std::string &name) {
        return Json{{"file", name},
                    {"sha256", sha256_file(file)},
                    {"bytes", std::filesystem::file_size(file)}};
    };
    const auto expect = [](bool yes, const std::string &label, const Json &result) {
        if (!yes) {
            throw std::runtime_error("build output release: " + label + ": " + result.dump());
        }
    };

    enum class Case {
        combined,
        leased,
        changed,
        unpublished,
        reused,
        shared_artifacts,
        stale_origin,
        independent_artifacts,
        leased_artifacts
    };
    const auto run = [&](const std::string &label, Case kind) {
        const auto root = stage / label, build = root / ".local/build",
                   android = build / "candidate-fixture", work = build / "artifacts-fixture",
                   transport = build / "tmp/artifact-transport-fixture",
                   source = build / "qnx-source-fixture", accepted = build / "qnx-accepted-fixture",
                   machine = root / "machines/fixture",
                   shared = root / ".local/profiles/fixture/original.apk";
        put(android / "android/system.img", "candidate system image");
        put(android / "android/packages/owned.apk", "exported candidate APK");
        put(android / "machine/base/system.img", "closed working disk");
        put(android / "proof.png", "frame evidence");
        put(android / "undeclared.apk", "useful APK evidence");
        write_private_json(android / "receipt.json",
                           {{"schema", 1},
                            {"kind", "android-candidate"},
                            {"stopped", true},
                            {"cold_boot_verified", true},
                            {"android",
                             {{"images",
                               {{"system.img", descriptor(android / "android/system.img",
                                                          "android/system.img")}}}}},
                            {"owned_package", descriptor(android / "android/packages/owned.apk",
                                                         "android/packages/owned.apk")}});
        put(work / "patched.apk", "generated APK");
        put(work / "library/lib.so", "generated library");
        put(work / "decoded/classes.dex", "undeclared scratch");
        put(work / "signing.jks", "signing authority");
        put(work / "source.patch", "source patch");
        put(shared, "shared OEM input");
        write_private_json(
                transport / "result.json",
                {{"artifacts",
                  {{"patched", descriptor(work / "patched.apk", (work / "patched.apk").string())},
                   {"library",
                    {{"directory", (work / "library").string()},
                     {"files",
                      {{"lib.so",
                        {{"sha256", sha256_file(work / "library/lib.so")}, {"bytes", 17}}}}}}},
                   {"original", descriptor(shared, shared.string())}}}});
        write_private_json(transport / "profile-artifacts.json",
                           {{"work", work.string()},
                            {"transport", transport.string()},
                            {"result_sha256", sha256_file(transport / "result.json")}});
        write_private_json(
                android / "artifact-origin.json",
                {{"artifact_receipt", (transport / "profile-artifacts.json").string()},
                 {"candidate_receipt_sha256", sha256_file(android / "receipt.json")},
                 {"artifact_receipt_sha256", sha256_file(transport / "profile-artifacts.json")}});
        put(source / "hyp-hvf.img", "source QNX image");
        put(source / "guest-src/unique.c", "guest source evidence");
        write_private_json(
                source / "build.json",
                {{"images", {{"hyp-hvf.img", descriptor(source / "hyp-hvf.img", "hyp-hvf.img")}}}});
        put(accepted / "qnx/hyp-hvf.img", "accepted QNX image");
        write_private_json(
                accepted / "acceptance.json",
                {{"images",
                  {{"hyp-hvf.img", descriptor(accepted / "qnx/hyp-hvf.img", "qnx/hyp-hvf.img")}}}});
        put(machine / "android/system.img", "candidate system image");
        write_private_json(machine / "runtime.json", {{"schema", 1}});
        Json publication = {{"machine_ready", true},
                            {"runtime", (machine / "runtime.json").string()},
                            {"runtime_sha256", sha256_file(machine / "runtime.json")}};

        if (kind == Case::shared_artifacts || kind == Case::stale_origin ||
            kind == Case::independent_artifacts) {
            const auto other = build / "candidate-other";
            put(other / "android/system.img", "candidate system image");
            auto other_receipt = read_private_json(android / "receipt.json", 1 << 20);

            if (kind == Case::independent_artifacts) {
                other_receipt["profile"] = "another-profile";
            }

            write_private_json(other / "receipt.json", other_receipt);
            auto origin_receipt = transport / "profile-artifacts.json";

            if (kind == Case::independent_artifacts) {
                const auto other_work = build / "artifacts-other";
                put(other_work / "patched.apk", "another profile APK");
                origin_receipt = build / "tmp/other-transport/profile-artifacts.json";
                write_private_json(origin_receipt, {{"work", other_work.string()}});
            }

            write_private_json(
                    other / "artifact-origin.json",
                    {{"artifact_receipt", origin_receipt.string()},
                     {"artifact_receipt_sha256", sha256_file(origin_receipt)},
                     {"candidate_receipt_sha256", kind == Case::stale_origin
                                                          ? std::string(64, '0')
                                                          : sha256_file(other / "receipt.json")}});
        }

        if (kind == Case::unpublished) {
            publication["runtime_sha256"] = std::string(64, '0');
        }

        if (kind == Case::changed) {
            put(android / "android/system.img", "replaced after the receipt");
        }

        std::optional<os::FileLock> consumer;

        if (kind == Case::leased) {
            consumer.emplace(accepted / "build-output.lock", false);
        } else if (kind == Case::leased_artifacts) {
            consumer.emplace(work / "build-output.lock", false);
        }

        const bool qnx = kind != Case::reused;
        const auto result = release_build_generation(root, publication, android,
                                                     qnx ? source : std::filesystem::path(),
                                                     qnx ? accepted : std::filesystem::path());
        const auto &report = result.at("build_outputs");
        const auto exists = [](const std::filesystem::path &file) {
            return std::filesystem::exists(file);
        };
        expect(result.at("machine_ready") == true && exists(machine / "android/system.img") &&
                       exists(android / "receipt.json") && exists(android / "proof.png") &&
                       exists(android / "undeclared.apk") && exists(shared) &&
                       exists(work / "signing.jks") && exists(work / "source.patch") &&
                       exists(transport / "result.json") && exists(source / "build.json") &&
                       exists(source / "guest-src/unique.c") &&
                       exists(accepted / "acceptance.json"),
               label + " lost the machine, evidence or a shared input", result);

        if (kind == Case::unpublished) {
            expect(report.at("released_bytes") == 0 && report.at("retained").size() == 1 &&
                           exists(android / "android/system.img") && exists(work / "patched.apk") &&
                           exists(accepted / "qnx/hyp-hvf.img"),
                   "unverified publication released outputs", result);

            return;
        }

        if (kind == Case::shared_artifacts || kind == Case::leased_artifacts) {
            expect(exists(work / "patched.apk") && exists(work / "library/lib.so") &&
                           exists(work / "decoded/classes.dex") &&
                           !exists(android / "android/system.img") &&
                           !exists(android / "machine") && report.at("retained").size() == 1,
                   "inactive candidate lost its shared artifact generation", result);

            if (kind == Case::shared_artifacts) {
                const auto other = build / "candidate-other";
                const auto finished = release_build_generation(root, publication, other);
                expect(!exists(work / "patched.apk") && !exists(work / "library/lib.so") &&
                               !exists(other / "android/system.img") &&
                               finished.at("build_outputs").at("retained").empty(),
                       "published candidate origin leaked the artifact generation", finished);
            }

            return;
        }

        if (kind == Case::independent_artifacts) {
            expect(exists(build / "artifacts-other/patched.apk"),
                   "another profile artifact generation changed", result);
        }

        expect(!exists(android / "machine") && !exists(android / "android/packages/owned.apk") &&
                       !exists(work / "patched.apk") && !exists(work / "library/lib.so") &&
                       !exists(work / "decoded/classes.dex"),
               label + " retained an owned payload", result);
        expect(exists(android / "android/system.img") == (kind == Case::changed),
               "published one-step candidate RAW retained or changed payload released", result);
        expect(exists(source / "hyp-hvf.img") == !qnx &&
                       exists(accepted / "qnx/hyp-hvf.img") == (!qnx || kind == Case::leased),
               label + " QNX release differs", result);
        expect(report.at("retained").size() ==
                       (kind == Case::leased || kind == Case::changed ? 1U : 0U),
               label + " retained report differs", result);
    };
    run("combined", Case::combined);
    run("leased", Case::leased);
    run("changed", Case::changed);
    run("unpublished", Case::unpublished);
    run("reused", Case::reused);
    run("shared-artifacts", Case::shared_artifacts);
    run("stale-origin", Case::stale_origin);
    run("independent-artifacts", Case::independent_artifacts);
    run("leased-artifacts", Case::leased_artifacts);
}

// Regression evidence for publisher GC of closed candidate VM working disks.
// Positive path: a stopped, cold-verified android candidate with a private working-disk tree
// is collected (machine/ removed, receipt/exported images/logs retained, gc.json written).
// Negative paths reject: live (stopped=false) candidate, failed cold verification, missing
// receipt, symlink inside working tree, outside-project candidate path, and re-run idempotence.
static void gc_working_disks_checks(const std::filesystem::path &stage) {
    const auto root = stage / "gc-root";
    const auto candidate = root / ".local/build/candidate-gc-fixture";

    ensure_private_directory(candidate / "machine/base");
    ensure_private_directory(candidate / "android");
    const auto put = [](const std::filesystem::path &file, const std::string &bytes) {
        ensure_private_directory(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };
    put(candidate / "machine/base/system.img", "working base");
    put(candidate / "machine/system.img.qcow2", "overlay");
    put(candidate / "machine/config.ini", "avd");
    put(candidate / "android/system.img", "exported image");
    put(candidate / "receipt.json", "{}");
    // helper to write a valid closed receipt
    const auto write_receipt = [&](bool stopped, bool cold) {
        Json receipt = {{"schema", 1},
                        {"kind", "android-candidate"},
                        {"profile", "gc-fixture"},
                        {"stopped", stopped},
                        {"cold_boot_verified", cold}};
        write_private_json(candidate / "receipt.json", receipt);
    };
    const char *const qemu_img_env = std::getenv("VOYEMU_TEST_QEMU_IMG");

    if (!qemu_img_env || !*qemu_img_env) {
        throw std::runtime_error("GC scratch requires VOYEMU_TEST_QEMU_IMG");
    }

    const std::filesystem::path real_qemu_img = qemu_img_env;
    const auto expect_refuse = [&](const char *label) {
        bool denied = false;
        try {
            purge_android_candidate_working_disks(root, candidate, real_qemu_img);
        } catch (...) {
            denied = true;
        }

        if (!denied) {
            throw std::runtime_error(std::string("GC accepted invalid state: ") + label);
        }
    };
    // unverified receipt content ({} not schema-valid) rejected
    expect_refuse("malformed receipt");
    write_receipt(false, true);
    expect_refuse("live candidate");
    write_receipt(true, false);
    expect_refuse("unverified candidate");
    std::filesystem::remove(candidate / "receipt.json");
    expect_refuse("missing receipt");
    write_receipt(true, true);
    // symlink inside working tree rejected
    put(candidate / "machine/evil-link", "x");
    std::filesystem::remove(candidate / "machine/evil-link");
    std::filesystem::create_directory(root / "outside");
    put(root / "outside/target.img", "o");
    std::filesystem::create_symlink(root / "outside/target.img", candidate / "machine/evil-link");
    expect_refuse("symlink in working tree");
    std::filesystem::remove(candidate / "machine/evil-link");
    // hardlinked file inside working tree rejected (unaliased closed working disk required)
    put(root / "hard-target.img", "h");
    std::filesystem::create_hard_link(root / "hard-target.img", candidate / "machine/hard.img");
    expect_refuse("hard-linked working disk");
    std::filesystem::remove(candidate / "machine/hard.img");
    // candidate outside the project dot-local tree rejected
    {
        const std::filesystem::path foreign = root / "elsewhere/candidate-x";
        ensure_private_directory(foreign / "machine");
        put(foreign / "receipt.json", "{}");
        bool denied = false;
        try {
            purge_android_candidate_working_disks(root, foreign, real_qemu_img);
        } catch (...) {
            denied = true;
        }

        if (!denied) {
            throw std::runtime_error("GC accepted candidate outside the owned project tree");
        }

        if (!std::filesystem::exists(foreign / "machine/receipt.json") &&
            !std::filesystem::exists(foreign / "receipt.json")) {
            throw std::runtime_error("outside candidate touched");
        }
    }

    // fixture SDK: copy the real qemu-img into the fixture root so the GC backing audit runs
    {
        const auto qemu_img = std::getenv("VOYEMU_TEST_QEMU_IMG");

        if (!qemu_img || !*qemu_img) {
            throw std::runtime_error(
                    "GC scratch requires VOYEMU_TEST_QEMU_IMG pointing at the pinned native qemu-img");
        }
    }

    // qcow2 overlay whose backing chain leaves the working tree must be refused (it would
    // release a foreign/canonical disk image through backing)
    {
        const auto qemu_img = std::getenv("VOYEMU_TEST_QEMU_IMG");

        if (qemu_img && *qemu_img) {
            const auto foreign_base = stage / "foreign-base.qcow2";
            (void)execute({qemu_img, {"create", "-f", "qcow2", foreign_base.string(), "4M"}});
            ensure_private_directory(candidate / "machine");
            (void)execute({qemu_img,
                           {"create", "-f", "qcow2", "-b", foreign_base.string(), "-F", "qcow2",
                            (candidate / "machine/overlay.qcow2").string()}});
            expect_refuse("qcow2 backing outside working tree");
            std::filesystem::remove(candidate / "machine/overlay.qcow2");
            std::filesystem::remove(foreign_base);
        }
    }
    // in-tree backing chain purges cleanly
    {
        const auto qemu_img = std::getenv("VOYEMU_TEST_QEMU_IMG");

        if (qemu_img && *qemu_img) {
            (void)execute(
                    {qemu_img,
                     {"create", "-f", "qcow2", (candidate / "machine/base.qcow2").string(), "4M"}});
            (void)execute(
                    {qemu_img,
                     {"create", "-f", "qcow2", "-b", (candidate / "machine/base.qcow2").string(),
                      "-F", "qcow2", (candidate / "machine/overlay.qcow2").string()}});
            const auto receipt_before = sha256_file(candidate / "receipt.json");
            purge_android_candidate_working_disks(root, candidate, real_qemu_img);

            if (std::filesystem::exists(candidate / "machine")) {
                throw std::runtime_error("in-tree qcow2 backing not collected");
            }

            if (sha256_file(candidate / "receipt.json") != receipt_before) {
                throw std::runtime_error("receipt damaged by qcow2 GC");
            }

            write_receipt(true, true); // restore receipt for the following recovery-path assert
        }
    }

    // positive purge: valid closed candidate with remaining disks
    const auto receipt_pin = sha256_file(candidate / "receipt.json");
    const auto exported_pin = sha256_file(candidate / "android/system.img");
    purge_android_candidate_working_disks(root, candidate, real_qemu_img);

    if (std::filesystem::exists(candidate / "machine")) {
        throw std::runtime_error("working disks not collected");
    }

    if (sha256_file(candidate / "receipt.json") != receipt_pin ||
        sha256_file(candidate / "android/system.img") != exported_pin) {
        throw std::runtime_error("receipt/exported image damaged by GC");
    }

    const auto gc = read_private_json(candidate / "gc.json", 64 * 1024);

    if (gc.at("kind") != "android-candidate-working-disk-gc") {
        throw std::runtime_error("gc evidence missing");
    }

    if (gc.at("freed_bytes").get<std::uint64_t>() == 0) {
        throw std::runtime_error("GC freed-bytes accounting missing");
    }

    // Lineage retry invariant: the pending stage is NOT yet published, so if working-tree
    // release partially removes and the RAII deletes the stage, the ORIGINAL candidate
    // exported images plus receipt metadata must fully suffice for a finalize retry.
    // Simulate the post-partial-removal state directly: machine/ gone, a pending directory
    // holding only the remainder, exported android/ images and receipt untouched.
    {
        ensure_private_directory(candidate / "machine");
        put(candidate / "machine/a.qcow2", "a");
        put(candidate / "machine/b.qcow2", "b");
        write_receipt(true, true);
        const auto receipt_before = sha256_file(candidate / "receipt.json");
        const auto export_before = sha256_file(candidate / "android/system.img");
        // simulated partial release: atomic rename happened, removal deleted only file a
        std::filesystem::rename(candidate / "machine", candidate / "machine.gc-pending-partial");
        std::filesystem::remove(candidate / "machine.gc-pending-partial/a.qcow2");
        // A partial release blocks retry until the remainder is explicitly addressed.
        expect_refuse("partial pending release");

        if (sha256_file(candidate / "receipt.json") != receipt_before ||
            sha256_file(candidate / "android/system.img") != export_before) {
            throw std::runtime_error("retry inputs damaged by stale pending remainder");
        }

        if (!std::filesystem::exists(candidate / "machine.gc-pending-partial/b.qcow2")) {
            throw std::runtime_error("pending remainder lost (recovery evidence must be retained)");
        }

        // Check fixture inputs remain available for a subsequent retry.
        const auto retry_receipt = read_private_json(candidate / "receipt.json", 1 << 20);
        const auto dump = retry_receipt.dump();

        if (dump.find("\"machine/") != std::string::npos) {
            throw std::runtime_error("receipt references released working tree");
        }

        std::filesystem::remove_all(candidate / "machine.gc-pending-partial");
        purge_android_candidate_working_disks(root, candidate, real_qemu_img);
    }
    // Upgrade lineage capsule (kind != android-candidate) must never be collected: it shares a
    // directory shape with direct candidates but its machine/ tree belongs to a base lineage.
    {
        Json capsule = {{"schema", 1},
                        {"kind", "android-owned-package-upgrade"},
                        {"profile", "gc-fixture"},
                        {"stopped", true},
                        {"cold_boot_verified", true}};
        write_private_json(candidate / "receipt.json", capsule);
        ensure_private_directory(candidate / "machine");
        put(candidate / "machine/lineage.qcow2", "lineage");
        const auto before = sha256_file(candidate / "machine/lineage.qcow2");
        bool denied = false;
        try {
            purge_android_candidate_working_disks(root, candidate, real_qemu_img);
        } catch (...) {
            denied = true;
        }

        if (!denied) {
            throw std::runtime_error("lineage capsule collected by direct-candidate GC");
        }

        if (sha256_file(candidate / "machine/lineage.qcow2") != before) {
            throw std::runtime_error("lineage capsule modified");
        }

        std::filesystem::remove_all(candidate / "machine");
        write_receipt(true, true);
    }

#ifndef _WIN32
    // An unwritable working directory makes macOS reject the release rename.
    {
        // restore a working tree first
        ensure_private_directory(candidate / "machine");
        put(candidate / "machine/system.img", "overlay-f");
        write_receipt(true, true);
        const auto receipt_before = sha256_file(candidate / "receipt.json");
        // Revoke write permission before the release attempt.
        ::chmod((candidate / "machine").c_str(), 0555);
        bool denied = false;
        try {
            purge_android_candidate_working_disks(root, candidate, real_qemu_img);
        } catch (...) {
            denied = true;
        }

        if (!denied) {
            throw std::runtime_error("GC accepted undeletable working tree");
        }

        ::chmod((candidate / "machine").c_str(), 0755);

        // Rename is refused on this platform before removal; the receipt survives.
        if (sha256_file(candidate / "receipt.json") != receipt_before) {
            throw std::runtime_error("receipt damaged by failed purge");
        }

        std::filesystem::remove_all(candidate / "machine");
        std::filesystem::remove(candidate / "gc.json");
        std::filesystem::remove(candidate / "gc-failure.json");
    }
#endif

    // idempotent second run keeps everything intact
    purge_android_candidate_working_disks(root, candidate, real_qemu_img);

    if (sha256_file(candidate / "receipt.json") != receipt_pin) {
        throw std::runtime_error("idempotent GC damaged receipts");
    }

    // canonical recovery: a stale gc.json never hides a later working tree; rebuilding disk tree
    // then purging works again
    ensure_private_directory(candidate / "machine");
    put(candidate / "machine/system.img.qcow2", "overlay2");
    purge_android_candidate_working_disks(root, candidate, real_qemu_img);

    if (std::filesystem::exists(candidate / "machine")) {
        throw std::runtime_error("repopulated working disks not collected");
    }

    std::cout
            << "Candidate working-disk GC positive path, six refusal guards and idempotence passed\n";
}

static void media_origin_checks(const std::filesystem::path &root) {
    artifact_resume_checks(root);
    const std::string profile = "free-11-chinese-20260731-7.3.1";
    const auto definition = root / "profiles" / profile / "profile.json",
               transport = root / ".local/build/tmp/artifact-transport",
               receipt = transport / "profile-artifacts.json",
               result_file = transport / "result.json", candidate = root / ".local/media-candidate",
               apk = transport / "QGMedia.apk";
    auto put = [](const std::filesystem::path &file, const std::string &text) {
        ensure_private_directory(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    };
    write_private_json(definition,
                       {{"schema", 1},
                        {"platform", "qinggan11"},
                        {"board", "8155"},
                        {"features", {{"system_upgrade", true}}},
                        {"patches",
                         {{"QGMedia", {{"steps", Json::array({"api30-usb-player.json"})}}},
                          {"Navi", {{"steps", Json::array({"api30-navi-vin.json"})}}}}}});
    put(apk, "cold source QGMedia APK");
    const auto sha = sha256_file(apk);
    const Json artifact = {
            {"file", apk.string()}, {"sha256", sha}, {"bytes", std::filesystem::file_size(apk)}};
    auto result = read_private_json(result_file);
    result["profile"] = profile;
    result["revision"] = sha256_bytes(Json{{"profile.json", sha256_file(definition)}}.dump());
    result["artifacts"]["patched:QGMedia"] = artifact;
    write_private_json(result_file, result);
    auto prepared = read_private_json(receipt);
    prepared["profile"] = profile;
    prepared["profile_sha256"] = sha256_file(definition);
    prepared["result_sha256"] = sha256_file(result_file);
    write_private_json(receipt, prepared);
    const auto prepared_pin = sha256_file(receipt);
    const Json cold = {
            {"schema", 1},
            {"kind", "android-candidate"},
            {"profile", profile},
            {"profile_sha256", sha256_file(definition)},
            {"stopped", true},
            {"prepared_VM", true},
            {"cold_boot_verified", true},
            {"old_machine_modified", false},
            {"artifact_result_sha256", sha256_file(result_file)},
            {"installed",
             Json::array(
                     {{{"artifact", "patched:QGMedia"},
                       {"package", "com.qinggan.media"},
                       {"sha256", sha},
                       {"mode", "0644"},
                       {"mutable", false},
                       {"target",
                        "/system/priv-app/qgmedia-release-signed/qgmedia-release-signed.apk"}}})}};
    write_private_json(candidate / "receipt.json", cold);
    const auto pin = sha256_file(candidate / "receipt.json");
    const auto sentinel = root / "machines/existing-userdata";
    put(sentinel, "retained installed apps and private preferences");
    const auto sentinel_id = cache_file_identity(sentinel);
    const auto sentinel_sha = sha256_file(sentinel);
    unsigned negatives = 0;
    auto rejects = [&](const std::filesystem::path &origin = {},
                       const std::string &origin_pin = std::string{}) {
        bool denied = false;
        try {
            (void)validate_profile_android_media_source(root, profile, candidate, pin, origin,
                                                        origin_pin);
        } catch (...) {
            denied = true;
        }

        if (!denied || cache_file_identity(sentinel) != sentinel_id ||
            sha256_file(sentinel) != sentinel_sha) {
            throw std::runtime_error("unbound media origin accepted or existing app state changed");
        }

        ++negatives;
    };
    rejects();
    const auto explicit_source = validate_profile_android_media_source(root, profile, candidate,
                                                                       pin, receipt, prepared_pin);

    if (explicit_source.at("sha256") != sha || explicit_source.at("file") != apk.string()) {
        throw std::runtime_error("valid explicit source media refused");
    }

    rejects(receipt, std::string(64, '0'));
    const Json origin = {{"schema", 1},
                         {"kind", "android-artifact-origin"},
                         {"profile", profile},
                         {"candidate_receipt_sha256", pin},
                         {"artifact_receipt", receipt.string()},
                         {"artifact_receipt_sha256", prepared_pin}};
    write_private_json(candidate / "artifact-origin.json", origin);

    if (validate_profile_android_media_source(root, profile, candidate, pin) != explicit_source) {
        throw std::runtime_error("valid retained artifact origin refused");
    }

    for (const auto *field : {"profile", "candidate_receipt_sha256", "artifact_receipt_sha256"}) {
        auto bad = origin;
        bad[field] = "wrong";
        write_private_json(candidate / "artifact-origin.json", bad);
        rejects();
    }

    write_private_json(candidate / "artifact-origin.json", origin);
    auto bad = cold;
    bad["artifact_result_sha256"] = std::string(64, '0');
    write_private_json(candidate / "receipt.json", bad);
    bool wrong_result = false;
    try {
        (void)validate_profile_android_media_source(root, profile, candidate,
                                                    sha256_file(candidate / "receipt.json"),
                                                    receipt, prepared_pin);
    } catch (...) {
        wrong_result = true;
    }

    if (!wrong_result) {
        throw std::runtime_error("cold media result mismatch accepted");
    }

    ++negatives;
    write_private_json(candidate / "receipt.json", cold);
    auto wrong_profile = cold;
    wrong_profile["profile"] = "other-profile";
    write_private_json(candidate / "receipt.json", wrong_profile);
    bool profile_denied = false;
    try {
        (void)validate_profile_android_media_source(root, profile, candidate,
                                                    sha256_file(candidate / "receipt.json"),
                                                    receipt, prepared_pin);
    } catch (...) {
        profile_denied = true;
    }

    if (!profile_denied) {
        throw std::runtime_error("cold media profile epoch mismatch accepted");
    }

    ++negatives;
    write_private_json(candidate / "receipt.json", cold);
    auto reject_cold = [&](const Json &value) {
        write_private_json(candidate / "receipt.json", value);
        bool denied = false;
        try {
            (void)validate_profile_android_media_source(root, profile, candidate,
                                                        sha256_file(candidate / "receipt.json"),
                                                        receipt, prepared_pin);
        } catch (...) {
            denied = true;
        }

        if (!denied) {
            throw std::runtime_error("unverified system media installation accepted");
        }

        ++negatives;
        write_private_json(candidate / "receipt.json", cold);
    };

    for (const auto *target : {"/data/app/~~original/com.qinggan.media-cold==/base.apk",
                               "/system/app/qgmedia-release-signed/qgmedia-release-signed.apk",
                               "/system/priv-app/foreign/foreign.apk"}) {
        auto changed = cold;
        changed["installed"][0]["target"] = target;
        reject_cold(changed);
    }

    for (const auto *field : {"package", "sha256", "mode"}) {
        auto changed = cold;
        changed["installed"][0][field] = "wrong";
        reject_cold(changed);
    }

    {
        auto changed = cold;
        changed["installed"].push_back(changed["installed"][0]);
        reject_cold(changed);
        changed = cold;
        changed["installed"][0]["mutable"] = true;
        reject_cold(changed);
        changed = cold;
        changed["schema"] = 2;
        reject_cold(changed);
        changed = cold;
        changed["installed"] = Json{{"row", cold["installed"][0]}};
        reject_cold(changed);
    }
    {
        const auto camera_apk = transport / "Camera.apk";
        put(camera_apk, "prepared Camera APK");
        const auto camera_sha = sha256_file(camera_apk);
        auto camera_result = result;
        camera_result["artifacts"]["patched:Camera"] = {
                {"file", camera_apk.string()},
                {"sha256", camera_sha},
                {"bytes", std::filesystem::file_size(camera_apk)}};
        write_private_json(result_file, camera_result);
        auto camera_prepared = prepared;
        camera_prepared["result_sha256"] = sha256_file(result_file);
        write_private_json(receipt, camera_prepared);
        auto camera_cold = cold;
        camera_cold["artifact_result_sha256"] = sha256_file(result_file);
        camera_cold["camera_package"] = {{"package", "com.qinggan.camera"},
                                         {"file", "android/packages/" + camera_sha + ".apk"},
                                         {"sha256", camera_sha},
                                         {"bytes", std::filesystem::file_size(camera_apk)}};
        write_private_json(candidate / "receipt.json", camera_cold);
        const auto camera_pin = sha256_file(candidate / "receipt.json"),
                   camera_prepared_pin = sha256_file(receipt);
        auto got = validate_profile_android_camera_source(root, profile, candidate, camera_pin,
                                                          receipt, camera_prepared_pin);

        if (got.at("sha256") != camera_sha) {
            throw std::runtime_error("Camera prepared source refused");
        }

        auto bad_camera = camera_cold;
        bad_camera["camera_package"]["sha256"] = std::string(64, '0');
        write_private_json(candidate / "receipt.json", bad_camera);
        bool refused = false;
        try {
            (void)validate_profile_android_camera_source(root, profile, candidate,
                                                         sha256_file(candidate / "receipt.json"),
                                                         receipt, camera_prepared_pin);
        } catch (...) {
            refused = true;
        }

        if (!refused) {
            throw std::runtime_error("foreign Camera artifact accepted");
        }

        write_private_json(result_file, result);
        write_private_json(receipt, prepared);
        write_private_json(candidate / "receipt.json", cold);
    }
    {
        const auto navi_apk = transport / "Navi.apk";
        put(navi_apk, "prepared Navi APK");
        const auto navi_sha = sha256_file(navi_apk);
        auto navi_result = result;
        navi_result["artifacts"]["patched:Navi"] = {
                {"file", navi_apk.string()},
                {"sha256", navi_sha},
                {"bytes", std::filesystem::file_size(navi_apk)}};
        write_private_json(result_file, navi_result);
        auto navi_prepared = prepared;
        navi_prepared["result_sha256"] = sha256_file(result_file);
        write_private_json(receipt, navi_prepared);
        auto navi_cold = cold;
        navi_cold["artifact_result_sha256"] = sha256_file(result_file);
        const Json camera = {{"package", "com.qinggan.camera"},
                             {"file", "android/packages/" + sha + ".apk"},
                             {"sha256", sha},
                             {"bytes", 1}};
        const Json navi = {{"package", "com.baidu.naviauto"},
                           {"file", "android/packages/" + navi_sha + ".apk"},
                           {"sha256", navi_sha},
                           {"bytes", std::filesystem::file_size(navi_apk)}};
        navi_cold["prepared_packages"] = Json::array({camera, navi});
        write_private_json(candidate / "receipt.json", navi_cold);
        const auto navi_pin = sha256_file(candidate / "receipt.json"),
                   navi_prepared_pin = sha256_file(receipt);
        auto got = validate_profile_android_navi_source(root, profile, candidate, navi_pin, receipt,
                                                        navi_prepared_pin);

        if (got.at("sha256") != navi_sha) {
            throw std::runtime_error("Navi prepared source refused");
        }

        auto bad_navi = navi_cold;
        bad_navi["prepared_packages"][1]["sha256"] = std::string(64, '0');
        bad_navi["prepared_packages"][1]["file"] = "android/packages/" + std::string(64, '0') +
                                                   ".apk";
        write_private_json(candidate / "receipt.json", bad_navi);
        bool refused = false;
        try {
            (void)validate_profile_android_navi_source(root, profile, candidate,
                                                       sha256_file(candidate / "receipt.json"),
                                                       receipt, navi_prepared_pin);
        } catch (...) {
            refused = true;
        }

        if (!refused) {
            throw std::runtime_error("foreign Navi artifact accepted");
        }

        write_private_json(result_file, result);
        write_private_json(receipt, prepared);
        write_private_json(candidate / "receipt.json", cold);
    }
    auto altered = result;
    altered["foreign_result"] = true;
    write_private_json(result_file, altered);
    rejects();
    write_private_json(result_file, result);
    const auto added = root / "src/prepare/java/com/voyemu/prepare/ForeignProducer.java";
    put(added, "changed producer inventory");

    if (validate_profile_android_media_source(root, profile, candidate, pin) != explicit_source) {
        throw std::runtime_error("unrelated producer edit invalidated media origin");
    }

    std::filesystem::remove(added);
    put(apk, "foreign signed sidecar APK");
    rejects();
    put(apk, "cold source QGMedia APK");

    if (validate_profile_android_media_source(root, profile, candidate, pin) != explicit_source ||
        sha256_file(candidate / "receipt.json") != pin ||
        cache_file_identity(sentinel) != sentinel_id || sha256_file(sentinel) != sentinel_sha) {
        throw std::runtime_error(
                "media validation changed original candidate or application state");
    }

    std::cout << "media artifact origin explicit/retained source bindings and " << negatives
              << " rejection cases passed\n";
}

static void resource_source_fixture(const std::filesystem::path &root, const Json &definition) {
    auto put = [](const std::filesystem::path &file, const std::string &bytes) {
        ensure_private_directory(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };

    put(root / "src/assets/voyemu-logo-white-black-1920-720.png", "synthetic splash");
    put(root / definition.value("wheel", Json::object())
                        .value("androidRoutes", std::string("src/app/android-wheel-routes.json")),
        "{\"routes\":[]}");

    if (definition.contains("wheel") && definition.at("wheel").contains("assets")) {
        for (const auto *file : {"wheel-left.png", "wheel-right.png", "wheel-hotspots.json"}) {
            put(root / definition.at("wheel").at("assets").get<std::string>() / file, file);
        }
    }
}

static void published_resources_check(const std::filesystem::path &seed) {
    const auto manifest = read_private_json(seed / "runtime.json");

    for (const auto &[name, record] : manifest.at("resources").items()) {
        if (sha256_file(portable_resource_path(seed, name)) !=
            record.at("sha256").get<std::string>()) {
            throw std::runtime_error("published resource payload differs");
        }
    }

    if (manifest.at("resources").size() < 2) {
        throw std::runtime_error("published machine has no self-contained splash/routes");
    }
}

static void frontend_binding_checks(const std::filesystem::path &stage) {
    bool rejected = false;
    const auto frontend_root = stage / "project", assets = frontend_root / "assets",
               bin = frontend_root / ".local/bin";

    ensure_private_directory(assets);
    ensure_private_directory(bin);
#ifdef _WIN32
    const std::string window_name = "voyemu.exe";
#else
    const std::string window_name = "voyemu";
#endif
    const auto window = bin / window_name, splash = assets / "splash.png";
    auto put = [](const std::filesystem::path &file, const std::string &bytes) {
        ensure_private_directory(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };
    put(window, "own frontend");
    put(splash, "own splash");
    Json frontend_pins = Json::array();

    for (const auto &file : {window, splash}) {
        frontend_pins.push_back({{"path", file.string()}, {"sha256", sha256_file(file)}});
    }

    for (const auto *name : {"wheel-left.png", "wheel-right.png", "wheel-hotspots.json"}) {
        put(assets / name, name);
        frontend_pins.push_back(
                {{"path", (assets / name).string()}, {"sha256", sha256_file(assets / name)}});
    }

    const Json frontend = {
            {"schema", 1},
            {"source_root", frontend_root.string()},
            {"executable", window.string()},
            {"splash", splash.string()},
            {"assets", assets.string()},
            {"state",
             (frontend_root / ".local/machines" / qinggan09_profile() / "state/placement.json")
                     .string()},
            {"discovery_root", (frontend_root / ".local/build/tmp/avd/running").string()},
            {"grpc_port", 45997},
            {"pins", frontend_pins},
            {"capture", false}};
    std::map<std::string, std::string> pins;
    const auto bound = profile_frontend_bindings(frontend_root, qinggan09_profile(), frontend,
                                                 pins);

    if (bound != frontend || pins.size() != 5) {
        throw std::runtime_error("own unified frontend binding rejected");
    }

    auto multimedia = frontend;
    multimedia["cluster_controls"] = false;
    multimedia["state"] =
            (frontend_root /
             ".local/machines/passion-11-russian-20260403-2.1.4.80/state/placement.json")
                    .string();
    multimedia["grpc_port"] = 35590;
    std::map<std::string, std::string> multimedia_pins;
    const auto media_bound = profile_frontend_bindings(
            frontend_root, "passion-11-russian-20260403-2.1.4.80", multimedia, multimedia_pins);

    if (media_bound != multimedia || multimedia_pins.size() != 5) {
        throw std::runtime_error("Passion cockpit lost pinned wheel assets");
    }

    for (const bool legacy_flag : {true, false}) {
        auto invalid = multimedia;
        invalid["cluster_controls"] = legacy_flag;
        invalid["pins"] = Json::array({frontend_pins[0], frontend_pins[1]});
        bool denied = false;
        std::map<std::string, std::string> empty;
        try {
            profile_frontend_bindings(frontend_root, "passion-11-russian-20260403-2.1.4.80",
                                      invalid, empty);
        } catch (...) {
            denied = true;
        }

        if (!denied || !empty.empty()) {
            throw std::runtime_error("frontend wheel omission accepted");
        }
    }

    const auto peer = stage / "peer/font.ttf";
    put(peer, "peer input");
    auto foreign = frontend;
    foreign["pins"].push_back({{"path", peer.string()}, {"sha256", sha256_file(peer)}});
    const auto old_pins = pins;
    rejected = false;
    try {
        profile_frontend_bindings(frontend_root, qinggan09_profile(), foreign, pins);
    } catch (...) {
        rejected = true;
    }

    if (!rejected || pins != old_pins) {
        throw std::runtime_error("peer frontend input accepted or partial host pins leaked");
    }

    auto missing_exe = frontend;
    missing_exe["pins"].erase(0);
    rejected = false;
    try {
        profile_frontend_bindings(frontend_root, qinggan09_profile(), missing_exe, pins);
    } catch (...) {
        rejected = true;
    }

    if (!rejected) {
        throw std::runtime_error("unpinned frontend executable accepted");
    }

    const auto built_root = stage / "built-project";
    put(built_root / window_name, "root unified program");
    // An imported OEM boot animation for screen 1 only; screen 2 stays black.
    const Json definition = {
            {"wheel", {{"assets", "profiles/common/wheels/fixture"}}},
            {"original",
             {{"BootAnimation",
               {{"file", "bootanimation.zip"}, {"sha256", sha256_bytes("synthetic animation")}}}}}};
    const auto originals = built_root / ".local/profiles" / qinggan09_profile() / "originals";
    const auto profile_file = built_root / "profiles" / qinggan09_profile() / "profile.json";
    write_private_json(profile_file, definition);
    resource_source_fixture(built_root, definition);
    put(originals / "bootanimation.zip", "tampered animation");
    rejected = false;
    try {
        export_portable_resources(built_root, definition, qinggan09_profile(),
                                  stage / "tampered-seed");
    } catch (const std::exception &error) {
        rejected = std::string_view(error.what()).starts_with("PORTABLE_RESOURCES: ");
    }

    if (!rejected) {
        throw std::runtime_error("unpinned imported boot animation was embedded");
    }

    put(originals / "bootanimation.zip", "synthetic animation");
    const auto prepared = built_root / "machines/frontend-snapshot";
    ensure_private_directory(prepared);
    std::filesystem::copy_file(profile_file, prepared / "profile.json");
    auto manifest = Json{{"schema", 1},
                         {"kind", "portable-guest-runtime"},
                         {"profile", qinggan09_profile()},
                         {"profile_sha256", sha256_file(profile_file)},
                         {"android", Json::object()}};
    write_private_json(prepared / "runtime.json", manifest);
    std::map<std::string, std::string> missing_resources;
    rejected = false;
    try {
        profile_built_frontend_bindings(built_root, qinggan09_profile(),
                                        machine_identity(qinggan09_profile(), "frontend-snapshot"),
                                        missing_resources);
    } catch (...) {
        rejected = true;
    }

    if (!rejected) {
        throw std::runtime_error("prepared frontend silently used shared source assets");
    }

    manifest["resources"] = export_portable_resources(built_root, definition, qinggan09_profile(),
                                                      prepared);
    write_private_json(prepared / "runtime.json", manifest);
    std::map<std::string, std::string> built_pins;
    const auto built = profile_built_frontend_bindings(
            built_root, qinggan09_profile(),
            machine_identity(qinggan09_profile(), "frontend-snapshot"), built_pins);
    std::filesystem::rename(built_root / "profiles", built_root / "moved-profiles");
    std::filesystem::rename(built_root / "src", built_root / "moved-src");
    std::map<std::string, std::string> prepared_pins;
    const auto prepared_frontend = profile_built_frontend_bindings(
            built_root, qinggan09_profile(),
            machine_identity(qinggan09_profile(), "frontend-snapshot"), prepared_pins);

    if (prepared_frontend != built || prepared_pins != built_pins) {
        throw std::runtime_error("prepared frontend depends on source profiles/assets");
    }

    published_resources_check(prepared);
    const auto bound_path = [&](const char *name) {
        return std::filesystem::path(built.at(name).get<std::string>());
    };

    if (bound_path("executable") != built_root / window_name ||
        bound_path("assets") != prepared / "ui/wheel" ||
        bound_path("fonts") != prepared / "ui/fonts" ||
        bound_path("splash") != prepared / "ui/splash.png" || built.at("startup").size() != 2 ||
        !built.at("startup")[1].is_null() ||
        std::filesystem::path(built.at("startup")[0].get<std::string>()) !=
                prepared / "ui/startup/display0.zip" ||
        built.at("pins").size() != 6 || built_pins.size() != 6) {
        throw std::runtime_error("machine-local default frontend binding differs");
    }

#ifdef _WIN32
    put(built_root / window_name, "new root unified program");
    std::map<std::string, std::string> stale_pins;
    rejected = false;
    try {
        profile_frontend_bindings(built_root, qinggan09_profile(), built, stale_pins);
    } catch (...) {
        rejected = true;
    }

    if (!rejected || !stale_pins.empty()) {
        throw std::runtime_error("unrefreshed root program pin was silently accepted");
    }
#endif
}

// A synthetic QNX ImageFS holding one regular file, with a valid checksum.
static std::string imagefs_fixture(const std::string &name, const std::string &payload) {
    const std::size_t entry = (32 + name.size() + 1 + 3) / 4 * 4, end = 92 + entry + 4,
                      extent = (end + payload.size() + 4 + 3) / 4 * 4;
    std::string image(extent, '\0');
    auto put = [&](std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) {
            image[at + i] = static_cast<char>(value >> (i * 8));
        }
    };
    auto word = [&](std::size_t at) {
        std::uint32_t value = 0;

        for (unsigned i = 0; i < 4; ++i) {
            value |= std::uint32_t(static_cast<unsigned char>(image[at + i])) << (i * 8);
        }

        return value;
    };

    image.replace(0, 8, std::string("imagefs\x04", 8));
    put(8, std::uint32_t(extent));
    put(12, std::uint32_t(end));
    put(16, 92);
    put(92, std::uint32_t(entry));
    put(100, 0100000);
    put(116, std::uint32_t(end));
    put(120, std::uint32_t(payload.size()));
    image.replace(124, name.size(), name);
    image.replace(end, payload.size(), payload);
    std::uint32_t sum = 0;

    for (std::size_t at = 0; at + 4 < extent; at += 4) {
        sum += word(at);
    }

    put(extent - 4, 0 - sum);

    return image;
}

// The startup videos come from a nested ImageFS of an imported QNX image, are checked against
// their profile pins and are bound to the window as frame archives with their own pins; each
// archive lasts its video, as decoded frames or, without a host decoder, as one held black frame.
static void startup_video_resource_checks(const std::filesystem::path &stage) {
    const auto root = stage / "startup-video-project";
    const auto clip = stage / "startup-video.mp4";

    startup_video_fixture::mp4(clip, 48, 32, 10,
                               std::vector<std::array<std::uint8_t, 3>>(5, {16, 128, 128}));
    std::ifstream source(clip, std::ios::binary);
    const std::string video(std::istreambuf_iterator<char>(source), {}),
            path = "proc/boot/startup_video.mp4";
    const auto nested = imagefs_fixture(path, video);
    const auto container = std::string(4096, '\0') + nested + std::string(512, '\0');
    const auto put = [](const std::filesystem::path &file, const std::string &bytes) {
        ensure_private_directory(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };
    const Json pin = {{"file", "startup_video.mp4"},
                      {"bytes", video.size()},
                      {"sha256", sha256_bytes(video)},
                      {"seconds", 3},
                      {"acquisition",
                       {{"image", "ifs2_la.img"}, {"nested", "ifs_video.img"}, {"path", path}}}};
    Json definition = {{"startupVideos", {{"display0", pin}, {"cluster", pin}}}};

    put(root / ".local/profiles" / qinggan09_profile() / "qnx/ifs2_la.img", container);
    write_private_json(
            root / "profiles" / qinggan09_profile() / "profile-qnx.json",
            {{"originals",
              {{"ifs2_la.img",
                {{"file", "ifs2_la.img"},
                 {"bytes", container.size()},
                 {"sha256", sha256_bytes(container)}}}}},
             {"nested_imagefs",
              {{"ifs2_la_img",
                Json::array({{{"file", "ifs_display.img"}, {"offset", 0}, {"bytes", 4096}},
                             {{"file", "ifs_video.img"},
                              {"offset", 4096},
                              {"bytes", nested.size()}}})}}}});
    resource_source_fixture(root, definition);
    const auto seed = root / "machines/startup-video";
    ensure_private_directory(seed);
    auto manifest = Json{{"schema", 1}, {"resources", Json::object()}};
    write_private_json(seed / "runtime.json", manifest);

    if (!portable_startup_video_paths(seed).is_null()) {
        throw std::runtime_error("a machine without the video bound one");
    }

    manifest["resources"] = export_portable_resources(root, definition, qinggan09_profile(), seed);
    write_private_json(seed / "runtime.json", manifest);
    const auto cluster = (seed / "ui/startup/cluster.zip").string(),
               display = (seed / "ui/startup/display0-video.zip").string();

    if (portable_startup_video_frontend(seed) !=
                Json{{"startup_video", Json::array({display, nullptr})},
                     {"startup_cluster", cluster}} ||
        manifest.at("resources").at("ui/startup/cluster.zip").at("sha256") !=
                sha256_file(cluster) ||
        std::filesystem::exists(seed / "ui/startup/cluster.zip.part")) {
        throw std::runtime_error("startup videos were not embedded with their pins");
    }

    std::ifstream frames(cluster, std::ios::binary);
    const std::string archive(std::istreambuf_iterator<char>(frames), {});

    if (archive.find("48 32 10\np 1 4 part0\n") == std::string::npos &&
        (archive.find("48 32 10\np 1 0 part0\n") == std::string::npos ||
         archive.find("part0/00004.png") == std::string::npos)) {
        throw std::runtime_error("a startup video archive must last its video");
    }

    published_resources_check(seed);
    definition["startupVideos"]["cluster"]["sha256"] = sha256_bytes("another video");
    bool rejected = false;
    try {
        export_portable_resources(root, definition, qinggan09_profile(), stage / "other-seed");
    } catch (const std::exception &error) {
        rejected = std::string_view(error.what()).starts_with("PORTABLE_RESOURCES: ");
    }

    if (!rejected) {
        throw std::runtime_error("an unpinned startup video was embedded");
    }
}

static void observer_cache_checks(const std::filesystem::path &stage) {
    const auto root = stage / "observer-project",
               source = root / "src/guest/android/api/Observer.java",
               test = root / "src/guest/android/api/ObserverTest.java";
    auto put = [](const std::filesystem::path &path, std::string_view bytes) {
        ensure_private_directory(path.parent_path());
        std::ofstream(path, std::ios::binary) << bytes;
    };

    put(source, "observer source fixture");
    put(test, "observer test fixture");

    if (!cached_profile_ui_observer(root).empty()) {
        throw std::runtime_error("absent observer cache accepted");
    }

    Json inputs = {{"schema", 1},
                   {"min_api", 28},
                   {"java", std::string(64, '1')},
                   {"javac", std::string(64, '2')},
                   {"platform", std::string(64, '3')},
                   {"dexer", std::string(64, '4')},
                   {"sources", {{"Observer.java", sha256_file(source)}}},
                   {"test_sources", {{"ObserverTest.java", sha256_file(test)}}},
                   {"host_test_policy", "assertions-enabled-v1"}};
    const auto folder = root / ".local/toolchains/ui-observer" / sha256_bytes(inputs.dump()),
               dex = folder / "classes.dex", receipt = folder / "observer.json";
    put(dex, "synthetic observer DEX fixture");
    const Json record = {{"schema", 1},
                         {"kind", "owned-api28-ui-observer"},
                         {"inputs", inputs},
                         {"sha256", sha256_file(dex)}};
    write_private_json(receipt, record);
    const auto bound = prepare_profile_ui_observer(root);

    if (bound.at("dex") != dex.string() || bound.at("sha256") != sha256_file(dex)) {
        throw std::runtime_error("ready source-matched observer cache reached compiler discovery");
    }

    auto rejects_cache = [&] {
        bool denied = false;
        try {
            cached_profile_ui_observer(root);
        } catch (...) {
            denied = true;
        }

        if (!denied) {
            throw std::runtime_error("invalid observer cache accepted");
        }
    };
    put(dex, "changed DEX");
    rejects_cache();
    put(dex, "synthetic observer DEX fixture");
    auto malformed = record;
    malformed["inputs"]["javac"] = "missing";
    write_private_json(receipt, malformed);
    rejects_cache();
    write_private_json(receipt, record);
    put(test, "changed host test");

    if (!cached_profile_ui_observer(root).empty()) {
        throw std::runtime_error("stale observer tests accepted");
    }

    put(test, "observer test fixture");
    const auto alias = stage / "dex-alias";
    std::filesystem::create_hard_link(dex, alias);
    rejects_cache();
    std::filesystem::remove(alias);

    if (prepare_profile_ui_observer(root) != bound) {
        throw std::runtime_error("restored observer source binding differs");
    }
}

static Json cache_fixture(const std::filesystem::path &parent) {
    const auto file = parent / "android/cache.img";

    ensure_private_directory(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.seekp(1080);
    out.put(0x53);
    out.put(static_cast<char>(0xef));
    out.seekp(69206015);
    out.put(0);
    out.close();

    if (!out) {
        throw std::runtime_error("cache fixture write");
    }

    return {{"file", "android/cache.img"}, {"bytes", 69206016}, {"sha256", sha256_file(file)}};
}

static void source_graphics_checks(const std::filesystem::path &stage) {
    const auto root = stage / "source-graphics";

    ensure_private_directory(root);
    const auto previous = root / "old-program", current = root / "current-program",
               engine = root / "engine";

    for (const auto &p : {previous, current, engine}) {
        std::ofstream out(p);
        out << p.filename().string();
    }

    const Json original = {
            {"schema", 1},
            {"graphics", previous.string()},
            {"qnx_qemu", engine.string()},
            {"angle", "unchanged-angle"},
            {"pins", Json::array({{{"path", previous.string()}, {"sha256", sha256_file(previous)}},
                                  {{"path", engine.string()}, {"sha256", sha256_file(engine)}}})}};

    if (profile_source_graphics_bindings(root, original, {}) != original) {
        throw std::runtime_error("legacy graphics binding changed");
    }

    const auto selected = profile_source_graphics_bindings(root, original, current);

    if (selected.at("graphics") != current.string() ||
        selected.at("pins").at(0).at("path") != current.string() ||
        selected.at("pins").at(0).at("sha256") != sha256_file(current) ||
        selected.at("pins").at(1) != original.at("pins").at(1) ||
        selected.at("qnx_qemu") != original.at("qnx_qemu") ||
        selected.at("angle") != original.at("angle")) {
        throw std::runtime_error("source acceptance did not select actual unified graphics");
    }

    auto deny = [&](const Json &value) {
        bool denied = false;
        try {
            profile_source_graphics_bindings(root, value, current);
        } catch (...) {
            denied = true;
        }

        if (!denied) {
            throw std::runtime_error("unverified graphics binding accepted");
        }
    };
    std::ofstream(previous, std::ios::trunc) << "new installed self program";
    const auto installed = profile_source_graphics_bindings(root, original, previous);

    if (installed.at("graphics") != previous.string() ||
        installed.at("pins").at(0).at("sha256") != sha256_file(previous) ||
        installed.at("pins").at(1) != original.at("pins").at(1) ||
        installed.at("qnx_qemu") != original.at("qnx_qemu") ||
        installed.at("angle") != original.at("angle")) {
        throw std::runtime_error("installed current executor retained stale self pin");
    }

    std::ofstream(previous, std::ios::trunc) << previous.filename().string();
    auto bad = original;
    bad["pins"][0]["sha256"] = std::string(64, '0');
    deny(bad);
    bad = original;
    bad["pins"].push_back(original.at("pins").at(0));
    deny(bad);
    bad = original;
    bad["pins"].push_back({{"path", current.string()}, {"sha256", sha256_file(current)}});
    deny(bad);
}

// The clean base pins that the registered profile of the API28 or API30 image declares.
static Json clean_base(unsigned api) {
    return embedded_profile(api == 30 ? "free-11-chinese-20260731-7.3.1" : qinggan09_profile())
            .at("definition")
            .at("android")
            .at("clean_base");
}

static void portable_boot_source_fixture(const std::filesystem::path &root,
                                         const Json &definition) {
    const auto base = root / ".local/profiles/common/android-base" /
                      definition.at("android").at("sha256").get<std::string>();

    ensure_private_directory(base);

    for (const auto &[name, pin] : portable_clean_boot_pins(definition.at("android"))) {
        (void)pin;
        std::ofstream(base / name) << "portable boot fixture " << name;
    }
}

static void published_boot_check(const std::filesystem::path &machine) {
    const auto manifest = read_private_json(machine / "runtime.json");
    const auto &files = manifest.at("android").at("boot_files");

    if (files.size() !=
        portable_clean_boot_pins(read_private_json(machine / "profile.json", 1 << 20).at("android"))
                .size()) {
        throw std::runtime_error("portable boot inventory incomplete");
    }

    for (const auto &[name, record] : files.items()) {
        const auto file = machine / record.at("file").get<std::string>();

        if (record.at("file") != std::string("android/") + name ||
            record.at("sha256") != sha256_file(file) ||
            record.at("bytes") != std::filesystem::file_size(file)) {
            throw std::runtime_error("portable boot export differs");
        }
    }
}

static void api30_publication_checks(const std::filesystem::path &stage, bool older = false) {
    if (!older) {
        source_graphics_checks(stage);
    }

    const std::string id = older ? "free-11-chinese-20250710-6.11.1"
                                 : "free-11-chinese-20260731-7.3.1",
                      archive = "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304";
    const auto root = stage / (older ? "api30-publication-611" : "api30-publication"),
               definition = root / "profiles" / id / "profile.json",
               candidate = root / ".local/candidate";
    ensure_private_directory(candidate);
    Json profile = {
            {"schema", 1},
            {"id", id},
            {"platform", "qinggan11"},
            {"board", "8155"},
            {"features", older ? Json{{"measured_disk_reserve", true}}
                               : Json{{"system_upgrade", true}, {"ui_observer_required", true}}},
            {"status", "incomplete"},
            {"android",
             {{"package", "system-images;android-30;default;arm64-v8a"},
              {"revision", "1"},
              {"url",
               "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"},
              {"sha256", archive},
              {"clean_base", clean_base(30)}}}};

    // The exact 6.11.1 definition keeps the measured 8 GiB reserve instead of the 20 GiB one of an
    // unregistered profile, so the fixture does not depend on the free disk of the host.
    if (older) {
        profile["android"]["offline_selinux_labels"] = "qinggan11-can-early-v1";
        profile["acquisition"]["images"]["system"]["sha256"] =
                "ee0c8241b7293b9cd9f936f622c556ac11ab3f92a0f60733e92dca1c1610f5c4";
    }

    if (android_candidate_reserve_bytes(profile, id) != 8ULL << 30) {
        throw std::runtime_error("API30 publication fixture lost the measured reserve");
    }

    write_private_json(definition, profile);
    write_private_json(
            definition.parent_path() / "profile-qnx.json",
            {{"schema", 1},
             {"hardware_adapter", older ? "sa8155-qinggan11-611" : "sa8155-qinggan11-731"}});
    resource_source_fixture(root, read_private_json(definition));
    Json images = Json::object();

    for (const auto *name :
         {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img"}) {
        const auto file = candidate / "android" / name;
        ensure_private_directory(file.parent_path());
        std::ofstream(file) << "API30 source publication fixture " << name;
        images[name] = {{"file", std::string("android/") + name},
                        {"bytes", std::filesystem::file_size(file)},
                        {"sha256", sha256_file(file)}};
    }

    images["cache.img"] = cache_fixture(candidate);
    const Json good = {{"schema", 1},
                       {"kind", "android-candidate"},
                       {"profile", id},
                       {"profile_file", "profile.json"},
                       {"profile_sha256", sha256_file(definition)},
                       {"clean_base_archive_sha256", archive},
                       {"stopped", true},
                       {"cold_boot_verified", true},
                       {"prepared_VM", true},
                       {"android", {{"abi", "arm64-v8a"}, {"images", images}}}};
    std::filesystem::copy_file(definition, candidate / "profile.json");
    const auto receipt = candidate / "receipt.json";
    auto reject = [&](Json bad, const std::string &name) {
        write_private_json(receipt, bad);
        bool denied = false;
        try {
            finalize_android_profile_build(root, id, machine_identity(id, name), candidate,
                                           sha256_file(receipt));
        } catch (...) {
            denied = true;
        }

        if (!denied || std::filesystem::exists(root / "machines" / name)) {
            throw std::runtime_error("invalid API30 partial candidate published");
        }

        if (std::filesystem::exists(root / "machines")) {
            for (const auto &entry : std::filesystem::directory_iterator(root / "machines")) {
                if (entry.path().filename().string().starts_with(".pending-")) {
                    throw std::runtime_error("failed sparse publication retained partial stage");
                }
            }
        }
    };
    auto bad = good;
    bad["clean_base_archive_sha256"] = std::string(64, '0');
    reject(bad, "wrong-base");
    bad = good;
    bad["android"]["abi"] = "x86_64";
    reject(bad, "wrong-abi");
    bad = good;
    bad["stopped"] = false;
    reject(bad, "not-stopped");
    bad = good;
    bad["cold_boot_verified"] = false;
    reject(bad, "not-cold");
    bad = good;
    bad["profile_sha256"] = std::string(64, '0');
    reject(bad, "wrong-profile");
    reject(good, "missing-source-observer");
    const auto observer_source = root / "src/guest/android/api/Observer.java",
               observer_test = root / "src/guest/android/api/ObserverTest.java";
    ensure_private_directory(observer_source.parent_path());
    std::ofstream(observer_source) << "source observer fixture";
    std::ofstream(observer_test) << "test observer fixture";
    const Json observer_inputs = {
            {"schema", 1},
            {"min_api", 28},
            {"java", std::string(64, '1')},
            {"javac", std::string(64, '2')},
            {"platform", std::string(64, '3')},
            {"dexer", std::string(64, '4')},
            {"sources", {{"Observer.java", sha256_file(observer_source)}}},
            {"test_sources", {{"ObserverTest.java", sha256_file(observer_test)}}},
            {"host_test_policy", "assertions-enabled-v1"}};
    const auto observer_cache = root / ".local/toolchains/ui-observer" /
                                sha256_bytes(observer_inputs.dump());
    ensure_private_directory(observer_cache);
    std::ofstream(observer_cache / "classes.dex") << "compiled observer publication fixture";
    write_private_json(observer_cache / "observer.json",
                       {{"schema", 1},
                        {"kind", "owned-api28-ui-observer"},
                        {"inputs", observer_inputs},
                        {"sha256", sha256_file(observer_cache / "classes.dex")}});
    portable_boot_source_fixture(root, read_private_json(definition));
    // An opt-in kernel with a mismatched digest must never be published.
    const auto original_profile = read_private_json(definition);
    auto invalid_kernel = original_profile;
    invalid_kernel["android"]["development_kernel"] = {
            {"url",
             "https://github.com/voyemu/voyemu-deps/releases/download/kernel-fixture-00000000/Image"},
            {"sha256", std::string(64, '0')},
            {"abi", "arm64-v8a"},
            {"provenance",
             {{"source_revision", "fixture"},
              {"source_content_sha256", std::string(64, '1')},
              {"embedded_config_sha256", std::string(64, '2')},
              {"normalized_config_sha256", std::string(64, '3')},
              {"toolchain", "fixture"},
              {"patches", Json::array({std::string(64, '4')})},
              {"configuration_deviations", Json::array()}}}};
    write_private_json(definition, invalid_kernel);
    auto invalid_receipt = good;
    invalid_receipt["profile_sha256"] = sha256_file(definition);
    std::filesystem::copy_file(definition, candidate / "profile.json",
                               std::filesystem::copy_options::overwrite_existing);
    write_private_json(receipt, invalid_receipt);
    bool kernel_denied = false;
    try {
        finalize_android_profile_build(root, id, machine_identity(id, "bad-development-kernel"),
                                       candidate, sha256_file(receipt));
    } catch (...) {
        kernel_denied = true;
    }

    if (!kernel_denied) {
        throw std::runtime_error("invalid development kernel published");
    }

    const auto kernel_bytes = std::string("development kernel fixture"),
               kernel_sha = sha256_bytes(kernel_bytes);
    const auto kernel_file = root / ".local/toolchains/downloads/development-kernel" /
                             (kernel_sha + ".blob");
    ensure_private_directory(kernel_file.parent_path());
    std::ofstream(kernel_file) << kernel_bytes;
    auto development = invalid_kernel;
    development["android"]["development_kernel"]["url"] =
            "https://github.com/voyemu/voyemu-deps/releases/download/kernel-fixture-" +
            kernel_sha.substr(0, 8) + "/Image";
    development["android"]["development_kernel"]["sha256"] = kernel_sha;
    write_private_json(definition, development);
    auto development_receipt = good;
    development_receipt["profile_sha256"] = sha256_file(definition);
    std::filesystem::copy_file(definition, candidate / "profile.json",
                               std::filesystem::copy_options::overwrite_existing);
    write_private_json(receipt, development_receipt);
    bool stale_kernel_denied = false;
    try {
        finalize_android_profile_build(root, id, machine_identity(id, "stale-kernel"), candidate,
                                       sha256_file(receipt));
    } catch (...) {
        stale_kernel_denied = true;
    }

    if (!stale_kernel_denied) {
        throw std::runtime_error("stock candidate accepted for development kernel");
    }

    development_receipt["development_kernel"] = development.at("android").at("development_kernel");
    auto wrong_kernel_receipt = development_receipt;
    wrong_kernel_receipt["development_kernel"]["sha256"] = std::string(64, 'f');
    write_private_json(receipt, wrong_kernel_receipt);
    bool wrong_kernel_denied = false;
    try {
        finalize_android_profile_build(root, id, machine_identity(id, "wrong-cold-kernel"),
                                       candidate, sha256_file(receipt));
    } catch (...) {
        wrong_kernel_denied = true;
    }

    if (!wrong_kernel_denied) {
        throw std::runtime_error("different cold kernel accepted");
    }

    write_private_json(receipt, development_receipt);
    finalize_android_profile_build(root, id, machine_identity(id, "development-kernel"), candidate,
                                   sha256_file(receipt));
    const auto prepared = root / "machines/development-kernel";
    const auto prepared_manifest = read_private_json(prepared / "runtime.json");

    if (prepared_manifest.at("android").at("development_kernel") !=
                development.at("android").at("development_kernel") ||
        sha256_file(prepared / "android/kernel-ranchu") != kernel_sha) {
        throw std::runtime_error("development kernel not embedded");
    }

    std::filesystem::rename(kernel_file, kernel_file.string() + ".hidden");
    std::filesystem::rename(definition, definition.string() + ".hidden");

    if (inspect_portable_runtime(root, id, machine_identity(id, "development-kernel"))
                .at("android")
                .at("development_kernel") != development.at("android").at("development_kernel")) {
        throw std::runtime_error("prepared kernel requires authoring input");
    }

    std::filesystem::rename(definition.string() + ".hidden", definition);
    std::filesystem::rename(kernel_file.string() + ".hidden", kernel_file);
    write_private_json(definition, original_profile);
    std::filesystem::copy_file(definition, candidate / "profile.json",
                               std::filesystem::copy_options::overwrite_existing);
    write_private_json(receipt, good);
    const auto built = finalize_android_profile_build(root, id, machine_identity(id, {}), candidate,
                                                      sha256_file(receipt));
    const auto manifest = read_private_json(root / "machines" / id / "runtime.json");

    if (built.at("android_ready") != true || built.at("profile_ready") != false ||
        built.at("scope") != "android" || built.at("cluster_available") != false ||
        manifest.at("android").at("api") != 30 ||
        manifest.at("android").at("base_archive_sha256") != archive || manifest.contains("qnx") ||
        read_private_json(definition).at("status") != "incomplete") {
        throw std::runtime_error(
                "API30 publication changed source readiness or Android/base identity");
    }

    published_boot_check(root / "machines" / id);
    std::filesystem::rename(root / ".local/profiles/common/android-base",
                            root / ".local/profiles/common/hidden-android-base");

    if (inspect_portable_runtime(root, id, machine_identity(id, {})).at("android").at("api") !=
        30) {
        throw std::runtime_error("published machine requires shared Android base");
    }

    std::filesystem::rename(root / ".local/profiles/common/hidden-android-base",
                            root / ".local/profiles/common/android-base");
    const auto exported_observer = root / "machines" / id / "observer/classes.dex";

    if (manifest.at("ui_observer") != Json{{"file", "observer/classes.dex"},
                                           {"bytes", std::filesystem::file_size(exported_observer)},
                                           {"sha256", sha256_file(exported_observer)}} ||
        sha256_file(exported_observer) != sha256_file(observer_cache / "classes.dex") ||
        std::filesystem::hard_link_count(exported_observer) != 1) {
        throw std::runtime_error("portable observer DEX export/pin differs");
    }

    if (inspect_portable_runtime(root, id, machine_identity(id, {})).at("android").at("api") !=
        30) {
        throw std::runtime_error("API30 partial publication cannot be inspected");
    }

    if (older) {
        const auto before = read_private_json_bytes(root / "machines" / id / "runtime.json");
        bool early = false;
        try {
            build_profile(root, id, machine_identity(id, {}), true);
        } catch (const std::exception &e) {
            early = std::string_view(e.what()) ==
                    "PROFILE_BUILD: machine " + progress::path_text(root / "machines" / id) +
                            " already exists.\nStart it: voyemu start " +
                            progress::path_text(root / "machines" / id) +
                            ", or delete its directory and run build again.";
        }

        if (!early || read_private_json_bytes(root / "machines" / id / "runtime.json") != before) {
            throw std::runtime_error(
                    "existing Free A11 6.11.1 was not rejected before dependencies/VM");
        }
    }

    const auto qnx = candidate / "accepted-qnx";
    Json qnx_images = Json::object();

    for (const auto &name :
         qnx_runtime_layout(older ? "sa8155-qinggan11-611" : "sa8155-qinggan11-731").images) {
        const auto path = qnx / "qnx" / name;
        ensure_private_directory(path.parent_path());
        std::ofstream(path) << "synthetic accepted QNX fixture " << name;
        qnx_images[name] = {{"file", std::string("qnx/") + name},
                            {"bytes", std::filesystem::file_size(path)},
                            {"sha256", sha256_file(path)}};
    }

    const auto proof = qnx / "proof.json";
    write_private_json(proof, {{"fixture", true}});
    const Json reference = {{"file", proof.string()}, {"sha256", sha256_file(proof)}};
    const auto qnx_receipt = qnx / "acceptance.json";
    Json qnx_acceptance = {{"schema", 1},
                           {"kind", "accepted-qnx-portable-seed"},
                           {"profile", id},
                           {"profile_sha256", sha256_file(definition)},
                           {"source_built", true},
                           {"cold_boot_verified", true},
                           {"stopped", true},
                           {"directory", qnx.string()},
                           {"images", qnx_images},
                           {"source_receipt", reference},
                           {"cold_boot_evidence", reference}};
    qnx_runtime_metadata(qnx_acceptance, qnx_runtime_layout(older ? "sa8155-qinggan11-611"
                                                                  : "sa8155-qinggan11-731"));
    write_private_json(qnx_receipt, qnx_acceptance);
    auto rejects_qnx = [&](Json value, const std::string &name) {
        write_private_json(qnx_receipt, value);
        bool denied = false;
        try {
            finalize_profile_build(root, id, machine_identity(id, name), candidate, qnx_receipt,
                                   sha256_file(qnx_receipt));
        } catch (...) {
            denied = true;
        }

        if (!denied || std::filesystem::exists(root / "machines" / name)) {
            throw std::runtime_error("invalid Free QNX projection/roles published");
        }
    };

    if (older) {
        auto foreign = qnx_acceptance;
        foreign["hardware_adapter"] = "sa8155-qinggan11-731";
        foreign["mcu_projection"] = qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_731);
        rejects_qnx(foreign, "wrong-611-version");
    }

    auto wrong_qnx = qnx_acceptance;
    wrong_qnx["mcu_projection"]["projected_fields"] = 579;
    rejects_qnx(wrong_qnx, "bad-free-scope");
    wrong_qnx = qnx_acceptance;
    wrong_qnx["images"].erase("gpt-primary.bin");
    rejects_qnx(wrong_qnx, "missing-free-gpt");
    write_private_json(qnx_receipt, qnx_acceptance);
    auto with_wheel = read_private_json(definition);
    with_wheel["wheel"] = {{"assets", "profiles/common/wheels/free-2024"}};
    write_private_json(definition, with_wheel);
    auto accepted_android = good;
    accepted_android["profile_sha256"] = sha256_file(definition);
    std::filesystem::copy_file(definition, candidate / "profile.json",
                               std::filesystem::copy_options::overwrite_existing);
    write_private_json(receipt, accepted_android);
    qnx_acceptance["profile_sha256"] = sha256_file(definition);
    write_private_json(qnx_receipt, qnx_acceptance);
    resource_source_fixture(root, read_private_json(definition));
    const auto combined = finalize_profile_build(root, id,
                                                 machine_identity(id, "api30-combined-fixture"),
                                                 candidate, qnx_receipt, sha256_file(qnx_receipt));
    const auto full = read_private_json(root / "machines/api30-combined-fixture/runtime.json");

    if (full.at("ui_observer") != manifest.at("ui_observer") ||
        sha256_file(root / "machines/api30-combined-fixture/observer/classes.dex") !=
                sha256_file(exported_observer)) {
        throw std::runtime_error("full portable observer DEX export differs");
    }

    if (full.contains("ui") ||
        std::filesystem::exists(root / "machines/api30-combined-fixture/ui/controls")) {
        throw std::runtime_error("full publication carries OEM control artwork");
    }

    if (full.at("qnx").at("images").size() != 10 ||
        full.at("qnx").at("mcu_projection") !=
                (older ? qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_611)
                       : qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_731)) ||
        combined.at("profile_ready") != true || full.at("android").at("api") != 30 ||
        full.at("android").at("base_archive_sha256") != archive ||
        read_private_json(definition).at("status") != "incomplete") {
        throw std::runtime_error(
                "full API30 finalizer changed API/base or source status despite accepted receipts");
    }

    std::filesystem::rename(root / "profiles", root / "moved-profiles");
    std::filesystem::rename(root / "src", root / "moved-src");

    for (const auto &machine : {id, std::string("api30-combined-fixture")}) {
        const auto seed = root / "machines" / machine;
        published_resources_check(seed);
        published_boot_check(seed);
        const auto inspected = inspect_portable_runtime(root, id, machine_identity(id, machine));

        for (const auto &[name, record] : inspected.at("android").at("images").items()) {
            if (record.at("sha256") != images.at(name).at("sha256")) {
                throw std::runtime_error("API30 resource export changed guest images");
            }
        }
    }
}

static void runtime_sdk_activation_checks(const std::filesystem::path &stage) {
    const auto root = stage / "runtime-activation-project", sdk = root / ".local/machines-sdk";

    ensure_private_directory(sdk);
    const auto definition = root / "profiles" / qinggan09_profile() / "profile.json";
    const std::string archive = "86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9";
    write_private_json(
            definition,
            {{"android",
              {{"package", "system-images;android-28;default;arm64-v8a"},
               {"revision", "1"},
               {"url",
                "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip"},
               {"sha256", archive},
               {"clean_base", clean_base(28)}}}});
    const auto seed = root / "machines" / qinggan09_profile();
    Json images = Json::object();

    for (const auto *name :
         {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img"}) {
        const auto file = seed / "android" / name;
        ensure_private_directory(file.parent_path());
        std::ofstream(file) << "synthetic guard fixture";
        images[name] = {{"file", std::string("android/") + name},
                        {"bytes", std::filesystem::file_size(file)},
                        {"sha256", sha256_file(file)}};
    }

    images["cache.img"] = cache_fixture(seed);
    write_private_json(seed / "runtime.json", {{"schema", 1},
                                               {"kind", "portable-guest-runtime"},
                                               {"profile", qinggan09_profile()},
                                               {"profile_sha256", sha256_file(definition)},
                                               {"android",
                                                {{"base_archive_sha256", archive},
                                                 {"api", 28},
                                                 {"abi", "arm64-v8a"},
                                                 {"images", images}}}});
    std::filesystem::copy_file(definition, seed / "profile.json");
    write_private_json(qnx_host_bindings_path(root), {{"schema", 1},
                                                      {"runtime_sdk",
                                                       {{"schema", 1},
                                                        {"host", "foreign"},
                                                        {"sdk_root", sdk.string()},
                                                        {"components", Json::array()}}}});
    bool denied = false;
    try {
        activate_built_profile(root, qinggan09_profile(), machine_identity(qinggan09_profile(), {}),
                               false);
    } catch (const std::exception &error) {
#ifdef _WIN32
        denied = std::string_view(error.what()).starts_with("DEPS_RUNTIME:");
#else
        denied = std::string_view(error.what())
                         .starts_with("PROFILE_BUILD: Windows runtime SDK binding");
#endif
    }

    if (!denied || std::filesystem::exists(root / ".local/toolchains/native-sdk")) {
        throw std::runtime_error(
                "untrusted runtime SDK reached full preparation dependencies or compiler discovery");
    }

    std::filesystem::remove(qnx_host_bindings_path(root));
    const auto unverified = root / ".local/toolchains/native-sdk/emulator" /
#ifdef _WIN32
                            "emulator.exe";
#else
                            "emulator";
#endif
    ensure_private_directory(unverified.parent_path());
    std::ofstream(unverified) << "unverified emulator fixture";
    const auto before = cache_file_identity(unverified);
    denied = false;
    try {
        activate_built_profile(root, qinggan09_profile(), machine_identity(qinggan09_profile(), {}),
                               false);
    } catch (const std::exception &error) {
        const std::string_view message = error.what();
        denied = message.starts_with("PROFILE_BUILD: runtime SDK pin differs") ||
                 message.starts_with(
                         "PROFILE_BUILD: verified runtime SDK pin or cached archive missing");
    }

    if (!denied || cache_file_identity(unverified) != before ||
        std::filesystem::exists(root / ".local/toolchains/native-sdk/native-deps.json")) {
        throw std::runtime_error("ready activation trusted or modified unverified SDK bytes");
    }
}

static void qnx_reuse_checks(const std::filesystem::path &stage) {
    const auto root = stage / "reuse-project";
    const std::string id(qnx_reuse_profile);
    const auto profile = root / "profiles" / id / "profile.json",
               config = profile.parent_path() / "profile-qnx.json",
               dir = root / ".local/build/accepted", source_file = dir / "source.json",
               cold_file = dir / "cold.json", accepted_file = dir / "acceptance.json";

    ensure_private_directory(profile.parent_path());
    ensure_private_directory(dir);
    auto put = [](const std::filesystem::path &p, const std::string &text) {
        ensure_private_directory(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    };
    Json definition = {{"status", "incomplete"},
                       {"android_recipe_fixture", "changed after original acceptance"}};
    write_private_json(profile, definition);
    const auto layout = qnx_runtime_layout("sa8155-qinggan11-731");
    Json originals = Json::object(), specs = Json::object();

    for (const auto &name : layout.originals) {
        const auto file = root / ".local/profiles" / id / "qnx" / name;
        put(file, "synthetic OEM " + name);
        originals[name] = {{"sha256", sha256_file(file)},
                           {"bytes", std::filesystem::file_size(file)}};
        specs[name] = {{"file", name}, {"sha256", sha256_file(file)}};
    }

    write_private_json(config, {{"hardware_adapter", layout.adapter}, {"originals", specs}});

    for (const auto &input : {profile, config}) {
        const auto guard = profile_build_detail::capture_authoring_inputs(root, id);
        guard();
        const auto before = read_private_json_bytes(input);
        put(input, before + "\n");
        bool denied = false;
        try {
            guard();
        } catch (const std::exception &error) {
            denied = std::string_view(error.what()) ==
                     "PROFILE_BUILD: authoring files changed during build";
        }

        put(input, before);

        if (!denied) {
            throw std::runtime_error("authoring edit during QNX build was accepted");
        }
    }

    const auto host_source = root / "src/app/qnx/fixture.cpp";
    put(host_source, "synthetic host source");
    const auto bundle = sha256_bytes("synthetic guest bundle");
    Json guest = {{"hardware_adapter", layout.adapter},
                  {"mcu_projection", layout.projection},
                  {"bundle_sha256", bundle},
                  {"modules_digest", qnx_guest_modules_digest(layout.adapter)}};
    Json source = {{"schema", 1},
                   {"kind", "raw-qnx-source-build"},
                   {"profile", id},
                   {"source_generated", true},
                   {"prepared_baseline_input", false},
                   {"originals", originals},
                   {"guest_build", guest}};
    qnx_runtime_metadata(source, layout);
    write_private_json(source_file, source);
    Json cold = {{"source_receipt_sha256", sha256_file(source_file)},
                 {"cold_boot_verified", true},
                 {"actual_exported_frame", true},
                 {"guest_ack", true}};
    write_private_json(cold_file, cold);
    Json images = Json::object();

    for (const auto &name : layout.images) {
        const auto f = dir / "qnx" / name;
        put(f, "synthetic accepted " + name);
        images[name] = {{"file", std::string("qnx/") + name},
                        {"sha256", sha256_file(f)},
                        {"bytes", std::filesystem::file_size(f)}};
    }

    Json accepted = {{"schema", 1},
                     {"kind", "accepted-qnx-portable-seed"},
                     {"profile", id},
                     {"source_built", true},
                     {"cold_boot_verified", true},
                     {"stopped", true},
                     {"source_bundle_sha256", bundle},
                     {"directory", dir.string()},
                     {"images", images},
                     {"source_receipt",
                      {{"file", source_file.string()}, {"sha256", sha256_file(source_file)}}},
                     {"cold_boot_evidence",
                      {{"file", cold_file.string()}, {"sha256", sha256_file(cold_file)}}}};
    qnx_runtime_metadata(accepted, layout);
    write_private_json(accepted_file, accepted);
    const auto pin = sha256_file(accepted_file);
    const auto result = validate_accepted_qnx_reuse(root, id, accepted_file, pin);

    if (result.contains("original_profile_sha256") || result.contains("current_profile_sha256") ||
        result.contains("qnx_profile_sha256") || sha256_file(accepted_file) != pin) {
        throw std::runtime_error("honest Android-only change reuse/provenance failed");
    }

    auto rejects = [&] {
        bool failed = false;
        try {
            validate_accepted_qnx_reuse(root, id, accepted_file, sha256_file(accepted_file));
        } catch (...) {
            failed = true;
        }

        if (!failed) {
            throw std::runtime_error("changed QNX closure accepted");
        }
    };
    auto changed = definition;
    changed["initial_state"] = {{"fuel", 1}};
    write_private_json(profile, changed);
    rejects();
    write_private_json(profile, definition);
    put(config, "changed QNX config");
    rejects();
    write_private_json(config, {{"hardware_adapter", layout.adapter}, {"originals", specs}});
    auto changed_config = read_private_json(config);
    changed_config["description"] = "authoring metadata changed after acceptance";
    put(config, changed_config.dump(2));
    validate_accepted_qnx_reuse(root, id, accepted_file, pin);
    write_private_json(config, {{"hardware_adapter", layout.adapter}, {"originals", specs}});
    put(host_source, "changed host source");
    validate_accepted_qnx_reuse(root, id, accepted_file, pin);
    std::filesystem::remove(host_source);
    validate_accepted_qnx_reuse(root, id, accepted_file, pin);
    // A source stage that staged another module set than the current pins is not reused.
    auto foreign = source;
    foreign["guest_build"]["modules_digest"] = qnx_guest_modules_digest("sa8155-qinggan09");
    write_private_json(source_file, foreign);
    auto foreign_cold = cold;
    foreign_cold["source_receipt_sha256"] = sha256_file(source_file);
    write_private_json(cold_file, foreign_cold);
    auto foreign_accepted = accepted;
    foreign_accepted["source_receipt"]["sha256"] = sha256_file(source_file);
    foreign_accepted["cold_boot_evidence"]["sha256"] = sha256_file(cold_file);
    write_private_json(accepted_file, foreign_accepted);
    std::string foreign_error;
    try {
        validate_accepted_qnx_reuse(root, id, accepted_file, sha256_file(accepted_file));
    } catch (const std::exception &error) {
        foreign_error = error.what();
    }

    if (foreign_error != "PROFILE_BUILD: QNX guest modules changed") {
        throw std::runtime_error("QNX source stage of another module set accepted: " +
                                 foreign_error);
    }

    write_private_json(source_file, source);
    write_private_json(cold_file, cold);
    write_private_json(accepted_file, accepted);
    const auto image = dir / "qnx" / layout.images.front();
    put(image, "changed accepted image");
    rejects();
    put(image, "synthetic accepted " + layout.images.front());
    const auto original = root / ".local/profiles" / id / "qnx" / layout.originals.front();
    put(original, "changed original OEM");
    rejects();
    put(original, "synthetic OEM " + layout.originals.front());
    auto stopped = accepted;
    stopped["stopped"] = false;
    write_private_json(accepted_file, stopped);
    rejects();
    write_private_json(accepted_file, accepted);
    put(cold_file, "changed pinned cold evidence");
    rejects();
    write_private_json(cold_file, cold);
    const std::string archive = "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304";
    definition["android"] = {
            {"package", "system-images;android-30;default;arm64-v8a"},
            {"revision", "1"},
            {"url",
             "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"},
            {"sha256", archive},
            {"clean_base", clean_base(30)}};
    write_private_json(profile, definition);
    resource_source_fixture(root, definition);
    const auto candidate = root / ".local/candidate";
    Json android_images = Json::object();

    for (const auto *name :
         {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img"}) {
        const auto f = candidate / "android" / name;
        put(f, "synthetic Android " + std::string(name));
        android_images[name] = {{"file", std::string("android/") + name},
                                {"bytes", std::filesystem::file_size(f)},
                                {"sha256", sha256_file(f)}};
    }

    android_images["cache.img"] = cache_fixture(candidate);
    std::filesystem::copy_file(profile, candidate / "profile.json");
    write_private_json(candidate / "receipt.json",
                       {{"schema", 1},
                        {"kind", "android-candidate"},
                        {"profile", id},
                        {"profile_file", "profile.json"},
                        {"profile_sha256", sha256_file(profile)},
                        {"clean_base_archive_sha256", archive},
                        {"stopped", true},
                        {"cold_boot_verified", true},
                        {"prepared_VM", true},
                        {"android", {{"abi", "arm64-v8a"}, {"images", android_images}}}});
    const auto observer_source = root / "src/guest/android/api/Observer.java",
               observer_test = root / "src/guest/android/api/ObserverTest.java";
    ensure_private_directory(observer_source.parent_path());
    std::ofstream(observer_source) << "source observer fixture";
    std::ofstream(observer_test) << "test observer fixture";
    const Json observer_inputs = {
            {"schema", 1},
            {"min_api", 28},
            {"java", std::string(64, '1')},
            {"javac", std::string(64, '2')},
            {"platform", std::string(64, '3')},
            {"dexer", std::string(64, '4')},
            {"sources", {{"Observer.java", sha256_file(observer_source)}}},
            {"test_sources", {{"ObserverTest.java", sha256_file(observer_test)}}},
            {"host_test_policy", "assertions-enabled-v1"}};
    const auto observer_cache = root / ".local/toolchains/ui-observer" /
                                sha256_bytes(observer_inputs.dump());
    ensure_private_directory(observer_cache);
    std::ofstream(observer_cache / "classes.dex") << "compiled observer publication fixture";
    write_private_json(observer_cache / "observer.json",
                       {{"schema", 1},
                        {"kind", "owned-api28-ui-observer"},
                        {"inputs", observer_inputs},
                        {"sha256", sha256_file(observer_cache / "classes.dex")}});
    portable_boot_source_fixture(root, definition);
    const auto android_receipt_file = candidate / "receipt.json";
    const auto android_receipt = read_private_json(android_receipt_file);
    const auto snapshot_bytes = read_private_json_bytes(candidate / "profile.json");
    auto rejects_snapshot = [&](const std::string &reason) {
        bool denied = false;
        try {
            finalize_profile_build(root, id, machine_identity(id, "reused-combined"), candidate,
                                   accepted_file, pin, true);
        } catch (const std::exception &error) {
            denied = std::string_view(error.what()).find(reason) != std::string_view::npos;
        }

        if (!denied || std::filesystem::exists(root / "machines/reused-combined")) {
            throw std::runtime_error("QNX reuse bypassed accepted Android snapshot validation");
        }
    };
    auto missing_snapshot = android_receipt;
    missing_snapshot.erase("profile_file");
    write_private_json(android_receipt_file, missing_snapshot);
    rejects_snapshot("accepted candidate profile snapshot required");
    write_private_json(android_receipt_file, android_receipt);
    put(candidate / "profile.json", snapshot_bytes + "\n");
    rejects_snapshot("accepted profile output checksum differs");
    put(candidate / "profile.json", snapshot_bytes);
    // Reformatting authoring input does not invalidate the cold accepted output bytes.
    put(profile, read_private_json_bytes(profile) + "\n");
    const auto published = finalize_profile_build(root, id, machine_identity(id, "reused-combined"),
                                                  candidate, accepted_file, pin, true);
    const auto seed = root / "machines/reused-combined";
    const auto runtime = read_private_json(seed / "runtime.json");
    qnx_portable_layout(id, runtime.at("qnx"));

    if (runtime.at("qnx").contains("acceptance_provenance") ||
        runtime.at("profile_sha256") != android_receipt.at("profile_sha256") ||
        sha256_file(seed / "profile.json") != sha256_bytes(snapshot_bytes) ||
        read_private_json(seed / "build-provenance.json").at("qnx").at("acceptance_sha256") !=
                pin ||
        published.at("build_provenance").at("sha256") !=
                sha256_file(seed / "build-provenance.json") ||
        sha256_file(accepted_file) != pin) {
        throw std::runtime_error("strict reused QNX finalization/provenance failed");
    }

    inspect_portable_runtime(root, id, machine_identity(id, "reused-combined"));
    // Existing-machine upgrades must reject an old full QNX profile epoch before Android
    // publication.
    definition["id"] = id;
    definition["platform"] = "qinggan11";
    definition["board"] = "8155";
    definition["features"]["system_upgrade"] = true;
    definition["wheel"] = {{"provider", "com.voyemu.input.wheel"}};
    definition["components"] = {{"view-observation", "components/view-observation"}};
    definition["install"] = Json::array({{{"artifact", "component:view-observation"},
                                          {"type", "file"},
                                          {"target", "/system/framework/framework.jar"},
                                          {"mode", "0644"}}});
    write_private_json(profile, definition);
    std::filesystem::copy_file(profile, candidate / "profile.json",
                               std::filesystem::copy_options::overwrite_existing);
    auto candidate_receipt = read_private_json(candidate / "receipt.json");
    candidate_receipt["profile_sha256"] = sha256_file(profile);
    candidate_receipt["old_machine_modified"] = false;
    candidate_receipt["checks"] = Json::array({{{"id", "boot"}, {"passed", true}}});
    candidate_receipt["installed"] = Json::array({{{"artifact", "component:view-observation"},
                                                   {"target", "/system/framework/framework.jar"},
                                                   {"mode", "0644"},
                                                   {"mutable", false}}});

    for (const auto &path : api30_framework_boot::paths(definition)) {
        candidate_receipt["installed"].push_back(
                {{"artifact", "compiled-boot-image"}, {"target", path}});
    }

    candidate_receipt["events"] =
            Json::array({{{"phase", "boot-art-compiled"},
                          {"files", 18},
                          {"command", api30_framework_boot::command(definition)}}});
    const auto package = candidate / "android/packages/owned.apk";
    put(package, "owned provider APK");
    const auto package_sha = sha256_file(package);
    const auto package_target = candidate / "android/packages" / (package_sha + ".apk");
    std::filesystem::rename(package, package_target);
    candidate_receipt["owned_package"] = {{"package", "com.voyemu.input.wheel"},
                                          {"file", "android/packages/" + package_sha + ".apk"},
                                          {"sha256", package_sha},
                                          {"bytes", std::filesystem::file_size(package_target)}};
    candidate_receipt["installed"].push_back({{"artifact", "component:wheel-input"},
                                              {"package", "com.voyemu.input.wheel"},
                                              {"target", "/data/app/owned/base.apk"},
                                              {"sha256", package_sha},
                                              {"mutable", false}});
    write_private_json(candidate / "receipt.json", candidate_receipt);
    const auto before_runtime = read_private_json_bytes(seed / "runtime.json"),
               before_data = sha256_file(seed / "android/userdata-qemu.img");
    auto unverified_qnx = accepted;
    unverified_qnx["cold_boot_verified"] = false;
    write_private_json(accepted_file, unverified_qnx);
    bool unverified_denied = false;
    try {
        finalize_profile_build(root, id, machine_identity(id, "reused-combined"), candidate,
                               accepted_file, sha256_file(accepted_file), true);
    } catch (const std::exception &e) {
        unverified_denied =
                std::string_view(e.what()).find("stopped genuine accepted QNX seed required") !=
                std::string_view::npos;
    }

    write_private_json(accepted_file, accepted);

    if (!unverified_denied || read_private_json_bytes(seed / "runtime.json") != before_runtime ||
        sha256_file(seed / "android/userdata-qemu.img") != before_data) {
        throw std::runtime_error("unverified QNX proof changed existing machine before rejection");
    }

    const auto moved = root / "machines" / id;
    ensure_private_directory(moved);
    std::filesystem::rename(dir / "qnx", moved / "qnx");
    validate_accepted_qnx_reuse(root, id, accepted_file, pin, moved);

    if (sha256_file(accepted_file) != pin) {
        throw std::runtime_error("original accepted receipt mutated");
    }
}

static void system_upgrade_candidate_checks(const std::filesystem::path &stage) {
    const std::string profile = "free-11-chinese-20260731-7.3.1";
    const auto root = stage / "system-upgrade", candidate = root / ".local/candidate",
               definition_file = root / "profiles" / profile / "profile.json";
    Json definition = {
            {"id", profile},
            {"platform", "qinggan11"},
            {"board", "8155"},
            {"features", {{"system_upgrade", true}}},
            {"wheel", {{"provider", "com.voyemu.input.wheel"}}},
            {"android",
             {{"package", "system-images;android-30;default;arm64-v8a"},
              {"revision", "1"},
              {"url",
               "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-30_r02.zip"},
              {"sha256", "cb50112cf2dd234a71e2a29b0241c23dbb37dd5fc3156c68cc9c49d0f58ad304"},
              {"clean_base", clean_base(30)}}},
            {"components", {{"view-observation", "components/view-observation"}}},
            {"install", Json::array({{{"artifact", "component:view-observation"},
                                      {"type", "file"},
                                      {"target", "/system/framework/framework.jar"},
                                      {"mode", "0644"}}})}};

    write_private_json(definition_file, definition);
    ensure_private_directory(candidate / "android");
    std::ofstream(candidate / "android/system.img") << "new synthetic immutable system";
    const Json image = {{"file", "android/system.img"},
                        {"bytes", std::filesystem::file_size(candidate / "android/system.img")},
                        {"sha256", sha256_file(candidate / "android/system.img")}};
    Json installed = Json::array({{{"artifact", "component:view-observation"},
                                   {"target", "/system/framework/framework.jar"},
                                   {"sha256", std::string(64, 'a')},
                                   {"mode", "0644"},
                                   {"mutable", false}}});

    for (const auto &path : api30_framework_boot::paths(definition)) {
        installed.push_back({{"artifact", "compiled-boot-image"}, {"target", path}});
    }

    Json receipt = {
            {"schema", 1},
            {"kind", "android-candidate"},
            {"profile", profile},
            {"profile_file", "profile.json"},
            {"profile_sha256", sha256_file(definition_file)},
            {"stopped", true},
            {"cold_boot_verified", true},
            {"prepared_VM", true},
            {"old_machine_modified", false},
            {"clean_base_archive_sha256", definition.at("android").at("sha256")},
            {"android", {{"abi", "arm64-v8a"}, {"images", {{"system.img", image}}}}},
            {"checks", Json::array({{{"id", "boot"}, {"passed", true}}})},
            {"installed", installed},
            {"events", Json::array({{{"phase", "boot-art-compiled"},
                                     {"files", 18},
                                     {"command", api30_framework_boot::command(definition)}}})}};
    std::filesystem::copy_file(definition_file, candidate / "profile.json");
    const std::string package_bytes = "owned provider APK";
    const auto package_sha = sha256_bytes(package_bytes);
    const auto package_file = candidate / "android/packages" / (package_sha + ".apk");
    ensure_private_directory(package_file.parent_path());
    std::ofstream(package_file) << package_bytes;
    receipt["owned_package"] = {{"package", "com.voyemu.input.wheel"},
                                {"file", "android/packages/" + package_sha + ".apk"},
                                {"sha256", package_sha},
                                {"bytes", static_cast<std::uint64_t>(package_bytes.size())}};
    receipt["installed"].push_back({{"artifact", "component:wheel-input"},
                                    {"package", "com.voyemu.input.wheel"},
                                    {"target", "/data/app/owned/base.apk"},
                                    {"sha256", package_sha},
                                    {"mutable", false}});
    const auto file = candidate / "receipt.json";
    write_private_json(file, receipt);

    if (validate_profile_android_system_candidate(root, profile, candidate, sha256_file(file)) !=
        image) {
        throw std::runtime_error("valid system-only source candidate rejected");
    }

    auto reject = [&](const Json &invalid) {
        write_private_json(file, invalid);
        bool denied = false;
        try {
            validate_profile_android_system_candidate(root, profile, candidate, sha256_file(file));
        } catch (...) {
            denied = true;
        }

        if (!denied) {
            throw std::runtime_error("unsafe system-upgrade candidate accepted");
        }
    };
    {
        auto camera_definition = definition;
        camera_definition["patches"]["Camera"]["steps"] =
                Json::array({"api30-camera-virtual.json"});
        write_private_json(definition_file, camera_definition);
        auto stale = receipt;
        stale["profile_sha256"] = sha256_file(definition_file);
        reject(stale);
        write_private_json(definition_file, definition);
    }
    {
        auto navi_definition = definition;
        navi_definition["patches"]["Navi"]["steps"] = Json::array({"api30-navi-vin.json"});
        write_private_json(definition_file, navi_definition);
        auto stale = receipt;
        stale["profile_sha256"] = sha256_file(definition_file);
        stale["camera_package"] = {{"package", "com.qinggan.camera"},
                                   {"file", "android/packages/" + std::string(64, 'a') + ".apk"},
                                   {"sha256", std::string(64, 'a')},
                                   {"bytes", 1}};
        reject(stale);
        write_private_json(definition_file, definition);
    }

    for (const auto *field : {"stopped", "cold_boot_verified", "prepared_VM"}) {
        auto invalid = receipt;
        invalid[field] = false;
        reject(invalid);
    }

    auto invalid = receipt;
    invalid["old_machine_modified"] = true;
    reject(invalid);
    invalid = receipt;
    invalid["profile"] = "other-profile";
    reject(invalid);
    invalid = receipt;
    invalid["checks"][0]["passed"] = false;
    reject(invalid);
    invalid = receipt;
    invalid["installed"].erase(0);
    reject(invalid);
    invalid = receipt;
    invalid["installed"].erase(1);
    reject(invalid);
    invalid = receipt;
    invalid["events"][0]["command"][0] = "foreign-dex2oat";
    reject(invalid);
    invalid = receipt;
    invalid["android"]["images"]["system.img"]["file"] = "../system.img";
    reject(invalid);
    invalid = receipt;
    invalid["android"]["images"]["system.img"]["sha256"] = std::string(64, '0');
    reject(invalid);
    write_private_json(file, receipt);
    bool denied = false;
    try {
        validate_profile_android_system_candidate(root, qinggan09_profile(), candidate,
                                                  sha256_file(file));
    } catch (...) {
        denied = true;
    }

    if (!denied) {
        throw std::runtime_error("system-upgrade producer outside Free A11 7.3.1 accepted");
    }

    const auto machine = root / "machines/accepted", manifest = machine / "runtime.json";
    write_private_json(manifest, {{"preserved", true}});
    const auto manifest_pin = sha256_file(manifest);
    invalid = receipt;
    invalid["stopped"] = false;
    write_private_json(file, invalid);
    // Passion uses the same exact API30 boot recipe without inventing a wheel APK.
    {
        const std::string passion = "passion-11-russian-20260403-2.1.4.80";
        auto source = definition;
        source["id"] = passion;
        source["platform"] = "passion";
        source["status"] = "incomplete";
        const auto profile_file = root / "profiles" / passion / "profile.json";
        write_private_json(profile_file, source);
        auto cold = receipt;
        cold["profile"] = passion;
        cold["profile_sha256"] = sha256_file(profile_file);
        std::filesystem::copy_file(profile_file, candidate / "profile.json",
                                   std::filesystem::copy_options::overwrite_existing);
        cold.erase("owned_package");
        cold["installed"].erase(cold["installed"].end() - 1);
        cold["checks"].push_back(android_candidate_vehicle_observer_policy(source));
        write_private_json(file, cold);

        if (validate_profile_android_system_candidate(root, passion, candidate,
                                                      sha256_file(file)) != image) {
            throw std::runtime_error("wheel-free Passion API30 candidate rejected");
        }

        for (unsigned mode = 0; mode < 4; ++mode) {
            auto bad = source;

            if (mode == 0) {
                bad["platform"] = "qinggan09";
            }

            if (mode == 1) {
                bad["android"]["package"] = "system-images;android-28;default;arm64-v8a";
            }

            if (mode == 2) {
                bad["install"].push_back(bad["install"][0]);
            }

            if (mode == 3) {
                bad["components"]["view-observation"] = "foreign/component";
            }

            bool refused = false;
            try {
                api30_framework_boot::command(bad);
            } catch (...) {
                refused = true;
            }

            if (!refused) {
                throw std::runtime_error("unsafe API30 Passion boot extension accepted");
            }
        }

        if (api30_framework_boot::command(source) != api30_framework_boot::command(definition)) {
            throw std::runtime_error("Passion changed pinned API30 boot recipe");
        }

        write_private_json(file, invalid);
        std::filesystem::copy_file(definition_file, candidate / "profile.json",
                                   std::filesystem::copy_options::overwrite_existing);
    }

    // Existing-target finalization must reject the system candidate before consulting or replacing
    // QNX/app payloads.
    for (const bool combined : {false, true}) {
        denied = false;
        try {
            if (combined) {
                finalize_profile_build(root, profile, machine_identity(profile, "accepted"),
                                       candidate, root / "missing-qnx.json", "missing");
            } else {
                finalize_android_profile_build(root, profile, machine_identity(profile, "accepted"),
                                               candidate, sha256_file(file));
            }
        } catch (const std::exception &error) {
            denied = std::string_view(error.what()).find("source-matched stopped cold") !=
                     std::string_view::npos;
        }

        if (!denied || sha256_file(manifest) != manifest_pin ||
            std::filesystem::exists(machine / "android")) {
            throw std::runtime_error("existing machine finalizer bypassed system-only gate");
        }
    }

    // A new proof capsule reuses the old immutable candidate without relabelling its receipt.
    receipt.erase("owned_package");

    for (const auto *role : {"vendor.img", "userdata-qemu.img", "encryptionkey.img", "cache.img"}) {
        const auto role_file = candidate / "android" / role;
        std::ofstream(role_file) << role;
        receipt["android"]["images"][role] = {{"file", std::string("android/") + role},
                                              {"bytes", std::filesystem::file_size(role_file)},
                                              {"sha256", sha256_file(role_file)}};
    }

    const auto artifacts_root = root / ".local/lineage-artifacts";
    ensure_private_directory(artifacts_root);
    const auto old_apk = artifacts_root / "old.apk", new_apk = artifacts_root / "new.apk";
    std::ofstream(old_apk) << "old source artifacts";
    std::ofstream(new_apk) << "reviewed new owned APK";
    auto describe = [](const auto &path) {
        return Json{{"file", path.string()},
                    {"bytes", std::filesystem::file_size(path)},
                    {"sha256", sha256_file(path)}};
    };
    Json old_artifacts = Json::object();

    for (unsigned n = 0; n < 54; ++n) {
        old_artifacts["synthetic:" + std::to_string(n)] = describe(old_apk);
    }

    old_artifacts["component:view-observation"] = describe(old_apk);
    old_artifacts["component:wheel-input"] = describe(old_apk);
    auto new_artifacts = old_artifacts;
    new_artifacts["component:wheel-input"] = describe(new_apk);
    const auto revision = sha256_bytes(Json{{"profile.json", sha256_file(definition_file)}}.dump());
    const auto parent_file = artifacts_root / "parent.json",
               result_file = artifacts_root / "result.json";
    write_private_json(parent_file, {{"schema", 1},
                                     {"profile", profile},
                                     {"revision", "historical-source-revision"},
                                     {"artifacts", old_artifacts}});
    Json result = {{"schema", 1},
                   {"profile", profile},
                   {"revision", revision},
                   {"artifacts", new_artifacts},
                   {"artifacts_built", true},
                   {"guest_started", false},
                   {"machine_ready", false}};
    write_private_json(result_file, result);
    receipt["artifact_result_sha256"] = sha256_file(parent_file);
    write_private_json(file, receipt);
    const auto base_pin = sha256_file(file);
    const auto capsule = root / ".local/package-upgrade",
               proof_file = artifacts_root / "actual-session-proof.json";
    ensure_private_directory(capsule);
    const std::string session(32, 'a'), boot = "12345678-1234-1234-1234-123456789abc",
                                        save_nonce(32, 'b');
    Json streams = Json::array();

    for (unsigned n = 0; n < 12; ++n) {
        streams.push_back({{"stream_index", n}, {"current_indices", Json::array({0, 0})}});
    }

    Json links = Json::array();

    for (unsigned n = 0; n < 6; ++n) {
        const auto evidence = artifacts_root / ("synthetic-evidence-" + std::to_string(n));
        std::ofstream(evidence) << "synthetic contract fixture, no real guest";
        links.push_back({{"file", evidence.string()}, {"sha256", sha256_file(evidence)}});
    }

    Json proof = {{"schema", 1},
                  {"kind", "qinggan11-owned-package-session-proof"},
                  {"profile", profile},
                  {"base_receipt_sha256", base_pin},
                  {"artifact_result_sha256", sha256_file(result_file)},
                  {"installed_package_sha256", sha256_file(new_apk)},
                  {"full_audio_startup_acceptance", true},
                  {"positive_control_restored", true},
                  {"same_boot_sdk_restore", true},
                  {"seed",
                   {{"session_id", session}, {"boot_id", boot}, {"quickboot", {{"mode", "cold"}}}}},
                  {"resume",
                   {{"session_id", std::string(32, 'c')},
                    {"boot_id", boot},
                    {"quickboot", {{"mode", "restored"}}}}},
                  {"seed_stop", {{"stopped", true}}},
                  {"resume_stop", {{"stopped", true}}},
                  {"seed_zero_streams", streams},
                  {"resume_zero_streams", streams},
                  {"saved_checkpoint",
                   {{"presave",
                     {{"host_session_id", session},
                      {"guest_boot_id", boot},
                      {"input_sequence", 0},
                      {"receipt",
                       {{"schema", 1},
                        {"status", "presave-ready"},
                        {"session_id", session},
                        {"boot_id", boot},
                        {"sequence", 0},
                        {"save_nonce", save_nonce},
                        {"acoustic_zero", true}}}}}}},
                  {"startup_quiet_pcm",
                   {{"bytes", 100},
                    {"samples", 50},
                    {"nonzero_samples", 0},
                    {"peak", 0},
                    {"seconds", 0.5}}},
                  {"resume_entire_startup_pcm",
                   {{"bytes", 100},
                    {"samples", 50},
                    {"nonzero_samples", 0},
                    {"peak", 0},
                    {"seconds", 0.5}}},
                  {"positive_pcm", {{"samples", 50}, {"nonzero_samples", 10}, {"peak", 100}}},
                  {"evidence_links", links}};
    auto reference = [](const auto &path) {
        return Json{{"file", path.string()}, {"sha256", sha256_file(path)}};
    };
    write_private_json(proof_file, proof);
    Json lineage = {
            {"schema", 1},
            {"kind", "android-owned-package-upgrade"},
            {"profile", profile},
            {"profile_sha256", sha256_file(definition_file)},
            {"base_candidate", {{"directory", candidate.string()}, {"receipt_sha256", base_pin}}},
            {"parent_artifacts", reference(parent_file)},
            {"artifacts", reference(result_file)},
            {"owned_package",
             {{"package", "com.voyemu.input.wheel"},
              {"file", "android/packages/" + sha256_file(new_apk) + ".apk"},
              {"bytes", std::filesystem::file_size(new_apk)},
              {"sha256", sha256_file(new_apk)}}},
            {"session_proof", reference(proof_file)}};
    const auto lineage_file = capsule / "receipt.json";
    auto save = [&](const Json &selected) {
        write_private_json(lineage_file, selected);
    };
    auto valid = [&] {
        save(lineage);

        return validate_profile_android_system_candidate(root, profile, capsule,
                                                         sha256_file(lineage_file));
    };

    if (valid() != image) {
        throw std::runtime_error("valid explicit package lineage rejected");
    }

    auto reject_lineage = [&](const Json &selected) {
        save(selected);
        bool failed = false;
        try {
            validate_profile_android_system_candidate(root, profile, capsule,
                                                      sha256_file(lineage_file));
        } catch (...) {
            failed = true;
        }

        if (!failed) {
            throw std::runtime_error("unsafe package upgrade lineage accepted");
        }

        if (sha256_file(file) != base_pin) {
            throw std::runtime_error("base candidate receipt modified by lineage validation");
        }
    };

    for (const auto *flag :
         {"full_audio_startup_acceptance", "positive_control_restored", "same_boot_sdk_restore"}) {
        auto bad_proof = proof;
        bad_proof[flag] = false;
        write_private_json(proof_file, bad_proof);
        auto bad = lineage;
        bad["session_proof"] = reference(proof_file);
        reject_lineage(bad);
    }

    for (const auto *counter : {"bytes", "samples"}) {
        auto bad_proof = proof;
        bad_proof["startup_quiet_pcm"][counter] = -1;
        write_private_json(proof_file, bad_proof);
        auto bad = lineage;
        bad["session_proof"] = reference(proof_file);
        reject_lineage(bad);
    }

    auto malformed = proof;
    malformed["full_audio_startup_acceptance"] = "true";
    write_private_json(proof_file, malformed);
    auto malformed_lineage = lineage;
    malformed_lineage["session_proof"] = reference(proof_file);
    reject_lineage(malformed_lineage);

    for (const auto *field : {"seed", "saved_checkpoint", "resume", "evidence_links"}) {
        auto bad_proof = proof;
        bad_proof.erase(field);
        write_private_json(proof_file, bad_proof);
        auto bad = lineage;
        bad["session_proof"] = reference(proof_file);
        reject_lineage(bad);
    }

    auto bad_proof = proof;
    bad_proof["resume"]["boot_id"] = "12345678-1234-1234-1234-123456789abd";
    write_private_json(proof_file, bad_proof);
    auto bad = lineage;
    bad["session_proof"] = reference(proof_file);
    reject_lineage(bad);
    bad_proof = proof;
    bad_proof["resume_zero_streams"][0]["current_indices"][0] = 1;
    write_private_json(proof_file, bad_proof);
    bad = lineage;
    bad["session_proof"] = reference(proof_file);
    reject_lineage(bad);
    write_private_json(proof_file, proof);

    for (auto it = old_artifacts.begin(); it != old_artifacts.end(); ++it) {
        if (it.key() != "component:wheel-input") {
            auto changed = result;
            changed["artifacts"][it.key()]["file"] = new_apk.string();
            write_private_json(result_file, changed);
            bad = lineage;
            bad["artifacts"] = reference(result_file);
            reject_lineage(bad);
        }
    }

    write_private_json(result_file, result);
    bad = lineage;
    bad["base_candidate"]["receipt_sha256"] = std::string(64, '0');
    reject_lineage(bad);
    bad = lineage;
    bad["session_proof"]["file"] = (root / "outside-local.json").string();
    reject_lineage(bad);
    const auto alias = artifacts_root / "new-alias.apk";
    std::filesystem::create_symlink(new_apk, alias);
    auto changed = result;
    changed["artifacts"]["component:wheel-input"]["file"] = alias.string();
    write_private_json(result_file, changed);
    bad = lineage;
    bad["artifacts"] = reference(result_file);
    reject_lineage(bad);
    std::filesystem::remove(alias);
    write_private_json(result_file, result);

    if (valid() != image) {
        throw std::runtime_error("valid explicit owned-package lineage rejected");
    }

    if (sha256_file(file) != base_pin) {
        throw std::runtime_error("valid lineage rewrote old candidate receipt");
    }
}

int main(int argc, char **argv) {
    if (argc == 3 && std::filesystem::path(argv[0]).stem() == "java" &&
        std::string_view(argv[1]) == "-XshowSettings:properties" &&
        std::string_view(argv[2]) == "-version") {
        const auto home = std::filesystem::absolute(argv[0]).parent_path().parent_path();
        std::cout << "java.home = " << home.string() << "\njava.version = 17.0.0\n";

        return 0;
    }

    try {
        if (argc == 8 && std::string_view(argv[1]) == "--upgrade-qnx-at") {
            const auto root = std::filesystem::canonical(argv[2]);
            std::cout << upgrade_profile_qnx_at(root, argv[3], std::filesystem::path(argv[4]),
                                                std::filesystem::path(argv[5]), argv[6],
                                                std::filesystem::path(argv[7]))
                                 .dump()
                      << '\n';

            return 0;
        }

        if (argc == 5 && std::string_view(argv[1]) == "--upgrade-qnx") {
            const auto root = std::filesystem::canonical(argv[2]);
            std::cout << upgrade_profile_qnx(root, qnx_reuse_profile,
                                             machine_identity(qnx_reuse_profile, {}),
                                             std::filesystem::canonical(argv[3]), argv[4])
                                 .dump()
                      << '\n';

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--media-origin-scratch") {
            const auto stage = std::filesystem::canonical(argv[2]) /
                               ("media-origin-" + random_token());
            ensure_private_directory(stage);
            struct Cleanup {
                std::filesystem::path p;

                ~Cleanup() {
                    std::error_code e;
                    std::filesystem::remove_all(p, e);
                }
            } cleanup{stage};
            media_origin_checks(stage);

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--artifact-resume-scratch") {
            const auto stage = std::filesystem::canonical(argv[2]) /
                               ("artifact-resume-" + random_token());
            ensure_private_directory(stage);
            struct Cleanup {
                std::filesystem::path p;

                ~Cleanup() {
                    std::error_code e;
                    std::filesystem::remove_all(p, e);
                }
            } cleanup{stage};
            artifact_resume_checks(stage);
            artifact_resume_checks(stage / "equipment", true);
            std::cout
                    << "Artifact resume exact epoch and equipment simulation derived-artifact negatives passed\n";

            return 0;
        }

        if (argc == 6 && std::string_view(argv[1]) == "--validate-artifact-resume") {
            std::cout << validate_profile_artifact_resume(
                                 std::filesystem::canonical(argv[2]), argv[3],
                                 std::filesystem::canonical(argv[4]), argv[5])
                                 .dump()
                      << '\n';

            return 0;
        }

        if (argc == 6 && std::string_view(argv[1]) == "--resume-android") {
            std::cout << build_profile_android(std::filesystem::canonical(argv[2]), argv[3],
                                               machine_identity(argv[3], {}), true, {}, {},
                                               std::filesystem::canonical(argv[4]), argv[5])
                                 .dump()
                      << '\n';

            return 0;
        }

        if (argc == 8 && std::string_view(argv[1]) == "--upgrade-android-artifacts") {
            std::cout << upgrade_profile_android_system(
                                 std::filesystem::canonical(argv[2]), argv[3],
                                 machine_identity(argv[3], {}), std::filesystem::canonical(argv[4]),
                                 argv[5], std::filesystem::canonical(argv[6]), argv[7])
                                 .dump()
                      << '\n';

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--bind-qinggan11-731") {
            const auto root = std::filesystem::canonical(argv[2]);
            std::cout << bind_machine_configuration(
                                 root, qnx_reuse_profile, machine_identity(qnx_reuse_profile, ""),
                                 root / "machines" / qnx_reuse_profile, root / "voyemu", true, true)
                      << '\n';

            return 0;
        }

        if (argc == 7 && std::string_view(argv[1]) == "--finalize-reused-qnx") {
            const auto root = std::filesystem::canonical(argv[2]);
            std::cout << finalize_profile_build(root, qnx_reuse_profile,
                                                machine_identity(qnx_reuse_profile, ""),
                                                std::filesystem::canonical(argv[3]),
                                                std::filesystem::canonical(argv[4]), argv[5], true,
                                                std::filesystem::canonical(argv[6]))
                                 .dump()
                      << '\n';

            return 0;
        }

        if ((argc == 5 || argc == 6) && std::string_view(argv[1]) == "--verify-qnx-reuse") {
            const auto root = std::filesystem::canonical(argv[2]);
            std::cout << validate_accepted_qnx_reuse(root, qnx_reuse_profile,
                                                     std::filesystem::path(argv[3]), argv[4],
                                                     argc == 6 ? std::filesystem::path(argv[5])
                                                               : std::filesystem::path{})
                                 .dump()
                      << '\n';

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--qnx-reuse-scratch") {
            const auto stage = std::filesystem::canonical(argv[2]) /
                               ("qnx-reuse-" + random_token());
            ensure_private_directory(stage);
            struct Cleanup {
                std::filesystem::path p;

                ~Cleanup() {
                    std::error_code e;
                    std::filesystem::remove_all(p, e);
                }
            } cleanup{stage};
            qnx_reuse_checks(stage);
            std::cout << "QNX genuine closure reuse plus eight changed-closure negatives passed\n";

            return 0;
        }

        if (argc == 6 && std::string_view(argv[1]) == "--validate-system-candidate") {
            std::cout << validate_profile_android_system_candidate(
                                 std::filesystem::canonical(argv[2]), argv[3],
                                 std::filesystem::canonical(argv[4]), argv[5])
                                 .dump()
                      << '\n';

            return 0;
        }

        if (argc == 7 && std::string_view(argv[1]) == "--finalize-android") {
            const auto root = std::filesystem::canonical(argv[2]),
                       candidate = std::filesystem::absolute(argv[4]);
            std::cout << finalize_android_profile_build(root, argv[3],
                                                        machine_identity(argv[3], argv[6]),
                                                        candidate, argv[5])
                                 .dump()
                      << '\n';

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--build-output-release-scratch") {
            const auto stage = std::filesystem::canonical(argv[2]) /
                               ("build-output-release-" + random_token());
            ensure_private_directory(stage);
            struct Cleanup {
                std::filesystem::path p;

                ~Cleanup() {
                    std::error_code error;
                    std::filesystem::remove_all(p, error);
                }
            } cleanup{stage};
            build_output_release_checks(stage);
            std::cout << "one-step build generation release passed\n";

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--gc-working-disks-scratch") {
            const auto stage = std::filesystem::canonical(argv[2]) /
                               ("gc-working-disks-" + random_token());
            ensure_private_directory(stage);
            struct Cleanup {
                std::filesystem::path p;

                ~Cleanup() {
                    std::error_code error;
                    std::filesystem::remove_all(p, error);
                }
            } cleanup{stage};
            gc_working_disks_checks(stage);

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--system-upgrade-scratch") {
            const auto stage = std::filesystem::canonical(argv[2]) /
                               ("system-upgrade-" + random_token());
            ensure_private_directory(stage);
            struct Cleanup {
                std::filesystem::path p;

                ~Cleanup() {
                    std::error_code error;
                    std::filesystem::remove_all(p, error);
                }
            } cleanup{stage};
            system_upgrade_candidate_checks(stage);
            std::cout
                    << "Free A11 7.3.1 system-upgrade source candidate and preserved finalizer gates passed\n";

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--api30-scratch") {
            const auto stage = std::filesystem::canonical(argv[2]) /
                               ("profile-build-api30-" + random_token());
            ensure_private_directory(stage);
            struct Cleanup {
                std::filesystem::path p;

                ~Cleanup() {
                    std::error_code error;
                    std::filesystem::remove_all(p, error);
                }
            } cleanup{stage};
            api30_publication_checks(stage);
            std::cout << "API30 partial publication and pinned acceptance guards passed\n";

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--frontend-scratch") {
            const auto stage = std::filesystem::canonical(argv[2]) /
                               ("frontend-binding-" + random_token());
            ensure_private_directory(stage);
            struct Cleanup {
                std::filesystem::path p;

                ~Cleanup() {
                    std::error_code error;
                    std::filesystem::remove_all(p, error);
                }
            } cleanup{stage};
            frontend_binding_checks(stage);
            startup_video_resource_checks(stage);
            observer_cache_checks(stage);
            runtime_sdk_activation_checks(stage);
            std::cout
                    << "explicit/default frontend bindings and source-matched observer cache passed\n";

            return 0;
        }

        if (argc == 6 && std::string_view(argv[1]) == "--refresh-observer-source") {
            const std::string profile = argv[3];

            if (std::string_view(argv[4]) != profile) {
                throw std::runtime_error("OBSERVER_SOURCE: exact canonical arguments required");
            }

            const auto root = std::filesystem::canonical(argv[2]),
                       seed = std::filesystem::canonical(argv[5]);

            if (seed != root / "machines" / profile) {
                throw std::runtime_error("OBSERVER_SOURCE: exact canonical seed required");
            }

            std::cout << refresh_machine_ui_observer(
                                 root, argv[3], machine_identity(argv[3], argv[4]), seed,
                                 [&] {
                                     return prepare_profile_ui_observer(root);
                                 },
                                 false)
                                 .dump()
                      << char(10);

            return 0;
        }

        if (argc == 6 && std::string(argv[1]) == "--refresh-observer") {
            std::cout << refresh_built_profile_ui_observer(std::filesystem::canonical(argv[2]),
                                                           argv[3],
                                                           machine_identity(argv[3], argv[4]),
                                                           std::filesystem::canonical(argv[5]))
                                 .dump()
                      << char(10);

            return 0;
        }

        if (argc == 5 && std::string_view(argv[1]) == "--observer-explicit-jdk") {
            const auto root = std::filesystem::canonical(argv[2]),
                       jdk = std::filesystem::canonical(argv[4]);

            if (const auto *home = std::getenv("JAVA_HOME"); home && *home) {
                throw std::runtime_error("explicit JDK regression requires unset JAVA_HOME");
            }

            if (!cached_profile_ui_observer(root).empty()) {
                throw std::runtime_error("explicit JDK regression requires a cold observer cache");
            }

            bool reached_artifacts = false;
            try {
                (void)build_profile_android(root, argv[3], machine_identity(argv[3], {}), true, jdk,
                                            {}, {}, "observer-dispatch-boundary");
            } catch (const std::exception &error) {
                if (std::string_view(error.what()) !=
                    "PROFILE_BUILD: artifact receipt pin requires receipt") {
                    throw;
                }

                reached_artifacts = true;
            }

            const auto observer = cached_profile_ui_observer(root);

            if (!reached_artifacts || observer.empty() ||
                observer.at("sha256").get<std::string>() !=
                        sha256_file(observer.at("dex").get<std::string>())) {
                throw std::runtime_error("explicit JDK build dispatch did not prepare observer");
            }

            if (prepare_profile_ui_observer(root) != observer) {
                throw std::runtime_error("ready observer cache rediscovered a JVM");
            }

            std::cout << observer.dump() << '\n';

            return 0;
        }

        if (argc == 3 && std::string(argv[1]) == "--observer") {
            const auto root = std::filesystem::absolute(argv[2]);
            const auto first = prepare_profile_ui_observer(root),
                       second = prepare_profile_ui_observer(root);

            if (first != second || first.at("sha256").get<std::string>() !=
                                           sha256_file(first.at("dex").get<std::string>())) {
                throw std::runtime_error("observer cache differs");
            }

            std::cout << first.dump() << char(10);

            return 0;
        }

        if (argc == 6 && std::string(argv[1]) == "--finalize") {
            const auto root = std::filesystem::absolute(argv[2]),
                       candidate = std::filesystem::absolute(argv[4]),
                       qnx = std::filesystem::absolute(argv[5]);
            std::cout << finalize_profile_build(root, argv[3], machine_identity(argv[3], {}),
                                                candidate, qnx, sha256_file(qnx))
                                 .dump()
                      << char(10);

            return 0;
        }

        const bool full_scratch = argc == 3 && std::string_view(argv[1]) == "--scratch";

        if (argc != 1 && !full_scratch) {
            throw std::runtime_error("unexpected arguments");
        }

        bool rejected = false;
        try {
            build_profile_android("relative", qinggan09_profile(),
                                  machine_identity(qinggan09_profile(), {}), true);
        } catch (...) {
            rejected = true;
        }

        if (!rejected) {
            throw std::runtime_error("relative project started preparation");
        }

        const auto source_root = std::filesystem::canonical(
                                         std::filesystem::absolute(std::filesystem::path(__FILE__)))
                                         .parent_path()
                                         .parent_path()
                                         .parent_path()
                                         .parent_path();
        const auto parent = full_scratch ? std::filesystem::path(argv[2])
                                         : source_root / ".local/build/tmp";

        if (!parent.is_absolute() || parent.lexically_normal() != parent ||
            parent.filename() != "tmp" || parent.parent_path().filename() != "build" ||
            parent.parent_path().parent_path().filename() != ".local") {
            throw std::runtime_error("full fixture requires absolute project .local/build/tmp");
        }

        ensure_private_directory(parent);
        const auto stage = parent / ("pb-" + random_token().substr(0, 16));
        ensure_private_directory(stage);
        struct Cleanup {
            std::filesystem::path p;

            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove_all(p, e);
            }
        } cleanup{stage};
        const auto root = stage / "source-project";
        ensure_private_directory(root / "profiles" / qinggan09_profile());
        std::filesystem::copy_file(source_root / "profiles" / qinggan09_profile() / "profile.json",
                                   root / "profiles" / qinggan09_profile() / "profile.json");
        const std::string unprepared_id = "free-11-chinese-20260731-7.3.1";
        ensure_private_directory(root / "profiles" / unprepared_id);
        std::filesystem::copy_file(source_root / "profiles" / unprepared_id / "profile.json",
                                   root / "profiles" / unprepared_id / "profile.json");

        for (const auto &machine : {MachineIdentity{}, machine_identity("other-profile", {})}) {
            for (bool full : {false, true}) {
                bool denied = false;
                try {
                    if (full) {
                        build_profile(root, qinggan09_profile(), machine, true);
                    } else {
                        build_profile_android(root, qinggan09_profile(), machine, true);
                    }
                } catch (const std::exception &error) {
                    denied = std::string_view(error.what()) ==
                             "PROFILE_BUILD: valid machine child name required";
                }

                if (!denied || std::filesystem::exists(root / "machines") ||
                    std::filesystem::exists(root / ".local/toolchains")) {
                    throw std::runtime_error("unbound machine identity reached build preparation");
                }
            }
        }

        for (bool full : {false, true}) {
            bool attempted = false;
            try {
                build_profile(root, unprepared_id,
                              machine_identity(unprepared_id, "unprepared-a11"), true, {}, {}, full,
                              true);
            } catch (const std::exception &error) {
                const std::string_view text = error.what();
                attempted = !text.starts_with("PROFILE_INCOMPLETE:") &&
                            !text.starts_with("CLI_ARGUMENT:");
            }

            if (!attempted || std::filesystem::exists(root / "machines")) {
                throw std::runtime_error(
                        "incomplete source cannot attempt genuine offline preparation");
            }
        }

        for (const auto &roles : {std::pair{false, false}, std::pair{true, false}}) {
            bool denied = false;
            try {
                build_profile(root, unprepared_id,
                              machine_identity(unprepared_id, "unprepared-a11"), true, {}, {},
                              roles.first, roles.second);
            } catch (const std::exception &error) {
                denied = std::string_view(error.what()).starts_with("PROFILE_BUILD:");
            }

            if (!denied) {
                throw std::runtime_error(
                        "unsupported role scope reached a guest or returned success");
            }
        }

        const auto candidate = root / ".local/android";
        ensure_private_directory(candidate);
        const auto profile_pin =
                sha256_file(root / "profiles" / qinggan09_profile() / "profile.json");
        write_private_json(candidate / "receipt.json", {{"schema", 1},
                                                        {"kind", "android-candidate"},
                                                        {"profile", qinggan09_profile()},
                                                        {"profile_sha256", profile_pin},
                                                        {"stopped", true},
                                                        {"cold_boot_verified", true},
                                                        {"prepared_VM", true}});
        const auto acceptance = root / ".local/unaccepted-qnx.json";
        write_private_json(acceptance, {{"schema", 1},
                                        {"kind", "accepted-qnx-portable-seed"},
                                        {"profile", qinggan09_profile()},
                                        {"profile_sha256", profile_pin},
                                        {"source_built", false},
                                        {"cold_boot_verified", false}});
        std::string acceptance_error;
        rejected = false;
        try {
            finalize_profile_build(root, qinggan09_profile(),
                                   machine_identity(qinggan09_profile(), {}), candidate, acceptance,
                                   sha256_file(acceptance));
        } catch (const std::exception &e) {
            acceptance_error = e.what();
            rejected = acceptance_error.find("accepted source-built QNX") != std::string::npos;
        }

        if (!rejected) {
            throw std::runtime_error("unaccepted QNX guard not reached: " + acceptance_error);
        }

        const auto isolated = stage / "android-only-project";
        ensure_private_directory(isolated);
        const auto def = isolated / "profiles" / qinggan09_profile() / "profile.json";
        write_private_json(
                def,
                {{"title", "Synthetic Free 1.0"},
                 {"platform", "qinggan09"},
                 {"board", "8155"},
                 {"android",
                  {{"package", "system-images;android-28;default;arm64-v8a"},
                   {"revision", "1"},
                   {"url",
                    "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip"},
                   {"sha256", "86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9"},
                   {"clean_base", clean_base(28)}}}});
        const auto own_candidate = isolated / ".local/candidate";
        ensure_private_directory(own_candidate);
        auto captured_definition = read_private_json(def);
        captured_definition["components"]["can-model-observer"] = "components/can-model-observer";
        write_private_json(def, captured_definition);
        write_private_json(def.parent_path() / "profile-qnx.json",
                           {{"schema", 1}, {"hardware_adapter", "sa8155-qinggan09"}});
        auto accepted = read_private_json(candidate / "receipt.json");
        accepted["profile_sha256"] = sha256_file(def);
        accepted["clean_base_archive_sha256"] =
                "86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9";
        Json image_records = Json::object();

        for (const auto *name :
             {"system.img", "vendor.img", "userdata-qemu.img", "encryptionkey.img"}) {
            const auto file = own_candidate / "android" / name;
            ensure_private_directory(file.parent_path());
            std::ofstream(file) << name;
            image_records[name] = {{"file", std::string("android/") + name},
                                   {"bytes", std::filesystem::file_size(file)},
                                   {"sha256", sha256_file(file)}};
        }

        image_records["cache.img"] = cache_fixture(own_candidate);
        // Insert before own_receipt serialization in the existing isolated A9 full/Android fixture.
        accepted["android"] = {{"abi", "arm64-v8a"}, {"images", image_records}};
        const std::string observer_sha(64, 'a'),
                observer_target = "/system/bin/voyemu-can-model-observer";
        accepted["old_machine_modified"] = false;
        accepted["installed"] = Json::array({{{"artifact", "component:can-model-observer"},
                                              {"target", observer_target},
                                              {"mode", "0755"},
                                              {"mutable", false},
                                              {"package", ""},
                                              {"sha256", observer_sha}}});
        accepted["checks"] =
                Json::array({{{"id", "artifact:component:can-model-observer:" + observer_target},
                              {"passed", true},
                              {"expected", observer_sha},
                              {"actual", observer_sha}}});

        // These synthetic bytes test publication contracts only, never guest
        // installation/signatures.
        for (const auto *component : {"audiopolicy", "bootstrap"}) {
            const auto source = def.parent_path() / "components" / component / "component.json";
            ensure_private_directory(source.parent_path());
            std::ofstream(source) << "{}";
        }

        accepted["artifact_result_sha256"] = sha256_bytes("synthetic full artifact result");
        Json own_packages = Json::array();

        for (unsigned i = 0; i < 2; ++i) {
            const auto bytes = std::string("synthetic own package ") + std::to_string(i);
            const auto sha = sha256_bytes(bytes), relative = "android/packages/" + sha + ".apk";
            ensure_private_directory((own_candidate / relative).parent_path());
            std::ofstream(own_candidate / relative) << bytes;
            own_packages.push_back({{"package", std::string(qinggan09_owned_package_names[i])},
                                    {"file", relative},
                                    {"sha256", sha},
                                    {"bytes", bytes.size()}});
        }

        accepted["qinggan09_owned_packages"] = {
                {"schema", 1},
                {"profile", qinggan09_profile()},
                {"api", 28},
                {"certificate_sha256", std::string(qinggan09_owned_packages_certificate_sha256)},
                {"receipt_sha256", accepted.at("artifact_result_sha256")},
                {"packages", own_packages}};

        const auto own_receipt = own_candidate / "receipt.json";
        std::filesystem::copy_file(def, own_candidate / "profile.json");
        accepted["profile_file"] = "profile.json";
        write_private_json(own_receipt, accepted);
        auto refuse_android = [&](const Json &bad, const std::string &pin) {
            write_private_json(own_receipt, bad);
            bool failed = false;
            try {
                finalize_android_profile_build(isolated, qinggan09_profile(),
                                               machine_identity(qinggan09_profile(), {}),
                                               own_candidate, pin);
            } catch (...) {
                failed = true;
            }

            if (!failed || std::filesystem::exists(isolated / "machines" / qinggan09_profile())) {
                throw std::runtime_error("invalid Android-only candidate published");
            }
        }; // Insert after refuse_android lambda and before positive finalization,
           // preserving all old negatives.
        {
            auto reject_owned = [&](const Json &bad) {
                write_private_json(own_receipt, bad);
                refuse_android(bad, sha256_file(own_receipt));
            };
            auto bad = accepted;
            bad.erase("qinggan09_owned_packages");
            reject_owned(bad);
            bad = accepted;
            bad["qinggan09_owned_packages"]["receipt_sha256"] = std::string(64, '0');
            reject_owned(bad);
            bad = accepted;
            bad["qinggan09_owned_packages"]["api"] = 30;
            reject_owned(bad);
            bad = accepted;
            bad["qinggan09_owned_packages"]["packages"][0]["package"] = "foreign.package";
            reject_owned(bad);
            bad = accepted;
            bad["qinggan09_owned_packages"]["packages"][0]["sha256"] = std::string(64, '0');
            reject_owned(bad);
            const auto file = own_candidate / accepted.at("qinggan09_owned_packages")
                                                      .at("packages")
                                                      .at(0)
                                                      .at("file")
                                                      .get<std::string>();
            std::filesystem::rename(file, file.string() + ".absent");
            reject_owned(accepted);
            std::filesystem::rename(file.string() + ".absent", file);
        }

        refuse_android(accepted, std::string(64, '0'));
        {
            auto legacy = accepted;
            legacy.erase("profile_file");
            write_private_json(own_receipt, legacy);
            refuse_android(legacy, sha256_file(own_receipt));
            std::filesystem::rename(own_candidate / "profile.json",
                                    own_candidate / "profile.json.absent");
            write_private_json(own_receipt, accepted);
            refuse_android(accepted, sha256_file(own_receipt));
            std::filesystem::rename(own_candidate / "profile.json.absent",
                                    own_candidate / "profile.json");
            const auto profile_before = sha256_file(own_candidate / "profile.json");
            std::ofstream(own_candidate / "profile.json", std::ios::app).put('\n');

            if (sha256_file(own_candidate / "profile.json") == profile_before) {
                throw std::runtime_error("candidate tamper fixture did not change its bytes");
            }

            refuse_android(accepted, sha256_file(own_receipt));
            std::filesystem::copy_file(def, own_candidate / "profile.json",
                                       std::filesystem::copy_options::overwrite_existing);
            auto different = read_private_json(def);
            different["android"]["revision"] = "2";
            write_private_json(def, different);
            refuse_android(accepted, sha256_file(own_receipt));
            std::filesystem::copy_file(own_candidate / "profile.json", def,
                                       std::filesystem::copy_options::overwrite_existing);
        }
        auto not_stopped = accepted;
        not_stopped["stopped"] = false;
        write_private_json(own_receipt, not_stopped);
        refuse_android(not_stopped, sha256_file(own_receipt));
        auto unverified = accepted;
        unverified["cold_boot_verified"] = false;
        write_private_json(own_receipt, unverified);
        refuse_android(unverified, sha256_file(own_receipt));
        auto bad_image = accepted;
        bad_image["android"]["images"]["system.img"]["sha256"] = std::string(64, '0');
        write_private_json(own_receipt, bad_image);
        refuse_android(bad_image, sha256_file(own_receipt));
        auto no_cache = accepted;
        no_cache["android"]["images"].erase("cache.img");
        write_private_json(own_receipt, no_cache);
        refuse_android(no_cache, sha256_file(own_receipt));
        write_private_json(own_receipt, accepted);
        resource_source_fixture(isolated, read_private_json(def));
        portable_boot_source_fixture(isolated, read_private_json(def));
        // A verified candidate owns its exact profile output, not the author's
        // formatting.
        std::ofstream(def, std::ios::app) << " \n\n";
        const auto finalized = finalize_android_profile_build(
                isolated, qinggan09_profile(), machine_identity(qinggan09_profile(), {}),
                own_candidate, sha256_file(own_receipt));

        if (sha256_file(isolated / "machines" / qinggan09_profile() / "profile.json") !=
            accepted.at("profile_sha256").get<std::string>()) {
            throw std::runtime_error("Android finalizer did not preserve exact profile bytes");
        }

        if (!read_private_json(isolated / "machines" / qinggan09_profile() / "runtime.json")
                     .at("android")
                     .contains("can_model_receipt")) {
            throw std::runtime_error("accepted profile snapshot lost the cold CAN proof");
        }

        std::filesystem::rename(def, def.parent_path() / "moved-profile.json");

        if (inspect_portable_runtime(isolated, qinggan09_profile(),
                                     machine_identity(qinggan09_profile(), {}))
                    .at("profile_sha256") != accepted.at("profile_sha256")) {
            throw std::runtime_error("finalized snapshot depends on source path");
        }

        std::filesystem::rename(def.parent_path() / "moved-profile.json", def);

        if (read_private_json(isolated / "machines" / qinggan09_profile() / "runtime.json")
                    .at("schema") != 1) {
            throw std::runtime_error("new candidate did not publish portable schema 1");
        }

        if (finalized.at("profile_ready") != false || finalized.at("android_ready") != true ||
            finalized.at("scope") != "android" || finalized.at("cluster_available") != false) {
            throw std::runtime_error("Android-only publication claimed full acceptance");
        }

        rejected = false;
        try {
            activate_built_profile(isolated, qinggan09_profile(),
                                   machine_identity(qinggan09_profile(), {}), true);
        } catch (const std::exception &e) {
            rejected = std::string(e.what()).find("cluster unavailable") != std::string::npos;
        }

        if (!rejected || std::filesystem::exists(isolated / ".local/toolchains")) {
            throw std::runtime_error("Android-only cluster request reached dependencies");
        }

        const auto own_manifest =
                read_private_json(isolated / "machines" / qinggan09_profile() / "runtime.json");

        if (own_manifest.at("android").at("qinggan09_owned_packages") !=
            accepted.at("qinggan09_owned_packages")) {
            throw std::runtime_error("full build lost portable own2 descriptor");
        }

        const auto canonical = isolated / "machines" / qinggan09_profile() / "runtime.json";
        const auto canonical_pin = sha256_file(canonical);
        const std::string alternate = "qinggan09-portable-second";
        std::vector<std::string> build_words{
                (isolated / "voyemu").string(), "build", qinggan09_profile(),
                (isolated / "machines" / alternate).string(), "--cluster=off"};
        std::vector<char *> build_argv;

        for (auto &word : build_words) {
            build_argv.push_back(word.data());
        }

        const auto selected = cli::parse(static_cast<int>(build_argv.size()), build_argv.data(),
                                         isolated / "voyemu");

        if (selected.root != isolated || selected.profile != qinggan09_profile() ||
            selected.machine.name != alternate || !selected.machine.selected || selected.cluster) {
            throw std::runtime_error("public build destination selected another backend machine");
        }

        const auto named = finalize_android_profile_build(selected.root, selected.profile,
                                                          selected.machine, own_candidate,
                                                          sha256_file(own_receipt));
        const auto named_runtime = isolated / "machines" / alternate / "runtime.json";

        // Compare paths, not spellings: Windows mixes separators in the fixture root.
        if (std::filesystem::path(named.at("runtime").get<std::string>()) != named_runtime ||
            sha256_file(canonical) != canonical_pin ||
            read_private_json(named_runtime).at("profile") != qinggan09_profile() ||
            inspect_portable_runtime(isolated, qinggan09_profile(),
                                     machine_identity(qinggan09_profile(), alternate))
                            .at("schema") != 1) {
            throw std::runtime_error(
                    "named publication changed source identity/canonical or inspected wrong seed");
        }

        for (const auto &target : {alternate, std::string("../escape")}) {
            bool refused = false;
            try {
                finalize_android_profile_build(isolated, qinggan09_profile(),
                                               machine_identity(qinggan09_profile(), target),
                                               own_candidate, sha256_file(own_receipt));
            } catch (...) {
                refused = true;
            }

            if (!refused || sha256_file(canonical) != canonical_pin ||
                sha256_file(named_runtime) != named.at("runtime_sha256").get<std::string>()) {
                throw std::runtime_error("existing/unsafe named target replaced a machine");
            }
        }

        for (bool full : {false, true}) {
            bool refused = false;
            try {
                if (full) {
                    build_profile(isolated, qinggan09_profile(),
                                  machine_identity(qinggan09_profile(), alternate), true, {}, {});
                } else {
                    build_profile_android(isolated, qinggan09_profile(),
                                          machine_identity(qinggan09_profile(), alternate), true,
                                          {}, {});
                }
            } catch (const std::exception &error) {
                refused = std::string(error.what()).starts_with("PROFILE_BUILD: machine ") &&
                          std::string(error.what()).find(" already exists.") != std::string::npos;
            }

            if (!refused || std::filesystem::exists(isolated / ".local/toolchains")) {
                throw std::runtime_error("existing named build reached dependencies");
            }
        }

        const auto qnx_source = isolated / ".local/qnx-accepted";
        ensure_private_directory(qnx_source / "qnx");
        Json qnx_images = Json::object();

        for (const auto *name : {"hyp-hvf.img", "ifs2-native.img", "host-native-lzo.bin",
                                 "smem.bin", "qnx-ifs2-restore.bin", "synthetic-gpt.bin",
                                 "sysla-gpt-ramdisk-v3.img", "qnx-ipl-virt.bin"}) {
            const auto path = qnx_source / "qnx" / name;
            std::ofstream(path) << name;
            qnx_images[name] = {{"file", std::string("qnx/") + name},
                                {"bytes", std::filesystem::file_size(path)},
                                {"sha256", sha256_file(path)}};
        }

        const auto proof = qnx_source / "proof.json";
        write_private_json(proof, {{"fixture", true}});
        const Json proof_ref = {{"file", proof.string()}, {"sha256", sha256_file(proof)}};
        const auto qnx_receipt = qnx_source / "acceptance.json";
        write_private_json(qnx_receipt, {{"schema", 1},
                                         {"kind", "accepted-qnx-portable-seed"},
                                         {"profile", qinggan09_profile()},
                                         {"profile_sha256", sha256_file(def)},
                                         {"source_built", true},
                                         {"cold_boot_verified", true},
                                         {"directory", qnx_source.string()},
                                         {"images", qnx_images},
                                         {"source_receipt", proof_ref},
                                         {"cold_boot_evidence", proof_ref}});
        const std::string combined = "qinggan09-portable-combined";
        const auto named_full = finalize_profile_build(
                isolated, qinggan09_profile(), machine_identity(qinggan09_profile(), combined),
                own_candidate, qnx_receipt, sha256_file(qnx_receipt));
        const auto combined_runtime = isolated / "machines" / combined / "runtime.json";

        if (sha256_file(isolated / "machines" / combined / "profile.json") !=
            accepted.at("profile_sha256").get<std::string>()) {
            throw std::runtime_error("full finalizer did not preserve exact profile bytes");
        }

        if (named_full.at("runtime") != combined_runtime.string() ||
            sha256_file(canonical) != canonical_pin ||
            inspect_portable_runtime(isolated, qinggan09_profile(),
                                     machine_identity(qinggan09_profile(), combined))
                            .at("profile") != qinggan09_profile() ||
            !read_private_json(combined_runtime).contains("qnx")) {
            throw std::runtime_error("full named finalizer did not preserve canonical/profile");
        }

        if (read_private_json(combined_runtime).at("android").at("qinggan09_owned_packages") !=
            accepted.at("qinggan09_owned_packages")) {
            throw std::runtime_error("combined finalizer lost own2 descriptor");
        }

        const std::string clone_id = "custom-qinggan09-development";
        auto clone_definition = read_private_json(def);
        clone_definition["id"] = clone_id;
        clone_definition["title"] = "Development";
        clone_definition["local"] = true;
        clone_definition["capture"] = {{"profile", clone_id},
                                       {"referenceProfile", qinggan09_profile()}};
        const auto clone_profile = isolated / "profiles" / clone_id;
        ensure_private_directory(clone_profile);
        write_private_json(clone_profile / "profile.json", clone_definition);
        write_private_json(clone_profile / "profile-qnx.json",
                           {{"schema", 1}, {"hardware_adapter", "sa8155-qinggan09"}});
        std::filesystem::copy(def.parent_path() / "components", clone_profile / "components",
                              std::filesystem::copy_options::recursive);
        resource_source_fixture(isolated, clone_definition);
        {
            const auto can_machine = isolated / "machines" / combined,
                       can_file = can_machine / "can-model/candidate.json";
            ensure_private_directory(can_file.parent_path());
            const auto can_manifest = read_private_json(combined_runtime);
            const auto can_system = can_manifest.at("android")
                                            .at("images")
                                            .at("system.img")
                                            .at("sha256")
                                            .get<std::string>();
            const std::string can_artifact = "component:can-model-observer",
                              can_target = "/system/bin/voyemu-can-model-observer",
                              can_pin(64, 'a');
            write_private_json(
                    can_file,
                    {{"schema", 1},
                     {"kind", "android-candidate"},
                     {"profile", qinggan09_profile()},
                     {"profile_sha256", can_manifest.at("profile_sha256")},
                     {"stopped", true},
                     {"prepared_VM", true},
                     {"cold_boot_verified", true},
                     {"old_machine_modified", false},
                     {"android", {{"images", {{"system.img", {{"sha256", can_system}}}}}}},
                     {"installed", Json::array({{{"artifact", can_artifact},
                                                 {"target", can_target},
                                                 {"mode", "0755"},
                                                 {"mutable", false},
                                                 {"package", ""},
                                                 {"sha256", can_pin}}})},
                     {"checks", Json::array({{{"id", "artifact:" + can_artifact + ":" + can_target},
                                              {"passed", true},
                                              {"expected", can_pin},
                                              {"actual", can_pin}}})}});
            auto can_runtime = can_manifest;
            can_runtime["android"]["can_model_receipt"] = {
                    {"file", "can-model/candidate.json"},
                    {"bytes", std::filesystem::file_size(can_file)},
                    {"sha256", sha256_file(can_file)}};
            write_private_json(combined_runtime, can_runtime);

            if (inspect_portable_runtime(isolated, qinggan09_profile(),
                                         machine_identity(qinggan09_profile(), combined))
                        .at("profile") != qinggan09_profile()) {
                throw std::runtime_error("can-model fixture source inspection failed");
            }
        }
        auto clone_candidate = accepted;
        clone_candidate["profile"] = clone_id;
        clone_candidate["profile_sha256"] = sha256_file(clone_profile / "profile.json");
        std::filesystem::copy_file(clone_profile / "profile.json", own_candidate / "profile.json",
                                   std::filesystem::copy_options::overwrite_existing);
        write_private_json(own_receipt, clone_candidate);
        const auto clone_result =
                finalize_android_profile_build(isolated, clone_id, machine_identity(clone_id, {}),
                                               own_candidate, sha256_file(own_receipt));
        const auto clone_machine = isolated / "machines" / clone_id;
        const auto clone_manifest = inspect_portable_runtime(isolated, clone_id,
                                                             machine_identity(clone_id, {}));

        if (clone_result.at("profile") != clone_id || clone_manifest.at("profile") != clone_id ||
            clone_manifest.at("android").at("qinggan09_owned_packages") !=
                    accepted.at("qinggan09_owned_packages")) {
            throw std::runtime_error(
                    "logical profile clone lost owned APK family or public identity");
        }

        auto clone_qnx = read_private_json(qnx_receipt);
        clone_qnx["profile"] = clone_id;
        clone_qnx["profile_sha256"] = sha256_file(clone_profile / "profile.json");
        clone_qnx["stopped"] = true;
        clone_qnx["persistence"] = {{"schema", 1},
                                    {"system_image", "sysla-gpt-ramdisk-v3.img"},
                                    {"userdata_image", "userdata-qnx6.img"}};
        clone_qnx["interguest_network"] = qnx_interguest_contract();
        const auto clone_userdata = qnx_source / "qnx/userdata-qnx6.img";
        std::ofstream(clone_userdata) << "synthetic empty private QNX userdata";
        clone_qnx["images"]["userdata-qnx6.img"] = {
                {"file", "qnx/userdata-qnx6.img"},
                {"bytes", std::filesystem::file_size(clone_userdata)},
                {"sha256", sha256_file(clone_userdata)}};
        write_private_json(qnx_receipt, clone_qnx);
        const auto clone_combined = finalize_profile_build(
                isolated, clone_id, machine_identity(clone_id, "custom-qinggan09-combined"),
                own_candidate, qnx_receipt, sha256_file(qnx_receipt));
        const auto clone_full = inspect_portable_runtime(
                isolated, clone_id, machine_identity(clone_id, "custom-qinggan09-combined"));

        if (clone_combined.at("profile") != clone_id || clone_full.at("profile") != clone_id ||
            clone_full.at("qnx").at("persistence") != clone_qnx.at("persistence") ||
            clone_full.at("qnx").at("interguest_network") != clone_qnx.at("interguest_network")) {
            throw std::runtime_error("logical clone lost persistent QNX family or identity");
        }

        published_boot_check(clone_machine);
        published_resources_check(clone_machine);
        std::filesystem::rename(isolated / "profiles", isolated / "moved-profiles");
        std::filesystem::rename(isolated / "src", isolated / "moved-src");

        // The authoring checkout is absent. Output byte integrity must still reject
        // tampering.
        {
            const auto output = isolated / "machines" / qinggan09_profile() / "profile.json";
            const auto bytes = read_private_json_bytes(output);
            const auto profile_before = sha256_file(output);
            std::ofstream(output, std::ios::app).put('\n');

            if (sha256_file(output) == profile_before) {
                throw std::runtime_error("machine tamper fixture did not change its bytes");
            }

            bool refused = false;
            try {
                inspect_portable_runtime(isolated, qinggan09_profile(),
                                         machine_identity(qinggan09_profile(), {}));
            } catch (...) {
                refused = true;
            }

            std::ofstream(output, std::ios::binary) << bytes;

            if (!refused) {
                throw std::runtime_error("source-free machine accepted altered profile output");
            }
        }

        for (const auto &machine : {std::string(qinggan09_profile()), alternate, combined}) {
            const auto seed = isolated / "machines" / machine;
            published_resources_check(seed);
            published_boot_check(seed);
            const auto inspected = inspect_portable_runtime(
                    isolated, qinggan09_profile(), machine_identity(qinggan09_profile(), machine));

            for (const auto &[name, record] : inspected.at("android").at("images").items()) {
                if (record.at("sha256") != image_records.at(name).at("sha256")) {
                    throw std::runtime_error("resource export changed Android images");
                }
            }
        }

        frontend_binding_checks(stage);
        startup_video_resource_checks(stage);
        observer_cache_checks(stage);
        runtime_sdk_activation_checks(stage);
        try {
            api30_publication_checks(stage);
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("Free A11 7.3.1 publication fixture: ") +
                                     e.what());
        }

        try {
            api30_publication_checks(stage, true);
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("Free A11 6.11.1 publication fixture: ") +
                                     e.what());
        }

        system_upgrade_candidate_checks(stage);
        media_origin_checks(stage / "media-origin");
        std::cout << "profile build preflight and QNX acceptance publication gate passed\n";

        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';

        return 1;
    }
}
