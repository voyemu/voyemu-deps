#include "media_readiness.hpp"
#include "process.hpp"
#include "program_modes.hpp"
#include "startup_animation_fixture.hpp"
#include "window_image.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
using namespace voyemu;
namespace {
void need(bool yes, const char *message) {
    if (!yes) {
        throw std::runtime_error(message);
    }
}

struct Capture {
    window::Image image;
    unsigned width, height;
};

// The media row of a native capture: the lower half of the top-left canvas.
Capture media_capture(const std::filesystem::path &path, const Json &proof) {
    auto image = window::decode_png(path);
    const auto width = unsigned(image.width), height = unsigned(image.height);

    need(width >= 1920 && height >= 720, "capture dimensions invalid");
    const auto &geometry = proof.at("canvas_geometry");
    const auto &content = geometry.at("content_pixels");
    need(content.at(0) == 0 && content.at(1) == 0, "top-left canvas expected");
    const auto content_width = static_cast<unsigned>(content.at(2).get<double>()),
               content_height = static_cast<unsigned>(content.at(3).get<double>());
    need(content_width <= width && content_height <= height && content_height > 0,
         "content escaped native capture");
    need(std::abs(geometry.at("framebuffer_scale").at(0).get<double>() -
                  geometry.at("framebuffer_scale").at(1).get<double>()) < 1e-6,
         "canvas must preserve wheel/text/OEM aspect");

    return {std::move(image), content_width, content_height};
}

std::array<std::uint8_t, 3> pixel(const Capture &capture, unsigned x, unsigned y) {
    const auto p = (std::size_t(y) * unsigned(capture.image.width) + x) * 4;

    return {capture.image.rgba[p], capture.image.rgba[p + 1], capture.image.rgba[p + 2]};
}

// Without OEM animations each Android screen shows only its own centered startup logo.
void startup_mask(const std::filesystem::path &path, const Json &proof) {
    const auto capture = media_capture(path, proof);
    const auto width = capture.width;
    std::size_t left = 0, right = 0, outside = 0;

    for (unsigned y = capture.height / 2; y < capture.height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            if (pixel(capture, x, y) != std::array<std::uint8_t, 3>{}) {
                ++(x < width / 2 ? left : right);
                const auto center = x < width / 2 ? width / 4 : width * 3 / 4;

                if ((x > center ? x - center : center - x) >= width / 8) {
                    ++outside;
                }
            }
        }
    }

    need(left > 100 && right > 100, "startup logo must be centered on each Android screen");
    need(outside == 0, "startup logo or guest content escaped the per-screen splash bounds");
}

// Synthetic boot animation: two parts at 2 fps, three solid frames and a held final frame.
constexpr std::array<std::array<std::uint8_t, 3>, 4> startup_colors{
        {{200, 0, 0}, {0, 200, 0}, {0, 0, 200}, {255, 255, 0}}};

std::filesystem::path startup_archive(const std::filesystem::path &directory,
                                      const std::string &name = "bootanimation",
                                      unsigned width = 1920, unsigned height = 720) {
    std::vector<std::pair<std::string, std::string>> entries = {
            {"desc.txt", std::to_string(width) + " " + std::to_string(height) +
                                 " 2\np 1 0 part0\np 0 0 part1\n"}};

    for (unsigned frame = 0; frame < startup_colors.size(); ++frame) {
        std::vector<std::uint8_t> bgra(std::size_t(width) * height * 4);

        for (std::size_t at = 0; at < bgra.size(); at += 4) {
            bgra[at] = startup_colors[frame][2];
            bgra[at + 1] = startup_colors[frame][1];
            bgra[at + 2] = startup_colors[frame][0];
            bgra[at + 3] = 255;
        }

        const auto png = directory / (name + std::to_string(frame) + ".png");
        window::save_png(png, int(width), int(height), bgra);
        std::ifstream source(png, std::ios::binary);
        entries.emplace_back(frame < 3 ? "part0/00" + std::to_string(frame + 1) + ".png"
                                       : "part1/001.png",
                             std::string(std::istreambuf_iterator<char>(source), {}));
    }

    const auto file = directory / (name + ".zip");
    startup_fixture::zip(file, entries);

    return file;
}

