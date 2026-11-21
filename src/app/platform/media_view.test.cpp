#include "media_view.hpp"
#include "media_readiness.hpp"
#include "os/process.hpp"
#include "process.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>
using namespace voyemu;
using namespace voyemu::media;
namespace {
template <class F> void wait(F ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (!ready()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

template <class F> void rejects(F action) {
    bool caught = false;

    try {
        action();
    } catch (const std::exception &) {
        caught = true;
    }

    assert(caught);
}
} // namespace

int main(int argc, char **argv) try {
    assert(argc == 2);
    const auto base = std::filesystem::path(argv[1]) / ("media-view-worker-" + random_token());
    const auto id = random_token();
    const auto session = base / id;
    ensure_private_directory(session);
    std::vector<std::string> args{"_window",
                                  "--session",
                                  session.string(),
                                  "--splash",
                                  (base / "splash.png").string(),
                                  "--state",
                                  (base / "state.json").string(),
                                  "--endpoint",
                                  (session / "endpoint").string(),
                                  "--assets",
                                  (base / "assets").string(),
                                  "--fonts",
                                  (base / "fonts").string(),
                                  "--profile",
                                  "fixture",
                                  "--with-cluster",
                                  "--background",
                                  "--capture",
                                  "--seconds",
                                  "2"};
    std::vector<const char *> values;

    for (const auto &value : args) {
        values.push_back(value.c_str());
    }

    assert(media_view_arguments(int(values.size()), values.data()));
    auto options = media_view_options(int(values.size()), values.data());
    assert(options.background && options.capture && options.with_cluster && options.seconds == 2 &&
           options.session == session);
    assert(!options.input_smoke && !options.keyboard_smoke && !options.startup_cluster &&
           !options.startup_video[0] && !options.startup_video[1]);

    // Each startup video needs an absolute path.
    for (const std::string video : {(base / "video.zip").string(), std::string("video.zip")}) {
        auto with_video = args;

        for (const auto *flag : {"--startup-cluster", "--startup-video0", "--startup-video1"}) {
            with_video.push_back(flag);
            with_video.push_back(video);
        }

        std::vector<const char *> values;

        for (const auto &value : with_video) {
            values.push_back(value.c_str());
        }

        if (std::filesystem::path(video).is_absolute()) {
            const auto parsed = media_view_options(int(values.size()), values.data());
            assert(parsed.startup_cluster == std::filesystem::path(video) &&
                   parsed.startup_video[0] == std::filesystem::path(video) &&
                   parsed.startup_video[1] == std::filesystem::path(video));
        } else {
            rejects([&] {
                media_view_options(int(values.size()), values.data());
            });
        }
    }

    // The screens window takes a boot animation or black and a startup video by role.
    {
        auto extra = args;

        for (const auto &value :
             {std::string("--startup-screen"), std::string("rear"), std::string("black"),
              std::string("--startup-screen"), std::string("hud"), (base / "hud.zip").string(),
              std::string("--startup-video-screen"), std::string("hud"),
              (base / "hud-video.zip").string()}) {
            extra.push_back(value);
        }

        std::vector<const char *> values;

        for (const auto &value : extra) {
            values.push_back(value.c_str());
        }

        const auto parsed = media_view_options(int(values.size()), values.data());
        assert(parsed.startup_screens.at("rear").empty() &&
               parsed.startup_screens.at("hud") == base / "hud.zip" &&
               parsed.startup_video_screens.at("hud") == base / "hud-video.zip");

        for (const auto &wrong :
             {std::vector<std::string>{"--startup-screen", "primary", "black"},
              std::vector<std::string>{"--startup-screen", "tertiary", "black"},
              std::vector<std::string>{"--startup-video-screen", "hud", "black"},
              std::vector<std::string>{"--startup-screen", "rear", "x.zip"}}) {
            auto invalid = args;
            invalid.insert(invalid.end(), wrong.begin(), wrong.end());
            values.clear();

            for (const auto &value : invalid) {
                values.push_back(value.c_str());
            }

            rejects([&] {
                media_view_options(int(values.size()), values.data());
            });
        }
    }

    // Session frontends with input_smoke pass --input-smoke; the keyboard smoke needs a bounded
    // capture.
    for (const auto *flag : {"--input-smoke", "--keyboard-smoke"}) {
        auto smoke = args;
        smoke.push_back(flag);
        std::vector<const char *> values;

        for (const auto &value : smoke) {
            values.push_back(value.c_str());
        }

        const auto parsed = media_view_options(int(values.size()), values.data());
        assert(parsed.input_smoke == (std::string_view(flag) == "--input-smoke") &&
               parsed.keyboard_smoke == (std::string_view(flag) == "--keyboard-smoke"));
        smoke.erase(std::find(smoke.begin(), smoke.end(), "--capture"));
        values.clear();

        for (const auto &value : smoke) {
            values.push_back(value.c_str());
        }

        if (std::string_view(flag) == "--input-smoke") {
            assert(media_view_options(int(values.size()), values.data()).input_smoke);
        } else {
            rejects([&] {
                media_view_options(int(values.size()), values.data());
            });
        }
    }

    auto multimedia_args = args;

    for (auto &argument : multimedia_args) {
        if (argument == "--with-cluster") {
            argument = "--no-cluster";
        }
    }

    std::vector<const char *> multimedia_values;

    for (const auto &value : multimedia_args) {
        multimedia_values.push_back(value.c_str());
    }

    assert(!media_view_options(int(multimedia_values.size()), multimedia_values.data())
                    .with_cluster);

    for (const auto *flag : {"--assets", "--fonts", "--endpoint"}) {
        auto missing = multimedia_args;
        const auto at = std::find(missing.begin(), missing.end(), flag);
        missing.erase(at, at + 2);
        std::vector<const char *> values;

        for (const auto &value : missing) {
            values.push_back(value.c_str());
        }

        rejects([&] {
            media_view_options(int(values.size()), values.data());
        });
    }

    auto bad = args;
    bad.back() = "121";
    values.clear();

    for (const auto &value : bad) {
        values.push_back(value.c_str());
    }

    rejects([&] {
        media_view_options(int(values.size()), values.data());
    });
    rejects([&] {
        MediaView missing(options);
    });
    // A machine with a rear screen: its frames and touches use its own display and size.
    const Json displays = Json::array({{{"role", "primary"},
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
    const GuestDisplay primary{0, 1920, 720}, secondary{1, 1920, 720};
    write_private_json(session / "media-view-owner.json",
                       readiness_owner(id, {{"screens", displays}}));
    assert(media_view_screens(session).size() == 3 &&
           media_view_screens(session)[2].android_display == 2u);
    auto runtime = [&](bool ready, bool powered = true, std::string epoch = {}) {
        write_private_json(
                session / "runtime.json",
                {{"session_id", epoch.empty() ? id : epoch},
                 {"stopped", false},
                 {"deliveries",
                  {{"android",
                    {{"powered", powered},
                     {"status", ready ? "guest_acknowledged" : "waiting"},
                     {"home", {{"status", ready ? "visible_on_primary" : "waiting"}}},
                     {"displays",
                      {{"status", ready ? "both_guest_displays_ready" : "waiting"}}}}}}}});
    };
    runtime(false);
    write_private_json(session / "media-connection.json", {{"fixture", true}});
    std::atomic<unsigned> streaming{};
    std::mutex mutex;
    std::vector<std::pair<std::string, Bytes>> sent;
    MediaViewTransports transport;
    transport.connection = [&](const auto &path, std::string_view epoch) {
        assert(path == session / "media-connection.json" && epoch == id);

        return Connection{45991, "owned-fixture", session / "media-frames"};
    };
    transport.stream = [&](const Connection &connection, GuestDisplay display,
                           const std::atomic<bool> &stop, std::function<void(Image)> receiver) {
        assert(connection.port == 45991 && display.id < 3);
        ++streaming;
        unsigned sequence = 0;

        while (!stop.load()) {
            Image frame;
            frame.display = display.id;
            frame.width = display.width;
            frame.height = display.height;
            frame.sequence = ++sequence;
            frame.timestamp_us = sequence;
            frame.storage.resize(display.pixel_bytes(), static_cast<std::uint8_t>(display.id + 1));
            receiver(std::move(frame));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        --streaming;
    };
    transport.input = [&](const Connection &connection, std::string_view method,
                          std::span<const std::uint8_t> payload) {
        assert(connection.port == 45991);
        std::lock_guard lock(mutex);
        sent.emplace_back(method, Bytes(payload.begin(), payload.end()));
    };
    MediaView media(options, transport);
    wait([&] {
        auto state = media.snapshot();

        return state.received[0] && state.received[1];
    });
    assert(!media.snapshot().ready && !media.snapshot().frames[0] && !media.press({0, 10, 20}));
    runtime(true);
    wait([&] {
        return media.snapshot().ready;
    });
    auto state = media.snapshot();
    assert(state.frames[0]->display == 0 && state.frames[1]->display == 1 &&
           state.frames[0]->pixels()[0] == 1 && state.frames[1]->pixels()[0] == 2);
    wait([&] {
        return bool(media.snapshot().frames[2]);
    });
    assert(media.snapshot().frames[2]->width == 1902 && media.screens()[2].role == "rear");
    assert(media.press({0, 10, 20}));
    wait([&] {
        return media.snapshot().input_acknowledgements >= 1;
    });
    assert(!media.snapshot().touches_released);
    assert(media.move({0, 11, 21}));
    media.release();
    wait([&] {
        return media.snapshot().touches_released;
    });
    {
        std::lock_guard lock(mutex);
        assert(sent.front().first == "sendTouch" &&
               sent.front().second == touch_request(primary, 10, 20, true));
        assert(sent.back().second == touch_request(primary, 11, 21, false));
    }
    // The rear screen takes touches in its own pixels; another size or display is rejected.
    rejects([&] {
        media.press({2, 1902, 0});
    });
    rejects([&] {
        media.press({3, 0, 0});
    });
    assert(media.press({2, 1901, 539}));
    media.release();
    wait([&] {
        std::lock_guard lock(mutex);

        return sent.back().second == touch_request({2, 1902, 540}, 1901, 539, false);
    });
    auto acknowledgements = media.snapshot().input_acknowledgements;
    assert(media.key("Home"));
    assert(media.key("тест", true));
    wait([&] {
        return media.snapshot().input_acknowledgements >= acknowledgements + 2;
    });
    assert(media.press({1, 30, 40}));
    wait([&] {
        return !media.snapshot().touches_released;
    });
    assert(!media.move({0, 1, 1}));
    wait([&] {
        return media.snapshot().touches_released;
    });
    assert(media.press({1, 50, 60}));
    wait([&] {
        return !media.snapshot().touches_released;
    });
    runtime(true, false);
    wait([&] {
        return !media.snapshot().powered && media.snapshot().touches_released;
    });
    assert(!media.snapshot().ready && !media.snapshot().frames[0]);
    wait([&] {
        return streaming == 0;
    });
    runtime(true, true);
    wait([&] {
        return media.snapshot().ready;
    });
    media.visible(false);
    wait([&] {
        return streaming == 0;
    });
    auto received = media.snapshot().received;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(media.snapshot().received == received);
    media.visible(true);
    wait([&] {
        return streaming == 3;
    });
    auto stop_request = [&](std::string epoch, std::uint64_t pid) {
        write_private_json(session / "media-view-stop.json",
                           {{"schema", 1}, {"session_id", epoch}, {"frontend_pid", pid}});
    };
    stop_request("different-epoch", std::uint64_t(os::current_process()));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    assert(!media.snapshot().stopped);
    stop_request(id, std::uint64_t(os::current_process()) + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    assert(!media.snapshot().stopped);
    assert(media.press({1, 70, 80}));
    wait([&] {
        return !media.snapshot().touches_released;
    });
    stop_request(id, std::uint64_t(os::current_process()));
    wait([&] {
        return media.snapshot().stopped;
    });
    media.stop();
    assert(media.snapshot().touches_released && streaming == 0);
    {
        std::lock_guard lock(mutex);
        assert(sent.back().second == touch_request(secondary, 70, 80, false));
    }
    std::filesystem::remove(session / "media-view-stop.json");
    runtime(true, true, "different-epoch");
    MediaView stale(options, transport);
    wait([&] {
        return stale.snapshot().stopped;
    });
    stale.stop();
    assert(stale.snapshot().error.starts_with("MEDIA_OWNER:"));
    runtime(true);
    auto failing_transport = transport;
    failing_transport.input = [](const Connection &, std::string_view,
                                 std::span<const std::uint8_t>) {
        throw std::runtime_error("MEDIA_RPC_UNKNOWN: fixture rejects input and release");
    };
    MediaView failing(options, failing_transport);
    wait([&] {
        return failing.snapshot().ready;
    });
    assert(failing.press({0, 12, 34}));
    wait([&] {
        return !failing.snapshot().error.empty();
    });
    failing.stop();
    assert(!failing.snapshot().touches_released && failing.snapshot().input_acknowledgements == 0);
    // An SDK that refuses the first streams, as before its gRPC listener accepts or while it
    // restarts, is retried until it streams; the refusals leave no error in the view state. The
    // SDK refuses only attempts opened after the view became ready: readiness cancels the open
    // attempts, the streaming one of display 1 among them, before display 1 returns, and the
    // refusal of an attempt the view cancelled is no stream failure.
    runtime(false);
    std::atomic<unsigned> refusals{};
    std::atomic<bool> refreshed{};
    auto refusing_transport = transport;
    refusing_transport.stream = [&](const Connection &connection, GuestDisplay display,
                                    const std::atomic<bool> &stop,
                                    std::function<void(Image)> receiver) {
        if (display.id == 1) {
            transport.stream(connection, display, stop, std::move(receiver));
            refreshed = true;

            return;
        }

        while (display.id == 0 && !refreshed && !stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (display.id == 0 && stop) {
            return;
        }

        if (display.id == 0 && refusals < 3) {
            ++refusals;
            throw std::runtime_error("MEDIA_RPC_UNKNOWN: local SDK request did not complete "
                                     "method=streamScreenshot curl=7 http=0");
        }

        transport.stream(connection, display, stop, std::move(receiver));
    };
    MediaView refusing(options, refusing_transport);
    wait([&] {
        return refusing.snapshot().received[1] > 0;
    });
    runtime(true);
    wait([&] {
        const auto state = refusing.snapshot();

        return state.ready && state.frames[0] && state.frames[1];
    });
    assert(refusals == 3 && refusing.snapshot().error.empty() &&
           refusing.snapshot().stream_failures == 3);
    refusing.stop();

    // Consecutive refusals back off from 100 ms to at most 2 s.
    for (const auto &[failures, delay] : std::vector<std::pair<unsigned, unsigned>>{
                 {0, 100}, {1, 100}, {2, 200}, {3, 400}, {5, 1600}, {6, 2000}, {1000, 2000}}) {
        assert(stream_retry_delay(failures) == std::chrono::milliseconds(delay));
    }

    runtime(false);
    auto static_transport = transport;
    static_transport.stream = [](const Connection &, GuestDisplay display,
                                 const std::atomic<bool> &stop,
                                 std::function<void(Image)> receiver) {
        Image frame;

        frame.display = display.id;
        frame.width = display.width;
        frame.height = display.height;
        frame.sequence = 1;
        frame.timestamp_us = 1;
        frame.storage.resize(display.pixel_bytes(), static_cast<std::uint8_t>(display.id + 3));
        receiver(std::move(frame));

        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    MediaView static_scene(options, static_transport);
    wait([&] {
        const auto state = static_scene.snapshot();

        return state.received[0] && state.received[1];
    });
    assert(!static_scene.snapshot().ready);
    runtime(true);
    wait([&] {
        return static_scene.snapshot().ready;
    });
    assert(static_scene.snapshot().received[0] >= 2 && static_scene.snapshot().received[1] >= 2);
    runtime(false);
    wait([&] {
        return !static_scene.snapshot().ready;
    });
    const auto prior = static_scene.snapshot().received;
    runtime(true);
    wait([&] {
        const auto state = static_scene.snapshot();

        return state.ready && state.received[0] > prior[0] && state.received[1] > prior[1];
    });
    static_scene.stop();
    std::filesystem::remove_all(base);
    std::cout
            << "Media view workers: ready-gated dual frames, owned RPC binding, input release, power/visibility cancellation and stale epoch guards passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
