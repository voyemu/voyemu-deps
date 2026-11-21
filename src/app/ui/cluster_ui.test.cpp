#include "cluster_ui.hpp"
#include "cluster_font.hpp"
#include "control_drawing.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "media_imgui_input.hpp"
#include "parameters_layout.hpp"
#include "qinggan11_startup.hpp"
#include "ui_localization.hpp"
#include "wheel_geometry.hpp"
#include "window_image.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
namespace {
// Decode the canonical A11 wheel photos through the same production decoder as the window.
std::vector<std::uint8_t> decode_photo_rgba(const std::filesystem::path &path) {
    auto image = voyemu::window::decode_png(path);

    if (image.width != 900 || image.height != 770) {
        throw std::runtime_error("A11 wheel photo keeps the exact original size");
    }

    return std::move(image.rgba);
}
} // namespace

using namespace voyemu;
using namespace voyemu::cluster;
namespace {
// The fixture font directory that stands for the fonts of a machine.
std::filesystem::path fonts_directory;

void check(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Predicate> void wait(Predicate predicate, const char *message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(message);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool same_rgb(ImU32 a, ImU32 b) {
    return (a & ~IM_COL32_A_MASK) == (b & ~IM_COL32_A_MASK);
}

// Calls visit for every indexed vertex of the current draw data with its draw command.
template <class Visit> void each_vertex(Visit visit) {
    for (const auto *list : ImGui::GetDrawData()->CmdLists) {
        for (const auto &command : list->CmdBuffer) {
            for (unsigned i = 0; i < command.ElemCount; ++i) {
                visit(list->VtxBuffer[command.VtxOffset + list->IdxBuffer[command.IdxOffset + i]],
                      command);
            }
        }
    }
}

// A vertex of the selected radio capsule, whose colour follows its height in the segment.
bool selection_vertex(const ImDrawVert &vertex, bool light, float top, float height) {
    return same_rgb(vertex.col, radio_selection_color(light, (vertex.pos.y - top) / height));
}

void appearance_following(const std::filesystem::path &assets) {
    std::mutex mutex;
    std::string scheme = "dark";
    const auto state = initial_state(VehicleCatalog::qinggan09);
    auto transport = [&](const Json &operation) {
        if (operation.at("op") != "status") {
            return Json{{"ok", true}, {"accepted", true}};
        }

        std::lock_guard lock(mutex);

        return Json{
                {"ok", true},
                {"session_id", "appearance-test"},
                {"revision", state.at("sequence")},
                {"state", state},
                {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                {"deliveries",
                 {{"android", {{"appearance", {{"status", "observed"}, {"scheme", scheme}}}}}}}};
    };
    Options options;

    options.assets = assets;
    options.endpoint = "appearance-fixture";
    options.smoke_seconds = .2;
    Window window(options, transport);
    wait(
            [&] {
                window.refresh_state();

                return window.client().state().ready;
            },
            "appearance fixture becomes ready");
    check(!window.light_theme(), "default appearance is dark");
    {
        std::lock_guard lock(mutex);
        scheme = "light";
    }
    wait(
            [&] {
                window.refresh_state();

                return window.light_theme();
            },
            "OEM light scheme updates without a window restart");
    {
        std::lock_guard lock(mutex);
        scheme = "unrecognized";
    }
    wait(
            [&] {
                return window.client()
                               .state()
                               .deliveries.value("android", Json::object())
                               .value("appearance", Json::object())
                               .value("scheme", std::string{}) == "unrecognized";
            },
            "unknown appearance reaches the fixture");
    window.refresh_state();
    check(window.light_theme(), "unknown scheme preserves the last observed palette");
    {
        std::lock_guard lock(mutex);
        scheme = "dark";
    }
    wait(
            [&] {
                window.refresh_state();

                return !window.light_theme();
            },
            "returning to OEM default restores dark");
}

void startup_frame_gate(const std::filesystem::path &assets) {
    std::mutex mutex;
    auto canonical = initial_state(VehicleCatalog::qinggan09);
    Json qnx = {{"status", "booting"}, {"generation", 1}, {"lifecycle_supported", true}};
    auto transport = [&](const Json &operation) {
        if (operation.at("op") != "status") {
            return Json{{"ok", true}, {"accepted", true}};
        }

        std::lock_guard lock(mutex);

        return Json{{"ok", true},
                    {"session_id", "first-frame-test"},
                    {"revision", canonical.at("sequence")},
                    {"state", canonical},
                    {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                    {"deliveries", {{"qnx", qnx}}}};
    };
    Options options;

    options.assets = assets;
    options.endpoint = "first-frame-fixture";
    options.smoke_seconds = .2;
    Window window(options, transport);
    auto sync = [&] {
        wait(
                [&] {
                    window.refresh_state();

                    return window.client().state().ready &&
                           window.client().state().deliveries.at("qnx") == qnx;
                },
                "startup delivery reaches the render client");
    };
    auto fence = [&](unsigned generation, std::uint64_t timestamp) {
        return Json{{"status", "vehicle_acknowledged"},
                    {"generation", generation},
                    {"vehicle_revision", canonical.at("sequence")},
                    {"after_timestamp_ns", timestamp}};
    };
    sync();
    check(!window.frame_presentable(90), "snapshot frame must stay hidden until current-state ACK");
    {
        std::lock_guard lock(mutex);
        qnx["presentation"] = fence(1, 100);
    }
    sync();
    check(!window.frame_presentable(90) && !window.frame_presentable(100) &&
                  window.frame_presentable(101),
          "only a completed frame strictly after the matching ACK opens the viewport");
    {
        std::lock_guard lock(mutex);
        canonical = apply_patch(canonical, {{"speed", 50}, {"speed10x", 500}});
        qnx["presentation"] = Json::object();
    }
    sync();
    check(window.frame_presentable(150) && !window.frame_presentable(50),
          "live model edits keep the first-generation gate open without permitting old frame timestamps");
    {
        std::lock_guard lock(mutex);
        qnx["generation"] = 2;
    }
    sync();
    check(!window.frame_presentable(500), "restart resets the accepted frame gate");
    {
        std::lock_guard lock(mutex);
        qnx["presentation"] = fence(1, 400);
    }
    sync();
    check(!window.frame_presentable(500), "previous-generation ACK cannot expose a new viewport");
    {
        std::lock_guard lock(mutex);
        qnx["presentation"] = fence(2, 400);
        qnx["presentation"]["vehicle_revision"] = 0;
    }
    sync();
    check(!window.frame_presentable(500), "old canonical revision cannot expose startup pixels");
    {
        std::lock_guard lock(mutex);
        qnx["presentation"] = fence(2, 400);
    }
    sync();
    check(!window.frame_presentable(400) && window.frame_presentable(401),
          "new generation opens with its own ACK and new frame");
    {
        std::lock_guard lock(mutex);
        qnx["status"] = "off";
    }
    sync();
    check(!window.frame_presentable(600), "power-off hides all cluster pixels");
    {
        std::lock_guard lock(mutex);
        qnx["status"] = "booting";
        qnx["generation"] = 3;
        qnx["presentation"] = fence(3, 0);
    }
    sync();
    check(!window.frame_presentable(600), "zero or missing clock fence never opens startup");
    {
        std::lock_guard lock(mutex);
        qnx["presentation"] = fence(3, 600);
        qnx["presentation"]["after_timestamp_ns"] = 600.5;
    }
    sync();
    check(!window.frame_presentable(601), "fractional fence metadata is rejected");
    {
        std::lock_guard lock(mutex);
        qnx["presentation"] = fence(3, 600);
    }
    sync();
    check(!window.frame_presentable(600) && window.frame_presentable(601),
          "valid final fence retains its strict edge");
    window.close();
}

void control_drawing_guards() {
    const ParameterRect track{10, 20, 200, 40};
    const auto left = radio_background_clip(track, 10, 50),
               right = radio_background_clip(track, 160, 50),
               middle = radio_background_clip(track, 60, 50),
               overshoot = radio_background_clip(track, 6, 50);

    check(left.x == 35 && left.right() == 210 && right.x == 10 && right.right() == 185,
          "selected P and D replace the white outer cap rather than compositing over it");
    check(middle.x == 10 && middle.right() == 210 && overshoot.x == 31,
          "white middle and animated edge track remain covered without a fringe");

    check(radio_selection_color(true, 0) == IM_COL32(66, 158, 253, 255) &&
                  radio_selection_color(true, 1) == radio_selection_color(true, 0),
          "the light selected capsule is the flat light accent");
    check(radio_selection_color(false, 0) == IM_COL32(85, 162, 239, 255) &&
                  radio_selection_color(false, .5f) == IM_COL32(38, 129, 221, 255) &&
                  radio_selection_color(false, 1) == IM_COL32(25, 117, 210, 255) &&
                  radio_selection_color(false, -1) == radio_selection_color(false, 0) &&
                  radio_selection_color(false, 2) == radio_selection_color(false, 1),
          "the dark selected capsule darkens from its lit top and clamps outside the segment");
    check(toggle_ink(false).knob == 20 && toggle_ink(true).knob == 17 &&
                  toggle_ink(false).on == toggle_ink(true).on && toggle_ink(false).border != 0 &&
                  toggle_ink(true).border == 0,
          "toggle knobs keep the 40 and 34 pixel sizes with one blue on fill");
    check((toggle_knob_color(false, .5f) >> IM_COL32_A_SHIFT) <
                          (toggle_knob_color(false, 0) >> IM_COL32_A_SHIFT) &&
                  toggle_knob_color(true, .5f) == toggle_knob_color(true, 0),
          "the dark knob is shaded and the light knob is flat");
    bool ascending = light_background.front().fraction == 0 &&
                     light_background.back().fraction == 1;

    for (std::size_t i = 1; i < light_background.size(); ++i) {
        ascending &= light_background[i].fraction > light_background[i - 1].fraction;
    }

    check(ascending, "the light background gradient covers the whole panel height in order");
}

void localized_tab_layout() {
    const auto layout = parameters_layout(0, 0, 0);

    for (const auto widths :
         {std::array<float, 5>{65, 35, 32, 36, 48}, std::array<float, 5>{80, 45, 50, 42, 58}}) {
        for (const bool search : {false, true}) {
            const auto tabs = parameter_tabs(layout, widths, search);
            check(tabs.items.front().x == layout.body.x + layout.content_inset(),
                  "tabs share the content left edge");

            for (std::size_t i = 1; i < 5; ++i) {
                check(tabs.items[i].x >= tabs.items[i - 1].right() + 7.99f,
                      "localized tab hit regions keep their gaps");
            }

            check(tabs.items.back().right() <= layout.body.right() + .001f,
                  "translated labels fit the panel");

            if (search) {
                check(tabs.search.x == layout.body.x + layout.content_inset() &&
                              tabs.search.width == layout.body.width - 2 * layout.content_inset() &&
                              tabs.search.y >=
                                      tabs.items.back().bottom() + 8 * layout.form_scale() &&
                              std::abs(tabs.content.y - tabs.search.bottom() -
                                       8 * layout.form_scale()) < .001f,
                      "search uses its own full width row below the tabs");
            }

            check(tabs.items.front().y > layout.tabs.y, "tab row has a top inset");
        }
    }

    const auto ru = parameter_tabs(layout, {65, 35, 32, 36, 48}, false),
               en = parameter_tabs(layout, {80, 45, 50, 42, 58}, false);
    check(ru.items[1].x != en.items[1].x, "locale change recomputes hit geometry");
}

void music_tab_animation() {
    MusicTabIndicator indicator;

    indicator.select(10, 0);
    check(indicator.position(0) == 10 && !indicator.active(0),
          "initial tab has no entrance animation");
    indicator.select(110, 1);
    check(indicator.position(1) == 10 && indicator.active(1),
          "tab selection starts at current position");
    check(indicator.position(1.3) > 110, "OEM interpolator overshoots target");
    check(std::abs(indicator.position(1.4) - 110) < 1e-9 && !indicator.active(1.4 + 1e-9),
          "400 ms settles exactly");
    indicator.select(10, 2);
    check(indicator.position(2.3) < 10, "reverse movement overshoots in reverse direction");
    const auto interrupted = indicator.position(2.15);
    indicator.select(70, 2.15);
    check(std::abs(indicator.position(2.15) - interrupted) < 1e-9,
          "rapid selection has no position discontinuity");
    indicator.snap(25, 2.2);
    check(indicator.position(2.2) == 25 && !indicator.active(2.2),
          "layout and locale changes snap without stale coordinates");
    check(MusicTabIndicator::interpolate(-1) == 0 && MusicTabIndicator::interpolate(2) == 1,
          "clock endpoints clamp safely");
}

void parsing() {
    {
        // The controls of the window read and write the concepts through the fields of the
        // platform: the Qinggan fields directly, the Passion signals in their encodings.
        ViewState passion;
        passion.platform = VehicleCatalog::passion;
        passion.raw = initial_state(VehicleCatalog::passion).at("raw_oem");
        const auto speed = concept_control(passion, VehicleConcept::speed);
        check(speed.ends_with("VI_VehicleDisplaySpeed.VehicleSpeed") &&
                      passion.raw.at(speed) == 50000 && control_value(passion, speed) == 0 &&
                      control_value(passion, concept_control(passion, VehicleConcept::ready)) ==
                              1 &&
                      control_value(passion, concept_control(passion, VehicleConcept::hood)) == 0,
              "Passion controls decode their signals");
        check(concept_control(passion, VehicleConcept::battery_voltage) == "battery_voltage" &&
                      !control_value(passion, "battery_voltage"),
              "an unbound concept has no control value");
        passion.raw[concept_control(passion, VehicleConcept::gear)] = 255;
        check(!control_value(passion, concept_control(passion, VehicleConcept::gear)),
              "an invalid gear shows no value");
        ViewState qinggan;
        qinggan.raw = initial_state(VehicleCatalog::qinggan09).at("raw_oem");
        check(concept_control(qinggan, VehicleConcept::speed) == "speed" &&
                      control_value(qinggan, "speed10x") == 0 &&
                      control_value(qinggan, "odo") == qinggan.raw.at("odo").get<std::int64_t>(),
              "Qinggan controls keep their raw fields");
    }
    check(!parse_unsigned("", 8), "empty raw draft must stay local");
    check(!parse_unsigned("-1", 8), "negative raw must reject");
    check(!parse_unsigned("256", 8), "width overflow must reject");
    check(!parse_unsigned("1.0", 8), "fractional raw must reject");
    check(!parse_unsigned("1x", 8), "partial parse must reject");
    check(parse_unsigned("255", 8) == 255, "maximum raw value");
    check(parse_unsigned("4294967295", 32) == 4294967295ULL, "32bit maximum");
    check(!parse_unsigned("1", 33), "unsupported width");
    check(parse_number("-40.5") == -40.5, "signed fractional semantic value");
    check(!parse_number("nan"), "NaN cannot enter canonical source");
    check(!parse_number("inf"), "infinity cannot enter canonical source");
    check(!parse_number("12x"), "trailing characters cannot enter source");
    check(parse_number("1e2") == 100, "finite scientific semantic value");
    check(parse_number("0.5") == .5, "locale-independent decimal point");
    check(!parse_number(" 1"), "leading whitespace must reject");
    check(!parse_number("1 "), "trailing whitespace must reject");
    check(!parse_number("1\n"), "trailing newline must reject");
    check(!parse_number("1,5"), "locale decimal comma must reject");
    check(!parse_number("1e309"), "double overflow must reject");
    check(!parse_number("1e"), "incomplete exponent must stay local");
    check(!parse_number(""), "empty semantic draft must stay local");
    const char *valid[] = {"voyemu-cluster", "--endpoint", "socket",  "--frame", "frame.bgra",
                           "--assets",       "assets",     "--fonts", "fonts"};
    auto options = parse_options(9, valid);
    check(options.frame == "frame.bgra", "live frame option");
    check(window_title(options) == "Voyemu", "standard title without a profile title");
    const char *profiled[] = {"voyemu-cluster", "--endpoint", "socket", "--frame",
                              "frame.bgra",     "--assets",   "assets", "--profile",
                              "Test 1.0",       "--fonts",    "fonts"};
    const auto named = parse_options(11, profiled);
    check(window_title(named) == "Voyemu Test 1.0", "title names the profile by its title");
    check(screens_window_title(named, "HUD") == "Voyemu Test 1.0 - HUD",
          "screens window title adds its caption");
    const char *invalid_profile[] = {"voyemu-cluster", "--endpoint", "socket", "--frame",
                                     "frame.bgra",     "--assets",   "assets", "--profile",
                                     "Test\n1.0",      "--fonts",    "fonts"};
    bool profile_rejected = false;
    try {
        parse_options(11, invalid_profile);
    } catch (...) {
        profile_rejected = true;
    }

    check(profile_rejected, "profile title with a control character must reject");
    const char *incomplete[] = {"voyemu-cluster", "--assets", "assets", "--fonts", "fonts"};
    bool rejected = false;
    try {
        parse_options(5, incomplete);
    } catch (...) {
        rejected = true;
    }

    check(rejected, "normal GUI requires actual live session/frame");
    const char *without_fonts[] = {"voyemu-cluster", "--endpoint", "socket", "--frame",
                                   "frame.bgra",     "--assets",   "assets"};
    rejected = false;
    try {
        parse_options(7, without_fonts);
    } catch (...) {
        rejected = true;
    }

    check(rejected, "the window requires the fonts of a machine");
    const char *replay[] = {"voyemu-cluster", "--replay-frame",  "original.bmp", "--assets",
                            "assets",         "--smoke-seconds", "0.25",         "--capture-ui",
                            "capture.png",    "--fonts",         "fonts"};
    check(parse_options(11, replay).smoke_seconds == .25, "explicit bounded replay smoke");
    check(!parse_options(11, replay).light_theme, "replay defaults to the dark theme");

    for (const bool light : {false, true}) {
        const char *themed[] = {
                "voyemu-cluster", "--replay-frame",         "original.bmp", "--assets", "assets",
                "--appearance",   light ? "light" : "dark", "--fonts",      "fonts"};
        check(parse_options(9, themed).light_theme == light,
              "replay selects the requested appearance");
    }

    const char *blue[] = {"voyemu-cluster", "--replay-frame", "original.bmp", "--assets", "assets",
                          "--appearance",   "blue",           "--fonts",      "fonts"};
    const char *live[] = {"voyemu-cluster", "--endpoint", "endpoint", "--frame",
                          "frame.bgra",     "--assets",   "assets",   "--appearance",
                          "light",          "--fonts",    "fonts"};

    for (const auto &[count, arguments] : {std::pair<int, const char *const *>{9, blue},
                                           std::pair<int, const char *const *>{11, live}}) {
        rejected = false;
        try {
            parse_options(count, arguments);
        } catch (...) {
            rejected = true;
        }

        check(rejected, "appearance is only light or dark and only for a replay");
    }

    check(!options.inspect_panel, "normal launch stays collapsed by default");
    const std::array<const char *, 5> panels = {"movement", "lights", "body", "tyres", "raw"};

    for (std::size_t i = 0; i < panels.size(); ++i) {
        const char *inspected[] = {
                "voyemu-cluster", "--replay-frame",  "original.bmp", "--assets",
                "assets",         "--smoke-seconds", "0.25",         "--capture-ui",
                "capture.png",    "--inspect-panel", panels[i],      "--fonts",
                "fonts"};
        check(parse_options(13, inspected).inspect_panel == i,
              "bounded own capture accepts every named panel");
        const auto number = std::to_string(i);
        inspected[10] = number.c_str();
        check(parse_options(13, inspected).inspect_panel == i,
              "bounded own capture accepts every numeric panel");
    }

    for (const auto bad : {"5", "-1", "01", "RAW", "invented"}) {
        const char *inspected[] = {"voyemu-cluster",
                                   "--replay-frame",
                                   "original.bmp",
                                   "--assets",
                                   "assets",
                                   "--smoke-seconds",
                                   "0.25",
                                   "--capture-ui",
                                   "capture.png",
                                   "--inspect-panel",
                                   bad,
                                   "--fonts",
                                   "fonts"};
        rejected = false;
        try {
            parse_options(13, inspected);
        } catch (...) {
            rejected = true;
        }

        check(rejected, "invalid inspection panels reject");
    }

    const char *unbounded[] = {"voyemu-cluster",
                               "--replay-frame",
                               "original.bmp",
                               "--assets",
                               "assets",
                               "--inspect-panel",
                               "0",
                               "--fonts",
                               "fonts"};
    rejected = false;
    try {
        parse_options(9, unbounded);
    } catch (...) {
        rejected = true;
    }

    check(rejected, "inspection cannot open an unbounded normal runtime");
    const char *duplicate[] = {"voyemu-cluster",
                               "--replay-frame",
                               "original.bmp",
                               "--assets",
                               "assets",
                               "--smoke-seconds",
                               "0.25",
                               "--capture-ui",
                               "capture.png",
                               "--inspect-panel",
                               "0",
                               "--inspect-panel",
                               "raw",
                               "--fonts",
                               "fonts"};
    rejected = false;
    try {
        parse_options(15, duplicate);
    } catch (...) {
        rejected = true;
    }

    check(rejected, "duplicate inspection flag rejects");
    const char *dual[] = {"voyemu-cluster", "--frame", "f",       "--replay-frame", "r",
                          "--assets",       "a",       "--fonts", "fonts"};
    rejected = false;
    try {
        parse_options(9, dual);
    } catch (...) {
        rejected = true;
    }

    check(rejected, "replay cannot silently override live frame");
}

void profile_wheel_layout(const std::filesystem::path &assets) {
    const char *qinggan11_env = std::getenv("VOYEMU_QINGGAN11_FIXTURE");
    const auto other = qinggan11_env ? std::filesystem::path(qinggan11_env)
                                     : assets.parent_path() / "free-2024";
    const auto geometry = read_wheel_geometry(other / "wheel-hotspots.json");

    check(geometry.regions.size() == 15 && geometry.rollers.size() == 1,
          "A11 exposes only existing generic actions and its right up/down rocker");
    check(geometry.joined_offset ==
                  read_wheel_geometry(assets / "wheel-hotspots.json").joined_offset,
          "A11 reuses the approved A9 center seam and plate spacing");
    const auto menu = std::find_if(geometry.regions.begin(), geometry.regions.end(),
                                   [](const auto &r) {
                                       return r.id == "menu";
                                   });
    const auto play = std::find_if(geometry.regions.begin(), geometry.regions.end(),
                                   [](const auto &r) {
                                       return r.id == "ok";
                                   });
    check(menu != geometry.regions.end() && menu->photo == "left" &&
                  play != geometry.regions.end() && play->photo == "right",
          "A11 cards move left and play/pause has its own upper button");

    for (const auto &r : geometry.regions) {
        check(r.id != "night_vision", "blank lower plate has no invented night-vision control");
        check(!r.contains((650. - 50) / 900, (795. - 115) / 770) || r.photo != "left",
              "blank left plate is inactive");
        check(!r.contains(((r.photo == "left" ? 567. : 433.) - 50) / 900, (525. - 115) / 770),
              "neither rocker center is a clickable button");
    }

    const auto root = std::filesystem::absolute(assets)
                              .lexically_normal()
                              .parent_path()
                              .parent_path()
                              .parent_path()
                              .parent_path();
    const auto scratch = root / ".local/build/tmp" / ("qinggan11-wheel-layout-" + random_token());
    ensure_private_directory(scratch);
    struct Cleanup {
        std::filesystem::path p;

        ~Cleanup() {
            std::error_code e;
            std::filesystem::remove_all(p, e);
        }
    } cleanup{scratch};
    std::ifstream input(other / "wheel-hotspots.json");
    Json original;
    input >> original;
    auto reject = [&](Json value) {
        const auto file = scratch / "invalid.json";
        write_private_json(file, value);
        bool rejected = false;
        try {
            read_wheel_geometry(file);
        } catch (...) {
            rejected = true;
        }

        check(rejected, "malformed profile wheel layout rejects");
    };
    auto changed = original;
    changed["schema"] = 2;
    reject(changed);
    changed = original;
    changed["joined_offset_logical_points"] = 180;
    reject(changed);
    changed = original;
    changed["regions"][0]["id"] = "invented-code";
    reject(changed);
    changed = original;
    changed["regions"][0]["photo"] = "invented-side";
    reject(changed);
    changed = original;
    changed["regions"][1]["enabled"] = "yes";
    reject(changed);
    changed = original;
    changed["rollers"][1]["up"] = "ok";
    reject(changed);
    std::ifstream legacy_input(assets / "wheel-hotspots.json");
    Json legacy;
    legacy_input >> legacy;
    changed = legacy;

    for (auto &region : changed["regions"]) {
        if (region.at("id") == "photo") {
            region["id"] = "distance_increase";
            region["source_face"] = "face-distance_increase";
        }
    }

    reject(changed);
    const auto passion_assets = assets.parent_path() / "passion-2024";
    const auto passion = read_wheel_geometry(passion_assets / "wheel-hotspots.json");
    const std::set<std::string> passion_ids = {"cruise_up",
                                               "custom",
                                               "distance_decrease",
                                               "acc",
                                               "distance_increase",
                                               "cruise_down",
                                               "menu",
                                               "voice",
                                               "volume_up",
                                               "previous",
                                               "ok",
                                               "next",
                                               "volume_down",
                                               "phone"};
    std::set<std::string> actual_ids;

    for (const auto &region : passion.regions) {
        actual_ids.insert(region.id);
    }

    check(actual_ids == passion_ids && passion.rollers.empty(),
          "Passion exposes exactly fourteen OEM touch controls without physical rollers");
    const auto passion_menu = std::find_if(passion.regions.begin(), passion.regions.end(),
                                           [](const auto &region) {
                                               return region.id == "menu";
                                           });
    check(passion_menu != passion.regions.end() && passion_menu->photo == "left",
          "profile layout permits Passion instrument menu on the independent left photo");
    std::ifstream passion_input(passion_assets / "wheel-hotspots.json");
    Json passion_source;
    passion_input >> passion_source;

    for (const auto &region : passion_source.at("regions")) {
        const auto point = region.at("interior_point_normalized").get<std::array<double, 2>>();
        const auto *hit = wheel_geometry::hit(
                passion.regions, region.at("photo").get<std::string>(), point[0], point[1]);
        check(hit && hit->id == region.at("id").get<std::string>(),
              "every Passion icon interior selects its own shared renderer hit contour");
    }
}

void regions(const std::filesystem::path &assets) {
    const auto geometry = read_wheel_geometry(assets / "wheel-hotspots.json");
    const auto all = geometry.regions;

    check(all.size() == 17, "all17physical controls");
    const auto left = std::find_if(all.begin(), all.end(), [](const auto &r) {
        return r.id == "cancel";
    });
    const auto right = std::find_if(all.begin(), all.end(), [](const auto &r) {
        return r.id == "menu";
    });
    const auto join_left = 180 * (left->rect[0] + left->rect[2]),
               join_right = 180 * right->rect[0] + geometry.joined_offset;
    check(std::abs(join_right - join_left - 2) < .0001,
          "profile contours retain the two-point center seam without stretching either wheel photo");

    for (const auto &region : all) {
        bool unique_point = false;

        // A rendered hit area must have at least one exclusive point; rectangular bounds alone are
        // insufficient for photographed button faces.
        for (int y = 1; y < 100 && !unique_point; ++y) {
            for (int x = 1; x < 100 && !unique_point; ++x) {
                const double px = region.rect[0] + region.rect[2] * x / 100,
                             py = region.rect[1] + region.rect[3] * y / 100;

                if (px < 0 || px > 1 || py < 0 || py > 1 || !region.contains(px, py)) {
                    continue;
                }

                std::size_t overlaps = 0;

                for (const auto &candidate : all) {
                    if (candidate.photo == region.photo && candidate.contains(px, py)) {
                        ++overlaps;
                    }
                }

                unique_point = overlaps == 1;
            }
        }

        check(unique_point, "a wheel button has no exclusive clickable polygon point");
        check(!region.contains(-2, -2), "out of photo bounds cannot press button");
    }

    check(photo_width == 194.4f && photo_height == 166.32f,
          "joined halves use the same approved 1.08 scale");
    check(closed_width == 1920 && closed_height == 360 && editor_height == 0,
          "approved joined native geometry stays 360 points high");
}

void panel_inspection_guards(const std::filesystem::path &assets) {
    Options normal;

    normal.assets = assets;
    Window collapsed(normal);
    check(collapsed.expanded(), "approved parameters are always visible");
    Options inspected = normal;
    inspected.inspect_panel = 0;
    inspected.smoke_seconds = .25;
    inspected.capture_ui = "capture.png";
    Window expanded(inspected);
    check(expanded.expanded(), "explicit bounded inspection opens the real parameter editor");
    inspected.inspect_panel = 5;
    bool rejected = false;
    try {
        Window invalid(inspected);
    } catch (...) {
        rejected = true;
    }

    check(rejected, "programmatic inspection also guards the section bound");
    inspected.inspect_panel = 4;
    inspected.capture_ui.clear();
    rejected = false;
    try {
        Window invalid(inspected);
    } catch (...) {
        rejected = true;
    }

    check(rejected, "programmatic inspection requires its own capture output");
}

void parameter_layout_guards() {
    // The gear, scale and language radios are one control: field height and one segment width.
    for (unsigned scale : {canonical_ui_scale, 50u, 75u, 100u}) {
        const auto form = parameters_layout(0, 0, 0, scale);
        const auto column = wheel_column(scale, {}, {}, 130, 30);
        const auto gear = gear_radio(form, column.choice_cell);
        check(form.radio_height() == form.input_height() && gear.height == form.input_height() &&
                      gear.y == form.movement_row(0, 4).y,
              "radio controls are as high as the fields on their row in every layout mode");
        check(column.scale_choice.height == gear.height &&
                      column.language_choice.height == gear.height &&
                      std::abs(gear.width / 4 - column.choice_cell) < .001f &&
                      std::abs(column.scale_choice.width / 3 - column.choice_cell) < .001f &&
                      std::abs(column.language_choice.width / 3 - column.choice_cell) < .001f,
              "gear, scale and language radios share one height and one segment width");
    }

    const auto reference = view_layout(canonical_ui_scale);

    for (unsigned scale : {canonical_ui_scale, 75u, 100u}) {
        const auto view = view_layout(scale);
        const auto form = parameters_layout(0, 0, 0, scale);
        const float density = form.density(),
                    fraction = scale == canonical_ui_scale ? .5f : scale / 100.f;
        check(std::abs(view.frame.width * density - 1920 * fraction) < .001f &&
                      std::abs(view.frame.height * density - 720 * fraction) < .001f,
              "QNX retains the requested original-pixel screen scale");
        check(view.parameters.right() == view.canvas_width &&
                      view.parameters.x == view.wheel.right(),
              "wheel and form fill the entire right quadrant without extra width");
        check(std::abs(view.wheel.width / view.frame.width -
                       reference.wheel.width / reference.frame.width) < .001f,
              "75/100 preserve the approved50 wheel/form width proportion");
        check(std::abs(view.wheel.y * 2 + view.wheel.height - view.top_height) < .001f,
              "wheel is vertically centered at every actual application scale");
        check(std::abs(view.wheel.height - photo_height * view.wheel.width / 334.f) < .001f,
              "wheel retains the original source aspect ratio");
        check(view.parameters.bottom() == view.top_height &&
                      form.panel.height == view.parameters.height,
              "form uses the complete available quadrant");
        check(std::abs(form.body.bottom() - (form.panel.bottom() - form.outer_inset())) < .001f,
              "body retains a scaled bottom inset");
        check(std::abs(form.movement_row(0, 0).y - form.body.y) < .001f,
              "first movement row uses the shared content origin");
        const auto column = wheel_column(scale, {}, {}, 130, 0);
        const auto gear = gear_radio(form, column.choice_cell);
        check(std::abs(gear.bottom() - (form.panel.bottom() - 40 * fraction / density)) < .001f &&
                      std::abs(form.movement_row(0, 4).y + form.input_height() / 2 -
                               (gear.y + gear.height / 2)) < .001f,
              "the gear radio ends 40 pixels at 100 percent above the bottom and the fifth "
              "movement row is centred on it");
        check(std::abs(form.input_height() * density - 80 * fraction) < .001f &&
                      form.input_height() * density <= 80.001f,
              "AP Hotspot fields scale up to the original80pixel height");
        check(std::abs(form.radio_height() * density - 80 * fraction) < .001f &&
                      std::abs(column.choice_font * density - 32 * fraction) < .001f,
              "radios are 80 pixels high with 32 pixel captions at 100 percent and scale with the "
              "screens");
        check(form.raw_input_width() == form.movement_value_width(),
              "All fields retain the semantic field width at every scale");

        for (const auto &button : {column.cluster_power, column.cluster_restart, column.media_power,
                                   column.media_restart}) {
            check(button.width == button.height &&
                          std::abs(button.width - 40 * column.scale) < .001f,
                  "lifecycle buttons are 40 points at 100 percent and scale with the wheel");
            check(button.x < view.wheel.x + view.wheel.width / 2,
                  "lifecycle buttons sit on the left of the wheel column");
        }

        check(column.cluster_restart.y > column.cluster_power.y &&
                      column.media_restart.y < column.media_power.y &&
                      column.screens_toggle.x > column.media_power.x &&
                      column.cluster_power.bottom() < view.wheel.bottom() &&
                      column.media_power.y > view.wheel.y,
              "cluster power stacks above restart at the top; multimedia restart stacks above power at the bottom next to the screens toggle");
    }

    const auto fifty = parameters_layout(0, 0, 0, canonical_ui_scale);
    const float density = 1920.f / canonical_ui_width;
    check(fifty.movement_row(0, 0).x == 1001 && fifty.movement_row(1, 0).x == 1175 &&
                  fifty.movement_value_width() == 71,
          "50 percent inputs share the approved inset grid");
    check(fifty.movement_row(0, 0).x + 71 + 8 == 1080 &&
                  fifty.movement_row(1, 0).x + 71 + 8 == 1254,
          "50 percent movement label anchors are preserved exactly");
    check(fifty.panel.bottom() - fifty.movement_row(1, 4).bottom() > 11,
          "50 percent bottom input keeps a normal panel inset");
    check(std::abs(fifty.group_margin_top() * density - 12) < .001f,
          "50percent headings reserve twelve physical pixels of extra top margin");
    check(std::abs((fifty.content_column(2, 1).x - fifty.checkbox_column(1).x) * density - 12) <
                          .001f &&
                  std::abs((fifty.content_column(2, 1).x - fifty.tyre_column(1).x) * density - 6) <
                          .001f,
          "right checkbox and tyre columns move left by twelve and six pixels");
    check(fifty.raw_input_width() == fifty.movement_value_width(),
          "All inputs use the compact movement width");
    check(std::abs((fifty.body.y - 46) * density - 8) < .001f,
          "50percent movement starts eight pixels lower below the tabs");
}

void input_transform_guards() {
    ImGui::CreateContext();
    try {
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        auto &context = *ImGui::GetCurrentContext();
        ImU32 mapped = 0;
        io.AddMousePosEvent(200, 100);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        io.AddMousePosEvent(240, 120);
        check(voyemu::media::normalize_mouse_events(context, mapped, .5, .5),
              "valid native density transform accepted");
        check(context.InputEventsQueue[0].MousePos.PosX == 100 &&
                      context.InputEventsQueue[2].MousePos.PosX == 120,
              "mouse events map to canonical controls before button processing");
        context.InputEventsQueue.erase(context.InputEventsQueue.begin(),
                                       context.InputEventsQueue.begin() + 2);
        io.AddMousePosEvent(300, 160);
        check(voyemu::media::normalize_mouse_events(context, mapped, .5, .5),
              "next frame transform accepted");
        check(context.InputEventsQueue[0].MousePos.PosX == 120 &&
                      context.InputEventsQueue[1].MousePos.PosX == 150,
              "trickled mouse event is not scaled twice while new events are scaled");
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        voyemu::media::normalize_mouse_events(context, mapped, .5, .5);
        check(context.InputEventsQueue.back().MousePos.PosX == -FLT_MAX,
              "outside-window sentinel is preserved");
        check(!voyemu::media::normalize_mouse_events(context, mapped, 0, 1),
              "invalid native density cannot corrupt input");
        ImGui::DestroyContext();
    } catch (...) {
        ImGui::DestroyContext();
        throw;
    }
}

void window_snapshot_before_frame(const std::filesystem::path &assets) {
    std::mutex mutex;
    unsigned generation = 7;
    auto transport = [&](const Json &operation) {
        if (operation.at("op") != "status") {
            return Json{{"ok", true}, {"accepted", true}};
        }

        std::lock_guard lock(mutex);

        return Json{{"ok", true},
                    {"session_id", "frame-refresh-test"},
                    {"revision", 1},
                    {"state", initial_state(VehicleCatalog::qinggan09)},
                    {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                    {"deliveries",
                     {{"qnx", {{"status", "graphics_running"}, {"generation", generation}}}}}};
    };
    Options options;

    options.assets = assets;
    options.endpoint = "frame-refresh-fixture";
    options.smoke_seconds = .2;
    Window window(options, transport);
    wait(
            [&] {
                return window.client().state().ready;
            },
            "frame refresh fixture did not receive state");
    check(!window.cluster_active(), "a hidden window has not copied its initial IPC state yet");
    check(window.refresh_state() && window.cluster_active() && window.cluster_generation() == 7,
          "GPU frame selection must see IPC state before ImGui draw");
    {
        std::lock_guard lock(mutex);
        generation = 8;
    }
    wait(
            [&] {
                return window.client().state().deliveries.at("qnx").at("generation") == 8;
            },
            "hidden restart fixture did not receive new generation");
    check(window.cluster_generation() == 7,
          "no hidden draw should accidentally refresh frame selection");
    check(window.refresh_state() && window.cluster_generation() == 8,
          "first resumed frame must select the new generation");
    check(!window.refresh_state(), "unchanged frame refresh avoids another deep state copy");
}

void inspection_lifetime(const std::filesystem::path &assets) {
    for (bool inspection : {false, true}) {
        std::mutex mutex;
        std::vector<std::string> operations;
        auto transport = [&](const Json &input) {
            std::lock_guard lock(mutex);
            operations.push_back(input.at("op").get<std::string>());

            if (input.at("op") == "status") {
                return Json{{"ok", true},
                            {"session_id", "lifetime-test"},
                            {"revision", 1},
                            {"state", initial_state(VehicleCatalog::qinggan09)},
                            {"catalog", catalog_json(VehicleCatalog::qinggan09)}};
            }

            return Json{{"ok", true}, {"accepted", true}};
        };
        {
            Options options;
            options.assets = assets;
            options.endpoint = "owned-lifetime-fixture";
            options.smoke_seconds = inspection ? .25 : 0;
            Window window(options, transport);
            wait(
                    [&] {
                        std::lock_guard lock(mutex);

                        return std::count(operations.begin(), operations.end(), "status") >= 2;
                    },
                    "lifetime fixture did not establish a canonical epoch");
            window.close();
        }
        std::lock_guard lock(mutex);
        auto closes = std::count(operations.begin(), operations.end(), "close_cluster");
        check(closes == (inspection ? 0 : 1),
              "bounded own inspection cannot terminate the live QNX session; normal window close can");
        check(std::count(operations.begin(), operations.end(), "release_owner") >= 1,
              "both close modes release only their own held buttons");
    }
}

void qinggan11_adversarial_wheel_acceptance(const std::filesystem::path &assets,
                                            const std::filesystem::path &qinggan11_assets) {
    // Real A11 free-2024 fixture: 15 enabled physical faces and 1 right roller.
    // Eight verified OEM routes dispatch; seven unverified faces remain visual controls.
    check(qinggan11_assets != assets,
          "A11 acceptance requires the distinct actual A11 fixture path");
    const WheelGeometry geometry = read_wheel_geometry(qinggan11_assets / "wheel-hotspots.json");
    check(geometry.regions.size() == 15 && geometry.rollers.size() == 1,
          "A11 exposes only existing generic actions and its right up/down rocker");
    std::mutex mutex;
    std::vector<Json> events;
    std::atomic<bool> partial = false;
    auto transport = [&](const Json &operation) {
        if (operation.at("op") == "wheel" || operation.at("op") == "release_owner" ||
            operation.at("op") == "close_cluster") {
            std::lock_guard lock(mutex);
            events.push_back(operation);

            return Json{{"ok", true}, {"accepted", true}};
        }

        if (operation.at("op") != "status") {
            return Json{{"ok", true}, {"accepted", true}};
        }

        const auto canonical = initial_state(VehicleCatalog::qinggan09);
        Json routes = Json::object();

        for (const auto &region : geometry.regions) {
            routes[region.id] = {{"qnx_index", nullptr}, {"android", Json::object()}};
        }

        Json response{{"ok", true},
                      {"session_id", "qinggan11-adversarial-test"},
                      {"revision", canonical.at("sequence")},
                      {"state", canonical},
                      {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                      {"wheel_routes", routes},
                      {"wheel_capability", nullptr}};

        if (partial) {
            response["wheel_capability"] =
                    qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1");

            for (const auto &[id, route] : response.at("wheel_routes").items()) {
                (void)route;
            }

            const auto adapter = qinggan11_wheel_adapter();

            for (const auto &[id, code] : adapter.at("routes").items()) {
                response["wheel_routes"][id]["android"] = {{"key_code", code}};
            }
        }

        return response;
    };
    ImGui::CreateContext();
    try {
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = ImVec2(closed_width, closed_height);
        io.DeltaTime = 1.f / 60;
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontDefault();
        Options options;
        options.assets = qinggan11_assets;
        options.endpoint = "headless-qinggan11-adversarial-fixture";
        Window window(options, transport);
        wait(
                [&] {
                    return window.client().state().ready;
                },
                "A11 adversarial fixture did not establish a canonical session");
        unsigned screen_scale = canonical_ui_scale;
        // Load the actual A11 wheel photos (900x770 RGBA) decoded from the real
        // profiles/common/wheels/free-2024 PNG assets; no geometry or asset change.
        auto load_bin = [&](const char *name) {
            return decode_photo_rgba(qinggan11_assets / name);
        };
        auto make_texture = [&](const std::vector<std::uint8_t> &rgba) {
            check(rgba.size() == static_cast<std::size_t>(900u * 770u) * 4u,
                  "A11 RGBA fixture keeps the exact original wheel photo size");

            return Texture{0u, 900, 770};
        };
        const auto left_pixels = load_bin("wheel-left.png"),
                   right_pixels = load_bin("wheel-right.png");
        Texture left_texture = make_texture(left_pixels),
                right_texture = make_texture(right_pixels);
        left_texture.id = 1002;
        right_texture.id = 1003;
        const Texture frame_texture{0, 1920, 720};
        auto frame = [&](ImVec2 mouse, bool pressed = false, float wheel = 0) {
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, pressed);

            if (wheel) {
                io.AddMouseWheelEvent(0, wheel);
            }

            io.DisplaySize = ImVec2(view_layout(screen_scale).canvas_width,
                                    view_layout(screen_scale).canvas_height);
            ImGui::NewFrame();
            window.draw(frame_texture, left_texture, right_texture, screen_scale);
            ImGui::Render();
        };
        auto key_frame = [&](ImGuiKey key) {
            io.AddKeyEvent(key, true);
            io.DisplaySize = ImVec2(view_layout(screen_scale).canvas_width,
                                    view_layout(screen_scale).canvas_height);
            ImGui::NewFrame();
            window.draw(frame_texture, left_texture, right_texture, screen_scale);
            ImGui::Render();
            io.AddKeyEvent(key, false);
            io.DisplaySize = ImVec2(view_layout(screen_scale).canvas_width,
                                    view_layout(screen_scale).canvas_height);
            ImGui::NewFrame();
            window.draw(frame_texture, left_texture, right_texture, screen_scale);
            ImGui::Render();
        };
        auto phases = [&](const std::string &id, const char *phase) {
            std::lock_guard lock(mutex);

            return std::count_if(events.begin(), events.end(), [&](const Json &event) {
                return event.at("id") == id && event.at("phase") == phase;
            });
        };
        auto draw_events = [&]() {
            std::lock_guard lock(mutex);

            return events.size();
        };
        auto highlight_alpha = [](ImVec2 point) {
            unsigned alpha = 0;
            auto cross = [](ImVec2 a, ImVec2 b, ImVec2 p) {
                return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            };

            for (const auto *draw : ImGui::GetDrawData()->CmdLists) {
                for (const auto &command : draw->CmdBuffer) {
                    if (command.UserCallback || point.x < command.ClipRect.x ||
                        point.y < command.ClipRect.y || point.x > command.ClipRect.z ||
                        point.y > command.ClipRect.w) {
                        continue;
                    }

                    for (unsigned index = 0; index + 2 < command.ElemCount; index += 3) {
                        const auto &a = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + index]];
                        const auto &b =
                                draw->VtxBuffer[command.VtxOffset +
                                                draw->IdxBuffer[command.IdxOffset + index + 1]];
                        const auto &c =
                                draw->VtxBuffer[command.VtxOffset +
                                                draw->IdxBuffer[command.IdxOffset + index + 2]];

                        if (a.col != b.col || a.col != c.col) {
                            continue;
                        }

                        const auto rgb = a.col & ~IM_COL32_A_MASK;

                        if (rgb != (IM_COL32(225, 234, 255, 0) & ~IM_COL32_A_MASK) &&
                            rgb != (IM_COL32(88, 164, 255, 0) & ~IM_COL32_A_MASK)) {
                            continue;
                        }

                        const float ab = cross(a.pos, b.pos, point),
                                    bc = cross(b.pos, c.pos, point),
                                    ca = cross(c.pos, a.pos, point);

                        if (std::abs(cross(a.pos, b.pos, c.pos)) < .0001f ||
                            ((ab < -.001f || bc < -.001f || ca < -.001f) &&
                             (ab > .001f || bc > .001f || ca > .001f))) {
                            continue;
                        }

                        alpha = std::max(alpha, (a.col >> IM_COL32_A_SHIFT) & 0xffu);
                    }
                }
            }

            return alpha;
        };
        auto image_bounds = [](ImTextureID texture) {
            ImVec2 first(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()),
                    last(-first.x, -first.y);
            bool found = false;

            for (const auto *draw : ImGui::GetDrawData()->CmdLists) {
                for (const auto &command : draw->CmdBuffer) {
                    if (command.TexRef._TexData || command.TexRef._TexID != texture ||
                        command.UserCallback) {
                        continue;
                    }

                    for (unsigned index = 0; index < command.ElemCount; ++index) {
                        const auto &point =
                                draw->VtxBuffer[command.VtxOffset +
                                                draw->IdxBuffer[command.IdxOffset + index]]
                                        .pos;
                        first.x = std::min(first.x, point.x);
                        first.y = std::min(first.y, point.y);
                        last.x = std::max(last.x, point.x);
                        last.y = std::max(last.y, point.y);
                        found = true;
                    }
                }
            }

            check(found, "actual A11 headless draw data must contain both wheel photo quads");

            return ImRect(first, last);
        };
        auto position = [&](std::string_view side, const std::array<double, 2> &point) {
            const auto bounds = side == "left" ? image_bounds(left_texture.id)
                                               : image_bounds(right_texture.id);

            return ImVec2(
                    bounds.Min.x + static_cast<float>(point[0]) * (bounds.Max.x - bounds.Min.x),
                    bounds.Min.y + static_cast<float>(point[1]) * (bounds.Max.y - bounds.Min.y));
        };
        frame(ImVec2(20, 20));
        frame(ImVec2(20, 20));
        const auto left = image_bounds(left_texture.id), right = image_bounds(right_texture.id);
        Json qinggan11_fixtures;
        {
            std::ifstream source(qinggan11_assets / "wheel-hotspots.json");
            source >> qinggan11_fixtures;
        }
        auto interior = [&](const std::string &id) {
            for (const auto &entry : qinggan11_fixtures.at("regions")) {
                if (entry.at("id") == id) {
                    return entry.at("interior_point_normalized").get<std::array<double, 2>>();
                }
            }

            throw std::runtime_error("A11 fixture interior point missing");
        };

        // A11 all-available baseline: every enabled face dispatches its exact down/up.
        for (const auto &region : geometry.regions) {
            check(wheel_action_available(window.client().state(), region.id),
                  "full A11 view keeps every enabled face available");
            const auto mouse = position(region.photo, interior(region.id));
            const auto downs = phases(region.id, "down"), ups = phases(region.id, "up");
            frame(mouse);
            const auto hover = highlight_alpha(mouse);
            check(hover > 0 && hover <= 48,
                  "A11 full view keeps a soft hover highlight on every face");
            frame(mouse, true);
            const auto pressed = highlight_alpha(mouse);
            check(pressed > hover && pressed <= 128,
                  "A11 full view keeps the stronger press highlight");
            frame(mouse, false);
            wait(
                    [&] {
                        return phases(region.id, "down") == downs + 1 &&
                               phases(region.id, "up") == ups + 1;
                    },
                    "A11 full view press and release preserve exact routing");
        }

        // Partial capability reaches the actual A11 view.
        partial = true;
        wait(
                [&] {
                    window.refresh_state();

                    return window.client().state().wheel_capability ==
                           qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1");
                },
                "partial capability reaches the actual A11 native view");
        // Non-face neutral point inside the wheel photos that no enabled face contains (the
        // A9-approved left gap area).
        const ImVec2 gap((left.Min.x + right.Max.x) / 2.f, right.Min.y - 2.f);
        const std::set<std::string> supported{"volume_up", "volume_down", "next",  "previous",
                                              "ok",        "photo",       "voice", "phone"};

        for (const auto &region : geometry.regions) {
            const std::string id = region.id;
            const bool available = supported.count(id) > 0;
            check(wheel_action_available(window.client().state(), id) == available,
                  "partial A11 view enables exactly the eight verified OEM routes");
            const auto mouse = position(region.photo, interior(region.id));
            const auto downs = phases(id, "down"), ups = phases(id, "up");
            frame(mouse);
            const auto rest = highlight_alpha(mouse);
            check(rest > 0 && rest <= 48,
                  "partial A11 view keeps a soft hover highlight even on unsupported faces");
            // press-in / hold
            frame(mouse, true);
            frame(mouse, true);
            const auto held = highlight_alpha(mouse);

            if (available) {
                check(held > rest && held <= 128,
                      "partial A11 supported pointer down keeps the stronger translucent press highlight");
                // drag outside mid-hold: the transported held token persists until release
                // (canonical hold semantics)
                frame(gap, true);
                frame(gap, true);
                const auto drag_away = highlight_alpha(mouse);
                check(drag_away > 48 && drag_away <= 128,
                      "supported A11 face keeps its transported pressed highlight while the pointer is dragged away mid-hold");
                wait(
                        [&] {
                            return phases(id, "down") == downs + 1;
                        },
                        "supported A11 mouse-down dispatches its single down event");
                // drag back in while still down
                frame(mouse, true);
                frame(mouse, true);
                const auto res = highlight_alpha(mouse);
                check(res > 48 && res <= 128,
                      "supported A11 face keeps its pressed highlight when dragged back in the same hold");
                // release on the face
                frame(mouse, false);
                wait(
                        [&] {
                            return phases(id, "up") == ups + 1;
                        },
                        "supported A11 hold/drag/release dispatches its single release event");
                check(phases(id, "down") == downs + 1 && phases(id, "up") == ups + 1,
                      "supported A11 hold/drag/release cycle dispatches exactly one down and one up");
            } else {
                check(held >= 105 && held <= 128,
                      "partial A11 unsupported face draws the same stronger pressed highlight");
                const auto before = draw_events();
                // drag outside mid-hold: pressed highlight clears, nothing dispatched
                frame(gap, true);
                frame(gap, true);
                check(highlight_alpha(mouse) < 48,
                      "unsupported A11 face clears its pressed highlight while dragged away mid-hold");
                // drag back in while still down: pressed visual resumes, nothing dispatched
                frame(mouse, true);
                frame(mouse, true);
                check(highlight_alpha(mouse) >= 105,
                      "unsupported A11 face resumes the pressed highlight when dragged back in the same hold");
                // release on the face
                frame(mouse, false);
                frame(mouse, false);
                check(highlight_alpha(mouse) <= 48,
                      "unsupported A11 face returns to rest highlight after the release");
                check(draw_events() == before,
                      "unsupported A11 press/hold/drag-out/drag-in/release dispatches exactly zero wheel events");
                // release outside after dragging away: still zero events
                frame(ImVec2(20, 20));
                frame(ImVec2(20, 20));
                frame(mouse, true);
                frame(mouse, true);
                frame(gap, true);
                frame(gap, false);
                frame(gap, false);
                check(draw_events() == before,
                      "unsupported A11 press-inside release-outside cycle dispatches exactly zero wheel events");
                // press outside, drag inside: face never reaches pressed and again zero events
                frame(gap, true);
                frame(gap, true);
                frame(mouse, true);
                frame(mouse, true);
                check(highlight_alpha(mouse) <= 48,
                      "press started outside never lights the unsupported A11 face as pressed");
                frame(mouse, false);
                frame(ImVec2(20, 20));
                check(draw_events() == before,
                      "press started outside an unsupported A11 face dispatches exactly zero wheel events");
                check(!window.client().wheel(id, "down") && !window.client().detent(id),
                      "unsupported partial A11 wheel cannot enqueue pointer or roller down");
            }

            frame(ImVec2(20, 20));
            check(highlight_alpha(mouse) == 0,
                  "A11 face highlight clears once the pointer leaves the wheel");
        }

        // Keyboard navigation: Tab enters the production focus chain and Space/Enter never
        // dispatch a wheel face that is not the actual focused item.
        partial = true;
        {
            auto nav_frame = [&](ImGuiKey key, bool down) {
                io.AddKeyEvent(key, down);
                io.DisplaySize = ImVec2(view_layout(screen_scale).canvas_width,
                                        view_layout(screen_scale).canvas_height);
                ImGui::NewFrame();
                window.draw(frame_texture, left_texture, right_texture, screen_scale);
                ImGui::Render();
            };
            const auto before_all = draw_events();
            check(!ImGui::GetCurrentContext()->NavCursorVisible,
                  "no keyboard focus cursor exists before Tab navigation");
            nav_frame(ImGuiKey_Tab, true);
            nav_frame(ImGuiKey_Tab, false);
            check(ImGui::GetCurrentContext()->NavCursorVisible,
                  "A11 Tab navigation exposes the actual production keyboard focus cursor");
            check(ImGui::GetCurrentContext()->NavId != 0,
                  "A11 Tab navigation activates a real focused item");

            // Space and Enter presses without wheel-face focus dispatch exactly zero wheel events.
            for (const ImGuiKey key : {ImGuiKey_Space, ImGuiKey_Enter}) {
                nav_frame(key, true);
                nav_frame(key, false);
                check(draw_events() == before_all,
                      "keyboard activation without a focused A11 wheel face dispatches exactly zero wheel events");
            }

            for (const auto &region : geometry.regions) {
                if (!supported.count(region.id)) {
                    check(!wheel_action_available(window.client().state(), region.id),
                          "keyboard focus can never reach a partially unsupported A11 face");
                }
            }

            frame(ImVec2(20, 20));
            frame(ImVec2(20, 20));
            io.AddKeyEvent(ImGuiKey_Escape, true);
            nav_frame(ImGuiKey_Escape, false);
            nav_frame(ImGuiKey_Tab, true);
            nav_frame(ImGuiKey_Tab, false);
        }
        // Canceled release / window focus loss mid-hold drops the held token with a cancel, not an
        // up.
        {
            const auto r = std::find_if(geometry.regions.begin(), geometry.regions.end(),
                                        [&](const auto &x) {
                                            return x.id == *supported.begin();
                                        });
            check(r != geometry.regions.end(), "A11 keyboard cancel fixture face exists");
            const auto k_mouse = position(r->photo, interior(r->id));
            const auto k_downs = phases(r->id, "down"), k_cancels = phases(r->id, "cancel"),
                       k_ups = phases(r->id, "up");
            frame(k_mouse, true);
            frame(k_mouse, true);
            wait(
                    [&] {
                        return phases(r->id, "down") == k_downs + 1;
                    },
                    "A11 focus-loss hold registers its exact down event");
            const auto release_owner_before =
                    std::count_if(events.begin(), events.end(), [&](const Json &e) {
                        std::lock_guard lock(mutex);

                        return e.value("op", std::string{}) == "release_owner";
                    });
            io.AddFocusEvent(false);
            frame(k_mouse, true);
            frame(ImVec2(20, 20));
            // The production cancel path is the owner-level release broadcast that frees every held
            // button.
            check(highlight_alpha(k_mouse) <= 48,
                  "window focus loss mid-hold drops the live pressed highlight");
            {
                wait(
                        [&] {
                            std::lock_guard lock(mutex);

                            return std::count_if(events.begin(), events.end(), [&](const Json &e) {
                                       return e.value("op", std::string{}) == "release_owner";
                                   }) > release_owner_before;
                        },
                        "window focus loss mid-hold calls the production release_owner broadcast");
                std::lock_guard lock(mutex);
                check(std::count_if(events.begin(), events.end(),
                                    [&](const Json &e) {
                                        return e.value("op", std::string{}) == "wheel" &&
                                               e.at("id") == r->id && e.at("phase") == "up";
                                    }) == k_ups,
                      "window focus loss mid-hold never dispatches a stale release up for the held face");
            }
            io.AddFocusEvent(true);
            frame(ImVec2(20, 20));
            frame(ImVec2(20, 20));
        }
        // A9 all-normal behavior: full capability view (canonical null) remains available for every
        // face.
        partial = false;
        wait(
                [&] {
                    window.refresh_state();

                    return window.client().state().wheel_capability == Json::object() ||
                           !window.client().state().wheel_capability.is_object();
                },
                "full A11 capability view returns after the adversarial run");

        for (const auto &region : geometry.regions) {
            check(wheel_action_available(window.client().state(), region.id),
                  "A9-style full capability view keeps every A11 enabled face available");
        }

        // AoO lifecycle colors unchanged: the shared production suite already draws lifecycle ink
        // assertions; here the A11 window must render the same four lifecycle circles with the
        // original 77/255 disabled core opacity.
        {
            screen_scale = 100;
            frame(ImVec2(20, 20));
            frame(ImVec2(20, 20));
            auto lifecycle_ink = [&](ImVec2 point) {
                float alpha = 0;
                auto cross = [](ImVec2 a, ImVec2 b, ImVec2 p) {
                    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
                };
                const auto rgb = IM_COL32(255, 255, 255, 0) & ~IM_COL32_A_MASK;

                for (const auto *draw : ImGui::GetDrawData()->CmdLists) {
                    for (const auto &command : draw->CmdBuffer) {
                        if (command.UserCallback || point.x < command.ClipRect.x ||
                            point.y < command.ClipRect.y || point.x >= command.ClipRect.z ||
                            point.y >= command.ClipRect.w) {
                            continue;
                        }

                        for (unsigned i = 0; i + 2 < command.ElemCount; i += 3) {
                            const auto &a = draw->VtxBuffer[command.VtxOffset +
                                                            draw->IdxBuffer[command.IdxOffset + i]];
                            const auto &b =
                                    draw->VtxBuffer[command.VtxOffset +
                                                    draw->IdxBuffer[command.IdxOffset + i + 1]];
                            const auto &c =
                                    draw->VtxBuffer[command.VtxOffset +
                                                    draw->IdxBuffer[command.IdxOffset + i + 2]];

                            if ((a.col & ~IM_COL32_A_MASK) != rgb ||
                                (b.col & ~IM_COL32_A_MASK) != rgb ||
                                (c.col & ~IM_COL32_A_MASK) != rgb) {
                                continue;
                            }

                            const float area = cross(a.pos, b.pos, c.pos);

                            if (std::abs(area) < .0001f) {
                                continue;
                            }

                            const float wa = cross(b.pos, c.pos, point) / area,
                                        wb = cross(c.pos, a.pos, point) / area, wc = 1 - wa - wb;

                            if (wa < -.001f || wb < -.001f || wc < -.001f) {
                                continue;
                            }

                            const auto opacity = [](ImU32 color) {
                                const auto alpha2 = (color >> IM_COL32_A_SHIFT) & 255;
                                check(alpha2 <= 77,
                                      "disabled lifecycle core uses the original Music 77/255 opacity");

                                return float(alpha2) / 77;
                            };
                            alpha = std::max(alpha, wa * opacity(a.col) + wb * opacity(b.col) +
                                                            wc * opacity(c.col));
                        }
                    }
                }

                return alpha;
            };
            const auto column = wheel_column(100, {}, {}, 130, 0);
            const auto power = column.cluster_power, restart = column.cluster_restart;
            const float lifecycle_scale = power.width / 28.f, ring_radius = 6.5f * lifecycle_scale;
            const ImVec2 power_center(power.x + power.width / 2, power.y + power.height / 2),
                    restart_center(restart.x + restart.width / 2, restart.y + restart.height / 2);
            check(lifecycle_ink(ImVec2(power_center.x + ring_radius, power_center.y)) > .5f,
                  "A11 power ring must continue on the right");
            check(lifecycle_ink(ImVec2(power_center.x - ring_radius, power_center.y)) > .5f,
                  "A11 power ring must continue on the left");
            check(lifecycle_ink(ImVec2(power_center.x, power_center.y + ring_radius)) > .5f,
                  "A11 power ring must continue below the stem");
            check(lifecycle_ink(ImVec2(restart_center.x + 5 * lifecycle_scale,
                                       restart_center.y - 8 * lifecycle_scale)) >= 0,
                  "A11 restart lifecycle ink renders through the same production path");
        }
        window.close();
        ImGui::DestroyContext();
    } catch (...) {
        ImGui::DestroyContext();
        throw;
    }
}

void passion_unsupported_wheel(const std::filesystem::path &legacy_assets) {
    const auto assets = legacy_assets.parent_path() / "passion-2024";
    std::ifstream input(assets / "wheel-hotspots.json");
    Json fixtures;

    input >> fixtures;
    const auto canonical = initial_state(VehicleCatalog::qinggan09);
    std::atomic<unsigned> wheel_events{}, polls{};
    auto transport = [&](const Json &operation) {
        if (operation.at("op") == "wheel") {
            ++wheel_events;

            return Json{{"ok", true}, {"accepted", false}};
        }

        if (operation.at("op") != "status") {
            return Json{{"ok", true}, {"accepted", false}};
        }

        ++polls;

        return Json{{"ok", true},
                    {"session_id", "passion-unsupported-wheel"},
                    {"revision", canonical.at("sequence")},
                    {"state", canonical},
                    {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                    {"wheel_routes", Json::object()},
                    {"wheel_capability", nullptr},
                    {"deliveries",
                     {{"android", {{"presentation_family", "passion"}}},
                      {"qnx", {{"status", "off"}, {"lifecycle_supported", false}}}}}};
    };
    ImGui::CreateContext();
    try {
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = ImVec2(1920, 720);
        io.DeltaTime = 1.f / 60;
        io.ConfigInputTrickleEventQueue = false;
        io.Fonts->AddFontDefault();
        Options options;
        options.assets = assets;
        options.endpoint = "passion-unsupported-fixture";
        options.capture_ui = "headless-passion-inspection";
        Window window(options, transport);
        wait(
                [&] {
                    return window.client().state().ready;
                },
                "Passion unsupported fixture must have validated canonical state");
        auto frame = [&](ImVec2 point, bool down = false) {
            io.AddMousePosEvent(point.x, point.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame();
            window.draw({}, {1002, 850, 727}, {1003, 560, 479}, 100);
            ImGui::Render();
        };
        auto image_bounds = [](ImTextureID texture) {
            ImVec2 first(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()),
                    last(-first.x, -first.y);
            bool found = false;

            for (const auto *draw : ImGui::GetDrawData()->CmdLists) {
                for (const auto &command : draw->CmdBuffer) {
                    if (command.UserCallback || command.TexRef._TexData ||
                        command.TexRef._TexID != texture) {
                        continue;
                    }

                    for (unsigned i = 0; i < command.ElemCount; ++i) {
                        const auto &p = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + i]]
                                                .pos;
                        first.x = std::min(first.x, p.x);
                        first.y = std::min(first.y, p.y);
                        last.x = std::max(last.x, p.x);
                        last.y = std::max(last.y, p.y);
                        found = true;
                    }
                }
            }

            check(found, "Passion shared renderer emits both real-sized photo quads");

            return ImRect(first, last);
        };
        auto highlight_alpha = [](ImVec2 point) {
            unsigned alpha = 0;
            auto cross = [](ImVec2 a, ImVec2 b, ImVec2 p) {
                return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            };

            for (const auto *draw : ImGui::GetDrawData()->CmdLists) {
                for (const auto &command : draw->CmdBuffer) {
                    if (command.UserCallback || point.x < command.ClipRect.x ||
                        point.y < command.ClipRect.y || point.x > command.ClipRect.z ||
                        point.y > command.ClipRect.w) {
                        continue;
                    }

                    for (unsigned i = 0; i + 2 < command.ElemCount; i += 3) {
                        const auto &a = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + i]];
                        const auto &b = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + i + 1]];
                        const auto &c = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + i + 2]];

                        if (a.col != b.col || a.col != c.col) {
                            continue;
                        }

                        const auto rgb = a.col & ~IM_COL32_A_MASK;

                        if (rgb != (IM_COL32(225, 234, 255, 0) & ~IM_COL32_A_MASK) &&
                            rgb != (IM_COL32(88, 164, 255, 0) & ~IM_COL32_A_MASK)) {
                            continue;
                        }

                        const auto ab = cross(a.pos, b.pos, point), bc = cross(b.pos, c.pos, point),
                                   ca = cross(c.pos, a.pos, point);

                        if (std::abs(cross(a.pos, b.pos, c.pos)) < .0001f ||
                            ((ab < -.001f || bc < -.001f || ca < -.001f) &&
                             (ab > .001f || bc > .001f || ca > .001f))) {
                            continue;
                        }

                        alpha = std::max(alpha, (a.col >> IM_COL32_A_SHIFT) & 0xffu);
                    }
                }
            }

