#include "cluster_ui_internal.hpp"
#include "field_delivery.hpp"

namespace voyemu::cluster {
using namespace cluster_ui_detail;
namespace {
constexpr std::array<const char *, 19> wheel_ids = {"ica",
                                                    "night_vision",
                                                    "acc",
                                                    "distance",
                                                    "cruise_up",
                                                    "cruise_down",
                                                    "custom",
                                                    "cancel",
                                                    "menu",
                                                    "voice",
                                                    "volume_up",
                                                    "ok",
                                                    "volume_down",
                                                    "previous",
                                                    "next",
                                                    "photo",
                                                    "phone",
                                                    "distance_decrease",
                                                    "distance_increase"};

std::string owner_id() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();

    return "cluster-" + std::to_string(tick);
}
} // namespace

using ui::tr;

namespace cluster_ui_detail {
const ControlPalette &control_palette(bool light) {
    static constexpr ControlPalette dark{IM_COL32(0, 0, 0, 255),       IM_COL32(12, 20, 30, 255),
                                         IM_COL32(21, 36, 52, 255),    IM_COL32(29, 51, 73, 255),
                                         IM_COL32(47, 67, 91, 255),    IM_COL32(255, 255, 255, 255),
                                         IM_COL32(255, 255, 255, 128), IM_COL32(83, 105, 129, 255),
                                         IM_COL32(71, 180, 255, 255),  IM_COL32(224, 113, 105, 255),
                                         IM_COL32(202, 202, 202, 255), IM_COL32(55, 63, 74, 255),
                                         IM_COL32(28, 40, 56, 255)};
    // OEM simple/Speedy Dash: Settings and Music resources, verified against original SDK pixels.
    static constexpr ControlPalette light_colors{
            IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255),
            IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255),
            IM_COL32(151, 151, 151, 80),  IM_COL32(45, 52, 66, 255),
            IM_COL32(45, 52, 66, 128),    IM_COL32(45, 52, 66, 179),
            IM_COL32(66, 158, 253, 255),  IM_COL32(211, 44, 44, 255),
            IM_COL32(80, 89, 102, 255),   IM_COL32(255, 255, 255, 255),
            IM_COL32(151, 151, 151, 80)};

    return light ? light_colors : dark;
}

ImU32 control_ink(ImU32 color) {
    return ImGui::GetColorU32(ImGui::ColorConvertU32ToFloat4(color));
}

