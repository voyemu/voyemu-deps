#pragma once
#include "check.hpp"
#include "logging.hpp"
#include "os/fs.hpp"
#include "portable_runtime.hpp"
#include "process.hpp"
#include "profile_originals.hpp"
#include "project_files.hpp"
#include "qnx_imagefs.hpp"
#include "qnx_originals_directory.hpp"
#include "startup_video_frames.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <set>
namespace voyemu {
inline constexpr Check portable_resource_need{"PORTABLE_RESOURCES"};

inline std::filesystem::path portable_resource_relative(std::string_view name) {
    const std::filesystem::path path(name);

    portable_resource_need(!path.empty() && !path.is_absolute() &&
                                   path == path.lexically_normal() &&
                                   path.generic_string().starts_with("ui/"),
                           "relative UI resource required");

    for (const auto &part : path) {
        portable_resource_need(part != ".." && part != ".", "resource leaves machine");
    }

    return path;
}

// OEM boot animations and the frames of the OEM startup videos are larger than the own UI images.
inline std::uint64_t portable_resource_limit(std::string_view name) {
    return name.starts_with("ui/startup/") ? 256ULL << 20 : 16ULL << 20;
}

// The imported OEM boot animation of each multimedia screen, by its original role.
inline constexpr std::array<std::pair<const char *, const char *>, 2> portable_startup_roles{
        {{"BootAnimation", "ui/startup/display0.zip"},
         {"BootAnimation1", "ui/startup/display1.zip"}}};
// The boot animation of each screen of the screens window, by its original role and screen role.
inline constexpr std::array<std::pair<const char *, const char *>, 1> portable_startup_screens{
        {{"BootAnimation2", "rear"}}};
// The frames of each OEM startup video declared in startupVideos of profile.json, by screen: the
// multimedia screens play theirs before their boot animation, the cluster in the cluster area and
// the screens of the screens window, by role, in their area.
inline constexpr std::array<std::pair<const char *, const char *>, 4> portable_startup_videos{
        {{"display0", "ui/startup/display0-video.zip"},
         {"display1", "ui/startup/display1-video.zip"},
         {"cluster", "ui/startup/cluster.zip"},
         {"hud", "ui/startup/hud-video.zip"}}};

inline std::string portable_startup_screen_resource(std::string_view role) {
    return "ui/startup/" + std::string(role) + ".zip";
}

// One video of startupVideos in profile.json: its size, SHA-256 and length, and its path in the
// nested ImageFS of an imported QNX image.
struct StartupVideoPin {
    std::uint64_t bytes{};
    std::string sha256, image, nested, path;
    double seconds{};
};

inline StartupVideoPin startup_video_pin(const Json &video) {
    const auto &acquisition = video.at("acquisition");
    StartupVideoPin pin;

    pin.bytes = video.at("bytes").get<std::uint64_t>();
    pin.sha256 = video.at("sha256").get<std::string>();
    pin.image = acquisition.at("image").get<std::string>();
    pin.nested = acquisition.at("nested").get<std::string>();
    pin.path = acquisition.at("path").get<std::string>();
    pin.seconds = video.at("seconds").get<double>();

    return pin;
}

// Extracts a declared startup video from the imported QNX image that holds it in a
// nested ImageFS; the image is pinned by profile-qnx.json and the video by profile.json.
inline std::string portable_startup_video(const std::filesystem::path &root,
                                          std::string_view profile, const StartupVideoPin &video) {
    const auto &image = video.image, &nested = video.nested;
    const auto qnx = read_private_json(
            root / "profiles" / std::string(profile) / "profile-qnx.json", 128 * 1024);
    const auto &original = qnx.at("originals").at(image);
    auto key = image;

    std::replace(key.begin(), key.end(), '.', '_');
    const Json *region = nullptr;

    for (const auto &entry : qnx.at("nested_imagefs").at(key)) {
        if (entry.at("file") == nested) {
            region = &entry;
        }
    }

    portable_resource_need(original.at("file") == image && region != nullptr,
                           "declared nested QNX image required");
    const auto file = qnx_originals_directory(root, profile) / image;
    portable_resource_need(std::filesystem::is_regular_file(file) &&
                                   std::filesystem::file_size(file) == original.at("bytes"),
                           "imported QNX image required; fetch the profile");
    const auto offset = region->at("offset").get<std::uint64_t>(),
               bytes = region->at("bytes").get<std::uint64_t>();
    portable_resource_need(offset <= original.at("bytes").get<std::uint64_t>() &&
                                   bytes <= original.at("bytes").get<std::uint64_t>() - offset &&
                                   bytes <= 512ULL << 20,
                           "nested QNX image outside its container");
    std::string slice(bytes, '\0');
    std::ifstream source(file, std::ios::binary);
    source.seekg(static_cast<std::streamoff>(offset));
    portable_resource_need(bool(source.read(slice.data(), static_cast<std::streamsize>(bytes))),
                           "unreadable QNX image");
    auto result = read_qnx_imagefs_file(slice, video.path);
    portable_resource_need(result.size() == video.bytes && video.sha256 == sha256_bytes(result),
                           "imported original differs");

    return result;
}

inline Json export_portable_resources(const std::filesystem::path &root, const Json &definition,
                                      std::string_view profile, const std::filesystem::path &seed) {
    Json resources = Json::object();
    const auto add = [&](const std::filesystem::path &source, const std::string &name,
                         std::string_view expected = {}) {
        const auto relative = portable_resource_relative(name);
        const auto size = std::filesystem::file_size(source);
        portable_resource_need(size > 0 && size <= portable_resource_limit(name),
                               "bounded nonempty UI resource required");
        const auto pin = sha256_file(source);
        portable_resource_need(expected.empty() || pin == expected, "imported original differs");
        ensure_private_directory((seed / relative).parent_path());
        copy_portable_image(source, pin, seed / relative, 0);
        resources[name] = {{"file", name}, {"bytes", size}, {"sha256", pin}};
    };
    // A project file; an installation without src/ adds the copy that the launcher embeds.
    const auto add_project = [&](const std::filesystem::path &file, const std::string &name) {
        if (!file.generic_string().starts_with("src/") || project_source_checkout(root)) {
            add(root / file, name);

            return;
        }

        const auto bytes = project_file_bytes(root, file.generic_string(),
                                              portable_resource_limit(name));
        ensure_private_directory(root / ".local/build/tmp");
        const auto copy = root / ".local/build/tmp" / ("project-file-" + random_token());
        struct Cleanup {
            std::filesystem::path path;

            ~Cleanup() {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } cleanup{copy};
        std::ofstream output(copy, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        portable_resource_need(bool(output), "project file copy failed");
        add(copy, name);
    };

    add_project("src/assets/voyemu-logo-white-black-1920-720.png", "ui/splash.png");

    for (const auto &[role, name] : portable_startup_roles) {
        if (definition.contains("original") && definition.at("original").contains(role)) {
            const auto &original = definition.at("original").at(role);
            ensure_private_directory(seed / "ui/startup");
            add(profile_originals_directory(root, profile) / original.at("file").get<std::string>(),
                name, original.at("sha256").get<std::string>());
        }
    }

    for (const auto &[role, screen] : portable_startup_screens) {
        if (definition.contains("original") && definition.at("original").contains(role)) {
            const auto &original = definition.at("original").at(role);
            ensure_private_directory(seed / "ui/startup");
            add(profile_originals_directory(root, profile) / original.at("file").get<std::string>(),
                portable_startup_screen_resource(screen), original.at("sha256").get<std::string>());
        }
    }

    if (definition.contains("startupVideos")) {
        // Without a host decoder the videos become black frames of the same length.
        for (const auto &[screen, name] : portable_startup_videos) {
            if (!definition.at("startupVideos").contains(screen)) {
                continue;
            }

            const auto video = startup_video_pin(definition.at("startupVideos").at(screen));
            ensure_private_directory(seed / "ui/startup");
            const auto frames = seed / (std::string(name) + ".part");
            const bool decoded = write_startup_video_animation(
                    portable_startup_video(root, profile, video), video.seconds, frames);
            logging::write(logging::Level::info, "prepare", "startup_video.frames",
                           {{"screen", screen}, {"decoded", decoded}});
            add(frames, name);
            std::filesystem::remove(frames);
        }
    }

    const auto route = definition.value("wheel", Json::object())
                               .value("androidRoutes",
                                      std::string("src/app/android-wheel-routes.json"));
    const std::filesystem::path route_path = route;
    portable_resource_need(!route_path.is_absolute(), "relative route source required");

    for (const auto &part : route_path) {
        portable_resource_need(part != "..", "route source leaves project");
    }

    add_project(route_path, "ui/wheel-routes.json");
    ensure_private_directory(seed / "ui/wheel");

    if (definition.contains("wheel") && definition.at("wheel").contains("assets")) {
        const std::filesystem::path wheel = definition.at("wheel").at("assets").get<std::string>();
        portable_resource_need(!wheel.is_absolute(), "relative wheel source required");

        for (const auto &part : wheel) {
            portable_resource_need(part != "..", "wheel source leaves project");
        }

        for (const auto *name : {"wheel-hotspots.json", "wheel-left.png", "wheel-right.png"}) {
            add(root / wheel / name, std::string("ui/wheel/") + name);
        }

        for (const auto *name : {"wheel-left-light.png", "wheel-right-light.png"}) {
            if (std::filesystem::is_regular_file(root / wheel / name)) {
                add(root / wheel / name, std::string("ui/wheel/") + name);
            }
        }
    }

    return resources;
}

inline std::filesystem::path portable_resource_path(const std::filesystem::path &seed,
                                                    std::string_view name) {
    const auto relative = portable_resource_relative(name);
    const auto manifest = read_private_json(seed / "runtime.json", 128 * 1024);

    portable_resource_need(manifest.contains("resources") && manifest.at("resources").is_object() &&
                                   manifest.at("resources").size() <= 32 &&
                                   manifest.at("resources").contains(std::string(name)),
                           "machine resource missing; prepare its local resource bundle");
    const auto &record = manifest.at("resources").at(std::string(name));
    portable_resource_need(record.is_object() && record.size() == 3 &&
                                   record.at("file") == relative.generic_string() &&
                                   record.at("bytes").is_number_unsigned() &&
                                   record.at("bytes").get<std::uint64_t>() > 0 &&
                                   record.at("bytes").get<std::uint64_t>() <=
                                           portable_resource_limit(name),
                           "bounded exact resource descriptor required");
    const auto file = seed / relative;

    for (const auto &at : os::ancestors(file)) {
        portable_resource_need(!std::filesystem::is_symlink(at),
                               "linked machine resource rejected");
    }

    portable_resource_need(
            std::filesystem::is_regular_file(file) && std::filesystem::hard_link_count(file) == 1 &&
                    std::filesystem::file_size(file) == record.at("bytes").get<std::uint64_t>() &&
                    sha256_file(file) == record.at("sha256").get<std::string>(),
            "machine resource differs");

    return file;
}

// The window's startup video frames: absent without any, otherwise display0, display1, cluster and
// hud, each the archive path or null for a screen without a video.
inline Json portable_startup_video_paths(const std::filesystem::path &seed) {
    const auto manifest = read_private_json(seed / "runtime.json", 128 * 1024);
    Json videos = Json::object();
    bool any = false;

    for (const auto &[screen, name] : portable_startup_videos) {
        const bool present = manifest.contains("resources") &&
                             manifest.at("resources").is_object() &&
                             manifest.at("resources").contains(name);
        videos[screen] = present ? Json(portable_resource_path(seed, name).string()) : Json();
        any = any || present;
    }

    return any ? videos : Json();
}

// The frontend fields of the startup videos: startup_video with both multimedia screens, each a
// path or null, and startup_cluster; a field is absent when its videos are.
inline Json portable_startup_video_frontend(const std::filesystem::path &seed) {
    const auto videos = portable_startup_video_paths(seed);
    Json fields = Json::object();

    if (videos.is_null()) {
        return fields;
    }

    if (!videos.at("display0").is_null() || !videos.at("display1").is_null()) {
        fields["startup_video"] = Json::array({videos.at("display0"), videos.at("display1")});
    }

    if (!videos.at("cluster").is_null()) {
        fields["startup_cluster"] = videos.at("cluster");
    }

    return fields;
}

// The frontend fields of the screens window: startup_screens with the boot animation and
// startup_video_screens with the startup video of each screen that has one, by role; a field is
// absent without any.
inline Json portable_startup_screens_frontend(const std::filesystem::path &seed) {
    const auto manifest = read_private_json(seed / "runtime.json", 128 * 1024);
    const auto present = [&](const std::string &name) {
        return manifest.contains("resources") && manifest.at("resources").is_object() &&
               manifest.at("resources").contains(name);
    };
    Json fields = Json::object();

    for (const auto &[role, screen] : portable_startup_screens) {
        if (const auto name = portable_startup_screen_resource(screen); present(name)) {
            fields["startup_screens"][screen] = portable_resource_path(seed, name).string();
        }
    }

    for (const auto &[screen, name] : portable_startup_videos) {
        if (screen != std::string_view("display0") && screen != std::string_view("display1") &&
            screen != std::string_view("cluster") && present(name)) {
            fields["startup_video_screens"][screen] = portable_resource_path(seed, name).string();
        }
    }

    return fields;
}

// Window startup screens: absent without imported animations, otherwise each screen's animation or
// null for a black screen.
inline Json portable_startup_animations(const std::filesystem::path &seed) {
    const auto manifest = read_private_json(seed / "runtime.json", 128 * 1024);
    Json startup = Json::array();
    bool any = false;

    for (const auto &[role, name] : portable_startup_roles) {
        const bool present = manifest.contains("resources") &&
                             manifest.at("resources").is_object() &&
                             manifest.at("resources").contains(name);
        startup.push_back(present ? Json(portable_resource_path(seed, name).string()) : Json());
        any = any || present;
    }

    return any ? startup : Json();
}
} // namespace voyemu