            return alpha;
        };
        frame(ImVec2(20, 20));
        frame(ImVec2(20, 20));
        const auto left = image_bounds(1002), right = image_bounds(1003);
        const ImVec2 gap((left.Max.x + right.Min.x) / 2, (left.Min.y + left.Max.y) / 2);

        for (const auto &region : fixtures.at("regions")) {
            const auto normalized =
                    region.at("interior_point_normalized").get<std::array<double, 2>>();
            const auto photo = region.at("photo") == "left" ? left : right;
            const ImVec2 point(photo.Min.x + normalized[0] * photo.GetWidth(),
                               photo.Min.y + normalized[1] * photo.GetHeight());
            frame(point);
            check(highlight_alpha(point) > 0 && highlight_alpha(point) <= 48,
                  "each unsupported Passion face retains its exact soft hover contour");
            frame(point, true);
            frame(point, true);
            check(highlight_alpha(point) >= 105 && highlight_alpha(point) <= 128,
                  "each unsupported Passion face retains its stronger pressed contour");
            frame(point, false);
            frame(point, false);
            check(highlight_alpha(point) > 0 && highlight_alpha(point) <= 48,
                  "unsupported Passion release returns to soft hover");
            frame(gap);
            check(highlight_alpha(gap) == 0,
                  "Passion blank center seam never highlights an invented face");
            frame(gap, true);
            frame(gap, false);
        }

        const auto after = polls.load();
        wait(
                [&] {
                    return polls.load() > after;
                },
                "Passion input fixture must drain queued operations");
        check(wheel_events == 0,
              "all fourteen unsupported Passion pointer gestures and blank gaps dispatch zero wheel operations");

        for (const auto &region : fixtures.at("regions")) {
            const auto id = region.at("id").get<std::string>();
            check(window.client().wheel(id, "down") && window.client().detent(id),
                  "unsupported Passion controls remain interactive no-ops");
        }

        const auto final_poll = polls.load();
        wait(
                [&] {
                    return polls.load() > final_poll;
                },
                "Passion no-op fixture drains");
        check(wheel_events == 0, "unsupported Passion controls never dispatch");
        check(window.client().state().ready && window.inspection().at("canonical_ready") == true,
              "unsupported Passion behavior is proven with a ready client and powered-off cluster");
    } catch (...) {
        ImGui::DestroyContext();
        throw;
    }

    ImGui::DestroyContext();
}