// Screen 1 plays its two-frame video, screen 2 its held black frame; the cluster video lasts
// longer than the capture.
constexpr std::array<std::uint8_t, 2> display_video_levels{70, 140};
constexpr std::array<std::uint8_t, 12> cluster_video_levels{40,  55,  70,  85,  100, 115,
                                                            130, 145, 160, 175, 190, 205};

// After its video, screen 1 plays the animation in order and holds its final frame; screen 2
// stays black.
void startup_animation(const std::filesystem::path &session, const Json &proof) {
    need(proof.at("startup_frames") ==
                 Json::array({Json::array({"0/0", "0/1", "0/2", "1/0"}), Json::array()}),
         "boot animation frames were not shown in desc.txt order");
    const auto first = media_capture(session / "media-native-proof/splash.png", proof),
               last = media_capture(session / "media-native-proof/combined-final.png", proof);
    const auto start = pixel(first, first.width / 4, first.height * 3 / 4);
    need(start == std::array<std::uint8_t, 3>{} ||
                 start == std::array<std::uint8_t, 3>{display_video_levels[0],
                                                      display_video_levels[0],
                                                      display_video_levels[0]},
         "screen 1 must start with its video");
    need(pixel(last, last.width / 4, last.height * 3 / 4) == startup_colors[3],
         "final boot animation frame is not held on screen 1");

    for (const auto *capture : {&first, &last}) {
        for (unsigned y = capture->height / 2; y < capture->height; ++y) {
            for (unsigned x = capture->width / 2; x < capture->width; ++x) {
                need(pixel(*capture, x, y) == std::array<std::uint8_t, 3>{},
                     "screen 2 without an OEM animation is not black");
            }
        }
    }
}

// A synthetic startup video as preparation stores it: solid gray frames at 2 fps, held `pause`
// frames after the last one.
std::filesystem::path startup_video(const std::filesystem::path &directory, const std::string &name,
                                    unsigned width, unsigned height,
                                    const std::vector<std::uint8_t> &levels, unsigned pause,
                                    unsigned hold_first = 0) {
    // With hold_first the first frame is a part of its own held that many frames, so it stays due
    // alone even when a loaded window draws its first frames late.
    std::vector<std::pair<std::string, std::string>> entries = {
            {"desc.txt", std::to_string(width) + " " + std::to_string(height) + " 2\n" +
                                 (hold_first ? "p 1 " + std::to_string(hold_first) + " first\n"
                                             : std::string()) +
                                 "p 1 " + std::to_string(pause) + " part0\n"}};

    for (unsigned frame = 0; frame < levels.size(); ++frame) {
        std::vector<std::uint8_t> bgra(std::size_t(width) * height * 4, levels[frame]);

        for (std::size_t at = 3; at < bgra.size(); at += 4) {
            bgra[at] = 255;
        }

        const auto png = directory / (name + std::to_string(frame) + ".png");
        window::save_png(png, int(width), int(height), bgra);
        std::ifstream source(png, std::ios::binary);
        char entry[32];
        std::snprintf(entry, sizeof entry, "%s/%05u.png",
                      hold_first && frame == 0 ? "first" : "part0", frame);
        entries.emplace_back(entry, std::string(std::istreambuf_iterator<char>(source), {}));
    }

    const auto file = directory / (name + ".zip");
    startup_fixture::zip(file, entries);

    return file;
}

// All videos start together from their first frame; the multimedia videos end before their
// screens show the boot animation, and the final capture shows the last cluster frame taken.
void startup_videos(const std::filesystem::path &session, const Json &proof) {
    const auto &videos = proof.at("startup_video");

    need(videos.at("display0").at("frames") == Json::array({0, 1}) &&
                 videos.at("display0").at("ended") == true &&
                 videos.at("display1").at("frames") == Json::array({0}) &&
                 videos.at("display1").at("ended") == true,
         "multimedia startup videos did not play once to their end");
    const auto &cluster = videos.at("cluster");
    const auto &frames = cluster.at("frames");

    for (const auto &video : {videos.at("display0"), videos.at("display1"), cluster}) {
        need(video.at("error") == "", "startup video failed");
    }

    need(cluster.at("playing") == true && !frames.empty() && frames.at(0) == 0 &&
                 frames.back() < cluster_video_levels.size(),
         "cluster startup video did not start from its first frame");

    for (std::size_t i = 1; i < frames.size(); ++i) {
        need(frames.at(i) > frames.at(i - 1), "cluster startup video frames went backwards");
    }

    const auto last = media_capture(session / "media-native-proof/combined-final.png", proof);
    const auto &rect = proof.at("cluster_ui_inspection").at("layout").at("qnx_rect_points");
    const auto scale = proof.at("canvas_geometry").at("framebuffer_scale").at(0).get<double>();
    const auto x = unsigned((rect.at(0).get<double>() + rect.at(2).get<double>() / 2) * scale),
               y = unsigned((rect.at(1).get<double>() + rect.at(3).get<double>() / 2) * scale);
    const auto gray = cluster_video_levels.at(frames.back().get<unsigned>());

    for (const auto channel : pixel(last, x, y)) {
        need(std::abs(int(channel) - int(gray)) <= 4,
             "cluster area does not show the taken video frame");
    }
}