ImU32 alpha_ink(ImU32 color, unsigned alpha) {
    return (color & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

bool event_name(std::string_view name) {
    return name.starts_with("swm_");
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return value;
}

float sample_ink_height(ImFont *font, float size, float density, const char *sample) {
    auto *baked = font->GetFontBaked(size, density);
    float top = FLT_MAX, bottom = -FLT_MAX;

    for (const unsigned char c : std::string_view(sample)) {
        if (c != ' ') {
            const auto *glyph = baked->FindGlyph(c);
            top = std::min(top, glyph->Y0);
            bottom = std::max(bottom, glyph->Y1);
        }
    }

    return bottom - top;
}

float sample_ink_bottom(ImFont *font, float size, float density, const char *sample) {
    auto *baked = font->GetFontBaked(size, density);
    float bottom = -FLT_MAX;

    for (const unsigned char c : std::string_view(sample)) {
        if (c != ' ') {
            bottom = std::max(bottom, baked->FindGlyph(c)->Y1);
        }
    }

    return bottom;
}

float reference_font_size(ImFont *font, const char *sample, float guest_ink, unsigned scale) {
    // Original1920x720 SDK frames: Music BT ink27px; Schedule charging ink28px.
    // Match active STB glyph bounds at the actual physical rasterizer density, then verify GPU
    // pixels separately.
    const float density = physical_density(scale), target = guest_ink * guest_scale(scale),
                probe = 24;
    const float estimate = probe * target /
                           (sample_ink_height(font, probe, density, sample) * density);
    const float middle = std::floor(estimate * 4) / 4;
    float best = middle, error = FLT_MAX;

    for (int i = -2; i <= 2; ++i) {
        const float size = middle + i * .25f;
        const float candidate =
                std::abs(sample_ink_height(font, size, density, sample) * density - target);

        if (candidate < error) {
            error = candidate;
            best = size;
        }
    }

    return best;
}
} // namespace cluster_ui_detail

Options parse_options(int argc, const char *const *argv) {
    Options options;
    std::set<std::string> seen;

    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];

        if (flag != "--endpoint" && flag != "--frame" && flag != "--assets" && flag != "--fonts" &&
            flag != "--replay-frame" && flag != "--smoke-seconds" && flag != "--capture-ui" &&
            flag != "--profile" && flag != "--inspect-panel" && flag != "--appearance") {
            throw VoyemuError("CLUSTER_UI", "Unknown cluster option: " + flag);
        }

        if (!seen.insert(flag).second || i + 1 >= argc || std::string_view(argv[i + 1]).empty() ||
            std::string_view(argv[i + 1]).starts_with("--")) {
            throw VoyemuError("CLUSTER_UI", "Missing or duplicate cluster option: " + flag);
        }

        ++i;
        auto path = std::filesystem::u8path(argv[i]);

        if (flag == "--endpoint") {
            options.endpoint = path;
        }

        if (flag == "--frame") {
            options.frame = path;
        }

        if (flag == "--assets") {
            options.assets = path;
        }

        if (flag == "--fonts") {
            options.fonts = path;
        }

        if (flag == "--replay-frame") {
            options.replay_frame = path;
        }

        if (flag == "--capture-ui") {
            options.capture_ui = path;
        }

        if (flag == "--appearance") {
            const std::string_view scheme = argv[i];

            if (scheme != "light" && scheme != "dark") {
                throw VoyemuError("CLUSTER_UI", "--appearance must be light or dark");
            }

            options.light_theme = scheme == "light";
        }

        if (flag == "--inspect-panel") {
            const std::string_view requested = argv[i];
            const std::array<std::string_view, ParametersLayout::section_count> names = {
                    "movement", "lights", "body", "tyres", "raw"};
            const auto named = std::find(names.begin(), names.end(), requested);

            if (requested.size() == 1 && requested[0] >= '0' && requested[0] <= '4') {
                options.inspect_panel = static_cast<std::size_t>(requested[0] - '0');
            } else if (named != names.end()) {
                options.inspect_panel = static_cast<std::size_t>(named - names.begin());
            } else {
                throw VoyemuError("CLUSTER_UI",
                                  "--inspect-panel must be 0..4 or movement/body/lights/tyres/raw");
            }
        }

        if (flag == "--profile") {
            options.profile = argv[i];

            if (options.profile.size() > 128 ||
                std::any_of(options.profile.begin(), options.profile.end(), [](unsigned char c) {
                    return c < 0x20 || c == 0x7f;
                })) {
                throw VoyemuError("CLUSTER_UI", "Invalid cluster profile title");
            }
        }

        if (flag == "--smoke-seconds") {
            const auto seconds = parse_number(argv[i]);

            if (!seconds || *seconds < .1 || *seconds > 3600) {
                throw VoyemuError("CLUSTER_UI", "Smoke duration must be0.1…3600seconds");
            }

            options.smoke_seconds = *seconds;
        }
    }

    if (options.assets.empty() || options.fonts.empty() ||
        (options.endpoint.empty() && options.replay_frame.empty()) ||
        (options.frame.empty() && options.replay_frame.empty())) {
        throw VoyemuError(
                "CLUSTER_UI",
                "voyemu-cluster requires --endpoint PATH --frame PATH --assets PATH --fonts PATH; explicit --replay-frame BMP permits developer frame inspection");
    }

    if (!options.replay_frame.empty() && !options.frame.empty()) {
        throw VoyemuError("CLUSTER_UI", "Replay and live frame paths cannot be combined");
    }

    if (!options.capture_ui.empty() && options.smoke_seconds == 0) {
        throw VoyemuError("CLUSTER_UI", "--capture-ui requires --smoke-seconds");
    }

    if (seen.contains("--appearance") && options.replay_frame.empty()) {
        throw VoyemuError("CLUSTER_UI", "--appearance requires --replay-frame");
    }

    if (options.inspect_panel && options.capture_ui.empty()) {
        throw VoyemuError("CLUSTER_UI",
                          "--inspect-panel requires --capture-ui and --smoke-seconds");
    }

    return options;
}

