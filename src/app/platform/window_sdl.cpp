#include "cluster_font.hpp"
#include "cluster_ui.hpp"
#include "error.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_internal.h"
#include "logging.hpp"
#include "media_imgui_input.hpp"
#include "media_view.hpp"
#include "model.hpp"
#include "parameters_layout.hpp"
#include "program_modes.hpp"
#include "startup_animation.hpp"
#include "startup_video.hpp"
#include "texture_pixels.hpp"
#include "ui_localization.hpp"
#include "ui_resources.hpp"
#include "window_canvas.hpp"
#include "window_image.hpp"
#include "window_shell.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace cluster = voyemu::cluster;
namespace media = voyemu::media;
namespace ui = voyemu::ui;
using voyemu::Json;

namespace {
std::unique_ptr<media::MediaView> multimedia;
void (*platform_ime)(ImGuiContext *, ImGuiViewport *, ImGuiPlatformImeData *) = nullptr;
// Window coordinates per logical canvas unit, for the IME candidate position.
float ime_scale_x = 1, ime_scale_y = 1;

void need(bool condition, const char *operation) {
    if (!condition) {
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
    }
}

std::filesystem::path utf8_path(const char *text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(text)));
}

bool background_arguments(int argc, const char *const *argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--background") {
            return true;
        }
    }

    return false;
}

cluster::Texture view(SDL_Texture *texture) {
    if (!texture) {
        return {};
    }

    return {reinterpret_cast<std::uint64_t>(texture), texture->w, texture->h};
}

SDL_Texture *upload(SDL_Renderer *renderer, const voyemu::window::Image &image) {
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STATIC, image.width, image.height);

    need(texture && SDL_UpdateTexture(texture, nullptr, image.rgba.data(), image.width * 4),
         "WINDOW_TEXTURE: cannot upload static UI image");
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);

    return texture;
}

// Wheel photos and control artwork: bounded straight RGBA with filled transparent edges.
SDL_Texture *load_wheel(SDL_Renderer *renderer, const std::filesystem::path &path,
                        cluster::WheelInk *ink = nullptr) {
    auto image = voyemu::window::decode_png(path);

    if (image.width > 1024 || image.height > 1024) {
        throw voyemu::VoyemuError("WINDOW_WHEEL", "image bounds changed");
    }

    if (ink) {
        *ink = cluster::wheel_ink(image.rgba.data(), image.width, image.height,
                                  std::size_t(image.width) * 4);
    }

    media::fill_transparent_rgb(image.rgba, image.width, image.height);

    return upload(renderer, image);
}

// Splash and the light panel background keep the pinned full-screen geometry.
SDL_Texture *load_screen(SDL_Renderer *renderer, const std::filesystem::path &path) {
    const auto image = voyemu::window::decode_png(path);

    if (image.width != 1920 || image.height != 720) {
        throw voyemu::VoyemuError("WINDOW_SCREEN", "expected pinned 1920x720 image");
    }

    return upload(renderer, image);
}

SDL_Texture *stream_texture(SDL_Renderer *renderer, SDL_PixelFormat format, bool opaque,
                            unsigned width = 1920, unsigned height = 720) {
    SDL_Texture *texture = SDL_CreateTexture(renderer, format, SDL_TEXTUREACCESS_STREAMING,
                                             int(width), int(height));

    need(texture, "WINDOW_TEXTURE: cannot allocate screen texture");
    SDL_SetTextureBlendMode(texture, opaque ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);

    return texture;
}

voyemu::frames::Frame read_replay(const std::filesystem::path &path) {
    const auto size = std::filesystem::file_size(path);

    if (size > 1920u * 720u * 4u + 4096u) {
        throw voyemu::VoyemuError("WINDOW", "Replay exceeds original Screen bound");
    }

    std::vector<std::byte> bytes(size);
    std::ifstream source(path, std::ios::binary);

    if (!source.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size))) {
        throw voyemu::VoyemuError("WINDOW", "Cannot read explicit replay BMP");
    }

    voyemu::frames::Frame frame;
    frame.sequence = 1;
    frame.width = 1920;
    frame.height = 720;
    frame.stride = 7680;
    frame.bgra = voyemu::frames::decode_screen_bmp(bytes);

    return frame;
}