// With the decoded photos of every wheel, the scale and language choices keep 40 physical pixels
// at 100 percent to the visible wheel above them and to the bottom edge below.
void choices_between_wheel_and_edge(const std::filesystem::path &assets) {
    for (const auto *name : {"free-2021", "free-2024", "passion-2024"}) {
        const auto directory = assets.parent_path() / name;
        const auto ink = [&](const char *file) {
            auto image = window::decode_png(directory / file);

            return wheel_ink(image.rgba.data(), image.width, image.height,
                             std::size_t(image.width) * 4);
        };
        const auto left = ink("wheel-left.png"), right = ink("wheel-right.png");
        const float joined = read_wheel_geometry(directory / "wheel-hotspots.json").joined_offset;

        for (const unsigned scale : {canonical_ui_scale, 75u, 100u}) {
            const auto view = view_layout(scale);
            const auto wheel = placed_wheel(scale, left, right, joined);
            const auto column = wheel_column(scale, left, right, joined, 30);
            const float pixel = 1.f / parameters_layout(0, 0, 0, scale).density(),
                        fraction = scale == canonical_ui_scale ? .5f : scale / 100.f;
            float bottom = wheel.y;

            for (const auto *side : {&left, &right}) {
                const auto photo = wheel_photo(wheel, joined, side == &right, side->width,
                                               side->height);
                const float pitch = photo.width / side->width;

                for (int c = 0; c < side->width; ++c) {
                    const float x0 = photo.x + c * pitch;

                    if (side->bottom[c] > side->top[c] && x0 + pitch > wheel.x &&
                        x0 < wheel.right()) {
                        bottom = std::max(bottom, photo.y + side->bottom[c] * photo.height /
                                                                    float(side->height));
                    }
                }
            }

            check(std::abs((column.scale_choice.y - bottom) / pixel - 40 * fraction) < .5f &&
                          std::abs((view.top_height - column.scale_choice.bottom()) / pixel -
                                   40 * fraction) < .01f,
                  "with each wheel the choices keep 40 pixels at 100 percent to the visible "
                  "wheel and to the bottom");
        }
    }
}