std::string window_title(const Options &options) {
    return options.profile.empty() ? std::string("Voyemu") : "Voyemu " + options.profile;
}

std::string screens_window_title(const Options &options, std::string_view screens_label) {
    return window_title(options) + " - " + std::string(screens_label);
}

std::optional<std::uint64_t> parse_unsigned(std::string_view text, unsigned width) {
    if (text.empty() || width < 1 || width > 32) {
        return std::nullopt;
    }

    std::uint64_t result{};
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), result);

    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() ||
        result > ((std::uint64_t{1} << width) - 1)) {
        return std::nullopt;
    }

    return result;
}

std::optional<double> parse_number(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    double result{};
    std::istringstream input(std::string{text});
    input.imbue(std::locale::classic());
    input >> std::noskipws >> result;

    if (!input || input.rdbuf()->sgetc() != std::char_traits<char>::eof() ||
        !std::isfinite(result)) {
        return std::nullopt;
    }

    return result;
}

bool Region::contains(double x, double y) const {
    return wheel_geometry::contains(rect, polygon, x, y);
}

bool Roller::contains(double x, double y) const {
    return wheel_geometry::contains(rect, polygon, x, y);
}

WheelGeometry read_wheel_geometry(const std::filesystem::path &path) {
    std::ifstream source(path);

    if (!source) {
        throw VoyemuError("CLUSTER_UI", "Cannot read wheel hit regions");
    }

    const auto document = Json::parse(
            std::string(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()));
    const bool profile_layout = document.contains("joined_offset_logical_points");

    if (document.at("schema") != 1 ||
        (!profile_layout &&
         (document.at("regions").size() != 17 || document.at("rollers").size() != 2)) ||
        (profile_layout &&
         (document.at("regions").empty() || document.at("regions").size() > wheel_ids.size() ||
          document.at("rollers").size() > 2))) {
        throw VoyemuError("CLUSTER_UI", "Unsupported wheel region schema");
    }

    for (const auto side : {"left", "right"}) {
        if (document.at("photo_display_logical_points").at(side) != Json::array({180, 154})) {
            throw VoyemuError("CLUSTER_UI",
                              "Wheel source geometry does not match the display boxes");
        }
    }

    const double maximum_error =
            document.at("flattening").at("maximum_error_logical_points").get<double>();

    if (!std::isfinite(maximum_error) || maximum_error <= 0 || maximum_error > .0625 ||
        document.at("flattening").at("maximum_backing_scale") != 4) {
        throw VoyemuError("CLUSTER_UI", "Unbounded wheel contour flattening");
    }

    auto geometry = [&](const Json &entry, auto &region) {
        region.photo = entry.at("photo").template get<std::string>();

        if (region.photo != "left" && region.photo != "right") {
            throw VoyemuError("CLUSTER_UI", "Invalid wheel region side");
        }

        region.rect = entry.at("rect_normalized").template get<std::array<double, 4>>();

        for (double n : region.rect) {
            if (!std::isfinite(n) || n < 0 || n > 1) {
                throw VoyemuError("CLUSTER_UI", "Invalid wheel region bounds");
            }
        }

        if (region.rect[2] <= 0 || region.rect[3] <= 0 ||
            region.rect[0] + region.rect[2] > 1 + wheel_geometry::coordinate_epsilon ||
            region.rect[1] + region.rect[3] > 1 + wheel_geometry::coordinate_epsilon) {
            throw VoyemuError("CLUSTER_UI", "Empty or out of photo wheel region");
        }

        for (const auto &point : entry.at("clip_polygon_normalized")) {
            const auto p = point.template get<std::array<double, 2>>();
            region.polygon.push_back({p[0], p[1]});
        }

        if (!wheel_geometry::valid_polygon(region.polygon)) {
            throw VoyemuError("CLUSTER_UI", "Invalid wheel face contour");
        }

        const auto interior =
                entry.at("interior_point_normalized").template get<std::array<double, 2>>();

        if (!region.contains(interior[0], interior[1])) {
            throw VoyemuError("CLUSTER_UI", "Wheel fixture lies outside its face contour");
        }

        const double error = entry.at("flattening_error_logical_points").template get<double>();

        if (!std::isfinite(error) || error < 0 || error > maximum_error) {
            throw VoyemuError("CLUSTER_UI", "Wheel face exceeds its flattening tolerance");
        }

        if (entry.at("source_svg") != "wheel-" + region.photo + ".svg" ||
            !entry.at("source_path").is_string() ||
            entry.at("source_path").template get<std::string>().empty()) {
            throw VoyemuError("CLUSTER_UI", "Wheel face source identity mismatch");
        }
    };
    std::set<std::string> ids;
    WheelGeometry result;

    for (const auto &entry : document.at("regions")) {
        Region region;
        region.id = entry.at("id").get<std::string>();
        region.label = entry.at("label").get<std::string>();
        const auto identity = std::find(wheel_ids.begin(), wheel_ids.end(), region.id);

        if (!ids.insert(region.id).second || identity == wheel_ids.end() ||
            (!profile_layout && std::distance(wheel_ids.begin(), identity) >= 17) ||
            region.label.empty() || region.label.size() > 128) {
            throw VoyemuError("CLUSTER_UI", "Invalid wheel region identity");
        }

        geometry(entry, region);
        const bool left = std::distance(wheel_ids.begin(), identity) < 8;

        if ((!profile_layout && region.photo != (left ? "left" : "right")) ||
            entry.at("source_face") != "face-" + region.id) {
            throw VoyemuError("CLUSTER_UI", "Wheel action does not belong to its source face");
        }

        if (!profile_layout || entry.value("enabled", true)) {
            result.regions.push_back(std::move(region));
        }
    }

    for (const auto &entry : document.at("regions")) {
        if (profile_layout && !entry.value("enabled", true)) {
            continue;
        }

        const auto interior = entry.at("interior_point_normalized").get<std::array<double, 2>>();
        const auto *selected = wheel_geometry::hit(
                result.regions, entry.at("photo").get<std::string>(), interior[0], interior[1]);

        if (!selected || selected->id != entry.at("id").get<std::string>()) {
            throw VoyemuError("CLUSTER_UI", "Wheel action fixture is ambiguous");
        }
    }

    const auto left_join = std::find_if(result.regions.begin(), result.regions.end(),
                                        [](const auto &r) {
                                            return r.id == "cancel";
                                        });
    const auto right_join = std::find_if(result.regions.begin(), result.regions.end(),
                                         [](const auto &r) {
                                             return r.id == "menu";
                                         });

    if (profile_layout) {
        result.joined_offset = document.at("joined_offset_logical_points").get<float>();
    } else {
        result.joined_offset = static_cast<float>(
                180 * (left_join->rect[0] + left_join->rect[2] - right_join->rect[0]) + 2);
    }

    if (!std::isfinite(result.joined_offset) || result.joined_offset <= 0 ||
        result.joined_offset >= 180) {
        throw VoyemuError("CLUSTER_UI", "Invalid wheel center join");
    }

    ids.clear();

    for (const auto &entry : document.at("rollers")) {
        Roller roller;
        geometry(entry, roller);
        roller.up = entry.at("up").get<std::string>();
        roller.down = entry.at("down").get<std::string>();
        const bool left = roller.photo == "left";

        if (!ids.insert(roller.photo).second || entry.at("id") != "roller-" + roller.photo ||
            entry.at("source_face") != "roller-" + roller.photo ||
            roller.up != (left ? "cruise_up" : "volume_up") ||
            roller.down != (left ? "cruise_down" : "volume_down")) {
            throw VoyemuError("CLUSTER_UI", "Invalid wheel roller route");
        }

        if (!profile_layout || entry.value("enabled", true)) {
            result.rollers.push_back(std::move(roller));
        }
    }

    return result;
}