// The rendered target before present, as BGRA rows.
void capture(SDL_Renderer *renderer, const std::filesystem::path &path) {
    SDL_Surface *pixels = SDL_RenderReadPixels(renderer, nullptr);

    need(pixels, "WINDOW_CAPTURE: cannot read the rendered surface");
    SDL_Surface *bgra = SDL_ConvertSurface(pixels, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(pixels);
    need(bgra, "WINDOW_CAPTURE: cannot convert the rendered surface");
    std::vector<std::uint8_t> rows(std::size_t(bgra->w) * bgra->h * 4);

    for (int y = 0; y < bgra->h; ++y) {
        std::copy_n(static_cast<const std::uint8_t *>(bgra->pixels) + std::size_t(y) * bgra->pitch,
                    std::size_t(bgra->w) * 4, rows.data() + std::size_t(y) * bgra->w * 4);
    }

    const int width = bgra->w, height = bgra->h;
    SDL_DestroySurface(bgra);
    voyemu::window::save_png(path, width, height, rows);
}

std::filesystem::path language_file() {
    char *base = SDL_GetPrefPath("", "Voyemu");

    if (!base) {
        return {};
    }

    auto result = utf8_path(base) / "ui-language.json";
    SDL_free(base);

    return result;
}

// A selected UI language is app-wide; first launch follows the preferred UI language.
ui::Language initial_language() {
    try {
        const auto file = language_file();

        if (!file.empty() && std::filesystem::is_regular_file(file)) {
            const auto saved =
                    voyemu::read_private_json(file, 4096).at("language").get<std::string>();

            if (saved == "ru" || saved == "en" || saved == "zh") {
                return ui::language_from_tag(saved);
            }
        }
    } catch (const std::exception &) {
        voyemu::logging::write(voyemu::logging::Level::debug, "window", "language.using_preferred");
    }

    int count = 0;
    SDL_Locale **locales = SDL_GetPreferredLocales(&count);
    auto language = ui::Language::english;

    if (locales && count > 0 && locales[0]->language) {
        language = ui::language_from_tag(locales[0]->language);
    }

    SDL_free(static_cast<void *>(locales));

    return language;
}

void save_language() {
    try {
        const auto file = language_file();

        if (!file.empty()) {
            voyemu::write_private_json(
                    file, {{"schema", 1}, {"language", ui::language_tag(ui::current_language)}});
        }
    } catch (const std::exception &) {
        voyemu::logging::write(voyemu::logging::Level::error, "window", "language.save_failed");
    }
}

// Bottom-left (y-up) rectangles of the shared placement helpers.
VoyemuWindowRect y_up(double x, double y, double width, double height) {
    return {x, -(y + height), width, height};
}

class App {
public:
    App(int argc, const char *const *argv) {
        if (media::media_view_arguments(argc, argv)) {
            media_options_ = media::media_view_options(argc, argv);
            options_.endpoint = media_options_->endpoint;
            options_.frame = media_options_->session / "frame.bgra";
            options_.assets = media_options_->assets;
            options_.fonts = media_options_->fonts;
            options_.profile = media_options_->profile;
            options_.smoke_seconds = media_options_->seconds;
            options_.inspect_panel = media_options_->inspect_panel;
            voyemu::ensure_private_directory(media_options_->session / "media-native-proof");

            if (media_options_->capture) {
                options_.capture_ui = media_options_->session /
                                      "media-native-proof/combined-final.png";
            }

            background_ = media_options_->background;
        } else {
            std::vector<const char *> cluster_args;

            for (int i = 0; i < argc; ++i) {
                if (std::string_view(argv[i]) != "--background") {
                    cluster_args.push_back(argv[i]);
                }
            }

            options_ = cluster::parse_options(int(cluster_args.size()), cluster_args.data());
            background_ = background_arguments(argc, argv);
        }
    }

    ~App() {
        if (multimedia) {
            multimedia->stop();
            multimedia.reset();
        }

        if (imgui_) {
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
        }

        // The UI owns logo textures that the renderer backend released above.
        ui_.reset();

        for (auto *texture : textures_) {
            SDL_DestroyTexture(texture);
        }

        if (screens_renderer_) {
            SDL_DestroyRenderer(screens_renderer_);
        }

        if (screens_window_) {
            SDL_DestroyWindow(screens_window_);
        }

        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
        }

        if (window_) {
            SDL_DestroyWindow(window_);
        }

        SDL_Quit();
    }

    int run();

private:
    bool cluster_visible() const {
        return !media_options_ || media_options_->with_cluster;
    }

    SDL_Texture *own(SDL_Texture *texture) {
        textures_.push_back(texture);

        return texture;
    }

    void create_window();
    void load_textures();
    void restore_placement();
    void save_placement();
    void apply_scale();
    void release_media();
    bool media_event(const SDL_Event &event);
    void apply_shell_request();
    void draw_media(const media::WindowCanvas &canvas);
    std::vector<media::ScreenSize> screen_sizes() const;
    void open_screens_window(bool open);
    void apply_screens_scale();
    void upload_screen_frames();
    void draw_screens_window(bool starting, bool final_frame);
    bool screens_event(const SDL_Event &event);
    std::pair<float, float> screen_point(std::size_t screen) const;
    void write_standalone_proof(bool backend_connected);
    void write_media_proof();
    Json canvas_proof() const;

    // Whether the label font draws Chinese, as the 中文 language choice needs.
    static bool chinese_glyph() {
        return cluster::ui_text_font(cluster::UiFontRole::Label)->IsGlyphInFont(0x4e2d);
    }

    double elapsed() const;
    void schedule(double delay, std::function<void()> step);
    std::pair<float, float> window_point(const Json &item) const;
    std::pair<float, float> android_point(unsigned display) const;
    void push_mouse(SDL_EventType type, std::pair<float, float> point,
                    SDL_Window *target = nullptr) const;
    void push_key(SDL_EventType type, SDL_Keycode key, SDL_Scancode scancode) const;
    void push_text(const char *text) const;
    void click(std::function<Json()> item, std::function<void()> released = {});
    void press_key(SDL_Keycode key, SDL_Scancode scancode);
    void plan_smokes();

    std::optional<media::MediaViewOptions> media_options_;
    cluster::Options options_;
    bool background_{}, imgui_{}, closing_{}, media_keyboard_{}, media_text_{}, window_focus_{};
    unsigned zoom_ = 100;
    SDL_Window *window_{};
    SDL_Renderer *renderer_{};
    std::vector<SDL_Texture *> textures_;
    std::unique_ptr<cluster::Window> ui_;
    std::optional<media::TouchPoint> touch_;
    ImU32 mapped_mouse_event_{};
    SDL_Texture *wheel_left_{}, *wheel_right_{}, *splash_{}, *qnx_{};
    std::array<SDL_Texture *, 2> android_{};
    std::array<std::shared_ptr<const media::Image>, 2> displayed_;
    // The OEM boot animation of each screen until Android is ready: one streaming texture holds
    // the current frame, decoded only when the frame changes.
    struct StartupScreen {
        std::optional<voyemu::window::StartupAnimation> animation;
        bool black{};
        SDL_Texture *texture{};
        std::optional<std::pair<std::size_t, std::size_t>> shown;
        Json frames = Json::array();
    };
    std::array<StartupScreen, 2> startup_;
    // The OEM startup videos of the multimedia screens and the cluster, stored by preparation as
    // boot animation archives: all play once from each entry into the startup phase, decoded on
    // worker threads, and black stands for a frame not decoded yet.
    struct StartupVideo {
        std::optional<voyemu::window::StartupAnimation> animation;
        std::unique_ptr<voyemu::window::StartupVideoPlayer> player;
        SDL_Texture *texture{};
        bool shown{}, ended{};
        // Window time of the end; the boot animation of the screen starts there.
        double ended_at{};
        Json frames = Json::array();
        std::string error;

        // The video phase of its screen: from the startup phase entry until the video ends.
        bool playing(bool starting) const {
            return animation && starting && !ended;
        }
    };
    // Multimedia screens 0 and 1, the cluster, then the videos of the screens window by role.
    std::vector<StartupVideo> startup_videos_ = std::vector<StartupVideo>(3);
    std::vector<std::string> startup_video_names_{"display0", "display1", "cluster"};
    // The screens outside the main window, drawn by their own renderer in the screens window that
    // the radio next to the wheel opens; textures exist while the window is open.
    struct ExtraScreen {
        voyemu::Screen screen;
        StartupScreen startup;
        std::optional<std::size_t> video;
        SDL_Texture *frame{}, *video_texture{};
        std::shared_ptr<const media::Image> displayed;
    };
    std::vector<ExtraScreen> extra_;
    SDL_Window *screens_window_{};
    SDL_Renderer *screens_renderer_{};
    // The screens window touch: the screen it started on and its held point.
    std::optional<std::pair<std::size_t, media::TouchPoint>> screens_touch_;
    bool screens_smoke_{}, screens_input_{};
    voyemu::window::StartupVideoTrigger startup_trigger_;
    void play_startup_video(unsigned slot, std::optional<double> seconds);
    media::MediaViewSnapshot media_state_;
    std::uint64_t save_deadline_{};
    std::string renderer_name_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    // Synthetic SDL input of the bounded smokes: ordered steps with a whole frame between them, so
    // each step observes the frame that processed the events of the previous one; a next_frame
    // step runs in the frame right after the previous step instead.
    struct SmokeStep {
        double at;
        std::function<void()> run;
        bool next_frame{};
    };
    std::deque<SmokeStep> smoke_;
    std::uint64_t frame_{}, smoke_frame_{};
    Json smoke_proof_ = Json::object();
    std::pair<float, float> smoke_point_{};
    std::array<std::shared_ptr<const media::Image>, 2> reaction_baseline_;
    std::array<double, 2> reaction_started_{};
    bool panel_smoke_{}, android_smoke_{}, keyboard_smoke_{}, after_input_{};
    // Cluster patches of the keyboard smoke arrive on the cluster client thread.
    struct Patches {
        std::mutex mutex;
        Json values = Json::array();
    };
    std::shared_ptr<Patches> keyboard_patches_;
};

void App::create_window() {
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, background_ ? "0" : "1");
    SDL_SetHint(SDL_HINT_APP_NAME, "Voyemu");
    need(SDL_Init(SDL_INIT_VIDEO), "WINDOW_VIDEO: cannot initialize SDL video");
    voyemu::window_shell::prepare_application();
    window_ = SDL_CreateWindow(cluster::window_title(options_).c_str(), int(cluster::closed_width),
                               360, SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    need(window_, "WINDOW_CREATE: cannot create the window");
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    need(renderer_, "WINDOW_RENDERER: no SDL renderer is available");
    SDL_SetRenderVSync(renderer_, 1);
    renderer_name_ = SDL_GetRendererName(renderer_);
    const auto icon_bytes = ui::resource(ui::Resource::Icon);

    if (SDL_Surface *icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(icon_bytes.data(), icon_bytes.size()),
                                           true)) {
        SDL_SetWindowIcon(window_, icon);
        SDL_DestroySurface(icon);
    }
}

void App::load_textures() {
    cluster::WheelInk left_ink, right_ink;

    wheel_left_ = own(load_wheel(renderer_, options_.assets / "wheel-left.png", &left_ink));
    wheel_right_ = own(load_wheel(renderer_, options_.assets / "wheel-right.png", &right_ink));
    ui_->set_wheel_ink(std::move(left_ink), std::move(right_ink));

    if (std::filesystem::is_regular_file(options_.assets / "wheel-left-light.png") &&
        std::filesystem::is_regular_file(options_.assets / "wheel-right-light.png")) {
        ui_->set_light_wheel_textures(
                view(own(load_wheel(renderer_, options_.assets / "wheel-left-light.png"))),
                view(own(load_wheel(renderer_, options_.assets / "wheel-right-light.png"))));
    }

    if (media_options_) {
        const auto &options = *media_options_;
        splash_ = own(load_screen(renderer_, options.splash));

        for (unsigned display = 0; display < 2; ++display) {
            const auto &source = options.startup[display];

            if (!source) {
                continue;
            }

            startup_[display].black = source->empty();

            if (!source->empty()) {
                const auto &animation = startup_[display].animation.emplace(
                        voyemu::window::read_startup_animation(*source));
                startup_[display].texture =
                        own(stream_texture(renderer_, SDL_PIXELFORMAT_ABGR8888, true,
                                           animation.width, animation.height));
            }
        }

        const std::array<std::optional<std::filesystem::path>, 3> videos = {
                options.startup_video[0], options.startup_video[1],
                options.with_cluster ? options.startup_cluster : std::nullopt};

        for (unsigned slot = 0; slot < videos.size(); ++slot) {
            if (const auto &video = videos[slot]) {
                const auto &animation = startup_videos_[slot].animation.emplace(
                        voyemu::window::read_startup_animation(*video));
                startup_videos_[slot].texture =
                        own(stream_texture(renderer_, SDL_PIXELFORMAT_ABGR8888, true,
                                           animation.width, animation.height));
            }
        }

        for (const auto &screen : media::media_view_screens(options.session)) {
            if (voyemu::multimedia_screen(screen.role)) {
                continue;
            }

            auto &extra = extra_.emplace_back();
            extra.screen = screen;

            if (const auto source = options.startup_screens.find(screen.role);
                source != options.startup_screens.end()) {
                extra.startup.black = source->second.empty();

                if (!source->second.empty()) {
                    extra.startup.animation.emplace(
                            voyemu::window::read_startup_animation(source->second));
                }
            }

            if (const auto video = options.startup_video_screens.find(screen.role);
                video != options.startup_video_screens.end()) {
                extra.video = startup_videos_.size();
                startup_videos_.emplace_back().animation.emplace(
                        voyemu::window::read_startup_animation(video->second));
                startup_video_names_.push_back(screen.role);
            }
        }
    }
}