// Bounded synthetic SDL input; the fixture has no SDK, so Android never becomes ready.
void input_smoke(const Json &proof, const std::filesystem::path &session) {
    need(proof.contains("panel_input_up_s") && proof.at("panel_input_up_s").get<double>() >= 1,
         "synthetic parameters tab click was not released");
    need(proof.contains("panel_input_drawn_ms") &&
                 proof.at("cluster_ui_inspection").at("selected_section") == 1,
         "synthetic parameters tab click did not select the second panel");
    need(!proof.contains("input_reaction") && proof.at("input_rpc_acknowledgements") == 0 &&
                 !std::filesystem::exists(session / "media-native-proof/after-input.png"),
         "Android input smoke ran without a ready Android");
}

// The radio next to the wheel opens the screens window at the zoom of the main window: the rear
// screen holds the final frame of its boot animation and the HUD is black after its video.
void screens_smoke(const Json &proof, const std::filesystem::path &session) {
    const auto &screens = proof.at("screens_window");
    const auto density = std::max(screens.at("pixel_density").get<double>(), 1.0);
    const auto pixels = [density](int requested) {
        return std::lround(std::lround(requested / density) * density);
    };
    const auto width = pixels(951 + 640), height = pixels(360);

    need(proof.contains("screens_radio_up_s") && screens.at("open") == true &&
                 screens.at("window_pixels") == Json::array({width, height}),
         "the screens radio did not open the screens window at 50 percent");
    need(screens.at("screens").at(0).at("role") == "rear" &&
                 screens.at("screens").at(0).at("startup_frames") == Json::array({"1/0"}) &&
                 screens.at("screens").at(1).at("role") == "hud",
         "the rear screen did not hold its final boot animation frame");
    const auto &video = proof.at("startup_video").at("hud");
    need(video.at("frames") == Json::array({0, 9}) && video.at("ended") == true &&
                 video.at("error") == "",
         "the HUD startup video did not play once");
    const auto capture = window::decode_png(session / "media-native-proof/screens-final.png");
    need(capture.width == width && capture.height == height, "screens window capture size");
    const auto at = [&](unsigned x, unsigned y) {
        const auto p = (std::size_t(y) * unsigned(capture.width) + x) * 4;

        return std::array<std::uint8_t, 3>{capture.rgba[p], capture.rgba[p + 1],
                                           capture.rgba[p + 2]};
    };
    need(at(475, 180) == startup_colors[3], "rear screen does not show its boot animation");
    need(at(951 + 320, 180) == std::array<std::uint8_t, 3>{}, "HUD is not black after its video");
    need(at(475, 20) != std::array<std::uint8_t, 3>{} && at(475, 20) != startup_colors[3],
         "the shorter rear screen is not centred in the row");
}

void keyboard_smoke(const Json &proof) {
    const auto &input = proof.at("keyboard_input");

    need(input.at("numeric_empty") == "" && input.at("numeric_replaced") == "65",
         "synthetic Backspace and digits did not replace the numeric field");
    need(input.at("search") == "speed" && input.at("search_utf8") == "speed\xc3\xa9" &&
                 input.at("search_after_backspace") == "speed",
         "synthetic text input and Backspace did not edit the search field");
    const auto &fields = input.at("search_fields");
    need(std::find(fields.begin(), fields.end(), "speed") != fields.end(),
         "search did not keep the speed field");
    const auto &patches = proof.at("keyboard_input_patches");
    need(std::any_of(patches.begin(), patches.end(),
                     [](const Json &values) {
                         return values.value("speed", 0) == 65;
                     }),
         "edited speed was not committed to the cluster state");
}
} // namespace

