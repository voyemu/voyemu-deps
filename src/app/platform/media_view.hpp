#pragma once
#include "media_rpc.hpp"
#include "media_window.hpp"
#include "model.hpp"
#include "screens.hpp"
#include "ui_localization.hpp"
#include <array>
#include <chrono>
#include <map>
#include <memory>

namespace voyemu::media {
struct MediaViewOptions {
    std::filesystem::path session, splash, state, endpoint, assets;
    // The car fonts of the machine, ui/fonts of its resources.
    std::filesystem::path fonts;
    // OEM boot animation per screen until Android is ready: a bootanimation.zip, an empty path for
    // black, or none for the Voyemu logo.
    std::array<std::optional<std::filesystem::path>, 2> startup;
    // OEM startup videos that preparation stored as boot animation archives, played once from the
    // start of the startup phase: one per multimedia screen before its boot animation, and the
    // cluster one in the cluster area.
    std::array<std::optional<std::filesystem::path>, 2> startup_video;
    std::optional<std::filesystem::path> startup_cluster;
    // The same for the screens of the screens window, by role: a boot animation or an empty path
    // for black, and a startup video before it.
    std::map<std::string, std::filesystem::path> startup_screens, startup_video_screens;
    std::string profile;
    unsigned seconds{};
    // Bounded own captures may select a parameters panel and UI language.
    std::optional<std::size_t> inspect_panel;
    std::optional<ui::Language> inspect_language;
    // Synthetic SDL input: panel and Android clicks, or cluster text editing (bounded capture).
    bool background{}, capture{}, with_cluster{}, input_smoke{}, keyboard_smoke{};
};
MediaViewOptions media_view_options(int argc, const char *const *argv);
bool media_view_arguments(int argc, const char *const *argv);
// The screens of the machine that the session wrote into the owner record of the window.
std::vector<Screen> media_view_screens(const std::filesystem::path &session);
struct MediaViewSnapshot {
    // One frame and receive count per Android screen, by emulator display.
    std::vector<std::shared_ptr<const Image>> frames;
    std::vector<std::uint64_t> received;
    // Displays the OEM power policy currently keeps off; they are not drawn.
    std::array<bool, 2> off{};
    bool ready{}, powered{true}, stopped{}, touches_released{true};
    std::uint64_t input_acknowledgements{}, display_wake_gestures{};
    // Failed stream attempts, retried while the window runs.
    std::uint64_t stream_failures{};
    // A terminal or input failure for the window result and its exit code.
    std::string error;
};
// The pause before the next stream attempt after consecutive failures: 100 ms doubling up to 2 s.
std::chrono::milliseconds stream_retry_delay(unsigned failures);
// Default transports use the same authenticated SDK RPC as the API. Tests inject owned fixtures.
struct MediaViewTransports {
    std::function<Connection(const std::filesystem::path &, std::string_view)> connection;
    std::function<void(const Connection &, GuestDisplay, const std::atomic<bool> &,
                       std::function<void(Image)>)>
            stream;
    std::function<void(const Connection &, std::string_view, std::span<const std::uint8_t>)> input;
};
// CPU workers only. The UI thread alone owns textures and window/ImGui state.
class MediaView {
public:
    explicit MediaView(MediaViewOptions options, MediaViewTransports transports = {});
    ~MediaView();
    MediaView(const MediaView &) = delete;
    MediaView &operator=(const MediaView &) = delete;
    MediaViewSnapshot snapshot() const;
    const std::vector<Screen> &screens() const;
    void visible(bool value);
    bool press(TouchPoint point);
    bool move(TouchPoint point);
    void release();
    bool key(std::string value, bool text = false);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace voyemu::media