void App::apply_scale() {
    unsigned pixel_width, pixel_height;

    if (media_options_) {
        const auto geometry = media::canvas_geometry(zoom_, 1, media_options_->with_cluster);
        pixel_width = geometry.value().pixel_width;
        pixel_height = geometry.value().pixel_height;
    } else {
        const auto layout = cluster::view_layout(cluster::ui_layout_scale(zoom_));
        const float scale = std::max(SDL_GetWindowDisplayScale(window_), 1.f);
        pixel_width = unsigned(std::ceil(layout.top_width * scale));
        pixel_height = unsigned(std::ceil(layout.top_height * scale));
    }

    const float density = std::max(SDL_GetWindowPixelDensity(window_), 1.f);
    SDL_SetWindowSize(window_, int(std::lround(pixel_width / density)),
                      int(std::lround(pixel_height / density)));
    apply_screens_scale();
}

std::vector<media::ScreenSize> App::screen_sizes() const {
    std::vector<media::ScreenSize> sizes;

    for (const auto &extra : extra_) {
        sizes.push_back({extra.screen.width, extra.screen.height});
    }

    return sizes;
}

// The screens window keeps the zoom of the main window.
void App::apply_screens_scale() {
    if (!screens_window_) {
        return;
    }

    const auto canvas = media::screens_canvas(screen_sizes(), zoom_);
    const float density = std::max(SDL_GetWindowPixelDensity(screens_window_), 1.f);
    SDL_SetWindowSize(screens_window_, int(std::lround(canvas.value().pixel_width / density)),
                      int(std::lround(canvas.value().pixel_height / density)));
}

// The window and its renderer exist only while open; a background launch never activates it.
void App::open_screens_window(bool open) {
    if (open == bool(screens_window_) || extra_.empty()) {
        return;
    }

    if (!open) {
        if (screens_touch_ && multimedia) {
            multimedia->release();
        }

        screens_touch_.reset();
        SDL_DestroyRenderer(screens_renderer_);
        SDL_DestroyWindow(screens_window_);
        screens_renderer_ = nullptr;
        screens_window_ = nullptr;

        for (auto &extra : extra_) {
            extra.frame = extra.video_texture = extra.startup.texture = nullptr;
            extra.startup.shown.reset();
            extra.displayed.reset();
        }

        ui_->set_screens_window(false);
        voyemu::logging::write(voyemu::logging::Level::debug, "window", "screens_window.closed");

        return;
    }

    screens_window_ =
            SDL_CreateWindow(cluster::screens_window_title(options_, ui_->screens_label()).c_str(),
                             640, 180, SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    need(screens_window_, "WINDOW_SCREENS: cannot create the screens window");
    screens_renderer_ = SDL_CreateRenderer(screens_window_, nullptr);
    need(screens_renderer_, "WINDOW_SCREENS: no SDL renderer is available");
    // The main window paces the loop; this renderer never waits for vertical sync.
    SDL_SetRenderVSync(screens_renderer_, 0);

    for (auto &extra : extra_) {
        if (extra.startup.animation) {
            extra.startup.texture = stream_texture(screens_renderer_, SDL_PIXELFORMAT_ABGR8888,
                                                   true, extra.startup.animation->width,
                                                   extra.startup.animation->height);
        }
    }

    apply_screens_scale();
    SDL_ShowWindow(screens_window_);

    if (background_) {
        voyemu::window_shell::send_to_back(screens_window_);
    }

    ui_->set_screens_window(true);
    voyemu::logging::write(voyemu::logging::Level::debug, "window", "screens_window.opened",
                           {{"screens", extra_.size()}, {"zoom", zoom_}});
}

// Android frames of the screens window, uploaded while it is open and Android is ready.
void App::upload_screen_frames() {
    for (auto &extra : extra_) {
        const auto display = extra.screen.android_display;
        const auto frame = display && media_state_.ready && *display < media_state_.frames.size()
                                   ? media_state_.frames[*display]
                                   : nullptr;

        if (!frame) {
            extra.displayed.reset();
            continue;
        }

        if (!screens_renderer_ || frame == extra.displayed) {
            continue;
        }

        if (!extra.frame) {
            extra.frame = stream_texture(screens_renderer_, SDL_PIXELFORMAT_ABGR8888, true,
                                         extra.screen.width, extra.screen.height);
        }

        need(SDL_UpdateTexture(extra.frame, nullptr, frame->pixels().data(),
                               int(extra.screen.width * 4)),
             "WINDOW_TEXTURE: cannot upload a screens window frame");
        extra.displayed = frame;
    }
}

void App::draw_screens_window(bool starting, bool final_frame) {
    int width = 0, height = 0;

    SDL_GetWindowSizeInPixels(screens_window_, &width, &height);
    const auto canvas = media::screens_canvas(screen_sizes(), zoom_);
    SDL_SetRenderDrawColorFloat(screens_renderer_, .055f, .06f, .07f, 1);
    SDL_RenderClear(screens_renderer_);

    if (!canvas || width <= 0 || height <= 0) {
        SDL_RenderPresent(screens_renderer_);

        return;
    }

    const float sx = float(width) / canvas->pixel_width, sy = float(height) / canvas->pixel_height;

    for (std::size_t i = 0; i < extra_.size(); ++i) {
        auto &extra = extra_[i];
        const auto &area = canvas->screens[i];
        const SDL_FRect target{float(area.x) * sx, float(area.y) * sy, float(area.width) * sx,
                               float(area.height) * sy};
        SDL_SetRenderDrawColor(screens_renderer_, 0, 0, 0, 255);
        SDL_RenderFillRect(screens_renderer_, &target);
        const auto *video = extra.video ? &startup_videos_[*extra.video] : nullptr;
        auto &screen = extra.startup;

        if (extra.displayed && media_state_.powered && media_state_.ready) {
            SDL_RenderTexture(screens_renderer_, extra.frame, nullptr, &target);
        } else if (video && video->playing(starting)) {
            if (video->shown && extra.video_texture) {
                SDL_RenderTexture(screens_renderer_, extra.video_texture, nullptr, &target);
            }
        } else if (starting && screen.animation) {
            const auto frame = voyemu::window::startup_frame(
                    *screen.animation, elapsed() - (video ? video->ended_at : 0));

            if (frame != screen.shown) {
                const auto image = voyemu::window::decode_png_bytes(
                        voyemu::window::read_startup_frame(*screen.animation, frame));

                if (unsigned(image.width) != screen.animation->width ||
                    unsigned(image.height) != screen.animation->height) {
                    throw voyemu::VoyemuError("WINDOW_STARTUP", "frame differs from desc.txt size");
                }

                need(SDL_UpdateTexture(screen.texture, nullptr, image.rgba.data(), image.width * 4),
                     "WINDOW_STARTUP: cannot upload boot animation frame");
                screen.shown = frame;
                screen.frames.push_back(std::to_string(frame.first) + "/" +
                                        std::to_string(frame.second));
            }

            SDL_RenderTexture(screens_renderer_, screen.texture, nullptr, &target);
        }
    }

    if (media_options_.value().capture && final_frame) {
        capture(screens_renderer_,
                media_options_.value().session / "media-native-proof/screens-final.png");
    }

    SDL_RenderPresent(screens_renderer_);
}

// Every event of the screens window: its touches reach the Android screen under the pointer and
// its close button closes only this window. True when the event belongs to it.
bool App::screens_event(const SDL_Event &event) {
    if (!screens_window_) {
        return false;
    }

    const auto id = SDL_GetWindowID(screens_window_);
    const bool button = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                        event.type == SDL_EVENT_MOUSE_BUTTON_UP,
               motion = event.type == SDL_EVENT_MOUSE_MOTION;

    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == id) {
        open_screens_window(false);

        return true;
    }

    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) {
        return event.window.windowID == id;
    }

    if (!(button && event.button.windowID == id) && !(motion && event.motion.windowID == id)) {
        return false;
    }

    if (button && event.button.button != SDL_BUTTON_LEFT) {
        return true;
    }

    const float density = SDL_GetWindowPixelDensity(screens_window_);
    const double x = (motion ? event.motion.x : event.button.x) * density,
                 y = (motion ? event.motion.y : event.button.y) * density;
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(screens_window_, &width, &height);
    const auto canvas = media::screens_canvas(screen_sizes(), zoom_);
    const auto sizes = screen_sizes();

    if (!canvas || !multimedia) {
        return true;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        release_media();
        const auto point = media::screens_touch_point(x, y, width, height, *canvas, sizes);

        if (!point || !extra_[point->screen].screen.android_display) {
            return true;
        }

        const media::TouchPoint touch{extra_[point->screen].screen.android_display.value(),
                                      point->x, point->y};

        if (multimedia->press(touch)) {
            screens_touch_ = {point->screen, touch};
            SDL_CaptureMouse(true);
        }

        return true;
    }

    if (!screens_touch_) {
        return true;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        multimedia->release();
        screens_touch_.reset();
        SDL_CaptureMouse(false);

        return true;
    }

    if (const auto point = media::screens_drag_point(x, y, width, height, *canvas, sizes,
                                                     screens_touch_->first)) {
        screens_touch_->second.x = point->x;
        screens_touch_->second.y = point->y;
        multimedia->move(screens_touch_->second);
    }

    return true;
}