void wheel_column_controls(const std::filesystem::path &assets) {
    const auto geometry = read_wheel_geometry(assets / "wheel-hotspots.json");
    auto transport = [&](const Json &operation) {
        if (operation.at("op") != "status") {
            return Json{{"ok", true}, {"accepted", true}};
        }

        const auto canonical = initial_state(VehicleCatalog::qinggan09);

        return Json{{"ok", true},
                    {"session_id", "wheel-column-test"},
                    {"revision", canonical.at("sequence")},
                    {"state", canonical},
                    {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                    {"deliveries",
                     {{"qnx", {{"status", "graphics_running"}, {"lifecycle_supported", true}}},
                      {"android", {{"powered", false}, {"lifecycle_supported", true}}}}}};
    };
    const auto previous_language = ui::current_language;

    ui::current_language = ui::Language::english;
    ImGui::CreateContext();
    try {
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = ImVec2(view_layout(100).canvas_width, view_layout(100).canvas_height);
        io.DisplayFramebufferScale = ImVec2(2, 2);
        io.DeltaTime = 1.f / 60;
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontDefault();
        Options options;
        options.assets = assets;
        options.endpoint = "headless-wheel-column-fixture";
        options.capture_ui = "headless-wheel-column-inspection";
        Window window(options, transport);
        wait(
                [&] {
                    return window.client().state().ready;
                },
                "wheel column fixture did not establish a canonical session");
        unsigned scale = 100;
        const auto frame = [&](ImVec2 mouse, bool pressed = false) {
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, pressed);
            ImGui::NewFrame();
            window.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308}, scale);
            ImGui::Render();
        };
        const auto center = [&](const char *name) {
            for (const auto &field : window.inspection().at("fields")) {
                if (field.at("name") == name) {
                    const auto r = field.at("rect_points").get<std::array<float, 4>>();

                    return ImVec2(r[0] + r[2] / 2, r[1] + r[3] / 2);
                }
            }

            throw std::runtime_error(std::string("missing wheel column item ") + name);
        };
        const auto click = [&](const char *name) {
            const auto point = center(name);
            frame(point);
            frame(point, true);
            frame(point);
        };
        frame(ImVec2(5, 5));
        frame(ImVec2(5, 5));
        const auto expected = wheel_column(100, {}, {}, geometry.joined_offset, 0);
        const auto &column = window.inspection().at("wheel_column");
        check(column.at("cluster_power") == Json::array({expected.cluster_power.x,
                                                         expected.cluster_power.y, 40, 40}) &&
                      column.at("media_restart") == Json::array({expected.media_restart.x,
                                                                 expected.media_restart.y, 40, 40}),
              "drawn lifecycle buttons follow the shared wheel column layout");
        bool wordmark = false;
        const auto word = column.at("wordmark").get<std::array<float, 4>>();

        for (const auto *list : ImGui::GetDrawData()->CmdLists) {
            for (const auto &command : list->CmdBuffer) {
                if (command.TexRef._TexData && command.TexRef._TexData != io.Fonts->TexData) {
                    const auto &first =
                            list->VtxBuffer[command.VtxOffset + list->IdxBuffer[command.IdxOffset]];
                    wordmark |= command.TexRef._TexData->Width == std::lround(word[2] * 2) &&
                                first.pos.x >= word[0] - .001f &&
                                first.pos.x <= word[0] + word[2] + .001f;
                }
            }
        }

        check(wordmark, "wordmark is rasterized at the framebuffer size of its slot");
        bool ring = false;
        const auto media = column.at("media_power").get<std::array<float, 4>>();
        const ImVec2 media_center(media[0] + 20, media[1] + 20);

        for (const auto *list : ImGui::GetDrawData()->CmdLists) {
            for (const auto &command : list->CmdBuffer) {
                if (command.TexRef._TexData != io.Fonts->TexData) {
                    continue;
                }

                for (unsigned i = 0; i < command.ElemCount; ++i) {
                    const auto &vertex = list->VtxBuffer[list->IdxBuffer[command.IdxOffset + i] +
                                                         command.VtxOffset];
                    const float distance = std::hypot(vertex.pos.x - media_center.x,
                                                      vertex.pos.y - media_center.y);
                    ring |= (vertex.col >> IM_COL32_A_SHIFT) != 0 && distance > 12 && distance < 22;
                }
            }
        }

        check(!ring, "a stopped but available guest draws no ring around its power glyph");
        // The opaque vertex colors in a wheel column slot: the tint layer of the artwork or the
        // stroked glyph within 9 of the 28 grid units around the slot centre.
        const auto inks = [&](const char *name, bool artwork) {
            const auto r =
                    window.inspection().at("wheel_column").at(name).get<std::array<float, 4>>();
            const float reach = artwork ? FLT_MAX : 9 * r[2] / 28;
            std::set<ImU32> colors;

            for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                for (const auto &command : list->CmdBuffer) {
                    if (artwork ? !command.TexRef._TexData ||
                                          command.TexRef._TexData == io.Fonts->TexData
                                : command.TexRef._TexData != io.Fonts->TexData) {
                        continue;
                    }

                    for (unsigned i = 0; i < command.ElemCount; ++i) {
                        const auto &vertex =
                                list->VtxBuffer[list->IdxBuffer[command.IdxOffset + i] +
                                                command.VtxOffset];

                        if (vertex.pos.x >= r[0] && vertex.pos.x <= r[0] + r[2] &&
                            vertex.pos.y >= r[1] && vertex.pos.y <= r[1] + r[3] &&
                            std::hypot(vertex.pos.x - r[0] - r[2] / 2,
                                       vertex.pos.y - r[1] - r[3] / 2) <= reach &&
                            vertex.uv.y < .5f && (vertex.col >> IM_COL32_A_SHIFT) == 255) {
                            colors.insert(vertex.col);
                        }
                    }
                }
            }

            return colors;
        };
        window.set_screens({"rear"});
        window.set_screens_window(true);
        check(screens_window_title(Options{}, window.screens_label()) == "Voyemu - Rear screen",
              "the screens window title carries the English caption");
        ui::current_language = ui::Language::russian;
        check(screens_window_title(Options{}, window.screens_label()) == "Voyemu - Задний экран",
              "the screens window title follows a language change");
        ui::current_language = ui::Language::english;
        frame(ImVec2(5, 5));
        frame(ImVec2(5, 5));
        check(inks("screens_toggle", true) == inks("cluster_power", false) &&
                      inks("cluster_power", false) == std::set<ImU32>{IM_COL32(255, 255, 255, 255)},
              "the open screens toggle takes the primary ink of the running power glyph");
        window.set_screens_window(false);
        frame(ImVec2(5, 5));
        check(inks("screens_toggle", true) == inks("cluster_restart", false) &&
                      inks("cluster_restart", false).size() == 1,
              "the closed screens toggle takes the inactive ink of the restart glyph");
        window.set_screens({});
        frame(ImVec2(5, 5));
        const auto wordmark_idle = inks("wordmark", true);
        frame(center("about"));
        frame(center("about"));
        check(wordmark_idle == std::set<ImU32>{IM_COL32(255, 255, 255, 255)} &&
                      inks("wordmark", true) == wordmark_idle,
              "the wordmark keeps the primary white ink under the pointer");
        frame(ImVec2(5, 5));
        // Left and right edges of the glyph quads inside a choice: the caption ink.
        const auto caption_ink = [&](const char *name) {
            std::array<float, 4> r{};

            for (const auto &field : window.inspection().at("fields")) {
                if (field.at("name") == name) {
                    r = field.at("rect_points").get<std::array<float, 4>>();
                }
            }

            const ImVec2 white = io.Fonts->TexUvWhitePixel;
            float left = FLT_MAX, right = -FLT_MAX;

            for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                for (const auto &command : list->CmdBuffer) {
                    if (command.TexRef._TexData != io.Fonts->TexData) {
                        continue;
                    }

                    for (unsigned i = 0; i < command.ElemCount; ++i) {
                        const auto &vertex =
                                list->VtxBuffer[list->IdxBuffer[command.IdxOffset + i] +
                                                command.VtxOffset];

                        if (vertex.pos.x >= r[0] && vertex.pos.x <= r[0] + r[2] &&
                            vertex.pos.y >= r[1] && vertex.pos.y <= r[1] + r[3] &&
                            (vertex.uv.x != white.x || vertex.uv.y != white.y)) {
                            left = std::min(left, vertex.pos.x);
                            right = std::max(right, vertex.pos.x);
                        }
                    }
                }
            }

            check(left < right, "choice caption is drawn inside its cell");

            return std::array<float, 3>{left - r[0], r[0] + r[2] - right, left};
        };

        for (const auto *name :
             {"language_RU", "language_EN", "scale_50", "scale_100", "gear_P", "gear_D"}) {
            const auto ink = caption_ink(name);
            check(std::abs(ink[0] - ink[1]) <= .5f + .001f &&
                          std::abs(ink[2] * 2 - std::round(ink[2] * 2)) < .001f,
                  "radio captions centre their ink within one physical pixel on the pixel grid");
        }

        click("scale_100");
        check(!window.take_shell_request().scale_percent,
              "choosing the current scale requests nothing");
        click("scale_75");
        check(window.take_shell_request().scale_percent == 75u,
              "scale choice requests the window percent");
        click("language_RU");
        check(window.take_shell_request().language == ui::Language::russian,
              "language choice requests the selected language");
        const auto selection_left = [&](const char *name) {
            const auto bounds =
                    window.inspection().at("wheel_column").at(name).get<std::array<float, 4>>();
            float left = FLT_MAX, right = -FLT_MAX;

            each_vertex([&](const ImDrawVert &vertex, const ImDrawCmd &) {
                if (vertex.pos.y >= bounds[1] && vertex.pos.y <= bounds[1] + bounds[3] &&
                    vertex.pos.x >= bounds[0] - bounds[2] / 10 &&
                    vertex.pos.x <= bounds[0] + bounds[2] * 1.1f &&
                    (selection_vertex(vertex, false, bounds[1], bounds[3]) ||
                     selection_vertex(vertex, true, bounds[1], bounds[3]))) {
                    left = std::min(left, vertex.pos.x);
                    right = std::max(right, vertex.pos.x);
                }
            });

            check(left < right, "choice selection draws its own capsule");
            const float cell = bounds[2] / 3;

            // The anti-aliased fringe widens the capsule equally on both sides.
            return ((left + right) / 2 - cell / 2 - bounds[0]) / cell;
        };
        scale = 75;
        frame(ImVec2(5, 5));
        check(std::abs(selection_left("scale_choice") - 2) < .001f,
              "scale resize preserves the starting segment instead of snapping to the target");
        frame(ImVec2(5, 5));
        check(selection_left("scale_choice") > 1 && selection_left("scale_choice") < 2,
              "scale selection visibly travels between segments after resizing");

        for (int i = 0; i < 30; ++i) {
            frame(ImVec2(5, 5));
        }

        check(std::abs(selection_left("scale_choice") - 1) < .001f,
              "scale selection settles on the requested segment");
        check(std::abs(selection_left("language_choice") - 1) < .001f,
              "language selection initially occupies the English segment");
        ui::current_language = ui::Language::russian;
        frame(ImVec2(5, 5));
        check(std::abs(selection_left("language_choice") - 1) < .001f,
              "locale change starts at the old language segment");
        frame(ImVec2(5, 5));
        check(selection_left("language_choice") > 1 && selection_left("language_choice") < 2,
              "language selection visibly travels between segments after changing locale");

        for (int i = 0; i < 30; ++i) {
            frame(ImVec2(5, 5));
        }

        check(std::abs(selection_left("language_choice") - 2) < .001f,
              "language selection settles on the requested segment");
        ui::current_language = ui::Language::english;
        check(!window.about_open(), "About starts closed");
        click("about");
        check(window.about_open(), "the wordmark opens About");
        frame(ImVec2(5, 5));
        click("site");
        check(window.take_shell_request().url == "https://voyemu.com",
              "Voyemu site follows the English interface");
        ui::current_language = ui::Language::russian;
        click("site");
        check(window.take_shell_request().url == "https://voyemu.ru",
              "Voyemu site follows the Russian interface");
        const auto field_rect = [&](const char *name) {
            for (const auto &field : window.inspection().at("fields")) {
                if (field.at("name") == name) {
                    return field.at("rect_points").get<std::array<float, 4>>();
                }
            }

            throw std::runtime_error(std::string("missing About item ") + name);
        };
        // Opaque vertex colours of a draw list inside a rectangle.
        const auto solid = [](const ImDrawList *list, std::array<float, 4> r) {
            std::set<ImU32> colors;

            for (const auto &vertex : list->VtxBuffer) {
                if (vertex.pos.x >= r[0] && vertex.pos.x <= r[0] + r[2] && vertex.pos.y >= r[1] &&
                    vertex.pos.y <= r[1] + r[3] && (vertex.col >> IM_COL32_A_SHIFT) == 255) {
                    colors.insert(vertex.col);
                }
            }

            return colors;
        };
        const auto *about_panel = ImGui::FindWindowByName("Voyemu About");
        const ImU32 white = IM_COL32(255, 255, 255, 255);
        frame(ImVec2(5, 5));
        const auto site_idle = solid(about_panel->DrawList, field_rect("site"));
        frame(center("site"));
        frame(center("site"));
        auto site_hovered = solid(about_panel->DrawList, field_rect("site"));
        check(!site_hovered.contains(IM_COL32(39, 129, 221, 255)) && site_hovered.contains(white) &&
                      !site_idle.contains(white) && site_hovered.size() == site_idle.size(),
              "the hovered Site button keeps its background and turns its caption white");
        site_hovered.erase(white);
        check(std::includes(site_idle.begin(), site_idle.end(), site_hovered.begin(),
                            site_hovered.end()),
              "the hovered Site button draws its idle background");
        // Opaque colours along the left edge of the panel: its border.
        const auto panel_edge = [&] {
            float left = FLT_MAX;

            for (const auto &vertex : about_panel->DrawList->VtxBuffer) {
                if ((vertex.col >> IM_COL32_A_SHIFT) == 255) {
                    left = std::min(left, vertex.pos.x);
                }
            }

            return solid(about_panel->DrawList, {left, 0, 1, io.DisplaySize.y});
        };
        window.set_title_colors(Window::TitleColors{0x123456, 0xffffff});
        frame(ImVec2(5, 5));
        check(panel_edge() == std::set<ImU32>{IM_COL32(0x12, 0x34, 0x56, 255)},
              "the About border takes the host title bar background");
        window.set_title_colors(std::nullopt);
        frame(ImVec2(5, 5));
        check(panel_edge() == std::set<ImU32>{IM_COL32(0, 0, 0, 255)},
              "without host title colours the About border takes the panel title background");
        const auto field_idle = solid(about_panel->DrawList, field_rect("license"));
        frame(center("license"));
        frame(center("license"));
        check(solid(about_panel->DrawList, field_rect("license")) == field_idle &&
                      ImGui::GetMouseCursor() == ImGuiMouseCursor_Hand,
              "the hovered license field keeps its background and shows the hand cursor");
        frame(center("license"), true);
        check(solid(about_panel->DrawList, field_rect("license")) == field_idle,
              "the pressed license field keeps its background");
        frame(center("license"));
        frame(ImVec2(5, 5));
        frame(ImVec2(5, 5));
        auto &context = *ImGui::GetCurrentContext();
        check(context.OpenPopupStack.Size == 1 && context.OpenPopupStack.back().Window,
              "the license field opens its list");
        const auto *list = context.OpenPopupStack.back().Window;
        const float list_padding = list->WindowPadding.y;
        check(!list->ScrollbarY && list->ScrollMax.y == 0 && list_padding > 0 &&
                      std::abs(list->Size.y - list->ContentSize.y - 2 * list_padding) < 1 &&
                      list->Pos.y + list->Size.y <= io.DisplaySize.y,
              "the license list holds every license without a scrollbar and pads its last row "
              "like its first");
        const std::array<float, 4> list_rect{list->Pos.x, list->Pos.y, list->Size.x, list->Size.y};
        const auto list_idle = solid(list->DrawList, list_rect);
        check(solid(about_panel->DrawList, field_rect("license")) == field_idle,
              "the license field keeps its background while its list is open");
        check(std::includes(list_idle.begin(), list_idle.end(), field_idle.begin(),
                            field_idle.end()),
              "the open license list has the background of the closed field");
        frame(ImVec2(list->Pos.x + list->Size.x / 2,
                     list->Pos.y + list->Size.y - list_padding - 2));
        frame(ImVec2(list->Pos.x + list->Size.x / 2,
                     list->Pos.y + list->Size.y - list_padding - 2));
        check(solid(list->DrawList, list_rect) == list_idle,
              "a hovered license row keeps the list background");
        io.AddKeyEvent(ImGuiKey_Escape, true);
        frame(ImVec2(5, 5));
        io.AddKeyEvent(ImGuiKey_Escape, false);
        frame(ImVec2(5, 5));
        check(context.OpenPopupStack.Size == 0 && window.about_open(),
              "Escape closes the open license list and keeps About open");
        io.AddKeyEvent(ImGuiKey_Escape, true);
        frame(ImVec2(5, 5));
        io.AddKeyEvent(ImGuiKey_Escape, false);
        frame(ImVec2(5, 5));
        check(!window.about_open(), "Escape closes About");
        window.close();
        ImGui::DestroyContext();
    } catch (...) {
        ImGui::DestroyContext();
        ui::current_language = previous_language;
        throw;
    }

    ui::current_language = previous_language;
}

// The lifecycle glyphs in both themes: the primary ink while on or under the pointer, a neutral
// grey while off, the primary ink at 77/255 while disabled and no ring around a stopped guest.
void lifecycle_glyph_inks(const std::filesystem::path &assets) {
    std::mutex mutex;
    std::string scheme = "dark";
    bool running = true;
    auto transport = [&](const Json &operation) {
        if (operation.at("op") != "status") {
            return Json{{"ok", true}, {"accepted", true}};
        }

        const auto canonical = initial_state(VehicleCatalog::qinggan09);
        std::lock_guard lock(mutex);

        return Json{{"ok", true},
                    {"session_id", "lifecycle-glyph-test"},
                    {"revision", canonical.at("sequence")},
                    {"state", canonical},
                    {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                    {"deliveries",
                     {{"qnx",
                       {{"status", running ? "graphics_running" : "off"},
                        {"lifecycle_supported", true}}},
                      {"android",
                       {{"powered", false},
                        {"lifecycle_supported", true},
                        {"appearance", {{"status", "observed"}, {"scheme", scheme}}}}}}}};
    };

    ImGui::CreateContext();
    try {
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = ImVec2(view_layout(100).canvas_width, view_layout(100).canvas_height);
        io.DisplayFramebufferScale = ImVec2(2, 2);
        io.DeltaTime = 1.f / 60;
        io.ConfigInputTrickleEventQueue = false;
        io.Fonts->AddFontDefault();
        Options options;
        options.assets = assets;
        options.endpoint = "headless-lifecycle-glyph-fixture";
        options.capture_ui = "headless-lifecycle-glyph-inspection";
        Window window(options, transport);
        const auto frame = [&](ImVec2 mouse) {
            io.AddMousePosEvent(mouse.x, mouse.y);
            ImGui::NewFrame();
            window.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308}, 100);
            ImGui::Render();
        };
        const auto settle = [&](bool light, bool cluster_running) {
            {
                std::lock_guard lock(mutex);
                scheme = light ? "light" : "dark";
                running = cluster_running;
            }
            wait(
                    [&] {
                        window.refresh_state();
                        const auto &state = window.client().state();

                        return state.ready && window.light_theme() == light &&
                               (state.deliveries.value("qnx", Json::object())
                                        .value("status", std::string{}) == "graphics_running") ==
                                       cluster_running;
                    },
                    "lifecycle glyph fixture reaches the requested theme and guest state");
            frame(ImVec2(5, 5));
            frame(ImVec2(5, 5));
        };
        const auto slot = [&](const char *name) {
            return window.inspection().at("wheel_column").at(name).get<std::array<float, 4>>();
        };
        const auto center = [&](const char *name) {
            const auto r = slot(name);

            return ImVec2(r[0] + r[2] / 2, r[1] + r[3] / 2);
        };
        // Visible untextured vertex colors at a distance from the slot centre, in slot widths.
        const auto colors = [&](const char *name, float from, float to) {
            const auto r = slot(name);
            const auto c = center(name);
            std::set<ImU32> found;

            for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                for (const auto &command : list->CmdBuffer) {
                    if (command.TexRef._TexData != io.Fonts->TexData) {
                        continue;
                    }

                    for (unsigned i = 0; i < command.ElemCount; ++i) {
                        const auto &vertex =
                                list->VtxBuffer[list->IdxBuffer[command.IdxOffset + i] +
                                                command.VtxOffset];
                        const float distance = std::hypot(vertex.pos.x - c.x, vertex.pos.y - c.y) /
                                               r[2];

                        // The light panel background gradient is no glyph ink.
                        const bool background = std::any_of(light_background.begin(),
                                                            light_background.end(),
                                                            [&](const ColorStop &stop) {
                                                                return vertex.col == stop.color;
                                                            });

                        if ((vertex.col >> IM_COL32_A_SHIFT) != 0 && !background &&
                            distance >= from && distance < to) {
                            found.insert(vertex.col);
                        }
                    }
                }
            }

            return found;
        };
        const auto glyph = [&](const char *name, ImVec2 mouse) {
            frame(mouse);
            frame(mouse);

            return colors(name, 0, 9.f / 28);
        };
        const auto ring = [&](const char *name) {
            return !colors(name, .3f, .55f).empty();
        };
        const ImVec2 away(5, 5);

        for (const bool light : {false, true}) {
            const ImU32 primary = light ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255),
                        grey = light ? IM_COL32(128, 128, 128, 255) : IM_COL32(202, 202, 202, 255),
                        disabled = (primary & ~IM_COL32_A_MASK) | (77u << IM_COL32_A_SHIFT);
            const std::set<ImU32> on{primary}, off{grey}, unavailable{disabled};
            settle(light, true);
            check(glyph("cluster_power", away) == on,
                  "a running guest draws its power glyph in the pure primary ink of the theme");
            check(glyph("media_power", away) == off && !ring("media_power"),
                  "a stopped guest draws its power glyph in neutral grey without a ring");
            check(glyph("media_power", center("media_power")) == on,
                  "the pointer turns a stopped power glyph into the primary ink");
            check(glyph("cluster_restart", away) == off,
                  "an enabled restart glyph rests in neutral grey");
            check(glyph("cluster_restart", center("cluster_restart")) == on,
                  "the pointer turns an enabled restart glyph into the primary ink");
            check(glyph("media_restart", away) == unavailable &&
                          glyph("media_restart", center("media_restart")) == unavailable,
                  "a disabled restart glyph keeps the primary ink at 77/255 under the pointer");
            settle(light, false);
            check(glyph("cluster_power", away) == off && !ring("cluster_power"),
                  "switching a guest off leaves a grey power glyph without a ring");
            check(glyph("cluster_power", center("cluster_power")) == on && !ring("cluster_power"),
                  "the pointer over a switched-off power glyph draws the primary ink and no ring");
            check(glyph("cluster_restart", away) == unavailable,
                  "the restart glyph of a switched-off guest is disabled");
        }

        window.close();
        ImGui::DestroyContext();
    } catch (...) {
        ImGui::DestroyContext();
        throw;
    }
}

