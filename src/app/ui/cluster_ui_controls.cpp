#include "cluster_ui_internal.hpp"
#include "version.hpp"

// The controls of the cluster window: vehicle state editor, wheel controls, About and frame
// drawing.
namespace voyemu::cluster {
using namespace cluster_ui_detail;
using ui::tr;
namespace {
const ui::VectorArtwork &vector_artwork(ui::Resource id) {
    static const auto parse = [](ui::Resource resource) {
        const auto bytes = ui::resource(resource);

        return ui::parse_vector_artwork(
                std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
    };
    static const auto text = parse(ui::Resource::LogoText),
                      figure = parse(ui::Resource::LogoFigure),
                      screens = parse(ui::Resource::ScreensIcon);

    return id == ui::Resource::LogoText ? text : id == ui::Resource::ScreensIcon ? screens : figure;
}

constexpr std::array<const char *, 4> gear_labels{"P", "R", "N", "D"};

constexpr std::array<const char *, 3> scale_labels{"50", "75", "100"};

constexpr std::array<unsigned, 3> scale_choices{50, 75, 100};

// The ink of an enabled on/off glyph beside the wheel: the primary ink while on or under the
// pointer and a neutral grey while off; the light theme mirrors the dark one with black for white.
ImU32 glyph_ink(bool light, bool hovered, bool on) {
    const auto primary = light ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);

    return hovered || on ? primary
           : light       ? IM_COL32(128, 128, 128, 255)
                         : control_palette(false).inactive;
}

// Draws a caption of the current font with its ink, not its advance, centred in a cell and on the
// physical pixel grid: the side bearings of the first and last glyph shift captions such as RU off
// centre by up to a point, and ImGui otherwise truncates text to whole points, two physical pixels
// at 100 percent.
void centered_caption(ImDrawList *draw, ImVec2 cell, ImVec2 size, ImU32 color, const char *text) {
    const float font_size = ImGui::GetFontSize(),
                density = ImGui::GetIO().DisplayFramebufferScale.x;
    auto *baked = ImGui::GetFont()->GetFontBaked(font_size);
    const float scale = font_size / baked->Size;
    float pen = 0, left = FLT_MAX, right = -FLT_MAX;

    for (const char *s = text; *s;) {
        unsigned int codepoint = 0;
        s += ImTextCharFromUtf8(&codepoint, s, nullptr);
        const auto *glyph = baked->FindGlyph(static_cast<ImWchar>(codepoint));

        if (glyph->Visible) {
            left = std::min(left, pen + glyph->X0 * scale);
            right = std::max(right, pen + glyph->X1 * scale);
        }

        pen += glyph->AdvanceX * scale;
    }

    if (left > right) {
        left = right = 0;
    }

    const auto snap = [density](float value) {
        return std::round(value * density) / density;
    };
    const auto flags = draw->Flags;
    draw->Flags |= ImDrawListFlags_TextNoPixelSnap;
    draw->AddText(ImVec2(snap(cell.x + (size.x - left - right) / 2),
                         snap(cell.y + (size.y - font_size) / 2)),
                  color, text);
    draw->Flags = flags;
}
} // namespace

void Window::photo(Texture texture, std::string_view side, float y) {
    if (light_theme_) {
        const auto light = side == "left" ? light_wheel_left_ : light_wheel_right_;

        if (light.id) {
            texture = light;
        }
    }

    if (!texture.id || texture.width <= 0 || texture.height <= 0) {
        return;
    }

    auto wheel = view_layout(screen_scale_).wheel;
    wheel.y = y;
    const auto rect = wheel_photo(wheel, joined_offset_, side == "right", texture.width,
                                  texture.height);
    const ImVec2 size(rect.width, rect.height), origin(rect.x, rect.y);
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddImage(ImTextureRef(texture.id), origin, ImVec2(origin.x + size.x, origin.y + size.y));
    const auto mouse = ImGui::GetIO().MousePos;
    const double px = (mouse.x - origin.x) / size.x, py = (mouse.y - origin.y) / size.y;
    // The About panel covers the wheel; raw pointer hits must not reach it.
    const Region *hovered = about_open_ ? nullptr : wheel_geometry::hit(regions_, side, px, py);

    for (const auto &roller : rollers_) {
        if (roller.photo != side) {
            continue;
        }

        const double delta = ImGui::GetIO().MouseWheel;
        const int count = wheel_geometry::detent_count(
                delta, !about_open_ && roller.contains(px, py), !held_.empty());
        const auto &id = delta > 0 ? roller.up : roller.down;

        if (wheel_action_available(view_, id)) {
            for (int i = 0; i < count; ++i) {
                if (!client_.detent(id)) {
                    break;
                }
            }
        }
    }

    for (const auto &region : regions_) {
        if (region.photo != side) {
            continue;
        }

        const bool available = wheel_action_available(view_, region.id);
        const bool hover = hovered == &region;
        const ImVec2 a(origin.x + region.rect[0] * size.x, origin.y + region.rect[1] * size.y),
                b(a.x + region.rect[2] * size.x, a.y + region.rect[3] * size.y);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(region.id.c_str());
        // Navigation uses the face's bounds. Pointer interaction uses the exact SVG contour, so a
        // gap or overlapping bounding box cannot claim focus.
        const ImRect bounds(a, b);
        const ImGuiID item = ImGui::GetID("##wheel");
        ImGui::ItemSize(bounds);
        bool item_hovered = false, item_held = false;

        if (ImGui::ItemAdd(bounds, item)) {
            ImGui::ButtonBehavior(hover ? bounds : ImRect(a, a), item, &item_hovered, &item_held,
                                  ImGuiButtonFlags{ImGuiButtonFlags_MouseButtonLeft} |
                                          ImGuiButtonFlags_NoNavFocus);
        }

        const bool focused = available && ImGui::IsItemFocused() &&
                             ImGui::GetCurrentContext()->NavCursorVisible;

        if (hover && available && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            hold(region.id, "mouse");
        }

        if (focused) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                hold(region.id, "keyboard");
            }
        } else if (held_.contains("keyboard") && held_.at("keyboard") == region.id) {
            release("keyboard", true);
        }

        bool pressed = false;

        if (!available) {
            pressed = item_held && hover;
        }

        for (const auto &[token, id] : held_) {
            if (id == region.id) {
                pressed = true;
            }
        }

        if (hover || pressed || focused) {
            const auto color = light_theme_
                                       ? alpha_ink(control_palette(true).accent, pressed ? 105 : 25)
                               : pressed ? IM_COL32(88, 164, 255, 105)
                                         : IM_COL32(225, 234, 255, 35);
            std::vector<ImVec2> points;
            points.reserve(region.polygon.size());

            for (auto point : region.polygon) {
                points.emplace_back(a.x + point.x * (b.x - a.x), a.y + point.y * (b.y - a.y));
            }

            draw_wheel_hover(*draw, points, color);
        }

        ImGui::PopID();
    }
}

void Window::record_item(const std::string &name, const char *kind) {
    if (options_.capture_ui.empty()) {
        return;
    }

    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const auto clip_a = ImGui::GetWindowDrawList()->GetClipRectMin(),
               clip_b = ImGui::GetWindowDrawList()->GetClipRectMax();
    Json item = {{"name", name},
                 {"kind", kind},
                 {"rect_points", {a.x, a.y, b.x - a.x, b.y - a.y}},
                 {"clip_points", {clip_a.x, clip_a.y, clip_b.x - clip_a.x, clip_b.y - clip_a.y}},
                 {"fully_inside_clip",
                  a.x >= clip_a.x && a.y >= clip_a.y && b.x <= clip_b.x && b.y <= clip_b.y},
                 {"visible", ImGui::IsItemVisible()},
                 {"enabled", !(ImGui::GetItemFlags() & ImGuiItemFlags_Disabled)}};

    if (std::string_view(kind) == "numeric" || std::string_view(kind) == "checkbox") {
        item["profile_limit"] =
                name == "hazard"
                        ? !field_reaches_guest(view_,
                                               concept_control(view_, VehicleConcept::turn_left)) ||
                                  !field_reaches_guest(
                                          view_, concept_control(view_, VehicleConcept::turn_right))
                        : !field_reaches_guest(view_, name);
    }

    if (std::string_view(kind) == "tab") {
        inspection_["tabs"].push_back(std::move(item));
    } else if (std::string_view(kind) == "toggle") {
        inspection_["toggle"] = std::move(item);
    } else {
        inspection_["fields"].push_back(std::move(item));
    }
}

void Window::caption(const std::string &name, const char *kind, const char *text, float x, float y,
                     float width, float height, float desired_size, std::uint32_t color) {
    const auto role = std::string_view(kind) == "tab" || std::string_view(kind) == "group"
                              ? UiFontRole::Emphasis
                      : std::string_view(kind) == "unit" ? UiFontRole::Body
                                                         : UiFontRole::Label;
    auto *font = ui_text_font(role);
    // A zero wrap width is deliberate: labels always remain complete single lines.
    const auto desired = font->CalcTextSizeA(desired_size, FLT_MAX, 0, text);
    const float fitted = std::min(
            desired_size,
            std::min(desired.x > 0 ? desired_size * width / desired.x : desired_size, height));
    // Bound ImGui's baked-size cache while rounding only downward, so the measured line still fits.
    const float size = std::max(.25f, std::floor(fitted * 4) / 4);
    const auto extent = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    const ImVec2 origin(x, y + (height - extent.y) / 2);
    auto *draw = ImGui::GetWindowDrawList();
    const int first_vertex = draw->VtxBuffer.Size;

    draw->AddText(font, size, origin, color, text);

    if (options_.capture_ui.empty()) {
        return;
    }

    const auto clip_a = ImGui::GetWindowDrawList()->GetClipRectMin(),
               clip_b = ImGui::GetWindowDrawList()->GetClipRectMax();
    ImVec2 ink_a(FLT_MAX, FLT_MAX), ink_b(-FLT_MAX, -FLT_MAX);

    for (int i = first_vertex; i < draw->VtxBuffer.Size; ++i) {
        ink_a = ImMin(ink_a, draw->VtxBuffer[i].pos);
        ink_b = ImMax(ink_b, draw->VtxBuffer[i].pos);
    }

    const bool ink_drawn = draw->VtxBuffer.Size > first_vertex;
    bool covered = true;

    for (const char *p = text; *p;) {
        unsigned int c = 0;
        p += ImTextCharFromUtf8(&c, p, nullptr);
        covered = font->IsGlyphInFont(static_cast<ImWchar>(c)) && covered;
    }

    inspection_["texts"].push_back(
            {{"name", name},
             {"kind", kind},
             {"text", text},
             {"font_role", role == UiFontRole::Emphasis ? "emphasis"
                           : role == UiFontRole::Body   ? "body"
                                                        : "label"},
             {"font_name", font->GetDebugName()},
             {"static_weight", role == UiFontRole::Emphasis ? 500 : 400},
             {"font_size_points", size},
             {"desired_font_size_points", desired_size},
             {"rect_points", {origin.x, origin.y, extent.x, extent.y}},
             {"ink_rect_points",
              ink_drawn ? Json::array({ink_a.x, ink_a.y, ink_b.x - ink_a.x, ink_b.y - ink_a.y})
                        : Json::array()},
             {"allocated_points", {x, y, width, height}},
             {"single_line", std::string_view(text).find('\n') == std::string_view::npos &&
                                     extent.y <= size + .001f},
             {"glyph_coverage", covered},
             {"fully_inside_clip", origin.x >= clip_a.x && origin.y >= clip_a.y &&
                                           origin.x + extent.x <= clip_b.x + .001f &&
                                           origin.y + extent.y <= clip_b.y + .001f},
             {"ink_fully_inside_clip", ink_drawn && ink_a.x >= clip_a.x && ink_a.y >= clip_a.y &&
                                               ink_b.x <= clip_b.x && ink_b.y <= clip_b.y}});
}