void App::restore_placement() {
    std::optional<std::pair<double, double>> saved;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();

    if (media_options_ && std::filesystem::exists(media_options_->state)) {
        try {
            const auto record = voyemu::read_private_json(media_options_->state, 4096);

            if (record.at("schema") == 1 && record.at("profile") == media_options_->profile) {
                if (record.contains("scale_percent") &&
                    record.at("scale_percent").is_number_integer() &&
                    (record.at("scale_percent") == 50 || record.at("scale_percent") == 75 ||
                     record.at("scale_percent") == 100)) {
                    zoom_ = record.at("scale_percent").get<unsigned>();
                }

                const auto identity = record.at("display").get<std::string>();
                int count = 0;
                SDL_DisplayID *displays = SDL_GetDisplays(&count);

                for (int i = 0; displays && i < count; ++i) {
                    const char *name = SDL_GetDisplayName(displays[i]);

                    if (name && identity == name) {
                        display = displays[i];
                        saved = {{record.at("left_offset").get<double>(),
                                  record.at("top_offset").get<double>()}};
                        break;
                    }
                }

                SDL_free(displays);
            }
        } catch (const std::exception &) {
            voyemu::logging::write(voyemu::logging::Level::debug, "window",
                                   "placement.using_first_launch");
        }
    }

    apply_scale();
    SDL_Rect area{};

    if (!SDL_GetDisplayUsableBounds(display, &area)) {
        return;
    }

    int width = 0, height = 0, top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowSize(window_, &width, &height);
    SDL_GetWindowBordersSize(window_, &top, &left, &bottom, &right);
    const auto frame_width = double(width + left + right),
               frame_height = double(height + top + bottom);
    const auto placed = media::restore_position(y_up(area.x, area.y, area.w, area.h),
                                                y_up(0, 0, frame_width, frame_height), saved);
    SDL_SetWindowPosition(window_, int(std::lround(placed.x)) + left,
                          int(std::lround(-(placed.y + frame_height))) + top);
}

void App::save_placement() {
    save_deadline_ = 0;

    if (!media_options_ || !window_) {
        return;
    }

    const auto display = SDL_GetDisplayForWindow(window_);
    const char *name = SDL_GetDisplayName(display);
    SDL_Rect area{};
    int x = 0, y = 0, top = 0, left = 0, bottom = 0, right = 0;

    if (!name || !SDL_GetDisplayUsableBounds(display, &area) ||
        !SDL_GetWindowPosition(window_, &x, &y)) {
        return;
    }

    SDL_GetWindowBordersSize(window_, &top, &left, &bottom, &right);
    try {
        voyemu::write_private_json(media_options_->state, {{"schema", 1},
                                                           {"profile", media_options_->profile},
                                                           {"display", name},
                                                           {"left_offset", x - left - area.x},
                                                           {"top_offset", y - top - area.y},
                                                           {"scale_percent", zoom_}});
    } catch (const std::exception &) {
        voyemu::logging::write(voyemu::logging::Level::error, "window", "placement.save_failed");
    }
}

void App::release_media() {
    if (multimedia) {
        multimedia->release();
    }

    if (touch_ || screens_touch_) {
        SDL_CaptureMouse(false);
    }

    touch_.reset();
    screens_touch_.reset();
}

// Mouse and keyboard input addressed to the Android screens; true when consumed.
bool App::media_event(const SDL_Event &event) {
    if (!multimedia) {
        return false;
    }

    const auto &io = ImGui::GetIO();

    if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        event.button.button != SDL_BUTTON_LEFT) {
        return false;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_MOTION ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        const float density = SDL_GetWindowPixelDensity(window_);
        const float x = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.x : event.button.x,
                    y = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.y : event.button.y;
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        const auto canvas = media::window_canvas(zoom_, media_options_.value().with_cluster, width,
                                                 height);

        if (!canvas) {
            return false;
        }

        const double px = x * density, py = y * density;
        // The in-window About panel covers the multimedia screens.
        const bool covered = ui_ && ui_->about_open();

        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            const auto point =
                    covered ? std::nullopt
                            : media::canvas_touch_point(px, py, canvas->content.width,
                                                        canvas->content.height,
                                                        media_options_.value().with_cluster, zoom_);
            media_keyboard_ = bool(point);

            if (!point) {
                release_media();

                return false;
            }

            ImGui::ClearActiveID();
            ImGui::SetWindowFocus(nullptr);

            if (media_options_.value().input_smoke) {
                reaction_baseline_[point->display] = media_state_.frames[point->display];
                reaction_started_[point->display] = elapsed();
                smoke_proof_["input_reaction"][std::to_string(point->display)] = {
                        {"source", "SDL mouse event; first significant OEM scene change"}};
            }

            if (multimedia->press(*point)) {
                touch_ = *point;
                SDL_CaptureMouse(true);
            }

            return true;
        }

        if (!touch_) {
            return false;
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP || covered) {
            release_media();

            return true;
        }

        // A drag stays on its screen and is pinned to the screen edges.
        const auto row = media::media_row(canvas->content.width, canvas->content.height,
                                          media_options_.value().with_cluster, zoom_);
        const auto mapped = media::canvas_touch_point(
                std::clamp(px, row.value().x, row.value().x + row.value().width - 0.01),
                std::clamp(py, row.value().y, row.value().y + row.value().height - 0.01),
                canvas->content.width, canvas->content.height, media_options_.value().with_cluster,
                zoom_);

        if (mapped) {
            touch_->x = std::clamp(mapped->x + (int(mapped->display) - int(touch_->display)) * 1920,
                                   0, 1919);
            touch_->y = mapped->y;
            multimedia->move(*touch_);
        }

        return true;
    }

    if (!media_keyboard_ || io.WantTextInput || io.WantCaptureKeyboard) {
        return false;
    }

    if (event.type == SDL_EVENT_KEY_DOWN) {
        const char *key = nullptr;

        switch (event.key.key) {
        case SDLK_ESCAPE:
            key = "Escape";
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            key = "Enter";
            break;
        case SDLK_BACKSPACE:
            key = "Backspace";
            break;
        case SDLK_TAB:
            key = "Tab";
            break;
        case SDLK_LEFT:
            key = "ArrowLeft";
            break;
        case SDLK_RIGHT:
            key = "ArrowRight";
            break;
        case SDLK_DOWN:
            key = "ArrowDown";
            break;
        case SDLK_UP:
            key = "ArrowUp";
            break;
        case SDLK_HOME:
            key = "Home";
            break;
        case SDLK_END:
            key = "End";
            break;
        case SDLK_DELETE:
            key = "Delete";
            break;
        default:
            break;
        }

        if (key) {
            multimedia->key(key);

            return true;
        }

        return false;
    }

    if (event.type == SDL_EVENT_TEXT_INPUT) {
        std::string text(event.text.text);
        std::erase_if(text, [](char c) {
            return static_cast<unsigned char>(c) < 32 || c == 127;
        });

        if (!text.empty()) {
            multimedia->key(std::move(text), true);
        }

        return true;
    }

    return false;
}