std::vector<Region> read_regions(const std::filesystem::path &path) {
    return read_wheel_geometry(path).regions;
}

Client::Client(std::filesystem::path endpoint, std::string owner, Transport transport)
    : endpoint_(std::move(endpoint)), owner_(std::move(owner)), transport_(std::move(transport)) {
    if (owner_.empty() || owner_.size() > 128) {
        throw VoyemuError("CLUSTER_UI", "Invalid cluster input owner");
    }

    if (!transport_) {
        transport_ = [this](const Json &operation) {
            const auto op = operation.value("op", std::string{});

            return voyemu::request(endpoint_, operation,
                                   op == "media_power" || op == "media_restart" ||
                                                   op == "cluster_power" || op == "cluster_restart"
                                           ? request_timeout_limit_ms(op)
                                           : 500);
        };
    }

    if (!endpoint_.empty()) {
        thread_ = std::thread([this] {
            run();
        });
    }
}

Client::~Client() {
    try {
        release_owner();
    } catch (...) { /* Ignored: destructors must not throw. */
    }

    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
}

ViewState Client::state() const {
    std::lock_guard lock(mutex_);

    return state_;
}

bool Client::copy_state(ViewState &destination, std::uint64_t &version) const {
    std::lock_guard lock(mutex_);

    if (version == state_version_) {
        return false;
    }

    destination = state_;
    version = state_version_;

    return true;
}