void Window::numeric(const char *label, const std::string &name, double divisor, double offset,
                     double minimum, double maximum, double step, bool available,
                     float requested_width, const char *unit) {
    const auto source = value(name);
    // A field that reaches neither guest stays visible but grayed out as a profile limit.
    const bool limited = !field_reaches_guest(view_, name);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = requested_width;
    const float value_x = origin.x;
    const bool compact = screen_scale_ == canonical_ui_scale;
    const auto field_layout = parameters_layout(0, 0, 0, screen_scale_);
    const bool three_digits = section_ == 3 ||
                              (section_ == 0 && name != "odo" && name != "remain_mileage_e" &&
                               name != "remain_mileage_f");
    const float density = physical_density(screen_scale_),
                unit_size = compact ? 11.f : numeric_unit_size_,
                reference_unit_inset = compact ? 3.f + 2.f / density
                                               : (3.f + 4.f / (1920.f / 1336.f)) *
                                                         field_layout.form_scale();
    auto *value_font = ui_text_font(UiFontRole::Body);
    const float unit_width = *unit ? value_font->CalcTextSizeA(unit_size, FLT_MAX, 0, unit).x : 0;
    // Move the previously snapped glyph origin by exactly two physical pixels.
    const float previous_unit_x = value_x + width - reference_unit_inset - unit_width,
                unit_x = compact ? std::floor(previous_unit_x) - 2.f / density : previous_unit_x;
    const float unit_gap = compact ? 3.f - (previous_unit_x - std::floor(previous_unit_x))
                                   : 3.f * field_layout.form_scale(),
                unit_inset = value_x + width - unit_x - unit_width;
    const float input_width = width - (unit_width > 0 ? unit_width + unit_gap + unit_inset : 0),
                horizontal_padding = field_layout.input_padding();
    // Match InputTextEx cursor-follow and fine clipping: text starts after the left padding
    // and can draw through to the input frame's right edge, before the separate unit gap.
    const float text_area = input_width - horizontal_padding;
    std::array<char, 128> canonical_text{};

    if (source && available) {
        std::snprintf(canonical_text.data(), canonical_text.size(), "%.10g",
                      *source / divisor + offset);
    }

    const float text_width =
            value_font->CalcTextSizeA(numeric_font_size_, FLT_MAX, 0, canonical_text.data()).x;
    // Long values scroll in a real InputText viewport; value changes never create a smaller font.
    const float value_font_size = numeric_font_size_;
    ImGui::PushID(name.c_str());
    // AP Hotspot normal field: #373f4a, no border, capsule radius=height/2.
    // OEM opens a separate editor; Voyemu keeps inline editing without changing drafts.
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, numeric_height_ / 2);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(horizontal_padding, (numeric_height_ - value_font_size) / 2));

    for (const auto slot : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive}) {
        ImGui::PushStyleColor(slot, ImVec4(0, 0, 0, 0));
    }

    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(control_palette(light_theme_).text));

    if (*label) {
        caption(name, "numeric_label", label, origin.x + width + field_layout.label_gap(), origin.y,
                numeric_label_width_, numeric_height_, label_font_size_,
                control_ink(source && !limited ? control_palette(light_theme_).text
                                               : control_palette(light_theme_).muted));
    }

    ImGui::BeginDisabled(limited);
    ImGui::PushFont(value_font, value_font_size);
    ImGui::SetCursorScreenPos(ImVec2(value_x, origin.y));
    ImGui::BeginGroup();
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + numeric_height_),
                        control_ink(control_palette(light_theme_).input), numeric_height_ / 2);
    const auto text_draw_flags = draw->Flags;

    if (compact) {
        draw->Flags |= ImDrawListFlags_TextNoPixelSnap;
    }

    const auto complete_group = [&] {
        if (*unit) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                ImGui::IsMouseHoveringRect(ImVec2(origin.x + input_width, origin.y),
                                           ImVec2(origin.x + width, origin.y + numeric_height_))) {
                ImGui::SetKeyboardFocusHere(-1);
            }

            ImGui::SetCursorScreenPos(ImVec2(origin.x + input_width, origin.y));
            ImGui::Dummy(ImVec2(width - input_width, numeric_height_));
            const auto unit_draw_flags = draw->Flags;

            if (compact) {
                draw->Flags |= ImDrawListFlags_TextNoPixelSnap;
            }

            caption(name + "/unit", "unit", unit, unit_x, origin.y, unit_width, numeric_height_,
                    unit_size, control_ink(control_palette(light_theme_).secondary));
            draw->Flags = unit_draw_flags;
        }

        ImGui::EndGroup();
        record_item(name, "numeric");
    };

    if (!source) {
        std::array<char, 4> empty{};
        ImGui::BeginDisabled();
        ImGui::InputTextEx("##value", "—", empty.data(), static_cast<int>(empty.size()),
                           ImVec2(input_width, numeric_height_), ImGuiInputTextFlags_ReadOnly);
        draw->Flags = text_draw_flags;
        complete_group();
        ImGui::EndDisabled();
        ImGui::PopFont();
        ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
        ImGui::PopID();

        return;
    }

    auto &draft = drafts_[name];
    unsigned bits = 0;

    for (const auto &field : view_.catalog) {
        if (field.at("name") == name) {
            bits = field.at("width").get<unsigned>();
            break;
        }
    }

    if (maximum < 0) {
        maximum = ((std::uint64_t{1} << bits) - 1) / divisor + offset;
    }

    if (draft.submitted && *draft.submitted == *source && !draft.active) {
        draft.dirty = false;
        draft.submitted.reset();
        draft.error.clear();
    }

    if (!draft.active && !draft.dirty) {
        const double decoded = *source / divisor + offset;

        if (!available || decoded < minimum || decoded > maximum) {
            draft.text[0] = '\0';
        } else {
            std::snprintf(draft.text.data(), draft.text.size(), "%.10g", decoded);
        }
    }

    const bool invalid = !draft.error.empty() && !draft.active;
    const bool focused = draft.active;

    if (focused) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(control_palette(light_theme_).text));
    }

    const int first_vertex = draw->VtxBuffer.Size;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    ThreeDigitEdit limit{draft.text.data()};
    const auto flags = event_name(name) ? ImGuiInputTextFlags_ReadOnly
                       : three_digits   ? ImGuiInputTextFlags_CallbackEdit |
                                                  ImGuiInputTextFlags_CallbackCharFilter
                                        : ImGuiInputTextFlags_None;
    const bool edited = ImGui::InputTextEx(
            "##value", nullptr, draft.text.data(), static_cast<int>(draft.text.size()),
            ImVec2(input_width, numeric_height_), flags,
            three_digits ? ThreeDigitEdit::callback : nullptr, &limit);
    ImGui::PopStyleColor();
    draw->Flags = text_draw_flags;

    if (edited) {
        draft.dirty = true;
        draft.submitted.reset();
        const auto decoded = parse_number(draft.text.data());
        const double encoded = decoded ? (*decoded - offset) * divisor : -1;
        const bool valid = decoded && *decoded >= minimum && *decoded <= maximum &&
                           std::abs(encoded - std::round(encoded)) < 0.000001 &&
                           std::abs((*decoded - minimum) / step -
                                    std::round((*decoded - minimum) / step)) < 0.000001;

        if (!valid) {
            draft.error = draft.text[0] ? "Диапазон или шаг" : "Введите значение";
        } else {
            const auto raw = static_cast<std::uint64_t>(std::llround(encoded));

            if (!concept_raw_allowed(view_.platform, name, raw)) {
                draft.error = "Ограничение профиля";
            } else if (client_.patch({{name, raw}})) {
                draft.submitted = raw;
                draft.error.clear();
            } else {
                draft.error = "Нет соединения";
            }
        }
    }

    const auto input_a = ImGui::GetItemRectMin(), input_b = ImGui::GetItemRectMax();
    const auto input_id = ImGui::GetItemID();
    const bool input_active = ImGui::IsItemActive();
    ImVec2 ink_a(FLT_MAX, FLT_MAX), ink_b(-FLT_MAX, -FLT_MAX);
    bool ink_drawn = false;

    if (!options_.capture_ui.empty()) {
        for (int i = first_vertex; i < draw->VtxBuffer.Size; ++i) {
            if (draw->VtxBuffer[i].col == ImGui::GetColorU32(ImGuiCol_Text)) {
                ink_a = ImMin(ink_a, draw->VtxBuffer[i].pos);
                ink_b = ImMax(ink_b, draw->VtxBuffer[i].pos);
                ink_drawn = true;
            }
        }
    }

    complete_group();
    ImGui::RenderNavCursor(ImRect(origin, ImVec2(origin.x + width, origin.y + numeric_height_)),
                           input_id, ImGuiNavRenderCursorFlags_Compact, numeric_height_ / 2);

    if (!options_.capture_ui.empty()) {
        auto &item = inspection_["fields"].back();
        item["maximum_digits"] = three_digits ? 3 : 0;
        item["font_name"] = value_font->GetDebugName();
        item["static_weight"] = 400;
        item["font_size_points"] = value_font_size;
        item["canonical_text_width_points"] = text_width;
        item["value_text_area_points"] = text_area;
        item["unit_width_points"] = unit_width;
        item["unit_gap_points"] = unit_gap;
        item["unit_right_inset_points"] = unit_inset;
        item["text_left_padding_points"] = horizontal_padding;
        item["capsule_radius_points"] = numeric_height_ / 2;
        item["normal_border_points"] = 0;
        item["horizontal_scroll_required"] = text_width >= text_area;
        item["input_id"] = input_id;
        item["input_rect_points"] = {input_a.x, input_a.y, input_b.x - input_a.x,
                                     input_b.y - input_a.y};
        item["input_ink_rect_points"] =
                ink_drawn ? Json::array({ink_a.x, ink_a.y, ink_b.x - ink_a.x, ink_b.y - ink_a.y})
                          : Json::array();
        const auto *state = ImGui::GetInputTextState(input_id);
        item["scroll_x_points"] = state ? state->Scroll.x : 0;
    }

    if (invalid) {
        draw->AddRect(origin, ImVec2(origin.x + width, origin.y + numeric_height_),
                      control_ink(control_palette(light_theme_).error), numeric_height_ / 2, 1.f,
                      ImDrawFlags(0));
    }

    if (focused) {
        ImGui::PopStyleColor();
    }

    draft.active = input_active;

    if (draft.active && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        draft.dirty = false;
        draft.submitted.reset();
        draft.error.clear();
        draft.active = false;
    }

    ImGui::PopFont();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
    ImGui::PopID();
}