void App::apply_shell_request() {
    // Scale, language and site requests from the canvas apply between frames.
    const auto request = ui_->take_shell_request();

    if (request.scale_percent) {
        ui_->focus_lost();
        release_media();
        zoom_ = *request.scale_percent;
        ImGui::GetIO().ClearEventsQueue();
        ImGui::GetIO().ClearInputMouse();
        mapped_mouse_event_ = ImGui::GetCurrentContext()->InputEventsNextEventId - 1;
        apply_scale();
        save_placement();
    }

    if (request.language) {
        ui::current_language = *request.language;

        if (!media_options_ || !media_options_->inspect_language) {
            save_language();
        }

        voyemu::window_shell::set_quit_title(ui::tr("Завершить Voyemu"));

        if (screens_window_) {
            SDL_SetWindowTitle(
                    screens_window_,
                    cluster::screens_window_title(options_, ui_->screens_label()).c_str());
        }
    }

    if (request.screens_window) {
        open_screens_window(*request.screens_window);
    }

    if (!request.url.empty() && !SDL_OpenURL(request.url.c_str())) {
        voyemu::logging::write(voyemu::logging::Level::error, "window", "site.open_failed",
                               {{"reason", SDL_GetError()}});
    }
}

void App::draw_media(const media::WindowCanvas &canvas) {
    const auto row = media::media_row(canvas.design_width, canvas.design_height,
                                      media_options_.value().with_cluster, zoom_);
    auto *draw = ImGui::GetBackgroundDrawList();
    const bool starting = media_state_.powered && !media_state_.ready;

    for (unsigned display = 0; display < 2; ++display) {
        const ImVec2 a(float(row.value().x + display * row.value().width / 2),
                       float(row.value().y)),
                b(a.x + float(row.value().width / 2), a.y + float(row.value().height));
        draw->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));

        if (media_state_.powered && media_state_.ready && displayed_[display] &&
            !media_state_.off[display]) {
            draw->AddImage(static_cast<ImTextureID>(view(android_[display]).id), a, b);
        }

        auto &screen = startup_[display];
        const auto &video = startup_videos_[display];

        if (video.playing(starting)) {
            if (video.shown) {
                draw->AddImage(static_cast<ImTextureID>(view(video.texture).id), a, b);
            }
        } else if (starting && screen.animation) {
            const auto frame = voyemu::window::startup_frame(
                    *screen.animation, elapsed() - (video.animation ? video.ended_at : 0));

            if (frame != screen.shown) {
                const auto image = voyemu::window::decode_png_bytes(
                        voyemu::window::read_startup_frame(*screen.animation, frame));

                if (unsigned(image.width) != screen.animation->width ||
                    unsigned(image.height) != screen.animation->height) {
                    throw voyemu::VoyemuError("WINDOW_STARTUP", "frame differs from desc.txt size");
                }

                need(SDL_UpdateTexture(screen.texture, nullptr, image.rgba.data(), image.width * 4),
                     "WINDOW_STARTUP: cannot upload boot animation frame");
                screen.shown = frame;
                screen.frames.push_back(std::to_string(frame.first) + "/" +
                                        std::to_string(frame.second));
            }

            draw->AddImage(static_cast<ImTextureID>(view(screen.texture).id), a, b);
        } else if (starting && !screen.black) {
            const auto logo = media::splash_rect(canvas.design_width, canvas.design_height,
                                                 media_options_.value().with_cluster, display,
                                                 zoom_);
            draw->AddImage(static_cast<ImTextureID>(view(splash_).id),
                           ImVec2(float(logo.value().x), float(logo.value().y)),
                           ImVec2(float(logo.value().x + logo.value().width),
                                  float(logo.value().y + logo.value().height)),
                           ImVec2(media::splash_uv[0], media::splash_uv[1]),
                           ImVec2(media::splash_uv[2], media::splash_uv[3]));
        }
    }
}

Json App::canvas_proof() const {
    int width = 0, height = 0, points_width = 0, points_height = 0;

    SDL_GetWindowSizeInPixels(window_, &width, &height);
    SDL_GetWindowSize(window_, &points_width, &points_height);
    const auto &io = ImGui::GetIO();
    const auto fit = media::window_canvas(zoom_, media_options_.value().with_cluster, width,
                                          height);
    auto result = Json{
            {"window_pixels", {width, height}},
            {"window_points", {points_width, points_height}},
            {"pixel_density", SDL_GetWindowPixelDensity(window_)},
            {"display_scale", SDL_GetWindowDisplayScale(window_)},
            {"imgui_logical_size", {io.DisplaySize.x, io.DisplaySize.y}},
            {"framebuffer_scale", {io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y}},
            {"source_display_pixels", {1920, 720}},
            {"capture", "native framebuffer pixels; no resize"}};

    if (fit) {
        result["design_size"] = {fit->design_width, fit->design_height};
        result["content_pixels"] = {fit->content.x, fit->content.y, fit->content.width,
                                    fit->content.height};
        const auto row = media::media_row(fit->design_width, fit->design_height,
                                          media_options_.value().with_cluster, zoom_);
        result["display_pixels"] = {row.value().width / 2 * fit->scale_x,
                                    row.value().height * fit->scale_y};
    }

    if (const auto requested = media::canvas_geometry(zoom_, 1,
                                                      media_options_.value().with_cluster)) {
        result["requested_pixels"] = {requested->pixel_width, requested->pixel_height};
    }

    return result;
}

void App::write_standalone_proof(bool backend_connected) {
    const auto metadata = options_.capture_ui.parent_path() /
                          (options_.capture_ui.stem().string() + ".ui.json");
    auto proof = ui_->inspection();

    proof["renderer"] = renderer_name_;
    proof["chinese_glyph"] = chinese_glyph();
    proof["window"] = {{"zoom", zoom_},
                       {"language", ui::language_tag(ui::current_language)},
                       {"backend_connected", backend_connected}};
    proof["background"] = background_;
    proof["activation_requested"] = !background_;
    proof["first_show"] = {{"window_foreground", window_focus_}};
    std::ofstream output(metadata);
    output << proof.dump(2) << '\n';

    if (!output) {
        throw voyemu::VoyemuError("WINDOW", "Cannot write own UI inspection metadata");
    }
}

void App::write_media_proof() {
    Json proof = {{"schema", 1},
                  {"source", "own SDL3 canvas; authenticated raw SDK frames"},
                  {"touches_released", media_state_.touches_released},
                  {"received_frames", media_state_.received},
                  {"ready", media_state_.ready},
                  {"powered", media_state_.powered},
                  {"input_rpc_acknowledgements", media_state_.input_acknowledgements},
                  {"display_wake_gestures", media_state_.display_wake_gestures},
                  {"stream_failures", media_state_.stream_failures},
                  {"error", media_state_.error},
                  {"renderer", renderer_name_},
                  {"background", background_},
                  {"activation_requested", !background_},
                  {"first_show", {{"window_foreground", window_focus_}}},
                  {"zoom", zoom_},
                  {"language", ui::language_tag(ui::current_language)}};

    proof["canvas_geometry"] = canvas_proof();
    proof["chinese_glyph"] = chinese_glyph();
    proof["startup_frames"] = Json::array({startup_[0].frames, startup_[1].frames});

    for (unsigned slot = 0; slot < startup_videos_.size(); ++slot) {
        const auto &video = startup_videos_[slot];

        if (video.animation) {
            proof["startup_video"][startup_video_names_[slot]] = {{"frames", video.frames},
                                                                  {"playing", bool(video.player)},
                                                                  {"ended", video.ended},
                                                                  {"error", video.error}};
        }
    }

    if (!extra_.empty()) {
        auto &screens = proof["screens_window"];
        screens = {{"open", bool(screens_window_)}, {"screens", Json::array()}};

        for (const auto &extra : extra_) {
            screens["screens"].push_back({{"role", extra.screen.role},
                                          {"guest", extra.screen.guest},
                                          {"size", {extra.screen.width, extra.screen.height}},
                                          {"frame_shown", bool(extra.displayed)},
                                          {"startup_frames", extra.startup.frames}});
        }

        if (screens_window_) {
            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(screens_window_, &width, &height);
            screens["window_pixels"] = {width, height};
            screens["pixel_density"] = SDL_GetWindowPixelDensity(screens_window_);
        }
    }

    proof["cluster_ui_inspection"] = ui_->inspection();
    proof.update(smoke_proof_);

    if (keyboard_patches_) {
        std::lock_guard lock(keyboard_patches_->mutex);
        proof["keyboard_input_patches"] = keyboard_patches_->values;
    }

    voyemu::write_private_json(media_options_.value().session / "media-native-proof/result.json",
                               proof);
}