bool Client::patch(const Json &values) {
    std::lock_guard lock(mutex_);

    if (!state_.ready || !values.is_object() || values.empty()) {
        return false;
    }

    for (const auto &[name, value] : values.items()) {
        if (!state_.raw.contains(name) || event_name(name) || !value.is_number_integer() ||
            value.get<std::int64_t>() < 0) {
            return false;
        }
    }

    for (const auto &[name, value] : values.items()) {
        patch_[name] = value;
    }

    wake_.notify_one();

    return true;
}

bool field_reaches_guest(const ViewState &state, const std::string &name) {
    return !state.model_only.contains(name);
}

std::string concept_control(const ViewState &state, VehicleConcept id) {
    const auto field = concept_field(state.platform, id);

    return field.empty() ? std::string(concept_definition(id).name) : field;
}

std::optional<std::int64_t> control_value(const ViewState &state, const std::string &name) {
    if (!state.raw.contains(name)) {
        return std::nullopt;
    }

    const auto raw = state.raw.at(name).get<std::uint64_t>();
    const auto *binding = field_binding(state.platform, name);

    if (!binding || binding->fields.front().field != name) {
        return static_cast<std::int64_t>(raw);
    }

    const auto decoded = concept_decode(binding->fields.front().encoding, raw);

    return decoded ? std::optional<std::int64_t>(std::llround(*decoded)) : std::nullopt;
}

bool wheel_action_available(const ViewState &state, std::string_view id) {
    const auto family = state.deliveries.value("android", Json::object())
                                .value("presentation_family", std::string{});

    if (family == "nxp" && !state.routes.contains(std::string(id))) {
        return false;
    }

    const auto &capability = state.wheel_capability;

    if (!capability.is_object() ||
        capability.value("authority", std::string{}) != "com.voyemu.input.wheel") {
        return true;
    }

    const bool partial = qinggan11_partial_wheel_capability_matches(capability);

    if (!partial) {
        const auto reason = capability.value("reason", std::string{});

        if ((reason != "provider_not_installed" && reason != "oem_delivery_unverified") ||
            capability != Json{{"schema", 1},
                               {"profile", capability.value("profile", std::string{})},
                               {"status", "unsupported"},
                               {"supported", false},
                               {"reason", reason},
                               {"authority", "com.voyemu.input.wheel"}}) {
            return false;
        }
    }

    if (partial) {
        const auto routes = capability.at("routes");

        if (routes.contains(id) &&
            state.routes.value(std::string(id), Json::object()).value("android", Json{}) ==
                    Json{{"key_code", routes.at(id)}}) {
            return true;
        }
    }

    const auto qnx = state.deliveries.value("qnx", Json::object());
    const auto status = qnx.value("status", std::string{});

    return qnx.value("lifecycle_supported", false) &&
           (status == "booting" || status == "graphics_running") &&
           state.routes.value(std::string(id), Json::object())
                   .value("qnx_index", Json{})
                   .is_number_integer();
}

