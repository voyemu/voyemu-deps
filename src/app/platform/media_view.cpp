#include "media_view.hpp"
#include "check.hpp"
#include "error.hpp"
#include "logging.hpp"
#include "media_capture.hpp"
#include "media_readiness.hpp"
#include "os/process.hpp"
#include "passion_native_power.hpp"
#include "session.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

namespace voyemu::media {
bool media_view_arguments(int argc, const char *const *argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--session") {
            return true;
        }
    }

    return false;
}

MediaViewOptions media_view_options(int argc, const char *const *argv) {
    MediaViewOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        auto value = [&]() {
            if (++i >= argc) {
                throw VoyemuError("MEDIA_ARGUMENT", "missing value");
            }

            return std::string(argv[i]);
        };

        if (name == "--session") {
            options.session = value();
        } else if (name == "--splash") {
            options.splash = value();
        } else if (name == "--startup0" || name == "--startup1") {
            const auto text = value();
            const std::filesystem::path path = text == "black" ? "" : text;

            if (!path.empty() && !path.is_absolute()) {
                throw VoyemuError("MEDIA_ARGUMENT", "absolute startup animation required");
            }

            options.startup[name == "--startup1"] = path;
        } else if (name == "--startup-cluster" || name == "--startup-video0" ||
                   name == "--startup-video1") {
            const std::filesystem::path path = value();

            if (!path.is_absolute()) {
                throw VoyemuError("MEDIA_ARGUMENT", "absolute startup video required");
            }

            (name == "--startup-cluster"
                     ? options.startup_cluster
                     : options.startup_video[name == "--startup-video1"]) = path;
        } else if (name == "--startup-screen" || name == "--startup-video-screen") {
            const auto role = value();
            const auto text = value();
            const bool video = name == "--startup-video-screen";
            const std::filesystem::path path = !video && text == "black" ? "" : text;
            const bool known = std::any_of(screen_roles.begin(), screen_roles.end(),
                                           [&](const auto &entry) {
                                               return entry.first == role;
                                           });

            if (!known || multimedia_screen(role)) {
                throw VoyemuError("MEDIA_ARGUMENT", "screen of the screens window required");
            }

            if ((video || !path.empty()) && !path.is_absolute()) {
                throw VoyemuError("MEDIA_ARGUMENT", "absolute startup animation required");
            }

            (video ? options.startup_video_screens : options.startup_screens)[role] = path;
        } else if (name == "--state") {
            options.state = value();
        } else if (name == "--endpoint") {
            options.endpoint = value();
        } else if (name == "--assets") {
            options.assets = value();
        } else if (name == "--fonts") {
            options.fonts = value();
        } else if (name == "--profile") {
            options.profile = value();
        } else if (name == "--seconds") {
            const auto text = value();
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                                options.seconds);

            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                !options.seconds || options.seconds > 120) {
                throw VoyemuError("MEDIA_ARGUMENT", "expected 1..120 seconds");
            }
        } else if (name == "--inspect-panel") {
            const auto text = value();

            if (text.size() != 1 || text[0] < '0' || text[0] > '4') {
                throw VoyemuError("WINDOW_INSPECT_PANEL", "expected 0..4");
            }

            options.inspect_panel = std::size_t(text[0] - '0');
        } else if (name == "--inspect-language") {
            const auto text = value();

            if (text != "en" && text != "ru" && text != "zh") {
                throw VoyemuError("WINDOW_INSPECT_LANGUAGE", "expected en, ru or zh");
            }

            options.inspect_language = ui::language_from_tag(text);
        } else if (name == "--background") {
            options.background = true;
        } else if (name == "--capture") {
            options.capture = true;
        } else if (name == "--input-smoke") {
            options.input_smoke = true;
        } else if (name == "--keyboard-smoke") {
            options.keyboard_smoke = true;
        } else if (name == "--with-cluster") {
            options.with_cluster = true;
        } else if (name == "--no-cluster") {
            options.with_cluster = false;
        } else {
            throw VoyemuError("MEDIA_ARGUMENT", "unknown window option");
        }
    }

    const auto id = options.session.filename().string();

    if (!options.session.is_absolute() || options.session != options.session.lexically_normal() ||
        !hex_text(id, 32) || !options.splash.is_absolute() || !options.state.is_absolute() ||
        options.profile.empty()) {
        throw VoyemuError("MEDIA_ARGUMENT", "explicit private paths and owned epoch required");
    }

    if (!options.endpoint.is_absolute() || !options.assets.is_absolute() ||
        !options.fonts.is_absolute()) {
        throw VoyemuError("WINDOW_CLUSTER", "explicit owned endpoint, assets and fonts required");
    }

    if ((options.keyboard_smoke || options.inspect_panel || options.inspect_language) &&
        (!options.capture || !options.seconds)) {
        throw VoyemuError("WINDOW_INPUT_SMOKE", "bounded own capture required");
    }

    return options;
}

