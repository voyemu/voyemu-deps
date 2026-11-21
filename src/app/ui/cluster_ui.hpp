#pragma once
#include "frame_transport.hpp"
#include "music_tab_animation.hpp"
#include "parameters_layout.hpp"
#include "session.hpp"
#include "toggle_animation.hpp"
#include "ui_localization.hpp"
#include "ui_resources.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

struct ImFont;
struct ImGuiContext;
struct ImTextureData;
namespace voyemu::cluster {
inline constexpr float closed_width = 1920.0f;
inline constexpr float closed_height = 360.0f;
inline constexpr float editor_height = 0.0f;
inline constexpr float photo_width = wheel_photo_width;
inline constexpr float photo_height = wheel_photo_height;
struct Options {
    std::filesystem::path endpoint, frame, assets, fonts, replay_frame, capture_ui;
    double smoke_seconds{};
    std::string profile;
    std::optional<std::size_t> inspect_panel;
    // Developer replay has no observed Android appearance; --appearance light selects the theme.
    bool light_theme{};
};
Options parse_options(int argc, const char *const *argv);
// The main window names the machine by its profile title, such as "Voyemu Free 7.3.1".
std::string window_title(const Options &options);
// The screens window adds its caption in the current UI language to the main window title.
std::string screens_window_title(const Options &options, std::string_view screens_label);
struct Point {
    double x{}, y{};
};
struct Region {
    std::string id, photo, label;
    std::array<double, 4> rect{};
    std::vector<Point> polygon;
    bool contains(double x, double y) const;
};
struct Roller {
    std::string photo, up, down;
    std::array<double, 4> rect{};
    std::vector<Point> polygon;
    bool contains(double x, double y) const;
};
struct WheelGeometry {
    std::vector<Region> regions;
    std::vector<Roller> rollers;
    float joined_offset{130};
};
WheelGeometry read_wheel_geometry(const std::filesystem::path &file);
std::vector<Region> read_regions(const std::filesystem::path &file);
std::optional<std::uint64_t> parse_unsigned(std::string_view text, unsigned width);
std::optional<double> parse_number(std::string_view text);
struct ViewState {
    bool ready{};
    std::uint64_t revision{};
    std::string session_id, error;
    Json raw = Json::object(), catalog = Json::array(), deliveries = Json::object(),
         routes = Json::object(), wheel_capability = Json::object();
    // Fields that reach neither guest of the machine and stay in the model.
    std::set<std::string> model_only;
    // The catalogue of the running machine, which map_sha256 of its state names.
    VehicleCatalog platform = VehicleCatalog::qinggan09;
};
bool wheel_action_available(const ViewState &state, std::string_view id);
// Whether a field reaches a guest of the machine; the window grays out the controls of the others
// as a profile limit.
bool field_reaches_guest(const ViewState &state, const std::string &name);
// The field of the state that carries a concept, its primary field, or the concept name when the
// platform does not bind it.
std::string concept_control(const ViewState &state, VehicleConcept id);
// The value of a field as the window shows it: the concept value of a bound primary field, else
// the raw value; null for an absent field or no value.
std::optional<std::int64_t> control_value(const ViewState &state, const std::string &name);
// Only this thread touches the IPC client. The render thread never waits on a guest.
class Client {
public:
    using Transport = std::function<Json(const Json &)>;
    Client(std::filesystem::path endpoint, std::string owner, Transport transport = {});
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    ViewState state() const;
    bool copy_state(ViewState &destination, std::uint64_t &version) const;
    bool patch(const Json &values);
    bool wheel(std::string_view id, std::string_view phase);
    bool detent(std::string_view id);
    void release_owner();
    void close_cluster();
    bool cluster_power(bool enabled);
    bool cluster_restart();
    bool media_power(bool enabled);
    bool media_restart();

    const std::string &owner() const {
        return owner_;
    }

private:
    void run();
    void ingest(const Json &response);
    std::filesystem::path endpoint_;
    std::string owner_;
    Transport transport_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    ViewState state_;
    std::uint64_t state_version_{1};
    Json patch_ = Json::object();
    std::deque<Json> events_;
    std::thread thread_;
    bool stopping_{};
};
// GPU handles are backend identifiers, never owning pointers.
struct Texture {
    std::uint64_t id{};
    int width{}, height{};
};
class Window {
public:
    explicit Window(Options options, Client::Transport transport = {});
    ~Window();

    bool expanded() const {
        return expanded_;
    }

    bool refresh_state();
    bool animation_active(double now) const;

    bool light_theme() const {
        return light_theme_;
    }

    bool cluster_active() const;
    std::uint64_t cluster_generation() const;
    bool frame_presentable(std::uint64_t timestamp_ns);

    const Json &inspection() const {
        return inspection_;
    }

    void draw(Texture frame, Texture left, Texture right, unsigned screen_scale = 100);

    void set_light_wheel_textures(Texture left, Texture right) {
        light_wheel_left_ = left;
        light_wheel_right_ = right;
    }

    // Silhouettes of the decoded wheel photos place the controls around the visible wheel.
    void set_wheel_ink(WheelInk left, WheelInk right) {
        wheel_ink_left_ = std::move(left);
        wheel_ink_right_ = std::move(right);
    }

