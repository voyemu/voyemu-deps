#include "cluster_font.hpp"
#include "error.hpp"
#include "imgui.h"
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace voyemu::cluster {
namespace {
constexpr std::array<const char *, 3> face_names{"Regular", "Medium", "Bold"};
std::array<float, 3> heights_per_em{1, 1, 1};

[[noreturn]] void rebuild(const std::filesystem::path &file) {
    throw VoyemuError("UI_FONT", file.filename().string() +
                                         " of the machine is missing or unreadable; rebuild the "
                                         "machine");
}

std::uint32_t big_endian(const unsigned char *bytes, int count) {
    std::uint32_t value = 0;

    for (int i = 0; i < count; ++i) {
        value = value << 8 | bytes[i];
    }

    return value;
}

// The hhea ascent to descent height per head em that stb_truetype scales a pixel height by.
std::optional<float> height_per_em(const std::filesystem::path &file) {
    std::ifstream input(file, std::ios::binary);
    unsigned char header[12]{};

    if (!input.read(reinterpret_cast<char *>(header), sizeof header)) {
        return std::nullopt;
    }

    const auto tables = big_endian(header + 4, 2);
    std::optional<std::uint32_t> head, hhea;

    for (std::uint32_t i = 0; i < tables && i < 128; ++i) {
        unsigned char entry[16]{};

        if (!input.read(reinterpret_cast<char *>(entry), sizeof entry)) {
            return std::nullopt;
        }

        const std::string_view tag(reinterpret_cast<const char *>(entry), 4);

        if (tag == "head") {
            head = big_endian(entry + 8, 4);
        } else if (tag == "hhea") {
            hhea = big_endian(entry + 8, 4);
        }
    }

    unsigned char em[2]{}, metrics[4]{};

    if (!head || !hhea || !input.seekg(*head + 18) ||
        !input.read(reinterpret_cast<char *>(em), 2) || !input.seekg(*hhea + 4) ||
        !input.read(reinterpret_cast<char *>(metrics), 4) || big_endian(em, 2) == 0) {
        return std::nullopt;
    }

    const auto ascent = static_cast<std::int16_t>(big_endian(metrics, 2)),
               descent = static_cast<std::int16_t>(big_endian(metrics + 2, 2));

    return float(ascent - descent) / float(big_endian(em, 2));
}

// The Latin, Russian and symbol glyphs that the window shows.
bool covers_latin(ImFont *font) {
    for (ImWchar c = 0x20; c <= 0x7e; ++c) {
        if (!font->IsGlyphInFont(c)) {
            return false;
        }
    }

    for (ImWchar c = 0x0410; c <= 0x044f; ++c) {
        if (!font->IsGlyphInFont(c)) {
            return false;
        }
    }

    for (const ImWchar c :
         {0x0401, 0x0451, 0x00ab, 0x00bb, 0x00a9, 0x00b0, 0x00b7, 0x2014, 0x2026}) {
        if (!font->IsGlyphInFont(c)) {
            return false;
        }
    }

    return true;
}

ImFont *add_face(const std::filesystem::path &file, const char *name, bool merge) {
    if (!std::filesystem::is_regular_file(file)) {
        rebuild(file);
    }

    const auto utf8 = file.u8string();
    ImFontConfig config;
    config.MergeMode = merge;

    if (name) {
        std::snprintf(config.Name, sizeof(config.Name), "%s", name);
    }

    auto *font = ImGui::GetIO().Fonts->AddFontFromFileTTF(
            reinterpret_cast<const char *>(utf8.c_str()), 13, &config);

    if (!font) {
        rebuild(file);
    }

    return font;
}
} // namespace

void load_ui_fonts(const std::filesystem::path &directory) {
    ImFont *regular = nullptr;

    // The Latin and Cyrillic face comes first, so its glyphs, « and » among them, win over those
    // of the Chinese face merged after it.
    for (std::size_t weight = 0; weight < ui_font_weights.size(); ++weight) {
        const auto latin = directory / (std::string("latin-") + ui_font_weights[weight] + ".ttf"),
                   chinese = directory / (std::string("cjk-") + ui_font_weights[weight] + ".ttf");
        auto *face = add_face(latin, face_names[weight], false);
        const auto height = height_per_em(latin);

        if (!height || !covers_latin(face)) {
            rebuild(latin);
        }

        heights_per_em[weight] = *height;
        add_face(chinese, nullptr, true);

        if (!face->IsGlyphInFont(0x4e2d) || !face->IsGlyphInFont(0x6587)) {
            rebuild(chinese);
        }

        regular = regular ? regular : face;
    }

    ImGui::GetIO().FontDefault = regular;
}

ImFont *ui_text_font(UiFontRole role) {
    const char *name = role == UiFontRole::Emphasis ? face_names[1]
                       : role == UiFontRole::Choice ? face_names[2]
                                                    : face_names[0];

    for (auto *font : ImGui::GetIO().Fonts->Fonts) {
        if (std::string(font->GetDebugName()) == name) {
            return font;
        }
    }

    return ImGui::GetFont();
}

ImFont *tab_font() {
    return ui_text_font(UiFontRole::Emphasis);
}

float ui_font_height_per_em(UiFontRole role) {
    return heights_per_em[role == UiFontRole::Emphasis ? 1 : role == UiFontRole::Choice ? 2 : 0];
}
} // namespace voyemu::cluster