std::chrono::milliseconds stream_retry_delay(unsigned failures) {
    const unsigned doublings = failures > 1 ? std::min(failures - 1, 5u) : 0;

    return std::min(std::chrono::milliseconds(100 << doublings), std::chrono::milliseconds(2000));
}

std::vector<Screen> media_view_screens(const std::filesystem::path &session) {
    return owner_screens(read_private_json(session / "media-view-owner.json", 4096),
                         session.filename().string());
}

struct MediaView::Impl {
    enum class Kind {
        down,
        move,
        up,
        key,
        cancel
    };
    struct Input {
        Kind kind;
        TouchPoint point{};
        std::string key;
        bool text{};
    };
    MediaViewOptions options;
    MediaViewTransports transport;
    std::string id;
    std::vector<Screen> screens;
    // The Android screens as emulator displays; the multimedia screens come first.
    std::vector<GuestDisplay> displays;
    bool require_oem{}, presentation_only_mode{};
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> stopped{}, shown{true};
    CaptureDemand demand;
    PresentationFrames<std::shared_ptr<const Image>> frames;
    MediaViewSnapshot state;
    std::deque<Input> inputs;
    std::optional<TouchPoint> pointer;
    std::vector<std::thread> workers;
    std::unique_ptr<PassionNativePower> display_power;
    std::atomic<bool> power_recheck{};

    Impl(MediaViewOptions value, MediaViewTransports calls)
        : options(std::move(value)), transport(std::move(calls)),
          id(options.session.filename().string()) {
        const auto owner = read_private_json(options.session / "media-view-owner.json", 4096);
        require_oem = require_oem_systemui(owner, id);
        presentation_only_mode = presentation_only(owner, id);
        screens = owner_screens(owner, id);
        std::size_t multimedia = 0;

        for (const auto &screen : screens) {
            if (screen.android_display) {
                displays.push_back({*screen.android_display, screen.width, screen.height});
                multimedia += multimedia_screen(screen.role);
            }
        }

        frames = PresentationFrames<std::shared_ptr<const Image>>(displays.size(), multimedia);
        state.frames.resize(displays.size());
        state.received.resize(displays.size());

        if (!transport.connection) {
            transport.connection = read_connection;
        }

        if (!transport.stream) {
            transport.stream = [](const Connection &connection, GuestDisplay display,
                                  const std::atomic<bool> &stop,
                                  std::function<void(Image)> receiver) {
                const char *transport = std::getenv("VOYEMU_MEDIA_TRANSPORT");
                ScreenshotStream stream(connection, display,
                                        transport && std::string_view(transport) == "mmap");
                stream.run(stop, std::move(receiver));
            };
        }

        if (!transport.input) {
            transport.input = [](const Connection &connection, std::string_view method,
                                 std::span<const std::uint8_t> payload) {
                send_input(connection, method, payload);
            };
        }

        // A touch on a powered-off OEM screen of a Passion machine wakes it.
        if (owner.value("presentation_family", std::string{}) == "passion") {
            display_power = std::make_unique<PassionNativePower>(
                    id,
                    [this](unsigned display, const std::string &boot) {
                        return voyemu::request(options.endpoint,
                                               {{"op", "media_screen"},
                                                {"session_id", id},
                                                {"boot_id", boot},
                                                {"action", "wake"},
                                                {"display_id", display}},
                                               4500);
                    },
                    [this] {
                        power_recheck = true;
                        wake.notify_all();
                    },
                    []() {
                        return static_cast<std::uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
                    });
        }

        try {
            workers.emplace_back([this] {
                monitor();
            });

            for (const auto display : displays) {
                workers.emplace_back([this, display] {
                    receive(display);
                });
            }

            workers.emplace_back([this] {
                send();
            });
        } catch (...) {
            stop();
            throw;
        }
    }