void Window::checkbox(const char *label, const std::string &name, float width) {
    const bool hazard = name == "hazard";
    const auto left = concept_control(view_, VehicleConcept::turn_left),
               right = concept_control(view_, VehicleConcept::turn_right);
    const auto raw = control_value(view_, hazard ? left : name),
               other = hazard ? control_value(view_, right) : raw;
    const bool delivered = hazard ? field_reaches_guest(view_, left) &&
                                            field_reaches_guest(view_, right)
                                  : field_reaches_guest(view_, name);
    const bool known = raw && other && *raw <= 1 && *other <= 1;
    const bool available = known && delivered;
    const bool checked = known && *raw == 1 && *other == 1;
    const auto origin = ImGui::GetCursorScreenPos();
    const auto ink = toggle_ink(light_theme_);

    ImGui::PushID(name.c_str());
    ImGui::BeginDisabled(!available);
    // Hover never modifies a toggle; InvisibleButton retains release/cancel semantics.
    const auto layout = parameters_layout(closed_width, closed_height, editor_height,
                                          screen_scale_);
    const float column_width = width > 0 ? width : layout.content_column(2, 0).width;
    constexpr float caption_x = 49.f;
    const float caption_width = column_width - caption_x;
    const bool changed = ImGui::InvisibleButton("##toggle", ImVec2(column_width, 26));
    record_item(name, "checkbox");

    if (changed) {
        const auto *binding = field_binding(view_.platform, name);
        auto patch = hazard ? concept_patch(view_.platform, VehicleConcept::turn_left, !checked)
                            : concept_patch(view_.platform, binding->id, !checked);

        if (hazard) {
            patch.update(concept_patch(view_.platform, VehicleConcept::turn_right, !checked));
        }

        client_.patch(patch);
    }

    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 track(origin.x, origin.y + 2.5f), track_end(track.x + 41.5f, track.y + 21);
    const float animated = toggle_animations_[name].value(checked, ImGui::GetTime());
    const float progress = known ? animated : 0;

    draw->AddRectFilled(track, track_end, control_ink(ink.track), 10.5f);

    if (ink.border != 0) {
        draw->AddRect(ImVec2(track.x + .5f, track.y + .5f),
                      ImVec2(track_end.x - .5f, track_end.y - .5f), control_ink(ink.border), 10,
                      ImDrawFlags(0), 1.f);
    }

    if (progress > 0) {
        draw->AddRectFilled(
                ImVec2(track.x, track.y + .5f), ImVec2(track.x + 41, track.y + 20.5f),
                control_ink(alpha_ink(ink.on,
                                      static_cast<unsigned>(255 * std::clamp(progress, 0.f, 1.f)))),
                10);
    }

    const ImVec2 knob(track.x + 11 + 19 * progress, track.y + 10.5f);
    const int first = draw->VtxBuffer.Size;
    draw->AddCircleFilled(knob, ink.knob / 2, IM_COL32(255, 255, 255, 255));
    shade_vertical(draw, first, knob.y - ink.knob / 2, ink.knob, [&](float fraction) {
        return toggle_knob_color(light_theme_, fraction);
    });

    const float label_height = std::max(26.f, checkbox_font_size_);
    caption(name, "checkbox_label", label, origin.x + caption_x, origin.y + (26 - label_height) / 2,
            caption_width, label_height, checkbox_font_size_,
            control_ink(control_palette(light_theme_).text));

    ImGui::RenderNavCursor(ImRect(track, track_end), ImGui::GetItemID(),
                           ImGuiNavRenderCursorFlags_Compact, 10.5f);

    ImGui::EndDisabled();
    ImGui::PopID();
}