void wheel_hover_without_tooltips(const std::filesystem::path &assets) {
    std::ifstream source(assets / "wheel-hotspots.json");
    Json fixtures;

    source >> fixtures;
    const auto geometry = read_wheel_geometry(assets / "wheel-hotspots.json");
    std::mutex mutex;
    std::vector<Json> events;
    std::atomic<bool> partial = false;
    auto transport = [&](const Json &operation) {
        if (operation.at("op") == "wheel") {
            std::lock_guard lock(mutex);
            events.push_back(operation);

            return Json{{"ok", true}, {"accepted", true}};
        }

        if (operation.at("op") != "status") {
            return Json{{"ok", true}, {"accepted", true}};
        }

        Json routes = Json::object();

        for (const auto &region : geometry.regions) {
            routes[region.id] = {{"qnx_index", nullptr}, {"android", "unrecovered"}};
        }

        const auto canonical = initial_state(VehicleCatalog::qinggan09);
        Json response{{"ok", true},
                      {"session_id", "wheel-hover-test"},
                      {"revision", canonical.at("sequence")},
                      {"state", canonical},
                      {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                      {"wheel_routes", routes},
                      {"wheel_capability", nullptr}};

        if (partial) {
            response["wheel_capability"] =
                    qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1");
            const auto adapter = qinggan11_wheel_adapter();

            for (const auto &[id, code] : adapter.at("routes").items()) {
                response["wheel_routes"][id]["android"] = {{"key_code", code}};
            }
        }

        return response;
    };
    ImGui::CreateContext();
    try {
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = ImVec2(closed_width, closed_height);
        io.DeltaTime = 1.f / 60;
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontDefault();
        Options options;
        options.assets = assets;
        options.endpoint = "headless-wheel-hover-fixture";
        options.capture_ui = "headless-wheel-inspection";
        Window window(options, transport);
        wait(
                [&] {
                    return window.client().state().ready;
                },
                "wheel hover fixture did not establish a canonical session");
        unsigned screen_scale = 100;
        auto frame = [&](ImVec2 mouse, bool pressed = false, float wheel = 0) {
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, pressed);

            if (wheel) {
                io.AddMouseWheelEvent(0, wheel);
            }

            io.DisplaySize = ImVec2(view_layout(screen_scale).canvas_width,
                                    view_layout(screen_scale).canvas_height);
            ImGui::NewFrame();
            window.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308}, screen_scale);
            ImGui::Render();

            for (const auto *rendered : ImGui::GetCurrentContext()->Windows) {
                check(!rendered->Active || !(rendered->Flags & ImGuiWindowFlags_Tooltip),
                      "wheel hover must not create an active tooltip window");
            }
        };
        auto image_bounds = [](ImTextureID texture) {
            ImVec2 first(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()),
                    last(-first.x, -first.y);
            bool found = false;

            for (const auto *draw : ImGui::GetDrawData()->CmdLists) {
                for (const auto &command : draw->CmdBuffer) {
                    if (command.TexRef._TexData || command.TexRef._TexID != texture ||
                        command.UserCallback) {
                        continue;
                    }

                    for (unsigned index = 0; index < command.ElemCount; ++index) {
                        const auto &point =
                                draw->VtxBuffer[command.VtxOffset +
                                                draw->IdxBuffer[command.IdxOffset + index]]
                                        .pos;
                        first.x = std::min(first.x, point.x);
                        first.y = std::min(first.y, point.y);
                        last.x = std::max(last.x, point.x);
                        last.y = std::max(last.y, point.y);
                        found = true;
                    }
                }
            }

            check(found, "actual headless draw data must contain both wheel photo quads");

            return ImRect(first, last);
        };
        auto highlight_alpha = [](ImVec2 point) {
            unsigned alpha = 0;
            auto cross = [](ImVec2 a, ImVec2 b, ImVec2 p) {
                return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            };

            for (const auto *draw : ImGui::GetDrawData()->CmdLists) {
                for (const auto &command : draw->CmdBuffer) {
                    if (command.UserCallback || point.x < command.ClipRect.x ||
                        point.y < command.ClipRect.y || point.x > command.ClipRect.z ||
                        point.y > command.ClipRect.w) {
                        continue;
                    }

                    for (unsigned index = 0; index + 2 < command.ElemCount; index += 3) {
                        const auto &a = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + index]];
                        const auto &b =
                                draw->VtxBuffer[command.VtxOffset +
                                                draw->IdxBuffer[command.IdxOffset + index + 1]];
                        const auto &c =
                                draw->VtxBuffer[command.VtxOffset +
                                                draw->IdxBuffer[command.IdxOffset + index + 2]];

                        if (a.col != b.col || a.col != c.col) {
                            continue;
                        }

                        const auto rgb = a.col & ~IM_COL32_A_MASK;

                        if (rgb != (IM_COL32(225, 234, 255, 0) & ~IM_COL32_A_MASK) &&
                            rgb != (IM_COL32(88, 164, 255, 0) & ~IM_COL32_A_MASK)) {
                            continue;
                        }

                        const float ab = cross(a.pos, b.pos, point),
                                    bc = cross(b.pos, c.pos, point),
                                    ca = cross(c.pos, a.pos, point);

                        if (std::abs(cross(a.pos, b.pos, c.pos)) < .0001f ||
                            ((ab < -.001f || bc < -.001f || ca < -.001f) &&
                             (ab > .001f || bc > .001f || ca > .001f))) {
                            continue;
                        }

                        alpha = std::max(alpha, (a.col >> IM_COL32_A_SHIFT) & 0xffu);
                    }
                }
            }

            return alpha;
        };
        auto phases = [&](const std::string &id, const char *phase) {
            std::lock_guard lock(mutex);

            return std::count_if(events.begin(), events.end(), [&](const Json &event) {
                return event.at("id") == id && event.at("phase") == phase;
            });
        };
        frame(ImVec2(20, 20));
        frame(ImVec2(20, 20));
        auto solid_at = [](ImVec2 point, ImU32 color) {
            auto cross = [](ImVec2 a, ImVec2 b, ImVec2 p) {
                return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            };
            bool found = false;

            for (const auto *draw : ImGui::GetDrawData()->CmdLists) {
                for (const auto &command : draw->CmdBuffer) {
                    if (command.UserCallback || command.TexRef._TexID == 1001 ||
                        command.TexRef._TexID == 1002 || command.TexRef._TexID == 1003 ||
                        point.x < command.ClipRect.x || point.y < command.ClipRect.y ||
                        point.x >= command.ClipRect.z || point.y >= command.ClipRect.w) {
                        continue;
                    }

                    for (unsigned i = 0; i + 2 < command.ElemCount; i += 3) {
                        const auto &a = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + i]];
                        const auto &b = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + i + 1]];
                        const auto &c = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + i + 2]];

                        if (std::abs(cross(a.pos, b.pos, c.pos)) < .0001f) {
                            continue;
                        }

                        const float ab = cross(a.pos, b.pos, point),
                                    bc = cross(b.pos, c.pos, point),
                                    ca = cross(c.pos, a.pos, point);

                        if ((ab < -.001f || bc < -.001f || ca < -.001f) &&
                            (ab > .001f || bc > .001f || ca > .001f)) {
                            continue;
                        }

                        if (a.col == b.col && a.col == c.col &&
                            (a.col & IM_COL32_A_MASK) == IM_COL32_A_MASK) {
                            found = a.col == color;
                        }
                    }
                }
            }

            return found;
        };
        check(solid_at(ImVec2(1000, 160), IM_COL32(0, 0, 0, 255)),
              "background under wheel is black");
        check(solid_at(ImVec2(1000, 359), IM_COL32(0, 0, 0, 255)),
              "parameter and tab background is black as requested");
        check(solid_at(ImVec2(959.75f, 200), IM_COL32(28, 40, 56, 255)),
              "vertical dark blue divider separates QNX and controls");
        check(solid_at(ImVec2(500, 359.75f), IM_COL32(28, 40, 56, 255)),
              "horizontal dark blue divider separates QNX and Android");
        auto lifecycle_ink = [](ImVec2 point) {
            float alpha = 0;
            auto cross = [](ImVec2 a, ImVec2 b, ImVec2 p) {
                return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            };
            const auto rgb = IM_COL32(255, 255, 255, 0) & ~IM_COL32_A_MASK;

            for (const auto *draw : ImGui::GetDrawData()->CmdLists) {
                for (const auto &command : draw->CmdBuffer) {
                    if (command.UserCallback || point.x < command.ClipRect.x ||
                        point.y < command.ClipRect.y || point.x >= command.ClipRect.z ||
                        point.y >= command.ClipRect.w) {
                        continue;
                    }

                    for (unsigned i = 0; i + 2 < command.ElemCount; i += 3) {
                        const auto &a = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + i]];
                        const auto &b = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + i + 1]];
                        const auto &c = draw->VtxBuffer[command.VtxOffset +
                                                        draw->IdxBuffer[command.IdxOffset + i + 2]];

                        if ((a.col & ~IM_COL32_A_MASK) != rgb ||
                            (b.col & ~IM_COL32_A_MASK) != rgb ||
                            (c.col & ~IM_COL32_A_MASK) != rgb) {
                            continue;
                        }

                        const float area = cross(a.pos, b.pos, c.pos);

                        if (std::abs(area) < .0001f) {
                            continue;
                        }

                        const float wa = cross(b.pos, c.pos, point) / area,
                                    wb = cross(c.pos, a.pos, point) / area, wc = 1 - wa - wb;

                        if (wa < -.001f || wb < -.001f || wc < -.001f) {
                            continue;
                        }

                        const auto opacity = [](ImU32 color) {
                            const auto alpha = (color >> IM_COL32_A_SHIFT) & 255;
                            check(alpha <= 77,
                                  "disabled lifecycle core uses the original Music 77/255 opacity");

                            return float(alpha) / 77;
                        };
                        alpha = std::max(alpha, wa * opacity(a.col) + wb * opacity(b.col) +
                                                        wc * opacity(c.col));
                    }
                }
            }

            return alpha;
        };
        const auto column = wheel_column(100, {}, {}, 130, 0);
        const auto power = column.cluster_power, restart = column.cluster_restart;
        const float lifecycle_scale = power.width / 28.f, ring_radius = 6.5f * lifecycle_scale;
        const ImVec2 power_center(power.x + power.width / 2, power.y + power.height / 2),
                restart_center(restart.x + restart.width / 2, restart.y + restart.height / 2);
        check(lifecycle_ink(ImVec2(power_center.x + ring_radius, power_center.y)) > .5f,
              "power ring must continue at the right side");
        check(lifecycle_ink(ImVec2(power_center.x - ring_radius, power_center.y)) > .5f,
              "power ring must continue at the left side");
        check(lifecycle_ink(ImVec2(power_center.x, power_center.y + ring_radius)) > .5f,
              "power ring must continue below the stem");
        check(lifecycle_ink(ImVec2(power_center.x + 2 * lifecycle_scale,
                                   power_center.y - std::sqrt(6.5f * 6.5f - 4) * lifecycle_scale)) <
                      .1f,
              "power ring gap must be above, right of its vertical stem");
        check(lifecycle_ink(ImVec2(power_center.x - 2 * lifecycle_scale,
                                   power_center.y - std::sqrt(6.5f * 6.5f - 4) * lifecycle_scale)) <
                      .1f,
              "power ring gap must be above, left of its vertical stem");
        check(lifecycle_ink(ImVec2(power_center.x, power_center.y - 4 * lifecycle_scale)) > .5f,
              "power stem stays vertical and centered in the top gap");
        const float angle = 4.9f;
        const ImVec2 tip(restart_center.x + ring_radius * std::cos(angle),
                         restart_center.y + ring_radius * std::sin(angle));
        const ImVec2 tangent(-std::sin(angle), std::cos(angle)), normal(-tangent.y, tangent.x);
        const ImVec2 first(tip.x + (-2.5f * tangent.x + 1.6f * normal.x) * lifecycle_scale,
                           tip.y + (-2.5f * tangent.y + 1.6f * normal.y) * lifecycle_scale),
                last(tip.x + (-2.5f * tangent.x - 1.6f * normal.x) * lifecycle_scale,
                     tip.y + (-2.5f * tangent.y - 1.6f * normal.y) * lifecycle_scale);
        check(lifecycle_ink(tip) > .5f, "restart arrow tip must join the exact arc endpoint");
        check(lifecycle_ink(first) > .5f && lifecycle_ink(last) > .5f,
              "both restart arrow wings must trail the endpoint tangent");
        check(lifecycle_ink(ImVec2(restart_center.x + 5 * lifecycle_scale,
                                   restart_center.y - 8 * lifecycle_scale)) < .1f,
              "restart must not retain a detached upright arrow");
        const auto toggle = column.screens_toggle;
        const ImVec2 toggle_center(toggle.x + toggle.width / 2, toggle.y + toggle.height / 2),
                toggle_edge(toggle_center.x + toggle.width * .4f, toggle_center.y);
        window.set_screens({"rear"});
        window.set_screens_window(true);
        frame(toggle_center);
        frame(ImVec2(20, 20));
        check(solid_at(toggle_edge, IM_COL32(0, 0, 0, 255)),
              "the open screens toggle draws no background disc and no hover tooltip");
        window.set_screens_window(false);
        window.set_screens({});
        const auto left = image_bounds(1002), right = image_bounds(1003);
        auto position = [&](std::string_view side, const std::array<double, 2> &point) {
            const auto bounds = side == "left" ? left : right;

            return ImVec2(
                    bounds.Min.x + static_cast<float>(point[0]) * (bounds.Max.x - bounds.Min.x),
                    bounds.Min.y + static_cast<float>(point[1]) * (bounds.Max.y - bounds.Min.y));
        };

        for (const auto &fixture : fixtures.at("regions")) {
            const std::string id = fixture.at("id"), side = fixture.at("photo");
            const auto mouse = position(
                    side, fixture.at("interior_point_normalized").get<std::array<double, 2>>());
            frame(mouse);
            const auto hover = highlight_alpha(mouse);
            check(hover > 0 && hover <= 48,
                  "each exact wheel face must draw a soft low-alpha hover highlight");
            frame(mouse, true);
            const auto pressed = highlight_alpha(mouse);
            check(pressed > hover && pressed <= 128,
                  "pointer down must draw a stronger translucent face highlight");
            wait(
                    [&] {
                        return phases(id, "down") == 1;
                    },
                    "a rendered wheel press did not reach its accepted route");
            frame(mouse, false);
            wait(
                    [&] {
                        return phases(id, "up") == 1;
                    },
                    "a rendered wheel release did not reach its accepted route");
            frame(ImVec2(20, 20));
            check(highlight_alpha(mouse) == 0,
                  "mouse clicks must not leave a focused wheel highlight after the pointer leaves");
        }

        for (const auto &roller : geometry.rollers) {
            for (float direction : {1.f, -1.f}) {
                const std::array<double, 2> point = {roller.rect[0] + roller.rect[2] / 2,
                                                     roller.rect[1] + roller.rect[3] / 4};
                check(roller.contains(point[0], point[1]),
                      "roller fixture must lie inside the actual physical contour");
                const auto mouse = position(roller.photo, point);
                const auto id = direction > 0 ? roller.up : roller.down;
                const auto downs = phases(id, "down"), ups = phases(id, "up");
                frame(mouse, false, direction);
                const auto alpha = highlight_alpha(mouse);
                check(roller.photo == "left" ? alpha == 0 : (alpha > 0 && alpha <= 48),
                      "only the right clickable roller highlights; the left rocker only dispatches detents");
                wait(
                        [&] {
                            return phases(id, "down") == downs + 1 && phases(id, "up") == ups + 1;
                        },
                        "roller wheel input must preserve its accepted atomic down/up pair");
            }
        }

        const auto normal_font = window.inspection().at("font").at("size_points");
        screen_scale = canonical_ui_scale;
        frame(ImVec2(20, 20));
        frame(ImVec2(20, 20));
        const auto compact_frame = image_bounds(1001), compact_left = image_bounds(1002),
                   compact_right = image_bounds(1003);
        check(compact_frame.GetWidth() == 668 && compact_frame.GetHeight() == 250.5f,
              "canonical draw data preserves QNX aspect ratio");
        check(std::abs(compact_left.GetWidth() * 460 / 320 - left.GetWidth()) < .001f &&
                      std::abs(compact_right.GetHeight() * 460 / 320 - right.GetHeight()) < .001f,
              "actual wheel images keep normal readable dimensions at50percent");
        check(std::abs((compact_left.Min.x - 668) * 460 / 320 - (left.Min.x - 960)) < .001f &&
                      window.inspection().at("font").at("size_points") == normal_font,
              "canonical wheel and controls share design coordinates before framebuffer scaling");

        for (const auto &item : window.inspection().at("fields")) {
            if (item.at("kind") != "lifecycle") {
                check(item.at("fully_inside_clip") == true,
                      "compact fields stay fully visible in the parameter column");
            }
        }

        const auto tab =
                window.inspection().at("tabs").at(1).at("rect_points").get<std::array<float, 4>>();
        const ImVec2 compact_tab(tab[0] + tab[2] / 2, tab[1] + tab[3] / 2);
        frame(compact_tab);
        frame(compact_tab, true);
        frame(compact_tab);
        check(window.inspection().at("selected_section") == 1,
              "actual tab click reaches relocated compact parameters");
        const auto &first_fixture = fixtures.at("regions").at(0);
        const auto normalized =
                first_fixture.at("interior_point_normalized").get<std::array<double, 2>>();
        const auto compact_photo = first_fixture.at("photo") == "left" ? compact_left
                                                                       : compact_right;
        const ImVec2 compact_mouse(
                compact_photo.Min.x + float(normalized[0]) * compact_photo.GetWidth(),
                compact_photo.Min.y + float(normalized[1]) * compact_photo.GetHeight());
        const auto compact_id = first_fixture.at("id").get<std::string>();
        const auto before_down = phases(compact_id, "down"), before_up = phases(compact_id, "up");
        frame(compact_mouse);
        check(highlight_alpha(compact_mouse) > 0,
              "wheel hit contour follows its compact layout position");
        frame(compact_mouse, true);
        frame(compact_mouse);
        wait(
                [&] {
                    return phases(compact_id, "down") == before_down + 1 &&
                           phases(compact_id, "up") == before_up + 1;
                },
                "compact wheel press and release preserve routing");
        screen_scale = 75;
        frame(ImVec2(20, 20));
        frame(ImVec2(20, 20));
        check(image_bounds(1001).GetWidth() == 720 && image_bounds(1001).GetHeight() == 270,
              "actual75percent QNX draw uses the intermediate scale");
        check(window.inspection().at("layout").at("parameters_rect_points") ==
                      Json::array({1065, 0, 375, 270}),
              "actual75percent parameters occupy the column right of the wheel");
        check(std::abs(image_bounds(1002).GetWidth() * 460 / 345 - left.GetWidth()) < .001f &&
                      window.inspection().at("font").at("size_points") == normal_font,
              "75percent leaves controls and fonts readable");

        for (const auto &item : window.inspection().at("fields")) {
            if (item.at("kind") != "lifecycle") {
                check(item.at("fully_inside_clip") == true,
                      "75percent fields stay fully inside the parameter column");
            }
        }

        screen_scale = 100;
        frame(ImVec2(20, 20));
        frame(ImVec2(20, 20));
        check(image_bounds(1001).GetWidth() == 960,
              "returning to100percent restores the original QNX draw extent");
        partial = true;
        wait(
                [&] {
                    window.refresh_state();

                    return window.client().state().wheel_capability ==
                           qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1");
                },
                "partial capability reaches actual native view");

        for (const auto &fixture : fixtures.at("regions")) {
            const auto id = fixture.at("id").get<std::string>();
            const bool available = id == "volume_up" || id == "volume_down" || id == "next" ||
                                   id == "previous" || id == "ok" || id == "photo" ||
                                   id == "voice" || id == "phone";
            check(wheel_action_available(window.client().state(), id) == available,
                  "partial view enables exactly eight verified OEM routes");
            const auto mouse =
                    position(fixture.at("photo").get<std::string>(),
                             fixture.at("interior_point_normalized").get<std::array<double, 2>>());
            const auto downs = phases(id, "down"), ups = phases(id, "up");
            frame(mouse);
            const auto partial_hover = highlight_alpha(mouse);
            check(partial_hover > 0 && partial_hover <= 48,
                  "partial view keeps a soft hover highlight on every face including unsupported ones");
            frame(mouse, true);
            frame(mouse, true);
            const auto partial_pressed = highlight_alpha(mouse);

            if (available) {
                check(partial_pressed > partial_hover && partial_pressed <= 128,
                      "supported pointer down keeps the stronger translucent press highlight");
                frame(mouse, false);
                wait(
                        [&] {
                            return phases(id, "down") == downs + 1 && phases(id, "up") == ups + 1;
                        },
                        "partial OEM button click routes through actual UI");
            } else {
                check(partial_pressed >= 105 && partial_pressed <= 128,
                      "unsupported pointer down draws the same stronger pressed highlight");
                check(phases(id, "down") == downs && phases(id, "up") == ups,
                      "unsupported press, hold and release dispatch exactly zero wheel events");
                frame(mouse, false);
                frame(mouse, false);
                check(highlight_alpha(mouse) < 48,
                      "releasing the pointer returns the unsupported face to hover");
                frame(mouse, true);
                frame(mouse, true);
                check(highlight_alpha(mouse) >= 105,
                      "pressing again re-enters the unsupported pressed highlight");
                frame(mouse, false);
                check(!window.client().wheel(id, "down") && !window.client().detent(id) &&
                              phases(id, "down") == downs,
                      "unsupported partial wheel cannot enqueue pointer or roller down");
            }
        }

        for (const char *family : {"nxp", "passion"}) {
            auto unavailable = window.client().state();
            unavailable.deliveries["android"]["presentation_family"] = family;
            unavailable.routes = Json::object();
            check(!wheel_action_available(unavailable, "menu") &&
                          !wheel_action_available(unavailable, "volume_up"),
                  "visible generic cockpit must not dispatch unverified wheel routes");
        }

        auto historical = window.client().state();
        historical.wheel_capability = {{"schema", 1},
                                       {"profile", "free-11-chinese-20260731-7.3.1"},
                                       {"status", "unsupported"},
                                       {"supported", false},
                                       {"reason", "oem_delivery_unverified"},
                                       {"authority", "com.voyemu.input.wheel"}};
        check(!wheel_action_available(historical, "volume_up") &&
                      !wheel_action_available(historical, "menu"),
              "historical unsupported Free graph disables Android wheel controls");
        historical.wheel_capability["supported"] = true;
        check(!wheel_action_available(historical, "volume_up"),
              "malformed Free capability cannot visually grant support");
        historical.wheel_capability =
                qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1");
        historical.routes["volume_up"]["android"] = {{"key_code", 2}};
        check(wheel_action_available(historical, "volume_up"),
              "partial Free capability grants its exact Android route");
        historical.wheel_capability = nullptr;
        check(wheel_action_available(historical, "menu"),
              "A9 explicit null capability keeps existing wheel behavior");
        auto view = window.client().state();
        view.routes["menu"]["qnx_index"] = 4;
        view.deliveries["qnx"] = {{"lifecycle_supported", true}, {"status", "graphics_running"}};
        check(wheel_action_available(view, "menu"), "real active QNX route remains available");
        view.deliveries["qnx"]["status"] = "off";
        check(!wheel_action_available(view, "menu"),
              "powered-off QNX cannot grant a partial wheel route");
        view.deliveries["qnx"]["status"] = "graphics_running";
        view.deliveries["qnx"]["lifecycle_supported"] = false;
        check(!wheel_action_available(view, "menu"),
              "unattached QNX cannot grant a partial wheel route");
        view.routes["menu"]["qnx_index"] = nullptr;
        view.routes["menu"]["ecu"] = Json::object();
        view.deliveries["qnx"]["lifecycle_supported"] = true;
        check(!wheel_action_available(view, "menu"),
              "ADAS-only metadata cannot grant a public Free route");
        check(window.client().wheel("menu", "up") && window.client().wheel("menu", "cancel"),
              "partial availability never blocks release safety");
        window.close();
        ImGui::DestroyContext();
    } catch (...) {
        ImGui::DestroyContext();
        throw;
    }
}

void unavailable_toggle(const std::filesystem::path &assets) {
    ImGui::CreateContext();
    try {
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DeltaTime = 1.f / 60;
        io.ConfigInputTrickleEventQueue = false;
        load_ui_fonts(fonts_directory);
        const auto geometry = view_layout(canonical_ui_scale);
        io.DisplaySize = ImVec2(geometry.canvas_width, geometry.canvas_height);
        io.DisplayFramebufferScale = ImVec2(1920.f / canonical_ui_width,
                                            1920.f / canonical_ui_width);
        std::atomic_size_t patches{};
        auto transport = [&](const Json &operation) -> Json {
            if (operation.at("op") == "patch") {
                ++patches;
            }

            throw std::runtime_error("fixture unavailable");
        };
        Options options;
        options.assets = assets;
        options.endpoint = "unavailable-toggle-fixture";
        options.capture_ui = "headless-unavailable-inspection";
        options.inspect_panel = 2;
        options.smoke_seconds = .25;
        Window window(options, transport);
        auto render = [&](ImVec2 mouse, bool down) {
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame();
            window.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308}, canonical_ui_scale);
            ImGui::Render();
        };

        for (int frame = 0; frame < 60; ++frame) {
            render(ImVec2(0, 0), false);
        }

        std::array<float, 4> rect{};
        bool found = false;

        for (const auto &item : window.inspection().at("fields")) {
            if (item.at("name") == "door_fl") {
                found = true;
                check(!item.at("enabled").get<bool>(), "unknown toggle is disabled");
                rect = item.at("rect_points").get<std::array<float, 4>>();
            }
        }

        check(found, "unknown toggle is rendered");
        bool track = false, knob = false;
        const auto ink = toggle_ink(false);

        each_vertex([&](const ImDrawVert &vertex, const ImDrawCmd &) {
            if (vertex.pos.x >= rect[0] - 1 && vertex.pos.x <= rect[0] + 43 &&
                vertex.pos.y >= rect[1] && vertex.pos.y <= rect[1] + 26 &&
                (vertex.col >> IM_COL32_A_SHIFT) != 0) {
                if (same_rgb(vertex.col, ink.track)) {
                    track = true;
                    check((vertex.col >> IM_COL32_A_SHIFT) ==
                                  unsigned(std::lround((ink.track >> IM_COL32_A_SHIFT) * .3f)),
                          "unavailable toggle track uses the established 0.3 alpha");
                } else if (same_rgb(vertex.col, IM_COL32(255, 255, 255, 255))) {
                    knob = true;
                    check((vertex.col >> IM_COL32_A_SHIFT) <= 77,
                          "unavailable toggle knob uses the established 0.3 alpha");
                    check(vertex.pos.x <= rect[0] + 22, "unavailable knob stays on the left");
                }
            }
        });

        check(track && knob, "unavailable toggle draws its own track and knob");
        render(ImVec2(rect[0] + 20, rect[1] + 13), false);
        render(ImVec2(rect[0] + 20, rect[1] + 13), true);
        render(ImVec2(rect[0] + 20, rect[1] + 13), false);
        check(patches == 0, "clicking an unknown toggle sends no patch");
        window.close();
        const auto canonical = initial_state(VehicleCatalog::qinggan09);
        std::atomic_size_t known_patches{};
        auto ready_transport = [&](const Json &operation) -> Json {
            if (operation.at("op") == "patch") {
                ++known_patches;
            }

            return Json{{"ok", true},
                        {"session_id", "known-toggle-fixture"},
                        {"revision", canonical.at("sequence")},
                        {"state", canonical},
                        {"catalog", catalog_json(VehicleCatalog::qinggan09)}};
        };
        options.endpoint = "known-toggle-fixture";
        Window known(options, ready_transport);
        wait(
                [&] {
                    return known.client().state().ready;
                },
                "known toggle fixture must have a canonical state");
        auto render_known = [&](ImVec2 mouse, bool down) {
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame();
            known.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308}, canonical_ui_scale);
            ImGui::Render();
        };

        for (int frame = 0; frame < 60; ++frame) {
            render_known(ImVec2(0, 0), false);
        }

        found = false;

        for (const auto &item : known.inspection().at("fields")) {
            if (item.at("name") == "door_fl") {
                found = true;
                check(item.at("enabled").get<bool>(), "known toggle is enabled");
                rect = item.at("rect_points").get<std::array<float, 4>>();
            }
        }

        check(found && rect[2] > 0 && rect[3] > 0, "known toggle is rendered");
        const ImVec2 center(rect[0] + rect[2] / 2, rect[1] + rect[3] / 2);
        render_known(center, false);
        render_known(center, true);
        render_known(center, false);
        wait(
                [&] {
                    return known_patches == 1;
                },
                "clicking a known toggle sends a patch");
        known.close();
        ImGui::DestroyContext();
    } catch (...) {
        ImGui::DestroyContext();
        throw;
    }
}