double App::elapsed() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

void App::play_startup_video(unsigned slot, std::optional<double> seconds) {
    auto &video = startup_videos_[slot];

    if (!video.animation) {
        return;
    }

    if (!seconds) {
        video.player.reset();
        video.shown = false;
        video.ended = false;

        return;
    }

    if (video.ended) {
        return;
    }

    if (!video.player) {
        // The videos of all screens start together and share one decoding budget.
        const auto videos = unsigned(std::count_if(startup_videos_.begin(), startup_videos_.end(),
                                                   [](const StartupVideo &each) {
                                                       return each.animation.has_value();
                                                   }));
        const auto workers = voyemu::window::startup_video_workers(videos);
        video.player = std::make_unique<voyemu::window::StartupVideoPlayer>(*video.animation,
                                                                            workers);
        voyemu::logging::write(voyemu::logging::Level::debug, "window", "startup_video.started",
                               {{"screen", startup_video_names_[slot]},
                                {"seconds", video.player->seconds()},
                                {"workers", workers}});
    }

    if (const auto frame = video.player->take(*seconds)) {
        // Videos of the screens window go to its renderer while it is open.
        auto *texture = video.texture;

        for (auto &extra : extra_) {
            if (extra.video == slot && screens_renderer_) {
                if (!extra.video_texture) {
                    extra.video_texture =
                            stream_texture(screens_renderer_, SDL_PIXELFORMAT_ABGR8888, true,
                                           video.animation->width, video.animation->height);
                }

                texture = extra.video_texture;
            }
        }

        if (texture) {
            need(SDL_UpdateTexture(texture, nullptr, frame->image.rgba.data(),
                                   frame->image.width * 4),
                 "WINDOW_STARTUP: cannot upload the startup video frame");
        }

        video.shown = true;
        video.frames.push_back(frame->index);
    }

    if (video.player->finished(*seconds)) {
        video.error = video.player->error();

        if (!video.error.empty()) {
            voyemu::logging::write(
                    voyemu::logging::Level::error, "window", "startup_video.failed",
                    {{"screen", startup_video_names_[slot]}, {"reason", video.error}});
        }

        voyemu::logging::write(
                voyemu::logging::Level::debug, "window", "startup_video.ended",
                {{"screen", startup_video_names_[slot]}, {"shown", video.frames.size()}});
        video.player.reset();
        video.shown = false;
        video.ended = true;
        video.ended_at = elapsed();
    }
}

// Steps run in order, each at least `delay` seconds after the previous one.
void App::schedule(double delay, std::function<void()> step) {
    const double previous = smoke_.empty() ? elapsed() : smoke_.back().at;

    smoke_.push_back({std::max(previous, elapsed()) + delay, std::move(step)});
}

// The center of an inspected UI item, from the logical canvas back into window coordinates.
std::pair<float, float> App::window_point(const Json &item) const {
    const auto box = item.at("rect_points").get<std::array<double, 4>>();
    int width = 0, height = 0;

    SDL_GetWindowSizeInPixels(window_, &width, &height);
    const auto canvas = media::window_canvas(zoom_, media_options_.value().with_cluster, width,
                                             height);

    if (!canvas) {
        throw voyemu::VoyemuError("WINDOW_INPUT_SMOKE", "canvas unavailable");
    }

    const double scale = canvas->scale_x / std::max(SDL_GetWindowPixelDensity(window_), 1.f);

    return {float((box[0] + box[2] / 2) * scale), float((box[1] + box[3] / 2) * scale)};
}

// The OEM navigation corner (72, 660) of an Android screen, in window coordinates.
std::pair<float, float> App::android_point(unsigned display) const {
    int width = 0, height = 0;

    SDL_GetWindowSizeInPixels(window_, &width, &height);
    const auto canvas = media::window_canvas(zoom_, media_options_.value().with_cluster, width,
                                             height);

    if (!canvas) {
        throw voyemu::VoyemuError("WINDOW_INPUT_SMOKE", "canvas unavailable");
    }

    const auto row = media::media_row(canvas->content.width, canvas->content.height,
                                      media_options_.value().with_cluster, zoom_);
    const double density = std::max(SDL_GetWindowPixelDensity(window_), 1.f);

    return {float((row.value().x + (display * 1920 + 72) * row.value().width / 3840) / density),
            float((row.value().y + 660 * row.value().height / 720) / density)};
}

void App::push_mouse(SDL_EventType type, std::pair<float, float> point, SDL_Window *target) const {
    SDL_Event event{};

    event.type = type;

    if (type == SDL_EVENT_MOUSE_MOTION) {
        event.motion.windowID = SDL_GetWindowID(target ? target : window_);
        event.motion.x = point.first;
        event.motion.y = point.second;
    } else {
        event.button.windowID = SDL_GetWindowID(target ? target : window_);
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.clicks = 1;
        event.button.x = point.first;
        event.button.y = point.second;
    }

    need(SDL_PushEvent(&event), "WINDOW_INPUT_SMOKE: cannot queue a mouse event");
}

// The centre of a screen of the screens window, in its window coordinates.
std::pair<float, float> App::screen_point(std::size_t screen) const {
    const auto canvas = media::screens_canvas(screen_sizes(), zoom_);
    int width = 0, height = 0;

    SDL_GetWindowSize(screens_window_, &width, &height);

    if (!canvas || width <= 0 || height <= 0) {
        throw voyemu::VoyemuError("WINDOW_INPUT_SMOKE", "screens window unavailable");
    }

    const auto &area = canvas->screens.at(screen);

    return {float((area.x + area.width / 2) * width / canvas->pixel_width),
            float((area.y + area.height / 2) * height / canvas->pixel_height)};
}

void App::push_key(SDL_EventType type, SDL_Keycode key, SDL_Scancode scancode) const {
    SDL_Event event{};

    event.type = type;
    event.key.windowID = SDL_GetWindowID(window_);
    event.key.key = key;
    event.key.scancode = scancode;
    event.key.down = type == SDL_EVENT_KEY_DOWN;
    need(SDL_PushEvent(&event), "WINDOW_INPUT_SMOKE: cannot queue a key event");
}

// Committed text as the platform IME delivers it; `text` is a static literal.
void App::push_text(const char *text) const {
    SDL_Event event{};

    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = SDL_GetWindowID(window_);
    event.text.text = text;
    need(SDL_PushEvent(&event), "WINDOW_INPUT_SMOKE: cannot queue text input");
}

// Move and press in one frame, release in a later frame at the same point.
void App::click(std::function<Json()> item, std::function<void()> released) {
    schedule(.15, [this, item = std::move(item)] {
        smoke_point_ = window_point(item());
        push_mouse(SDL_EVENT_MOUSE_MOTION, smoke_point_);
        push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, smoke_point_);
    });
    schedule(.12, [this, released = std::move(released)] {
        push_mouse(SDL_EVENT_MOUSE_MOTION, smoke_point_);
        push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, smoke_point_);

        if (released) {
            released();
        }
    });
}

// Press in one frame and release in the next, as a tap whose release is queued before the next
// frame: ImGui sees the key down in a single frame, so a loaded frame longer than its key repeat
// delay never turns the tap into a repeated key.
void App::press_key(SDL_Keycode key, SDL_Scancode scancode) {
    schedule(.15, [this, key, scancode] {
        push_key(SDL_EVENT_KEY_DOWN, key, scancode);
    });
    smoke_.push_back({smoke_.back().at,
                      [this, key, scancode] {
                          push_key(SDL_EVENT_KEY_UP, key, scancode);
                      },
                      true});
}

