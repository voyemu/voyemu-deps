#pragma once
#include <array>
#include <filesystem>

struct ImFont;
namespace voyemu::cluster {
enum class UiFontRole {
    Body,
    Label,
    Emphasis,
    // Captions of the radio choices: scale, language and gear.
    Choice
};
// The weights of the car fonts in the machine; each is a primary face, primary-<weight>.ttf, and a
// fallback face, fallback-<weight>.ttf, merged after it for the glyphs the primary face lacks.
inline constexpr std::array<const char *, 3> ui_font_weights{"regular", "medium", "bold"};
ImFont *ui_text_font(UiFontRole role);
ImFont *tab_font();
// The ascent to descent height of the face of role per rendered em; an ImGui font size is that
// height.
float ui_font_height_per_em(UiFontRole role);
// Loads the car fonts of a machine from its font directory, each face scaled to the cap height of
// the layout; a missing or unreadable face stops with UI_FONT, as only a rebuilt machine carries
// them.
void load_ui_fonts(const std::filesystem::path &directory);
} // namespace voyemu::cluster