bool Client::wheel(std::string_view id, std::string_view phase) {
    std::lock_guard lock(mutex_);

    if (!state_.ready || std::find(wheel_ids.begin(), wheel_ids.end(), id) == wheel_ids.end() ||
        (phase != "down" && phase != "up" && phase != "cancel")) {
        return false;
    }

    if (state_.deliveries.value("android", Json::object())
                        .value("presentation_family", std::string{}) == "passion" &&
        (!state_.wheel_capability.is_object() ||
         !state_.wheel_capability.value("routes", Json::object()).contains(std::string(id)))) {
        return true;
    }

    if (phase == "down" && !wheel_action_available(state_, id)) {
        return false;
    }

    if (events_.size() >= 512) {
        state_.error = "Wheel queue limit reached";
        ++state_version_;

        return false;
    }

    events_.push_back(
            {{"schema", 1}, {"op", "wheel"}, {"owner", owner_}, {"id", id}, {"phase", phase}});
    wake_.notify_one();

    return true;
}

bool Client::detent(std::string_view id) {
    std::lock_guard lock(mutex_);

    if (!state_.ready || std::find(wheel_ids.begin(), wheel_ids.end(), id) == wheel_ids.end() ||
        events_.size() > 510) {
        return false;
    }

    if (state_.deliveries.value("android", Json::object())
                        .value("presentation_family", std::string{}) == "passion" &&
        (!state_.wheel_capability.is_object() ||
         !state_.wheel_capability.value("routes", Json::object()).contains(std::string(id)))) {
        return true;
    }

    if (!wheel_action_available(state_, id)) {
        return false;
    }

    events_.push_back(
            {{"schema", 1}, {"op", "wheel"}, {"owner", owner_}, {"id", id}, {"phase", "down"}});
    events_.push_back(
            {{"schema", 1}, {"op", "wheel"}, {"owner", owner_}, {"id", id}, {"phase", "up"}});
    wake_.notify_one();

    return true;
}

void Client::release_owner() {
    std::lock_guard lock(mutex_);

    if (endpoint_.empty()) {
        return;
    }

    // A release is never dropped because of the normal event queue limit.
    events_.push_back({{"schema", 1}, {"op", "release_owner"}, {"owner", owner_}});
    wake_.notify_one();
}

void Client::close_cluster() {
    std::lock_guard lock(mutex_);

    if (endpoint_.empty()) {
        return;
    }

    events_.push_back({{"schema", 1}, {"op", "release_owner"}, {"owner", owner_}});
    events_.push_back({{"schema", 1}, {"op", "close_cluster"}, {"owner", owner_}});
    wake_.notify_one();
}

bool Client::cluster_power(bool enabled) {
    std::lock_guard lock(mutex_);

    if (!state_.ready ||
        !state_.deliveries.value("qnx", Json::object()).value("lifecycle_supported", false) ||
        events_.size() >= 512) {
        return false;
    }

    events_.push_back(
            {{"schema", 1}, {"op", "cluster_power"}, {"enabled", enabled}, {"owner", owner_}});
    wake_.notify_one();

    return true;
}

bool Client::cluster_restart() {
    std::lock_guard lock(mutex_);

    if (!state_.ready ||
        !state_.deliveries.value("qnx", Json::object()).value("lifecycle_supported", false) ||
        events_.size() >= 512) {
        return false;
    }

    events_.push_back({{"schema", 1}, {"op", "cluster_restart"}, {"owner", owner_}});
    wake_.notify_one();

    return true;
}