void editor_sections_fill_available_height(const std::filesystem::path &assets,
                                           bool light = false) {
    struct LanguageRestore {
        ui::Language previous = ui::current_language;

        ~LanguageRestore() {
            ui::current_language = previous;
        }
    } language_restore;

    ImGui::CreateContext();
    try {
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DeltaTime = 1.f / 60;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        load_ui_fonts(fonts_directory);
        check(ui_text_font(UiFontRole::Body) == ui_text_font(UiFontRole::Label) &&
                      ui_text_font(UiFontRole::Body) != ui_text_font(UiFontRole::Emphasis),
              "production UI uses the requested Regular body/label and Medium tab/group roles");
        const ImU32 active_color = light ? IM_COL32(66, 158, 253, 255)
                                         : IM_COL32(71, 180, 255, 255),
                    inactive_color = light ? IM_COL32(80, 89, 102, 255)
                                           : IM_COL32(202, 202, 202, 255),
                    text_color = light ? IM_COL32(45, 52, 66, 255) : IM_COL32(255, 255, 255, 255),
                    input_color = light ? IM_COL32(255, 255, 255, 255) : IM_COL32(55, 63, 74, 255),
                    surface_color = light ? IM_COL32(255, 255, 255, 255)
                                          : IM_COL32(12, 20, 30, 255),
                    border_color = light ? IM_COL32(151, 151, 151, 80) : IM_COL32(47, 67, 91, 255);
        bool chinese_covered = true;
        std::size_t matrix_cases = 0, scroll_cases = 0;
        std::array<float, 3> underline_y{};
        const char *requested_scale = std::getenv("VOYEMU_UI_TEST_SCALE");
        const bool fifty_only = requested_scale && std::string_view(requested_scale) == "50";

        if (requested_scale && !fifty_only) {
            throw std::runtime_error("VOYEMU_UI_TEST_SCALE only accepts50");
        }

        const std::vector<unsigned> scales = fifty_only ? std::vector<unsigned>{canonical_ui_scale}
                                                        : std::vector<unsigned>{canonical_ui_scale,
                                                                                75u, 100u};
        Json fixture = {{"door_fl", 1},
                        {"speed", 255},
                        {"soc", 100},
                        {"odo", 16777215},
                        {"remain_mileage_e", 2046},
                        {"remain_mileage_f", 1022},
                        {"ambient_temp", 255},
                        {"fuel_percent", 100},
                        {"actual_power", 100},
                        {"allowed_power", 100}};

        for (const auto *corner : {"LF", "RF", "LR", "RR"}) {
            const std::string c = corner;
            fixture["bcm_tyre" + c + "Pressure"] = 70;
            fixture["bcm_tyre" + c + "Temp"] = 255;
            fixture["bcm_tpms" + c + "Sts"] = 1;
            fixture["can_v1_5_bcm_tpms" + c + "Sts"] = 1;
        }

        const auto canonical = apply_patch(initial_state(VehicleCatalog::qinggan09), fixture);
        std::atomic_bool gear_selected{};
        std::atomic_size_t power_events{}, restart_events{}, media_power_events{},
                media_restart_events{}, hazard_events{};
        auto transport = [&](const Json &operation) {
            if (operation.at("op") == "patch" && operation.at("values").value("gear", -1) == 3 &&
                operation.at("values").value("gear_valid", -1) == 1) {
                gear_selected = true;
            }

            if (operation.at("op") == "patch" &&
                operation.at("values") == Json{{"turn_left", 1}, {"turn_right", 1}}) {
                ++hazard_events;
            }

            if (operation.at("op") == "cluster_power") {
                ++power_events;
            }

            if (operation.at("op") == "cluster_restart") {
                ++restart_events;
            }

            if (operation.at("op") == "media_power") {
                ++media_power_events;
            }

            if (operation.at("op") == "media_restart") {
                ++media_restart_events;
            }

            Json result{
                    {"ok", true},
                    {"session_id", "editor-height-test"},
                    {"revision", canonical["sequence"]},
                    {"state", canonical},
                    {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                    {"wheel_routes", EventQueue::routes()},
                    {"deliveries",
                     {{"qnx", {{"status", "graphics_running"}, {"lifecycle_supported", true}}}}}};
            result["deliveries"]["android"]["appearance"] = {{"status", "observed"},
                                                             {"scheme", light ? "light" : "dark"}};
            result["deliveries"]["android"]["lifecycle_supported"] = true;
            result["deliveries"]["android"]["powered"] = true;

            return result;
        };

        for (const auto language :
             {ui::Language::russian, ui::Language::english, ui::Language::chinese}) {
            for (unsigned scale : scales) {
                for (std::size_t section = 0; section < 5; ++section) {
                    ui::current_language = language;
                    const auto geometry = view_layout(scale);
                    const auto layout = parameters_layout(closed_width, closed_height,
                                                          editor_height, scale);
                    Options options;
                    options.assets = assets;
                    options.endpoint = "editor-height-fixture";
                    options.capture_ui = "headless-editor-inspection";
                    options.inspect_panel = section;
                    options.smoke_seconds = .25;
                    Window window(options, transport);
                    wait(
                            [&] {
                                return window.client().state().ready;
                            },
                            "editor fixture must have a canonical state");
                    io.DisplaySize = ImVec2(geometry.canvas_width, geometry.canvas_height);
                    const float density = scale == canonical_ui_scale ? 1920.f / canonical_ui_width
                                                                      : 2.f;
                    io.DisplayFramebufferScale = ImVec2(density, density);
                    io.AddMousePosEvent(20, 20);

                    for (int frame = 0; frame < 2; ++frame) {
                        ImGui::NewFrame();
                        window.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308}, scale);
                        ImGui::Render();
                    }

                    bool active_tab_color = false, inactive_tab_color = false;

                    for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                        for (const auto &vertex : list->VtxBuffer) {
                            if (vertex.pos.y < layout.tabs.y ||
                                vertex.pos.y >= layout.tabs.bottom() + 6) {
                                continue;
                            }

                            active_tab_color |= vertex.col == active_color;
                            inactive_tab_color |= vertex.col == inactive_color;
                        }
                    }

                    check(active_tab_color && inactive_tab_color,
                          "OEM Music active and inactive exact text colors are present at every scale and section");
                    bool actual_child = false;

                    for (const auto *child : ImGui::GetCurrentContext()->Windows) {
                        if (child->Active &&
                            std::string(child->Name).find("/parameters_") != std::string::npos &&
                            std::string(child->Name).find("/signals_") == std::string::npos) {
                            // ImGui::SetWindowSize truncates explicit sizes to whole design
                            // coordinates. Require the exact toolkit result, not an arbitrary
                            // tolerance that could hide a missing row.
                            const auto expected_size = ImTrunc(
                                    ImVec2(geometry.parameters.width, geometry.parameters.height));
                            check(child->Pos.x == geometry.parameters.x &&
                                          child->Pos.y == geometry.parameters.y &&
                                          child->Size.x == expected_size.x &&
                                          child->Size.y == expected_size.y,
                                  "actual ImGui parameters child fills the exact pixel-quantized available rectangle at every scale");
                            check(geometry.parameters.width - child->Size.x >= 0 &&
                                          geometry.parameters.width - child->Size.x < 1 &&
                                          geometry.parameters.height - child->Size.y >= 0 &&
                                          geometry.parameters.height - child->Size.y < 1,
                                  "only ImGui sub-coordinate quantization may reduce the child extent");
                            actual_child = true;
                        }
                    }

                    check(actual_child, "actual parameters child must be rendered");
                    const auto &tabs = window.inspection().at("tabs");
                    check(tabs.size() == 5, "all five localized tabs are rendered");
                    const std::array<const char *, 5> localized_titles = {
                            ui::tr("Движение"), ui::tr("Свет"), ui::tr("Кузов"), ui::tr("Шины"),
                            ui::tr("Все")};
                    float previous_tab_right = layout.body.x - 8;

                    for (std::size_t i = 0; i < tabs.size(); ++i) {
                        const auto r = tabs[i].at("rect_points").get<std::array<float, 4>>();
                        check(tabs[i].at("name") == localized_titles[i],
                              "actual tab labels follow the selected locale");
                        check(r[0] >= previous_tab_right + 7.99f &&
                                      r[0] + r[2] <= layout.body.right() + .001f,
                              "localized tab hit regions fit and never overlap");
                        check(tabs[i].at("fully_inside_clip") == true,
                              "localized tab hit regions stay within the actual panel");
                        previous_tab_right = r[0] + r[2];
                    }

                    if (layout.compact50()) {
                        const auto
                                first = tabs.front().at("rect_points").get<std::array<float, 4>>(),
                                second = tabs[1].at("rect_points").get<std::array<float, 4>>();
                        const float gap = second[0] - first[0] - first[2];
                        check(first[0] == layout.movement_row(0, 0).x && gap > 8,
                              "50percent first tab shares the numeric input left edge");

                        for (std::size_t i = 1; i < tabs.size(); ++i) {
                            const auto
                                    a = tabs[i - 1].at("rect_points").get<std::array<float, 4>>(),
                                    b = tabs[i].at("rect_points").get<std::array<float, 4>>();
                            check(std::abs(b[0] - a[0] - a[2] - gap) < .001f,
                                  "50percent internal tab spacing remains uniform");
                        }

                        check(std::abs((layout.body.right() - layout.content_inset() -
                                        previous_tab_right) *
                                               density -
                                       8) < .001f,
                              "50percent four gaps shrink by two physical pixels each, leaving eight extra pixels after All");
                    }

                    const auto &reference = window.inspection().at("tab_reference");
                    const auto line =
                            reference.at("underline_rect_points").get<std::array<float, 4>>();
                    const float guest_scale = scale == canonical_ui_scale ? .5f : scale / 100.f;
                    check(std::abs(reference.at("sample_ink_pixels").get<float>() -
                                   (27 * guest_scale + (layout.compact50() ? 2 : 0))) <= 1.1f,
                          "actual Medium baked BT Music ink matches the measured OEM pixel height");
                    check(std::abs(line[2] * density - 42 * guest_scale) < .001f &&
                                  std::abs(line[3] * density - 6 * guest_scale) < .001f,
                          "underline physical size follows the measured42x6OEM artwork");
                    check(std::abs((line[1] -
                                    reference.at("sample_ink_bottom_points").get<float>()) *
                                           density -
                                   23 * guest_scale) < .001f,
                          "fixed underline baseline has the measured OEM23pixel reference ink gap");
                    const std::size_t scale_index = scale == canonical_ui_scale ? 0
                                                    : scale == 75               ? 1
                                                                                : 2;

                    if (section == 0) {
                        underline_y[scale_index] = line[1];
                    } else {
                        check(underline_y[scale_index] == line[1],
                              "underline vertical position never changes with selected tab or descenders");
                    }

                    if (!layout.compact50()) {
                        check(std::abs(window.inspection()
                                               .at("input_reference")
                                               .at("sample_ink_pixels")
                                               .get<float>() -
                                       24 * guest_scale) <= 1.1f,
                              "75/100 numeric glyphs match the measured AP Hotspot input and never exceed its original size");
                    }

                    const float heading_size = window.inspection()
                                                       .at("checkbox_reference")
                                                       .at("heading_size_points")
                                                       .get<float>();
                    check(std::abs(window.inspection()
                                           .at("checkbox_reference")
                                           .at("sample_ink_pixels")
                                           .get<float>() -
                                   (layout.compact50() ? 38.f / (4832.f / 3840.f) : 28.f) *
                                           guest_scale) <= 1.1f,
                          "actual Regular checkbox reference ink matches the normalized original Seat PNG");

                    if (section == 4) {
                        for (const auto &field : window.inspection().at("fields")) {
                            if (field.at("name") == "raw_search") {
                                const auto r = field.at("rect_points").get<std::array<float, 4>>();
                                check(std::abs(r[0] - layout.body.x - layout.content_inset()) <
                                                      .001f &&
                                              std::abs(r[2] - layout.body.width +
                                                       2 * layout.content_inset()) < .001f &&
                                              r[1] >= layout.tabs.bottom() + 14,
                                      "localized search shares the content inset below the tab row");
                                check(field.at("fully_inside_clip") == true,
                                      "localized search fits the actual panel clipping rectangle");
                            }
                        }
                    }

                    const auto &fields = window.inspection().at("fields");
                    std::size_t checkboxes = 0, numeric = 0;
                    float last_bottom = 0, first_raw = 100000;

                    for (const auto &item : fields) {
                        const auto kind = item.at("kind").get<std::string>();
                        const auto rect = item.at("rect_points").get<std::array<float, 4>>();

                        // Wheel column controls are outside the parameter form.
                        if (kind == "lifecycle" || kind == "about" ||
                            (kind == "choice" &&
                             item.at("name").get<std::string>().rfind("gear_", 0) != 0)) {
                            continue;
                        }

                        if (kind == "checkbox") {
                            ++checkboxes;
                            check(std::abs(rect[3] - 26) < .001f,
                                  "body and light toggles match compact input row height");
                        }

                        if (kind == "numeric") { // Fractional row pitches introduce float
                                                 // subtraction error below0.0001point.
                            check(std::abs(rect[3] - layout.input_height()) < .0001f,
                                  "numeric input height follows available space at each scale");

                            if (layout.compact50()) {
                                check(std::abs((15 -
                                                item.at("text_left_padding_points").get<float>()) *
                                                       density -
                                               2) < .001f,
                                      "50percent value glyph origin moves two physical pixels left from its previous inset");
                                check(std::abs(rect[3] * density - 40) < .001f &&
                                              std::abs(item.at("capsule_radius_points")
                                                                       .get<float>() *
                                                               density -
                                                       20) < .001f &&
                                              item.at("normal_border_points") == 0,
                                      "50percent AP Hotspot capsule is exactly40pixels high with20pixel radius and no inactive border");

                                if (item.at("unit_width_points").get<float>() > 0) {
                                    check(item.at("unit_right_inset_points").get<float>() *
                                                                  density >=
                                                          3 * density + 4 - .001f &&
                                                  item.at("unit_right_inset_points").get<float>() *
                                                                  density <
                                                          4 * density + 4 + .001f,
                                          "50percent units include the additional two-pixel inset from their actual previously snapped glyph origin");
                                }
                            }

                            check(item.at("font_size_points").get<float>() ==
                                          window.inspection()
                                                  .at("font")
                                                  .at("input_size_points")
                                                  .get<float>(),
                                  "canonical values retain one OEM-bounded font without value-dependent shrinking");
                            const std::string name = item.at("name");
                            const bool limited = section == 3 || (section == 0 && name != "odo" &&
                                                                  name != "remain_mileage_e" &&
                                                                  name != "remain_mileage_f");
                            check(item.at("maximum_digits") == (limited ? 3 : 0),
                                  "only driving and tyre values are limited to three digits; mileage and ranges remain scrollable");
                            check(item.at("static_weight") == 400 &&
                                          item.at("font_name") ==
                                                  ui_text_font(UiFontRole::Body)->GetDebugName(),
                                  "numeric values use the requested production Regular face");
                            check(item.at("value_text_area_points").get<float>() > 0 &&
                                          item.at("horizontal_scroll_required").get<bool>() ==
                                                  (item.at("canonical_text_width_points")
                                                           .get<float>() >=
                                                   item.at("value_text_area_points").get<float>()),
                                  "scroll requirement matches native InputText cursor-follow drawable width");
                            const auto input =
                                    item.at("input_rect_points").get<std::array<float, 4>>();
                            check(input[0] == rect[0] && input[1] == rect[1] &&
                                          input[3] == rect[3] && input[2] <= rect[2],
                                  "numeric control inspection preserves separate actual group and InputText rectangles");
                            check(std::abs(item.at("value_text_area_points").get<float>() -
                                           (input[2] -
                                            item.at("text_left_padding_points").get<float>())) <
                                          .001f,
                                  "reported numeric viewport uses native frame width minus actual left padding");

                            if (item.at("unit_width_points").get<float>() > 0) {
                                check(std::abs(input[0] + input[2] +
                                               item.at("unit_gap_points").get<float>() -
                                               (rect[0] + rect[2] -
                                                item.at("unit_right_inset_points").get<float>() -
                                                item.at("unit_width_points").get<float>())) < .001f,
                                      "actual InputText viewport keeps the measured gap before the right-aligned unit");
                            }

                            if (item.at("input_ink_rect_points").size() == 4) {
                                const auto ink = item.at("input_ink_rect_points")
                                                         .get<std::array<float, 4>>();
                                check(ink[0] >= input[0] - .001f &&
                                              ink[0] + ink[2] <= input[0] + input[2] + .001f,
                                      "actual value glyph vertices are clipped inside their own viewport before the unit");
                            }

                            if (section == 3) {
                                check(std::abs(rect[2] - layout.movement_value_width()) < .001f,
                                      "tyre readings grow only when the two-column grid has room");
                            }

                            if (section == 4) {
                                check(rect[0] >= layout.body.x + layout.content_inset() &&
                                              rect[0] <= layout.body.x + layout.content_inset() + 4,
                                      "raw values share the content left inset, names follow on the right");
                                check(std::abs(rect[2] - layout.raw_input_width()) < .001f,
                                      "raw fields reserve more readable text area at100percent");
                            }

                            ++numeric;
                            first_raw = std::min(first_raw, rect[1]);
                        }

                        last_bottom = std::max(last_bottom, rect[1] + rect[3]);

                        if (section != 4) {
                            check(item.at("fully_inside_clip") == true,
                                  "all semantic fields must fit the actual editor clipping rectangle");
                        }
                    }

                    const auto rect_of = [](const Json &item) {
                        const auto r = item.at("rect_points").get<std::array<float, 4>>();

                        return ParameterRect{r[0], r[1], r[2], r[3]};
                    };
                    const auto overlaps = [](ParameterRect a, ParameterRect b) {
                        return std::min(a.right(), b.right()) - std::max(a.x, b.x) > .01f &&
                               std::min(a.bottom(), b.bottom()) - std::max(a.y, b.y) > .01f;
                    };
                    const auto inside = [](ParameterRect a, ParameterRect b) {
                        return b.x >= a.x - .001f && b.y >= a.y - .001f &&
                               b.right() <= a.right() + .001f && b.bottom() <= a.bottom() + .001f;
                    };

                    for (std::size_t i = 0; i < fields.size(); ++i) {
                        for (std::size_t j = i + 1; j < fields.size(); ++j) {
                            check(!overlaps(rect_of(fields[i]), rect_of(fields[j])),
                                  "actual field, search and lifecycle hit bounds never overlap");
                        }
                    }

                    std::size_t group_count = 0, checkbox_labels = 0;
                    const auto &texts = window.inspection().at("texts");

                    for (std::size_t i = 0; i < texts.size(); ++i) {
                        const auto &text = texts[i];
                        const auto kind = text.at("kind").get<std::string>();
                        const auto r = rect_of(text);
                        const auto allocated =
                                text.at("allocated_points").get<std::array<float, 4>>();
                        check(text.at("single_line") == true,
                              "actual draw-list captions never wrap or contain a second line");
                        const float quarter_size = text.at("font_size_points").get<float>() * 4;
                        check(std::abs(quarter_size - std::round(quarter_size)) < .001f,
                              "fitted captions use a bounded quarter-point baked-font size grid");
                        check(inside({allocated[0], allocated[1], allocated[2], allocated[3]}, r),
                              "measured complete text stays inside its allocated rectangle");
                        check(text.at("fully_inside_clip") == true && inside(layout.panel, r),
                              "all tab, group, field and checkbox labels stay inside actual panel clipping bounds");
                        check(text.at("ink_fully_inside_clip") == true &&
                                      text.at("ink_rect_points").size() == 4,
                              "actual production-font glyph quads are drawn and stay inside the panel clip");

                        if (language != ui::Language::chinese) {
                            check(text.at("glyph_coverage") == true,
                                  "pinned production fonts cover every actual RU/EN caption");
                        } else {
                            chinese_covered = chinese_covered &&
                                              text.at("glyph_coverage").get<bool>();
                        }

                        if (kind == "tab") {
                            check(text.at("font_size_points") == reference.at("font_size_points"),
                                  "all tabs retain the single measured OEM-matched size without locale shrinking");
                        }

                        const auto role = kind == "tab" || kind == "group" ? UiFontRole::Emphasis
                                          : kind == "unit"                 ? UiFontRole::Body
                                                                           : UiFontRole::Label;
                        check(text.at("font_name") == ui_text_font(role)->GetDebugName() &&
                                      text.at("static_weight") ==
                                              (role == UiFontRole::Emphasis ? 500 : 400),
                              "actual tabs and group headings use Medium while labels and units use Regular");

                        if (layout.compact50() && (kind == "checkbox_label" || kind == "group")) {
                            check(text.at("font_size_points") == window.inspection()
                                                                         .at("checkbox_reference")
                                                                         .at("heading_size_points"),
                                  "50percent switches and tyre headings share the unshrunk movement label size");
                        }

                        if (kind == "checkbox_label") {
                            ++checkbox_labels;
                            check(text.at("font_size_points").get<float>() <=
                                          window.inspection()
                                                  .at("checkbox_reference")
                                                  .at("font_size_points")
                                                  .get<float>(),
                                  "single-line checkbox text follows the measured Seat reference size");
                        }

                        if (kind == "numeric_label") {
                            check(text.at("font_size_points") == window.inspection()
                                                                         .at("checkbox_reference")
                                                                         .at("heading_size_points"),
                                  "movement labels and toggle captions retain one measured locale-safe size");
                        }

                        if (kind == "group") {
                            ++group_count;
                        }

                        if (kind == "unit") {
                            continue;
                        }

                        for (std::size_t j = i + 1; j < texts.size(); ++j) {
                            if (texts[j].at("kind") != "unit") {
                                check(!overlaps(r, rect_of(texts[j])),
                                      "actual full tab/group/label text rectangles never overlap");
                            }
                        }

                        for (const auto &field : fields) {
                            if (kind == "checkbox_label" && field.at("kind") == "checkbox" &&
                                field.at("name") == text.at("name")) {
                                continue;
                            }

                            check(!overlaps(r, rect_of(field)),
                                  "group and external label rectangles never overlap other actual input hit regions");
                        }
                    }

                    check(group_count == (section == 1 || section == 2 ? 1
                                          : section == 3               ? 4
                                                                       : 0),
                          "body, indicator and tyre group headings are actually drawn");
                    check(checkbox_labels == checkboxes,
                          "every checkbox has exactly one full single-line caption");

                    if (layout.compact50() && section == 0) {
                        for (const auto &field : fields) {
                            if (field.at("kind") == "numeric") {
                                const std::string name = field.at("name");
                                const bool right = name == "odo" || name == "fuel_percent" ||
                                                   name == "actual_power" ||
                                                   name == "allowed_power" ||
                                                   name == "ambient_temp";
                                const float edge = layout.content_column(2, right ? 1 : 0).x + 71;
                                const auto r = rect_of(field);
                                check(r.width == 71 && r.right() == edge && r.x == edge - 71,
                                      "50percent numeric fields reserve the capsule padding without changing the shared grid");

                                if (name == "speed" || name == "soc" || name == "fuel_percent" ||
                                    name == "actual_power" || name == "allowed_power") {
                                    check(field.at("canonical_text_width_points").get<float>() <=
                                                  field.at("value_text_area_points").get<float>(),
                                          "50 percent three-digit semantic values fit at18points beside their units");
                                }

                                bool label = false;

                                for (const auto &text : texts) {
                                    if (text.at("kind") == "numeric_label" &&
                                        text.at("name") == name) {
                                        const auto a = text.at("allocated_points")
                                                               .get<std::array<float, 4>>();
                                        check(a[0] == edge + 8 && a[2] == 76,
                                              "50 percent movement label x anchors and width are strictly preserved");
                                        label = true;
                                    }
                                }

                                check(label,
                                      "every numeric field retains its approved movement label");
                            }
                        }

                        for (const auto &text : texts) {
                            check(text.at("name") != "gear",
                                  "50 percent gear selector has no redundant Gear caption");
                        }

                        check(2 * ParametersLayout::value_width > layout.movement_value_width(),
                              "the compact gear track stays wider than the three-digit numeric fields");
                    }

                    for (const bool restart : {false, true}) {
                        const auto column = wheel_column(scale, {}, {}, 130, 0);
                        const auto expected = restart ? column.cluster_restart
                                                      : column.cluster_power;
                        bool found = false;

                        for (const auto &item : fields) {
                            if (item.at("name") ==
                                (restart ? "cluster_restart" : "cluster_power")) {
                                const auto r = rect_of(item);
                                check(std::abs(r.width - 2 * column.button_hit_radius) < .001f &&
                                              std::abs(r.height - r.width) < .001f &&
                                              r.width < expected.width,
                                      "actual lifecycle hit square bounds its circle inside the button box");
                                const float center_x = r.x + r.width / 2,
                                            center_y = r.y + r.height / 2;
                                check(std::abs(center_y - (expected.y + expected.height / 2)) <
                                                      .001f &&
                                              std::abs(center_x -
                                                       (expected.x + expected.width / 2)) < .001f,
                                      "actual lifecycle circles remain centered in their click rectangles");
                                found = true;
                            }
                        }

                        check(found,
                              "both lifecycle buttons are present in every geometry matrix case");
                    }

                    for (const auto &item : fields) {
                        if (item.at("kind") != "checkbox" ||
                            !item.at("fully_inside_clip").get<bool>()) {
                            continue;
                        }

                        const auto r = item.at("rect_points").get<std::array<float, 4>>();
                        ImVec2 a(FLT_MAX, FLT_MAX), b(-FLT_MAX, -FLT_MAX);

                        each_vertex([&](const ImDrawVert &vertex, const ImDrawCmd &) {
                            if (vertex.pos.x >= r[0] - 1 && vertex.pos.x <= r[0] + 43 &&
                                vertex.pos.y >= r[1] && vertex.pos.y <= r[1] + 26 &&
                                same_rgb(vertex.col, toggle_ink(light).track)) {
                                a = ImMin(a, vertex.pos);
                                b = ImMax(b, vertex.pos);
                            }
                        });

                        // The anti-aliased fringe widens the track equally on every side.
                        check(std::abs((a.x + b.x) / 2 - r[0] - 41.5f / 2) < .001f &&
                                      std::abs((a.y + b.y) / 2 - r[1] - 13) < .001f &&
                                      std::abs(b.x - a.x - 41.5f) <= 1.001f &&
                                      std::abs(b.y - a.y - 21) <= 1.001f,
                              "own toggle tracks keep 83x42 physical pixels at every scale and locale");
                    }

                    if (section == 2) {
                        const auto find_text = [&](const char *name) -> const Json & {
                            for (const auto &text : texts) {
                                if (text.at("name") == name) {
                                    return text;
                                }
                            }

                            throw std::runtime_error("missing body caption");
                        };
                        const auto heading = rect_of(find_text("doors"));
                        check(find_text("doors").at("text") == ui::tr("Двери"),
                              "body has the localized common Doors heading");
                        check(rect_of(find_text("hood")).bottom() < heading.y &&
                                      rect_of(find_text("tail_gate")).bottom() < heading.y,
                              "bonnet and boot captions precede the Doors group");
                        const std::array<const char *, 4> names = {"door_fl", "door_rl", "door_fr",
                                                                   "door_rr"},
                                                          labels = {ui::tr("Передняя"),
                                                                    ui::tr("Задняя"),
                                                                    ui::tr("Передняя"),
                                                                    ui::tr("Задняя")};

                        for (std::size_t i = 0; i < names.size(); ++i) {
                            const auto &text = find_text(names[i]);
                            check(text.at("text") == labels[i] &&
                                          rect_of(text).y > heading.bottom(),
                                  "doors use full short labels below one group heading");
                        }

                        if (layout.scaled_form()) {
                            check(find_text("doors_left").at("text") == ui::tr("Левые") &&
                                          find_text("doors_right").at("text") == ui::tr("Правые"),
                                  "door columns use complete left/right headings");
                        }

                        if (layout.scaled_form()) {
                            const auto fl = rect_of(find_text("door_fl")),
                                       fr = rect_of(find_text("door_fr")),
                                       rl = rect_of(find_text("door_rl")),
                                       rr = rect_of(find_text("door_rr"));
                            check(fl.y == fr.y && rl.y == rr.y && rl.y > fl.bottom() &&
                                          fl.x == rl.x && fr.x == rr.x && fr.x > fl.right(),
                                  "50 percent doors form front-left/front-right and rear-left/rear-right rows in two columns");
                        }
                    }

                    if (section == 1) {
                        bool heading = false, left = false, right = false;

                        for (const auto &text : texts) {
                            if (text.at("name") == "indicators") {
                                heading = text.at("text") == ui::tr("Поворотник");
                            }

                            if (text.at("name") == "turn_left") {
                                left = text.at("text") == ui::tr("Левый");
                            }

                            if (text.at("name") == "turn_right") {
                                right = text.at("text") == ui::tr("Правый");
                            }
                        }

                        check(heading && left && right,
                              "indicators have one localized heading and two short captions");

                        if (layout.scaled_form()) {
                            ParameterRect low, high, drl;

                            for (const auto &field : fields) {
                                if (field.at("name") == "low_beam") {
                                    low = rect_of(field);
                                }

                                if (field.at("name") == "high_beam") {
                                    high = rect_of(field);
                                }

                                if (field.at("name") == "bcm_DRLStatus") {
                                    drl = rect_of(field);
                                }
                            }

                            check(low.y == high.y && high.x > low.right() &&
                                          std::abs(drl.y - low.y + 34 * layout.form_scale()) <
                                                  .001f,
                                  "50 percent low/high beams share one row and the remaining lamps move upward");
                        }
                    }

                    if (layout.scaled_form() && section == 1) {
                        const auto get = [&](const char *name) {
                            for (const auto &field : window.inspection().at("fields")) {
                                if (field.at("name") == name) {
                                    return field.at("rect_points").get<std::array<float, 4>>();
                                }
                            }

                            throw std::runtime_error("missing light control");
                        };
                        const auto position = get("pos_lamp"), daytime = get("bcm_DRLStatus"),
                                   low = get("low_beam"), high = get("high_beam"),
                                   fog = get("rear_fog"), hazard = get("hazard");
                        check(position[1] == daytime[1] && low[1] == high[1] &&
                                      fog[1] == hazard[1] &&
                                      std::abs(low[1] - position[1] - 34 * layout.form_scale()) <
                                              .001f &&
                                      std::abs(fog[1] - low[1] - 34 * layout.form_scale()) < .001f,
                              "50percent lights form the exact three requested paired rows");
                        const auto before = hazard_events.load();
                        const ImVec2 point(hazard[0] + 20, hazard[1] + 13);

                        for (bool down : {false, true, false}) {
                            io.AddMousePosEvent(point.x, point.y);
                            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
                            ImGui::NewFrame();
                            window.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308},
                                        scale);
                            ImGui::Render();
                        }

                        wait(
                                [&] {
                                    return hazard_events.load() == before + 1;
                                },
                                "hazard click must send exactly one atomic left/right patch");
                    }

                    ++matrix_cases;

                    if (section == 0) {
                        check(std::abs(last_bottom - (layout.panel.bottom() - layout.edge_margin() -
                                                      layout.radio_height() / 2 +
                                                      layout.input_height() / 2)) < .001f,
                              "the bottom movement row is centred on the gear radio 40 pixels above the bottom");
                    }

                    if (section == 3) {
                        check(checkboxes == 0 && numeric == 8,
                              "tyres contain only four pressure/temperature pairs");
                        check(last_bottom <= layout.body.bottom(),
                              "tyre groups fit the available body at every scale");
                        const std::array<const char *, 4> suffix = {"LF", "RF", "LR", "RR"};

                        for (std::size_t i = 0; i < suffix.size(); ++i) {
                            for (const bool temperature : {false, true}) {
                                const auto name = std::string("bcm_tyre") + suffix[i] +
                                                  (temperature ? "Temp" : "Pressure");
                                bool found = false;

                                for (const auto &field : fields) {
                                    if (field.at("name") == name) {
                                        const auto r =
                                                field.at("rect_points").get<std::array<float, 4>>();
                                        const auto column = layout.tyre_column(i % 2);
                                        const std::size_t row = i / 2;
                                        const float tyre_width = layout.movement_value_width();
                                        check(std::abs(r[0] - column.x -
                                                       (temperature
                                                                ? tyre_width +
                                                                          8 * layout.form_scale()
                                                                : 0)) < .001f,
                                              "actual tyre fields share the semantic two-column grid at every scale and locale");
                                        check(std::abs(r[1] - layout.body.y -
                                                       layout.first_heading_offset() -
                                                       static_cast<float>(row) *
                                                               layout.tyre_group_pitch(
                                                                       heading_size) -
                                                       heading_size - 10 * layout.form_scale()) <
                                                      .001f,
                                              "actual tyre rows stay compact below their measured headings");
                                        check(std::abs(r[2] - tyre_width) < .001f &&
                                                      std::abs(r[3] - layout.input_height()) <
                                                              .0001f,
                                              "tyre inputs retain their measured column width and OEM height");
                                        check(r[0] >= column.x && r[0] + r[2] <= column.right() &&
                                                      r[1] + r[3] <= layout.body.bottom(),
                                              "actual tyre fields stay inside their own column and body bounds");
                                        found = true;
                                    }
                                }

                                check(found, "all eight actual tyre fields must be inspected");
                            }
                        }

                        check(layout.tyre_group_pitch(heading_size) >
                                      layout.input_height() + heading_size,
                              "tyre groups keep a visible separation after their fields");

                        for (const auto &text : texts) {
                            if (text.at("name") == "tyre_LR" || text.at("name") == "tyre_RR") {
                                const auto allocated =
                                        text.at("allocated_points").get<std::array<float, 4>>();
                                check(std::abs(allocated[1] - layout.body.y -
                                               layout.first_heading_offset() -
                                               layout.tyre_group_pitch(heading_size)) < .001f &&
                                              std::abs(allocated[1] -
                                                       (layout.body.y +
                                                        layout.first_heading_offset() +
                                                        heading_size + 10 * layout.form_scale() +
                                                        layout.input_height()) -
                                                       (12 * layout.form_scale() +
                                                        layout.group_margin_top() -
                                                        layout.form_scale() / (1920.f / 1336.f))) <
                                                      .001f,
                                      "actual lower tyre titles include the requested extra top group margin");
                            }
                        }
                    }

                    if (section == 4 && light) {
                        bool table_found = false;

                        for (const auto *child : ImGui::GetCurrentContext()->Windows) {
                            if (child->Active &&
                                std::string(child->Name).find("/signals") != std::string::npos) {
                                table_found = true;

                                for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                                    for (const auto &command : list->CmdBuffer) {
                                        for (unsigned index = 0; index + 2 < command.ElemCount;
                                             index += 3) {
                                            const auto &a =
                                                    list->VtxBuffer
                                                            [command.VtxOffset +
                                                             list->IdxBuffer[command.IdxOffset +
                                                                             index]];
                                            const auto &b =
                                                    list->VtxBuffer
                                                            [command.VtxOffset +
                                                             list->IdxBuffer[command.IdxOffset +
                                                                             index + 1]];
                                            const auto &c =
                                                    list->VtxBuffer
                                                            [command.VtxOffset +
                                                             list->IdxBuffer[command.IdxOffset +
                                                                             index + 2]];
                                            const float
                                                    width = std::max({a.pos.x, b.pos.x, c.pos.x}) -
                                                            std::min({a.pos.x, b.pos.x, c.pos.x}),
                                                    height = std::max({a.pos.y, b.pos.y, c.pos.y}) -
                                                             std::min({a.pos.y, b.pos.y, c.pos.y});
                                            check(!(a.col == IM_COL32(255, 255, 255, 255) &&
                                                    b.col == a.col && c.col == a.col &&
                                                    width > 2 * layout.raw_input_width() &&
                                                    height > layout.input_height() &&
                                                    std::min({a.pos.x, b.pos.x, c.pos.x}) >=
                                                            layout.body.x - 1 &&
                                                    std::max({a.pos.x, b.pos.x, c.pos.x}) <=
                                                            layout.body.right() + 1 &&
                                                    std::min({a.pos.y, b.pos.y, c.pos.y}) >=
                                                            layout.body.y + layout.input_height() -
                                                                    1),
                                                  "raw table cannot cover the OEM scene with a solid white child background");
                                        }
                                    }
                                }
                            }
                        }

                        check(table_found, "actual scrolling raw table child must be inspected");
                    }

                    if (section == 4) {
                        bool search_fill = false;

                        for (const auto &item : fields) {
                            if (item.at("name") == "raw_search") {
                                const auto r = item.at("rect_points").get<std::array<float, 4>>();

                                for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                                    for (const auto &vertex : list->VtxBuffer) {
                                        if (vertex.pos.x >= r[0] && vertex.pos.x <= r[0] + r[2] &&
                                            vertex.pos.y >= r[1] && vertex.pos.y <= r[1] + r[3]) {
                                            search_fill |= vertex.col == input_color;
                                        }
                                    }
                                }
                            }
                        }

                        check(search_fill, "raw search uses measured OEM capsule fill");
                        check(numeric > 0 && first_raw - layout.body.y - layout.input_height() -
                                                             8 * layout.form_scale() <=
                                                     8 * layout.form_scale(),
                              "raw signal rows start below the separate search without a header");
                        const auto clip =
                                fields.back().at("clip_points").get<std::array<float, 4>>();
                        check(std::abs(clip[1] + clip[3] - layout.body.bottom()) <= 2,
                              "raw signal scrolling uses the full available height");
                    }

                    if (section == 4 && layout.compact50()) {
                        float previous = -1;

                        for (const auto &field : fields) {
                            if (field.at("kind") == "numeric") {
                                const auto r = field.at("rect_points").get<std::array<float, 4>>();

                                if (previous >= 0) {
                                    check(std::abs((r[1] - previous) * density -
                                                   (38 * density - 2)) < .001f,
                                          "raw fields use a uniform actual row pitch two physical pixels below the previous steady spacing");
                                }

                                previous = r[1];
                            }
                        }
                    }

                    if (section == 2) {
                        check(checkboxes == 6, "body contains four doors, hood and tailgate");
                        bool tailgate = false;
                        std::array<float, 4> checked_rect{};

                        for (const auto &item : fields) {
                            if (item.at("name") == "tail_gate") {
                                tailgate = true;
                            }

                            if (item.at("name") == "door_fl") {
                                checked_rect = item.at("rect_points").get<std::array<float, 4>>();
                            }
                        }

                        check(tailgate, "body exposes the actual OEM tail_gate input");
                        check(std::abs(checked_rect[2] - layout.checkbox_column(0).width) < .001f &&
                                      std::abs(checked_rect[3] - 26) < .001f,
                              "door toggle uses its measured column width and fixed hit height");
                        bool track = false, knob = false, blue_fill = false;
                        const auto ink = toggle_ink(light);

                        each_vertex([&](const ImDrawVert &vertex, const ImDrawCmd &) {
                            if (vertex.pos.x >= checked_rect[0] &&
                                vertex.pos.x <= checked_rect[0] + 41.5f &&
                                vertex.pos.y >= checked_rect[1] &&
                                vertex.pos.y <= checked_rect[1] + 26) {
                                track |= vertex.col == ink.track;
                                knob |= same_rgb(vertex.col, IM_COL32(255, 255, 255, 255)) &&
                                        (vertex.col >> IM_COL32_A_SHIFT) != 0;
                                blue_fill |= vertex.col == ink.on;
                            }
                        });

                        check(track && knob && blue_fill,
                              "own toggle draws its track and knob with the blue on fill");
                        const auto toggle_vertices = [&] {
                            std::vector<std::array<float, 5>> result;

                            for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                                for (const auto &vertex : list->VtxBuffer) {
                                    if (vertex.pos.x >= checked_rect[0] &&
                                        vertex.pos.x <= checked_rect[0] + 41.5f &&
                                        vertex.pos.y >= checked_rect[1] &&
                                        vertex.pos.y <= checked_rect[1] + 26) {
                                        result.push_back({vertex.pos.x, vertex.pos.y, vertex.uv.x,
                                                          vertex.uv.y, float(vertex.col)});
                                    }
                                }
                            }

                            return result;
                        };
                        const auto normal = toggle_vertices();
                        io.AddMousePosEvent(checked_rect[0] + 20, checked_rect[1] + 13);
                        ImGui::NewFrame();
                        window.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308}, scale);
                        ImGui::Render();
                        check(toggle_vertices() == normal,
                              "hover must not alter toggle track, mask or knob vertices/colors");
                    }

                    if (section == 0) {
                        const auto item = [&](const char *name) {
                            for (const auto &field : window.inspection().at("fields")) {
                                if (field.at("name") == name) {
                                    return field.at("rect_points").get<std::array<float, 4>>();
                                }
                            }

                            throw std::runtime_error("missing rendered gear item");
                        };
                        const auto render = [&](ImVec2 mouse, bool down) {
                            io.AddMousePosEvent(mouse.x, mouse.y);
                            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
                            ImGui::NewFrame();
                            window.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308},
                                        scale);
                            ImGui::Render();
                        };

                        for (const bool multimedia : {false, true}) {
                            for (const bool restart : {false, true}) {
                                const auto column = wheel_column(scale, {}, {}, 130, 0);
                                const auto bounds = multimedia ? (restart ? column.media_restart
                                                                          : column.media_power)
                                                               : (restart ? column.cluster_restart
                                                                          : column.cluster_power);
                                // A running guest draws its power glyph in the primary ink, an
                                // enabled restart glyph rests in neutral grey; the pointer turns
                                // both into the primary ink.
                                const ImU32 hover = light ? IM_COL32(0, 0, 0, 255)
                                                          : IM_COL32(255, 255, 255, 255),
                                            idle = !restart ? hover
                                                   : light  ? IM_COL32(128, 128, 128, 255)
                                                            : IM_COL32(202, 202, 202, 255);
                                const char *name =
                                        multimedia
                                                ? (restart ? "media_restart" : "media_power")
                                                : (restart ? "cluster_restart" : "cluster_power");
                                const auto control = [&]() -> const Json & {
                                    for (const auto &field : window.inspection().at("fields")) {
                                        if (field.at("name") == name) {
                                            return field;
                                        }
                                    }

                                    throw std::runtime_error("missing lifecycle interaction item");
                                };
                                // Font-atlas geometry only: wheel photo corners may fall inside
                                // the box.
                                const auto background = [&](ImU32 color) {
                                    for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                                        for (const auto &command : list->CmdBuffer) {
                                            if (command.TexRef._TexData != io.Fonts->TexData) {
                                                continue;
                                            }

                                            for (unsigned i = 0; i < command.ElemCount; ++i) {
                                                const auto &vertex =
                                                        list->VtxBuffer
                                                                [command.VtxOffset +
                                                                 list->IdxBuffer[command.IdxOffset +
                                                                                 i]];

                                                if (vertex.pos.x >= bounds.x &&
                                                    vertex.pos.x <= bounds.right() &&
                                                    vertex.pos.y >= bounds.y &&
                                                    vertex.pos.y <= bounds.bottom() &&
                                                    vertex.col == color) {
                                                    return true;
                                                }
                                            }
                                        }
                                    }

                                    return false;
                                };
                                const ImVec2 center(bounds.x + bounds.width / 2,
                                                    bounds.y + bounds.height / 2),
                                        corner(bounds.x + 1, bounds.y + 1);
                                render(corner, false);
                                check(control().at("hit_shape") == "circle" &&
                                              control().at("pointer_hovered") == false &&
                                              background(idle) && !background(surface_color) &&
                                              !background(border_color),
                                      "lifecycle bounding-square corners do not hover the actual circle at any scale");
                                render(corner, true);
                                check(control().at("pointer_held") == false &&
                                              control().at("pressed") == false,
                                      "pressing a lifecycle bounding-square corner never captures the circle");
                                render(corner, false);
                                check(control().at("pressed") == false,
                                      "releasing a corner click never activates lifecycle");
                                render(center, false);
                                check(control().at("pointer_hovered") == true && background(hover),
                                      "the actual lifecycle circle center draws the hover color");
                                render(center, true);
                                check(control().at("pointer_held") == true && background(hover),
                                      "pressing the actual lifecycle circle keeps the hover color");
                                render(corner, false);
                                check(control().at("pressed") == false,
                                      "dragging out of the lifecycle circle cancels release activation");
                                auto &counter = multimedia
                                                        ? (restart ? media_restart_events
                                                                   : media_power_events)
                                                        : (restart ? restart_events : power_events);
                                const auto before = counter.load();
                                render(center, false);
                                render(center, true);
                                render(center, false);
                                check(control().at("pressed") == true,
                                      "a center click activates the actual lifecycle circle");
                                wait(
                                        [&] {
                                            return counter.load() == before + 1;
                                        },
                                        "a real lifecycle circle click must dispatch exactly one accepted operation");
                            }
                        }

                        const auto speed_rect = item("speed");
                        bool capsule_fill = false;

                        for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                            for (const auto &vertex : list->VtxBuffer) {
                                if (vertex.pos.x >= speed_rect[0] &&
                                    vertex.pos.x <= speed_rect[0] + speed_rect[2] &&
                                    vertex.pos.y >= speed_rect[1] &&
                                    vertex.pos.y <= speed_rect[1] + speed_rect[3]) {
                                    capsule_fill |= vertex.col == input_color;
                                }
                            }
                        }

                        check(capsule_fill,
                              "numeric input uses measured OEM AP Hotspot #373f4a fill");
                        const auto odometer = [&]() -> const Json & {
                            for (const auto &field : window.inspection().at("fields")) {
                                if (field.at("name") == "odo") {
                                    return field;
                                }
                            }

                            throw std::runtime_error("missing rendered odometer");
                        };
                        const bool clear_overflow =
                                odometer().at("canonical_text_width_points").get<float>() >
                                odometer().at("value_text_area_points").get<float>() + 2;
                        const auto odo_input =
                                odometer().at("input_rect_points").get<std::array<float, 4>>();
                        const ImVec2 odo_point(odo_input[0] + odo_input[2] / 2,
                                               odo_input[1] + odo_input[3] / 2);
                        render(odo_point, false);
                        render(odo_point, true);
                        render(odo_point, false);

                        if (ImGui::GetCurrentContext()->ActiveId !=
                            odometer().at("input_id").get<ImGuiID>()) {
                            std::cerr << "odometer focus diagnostic active="
                                      << ImGui::GetCurrentContext()->ActiveId
                                      << " input=" << odometer().at("input_id")
                                      << " mouse=" << io.MousePos.x << ',' << io.MousePos.y
                                      << " hovered="
                                      << (ImGui::GetCurrentContext()->HoveredWindow
                                                  ? ImGui::GetCurrentContext()->HoveredWindow->Name
                                                  : "none")
                                      << '\n';
                        }

                        check(ImGui::GetCurrentContext()->ActiveId ==
                                      odometer().at("input_id").get<ImGuiID>(),
                              "mouse click must activate the actual odometer InputText before keyboard scrolling");
                        io.AddKeyEvent(ImGuiKey_End, true);
                        render(odo_point, false);
                        io.AddKeyEvent(ImGuiKey_End, false);
                        render(odo_point, false);
                        auto *odo_state =
                                ImGui::GetInputTextState(odometer().at("input_id").get<ImGuiID>());
                        check(odo_state && odo_state->GetCursorPos() == 8 &&
                                      odometer().at("font_size_points") ==
                                              window.inspection().at("font").at(
                                                      "input_size_points"),
                              "actual native InputText reaches the eighth digit at the unchanged readable font size");

                        if (clear_overflow) {
                            ++scroll_cases;

                            if (odometer().at("scroll_x_points").get<float>() <= 0) {
                                std::cerr << "odometer scroll diagnostic active="
                                          << ImGui::GetCurrentContext()->ActiveId
                                          << " input=" << odometer().at("input_id")
                                          << " cursor=" << odo_state->GetCursorPos()
                                          << " field=" << odometer() << '\n';
                            }

                            check(odometer().at("scroll_x_points").get<float>() > 0,
                                  "actual native InputText must scroll when eight digits exceed its real drawable viewport");
                        }

                        const auto end_ink =
                                odometer().at("input_ink_rect_points").get<std::array<float, 4>>();
                        check(end_ink[0] >= odo_input[0] - .001f &&
                                      end_ink[0] + end_ink[2] <=
                                              odo_input[0] + odo_input[2] + .001f,
                              "all visible odometer glyphs remain before the separate unit after End, whether text fits or scrolls");
                        io.AddKeyEvent(ImGuiKey_Home, true);
                        render(odo_point, false);
                        io.AddKeyEvent(ImGuiKey_Home, false);
                        render(odo_point, false);
                        check(odometer().at("scroll_x_points").get<float>() == 0,
                              "actual native InputText scrolls back to the first digit");
                        const ImVec2 speed_point(speed_rect[0] + 8,
                                                 speed_rect[1] + speed_rect[3] / 2);
                        render(speed_point, true);
                        render(speed_point, false);
                        const auto odo_group = item("odo");
                        const ImVec2 unit_point(odo_group[0] + odo_group[2] - 5,
                                                odo_group[1] + odo_group[3] / 2);
                        render(unit_point, true);
                        render(unit_point, false);
                        render(unit_point, false);
                        check(ImGui::GetCurrentContext()->ActiveId ==
                                      odometer().at("input_id").get<ImGuiID>(),
                              "clicking the unit suffix focuses its actual InputText component");

                        if (language == ui::Language::russian) {
                            std::cout << "odometer8digits+km scale=" << scale
                                      << " font=" << odometer().at("font_size_points")
                                      << " value_width="
                                      << odometer().at("canonical_text_width_points")
                                      << " viewport=" << odometer().at("value_text_area_points")
                                      << " unit_width=" << odometer().at("unit_width_points")
                                      << " control_width=" << odo_group[2] << '\n';
                        }

                        if (layout.compact50()) {
                            render(speed_point, true);
                            render(speed_point, false);
                            io.AddKeyEvent(ImGuiKey_End, true);
                            render(speed_point, false);
                            io.AddKeyEvent(ImGuiKey_End, false);
                            render(speed_point, false);
                            ImGuiID speed_id{};

                            for (const auto &field : window.inspection().at("fields")) {
                                if (field.at("name") == "speed") {
                                    speed_id = field.at("input_id").get<ImGuiID>();
                                }
                            }

                            const auto speed_text = [&] {
                                auto *state = ImGui::GetInputTextState(speed_id);
                                check(state,
                                      "digit guard must exercise the actual active InputText state");

                                return std::string(state->GetText());
                            };
                            io.AddInputCharactersUTF8("9");
                            render(speed_point, false);
                            check(speed_text() == "255",
                                  "typing a fourth digit leaves the existing three-digit speed unchanged");
                            const auto modifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super
                                                                           : ImGuiMod_Ctrl;
                            io.AddKeyEvent(modifier, true);
                            render(speed_point, false);
                            io.AddKeyEvent(ImGuiKey_A, true);
                            render(speed_point, false);
                            io.AddKeyEvent(ImGuiKey_A, false);
                            render(speed_point, false);
                            io.AddKeyEvent(modifier, false);
                            render(speed_point, false);
                            io.AddInputCharactersUTF8("1234");
                            render(speed_point, false);
                            check(speed_text() == "123",
                                  "replacement/paste accepts only the first three digits");
                            io.AddKeyEvent(ImGuiKey_Home, true);
                            render(speed_point, false);
                            io.AddKeyEvent(ImGuiKey_Home, false);
                            render(speed_point, false);
                            io.AddKeyEvent(ImGuiKey_RightArrow, true);
                            render(speed_point, false);
                            io.AddKeyEvent(ImGuiKey_RightArrow, false);
                            render(speed_point, false);
                            io.AddInputCharactersUTF8("9");
                            render(speed_point, false);
                            check(speed_text() == "123",
                                  "inserting a fourth digit in the middle preserves the original suffix");
                        }

                        if (layout.compact50()) {
                            io.AddKeyEvent(ImGuiKey_Tab, true);
                            render(odo_point, false);
                            io.AddKeyEvent(ImGuiKey_Tab, false);
                            render(odo_point, false);
                            const auto &context = *ImGui::GetCurrentContext();
                            check(context.NavCursorVisible,
                                  "actual Tab navigation exposes a keyboard focus cursor");
                            const Json *focused = nullptr;

                            for (const auto &field : window.inspection().at("fields")) {
                                if (field.at("kind") == "numeric" &&
                                    field.at("input_id").get<ImGuiID>() == context.NavId) {
                                    focused = &field;
                                }
                            }

                            check(focused, "Tab navigation focuses a real numeric input");
                            const auto focus =
                                    focused->at("rect_points").get<std::array<float, 4>>();
                            float left = FLT_MAX, right = -FLT_MAX, top = FLT_MAX,
                                  bottom = -FLT_MAX;

                            for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                                for (const auto &vertex : list->VtxBuffer) {
                                    if (vertex.col == active_color &&
                                        vertex.pos.y >= focus[1] - 2 &&
                                        vertex.pos.y <= focus[1] + focus[3] + 2) {
                                        left = std::min(left, vertex.pos.x);
                                        right = std::max(right, vertex.pos.x);
                                        top = std::min(top, vertex.pos.y);
                                        bottom = std::max(bottom, vertex.pos.y);
                                    }
                                }
                            }

                            // The toolkit draws a two-point stroke with a half-point antialias
                            // fringe.
                            check(std::abs(left - (focus[0] - 1.5f)) < .01f &&
                                          std::abs(right - (focus[0] + focus[2] + 1.5f)) < .01f &&
                                          std::abs(top - (focus[1] - 1.5f)) < .01f &&
                                          std::abs(bottom - (focus[1] + focus[3] + 1.5f)) < .01f,
                                  "keyboard focus uses the tab blue and outlines the complete visible capsule including its unit");
                        }

                        const auto first = item("gear_P");
                        const auto choice = item("gear_D");
                        check(std::abs(choice[1] - layout.movement_row(0, 4).y) < .001f,
                              "gear selector stays vertically centered in the left column bottom row");
                        const auto wheel = window.inspection().at("wheel_column");
                        const auto scale_choice =
                                           wheel.at("scale_choice").get<std::array<float, 4>>(),
                                   language_choice =
                                           wheel.at("language_choice").get<std::array<float, 4>>();
                        const float gear_width = 4 * scale_choice[2] / 3;
                        check(std::abs(first[3] - scale_choice[3]) < .001f &&
                                      std::abs(first[3] - language_choice[3]) < .001f &&
                                      std::abs(first[2] - scale_choice[2] / 3) < .001f &&
                                      std::abs(first[2] - language_choice[2] / 3) < .001f,
                              "the gear, scale and language radios render with one height and one segment width");
                        const float animation_padding =
                                MusicTabIndicator::animation_padding(gear_width * .75f);
                        check(first[0] == layout.content_column(2, 0).x &&
                                      std::abs(choice[0] + choice[2] - first[0] - gear_width) <
                                              .001f,
                              "compact gear track aligns with the left numeric fields");
                        check(first[0] - animation_padding >= layout.panel.x,
                              "the panel inset reserves left overshoot without shifting the track");
                        MusicTabIndicator edge;
                        edge.snap(0, 0);

                        for (int transition = 0; transition < 20; ++transition) {
                            const double start = transition * .31;
                            edge.select(transition % 2 ? 0 : gear_width * .75, start);

                            for (int sample = 0; sample <= 400; ++sample) {
                                const double x = edge.position(start + sample * .001);
                                check(x >= -animation_padding &&
                                              x + first[2] <= gear_width + animation_padding,
                                      "both extreme gear animations and interrupted reversals fit the reserved envelope");
                            }
                        }

                        check(std::abs(first[2] - gear_width / 4) < .001f &&
                                      std::abs(first[3] - layout.radio_height()) < .001f,
                              "gear choices retain the configured OEM radio height");

                        if (layout.compact50()) {
                            const auto &radio = window.inspection().at("radio_reference");
                            check(std::abs(first[3] * density - 40) < .001f &&
                                          std::abs(radio.at("font_size_points").get<float>() *
                                                           density -
                                                   16) < .001f &&
                                          radio.at("normal_border_points") == 0 &&
                                          radio.at("animation_seconds") == .4,
                                  "50percent gear is half the 100 percent radio: 40 pixels high with 16 pixel text and 400ms overshoot");
                        }

                        check(first[1] == choice[1] && first[2] == choice[2] &&
                                      choice[0] > first[0] + first[2],
                              "gear choices have equal bounds in a horizontal row");
                        const ImVec2 target(choice[0] + choice[2] / 2, choice[1] + choice[3] / 2);
                        gear_selected = false;
                        render(target, true);
                        render(target, false);
                        wait(
                                [&] {
                                    return gear_selected.load();
                                },
                                "clicking the gear button must submit the OEM gear and validity together");
                    }

                    window.close();
                }
            }
        }

        check(matrix_cases == (fifty_only ? 15 : 45),
              "all requested panel/scale/locale cases use the actual production font and ImGui draw path");
        check(scroll_cases > 0,
              "production font matrix must exercise real native scrolling with a valid eight-digit odometer fixture");
        std::cout << "UI geometry matrix" << matrix_cases << "cases; production CJK glyph coverage="
                  << (chinese_covered ? "yes" : "no (geometry only)")
                  << "; rendered pixel acceptance requires separate capture\n";
        ImGui::DestroyContext();
    } catch (...) {
        ImGui::DestroyContext();
        throw;
    }
}

