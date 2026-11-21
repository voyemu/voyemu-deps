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
// The weights of the car fonts in the machine; each is a Latin and Cyrillic face,
// latin-<weight>.ttf, and a Chinese face, cjk-<weight>.ttf, merged after it.
inline constexpr std::array<const char *, 3> ui_font_weights{"regular", "medium", "bold"};
ImFont *ui_text_font(UiFontRole role);
ImFont *tab_font();
// The ascent to descent height of the face of role per em; an ImGui font size is that height.
float ui_font_height_per_em(UiFontRole role);
// Loads the car fonts of a machine from its font directory; a missing or unreadable face stops
// with UI_FONT, as only a rebuilt machine carries them.
void load_ui_fonts(const std::filesystem::path &directory);
} // namespace voyemu::cluster