// --input-smoke: a parameters tab click (with capture) and, once Android is ready, a click into
// each Android screen followed by Home. --keyboard-smoke: cluster numeric and search editing.
void App::plan_smokes() {
    if (!media_options_) {
        return;
    }

    if (media_options_->input_smoke && media_options_->capture && !panel_smoke_ && elapsed() >= 1) {
        panel_smoke_ = true;
        click(
                [this] {
                    return ui_->inspection().at("tabs").at(1);
                },
                [this] {
                    smoke_proof_["panel_input_up_s"] = elapsed();
                });
    }

    // With screens outside the main window the input smoke opens their window with the toggle next
    // to the wheel.
    if (media_options_.value().input_smoke && media_options_.value().capture && !extra_.empty() &&
        !screens_smoke_ && elapsed() >= 1.5) {
        screens_smoke_ = true;
        click(
                [this] {
                    for (const auto &item : ui_->inspection().at("fields")) {
                        if (item.at("name") == "screens") {
                            return item;
                        }
                    }

                    throw voyemu::VoyemuError("WINDOW_INPUT_SMOKE", "screens toggle unavailable");
                },
                [this] {
                    smoke_proof_["screens_radio_up_s"] = elapsed();
                });
    }

    // Once Android is ready and the screens window is open, a touch on each of its Android screens.
    if (media_options_.value().input_smoke && media_state_.ready && screens_window_ &&
        !screens_input_) {
        screens_input_ = true;

        for (std::size_t screen = 0; screen < extra_.size(); ++screen) {
            if (!extra_[screen].screen.android_display) {
                continue;
            }

            schedule(.15, [this, screen] {
                if (screens_window_) {
                    push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, screen_point(screen), screens_window_);
                    smoke_proof_["screens_input"][extra_[screen].screen.role] = {
                            {"acknowledgements_before", media_state_.input_acknowledgements}};
                }
            });
            schedule(.05, [this, screen] {
                if (screens_window_) {
                    push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, screen_point(screen), screens_window_);
                }
            });
        }
    }

    if (media_options_.value().input_smoke && media_state_.ready && !android_smoke_) {
        android_smoke_ = true;

        for (unsigned display = 0; display < 2; ++display) {
            schedule(display ? .15 : 2, [this, display] {
                smoke_point_ = android_point(display);
                push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, smoke_point_);
            });
            schedule(.05, [this] {
                push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, smoke_point_);
            });
        }

        press_key(SDLK_HOME, SDL_SCANCODE_HOME);
        schedule(1, [this] {
            after_input_ = true;
        });
    }

    if (media_options_.value().keyboard_smoke && !keyboard_smoke_ && elapsed() >= 2) {
        keyboard_smoke_ = true;
        const auto field = [this](std::string name) {
            return [this, name] {
                for (const auto &item : ui_->inspection().at("fields")) {
                    if (item.at("name") == name) {
                        return item;
                    }
                }

                throw voyemu::VoyemuError("WINDOW_KEYBOARD_SMOKE", "field unavailable: " + name);
            };
        };
        const auto record = [this](const char *name) {
            schedule(.15, [this, name] {
                auto *input = ImGui::GetInputTextState(ImGui::GetActiveID());
                smoke_proof_["keyboard_input"][name] = input ? input->GetText() : "";
            });
        };
        const auto type = [this](const char *text) {
            schedule(.15, [this, text] {
                push_text(text);
            });
        };
        click(field("speed"));

        for (int i = 0; i < 3; ++i) {
            press_key(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE);
        }

        record("numeric_empty");
        type("6");
        type("5");
        record("numeric_replaced");
        click([this] {
            return ui_->inspection().at("tabs").at(4);
        });
        click(field("raw_search"));

        for (const char *letter : {"s", "p", "e", "e", "d"}) {
            type(letter);
        }

        record("search");
        schedule(0, [this] {
            auto &names = smoke_proof_["keyboard_input"]["search_fields"];
            names = Json::array();

            for (const auto &item : ui_->inspection().at("fields")) {
                if (item.at("kind") == "numeric") {
                    names.push_back(item.at("name"));
                }
            }
        });
        type("\xc3\xa9");
        record("search_utf8");
        press_key(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE);
        record("search_after_backspace");
    }
}