    void error(const std::exception &value) {
        std::lock_guard lock(mutex);

        if (state.error == value.what()) {
            return;
        }

        state.error = value.what();
        logging::write(logging::Level::error, "media", state.error);
    }

    Connection connection() {
        return transport.connection(options.session / "media-connection.json", id);
    }

    void pause(std::chrono::milliseconds delay = std::chrono::milliseconds(100)) {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, delay, [&] {
            return stopped.load();
        });
    }

    void monitor() {
        while (!stopped.load()) {
            // Only this owned frontend epoch may request graceful shutdown. Invalid
            // or stale requests never prevent normal runtime/stream monitoring.
            try {
                const auto path = options.session / "media-view-stop.json";

                if (std::filesystem::exists(path)) {
                    const auto request = read_private_json(path, 4096);

                    if (request.value("schema", 0) == 1 &&
                        request.value("session_id", std::string{}) == id &&
                        request.value("frontend_pid", std::uint64_t{}) ==
                                std::uint64_t(os::current_process())) {
                        stopped = true;
                        break;
                    }
                }
            } catch (const std::exception
                             &) { /* Ignored: an unreadable stop request is polled again. */
            }

            try {
                if (std::filesystem::exists(options.session / "runtime.json")) {
                    const auto runtime = read_private_json(options.session / "runtime.json");

                    if (runtime.at("session_id") != id) {
                        std::runtime_error stale("MEDIA_OWNER: runtime session changed");
                        error(stale);
                        stopped = true;
                        break;
                    }

                    if (display_power) {
                        display_power->observe(runtime);

                        if (power_recheck.exchange(false)) {
                            const auto boot = display_power->boot();

                            if (!boot.empty()) {
                                const auto result = voyemu::request(options.endpoint,
                                                                    {{"op", "media_screen"},
                                                                     {"session_id", id},
                                                                     {"boot_id", boot},
                                                                     {"action", "recheck"}},
                                                                    300);

                                if (!result.value("ok", false)) {
                                    logging::write(logging::Level::debug, "media",
                                                   "display_power.recheck_pending");
                                }
                            }
                        }
                    }

                    const auto android = runtime.at("deliveries").at("android");
                    const bool powered = android.value("powered", true),
                               ready = powered && presentation_ready(android, require_oem,
                                                                     presentation_only_mode);
                    {
                        std::lock_guard lock(mutex);
                        state.powered = powered;
                        state.ready = ready;
                        set_presentation_ready(frames, demand, ready);

                        if (!ready && (pointer || !state.touches_released || !inputs.empty())) {
                            inputs.clear();
                            pointer.reset();
                            inputs.push_back({Kind::cancel, {}, {}});
                            wake.notify_all();
                        }
                    }

                    if (runtime.value("stopped", false)) {
                        stopped = true;
                        break;
                    }
                }
            } catch (const std::exception &value) {
                error(value);
            }

            bool complete, powered;
            {
                std::lock_guard lock(mutex);
                complete = frames.complete();
                powered = state.powered;
            }
            demand.set_wanted(powered && (shown.load() || !complete));
            pause();
        }

        demand.stop();
        wake.notify_all();
    }

    // The emulator display of a touch, at the size of its screen.
    GuestDisplay guest(unsigned display) const {
        for (const auto &candidate : displays) {
            if (candidate.id == display) {
                return candidate;
            }
        }

        throw VoyemuError("MEDIA_DISPLAY", "expected an Android screen of the machine");
    }

    void receive(GuestDisplay display) {
        unsigned failures = 0;

        while (!stopped.load()) {
            auto attempt = demand.wait();

            if (!attempt) {
                break;
            }

            // The session publishes the binding of each SDK it starts after the SDK listens.
            if (!std::filesystem::exists(options.session / "media-connection.json")) {
                failures = 0;
                pause();
                continue;
            }

            bool received = false;

            try {
                const auto owned = connection();
                transport.stream(owned, display, *attempt, [&](Image image) {
                    if (image.display != display.id || image.width != display.width ||
                        image.height != display.height ||
                        image.pixel_offset > image.storage.size() ||
                        image.storage.size() - image.pixel_offset < display.pixel_bytes()) {
                        throw VoyemuError("MEDIA_PROTO", "incomplete display frame");
                    }

                    std::lock_guard lock(mutex);

                    if (attempt->load()) {
                        return;
                    }

                    received = true;
                    ++state.received[display.id];
                    frames.put(display.id, std::make_shared<Image>(std::move(image)));
                });
                failures = 0;
            } catch (const std::exception &value) {
                if (!attempt->load() && !stopped.load()) {
                    failures = received ? 1 : failures + 1;
                    const auto delay = stream_retry_delay(failures);
                    {
                        std::lock_guard lock(mutex);
                        ++state.stream_failures;
                    }
                    logging::write(logging::Level::debug, "media", "stream.retry",
                                   {{"display", display.id},
                                    {"failures", failures},
                                    {"delay_ms", delay.count()},
                                    {"reason", value.what()}});
                    pause(delay);
                    continue;
                }
            }

            pause();
        }
    }

    bool enqueue(Input input) {
        if (stopped.load()) {
            return false;
        }

        if (input.kind == Kind::move && !inputs.empty() && inputs.back().kind == Kind::move) {
            inputs.back() = std::move(input);

            return true;
        }

        if (inputs.size() >= 64) {
            std::erase_if(inputs, [](const Input &item) {
                return item.kind == Kind::move;
            });

            if (inputs.size() >= 64) {
                state.error = "MEDIA_INPUT: bounded input queue full";

                return false;
            }
        }

        inputs.push_back(std::move(input));
        wake.notify_all();

        return true;
    }

    void release_locked() {
        if (!pointer) {
            return;
        }

        // Release is never lost behind a saturated keyboard queue; the worker uses its actual held
        // point.
        if (inputs.size() >= 64) {
            inputs.clear();
        }

        inputs.push_back({Kind::up, *pointer, {}});
        pointer.reset();
        wake.notify_all();
    }

    void send() {
        std::optional<TouchPoint> held;
        std::optional<Connection> owned;
        auto release = [&] {
            if (!held) {
                return;
            }

            if (!owned) {
                owned = connection();
            }

            transport.input(*owned, "sendTouch",
                            touch_request(guest(held->display), held->x, held->y, false));
            held.reset();
            std::lock_guard lock(mutex);
            state.touches_released = true;
            ++state.input_acknowledgements;
        };

        while (!stopped.load()) {
            std::optional<Input> input;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    return stopped.load() || !inputs.empty();
                });

                if (!stopped.load() && !inputs.empty()) {
                    input = std::move(inputs.front());
                    inputs.pop_front();
                }
            }

            if (!input) {
                continue;
            }

            if (input->kind == Kind::cancel) {
                try {
                    if (display_power) {
                        display_power->cancel(release);
                    } else {
                        release();
                    }
                } catch (const std::exception &value) {
                    error(value);
                }

                continue;
            }

            if (input->kind != Kind::key && display_power) {
                const auto phase = input->kind == Kind::down   ? PassionPointerWake::Phase::down
                                   : input->kind == Kind::move ? PassionPointerWake::Phase::move
                                                               : PassionPointerWake::Phase::up;
                try {
                    if (display_power->before(phase, input->point.display) ==
                        PassionPointerWake::Result::consumed) {
                        std::lock_guard lock(mutex);

                        if (input->kind == Kind::down) {
                            ++state.display_wake_gestures;
                        }

                        state.touches_released = !held;
                        continue;
                    }
                } catch (const std::exception &value) {
                    error(value);
                    continue;
                }
            }

            try {
                if (input->kind == Kind::up) {
                    release();
                    continue;
                }

                {
                    std::lock_guard lock(mutex);

                    if (!state.ready || !state.powered) {
                        continue;
                    }
                }
                owned = connection();

                if (input->kind == Kind::key) {
                    transport.input(*owned, "sendKey", key_request(input->key, input->text));
                } else {
                    held = input->point;
                    {
                        std::lock_guard lock(mutex);
                        state.touches_released = false;
                    }
                    transport.input(*owned, "sendTouch",
                                    touch_request(guest(held->display), held->x, held->y, true));
                }

                {
                    std::lock_guard lock(mutex);
                    ++state.input_acknowledgements;
                }
            } catch (const std::exception &value) {
                error(value);
                try {
                    release();
                } catch (const std::exception &cleanup) {
                    error(cleanup);
                }
            }
        }

        try {
            release();
        } catch (const std::exception &value) {
            error(value);
        }
    }

    void stop() {
        stopped = true;
        demand.stop();
        wake.notify_all();

        for (auto &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
};

MediaView::MediaView(MediaViewOptions options, MediaViewTransports transports)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(transports))) {}