bool Client::media_power(bool enabled) {
    std::lock_guard lock(mutex_);

    if (!state_.ready ||
        !state_.deliveries.value("android", Json::object()).value("lifecycle_supported", false) ||
        events_.size() >= 512) {
        return false;
    }

    events_.push_back(
            {{"schema", 1}, {"op", "media_power"}, {"enabled", enabled}, {"owner", owner_}});
    wake_.notify_one();

    return true;
}

bool Client::media_restart() {
    std::lock_guard lock(mutex_);

    if (!state_.ready ||
        !state_.deliveries.value("android", Json::object()).value("lifecycle_supported", false) ||
        events_.size() >= 512) {
        return false;
    }

    events_.push_back({{"schema", 1}, {"op", "media_restart"}, {"owner", owner_}});
    wake_.notify_one();

    return true;
}

void Client::ingest(const Json &response) {
    if (!response.value("ok", false)) {
        throw std::runtime_error(
                response.value("error", Json("Session rejected operation")).dump());
    }

    if (!response.contains("state")) {
        return;
    }

    validate_state(response.at("state"));

    if (response.at("revision") != response.at("state").at("sequence")) {
        throw VoyemuError("CLUSTER_UI", "Canonical revision identity mismatch");
    }

    const auto &raw = response.at("state").at("raw_oem");
    const auto &catalog = response.at("catalog");
    // The catalogue of the platform of the running machine, which map_sha256 of its state names.
    const auto &platform_catalog = voyemu::catalog(state_catalog(response.at("state")));

    if (!raw.is_object() || raw.size() != platform_catalog.size() || !catalog.is_array() ||
        catalog.size() != platform_catalog.size()) {
        throw VoyemuError("CLUSTER_UI", "Incompatible canonical vehicle catalog");
    }

    std::set<std::string> names, model_only;
    std::set<unsigned> indexes;

    for (const auto &field : catalog) {
        const std::string name = field.at("name").get<std::string>();
        const unsigned width = field.at("width").get<unsigned>(),
                       index = field.at("index").get<unsigned>();

        if (!names.insert(name).second || !indexes.insert(index).second ||
            (index < 1 || index > platform_catalog.size()) || width < 1 || width > 64 ||
            !raw.contains(name) || !raw.at(name).is_number_integer() ||
            (!raw.at(name).is_number_unsigned() && raw.at(name).get<std::int64_t>() < 0) ||
            raw.at(name).get<std::uint64_t>() > field_maximum(width)) {
            throw VoyemuError("CLUSTER_UI", "Malformed canonical vehicle field");
        }

        const auto &expected = platform_catalog.at(index - 1);

        if (expected.index != index || expected.name != name || expected.width != width) {
            throw VoyemuError("CLUSTER_UI", "Canonical profile catalog mismatch");
        }
    }

    // A status without field delivery, such as the local keyboard smoke, reaches every guest.
    if (response.contains("field_delivery")) {
        const auto delivery = field_delivery_from_status(response.at("field_delivery"));

        for (const auto &name : names) {
            if (!delivery.cluster.contains(name) && !delivery.multimedia.contains(name)) {
                model_only.insert(name);
            }
        }
    }

    const std::string session = response.at("session_id").get<std::string>();
    const auto revision = response.at("revision").get<std::uint64_t>();
    std::lock_guard lock(mutex_);

    if (!state_.session_id.empty() && state_.session_id != session) {
        throw VoyemuError("CLUSTER_UI", "Endpoint changed session identity");
    }

    if (revision < state_.revision) {
        return;
    }

    state_.ready = true;
    state_.session_id = session;
    state_.revision = revision;
    state_.raw = raw;
    state_.catalog = catalog;
    state_.model_only = std::move(model_only);
    state_.platform = state_catalog(response.at("state"));
    state_.deliveries = response.value("deliveries", Json::object());
    state_.routes = response.value("wheel_routes", Json::object());
    state_.wheel_capability = response.value("wheel_capability", Json::object());
    state_.error.clear();
    ++state_version_;
}