int App::run() {
    create_window();
    ui::current_language = initial_language();

    if (media_options_ && media_options_->inspect_language) {
        ui::current_language = *media_options_->inspect_language;
    }

    voyemu::window_shell::set_quit_title(ui::tr("Завершить Voyemu"));
    restore_placement();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imgui_ = true;
    auto &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigInputTrickleEventQueue = false;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    auto &style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 4;
    style.FontSizeBase = 13;
    cluster::load_ui_fonts(options_.fonts);

    if (!ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_) ||
        !ImGui_ImplSDLRenderer3_Init(renderer_)) {
        throw voyemu::VoyemuError("WINDOW_UI", "SDL backend initialization failed");
    }

    // The IME candidate window follows the logical canvas back into window coordinates.
    platform_ime = ImGui::GetPlatformIO().Platform_SetImeDataFn;
    ImGui::GetPlatformIO().Platform_SetImeDataFn = [](ImGuiContext *context,
                                                      ImGuiViewport *viewport,
                                                      ImGuiPlatformImeData *data) {
        if (!platform_ime) {
            return;
        }

        auto native = *data;
        native.InputPos = ImVec2(data->InputPos.x * ime_scale_x, data->InputPos.y * ime_scale_y);
        native.InputLineHeight *= ime_scale_y;
        platform_ime(context, viewport, &native);
    };
    cluster::Client::Transport keyboard_transport;

    if (media_options_ && media_options_->keyboard_smoke) {
        // The keyboard smoke edits a local canonical state instead of the session cluster.
        keyboard_patches_ = std::make_shared<Patches>();
        constexpr auto catalog = voyemu::VehicleCatalog::qinggan09;
        keyboard_transport =
                [patches = keyboard_patches_, id = media_options_->session.filename().string(),
                 canonical = voyemu::apply_patch(voyemu::initial_state(catalog),
                                                 {{"speed", 114}, {"speed10x", 1140}})](
                        const Json &operation) mutable {
                    if (operation.at("op") == "patch") {
                        canonical = voyemu::apply_patch(canonical, operation.at("values"));
                        std::lock_guard lock(patches->mutex);
                        patches->values.push_back(operation.at("values"));
                    }

                    return Json{{"ok", true},
                                {"session_id", id},
                                {"revision", canonical.at("sequence")},
                                {"state", canonical},
                                {"catalog", voyemu::catalog_json(catalog)}};
                };
    }

    ui_ = std::make_unique<cluster::Window>(options_, std::move(keyboard_transport));
    std::unique_ptr<voyemu::frames::Reader> reader;

    if (cluster_visible() && !options_.frame.empty()) {
        reader = std::make_unique<voyemu::frames::Reader>(options_.frame);
    }

    load_textures();
    {
        std::vector<std::string> roles;

        for (const auto &extra : extra_) {
            roles.push_back(extra.screen.role);
        }

        ui_->set_screens(std::move(roles));
    }
    std::uint64_t sequence{}, qnx_timestamp{}, qnx_generation{};
    bool qnx_active{};

    if (!options_.replay_frame.empty()) {
        const auto initial = read_replay(options_.replay_frame);
        qnx_ = own(stream_texture(renderer_, SDL_PIXELFORMAT_ARGB8888, false));
        need(SDL_UpdateTexture(qnx_, nullptr, initial.bgra.data(), 7680),
             "WINDOW_TEXTURE: cannot upload replay frame");
        sequence = 1;
    }

    if (media_options_) {
        multimedia = std::make_unique<media::MediaView>(*media_options_);
    }

    SDL_ShowWindow(window_);

    if (background_) {
        voyemu::window_shell::send_to_back(window_);
    }

    SDL_SyncWindow(window_);
    window_focus_ = SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS;
    bool vsync = false;
    {
        int interval = 0;
        vsync = SDL_GetRenderVSync(renderer_, &interval) && interval != 0;
    }
    bool captured_splash{}, captured_media{}, captured_after_input{}, captured_off{},
            captured_first{};
    started_ = std::chrono::steady_clock::now();
    bool final_frame = false;

    while (!final_frame) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (screens_event(event) || media_event(event)) {
                continue;
            }

            ImGui_ImplSDL3_ProcessEvent(&event);

            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                release_media();
                closing_ = true;
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                release_media();
                media_keyboard_ = false;
                ui_->focus_lost();
                break;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                apply_scale();
                break;
            case SDL_EVENT_WINDOW_MOVED:
                save_deadline_ = SDL_GetTicks() + 250;
                break;
            default:
                break;
            }
        }

        if (save_deadline_ && SDL_GetTicks() >= save_deadline_) {
            save_placement();
        }

        const bool minimized = SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED;

        if (multimedia) {
            media_state_ = multimedia->snapshot();
            multimedia->visible(!minimized);

            if (media_state_.stopped) {
                closing_ = true;
            }
        }

        plan_smokes();
        ++frame_;

        if (!smoke_.empty() && (smoke_.front().next_frame ? frame_ > smoke_frame_
                                                          : elapsed() >= smoke_.front().at &&
                                                                    frame_ > smoke_frame_ + 1)) {
            const auto step = std::move(smoke_.front().run);
            smoke_.pop_front();
            smoke_frame_ = frame_;
            step();
        }

        const bool deadline = options_.smoke_seconds > 0 && elapsed() >= options_.smoke_seconds;
        final_frame = closing_ || deadline;

        if (minimized && !final_frame) {
            SDL_WaitEventTimeout(nullptr, 100);
            continue;
        }

        // Android text reaches the guest only after a click into its screens.
        const bool want_media_text = media_keyboard_ && !io.WantTextInput;

        if (want_media_text && !SDL_TextInputActive(window_)) {
            SDL_StartTextInput(window_);
            media_text_ = true;
        } else if (!media_keyboard_ && media_text_ && !io.WantTextInput) {
            SDL_StopTextInput(window_);
            media_text_ = false;
        }

        ui_->refresh_state();
        voyemu::window_shell::set_light_appearance(window_, ui_->light_theme());

        if (ui_->about_open()) {
            if (const auto colors = voyemu::window_shell::title_colors(window_)) {
                ui_->set_title_colors(
                        cluster::Window::TitleColors{colors->background, colors->text});
            } else {
                ui_->set_title_colors(std::nullopt);
            }

            const auto title_font = voyemu::window_shell::title_font_pixels(window_);
            ui_->set_title_font_size(
                    title_font ? std::optional<float>(*title_font / io.DisplayFramebufferScale.x)
                               : std::nullopt);
        }

        if (reader) {
            if (media_options_) {
                const auto active = ui_->cluster_active();
                const auto generation = ui_->cluster_generation();

                if (active != qnx_active || generation != qnx_generation) {
                    qnx_active = active;
                    qnx_generation = generation;
                    sequence = qnx_timestamp = 0;
                    captured_first = false;
                    reader = std::make_unique<voyemu::frames::Reader>(options_.frame);
                }
            }

            try {
                if (!media_options_ || qnx_active) {
                    if (auto latest = reader->read_latest(sequence)) {
                        if (!qnx_) {
                            qnx_ = own(stream_texture(renderer_, SDL_PIXELFORMAT_ARGB8888, false));
                        }

                        need(SDL_UpdateTexture(qnx_, nullptr, latest->bgra.data(), 7680),
                             "WINDOW_TEXTURE: cannot upload cluster frame");
                        sequence = latest->sequence;
                        qnx_timestamp = latest->timestamp_ns;
                        ui_->frame_error({});
                    }
                }

                if (reader->invalidated()) {
                    sequence = qnx_timestamp = 0;
                }
            } catch (const std::exception &error) {
                ui_->frame_error(error.what());
            }
        }

        if (multimedia && media_state_.ready) {
            for (unsigned display = 0; display < 2; ++display) {
                if (media_state_.frames[display] &&
                    displayed_[display] != media_state_.frames[display]) {
                    if (!android_[display]) {
                        android_[display] =
                                own(stream_texture(renderer_, SDL_PIXELFORMAT_ABGR8888, true));
                    }

                    need(SDL_UpdateTexture(android_[display], nullptr,
                                           media_state_.frames[display]->pixels().data(), 7680),
                         "WINDOW_TEXTURE: cannot upload Android frame");
                    displayed_[display] = media_state_.frames[display];
                    auto &baseline = reaction_baseline_[display];

                    if (baseline &&
                        media::scene_changed(baseline->pixels(), displayed_[display]->pixels())) {
                        smoke_proof_["input_reaction"][std::to_string(display)]
                                    ["changed_frame_drawn_ms"] =
                                            (elapsed() - reaction_started_[display]) * 1000;
                        baseline.reset();
                    }
                }
            }
        } else {
            displayed_ = {};
        }

        upload_screen_frames();

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        int pixel_width = 0, pixel_height = 0, width = 0, height = 0;
        SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
        SDL_GetWindowSize(window_, &width, &height);

        if (width <= 0 || height <= 0) {
            continue;
        }

        const double density_x = double(pixel_width) / width,
                     density_y = double(pixel_height) / height;
        std::optional<media::WindowCanvas> canvas;

        if (media_options_) {
            canvas = media::window_canvas(zoom_, media_options_->with_cluster, pixel_width,
                                          pixel_height);

            if (!canvas) {
                continue;
            }

            io.DisplaySize = ImVec2(float(canvas->logical_width), float(canvas->logical_height));
            io.DisplayFramebufferScale = ImVec2(float(canvas->scale_x), float(canvas->scale_y));
        } else {
            const float scale = std::max(SDL_GetWindowDisplayScale(window_), 1.f);
            io.DisplaySize = ImVec2(pixel_width / scale, pixel_height / scale);
            io.DisplayFramebufferScale = ImVec2(scale, scale);
        }

        // Backend events use window coordinates; map them into the shared logical canvas.
        voyemu::media::normalize_mouse_events(*ImGui::GetCurrentContext(), mapped_mouse_event_,
                                              density_x / io.DisplayFramebufferScale.x,
                                              density_y / io.DisplayFramebufferScale.y);
        ime_scale_x = float(io.DisplayFramebufferScale.x / density_x);
        ime_scale_y = float(io.DisplayFramebufferScale.y / density_y);
        ImGui::NewFrame();
        const auto displayed_qnx =
                media_options_ && !ui_->frame_presentable(qnx_timestamp) ? 0 : sequence;
        const auto startup_seconds =
                startup_trigger_.update(media_state_.powered && !media_state_.ready, elapsed());

        for (unsigned slot = 0; slot < startup_videos_.size(); ++slot) {
            play_startup_video(slot, startup_seconds);
        }

        // The video spans the whole cluster frame at any frame size, as the OEM player fills the
        // cluster display.
        const auto &video = startup_videos_[2];
        const auto cluster_frame = video.playing(bool(startup_seconds)) && video.shown
                                           ? cluster::Texture{view(video.texture).id, 1920, 720}
                                   : displayed_qnx ? view(qnx_)
                                                   : cluster::Texture{};
        ui_->draw(cluster_frame, view(wheel_left_), view(wheel_right_),
                  cluster::ui_layout_scale(zoom_));

        if (smoke_proof_.contains("panel_input_up_s") &&
            !smoke_proof_.contains("panel_input_drawn_ms") &&
            ui_->inspection().value("selected_section", 0u) == 1) {
            smoke_proof_["panel_input_drawn_ms"] =
                    (elapsed() - smoke_proof_.at("panel_input_up_s").get<double>()) * 1000;
        }

        if (media_options_) {
            draw_media(*canvas);
        }

        ImGui::Render();
        SDL_SetRenderDrawColorFloat(renderer_, .055f, .06f, .07f, 1);
        SDL_RenderClear(renderer_);
        // The SDL renderer backend expects the logical-to-pixel scale on the renderer.
        SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_SetRenderScale(renderer_, 1, 1);

        if (media_options_ && media_options_->capture) {
            // Own milestone captures of the actual rendered target, before present.
            std::string milestone;

            if (!media_state_.ready && !captured_splash) {
                milestone = "splash.png";
                captured_splash = true;
            } else if (media_state_.ready && !captured_media) {
                milestone = "multimedia.png";
                captured_media = true;
            } else if (media_state_.ready && after_input_ && !captured_after_input) {
                milestone = "after-input.png";
                captured_after_input = true;
            } else if (media_options_->with_cluster && !qnx_active && qnx_generation &&
                       !captured_off) {
                milestone = "cluster-off.png";
                captured_off = true;
            } else if (displayed_qnx && !captured_first) {
                milestone = "cluster-first-g" + std::to_string(qnx_generation) + ".png";
                captured_first = true;
            }

            if (!milestone.empty()) {
                capture(renderer_, media_options_->session / "media-native-proof" / milestone);
            }
        }

        if (final_frame && !options_.capture_ui.empty()) {
            capture(renderer_, options_.capture_ui);
            write_standalone_proof(displayed_qnx != 0);
        }

        SDL_RenderPresent(renderer_);

        if (screens_window_) {
            draw_screens_window(media_state_.powered && !media_state_.ready, final_frame);
        }

        apply_shell_request();

        if (!vsync) {
            SDL_WaitEventTimeout(nullptr, 16);
        }
    }

    save_placement();
    release_media();

    if (multimedia) {
        multimedia->stop();
        media_state_ = multimedia->snapshot();
        write_media_proof();
        multimedia.reset();
    }

    ui_->close();

    return media_options_ && !media_state_.error.empty() ? 1 : 0;
}
} // namespace

int voyemu::window_main(int argc, char **argv) {
    try {
        App app(argc, argv);

        return app.run();
    } catch (const std::exception &error) {
        if (multimedia) {
            multimedia->stop();
            multimedia.reset();
        }

        voyemu::logging::try_write(voyemu::logging::Level::error, "window", error.what());

        if (!background_arguments(argc, argv)) {
            SDL_ShowSimpleMessageBox(
                    SDL_MESSAGEBOX_ERROR, "Voyemu",
                    (std::string("Voyemu could not start: ") + error.what()).c_str(), nullptr);
        }

        return 1;
    }
}