void radio_transition_colors(const std::filesystem::path &assets,
                             unsigned scale = canonical_ui_scale) {
    ImGui::CreateContext();
    try {
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DeltaTime = 1.f / 60;
        const auto geometry = view_layout(scale);
        io.DisplaySize = ImVec2(geometry.canvas_width, geometry.canvas_height);
        const float density = scale == canonical_ui_scale ? 1920.f / canonical_ui_width : 2.f;
        io.DisplayFramebufferScale = ImVec2(density, density);
        load_ui_fonts(fonts_directory);
        std::mutex mutex;
        std::string scheme = "dark";
        auto canonical = apply_patch(initial_state(VehicleCatalog::qinggan09),
                                     {{"gear", 0}, {"gear_valid", 1}});
        auto transport = [&](const Json &operation) {
            std::lock_guard lock(mutex);

            if (operation.at("op") == "patch") {
                canonical = apply_patch(canonical, operation.at("values"));
            }

            return Json{{"ok", true},
                        {"session_id", "radio-animation-test"},
                        {"revision", canonical.at("sequence")},
                        {"state", canonical},
                        {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                        {"deliveries",
                         {{"qnx", {{"status", "graphics_running"}}},
                          {"android",
                           {{"appearance", {{"status", "observed"}, {"scheme", scheme}}}}}}}};
        };
        {
            Options options;
            options.assets = assets;
            options.endpoint = "radio-animation-fixture";
            options.capture_ui = "radio-animation-inspection";
            options.smoke_seconds = .25;
            Window window(options, transport);
            wait(
                    [&] {
                        return window.client().state().ready;
                    },
                    "radio animation fixture must receive canonical state");
            const auto frame = [&] {
                io.AddMousePosEvent(20, 20);
                ImGui::NewFrame();
                window.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308}, scale);
                ImGui::Render();
            };
            const auto white = [&](const char *name) {
                std::array<float, 4> r{};

                for (const auto &field : window.inspection().at("fields")) {
                    if (field.at("name") == name) {
                        r = field.at("rect_points").get<std::array<float, 4>>();
                    }
                }

                bool found_white = false, found_gray = false;

                for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                    for (const auto &command : list->CmdBuffer) {
                        if (command.TexRef._TexData) {
                            for (unsigned i = 0; i < command.ElemCount; ++i) {
                                const auto &vertex =
                                        list->VtxBuffer[list->IdxBuffer[command.IdxOffset + i] +
                                                        command.VtxOffset];

                                if (vertex.pos.x >= r[0] && vertex.pos.x <= r[0] + r[2] &&
                                    vertex.pos.y >= r[1] && vertex.pos.y <= r[1] + r[3]) {
                                    found_white |= vertex.col == IM_COL32(255, 255, 255, 255);
                                    found_gray |= vertex.col == IM_COL32(202, 202, 202, 255);
                                }
                            }
                        }
                    }
                }

                check(found_white != found_gray,
                      "actual radio glyph vertices have exactly one OEM selected/unselected color");

                return found_white;
            };
            // The selected capsule vertices of the gear radio, right of the wheel column choices.
            const auto gear_selection = [&](bool light, float parameters_x, auto visit) {
                std::array<float, 4> r{};

                for (const auto &field : window.inspection().at("fields")) {
                    if (field.at("name") == "gear_P") {
                        r = field.at("rect_points").get<std::array<float, 4>>();
                    }
                }

                each_vertex([&](const ImDrawVert &vertex, const ImDrawCmd &command) {
                    if (vertex.pos.x >= parameters_x && vertex.pos.y >= r[1] - 1 &&
                        vertex.pos.y <= r[1] + r[3] + 1 &&
                        selection_vertex(vertex, light, r[1], r[3])) {
                        visit(vertex, command);
                    }
                });
            };
            frame();
            frame();
            check(white("gear_P") && !white("gear_D"),
                  "settled radio marks only the initial choice white");
            check(window.client().patch({{"gear", 3}, {"gear_valid", 1}}),
                  "radio transition fixture accepts the target gear");
            wait(
                    [&] {
                        return window.client().state().raw.at("gear") == 3;
                    },
                    "radio target gear reaches the render state");
            frame();
            check(!white("gear_P") && !white("gear_D"),
                  "target radio text stays gray until the moving selection overlaps it");

            for (int i = 0; i < 30; ++i) {
                frame();
            }

            check(!white("gear_P") && white("gear_D"),
                  "settled radio marks only the destination white after the OEM transition");
            check(window.client().patch({{"gear", 0}, {"gear_valid", 1}}),
                  "reverse edge transition accepted");
            wait(
                    [&] {
                        return window.client().state().raw.at("gear") == 0;
                    },
                    "reverse target reaches renderer");
            frame();

            for (int sample = 0; sample < 26; ++sample) {
                frame();

                bool drawn = false;

                gear_selection(
                        false, geometry.parameters.x,
                        [&](const ImDrawVert &vertex, const ImDrawCmd &command) {
                            const auto pos = vertex.pos;
                            drawn = true;

                            // The transparent anti-aliased fringe may leave the clip.
                            if ((vertex.col >> IM_COL32_A_SHIFT) == 0) {
                                return;
                            }

                            if (pos.x < command.ClipRect.x - .001f ||
                                pos.x > command.ClipRect.z + .001f ||
                                pos.y < command.ClipRect.y - .001f ||
                                pos.y > command.ClipRect.w + .001f) {
                                std::cerr << "radio clipped vertex " << pos.x << "," << pos.y
                                          << " clip " << command.ClipRect.x << ","
                                          << command.ClipRect.y << "," << command.ClipRect.z << ","
                                          << command.ClipRect.w << "\n";
                            }

                            check(pos.x >= command.ClipRect.x - .001f &&
                                          pos.x <= command.ClipRect.z + .001f &&
                                          pos.y >= command.ClipRect.y - .001f &&
                                          pos.y <= command.ClipRect.w + .001f,
                                  "actual animated radio pixels must fit the draw-command clip at the left extreme");
                        });
                check(drawn, "the animated gear radio draws its own selected capsule");
            }

            {
                std::lock_guard lock(mutex);
                scheme = "light";
            }
            wait(
                    [&] {
                        window.refresh_state();

                        return window.light_theme();
                    },
                    "light theme reaches the live renderer");
            frame();
            frame();
            ImVec2 a(FLT_MAX, FLT_MAX), b(-FLT_MAX, -FLT_MAX);
            std::set<ImU32> stops;

            each_vertex([&](const ImDrawVert &vertex, const ImDrawCmd &) {
                for (const auto &stop : light_background) {
                    if (vertex.col == stop.color && vertex.pos.x >= geometry.frame.right()) {
                        a = ImMin(a, vertex.pos);
                        b = ImMax(b, vertex.pos);
                        stops.insert(stop.color);
                    }
                }
            });

            check(stops.size() == light_background.size() && a.x == geometry.frame.right() &&
                          a.y == 0 && b.x == geometry.top_width && b.y == geometry.top_height,
                  "one light background gradient spans wheel and controls without a seam");
            bool light_radio = false, dark_radio = false;
            gear_selection(true, geometry.parameters.x, [&](const ImDrawVert &, const ImDrawCmd &) {
                light_radio = true;
            });
            gear_selection(false, geometry.parameters.x,
                           [&](const ImDrawVert &, const ImDrawCmd &) {
                               dark_radio = true;
                           });
            check(light_radio && !dark_radio,
                  "light theme draws the flat light capsule, never the dark gradient");
            const auto off_icon = wheel_column(scale, {}, {}, 130, 0).media_power;
            bool original_disabled_ink = false;

            for (const auto *list : ImGui::GetDrawData()->CmdLists) {
                for (const auto &vertex : list->VtxBuffer) {
                    if (vertex.pos.x >= off_icon.x && vertex.pos.x <= off_icon.right() &&
                        vertex.pos.y >= off_icon.y && vertex.pos.y <= off_icon.bottom()) {
                        original_disabled_ink |= vertex.col == IM_COL32(0, 0, 0, 77);
                    }
                }
            }

            check(original_disabled_ink,
                  "light disabled lifecycle icon uses the black primary ink at the original Music 77/255 opacity");

            for (const auto *child : ImGui::GetCurrentContext()->Windows) {
                if (child->Active &&
                    std::string(child->Name).find("/parameters") != std::string::npos) {
                    check(child->Flags & ImGuiWindowFlags_NoBackground,
                          "parameter child cannot cover the light background with a flat fill");
                }
            }

            check(window.inspection().at("appearance").at("tab_inactive_color") ==
                          IM_COL32(80, 89, 102, 255),
                  "unselected Music tab uses the OEM text color");
            check(window.inspection().at("appearance").at("divider_color") ==
                          IM_COL32(151, 151, 151, 80),
                  "divider uses the OEM light alpha/color");

            for (const unsigned target : {3u, 0u}) {
                check(window.client().patch({{"gear", target}, {"gear_valid", 1}}),
                      "light edge transition accepted");
                wait(
                        [&] {
                            return window.client().state().raw.at("gear") == target;
                        },
                        "light target reaches renderer");

                for (unsigned sample = 0; sample < 30; ++sample) {
                    frame();

                    gear_selection(
                            true, geometry.parameters.x,
                            [&](const ImDrawVert &vertex, const ImDrawCmd &command) {
                                const auto pos = vertex.pos;

                                // The transparent anti-aliased fringe may leave the clip.
                                if ((vertex.col >> IM_COL32_A_SHIFT) == 0) {
                                    return;
                                }

                                check(pos.x >= command.ClipRect.x - .001f &&
                                              pos.x <= command.ClipRect.z + .001f &&
                                              pos.y >= command.ClipRect.y - .001f &&
                                              pos.y <= command.ClipRect.w + .001f,
                                      "both light radio edge transitions keep the complete capsule inside the actual clip");
                            });
                }
            }

            check(window.client().patch({{"gear", 3}, {"gear_valid", 1}}),
                  "zoom fixture selects D");
            wait(
                    [&] {
                        return window.client().state().raw.at("gear") == 3;
                    },
                    "zoom fixture receives D");

            for (unsigned sample = 0; sample < 30; ++sample) {
                frame();
            }

            for (const unsigned zoom : {75u, 100u, canonical_ui_scale}) {
                const auto zoom_view = view_layout(zoom);
                io.DisplaySize = ImVec2(zoom_view.canvas_width, zoom_view.canvas_height);
                const float zoom_density = zoom == canonical_ui_scale ? 1920.f / canonical_ui_width
                                                                      : 2.f;
                io.DisplayFramebufferScale = ImVec2(zoom_density, zoom_density);
                ImGui::NewFrame();
                window.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308}, zoom);
                ImGui::Render();
                std::array<float, 4> expected{};
                float left = FLT_MAX, right = -FLT_MAX;

                for (const auto &field : window.inspection().at("fields")) {
                    if (field.at("name") == "gear_D") {
                        expected = field.at("rect_points").get<std::array<float, 4>>();
                    }
                }

                // The wheel column choices share the selected capsule colours.
                gear_selection(true, zoom_view.parameters.x,
                               [&](const ImDrawVert &vertex, const ImDrawCmd &) {
                                   left = std::min(left, vertex.pos.x);
                                   right = std::max(right, vertex.pos.x);
                               });

                // The anti-aliased fringe widens the capsule equally on both sides.
                check(std::abs((left + right) / 2 - expected[0] - expected[2] / 2) < .001f,
                      "changing zoom snaps the radio to its selected cell in the very first frame");
            }

            window.close();
        }
        ImGui::DestroyContext();
    } catch (...) {
        ImGui::DestroyContext();
        throw;
    }
}

