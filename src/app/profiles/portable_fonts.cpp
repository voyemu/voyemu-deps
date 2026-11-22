#include "portable_fonts.hpp"
#include "check.hpp"
#include "java_toolchain.hpp"
#include "logging.hpp"
#include "os/platform.hpp"
#include "portable_resources.hpp"
#include "process.hpp"
#include "zip_archive.hpp"
#include <fstream>
#include <map>
#include <set>

namespace voyemu {
namespace {
constexpr Check need{"PORTABLE_FONTS"};

std::set<std::string> keys(const Json &font) {
    std::set<std::string> names;

    for (const auto &[key, value] : font.items()) {
        if (key != "corrected") {
            names.insert(key);
        }
    }

    return names;
}

std::string read_file(const std::filesystem::path &file) {
    std::ifstream input(file, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});

    need(bool(input) || input.eof(), "unreadable font");

    return bytes;
}

// The bytes of a declared font, each read once from its pinned original.
std::string source_font(const std::filesystem::path &root, const Json &definition,
                        std::string_view profile, const Json &font) {
    const auto names = keys(font);

    if (names == std::set<std::string>{"original"}) {
        const auto &original = definition.at("original").at(font.at("original").get<std::string>());
        const auto file = profile_originals_directory(root, profile) /
                          original.at("file").get<std::string>();
        need(original.at("bytes").get<std::uint64_t>() <= portable_resource_limit("ui/fonts/") &&
                     std::filesystem::is_regular_file(file) &&
                     std::filesystem::file_size(file) ==
                             original.at("bytes").get<std::uint64_t>() &&
                     sha256_file(file) == original.at("sha256").get<std::string>(),
             "imported font original required; fetch the profile");

        return read_file(file);
    }

    need(names == std::set<std::string>{"original", "entry", "bytes", "sha256"},
         "font declaration required");
    const auto bytes = font.at("bytes").get<std::uint64_t>();
    const auto sha256 = font.at("sha256").get<std::string>();
    need(bytes > 0 && bytes <= portable_resource_limit("ui/fonts/") && sha256_text(sha256),
         "bounded pinned font required");
    const auto &original = definition.at("original").at(font.at("original").get<std::string>());
    const auto data = read_pinned_zip_entry(profile_originals_directory(root, profile) /
                                                    original.at("file").get<std::string>(),
                                            original.at("sha256").get<std::string>(),
                                            font.at("entry").get<std::string>(), sha256, bytes);
    need(data.size() == bytes && sha256_bytes(data) == sha256, "imported font differs");

    return data;
}

// The DFP King Gothic GB Regular and Semibold originals space their Russian letters out; Java
// preparation writes the one corrected font of each.
std::string corrected_font(const std::filesystem::path &root, const std::string &source,
                           const Json &corrected, const std::filesystem::path &stage) {
    const auto tools = ensure_java_preparation_tools(root, {}, true);
    const auto classpath = java_preparation_classpath(root, tools.runtime);
    const auto java = tools.runtime.jdk / "bin" / ("java" + std::string(os::executable_suffix));

    need(sha256_file(java) == tools.runtime.java_sha256, "installed JDK changed");
    const auto input = stage / "input.ttf", output = stage / "output.ttf";
    std::filesystem::remove(input);
    std::filesystem::remove(output);
    {
        std::ofstream file(input, std::ios::binary);
        file.write(source.data(), static_cast<std::streamsize>(source.size()));
        need(bool(file), "font repair input failed");
    }
    ProcessSpec command;
    command.executable = java;
    command.directory = root;
    command.arguments = {"-Xmx512m",     "-Djava.io.tmpdir=" + stage.string(),  "-cp",
                         classpath,      "com.voyemu.prepare.FontCyrillicMain", input.string(),
                         output.string()};
    command.environment = {{"JDK_JAVA_OPTIONS", ""},
                           {"JAVA_TOOL_OPTIONS", ""},
                           {"_JAVA_OPTIONS", ""},
                           {"CLASSPATH", ""},
                           {"TMPDIR", stage.string()}};
    const auto run = execute(command, 120000, 1 << 16);
    logging::write(logging::Level::debug, "prepare", "font.cyrillic",
                   {{"exit_code", run.exit_code}, {"output", run.output}});
    need(!run.timed_out && run.exit_code == 0, "Cyrillic font repair failed: " + run.output);
    auto data = read_file(output);
    need(data.size() == corrected.at("bytes").get<std::uint64_t>() &&
                 sha256_bytes(data) == corrected.at("sha256").get<std::string>(),
         "corrected font differs");

    return data;
}
} // namespace

Json export_portable_fonts(const std::filesystem::path &root, const Json &definition,
                           std::string_view profile, const std::filesystem::path &seed) {
    Json resources = Json::object();

    if (!definition.contains("fonts")) {
        return resources;
    }

    const auto &fonts = definition.at("fonts");
    need(fonts.is_object() && fonts.size() == 2 && fonts.contains("primary") &&
                 fonts.contains("fallback"),
         "primary and fallback fonts required");
    const auto stage = seed / "ui/fonts/.stage";
    ensure_private_directory(stage);
    std::map<std::string, std::string> sources;

    for (const auto *role : {"primary", "fallback"}) {
        const auto &weights = fonts.at(role);
        need(weights.is_object() && weights.size() == 3 && weights.contains("regular") &&
                     weights.contains("medium") && weights.contains("bold"),
             "regular, medium and bold fonts required");

        for (const auto *weight : {"regular", "medium", "bold"}) {
            const auto &font = weights.at(weight);
            need(font.is_object(), "font declaration required");
            const auto key = font.dump();
            auto found = sources.find(key);

            if (found == sources.end()) {
                auto data = source_font(root, definition, profile, font);

                if (font.contains("corrected")) {
                    data = corrected_font(root, data, font.at("corrected"), stage);
                }

                found = sources.emplace(key, std::move(data)).first;
            }

            const auto name = std::string("ui/fonts/") + role + "-" + weight + ".ttf";
            const auto &data = found->second;
            {
                std::ofstream file(seed / portable_resource_relative(name), std::ios::binary);
                file.write(data.data(), static_cast<std::streamsize>(data.size()));
                need(bool(file), "font write failed");
            }
            resources[name] = {
                    {"file", name}, {"bytes", data.size()}, {"sha256", sha256_bytes(data)}};
        }
    }

    std::filesystem::remove_all(stage);
    logging::write(logging::Level::info, "prepare", "fonts.exported",
                   {{"count", resources.size()}});

    return resources;
}
} // namespace voyemu