MediaView::~MediaView() {
    impl_->stop();
}

MediaViewSnapshot MediaView::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    auto state = impl_->state;

    state.frames = impl_->frames.snapshot();
    state.ready = state.ready && impl_->frames.complete();
    state.stopped = impl_->stopped.load();

    if (impl_->display_power) {
        state.off = impl_->display_power->off_displays();
    }

    return state;
}

const std::vector<Screen> &MediaView::screens() const {
    return impl_->screens;
}

void MediaView::visible(bool value) {
    impl_->shown = value;
}

bool MediaView::press(TouchPoint point) {
    touch_request(impl_->guest(point.display), point.x, point.y, true);
    std::lock_guard lock(impl_->mutex);

    if (!impl_->frames.complete()) {
        return false;
    }

    impl_->release_locked();

    if (!impl_->enqueue({Impl::Kind::down, point, {}})) {
        return false;
    }

    impl_->pointer = point;

    return true;
}

bool MediaView::move(TouchPoint point) {
    touch_request(impl_->guest(point.display), point.x, point.y, true);
    std::lock_guard lock(impl_->mutex);

    if (!impl_->pointer) {
        return false;
    }

    if (impl_->pointer->display != point.display) {
        impl_->release_locked();

        return false;
    }

    impl_->pointer = point;

    return impl_->enqueue({Impl::Kind::move, point, {}});
}

void MediaView::release() {
    std::lock_guard lock(impl_->mutex);

    impl_->release_locked();
}

bool MediaView::key(std::string value, bool text) {
    key_request(value, text);
    std::lock_guard lock(impl_->mutex);

    if (!impl_->frames.complete()) {
        return false;
    }

    return impl_->enqueue({Impl::Kind::key, {}, std::move(value), text});
}

void MediaView::stop() {
    impl_->stop();
}
} // namespace voyemu::media