int main(int argc, char **argv) {
    try {
        need(argc == 6 || argc == 7,
             "usage: window_sdl_test VIEWER ASSETS FONTS SPLASH PRIVATE_OUTPUT "
             "[input|keyboard|startup|screens]");
        const std::string mode = argc == 7 ? argv[6] : "";
        need(mode.empty() || mode == "input" || mode == "keyboard" || mode == "startup" ||
                     mode == "screens",
             "unknown smoke mode");
        const auto viewer = std::filesystem::canonical(argv[1]),
                   assets = std::filesystem::canonical(argv[2]),
                   fonts = std::filesystem::canonical(argv[3]),
                   splash = std::filesystem::canonical(argv[4]);
        const std::filesystem::path output = argv[5];
        ensure_private_directory(output);
        const auto id = random_token();
        const auto session = output / id;
        ensure_private_directory(session);
        // The screens mode adds a rear Android screen and a QNX HUD to the multimedia screens.
        const auto android = [](const char *role, unsigned width, unsigned height) {
            return Json{{"role", role},
                        {"guest", "android"},
                        {"width", width},
                        {"height", height},
                        {"density", 160}};
        };
        auto displays =
                Json::array({android("primary", 1920, 720), android("secondary", 1920, 720)});

        if (mode == "screens") {
            displays.push_back(android("rear", 1902, 540));
            displays.push_back(
                    {{"role", "hud"}, {"guest", "qnx"}, {"width", 1280}, {"height", 720}});
        }

        write_private_json(session / "media-view-owner.json",
                           media::readiness_owner(id, {{"screens", displays}}));
        write_private_json(
                session / "runtime.json",
                {{"session_id", id},
                 {"stopped", false},
                 {"deliveries", {{"android", {{"powered", true}, {"status", "waiting"}}}}}});

        if (mode.empty()) {
            // An SDK binding whose gRPC port refuses connections: the window retries the stream
            // and draws no error over the screens.
            unsigned refused = 47000;

            while (refused < 48000 && !tcp_port_free(refused)) {
                ++refused;
            }

            const auto descriptor = session / "pid_1.ini";
            std::ofstream(descriptor)
                    << "grpc.port=" << refused << "\nport.serial=5554\ngrpc.token=fixture\n";
            write_private_json(session / "media-connection.json",
                               {{"schema", 1},
                                {"session_id", id},
                                {"vm_pid", 1},
                                {"discovery", descriptor.string()},
                                {"grpc_port", refused},
                                {"serial_port", 5554}});
        }

        if (mode == "startup" || mode == "screens") {
            // At 50% each screen quad stays within what SDL's software renderer draws.
            write_private_json(session / "state.json", {{"schema", 1},
                                                        {"profile", "synthetic-host-ui-no-sdk"},
                                                        {"scale_percent", 50},
                                                        {"display", "none"}});
        }

        ProcessSpec spec;
        spec.executable = viewer;
        spec.arguments = {"--session",
                          session.string(),
                          "--splash",
                          splash.string(),
                          "--state",
                          (session / "state.json").string(),
                          "--endpoint",
                          (session / "absent-endpoint.json").string(),
                          "--assets",
                          assets.string(),
                          "--fonts",
                          fonts.string(),
                          "--profile",
                          "synthetic-host-ui-no-sdk",
                          "--with-cluster",
                          "--background",
                          "--capture",
                          "--seconds",
                          "20"};

        if (mode == "startup") {
            for (const auto &argument :
                 {std::string("--startup0"), startup_archive(session).string(),
                  std::string("--startup1"), std::string("black"), std::string("--startup-video0"),
                  startup_video(session, "display0", 64, 24,
                                {display_video_levels.begin(), display_video_levels.end()}, 0)
                          .string(),
                  std::string("--startup-video1"),
                  startup_video(session, "display1", 64, 24, {0}, 1).string(),
                  std::string("--startup-cluster"),
                  startup_video(session, "cluster", 128, 48,
                                {cluster_video_levels.begin(), cluster_video_levels.end()}, 0)
                          .string()}) {
                spec.arguments.push_back(argument);
            }
        } else if (mode == "screens") {
            for (const auto &argument :
                 {std::string("--input-smoke"), std::string("--startup-screen"),
                  std::string("rear"), startup_archive(session, "rear", 1902, 540).string(),
                  std::string("--startup-video-screen"), std::string("hud"),
                  startup_video(session, "hud", 128, 72, {90, 180}, 0, 8).string()}) {
                spec.arguments.push_back(argument);
            }
        } else if (!mode.empty()) {
            spec.arguments.push_back("--" + mode + "-smoke");
        }

        program_mode(viewer, "_window", spec.arguments);
        spec.directory = session;
        spec.output = session / "window.log";
        auto child = ChildProcess::launch(spec);

        if (mode == "input" || mode == "keyboard" || mode == "screens") {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);

            while (child.alive() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            need(!child.alive() && child.exit_code() == 0, "bounded smoke frontend failed");
            const auto proof = read_private_json(session / "media-native-proof/result.json",
                                                 1 << 20);

            if (mode == "input") {
                input_smoke(proof, session);
            } else if (mode == "screens") {
                input_smoke(proof, session);
                screens_smoke(proof, session);
            } else {
                keyboard_smoke(proof);
            }

            std::cout << "SDL3 " << mode << " smoke: synthetic SDL events passed with renderer "
                      << proof.at("renderer").get<std::string>() << "\n";

            return 0;
        }

        const auto splash_capture = session / "media-native-proof/splash.png";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);

        while (child.alive() && !std::filesystem::exists(splash_capture) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        need(child.alive() && std::filesystem::exists(splash_capture),
             "owned background frontend did not render its first frame");
        // The synthetic animation reaches its final frame 1.5 s after the 1 s video of its screen.
        std::this_thread::sleep_for(std::chrono::milliseconds(mode == "startup" ? 3500 : 500));
        const auto requested = std::chrono::steady_clock::now();
        write_private_json(session / "media-view-stop.json",
                           {{"schema", 1}, {"session_id", id}, {"frontend_pid", child.pid()}});

        while (child.alive() &&
               std::chrono::steady_clock::now() - requested < std::chrono::seconds(4)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        need(!child.alive() && child.exit_code() == 0,
             "graceful background frontend failed/timed out");
        const auto proof = read_private_json(session / "media-native-proof/result.json", 1 << 20);
        // SDL's offscreen driver focuses every shown window; real desktops must not.
        const char *driver = std::getenv("SDL_VIDEO_DRIVER");
        const bool offscreen = driver && std::string_view(driver) == "offscreen";
        // The language choice draws 中文 with the Chinese face of the machine fonts.
        need(proof.at("chinese_glyph") == true, "the label font must draw Chinese");
        need(proof.at("touches_released") == true && proof.at("background") == true &&
                     proof.at("activation_requested") == false &&
                     (offscreen || proof.at("first_show").at("window_foreground") == false),
             "release/background proof failed");
        const unsigned zoom = mode == "startup" ? 50 : 100;
        need(proof.at("zoom") == zoom && !proof.contains("native_menu"),
             "captured window state differs");
        need(proof.at("received_frames") == Json::array({0, 0}) && proof.at("ready") == false &&
                     proof.at("input_rpc_acknowledgements") == 0,
             "synthetic fixture unexpectedly claimed SDK activity");
        need(mode == "startup" ||
                     (proof.at("error") == "" && proof.at("stream_failures").get<unsigned>() > 0),
             "a refused SDK stream must be retried without an error");
        need(read_private_json(session / "state.json", 4096).at("scale_percent") == zoom,
             "placement and scale were not persisted");
        const auto capture = session / "media-native-proof/combined-final.png";
        need(std::filesystem::is_regular_file(capture) &&
                     std::filesystem::file_size(capture) > 1000,
             "actual renderer PNG missing");

        if (mode == "startup") {
            startup_animation(session, proof);
            startup_videos(session, proof);
        } else {
            startup_mask(capture, proof);
        }

        std::cout << "Background SDL3 canvas: " << (mode == "startup" ? "boot animation" : "splash")
                  << ", final capture, persisted placement and graceful stop passed with renderer "
                  << proof.at("renderer").get<std::string>()
                  << "; synthetic host fixture, no OEM streaming proof\n";

        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