void Window::editor() {
    const auto layout = parameters_layout(closed_width, closed_height, editor_height,
                                          screen_scale_);

    ImGui::SetCursorScreenPos(ImVec2(layout.panel.x, layout.tabs.y));
    ImGui::BeginChild("parameters", ImVec2(layout.panel.width, layout.panel.height),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                              ImGuiWindowFlags_NoBackground);
    const std::array<const char *, 5> titles = {tr("Движение"), tr("Свет"), tr("Кузов"), tr("Шины"),
                                                tr("Все")};
    auto *draw = ImGui::GetWindowDrawList();
    auto &font_reference = font_references_[screen_scale_ == 75 ? 1 : screen_scale_ == 100 ? 2 : 0];
    auto *body_font = ui_text_font(UiFontRole::Body);
    auto *navigation_font = tab_font();

    if (font_reference.context != ImGui::GetCurrentContext() || font_reference.body != body_font ||
        font_reference.tab != navigation_font || font_reference.scale != screen_scale_ ||
        font_reference.density != physical_density(screen_scale_)) {
        font_reference = {ImGui::GetCurrentContext(), body_font, navigation_font, screen_scale_,
                          physical_density(screen_scale_)};
        font_reference.tab_size = reference_font_size(
                navigation_font, "BT Music", layout.compact50() ? 31.f : 27.f, screen_scale_);
        font_reference.checkbox_size = reference_font_size(
                body_font, "Welcome & courtesy",
                layout.compact50() ? 38.f / (4832.f / 3840.f) : 28.f, screen_scale_);
        font_reference.input_size = layout.compact50() ? layout.input_font_size()
                                                       : reference_font_size(body_font, "255", 24.f,
                                                                             screen_scale_);
        font_reference.unit_size = layout.compact50() ? 11.f
                                                      : reference_font_size(body_font, "km/h", 18.f,
                                                                            screen_scale_);
        font_reference.tab_ink_bottom = sample_ink_bottom(navigation_font, font_reference.tab_size,
                                                          font_reference.density, "BT Music");
        font_reference.tab_ink_height = sample_ink_height(navigation_font, font_reference.tab_size,
                                                          font_reference.density, "BT Music") *
                                        font_reference.density;
        font_reference.checkbox_ink_height =
                sample_ink_height(body_font, font_reference.checkbox_size, font_reference.density,
                                  "Welcome & courtesy") *
                font_reference.density;
    }

    // Medium identifies navigation/groups; other text uses Regular.
    const float requested_tab_size = font_reference.tab_size;
    ImGui::PushFont(tab_font(), requested_tab_size);
    std::array<float, 5> centers{};
    std::array<float, 5> label_widths{};

    for (std::size_t i = 0; i < titles.size(); ++i) {
        label_widths[i] = std::ceil(
                navigation_font->CalcTextSizeA(requested_tab_size, FLT_MAX, 0, titles[i]).x);
    }

    const auto tab_layout = parameter_tabs(layout, label_widths, section_ == 4);
    const auto &tab_rects = tab_layout.items;
    const float tab_size = std::floor(requested_tab_size * tab_layout.font_scale * 4) / 4;
    ImGui::PopFont();
    ImGui::PushFont(tab_font(), tab_size);

    for (std::size_t i = 0; i < titles.size(); ++i) {
        centers[i] = tab_rects[i].x + tab_rects[i].width / 2;
    }

    const double now = ImGui::GetTime();

    // Language/scale/layout changes relocate without animating across unrelated geometry.
    if (!tab_geometry_initialized_ || centers != tab_centers_) {
        tab_indicator_.snap(centers[section_], now);
        tab_centers_ = centers;
        tab_geometry_initialized_ = true;
    }

    for (std::size_t i = 0; i < titles.size(); ++i) {
        const auto r = tab_rects[i];
        ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
        ImGui::PushID(static_cast<int>(i));

        if (ImGui::InvisibleButton("##tab", ImVec2(r.width, r.height))) {
            section_ = i;
            tab_indicator_.select(centers[i], now);
        }

        ImGui::PopID();
        record_item(titles[i], "tab");
        const auto color = control_ink(section_ == i ? control_palette(light_theme_).accent
                                                     : control_palette(light_theme_).inactive);
        // GetFontSize rounds quarter-point sizes; preserve the measured size used by AddText.
        caption(std::string("tab_") + std::to_string(i), "tab", titles[i], r.x, r.y, r.width,
                r.height, tab_size, color);
    }

    const float center = static_cast<float>(tab_indicator_.position(now));
    const float oem_scale = guest_scale(screen_scale_) / physical_density(screen_scale_);
    const float reference_bottom =
            std::floor(tab_rects.front().y + (tab_rects.front().height - tab_size) / 2) +
            (tab_size == font_reference.tab_size
                     ? font_reference.tab_ink_bottom
                     : sample_ink_bottom(tab_font(), tab_size, font_reference.density, "BT Music"));
    const float indicator_width = 42 * oem_scale, indicator_height = 6 * oem_scale,
                indicator_y = reference_bottom + 23 * oem_scale;
    draw->AddRectFilled(ImVec2(center - indicator_width / 2, indicator_y),
                        ImVec2(center + indicator_width / 2, indicator_y + indicator_height),
                        control_ink(control_palette(light_theme_).accent), indicator_height / 2);

    if (!options_.capture_ui.empty()) {
        inspection_["tab_reference"] = {
                {"font_size_points", tab_size},
                {"original_reference_ink_pixels", 27 * guest_scale(screen_scale_)},
                {"requested_delta_physical_pixels", layout.compact50() ? 2 : 0},
                {"reference_ink_pixels",
                 27 * guest_scale(screen_scale_) + (layout.compact50() ? 2 : 0)},
                {"sample_ink_pixels",
                 tab_size == font_reference.tab_size
                         ? font_reference.tab_ink_height
                         : sample_ink_height(tab_font(), tab_size, font_reference.density,
                                             "BT Music") *
                                   font_reference.density},
                {"sample_ink_bottom_points", reference_bottom},
                {"underline_rect_points",
                 {center - indicator_width / 2, indicator_y, indicator_width, indicator_height}},
                {"ink_gap_points", 23 * oem_scale},
                {"physical_density", physical_density(screen_scale_)}};
    }

    ImGui::PopFont();

    if (section_ == 4) {
        ImGui::SetCursorScreenPos(ImVec2(tab_layout.search.x, tab_layout.search.y));
        ImGui::SetNextItemWidth(tab_layout.search.width);
        ImGui::PushFont(ui_text_font(UiFontRole::Body), font_reference.input_size);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, tab_layout.search.height / 2);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(layout.input_padding(),
                                   (tab_layout.search.height - ImGui::GetFontSize()) / 2));

        for (const auto slot :
             {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive}) {
            ImGui::PushStyleColor(
                    slot, ImGui::ColorConvertU32ToFloat4(control_palette(light_theme_).input));
        }

        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(control_palette(light_theme_).text));
        ImGui::PushStyleColor(
                ImGuiCol_TextDisabled,
                ImGui::ColorConvertU32ToFloat4(control_palette(light_theme_).secondary));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        const auto search_draw_flags = draw->Flags;

        if (layout.compact50()) {
            draw->Flags |= ImDrawListFlags_TextNoPixelSnap;
        }

        ImGui::InputTextEx("##search", tr("Поиск"), search_.data(),
                           static_cast<int>(search_.size()),
                           ImVec2(tab_layout.search.width, tab_layout.search.height),
                           ImGuiInputTextFlags_None);
        ImGui::PopStyleColor();
        draw->Flags = search_draw_flags;
        ImGui::RenderNavCursor(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()),
                               ImGui::GetItemID(), ImGuiNavRenderCursorFlags_Compact,
                               tab_layout.search.height / 2);
        record_item("raw_search", "search");
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);
        ImGui::PopFont();
    }

    auto position = [&](std::size_t column, std::size_t row) {
        const auto r = layout.movement_row(column, row);
        ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
    };
    numeric_height_ = layout.input_height();
    numeric_font_size_ = font_reference.input_size;
    numeric_unit_size_ = font_reference.unit_size;
    const float desired_label_size = layout.compact50() ? layout.label_font_size()
                                                        : font_reference.checkbox_size;
    label_font_size_ = desired_label_size;

    if (!options_.capture_ui.empty()) {
        inspection_["input_reference"] = {
                {"original_ink_pixels", 24},
                {"sample_ink_pixels", sample_ink_height(body_font, numeric_font_size_,
                                                        physical_density(screen_scale_), "255") *
                                              physical_density(screen_scale_)},
                {"approved_50_override", layout.compact50()}};
    }

    const float movement_width = layout.movement_value_width();
    numeric_label_width_ = layout.content_column(2, 0).width - movement_width - layout.label_gap();

    // One measured size per movement column keeps a consistent baseline across all labels.
    for (const auto *label :
         {tr("Скорость"), tr("Заряд"), tr("Запас EV"), tr("Запас ДВС"), tr("Пробег"), tr("Топливо"),
          tr("Мощность"), tr("Лимит"), tr("Снаружи")}) {
        const float measured = ui_text_font(UiFontRole::Label)
                                       ->CalcTextSizeA(desired_label_size, FLT_MAX, 0, label)
                                       .x;

        if (measured > 0) {
            label_font_size_ = std::min(label_font_size_,
                                        desired_label_size * numeric_label_width_ / measured);
        }
    }

    if (layout.scaled_form()) {
        const float switch_caption_width = layout.content_column(2, 0).width - 49;

        for (const auto *label : {tr("Габариты"), tr("Дневные огни"), tr("Ближний"), tr("Дальний"),
                                  tr("Туман сзади"), tr("Аварийка"), tr("Капот"), tr("Багажник")}) {
            const float measured = ui_text_font(UiFontRole::Label)
                                           ->CalcTextSizeA(desired_label_size, FLT_MAX, 0, label)
                                           .x;

            if (measured > 0) {
                label_font_size_ = std::min(label_font_size_,
                                            desired_label_size * switch_caption_width / measured);
            }
        }
    }

    label_font_size_ = std::floor(label_font_size_ * 4) / 4;
    checkbox_font_size_ = label_font_size_;
    heading_font_size_ = label_font_size_;

    if (!options_.capture_ui.empty()) {
        inspection_["checkbox_reference"] = {
                {"font_size_points", checkbox_font_size_},
                {"heading_size_points", heading_font_size_},
                {"basis", layout.compact50() ? "movement-labels" : "OEM Seat"},
                {"reference_ink_pixels", (layout.compact50() ? 38.f / (4832.f / 3840.f) : 28.f) *
                                                 guest_scale(screen_scale_)},
                {"sample_ink_pixels", font_reference.checkbox_ink_height}};
    }

    ImGui::BeginDisabled(!view_.ready);

    if (section_ == 0) {
        position(0, 0);
        // The speed and the charge in the encoding of the field of the platform.
        const auto numeric_concept = [&](const char *label, VehicleConcept id, double maximum,
                                         const char *unit) {
            const auto *binding = concept_binding(view_.platform, id);
            const auto encoding = binding ? binding->fields.front().encoding : ConceptEncoding{};
            numeric(label, concept_control(view_, id), 1 / encoding.resolution, encoding.offset, 0,
                    maximum, 1, true, movement_width, unit);
        };
        numeric_concept(tr("Скорость"), VehicleConcept::speed, 255, tr("км/ч"));
        position(0, 1);
        numeric_concept(tr("Заряд"), VehicleConcept::charge, 100, "%");
        const auto column = wheel_layout();
        const auto radio = gear_radio(layout, column.choice_cell);
        const auto gear_field = concept_control(view_, VehicleConcept::gear);
        const auto gear = control_value(view_, gear_field),
                   valid = control_value(view_, concept_control(view_, VehicleConcept::gear_valid));
        const bool gear_known = gear && valid && *valid == 1 && *gear >= 0 && *gear < 4;

        if (!options_.capture_ui.empty()) {
            inspection_["radio_reference"] = {{"height_points", radio.height},
                                              {"font_size_points", column.choice_font},
                                              {"source_pixel_scale", radio.height / 70},
                                              {"source_height_pixels", 80},
                                              {"source_font_pixels", 32},
                                              {"normal_border_points", 0},
                                              {"animation_seconds", MusicTabIndicator::duration}};
        }

        ImGui::BeginDisabled(!field_reaches_guest(view_, gear_field));

        if (const int picked = choice("gear", radio, column.choice_cell, column.choice_font,
                                      gear_labels, gear_known ? int(*gear) : -1, gear_indicator_);
            picked >= 0) {
            auto patch = concept_patch(view_.platform, VehicleConcept::gear, picked);
            patch.update(concept_patch(view_.platform, VehicleConcept::gear_valid, 1));
            client_.patch(patch);
        }

        ImGui::EndDisabled();

        position(0, 2);
        numeric(tr("Запас EV"), "remain_mileage_e", 1, 0, 0, 2046, 1, true, movement_width,
                tr("км"));
        position(0, 3);
        numeric(tr("Запас ДВС"), "remain_mileage_f", 1, 0, 0, 1022, 1, true, movement_width,
                tr("км"));
        position(1, 0);
        numeric(tr("Пробег"), "odo", 1, 0, 0, -1, 1, true, movement_width, tr("км"));
        position(1, 1);
        numeric(tr("Топливо"), "fuel_percent", 1, 0, 0, 100, 1, true, movement_width, "%");
        position(1, 2);
        numeric(tr("Мощность"), "actual_power", 1, 0, 0, 100, 1, true, movement_width, "%");
        position(1, 3);
        numeric(tr("Лимит"), "allowed_power", 1, 0, 0, 100, 1, true, movement_width, "%");
        position(1, 4);
        numeric(tr("Снаружи"), "ambient_temp", 2, -40, -40, 87.5, .5, true, movement_width, "°C");
    } else if (section_ == 2) {
        for (std::size_t i = 0; i < 2; ++i) {
            const auto c = layout.checkbox_column(i);
            ImGui::SetCursorScreenPos(ImVec2(c.x, layout.body.y + 1));
            checkbox(i ? tr("Багажник") : tr("Капот"),
                     concept_control(view_, i ? VehicleConcept::tailgate : VehicleConcept::hood));
        }

        const float heading_y = layout.body.y + 36 * layout.form_scale() +
                                layout.group_margin_top(),
                    heading_height = heading_font_size_ + 4 * layout.form_scale();
        caption("doors", "group", tr("Двери"), layout.body.x + layout.content_inset(), heading_y,
                layout.body.width - layout.content_inset(), heading_height, heading_font_size_,
                control_ink(control_palette(light_theme_).text));
        const float first_y = heading_y + heading_height + 6 * layout.form_scale(),
                    last_y = layout.body.bottom() - std::max(26.f, layout.label_font_size());

        if (layout.scaled_form()) {
            for (std::size_t i = 0; i < 2; ++i) {
                const auto c = layout.checkbox_column(i);
                caption(i ? "doors_right" : "doors_left", "column_heading",
                        i ? tr("Правые") : tr("Левые"), c.x + 49, first_y, c.width - 49,
                        heading_height, heading_font_size_,
                        control_ink(control_palette(light_theme_).text));
            }

            const std::array<const char *, 4> labels = {tr("Передняя"), tr("Передняя"),
                                                        tr("Задняя"), tr("Задняя")};
            const std::array<std::string, 4> names = {
                    concept_control(view_, VehicleConcept::door_front_left),
                    concept_control(view_, VehicleConcept::door_front_right),
                    concept_control(view_, VehicleConcept::door_rear_left),
                    concept_control(view_, VehicleConcept::door_rear_right)};

            for (std::size_t i = 0; i < 4; ++i) {
                const auto c = layout.checkbox_column(i % 2);
                const std::size_t row = i / 2;
                ImGui::SetCursorScreenPos(
                        ImVec2(c.x, first_y + heading_height + 6 * layout.form_scale() +
                                            static_cast<float>(row) * 34 * layout.form_scale()));
                checkbox(labels[i], names[i], c.width);
            }
        } else {
            const std::array<const char *, 4> labels = {tr("Передняя левая"), tr("Задняя левая"),
                                                        tr("Передняя правая"), tr("Задняя правая")};
            const std::array<std::string, 4> names = {
                    concept_control(view_, VehicleConcept::door_front_left),
                    concept_control(view_, VehicleConcept::door_rear_left),
                    concept_control(view_, VehicleConcept::door_front_right),
                    concept_control(view_, VehicleConcept::door_rear_right)};

            for (std::size_t i = 0; i < 4; ++i) {
                ImGui::SetCursorScreenPos(
                        ImVec2(layout.body.x, first_y + (last_y - first_y) * i / 3));
                checkbox(labels[i], names[i], layout.body.width);
            }
        }
    } else if (section_ == 1) {
        if (layout.scaled_form()) {
            const auto c = layout.content_column(1, 0);
            const float y = layout.body.y + 1;
            const std::array<const char *, 6> labels = {tr("Габариты"),    tr("Дневные огни"),
                                                        tr("Ближний"),     tr("Дальний"),
                                                        tr("Туман сзади"), tr("Аварийка")};
            const std::array<std::string, 6> names = {
                    concept_control(view_, VehicleConcept::position_lamp),
                    concept_control(view_, VehicleConcept::daytime_lamp),
                    concept_control(view_, VehicleConcept::low_beam),
                    concept_control(view_, VehicleConcept::high_beam),
                    concept_control(view_, VehicleConcept::rear_fog),
                    "hazard"};

            for (std::size_t i = 0; i < 6; ++i) {
                const auto pair = layout.checkbox_column(i % 2);
                const std::size_t row = i / 2;
                ImGui::SetCursorScreenPos(
                        ImVec2(pair.x, y + static_cast<float>(row) * 34 * layout.form_scale()));
                checkbox(labels[i], names[i], pair.width);
            }

            const float heading_y = y + 102 * layout.form_scale() + layout.group_margin_top(),
                        heading_height = heading_font_size_ + 4 * layout.form_scale();
            caption("indicators", "group", tr("Поворотник"), c.x, heading_y, c.width,
                    heading_height, heading_font_size_,
                    control_ink(control_palette(light_theme_).text));

            for (std::size_t i = 0; i < 2; ++i) {
                const auto pair = layout.checkbox_column(i);
                ImGui::SetCursorScreenPos(
                        ImVec2(pair.x, heading_y + heading_height + 6 * layout.form_scale()));
                checkbox(i ? tr("Правый") : tr("Левый"),
                         concept_control(view_, i ? VehicleConcept::turn_right
                                                  : VehicleConcept::turn_left),
                         pair.width);
            }
        } else {
            const float heading_height = layout.label_font_size() + 4,
                        gap = (layout.body.height - 6 * 26 - heading_height - 2) / 6;
            const std::array<const char *, 5> labels = {tr("Габариты"), tr("Ближний"),
                                                        tr("Дальний"), tr("Дневные огни"),
                                                        tr("Туман сзади")};
            const std::array<std::string, 5> names = {
                    concept_control(view_, VehicleConcept::position_lamp),
                    concept_control(view_, VehicleConcept::low_beam),
                    concept_control(view_, VehicleConcept::high_beam),
                    concept_control(view_, VehicleConcept::daytime_lamp),
                    concept_control(view_, VehicleConcept::rear_fog)};

            for (std::size_t i = 0; i < 5; ++i) {
                ImGui::SetCursorScreenPos(
                        ImVec2(layout.body.x, layout.body.y + 1 + i * (26 + gap)));
                checkbox(labels[i], names[i], layout.body.width);
            }

            const float heading_y = layout.body.y + 1 + 5 * (26 + gap);
            caption("indicators", "group", tr("Поворотник"), layout.body.x, heading_y,
                    layout.body.width, heading_height, layout.label_font_size(),
                    control_ink(control_palette(light_theme_).text));

            for (std::size_t i = 0; i < 2; ++i) {
                const auto c = layout.column(2, i);
                ImGui::SetCursorScreenPos(ImVec2(c.x, heading_y + heading_height + gap));
                checkbox(i ? tr("Правый") : tr("Левый"),
                         concept_control(view_, i ? VehicleConcept::turn_right
                                                  : VehicleConcept::turn_left));
            }
        }
    } else if (section_ == 3) {
        const std::array<const char *, 4> suffix = {"LF", "RF", "LR", "RR"},
                                          labels = {tr("Передняя левая"), tr("Передняя правая"),
                                                    tr("Задняя левая"), tr("Задняя правая")};

        for (std::size_t i = 0; i < 4; ++i) {
            const auto column = layout.tyre_column(i % 2);
            const std::size_t row = i / 2;
            const float x = column.x,
                        y = layout.body.y + layout.first_heading_offset() +
                            static_cast<float>(row) * layout.tyre_group_pitch(heading_font_size_);
            const std::string s = suffix[i], a = "bcm_tpms" + s + "Sts",
                              b = "can_v1_5_bcm_tpms" + s + "Sts";
            const auto av = value(a), bv = value(b);
            bool known = av && bv && *av == 1 && *bv == 1;
            caption("tyre_" + s, "group", labels[i], x, y, column.width,
                    heading_font_size_ + 4 * layout.form_scale(), heading_font_size_,
                    ImGui::GetColorU32(ImGuiCol_Text));
            const float field_width = std::min(layout.movement_value_width(),
                                               (column.width - 8 * layout.form_scale()) / 2),
                        field_y = y + heading_font_size_ + 10 * layout.form_scale();
            ImGui::SetCursorScreenPos(ImVec2(x, field_y));
            numeric("", "bcm_tyre" + s + "Pressure", 10, 0, 0, 7, .1, known, field_width,
                    tr("бар"));
            ImGui::SetCursorScreenPos(ImVec2(x + field_width + 8 * layout.form_scale(), field_y));
            numeric("", "bcm_tyre" + s + "Temp", 1, -61, -61, 194, 1, known, field_width, "°C");
        }
    } else {
        ImGui::SetCursorScreenPos(ImVec2(tab_layout.content.x, tab_layout.content.y));
        const auto query = lower(search_.data());
        std::vector<const Json *> visible;

        for (const auto &field : view_.catalog) {
            if (lower(field.at("name").get<std::string>()).find(query) != std::string::npos) {
                visible.push_back(&field);
            }
        }

        const float raw_width = layout.raw_input_width();
        const float line_height = std::max(
                numeric_height_, numeric_font_size_ + 2 * ImGui::GetStyle().FramePadding.y);
        const float row_height =
                layout.compact50()
                        ? std::floor(line_height + 8 - 1 / physical_density(screen_scale_)) -
                                  2 / physical_density(screen_scale_)
                        : numeric_height_ + 8 * layout.form_scale();
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.f, (row_height - line_height) / 2));
        ImGui::PushFont(ui_text_font(UiFontRole::Body),
                        layout.compact50() ? numeric_font_size_ : label_font_size_);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));

        if (ImGui::BeginTable("signals", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                              ImVec2(tab_layout.content.width, tab_layout.content.height))) {
            ImGui::TableSetupColumn(tr("Значение"), ImGuiTableColumnFlags_WidthFixed,
                                    layout.compact50() ? raw_width : std::ceil(raw_width + 1));
            ImGui::TableSetupColumn(tr("Сигнал"), ImGuiTableColumnFlags_WidthStretch);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible.size()), row_height);

            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto &field = *visible[i];
                    const std::string name = field.at("name");
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
                    ImGui::TableNextColumn();
                    auto raw_origin = ImGui::GetCursorScreenPos();

                    if (!layout.compact50()) {
                        raw_origin.x = tab_layout.content.x;
                        ImGui::SetCursorScreenPos(raw_origin);
                    }

                    numeric("", name, 1, 0, 0, -1, 1, true, raw_width);
                    ImGui::TableNextColumn();

                    if (layout.compact50()) {
                        ImGui::AlignTextToFramePadding();
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
                    } else {
                        ImGui::SetCursorScreenPos(ImVec2(
                                raw_origin.x + raw_width + layout.label_gap(),
                                raw_origin.y + (numeric_height_ - ImGui::GetFontSize()) / 2));
                    }

                    ImGui::TextUnformatted(name.c_str());
                }
            }

            ImGui::EndTable();
        }

        ImGui::PopStyleColor(2);
        ImGui::PopFont();
        ImGui::PopStyleVar();
    }

    ImGui::EndDisabled();
    ImGui::EndChild();
}