void Client::run() {
    using Clock = std::chrono::steady_clock;
    auto next_poll = Clock::now();

    while (true) {
        Json operation;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_until(lock, next_poll, [this] {
                return stopping_ || !events_.empty() || !patch_.empty();
            });

            if (!events_.empty()) {
                operation = std::move(events_.front());
                events_.pop_front();
            } else if (!patch_.empty()) {
                operation = {{"schema", 1},
                             {"op", "patch"},
                             {"owner", owner_},
                             {"values", std::move(patch_)}};
                patch_ = Json::object();
            } else if (stopping_) {
                break;
            } else {
                operation = {{"schema", 1}, {"op", "status"}, {"owner", owner_}};
                next_poll = Clock::now() + std::chrono::milliseconds(250);
            }

            if (!state_.session_id.empty()) {
                operation["session_id"] = state_.session_id;
            }
        }
        try {
            ingest(transport_(operation));
        } catch (const std::exception &error) {
            std::lock_guard lock(mutex_);
            state_.ready = false;
            state_.error = error.what();
            ++state_version_;
            patch_ = Json::object();
            events_.clear();

            // Stale transient presses never replay after recovery. Supervisor owner lease forces
            // release.
            if (stopping_) {
                break;
            }

            next_poll = Clock::now() + std::chrono::milliseconds(500);
        }
    }
}

Window::Window(Options options, Client::Transport transport)
    : options_(std::move(options)), client_(options_.endpoint, owner_id(), std::move(transport)),
      light_theme_(options_.light_theme) {
    if (options_.inspect_panel) {
        if (*options_.inspect_panel >= ParametersLayout::section_count ||
            options_.capture_ui.empty() || !std::isfinite(options_.smoke_seconds) ||
            options_.smoke_seconds < .1 || options_.smoke_seconds > 3600) {
            throw VoyemuError("CLUSTER_UI",
                              "Panel inspection requires a valid section and bounded own capture");
        }

        section_ = *options_.inspect_panel;
        expanded_ = true;
    }

    auto geometry = read_wheel_geometry(options_.assets / "wheel-hotspots.json");
    regions_ = std::move(geometry.regions);
    rollers_ = std::move(geometry.rollers);
    joined_offset_ = geometry.joined_offset;
}

Window::~Window() {
    try {
        focus_lost();
    } catch (...) { /* Ignored: destructors must not throw. */
    }

    // After the context is gone its backend has already released these textures.
    if (artwork_context_ && ImGui::GetCurrentContext() == artwork_context_) {
        for (auto &[key, texture] : artwork_textures_) {
            ImGui::UnregisterUserTexture(texture.get());
        }
    }
}

std::optional<std::uint64_t> Window::value(const std::string &name) const {
    if (!view_.raw.contains(name)) {
        return std::nullopt;
    }

    return view_.raw.at(name).get<std::uint64_t>();
}

void Window::hold(const std::string &id, const std::string &token) {
    if (held_.contains(token)) {
        return;
    }

    for (const auto &[other, held_id] : held_) {
        if (held_id == id) {
            held_[token] = id;

            return;
        }
    }

    if (client_.wheel(id, "down")) {
        held_[token] = id;
    }
}

void Window::release(const std::string &token, bool cancel) {
    const auto item = held_.find(token);

    if (item == held_.end()) {
        return;
    }

    const auto id = item->second;
    held_.erase(item);

    for (const auto &[other, held_id] : held_) {
        if (held_id == id) {
            return;
        }
    }

    client_.wheel(id, cancel ? "cancel" : "up");
}

void Window::focus_lost() {
    held_.clear();
    client_.release_owner();
}

void Window::close() {
    if (!closed_) {
        focus_lost();

        if (options_.smoke_seconds == 0) {
            client_.close_cluster();
        }

        closed_ = true;
    }
}

bool Window::animation_active(double now) const {
    if (tab_indicator_.active(now) || gear_indicator_.active(now) || scale_indicator_.active(now) ||
        language_indicator_.active(now)) {
        return true;
    }

    if (about_open_ && std::abs(about_scroll_target_ - about_scroll_y_) > .25f) {
        return true;
    }

    for (const auto &[name, animation] : toggle_animations_) {
        if (animation.active(now)) {
            return true;
        }
    }

    return false;
}
} // namespace voyemu::cluster