void asynchronous_client() {
    {
        Client idle({}, "idle-owner");
        ViewState snapshot;
        std::uint64_t version = 0;
        check(idle.copy_state(snapshot, version) && version != 0,
              "initial view snapshot must be copied");
        snapshot.error = "local sentinel";

        for (unsigned i = 0; i < 60; ++i) {
            check(!idle.copy_state(snapshot, version) && snapshot.error == "local sentinel",
                  "unchanged render polls must preserve the existing snapshot without a copy");
        }
    }
    std::mutex mutex;
    Json canonical = initial_state(VehicleCatalog::qinggan09);
    std::vector<Json> operations;
    Json qnx = {{"status", "disconnected"}};
    bool fail_next = false;
    auto transport = [&](const Json &operation) {
        std::lock_guard lock(mutex);

        if (!operations.empty()) {
            check(operation.value("session_id", std::string{}) == "test-session",
                  "every operation is bound to the accepted session identity");
        }

        operations.push_back(operation);

        if (fail_next) {
            fail_next = false;
            throw std::runtime_error("transport failure fixture");
        }

        if (operation.at("op") == "patch") {
            canonical = apply_patch(canonical, operation.at("values"));
        }

        if (operation.at("op") == "wheel" || operation.at("op") == "release_owner" ||
            operation.at("op") == "close_cluster") {
            return Json{{"ok", true}, {"accepted", true}};
        }

        return Json{{"ok", true},
                    {"session_id", "test-session"},
                    {"revision", canonical.at("sequence")},
                    {"state", canonical},
                    {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                    {"deliveries", {{"android", {{"status", "pending"}}}, {"qnx", qnx}}},
                    {"wheel_routes", Json::object()}};
    };
    {
        // The window works on the catalogue of the platform of the running machine.
        auto platform = initial_state(VehicleCatalog::qinggan11);
        Client platform_client("test-endpoint", "test-owner", [&](const Json &operation) {
            if (operation.at("op") == "patch") {
                platform = apply_patch(platform, operation.at("values"));
            }

            return Json{{"ok", true},
                        {"session_id", "test-qinggan11"},
                        {"revision", platform.at("sequence")},
                        {"state", platform},
                        {"catalog", catalog_json(VehicleCatalog::qinggan11)},
                        {"wheel_routes", Json::object()}};
        });
        wait(
                [&] {
                    return platform_client.state().ready;
                },
                "qinggan11 canonical snapshot");
        check(platform_client.state().raw.size() == 736 &&
                      !platform_client.state().raw.contains("mrr_vSetDis"),
              "qinggan11 catalogue values");
        check(platform_client.patch({{"ADCU_TargetType_1", 2}}), "qinggan11 field patch enqueued");
        wait(
                [&] {
                    return platform_client.state().raw.at("ADCU_TargetType_1") == 2;
                },
                "qinggan11 patch acknowledgement");
    }
    {
        Client client("test-endpoint", "test-owner", transport);
        wait(
                [&] {
                    return client.state().ready;
                },
                "async initial canonical snapshot");
        check(client.state().raw.size() == 579, "all579canonical values");
        check(!client.patch({{"swm_menu", 1}}), "transient SWM must never be persisted");
        check(!client.patch({{"invented", 1}}), "unknown field cannot be persisted");
        check(client.patch({{"speed", 65}}), "valid patch enqueued");
        wait(
                [&] {
                    return client.state().raw.at("speed") == 65;
                },
                "patch canonical acknowledgement");
        check(client.state().raw.at("speed10x") == 650, "the model couples the written field");
        ViewState snapshot;
        std::uint64_t version = 0;
        check(client.copy_state(snapshot, version) && snapshot.raw.at("speed") == 65,
              "changed canonical data must refresh the render snapshot");
        const auto revision = snapshot.revision;
        {
            std::lock_guard lock(mutex);
            qnx = {{"status", "booting"}, {"generation", 2}};
        }
        wait(
                [&] {
                    client.copy_state(snapshot, version);

                    return snapshot.deliveries.at("qnx").value("generation", 0) == 2;
                },
                "delivery changes at the same canonical revision must reach the render snapshot");
        check(snapshot.revision == revision, "delivery refresh does not invent a vehicle revision");
        {
            std::lock_guard lock(mutex);
            fail_next = true;
        }
        wait(
                [&] {
                    client.copy_state(snapshot, version);

                    return !snapshot.ready && snapshot.error == "transport failure fixture";
                },
                "transport failure must invalidate the render snapshot");
        wait(
                [&] {
                    client.copy_state(snapshot, version);

                    return snapshot.ready && snapshot.error.empty();
                },
                "recovery at the same revision must refresh the render snapshot");
        check(client.wheel("menu", "down") && client.wheel("menu", "up"), "phase events accepted");
        check(!client.wheel("horn", "down"), "unrequested horn rejected");
        check(!client.wheel("menu", "toggle"), "unknown phase rejected");
        check(client.detent("volume_up"), "roller atomic phase pair");
        client.release_owner();
        wait(
                [&] {
                    std::lock_guard lock(mutex);

                    for (const auto &op : operations) {
                        if (op.at("op") == "release_owner") {
                            return true;
                        }
                    }

                    return false;
                },
                "explicit blur release reaches transport");
        std::lock_guard lock(mutex);
        std::vector<std::string> phases;

        for (const auto &op : operations) {
            if (op.at("op") == "wheel") {
                check(op.at("owner") == "test-owner", "owned wheel events");
                phases.push_back(op.at("phase").get<std::string>());
            }
        }

        check(phases == std::vector<std::string>{"down", "up", "down", "up"},
              "down/up queue preserves all phases and order");
    }
    std::lock_guard lock(mutex);

    check(operations.back().at("op") == "release_owner", "client shutdown forces final release");
}
} // namespace

static void lifecycle_client_deadlines() {
    std::atomic<bool> stopped = false, entered = false, completed = false;

#ifdef _WIN32
    const auto endpoint = std::filesystem::path(R"(\\.\pipe\voyemu-ui-test-)" + random_token());
#else
    const auto endpoint = native_socket_root() / (random_token() + "-ui.sock");
#endif
    const auto canonical = initial_state(VehicleCatalog::qinggan09);
    {
        ControlServer server(endpoint, [&](const Json &input) {
            const auto op = input.value("op", std::string{});

            if (op == "cluster_power" || op == "cluster_restart") {
                entered = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(650));
                completed = true;
            }

            return Json{{"ok", true},
                        {"session_id", "lifecycle-wait"},
                        {"revision", canonical.at("sequence")},
                        {"state", canonical},
                        {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                        {"wheel_routes", EventQueue::routes()},
                        {"deliveries", {{"qnx", {{"lifecycle_supported", true}}}}}};
        });
        std::thread worker([&] {
            server.serve(stopped);
        });
        try {
            {
                Client client(endpoint, "lifecycle-test");
                wait(
                        [&] {
                            return client.state().ready;
                        },
                        "IPC client must become ready");

                for (bool restart : {false, true}) {
                    entered = false;
                    completed = false;
                    check(restart ? client.cluster_restart() : client.cluster_power(false),
                          "QNX lifecycle queued");
                    try {
                        wait(
                                [&] {
                                    return entered.load();
                                },
                                "QNX lifecycle reaches supervisor");
                    } catch (...) {
                        throw std::runtime_error("QNX lifecycle reaches supervisor: " +
                                                 client.state().error);
                    }

                    const auto deadline = std::chrono::steady_clock::now() +
                                          std::chrono::seconds(2);

                    while (!completed.load()) {
                        check(client.state().error.empty(),
                              "QNX lifecycle timed out while supervisor still worked");
                        check(std::chrono::steady_clock::now() < deadline,
                              "QNX lifecycle fixture deadline");
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    check(client.state().ready && client.state().error.empty(),
                          "QNX lifecycle response retained");
                }
            }
        } catch (...) {
            stopped = true;
            worker.join();
            throw;
        }

        stopped = true;
        worker.join();
    }
}

// Controls of fields that reach neither guest are grayed out as a profile limit, and every edited
// field reaches the session alone: the model keeps its coupled fields.
void profile_limited_controls(const std::filesystem::path &assets) {
    ImGui::CreateContext();
    try {
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DeltaTime = 1.f / 60;
        io.ConfigInputTrickleEventQueue = false;
        load_ui_fonts(fonts_directory);
        const auto geometry = view_layout(canonical_ui_scale);
        io.DisplaySize = ImVec2(geometry.canvas_width, geometry.canvas_height);
        io.DisplayFramebufferScale = ImVec2(1920.f / canonical_ui_width,
                                            1920.f / canonical_ui_width);
        const auto canonical = initial_state(VehicleCatalog::qinggan09);
        auto cluster = Json::array();

        for (const auto &field : catalog(VehicleCatalog::qinggan09)) {
            if (field.name != "door_fl" && field.name != "remain_mileage_e" &&
                field.name != "hood") {
                cluster.push_back(field.name);
            }
        }

        const Json delivery = {{"cluster", cluster}, {"multimedia", Json::array({"hood"})}};

        std::mutex mutex;
        std::vector<Json> patches;
        auto transport = [&](const Json &operation) -> Json {
            if (operation.at("op") == "patch") {
                std::lock_guard lock(mutex);
                patches.push_back(operation.at("values"));
            }

            return Json{{"ok", true},
                        {"session_id", "profile-limit-fixture"},
                        {"revision", canonical.at("sequence")},
                        {"state", canonical},
                        {"catalog", catalog_json(VehicleCatalog::qinggan09)},
                        {"field_delivery", delivery}};
        };
        auto sent = [&] {
            std::lock_guard lock(mutex);

            return patches;
        };

        for (const std::size_t panel : {std::size_t{2}, std::size_t{0}}) {
            Options options;
            options.assets = assets;
            options.endpoint = "profile-limit-fixture";
            options.capture_ui = "headless-profile-limit-inspection";
            options.inspect_panel = panel;
            options.smoke_seconds = .25;
            Window window(options, transport);
            wait(
                    [&] {
                        return window.client().state().ready;
                    },
                    "profile limit fixture must have a canonical state");
            check(!field_reaches_guest(window.client().state(), "door_fl") &&
                          field_reaches_guest(window.client().state(), "hood"),
                  "a field in neither delivery list stays in the model");
            auto render = [&](ImVec2 mouse, bool down) {
                io.AddMousePosEvent(mouse.x, mouse.y);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
                ImGui::NewFrame();
                window.draw({1001, 1920, 720}, {1002, 360, 308}, {1003, 360, 308},
                            canonical_ui_scale);
                ImGui::Render();
            };

            for (int frame = 0; frame < 60; ++frame) {
                render(ImVec2(0, 0), false);
            }

            auto item = [&](const char *name) {
                for (const auto &field : window.inspection().at("fields")) {
                    if (field.at("name") == name) {
                        return field;
                    }
                }

                throw std::runtime_error(std::string("control is not rendered: ") + name);
            };
            auto click = [&](const Json &field) {
                const auto rect = field.at("rect_points").get<std::array<float, 4>>();
                const ImVec2 point(rect[0] + 8, rect[1] + rect[3] / 2);
                render(point, false);
                render(point, true);
                render(point, false);
                render(point, false);
            };
            const char *limited = panel == 2 ? "door_fl" : "remain_mileage_e";
            const char *delivered = panel == 2 ? "hood" : "actual_power";
            check(!item(limited).at("enabled").get<bool>() &&
                          item(limited).at("profile_limit").get<bool>(),
                  "a control whose field reaches neither guest is grayed out as a profile limit");
            check(item(delivered).at("enabled").get<bool>() &&
                          !item(delivered).at("profile_limit").get<bool>(),
                  "a control whose field reaches one guest stays enabled");
            click(item(limited));

            if (panel == 0) {
                io.AddInputCharactersUTF8("3");
                render(ImVec2(0, 0), false);
                render(ImVec2(0, 0), false);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            check(sent().empty(), "a profile-limited control sends no patch");
            click(item(delivered));

            if (panel == 0) {
                io.AddInputCharactersUTF8("3");
                const auto rect = item(delivered).at("rect_points").get<std::array<float, 4>>();
                render(ImVec2(rect[0] + 8, rect[1] + rect[3] / 2), false);
            }

            wait(
                    [&] {
                        return sent().size() == 1;
                    },
                    "an enabled control sends its patch");
            const auto values = sent().front();
            check(values.size() == 1 && values.contains(delivered),
                  "the window sends the edited field alone");
            window.close();
            std::lock_guard lock(mutex);
            patches.clear();
        }

        ImGui::DestroyContext();
    } catch (...) {
        ImGui::DestroyContext();
        throw;
    }
}

int main(int argc, char **argv) {
    try {
        lifecycle_client_deadlines();
        ToggleAnimation toggle;
        check(toggle.value(false, 0) == 0, "toggle initializes without animation");
        check(toggle.value(true, 1) == 0 && toggle.active(1),
              "toggle starts at current position and wakes native frames");
        check(std::abs(toggle.value(true, 1.1) - .71875f) < .00001f,
              "toggle follows OEM Overshoot(1) at100ms");
        check(toggle.value(true, 1.3) > 1 && toggle.active(1.3), "toggle overshoots before400ms");
        check(toggle.value(true, 1.4) == 1 && !toggle.active(1.4), "toggle settles after400ms");
        check(toggle.value(false, 2) == 1, "reverse starts at the current endpoint");
        const auto interrupted = toggle.value(false, 2.15);
        check(toggle.value(true, 2.15) == interrupted, "rapid retarget has no position jump");
        control_drawing_guards();
        localized_tab_layout();
        music_tab_animation();
        parsing();
        parameter_layout_guards();
        input_transform_guards();

        if (argc != 3) {
            throw std::runtime_error("test requires approved assets and fixture font paths");
        }

        fonts_directory = argv[2];

        regions(argv[1]);
        profile_wheel_layout(argv[1]);
        panel_inspection_guards(argv[1]);
        inspection_lifetime(argv[1]);
        window_snapshot_before_frame(argv[1]);
        appearance_following(argv[1]);
        startup_frame_gate(argv[1]);
        wheel_hover_without_tooltips(argv[1]);
        choices_between_wheel_and_edge(argv[1]);
        wheel_column_controls(argv[1]);
        lifecycle_glyph_inks(argv[1]);
        passion_unsupported_wheel(argv[1]);
        {
            const char *qinggan11_env = std::getenv("VOYEMU_QINGGAN11_FIXTURE");
            qinggan11_adversarial_wheel_acceptance(
                    argv[1], qinggan11_env
                                     ? std::filesystem::path(qinggan11_env)
                                     : std::filesystem::path(argv[1]).parent_path() / "free-2024");
        }
        unavailable_toggle(argv[1]);
        profile_limited_controls(argv[1]);
        editor_sections_fill_available_height(argv[1]);
        editor_sections_fill_available_height(argv[1], true);
        radio_transition_colors(argv[1]);
        radio_transition_colors(argv[1], 75);
        radio_transition_colors(argv[1], 100);
        asynchronous_client();
        std::cout << "cluster UI guards: pass\n";

        return 0;
    } catch (const std::exception &error) {
        std::cerr << "cluster UI guards: " << error.what() << '\n';

        return 1;
    }
}