bool Window::refresh_state() {
    const bool changed = client_.copy_state(view_, view_version_);

    if (changed) {
        const auto appearance = view_.deliveries.value("android", Json::object())
                                        .value("appearance", Json::object());
        const auto scheme = appearance.value("scheme", std::string{});

        if (appearance.value("status", std::string{}) == "observed" &&
            (scheme == "light" || scheme == "dark")) {
            light_theme_ = scheme == "light";
        }
    }

    return changed;
}

bool Window::cluster_active() const {
    const auto qnx = view_.deliveries.value("qnx", Json::object());
    const auto status = qnx.value("status", std::string{});

    return status == "booting" || status == "graphics_running";
}

std::uint64_t Window::cluster_generation() const {
    return view_.deliveries.value("qnx", Json::object()).value("generation", std::uint64_t{0});
}

bool Window::frame_presentable(std::uint64_t timestamp_ns) {
    if (!options_.replay_frame.empty()) {
        return true; // Explicit developer replay has no live epoch.
    }

    const auto generation = cluster_generation();

    if (!cluster_active() || !generation) {
        frame_gate_open_ = false;
        frame_min_timestamp_ = 0;
        frame_generation_ = generation;

        return false;
    }

    if (frame_generation_ != generation) {
        frame_generation_ = generation;
        frame_gate_open_ = false;
        frame_min_timestamp_ = 0;
    }

    if (!view_.ready || !timestamp_ns) {
        return false;
    }

    if (frame_gate_open_) {
        return timestamp_ns > frame_min_timestamp_;
    }

    const auto &qnx = view_.deliveries.at("qnx");
    const auto found = qnx.find("presentation");

    if (found == qnx.end() || !found->is_object()) {
        return false;
    }

    const auto &fence = *found;

    if (!fence.contains("status") || fence.at("status") != "vehicle_acknowledged") {
        return false;
    }

    for (const char *field : {"generation", "vehicle_revision", "after_timestamp_ns"}) {
        if (!fence.contains(field) || !fence.at(field).is_number_integer()) {
            return false;
        }

        if (fence.at(field).is_number_integer() && !fence.at(field).is_number_unsigned() &&
            fence.at(field).get<std::int64_t>() < 0) {
            return false;
        }
    }

    const auto minimum = fence.at("after_timestamp_ns").get<std::uint64_t>();

    if (fence.at("generation").get<std::uint64_t>() != generation ||
        fence.at("vehicle_revision").get<std::uint64_t>() != view_.revision || !minimum ||
        timestamp_ns <= minimum) {
        return false;
    }

    frame_min_timestamp_ = minimum;
    frame_gate_open_ = true;
    logging::write(logging::Level::info, "frontend", "qnx.first_frame.accepted",
                   {{"generation", generation},
                    {"vehicle_revision", view_.revision},
                    {"timestamp_ns", timestamp_ns},
                    {"ack_fence_ns", minimum}});

    return true;
}

// Logo SVGs are rasterized at the exact framebuffer size: the theme layer is tinted, the accent
// layer keeps its colour.
void Window::artwork(ui::Resource id, const ParameterRect &bounds, std::uint32_t color) {
    const auto &io = ImGui::GetIO();
    const int width = std::max(1, int(std::lround(bounds.width * io.DisplayFramebufferScale.x))),
              height = std::max(1, int(std::lround(bounds.height * io.DisplayFramebufferScale.y)));

    if (artwork_context_ != ImGui::GetCurrentContext()) {
        artwork_textures_.clear();
        artwork_context_ = ImGui::GetCurrentContext();
    }

    auto &texture = artwork_textures_[{int(id), width, height}];

    if (!texture) {
        const auto pixels = ui::rasterize_artwork(vector_artwork(id), width, height);
        texture = std::make_unique<ImTextureData>();
        texture->Create(ImTextureFormat_RGBA32, width, 2 * height + ui::artwork_layer_gap);
        std::copy(pixels.begin(), pixels.end(), static_cast<unsigned char *>(texture->GetPixels()));
        texture->UseColors = true;
        ImGui::RegisterUserTexture(texture.get());
    }

    const float total = float(2 * height + ui::artwork_layer_gap);
    const ImVec2 a(bounds.x, bounds.y), b(bounds.right(), bounds.bottom());
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddImage(texture->GetTexRef(), a, b, ImVec2(0, 0), ImVec2(1, height / total), color);
    draw->AddImage(texture->GetTexRef(), a, b, ImVec2(0, (height + ui::artwork_layer_gap) / total),
                   ImVec2(1, 1), IM_COL32(255, 255, 255, 255));
}