    // Window changes the platform applies after the frame.
    struct ShellRequest {
        std::optional<unsigned> scale_percent;
        std::optional<ui::Language> language;
        std::string url;
        // Opens or closes the screens window.
        std::optional<bool> screens_window;
    };

    ShellRequest take_shell_request() {
        return std::exchange(shell_request_, {});
    }

    // The in-window About panel covers the whole canvas, including the multimedia screens.
    bool about_open() const {
        return about_open_;
    }

    // Host title bar colours (0xRRGGBB) for the About title; empty keeps the theme colours.
    struct TitleColors {
        unsigned background, text;
    };

    void set_title_colors(std::optional<TitleColors> colors) {
        title_colors_ = colors;
    }

    // Em size of the host title bar font in canvas units for the About title; empty keeps the About
    // text size.
    void set_title_font_size(std::optional<float> em) {
        title_font_size_ = em;
    }

    // Roles of the screens outside the main window; the radio next to the wheel opens their window
    // and is hidden without them.
    void set_screens(std::vector<std::string> roles) {
        screens_ = std::move(roles);
    }

    void set_screens_window(bool open) {
        screens_window_ = open;
    }

    // Name of the screens toggle and title of the screens window; null without screens.
    const char *screens_label() const;
    void focus_lost();
    void close();

    void frame_error(std::string message) {
        frame_error_ = std::move(message);
    }

    const Options &options() const {
        return options_;
    }

    Client &client() {
        return client_;
    }

private:
    struct Draft {
        std::array<char, 128> text{};
        bool active{}, dirty{};
        std::optional<std::uint64_t> submitted;
        std::string error;
    };
    struct FontReference {
        ImGuiContext *context{};
        ImFont *body{};
        ImFont *tab{};
        unsigned scale{};
        float density{}, tab_size{}, checkbox_size{}, input_size{}, unit_size{}, tab_ink_bottom{},
                tab_ink_height{}, checkbox_ink_height{};
    };
    void photo(Texture texture, std::string_view side, float y);
    WheelColumn wheel_layout() const;
    void wheel_controls();
    int choice(const char *id, const ParameterRect &bounds, float cell, float font,
               std::span<const char *const> labels, int selected, MusicTabIndicator &indicator);
    void artwork(ui::Resource id, const ParameterRect &bounds, std::uint32_t color);
    void screens_toggle(const ParameterRect &bounds);
    void about();
    void editor();
    void record_item(const std::string &name, const char *kind);
    void caption(const std::string &name, const char *kind, const char *text, float x, float y,
                 float width, float height, float desired_size, std::uint32_t color);
    void numeric(const char *label, const std::string &name, double divisor = 1, double offset = 0,
                 double minimum = 0, double maximum = -1, double step = 1, bool available = true,
                 float width = 71, const char *unit = "");
    void checkbox(const char *label, const std::string &name, float width = 0);
    void hold(const std::string &id, const std::string &token);
    void release(const std::string &token, bool cancel = false);
    std::optional<std::uint64_t> value(const std::string &name) const;
    Texture light_wheel_left_, light_wheel_right_;
    Options options_;
    Client client_;
    ViewState view_;
    std::uint64_t view_version_{};
    std::uint64_t frame_generation_{}, frame_min_timestamp_{};
    bool frame_gate_open_{};
    std::vector<Region> regions_;
    std::vector<Roller> rollers_;
    float joined_offset_{130};
    std::map<std::string, Draft> drafts_;
    std::map<std::string, ToggleAnimation> toggle_animations_;
    std::map<std::string, std::string> held_;
    std::array<char, 128> search_{};
    std::string frame_error_;
    Json inspection_ = Json::object();
    MusicTabIndicator tab_indicator_, gear_indicator_, scale_indicator_, language_indicator_;
    WheelInk wheel_ink_left_, wheel_ink_right_;
    ShellRequest shell_request_;
    bool about_open_{}, about_scroll_reset_{};
    // The license list was open in the previous frame: ImGui closes it on Escape before the About
    // panel sees the key, which then must not close the panel too.
    bool about_list_open_{};
    std::size_t about_license_{};
    // License text scrolls toward a target so wheel steps and trackpad momentum glide.
    float about_scroll_target_{}, about_scroll_y_{};
    std::optional<TitleColors> title_colors_;
    std::optional<float> title_font_size_;
    std::vector<std::string> screens_;
    bool screens_window_{};
    std::map<std::tuple<int, int, int>, std::unique_ptr<ImTextureData>> artwork_textures_;
    ImGuiContext *artwork_context_{};
    std::array<float, 5> tab_centers_{};
    bool tab_geometry_initialized_{};
    std::size_t section_{};
    unsigned screen_scale_{100};
    float numeric_height_{26}, numeric_font_size_{13}, numeric_unit_size_{11}, label_font_size_{13},
            numeric_label_width_{}, checkbox_font_size_{20}, heading_font_size_{20};
    std::array<FontReference, 3> font_references_{};
    bool expanded_{true}, closed_{}, light_theme_{};
};
} // namespace voyemu::cluster