// The one segmented radio of the gear, scale and language choices: track, selected capsule,
// animation, hit regions and bold captions. Without a selection (-1) only the track and captions
// are drawn.
int Window::choice(const char *id, const ParameterRect &bounds, float cell, float font,
                   std::span<const char *const> labels, int selected,
                   MusicTabIndicator &indicator) {
    auto *draw = ImGui::GetWindowDrawList();
    const auto &palette = control_palette(light_theme_);
    const double now = ImGui::GetTime();

    // Segment coordinates preserve the transition when changing the window scale.
    if (selected >= 0) {
        indicator.select(selected, now);
    }

    const float selection_left = bounds.x + float(indicator.position(now)) * cell;

    if (light_theme_ && selected >= 0) {
        const auto clip = radio_background_clip(bounds, selection_left, cell);
        draw->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.right(), clip.bottom()), true);
    }

    draw->AddRectFilled(ImVec2(bounds.x, bounds.y), ImVec2(bounds.right(), bounds.bottom()),
                        control_ink(palette.input), bounds.height / 2);

    if (light_theme_ && selected >= 0) {
        draw->PopClipRect();
    }

    if (selected >= 0) {
        // The clip reserves the animated envelope, including interrupted edge transitions.
        const float padding = MusicTabIndicator::animation_padding(bounds.width - cell);
        draw->PushClipRect(
                ImVec2(std::floor(bounds.x - padding), std::floor(bounds.y - 1)),
                ImVec2(std::ceil(bounds.right() + padding), std::ceil(bounds.bottom() + 1)), true);
        const int first = draw->VtxBuffer.Size;
        draw->AddRectFilled(ImVec2(selection_left, bounds.y),
                            ImVec2(selection_left + cell, bounds.bottom()),
                            IM_COL32(255, 255, 255, 255), bounds.height / 2);
        shade_vertical(draw, first, bounds.y, bounds.height, [&](float fraction) {
            return radio_selection_color(light_theme_, fraction);
        });
        draw->PopClipRect();
    }

    int picked = -1;
    ImGui::PushFont(ui_text_font(UiFontRole::Choice), font);
    ImGui::PushID(id);

    for (std::size_t i = 0; i < labels.size(); ++i) {
        const ImVec2 a(bounds.x + i * cell, bounds.y), b(a.x + cell, bounds.bottom());
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(static_cast<int>(i));

        if (ImGui::InvisibleButton("##choice", ImVec2(cell, bounds.height),
                                   ImGuiButtonFlags_NoNavFocus)) {
            picked = static_cast<int>(i);
        }

        std::string key(labels[i]);
        std::erase(key, '%');
        record_item(std::string(id) + "_" + key, "choice");
        const float center = a.x + cell / 2;
        const bool chosen = int(i) == selected && selection_left + cell >= center - cell * .4f &&
                            selection_left <= center + cell * .4f;
        centered_caption(draw, a, ImVec2(cell, bounds.height),
                         control_ink(chosen         ? IM_COL32(255, 255, 255, 255)
                                     : light_theme_ ? palette.text
                                                    : palette.inactive),
                         labels[i]);
        ImGui::RenderNavCursor(ImRect(a, b), ImGui::GetItemID(), ImGuiNavRenderCursorFlags_Compact,
                               bounds.height / 2);
        ImGui::PopID();
    }

    ImGui::PopID();
    ImGui::PopFont();

    return picked;
}

// One caption for the screens window: the rear screen, the HUD or several screens.
const char *Window::screens_label() const {
    if (screens_.empty()) {
        return nullptr;
    }

    return screens_.size() > 1          ? tr("Экраны")
           : screens_.front() == "rear" ? tr("Задний экран")
                                        : tr("HUD");
}

// The screens window toggle beside the multimedia power button: its icon takes the primary ink
// while the window is open and the inactive ink while it is closed, like the lifecycle glyphs.
void Window::screens_toggle(const ParameterRect &bounds) {
    ImGui::SetCursorScreenPos(ImVec2(bounds.x, bounds.y));

    if (ImGui::InvisibleButton("##screens", ImVec2(bounds.width, bounds.height),
                               ImGuiButtonFlags_NoNavFocus)) {
        shell_request_.screens_window = !screens_window_;
    }

    const bool hovered = ImGui::IsItemHovered();
    record_item("screens", "screens");

    if (!options_.capture_ui.empty()) {
        inspection_["fields"].back()["selected"] = screens_window_;
        inspection_["fields"].back()["label"] = screens_label();
    }

    // The icon keeps its place on the 28-unit grid of the lifecycle glyphs.
    const auto &icon = vector_artwork(ui::Resource::ScreensIcon);
    const float unit = bounds.width / 28;
    artwork(ui::Resource::ScreensIcon,
            {bounds.x + icon.x * unit, bounds.y + icon.y * unit, icon.width * unit,
             icon.height * unit},
            glyph_ink(light_theme_, hovered, screens_window_));
    ImGui::RenderNavCursor(
            ImRect(ImVec2(bounds.x, bounds.y), ImVec2(bounds.right(), bounds.bottom())),
            ImGui::GetItemID());
}

// All radios share one segment: the widest caption of the gear, scale and language radios.
WheelColumn Window::wheel_layout() const {
    float widest = 0;
    auto *label_font = ui_text_font(UiFontRole::Choice);
    const auto measure = [&](const char *label) {
        widest = std::max(widest,
                          label_font->CalcTextSizeA(wheel_choice_font, FLT_MAX, 0, label).x);
    };

    for (const auto *label : gear_labels) {
        measure(label);
    }

    for (const auto *label : scale_labels) {
        measure(label);
    }

    for (const auto language : ui::language_choices) {
        measure(ui::language_choice_label(language));
    }

    return wheel_column(screen_scale_, wheel_ink_left_, wheel_ink_right_, joined_offset_, widest);
}

void Window::wheel_controls() {
    auto *draw = ImGui::GetWindowDrawList();
    std::array<const char *, 3> languages{};
    int language = 0;

    for (std::size_t i = 0; i < languages.size(); ++i) {
        languages[i] = ui::language_choice_label(ui::language_choices[i]);

        if (ui::language_choices[i] == ui::current_language) {
            language = static_cast<int>(i);
        }
    }

    const auto column = wheel_layout();
    {
        const auto &bounds = column.wordmark;
        ImGui::SetCursorScreenPos(ImVec2(bounds.x, bounds.y));

        if (ImGui::InvisibleButton("##about", ImVec2(bounds.width, bounds.height))) {
            about_open_ = true;
            about_license_ = 0;
            about_scroll_reset_ = true;
        }

        record_item("about", "about");
        // The primary ink of the powered glyphs; the pointer changes only the cursor.
        artwork(ui::Resource::LogoText, bounds,
                control_ink(light_theme_ ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255)));
        ImGui::RenderNavCursor(
                ImRect(ImVec2(bounds.x, bounds.y), ImVec2(bounds.right(), bounds.bottom())),
                ImGui::GetItemID());
    }
    const auto qnx = view_.deliveries.value("qnx", Json::object());
    const auto android = view_.deliveries.value("android", Json::object());
    const auto lifecycle = [&](const char *id, const ParameterRect &box, bool restart,
                               bool multimedia) {
        const bool active = multimedia ? android.value("powered", true) : cluster_active();
        const bool available = view_.ready &&
                               (multimedia ? android : qnx).value("lifecycle_supported", false);
        const auto center = ImVec2(box.x + box.width / 2, box.y + box.height / 2);
        const float glyph_scale = box.width / 28.f, radius = column.button_hit_radius;
        const ParameterRect bounds{center.x - radius, center.y - radius, 2 * radius, 2 * radius};
        const auto mouse = ImGui::GetIO().MousePos;
        const float dx = mouse.x - center.x, dy = mouse.y - center.y;
        const bool inside_circle = dx * dx + dy * dy <= radius * radius;
        const bool enabled = available && (!restart || active);
        ImGui::SetCursorScreenPos(ImVec2(bounds.x, bounds.y));
        ImGui::BeginDisabled(!enabled);
        const ImRect rect(ImVec2(bounds.x, bounds.y), ImVec2(bounds.right(), bounds.bottom()));
        const auto item_id = ImGui::GetID(id);
        ImGui::ItemSize(rect);
        bool hovered = false, held = false, pressed = false;

        if (ImGui::ItemAdd(rect, item_id, nullptr, ImGuiItemFlags_NoNav)) {
            pressed = ImGui::ButtonBehavior(inside_circle ? rect : ImRect(rect.Min, rect.Min),
                                            item_id, &hovered, &held,
                                            ImGuiButtonFlags_MouseButtonLeft);
        }

        if (pressed) {
            if (multimedia) {
                if (restart) {
                    client_.media_restart();
                } else {
                    client_.media_power(!active);
                }
            } else {
                if (restart) {
                    client_.cluster_restart();
                } else {
                    client_.cluster_power(!active);
                }
            }
        }

        ImGui::RenderNavCursor(rect, item_id);
        record_item(multimedia ? (restart ? "media_restart" : "media_power")
                               : (restart ? "cluster_restart" : "cluster_power"),
                    "lifecycle");

        if (!options_.capture_ui.empty()) {
            auto &item = inspection_["fields"].back();
            item["hit_shape"] = "circle";
            item["hit_radius_points"] = radius;
            item["pointer_hovered"] = hovered;
            item["pointer_held"] = held;
            item["pressed"] = pressed;
            item["powered"] = active;
            item["available"] = available;
        }

        // Original Music Play/Next disabled PNGs: primary ink at77/255 opacity. A running guest
        // shows its power glyph in the primary ink, a stopped one in the inactive ink.
        const auto disabled = light_theme_ ? IM_COL32(0, 0, 0, 77) : IM_COL32(255, 255, 255, 77);
        const auto color = enabled ? glyph_ink(light_theme_, hovered, !restart && active)
                                   : disabled;
        constexpr float pi = 3.14159265358979323846f;
        const float start = restart ? -.7f : -pi / 2 + .7f, end = restart ? 4.9f : 3 * pi / 2 - .7f;
        draw->PathArcTo(center, 6.5f * glyph_scale, start, end, 48);
        const auto tip = draw->_Path.back();
        draw->PathStroke(color, ImDrawFlags_None, 1.4f * glyph_scale);

        if (restart) {
            const ImVec2 tangent(-std::sin(end), std::cos(end)), normal(-tangent.y, tangent.x);
            draw->PathLineTo(ImVec2(tip.x + (-2.5f * tangent.x + 1.6f * normal.x) * glyph_scale,
                                    tip.y + (-2.5f * tangent.y + 1.6f * normal.y) * glyph_scale));
            draw->PathLineTo(tip);
            draw->PathLineTo(ImVec2(tip.x + (-2.5f * tangent.x - 1.6f * normal.x) * glyph_scale,
                                    tip.y + (-2.5f * tangent.y - 1.6f * normal.y) * glyph_scale));
        } else {
            draw->PathLineTo(ImVec2(center.x, center.y - 8 * glyph_scale));
            draw->PathLineTo(ImVec2(center.x, center.y - glyph_scale));
        }

        draw->PathStroke(color, ImDrawFlags_None, 1.4f * glyph_scale);
        ImGui::EndDisabled();
    };
    lifecycle("##power", column.cluster_power, false, false);
    lifecycle("##restart", column.cluster_restart, true, false);
    lifecycle("##media_power", column.media_power, false, true);
    lifecycle("##media_restart", column.media_restart, true, true);
    const auto percent = ui_scale_percent(screen_scale_);
    const int scale_index = percent == 50 ? 0 : percent == 75 ? 1 : 2;

    if (const int picked = choice("scale", column.scale_choice, column.choice_cell,
                                  column.choice_font, scale_labels, scale_index, scale_indicator_);
        picked >= 0 && picked != scale_index) {
        shell_request_.scale_percent = scale_choices[picked];
    }

    if (const int picked = choice("language", column.language_choice, column.choice_cell,
                                  column.choice_font, languages, language, language_indicator_);
        picked >= 0 && picked != language) {
        shell_request_.language = ui::language_choices[picked];
    }

    if (!screens_.empty()) {
        screens_toggle(column.screens_toggle);
    }

    if (!options_.capture_ui.empty()) {
        const auto rect = [](const ParameterRect &r) {
            return Json::array({r.x, r.y, r.width, r.height});
        };
        inspection_["wheel_column"] = {{"scale", column.scale},
                                       {"wordmark", rect(column.wordmark)},
                                       {"scale_choice", rect(column.scale_choice)},
                                       {"language_choice", rect(column.language_choice)},
                                       {"cluster_power", rect(column.cluster_power)},
                                       {"cluster_restart", rect(column.cluster_restart)},
                                       {"media_power", rect(column.media_power)},
                                       {"media_restart", rect(column.media_restart)},
                                       {"screens_toggle", rect(column.screens_toggle)}};
    }
}

// In-window About: product, release date and the licenses embedded in this build.
void Window::about() {
    const auto &io = ImGui::GetIO();
    const auto &palette = control_palette(light_theme_);
    const auto notices = ui::embedded_license_notices();

    about_license_ = std::min(about_license_, notices.size() - 1);
    const auto &notice = notices[about_license_];
    const float f = std::min(36 * guest_scale(screen_scale_), 28.f) /
                    physical_density(screen_scale_),
                control = parameters_layout(closed_width, closed_height, editor_height,
                                            screen_scale_)
                                  .radio_height();
    const ImVec2 display = io.DisplaySize;
    const float width = std::min(44 * f, display.x - 48),
                height = std::min(display.y * .9f, 46 * f);
    const ImVec2 origin(std::floor((display.x - width) / 2), std::floor((display.y - height) / 2)),
            end(origin.x + width, origin.y + height);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::Begin("Voyemu About", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                         ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    if (ImGui::IsWindowAppearing()) {
        ImGui::FocusWindow(ImGui::GetCurrentWindow());
    }

    const float padding = 1.1f * f, title = 2.3f * f;
    const auto rgb = [](unsigned value, unsigned alpha) {
        return IM_COL32(value >> 16 & 0xff, value >> 8 & 0xff, value & 0xff, alpha);
    };
    // The title strip takes the host title bar colours where the host exposes them; the panel
    // border has the colour of the title strip.
    const auto panel = light_theme_ ? palette.surface : palette.background,
               title_background = title_colors_ ? rgb(title_colors_->background, 255) : panel,
               title_ink = title_colors_ ? rgb(title_colors_->text, 255) : palette.text,
               title_idle = title_colors_ ? rgb(title_colors_->text, 150) : palette.inactive;
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(0, 0), display,
                        light_theme_ ? IM_COL32(45, 52, 66, 71) : IM_COL32(0, 0, 0, 128));
    draw->AddRectFilled(origin, end, control_ink(panel), 4);
    draw->AddRect(origin, end, control_ink(title_background), 4);
    // Escape and outside clicks that close the license list leave the panel open.
    bool close = (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !about_list_open_) ||
                 (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                  !ImRect(origin, end).Contains(io.MousePos));

    if (title_colors_) {
        draw->AddRectFilled(origin, ImVec2(end.x, origin.y + title), control_ink(title_background),
                            4, ImDrawFlags_RoundCornersTop);
    }

    // The title has the em of the host title bar font; an ImGui size is the ascent to descent
    // height of the face.
    const float title_size =
            title_font_size_ ? *title_font_size_ * ui_font_height_per_em(UiFontRole::Emphasis) : f;
    ImGui::PushFont(ui_text_font(UiFontRole::Emphasis), title_size);
    draw->AddText(ImVec2(origin.x + padding, origin.y + (title - title_size) / 2),
                  control_ink(title_ink), tr("О программе"));
    ImGui::PopFont();
    {
        const float size = 1.7f * f;
        const ImVec2 a(end.x - padding / 2 - size, origin.y + (title - size) / 2),
                b(a.x + size, a.y + size);
        ImGui::SetCursorScreenPos(a);
        close = ImGui::InvisibleButton("##about_close", ImVec2(size, size)) || close;
        record_item("about_close", "about");
        const bool hovered = ImGui::IsItemHovered();

        if (hovered) {
            draw->AddRectFilled(
                    a, b, control_ink(title_colors_ ? rgb(title_colors_->text, 40) : palette.hover),
                    4);
        }

        const ImVec2 c((a.x + b.x) / 2, (a.y + b.y) / 2);
        const float arm = .2625f * f, stroke = .09f * f;
        const auto ink = control_ink(hovered ? title_ink : title_idle);
        draw->AddLine(ImVec2(c.x - arm, c.y - arm), ImVec2(c.x + arm, c.y + arm), ink, stroke);
        draw->AddLine(ImVec2(c.x + arm, c.y - arm), ImVec2(c.x - arm, c.y + arm), ink, stroke);
        ImGui::RenderNavCursor(ImRect(a, b), ImGui::GetItemID());
    }

    if (!title_colors_) {
        draw->AddRectFilled(ImVec2(origin.x + 1, origin.y + title - .5f),
                            ImVec2(end.x - 1, origin.y + title), control_ink(palette.divider));
    }

    const float left = origin.x + padding, right = end.x - padding, gap = .9f * f;
    float y = origin.y + title + padding;
    {
        // The flat mark is as tall as the two text lines and centred on them.
        const float name = 1.7f * f, name_line = 1.1f * name, date_line = 1.3f * f,
                    spacing = .5f * f, block = name_line + spacing + date_line;
        artwork(ui::Resource::LogoFigure, {left, y, 2 * block, block},
                control_ink(light_theme_ ? IM_COL32(37, 40, 43, 255)
                                         : IM_COL32(255, 255, 255, 255)));
        const float text_x = left + 2 * block + padding;
        ImGui::PushFont(ui_text_font(UiFontRole::Emphasis), name);
        const auto title_text = "Voyemu " + std::string(version());
        draw->AddText(ImVec2(text_x, y + (name_line - name) / 2), control_ink(palette.text),
                      title_text.c_str());
        ImGui::PopFont();
        ImGui::PushFont(ui_text_font(UiFontRole::Body), f);
        const auto release = ui::release_text(release_date());
        draw->AddText(ImVec2(text_x, y + name_line + spacing + (date_line - f) / 2),
                      control_ink(palette.secondary), release.c_str());
        ImGui::PopFont();
        y += block + gap;
    }
    ImGui::PushFont(ui_text_font(UiFontRole::Body), f);
    {
        const char *licenses_label = tr("Лицензии"), *site_label = tr("Сайт");
        const float label_width = ImGui::CalcTextSize(licenses_label).x,
                    site_width = ImGui::CalcTextSize(site_label).x + 1.8f * f;
        draw->AddText(ImVec2(left, y + (control - f) / 2), control_ink(palette.secondary),
                      licenses_label);
        const float combo_x = left + label_width + .8f * f, site_x = right - site_width;
        const float combo_right = site_x - .8f * f, inset = .9f * f, arrow = control;
        // License names end on one vertical in the closed field and in the list.
        const float license_right = combo_right - arrow;
        const ImVec2 combo_a(combo_x, y), combo_b(combo_right, y + control);
        ImGui::SetCursorScreenPos(combo_a);

        if (ImGui::InvisibleButton("##license", ImVec2(combo_b.x - combo_a.x, control))) {
            ImGui::OpenPopup("##license_list");
        }

        record_item("license", "about");
        // The field keeps its background under the pointer, while pressed and while its list is
        // open; the pointer changes only the cursor.
        draw->AddRectFilled(combo_a, combo_b, control_ink(palette.input), control / 2);

        if (light_theme_) {
            draw->AddRect(combo_a, combo_b, control_ink(palette.border), control / 2);
        }

        {
            const auto component = std::string(notices[about_license_].component),
                       license = std::string(notices[about_license_].license);
            const float text_y = y + (control - f) / 2,
                        license_width = ImGui::CalcTextSize(license.c_str()).x;
            draw->PushClipRect(combo_a, ImVec2(license_right - license_width - .5f * f, combo_b.y),
                               true);
            draw->AddText(ImVec2(combo_x + inset, text_y), control_ink(palette.text),
                          component.c_str());
            draw->PopClipRect();
            draw->AddText(ImVec2(license_right - license_width, text_y),
                          control_ink(palette.secondary), license.c_str());
            ImGui::RenderArrow(draw,
                               ImVec2(combo_right - arrow / 2 - .35f * f, y + (control - f) / 2),
                               control_ink(palette.secondary), ImGuiDir_Down, .7f);
        }
        ImGui::RenderNavCursor(ImRect(combo_a, combo_b), ImGui::GetItemID());
        // The list holds every license without a scrollbar, with the same padding above the first
        // row and below the last, on the background of the closed field; it moves up only where the
        // canvas below the field is too short for it. ImGui lays rows out on whole pixels, so the
        // list position, row and padding are whole too.
        const float list_gap = .3f * f, row = std::round(1.6f * f),
                    list_padding = std::round(.4f * f),
                    list_height = static_cast<float>(notices.size()) * row + 2 * list_padding;
        ImGui::SetNextWindowPos(
                ImVec2(combo_x, std::max(0.f, std::floor(std::min(y + control + list_gap,
                                                                  display.y - list_height)))));
        ImGui::SetNextWindowSize(ImVec2(combo_right - combo_x, list_height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, list_padding));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, control / 2);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(palette.input));

        if (ImGui::BeginPopup("##license_list",
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            auto *list = ImGui::GetWindowDrawList();

            for (std::size_t i = 0; i < notices.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const auto component = std::string(notices[i].component),
                           license = std::string(notices[i].license);
                const ImVec2 row_a = ImGui::GetCursorScreenPos();
                // A row keeps its background under the pointer and while pressed.
                const auto row_background = ImGui::GetStyleColorVec4(
                        i == about_license_ ? ImGuiCol_Header : ImGuiCol_PopupBg);
                ImGui::PushStyleColor(ImGuiCol_Header, row_background);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, row_background);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, row_background);

                if (ImGui::Selectable("##row", i == about_license_, 0,
                                      ImVec2(combo_right - combo_x, row))) {
                    about_scroll_reset_ = about_license_ != i;
                    about_license_ = i;
                }

                ImGui::PopStyleColor(3);
                const float text_y = row_a.y + (row - f) / 2,
                            license_width = ImGui::CalcTextSize(license.c_str()).x;
                list->AddText(ImVec2(combo_x + inset, text_y), control_ink(palette.text),
                              component.c_str());
                list->AddText(ImVec2(license_right - license_width, text_y),
                              control_ink(palette.secondary), license.c_str());
                ImGui::PopID();
            }

            ImGui::EndPopup();
        }

        about_list_open_ = ImGui::IsPopupOpen("##license_list");
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        const auto site = ui::license_site(notice, ui::current_language);
        const ImVec2 a(site_x, y), b(right, y + control);
        ImGui::SetCursorScreenPos(a);
        ImGui::BeginDisabled(site.empty());

        if (ImGui::InvisibleButton("##site", ImVec2(b.x - a.x, control))) {
            shell_request_.url = std::string(site);
        }

        record_item("site", "about");
        // Under the pointer only the caption changes, to the primary ink of the wordmark.
        const bool hovered = ImGui::IsItemHovered();
        draw->AddRectFilled(a, b, control_ink(palette.input), control / 2);

        if (light_theme_) {
            draw->AddRect(a, b, control_ink(palette.border), control / 2);
        }

        draw->AddText(ImVec2(a.x + .9f * f, y + (control - f) / 2),
                      control_ink(hovered        ? (light_theme_ ? IM_COL32(0, 0, 0, 255)
                                                                 : IM_COL32(255, 255, 255, 255))
                                  : site.empty() ? palette.secondary
                                  : light_theme_ ? palette.text
                                                 : palette.inactive),
                      site_label);
        ImGui::RenderNavCursor(ImRect(a, b), ImGui::GetItemID());
        ImGui::EndDisabled();
        y += control + gap;
    }
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(left, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    if (ImGui::BeginChild("##license_text", ImVec2(right - left, std::max(f, end.y - padding - y)),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::PushFont(ui_text_font(UiFontRole::Body), 1.08f * f);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(notice.text.data(), notice.text.data() + notice.text.size());
        ImGui::PopTextWrapPos();
        // Wheel steps and trackpad deltas move a target; the view eases toward it each frame.
        const float limit = ImGui::GetScrollMaxY();

        if (about_scroll_reset_) {
            about_scroll_target_ = about_scroll_y_ = 0;
            about_scroll_reset_ = false;
        } else if (std::abs(ImGui::GetScrollY() - about_scroll_y_) > .5f) {
            about_scroll_target_ = about_scroll_y_ = ImGui::GetScrollY(); // scrollbar drag
        }

        if (ImGui::IsWindowHovered() && io.MouseWheel != 0) {
            about_scroll_target_ -= io.MouseWheel * 3 * ImGui::GetTextLineHeightWithSpacing();
        }

        about_scroll_target_ = std::clamp(about_scroll_target_, 0.f, limit);
        const float ease = 1 - std::exp(-io.DeltaTime * 14);
        about_scroll_y_ += (about_scroll_target_ - about_scroll_y_) * ease;

        if (std::abs(about_scroll_target_ - about_scroll_y_) <= .25f) {
            about_scroll_y_ = about_scroll_target_;
        }

        ImGui::SetScrollY(about_scroll_y_);
        ImGui::PopFont();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::End();

    if (close) {
        about_open_ = about_list_open_ = false;
    }
}

void Window::draw(Texture frame, Texture left, Texture right, unsigned screen_scale) {
    refresh_state();
    const ControlStyle control_style(control_palette(light_theme_));
    ImGui::PushFont(ui_text_font(UiFontRole::Body), ImGui::GetFontSize());
    screen_scale_ = screen_scale == canonical_ui_scale || screen_scale == 50 || screen_scale == 75
                            ? screen_scale
                            : 100;
    const auto geometry = view_layout(screen_scale_);

    if (!options_.capture_ui.empty()) {
        inspection_ = {
                {"schema", 1},
                {"source", "actual own ImGui item rectangles and measured draw-list text bounds"},
                {"units", "logical points"},
                {"tabs", Json::array()},
                {"fields", Json::array()},
                {"texts", Json::array()}};
    }

    if (!options_.capture_ui.empty()) {
        inspection_["appearance"] = {{"scheme", light_theme_ ? "light" : "dark"},
                                     {"input_color", control_palette(light_theme_).input},
                                     {"text_color", control_palette(light_theme_).text},
                                     {"tab_selected_color", control_palette(light_theme_).accent},
                                     {"tab_inactive_color", control_palette(light_theme_).inactive},
                                     {"divider_color", control_palette(light_theme_).divider}};
    }

    if (!view_.ready && !held_.empty()) {
        held_.clear();
        client_.release_owner();
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        release("mouse");
    }

    if (ImGui::IsKeyReleased(ImGuiKey_Space) || ImGui::IsKeyReleased(ImGuiKey_Enter)) {
        release("keyboard");
    }

    if (ImGui::GetIO().AppFocusLost && !held_.empty()) {
        focus_lost();
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(geometry.top_width, geometry.top_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("Voyemu Cluster", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                         ImGuiWindowFlags_NoScrollbar);
    ImGui::SetScrollX(0);
    ImGui::SetScrollY(0);
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(0, 0), ImVec2(geometry.top_width, geometry.top_height),
                        control_ink(control_palette(light_theme_).background));
    draw->AddRectFilled(ImVec2(0, 0), ImVec2(geometry.frame.width, geometry.frame.height),
                        IM_COL32(0, 0, 0, 255));

    // One light background spans both control columns.
    if (light_theme_) {
        for (std::size_t i = 1; i < light_background.size(); ++i) {
            const auto &top = light_background[i - 1], &bottom = light_background[i];
            const auto top_ink = control_ink(top.color), bottom_ink = control_ink(bottom.color);
            draw->AddRectFilledMultiColor(
                    ImVec2(geometry.frame.right(), top.fraction * geometry.top_height),
                    ImVec2(geometry.top_width, bottom.fraction * geometry.top_height), top_ink,
                    top_ink, bottom_ink, bottom_ink);
        }
    }

    if (frame.id && frame.width == 1920 && frame.height == 720) {
        draw->AddImage(ImTextureRef(frame.id), ImVec2(0, 0),
                       ImVec2(geometry.frame.width, geometry.frame.height));
    }

    const auto wheel = placed_wheel(screen_scale_, wheel_ink_left_, wheel_ink_right_,
                                    joined_offset_);
    draw->PushClipRect(ImVec2(wheel.x, wheel.y), ImVec2(wheel.right(), wheel.bottom()), true);
    photo(left, "left", wheel.y);
    photo(right, "right", wheel.y);
    draw->PopClipRect();

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        release("mouse");
    }

    if (!ImGui::IsKeyDown(ImGuiKey_Space) && !ImGui::IsKeyDown(ImGuiKey_Enter)) {
        release("keyboard");
    }

    const auto layout = parameters_layout(closed_width, closed_height, editor_height,
                                          screen_scale_);
    wheel_controls();
    editor();

    // Half a logical point is one physical pixel at the native 2x canvas scale. The foreground
    // dividers would cross the About panel.
    if (!about_open_) {
        auto *dividers = ImGui::GetForegroundDrawList();
        dividers->AddRectFilled(ImVec2(geometry.frame.right() - .5f, 0),
                                ImVec2(geometry.frame.right(), geometry.top_height),
                                control_ink(control_palette(light_theme_).divider));
        dividers->AddRectFilled(ImVec2(0, geometry.top_height - .5f),
                                ImVec2(ImGui::GetIO().DisplaySize.x, geometry.top_height),
                                control_ink(control_palette(light_theme_).divider));
        dividers->AddRectFilled(ImVec2(geometry.parameters.x - .5f, 0),
                                ImVec2(geometry.parameters.x, geometry.top_height),
                                control_ink(control_palette(light_theme_).divider));
    }

    if (!options_.capture_ui.empty()) {
        inspection_["screen_scale"] = screen_scale_;
        inspection_["layout"] = {
                {"qnx_rect_points",
                 {geometry.frame.x, geometry.frame.y, geometry.frame.width, geometry.frame.height}},
                {"wheel_rect_points", {wheel.x, wheel.y, wheel.width, wheel.height}},
                {"parameters_rect_points",
                 {geometry.parameters.x, geometry.parameters.y, geometry.parameters.width,
                  geometry.parameters.height}},
                {"top_height_points", geometry.top_height}};
        inspection_["qnx_presentation"] = {{"generation", frame_generation_},
                                           {"gate_open", frame_gate_open_},
                                           {"after_timestamp_ns", frame_min_timestamp_}};
        inspection_["expanded"] = expanded_;
        inspection_["selected_section"] = section_;
        inspection_["catalog_fields"] = view_.catalog.size();
        inspection_["canonical_ready"] = view_.ready;
        auto *font = ImGui::GetFont();
        bool russian = font->IsGlyphInFont(0x0401) && font->IsGlyphInFont(0x0451);

        for (ImWchar c = 0x0410; c <= 0x044f; ++c) {
            russian = font->IsGlyphInFont(c) && russian;
        }

        bool symbols = true;

        for (const ImWchar c : {0x00b0, 0x00b7, 0x2014, 0x2026}) {
            symbols = font->IsGlyphInFont(c) && symbols;
        }

        const float digits = font->CalcTextSizeA(numeric_font_size_, FLT_MAX, 0, "4294967295").x,
                    raw_area = layout.raw_input_width() - layout.input_padding();
        inspection_["font"] = {{"name", font->GetDebugName()},
                               {"size_points", ImGui::GetFontSize()},
                               {"input_size_points", numeric_font_size_},
                               {"label_size_points", label_font_size_},
                               {"tab_size_points", layout.tab_font_size()},
                               {"russian_alphabet_coverage", russian},
                               {"required_symbol_coverage", symbols},
                               {"raw_uint32_text_width_points", digits},
                               {"numeric_text_area_points", raw_area},
                               {"raw_uint32_fits", digits <= raw_area}};
    }

    ImGui::End();

    if (about_open_) {
        about();
    }

    // Clickable items show the pointing hand; text fields keep the I-beam ImGui sets.
    if (ImGui::IsAnyItemHovered() && ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImGui::PopFont();
    ImGui::PopStyleVar(2);
}
} // namespace voyemu::cluster
