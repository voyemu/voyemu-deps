#include "cluster_font.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "parameters_layout.hpp"
#include "ui_localization.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace voyemu::cluster;
namespace {
void check(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// A copy of the fixture directory without one face, or with one face unreadable.
std::filesystem::path broken_directory(const std::filesystem::path &fonts, const char *face,
                                       bool remove) {
    const auto scratch =
            fonts.parent_path().parent_path().parent_path().parent_path().parent_path() /
            ".local/build/tmp" /
            (".voyemu-fonts-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    std::filesystem::create_directories(scratch);

    for (const auto &entry : std::filesystem::directory_iterator(fonts)) {
        std::filesystem::copy_file(entry.path(), scratch / entry.path().filename());
    }

    if (remove) {
        std::filesystem::remove(scratch / face);
    } else {
        std::ofstream(scratch / face, std::ios::binary | std::ios::trunc) << "not a font";
    }

    return scratch;
}

void font_guards(const std::filesystem::path &path) {
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    try {
        for (const auto *face : {"latin-regular.ttf", "cjk-bold.ttf"}) {
            for (const bool remove : {true, false}) {
                const auto broken = broken_directory(path, face, remove);
                std::string message;

                try {
                    load_ui_fonts(broken);
                } catch (const std::exception &error) {
                    message = error.what();
                }

                std::filesystem::remove_all(broken);
                check(message.starts_with("UI_FONT: ") && message.find(face) != std::string::npos &&
                              message.find("rebuild the machine") != std::string::npos,
                      "a missing or unreadable machine font asks for a rebuilt machine");
                ImGui::DestroyContext();
                ImGui::CreateContext();
                ImGui::GetIO().IniFilename = nullptr;
            }
        }

        load_ui_fonts(path);
        auto &io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = ImVec2(1294, 360);
        io.DeltaTime = 1.f / 60;
        ImGui::GetStyle().FontSizeBase = 13;
        check(std::string(io.FontDefault->GetDebugName()) == "Regular",
              "Regular remains the default field font");
        ImGui::NewFrame();
        ImGui::Begin("font metric guards");
        check(io.Fonts->Fonts.Size == 3, "the window loads the three weights of the machine");
        check(std::string(tab_font()->GetDebugName()) == "Medium", "tabs use the Medium face");
        check(ui_text_font(UiFontRole::Body) == io.FontDefault,
              "body role is the default official Regular face");
        check(ui_text_font(UiFontRole::Label) == io.FontDefault,
              "labels and body share the same Regular face");
        check(ui_text_font(UiFontRole::Emphasis) == tab_font() && tab_font() != io.FontDefault,
              "tabs and group headings share Medium500 above Regular400 body");
        // Chinese captions sit beside Latin ones with a comparable stroke: the ink of 中 and 文 is
        // close to that of E and N in Regular.
        const auto ink = [&](ImFont *font, const char *text) {
            float total = 0;

            while (*text) {
                unsigned int codepoint = 0;
                text += ImTextCharFromUtf8(&codepoint, text, nullptr);
                const auto *glyph =
                        font->GetFontBaked(26)->FindGlyph(static_cast<ImWchar>(codepoint));
                auto *atlas = io.Fonts->TexData;
                check(glyph && glyph->Visible && glyph->Codepoint == codepoint,
                      "caption glyph is rasterized");

                for (int y = std::lround(glyph->V0 * atlas->Height);
                     y < std::lround(glyph->V1 * atlas->Height); ++y) {
                    for (int x = std::lround(glyph->U0 * atlas->Width);
                         x < std::lround(glyph->U1 * atlas->Width); ++x) {
                        const auto *pixel =
                                static_cast<const unsigned char *>(atlas->GetPixelsAt(x, y));
                        total += atlas->Format == ImTextureFormat_Alpha8 ? pixel[0] : pixel[3];
                    }
                }
            }

            return total / 255;
        };
        const float latin = ink(io.FontDefault, "EN"), chinese = ink(io.FontDefault, "中文");
        check(chinese > .8f * latin && chinese < 1.4f * latin,
              "Regular Chinese captions have the stroke weight of Regular Latin captions");
        check(std::string(ui_text_font(UiFontRole::Choice)->GetDebugName()) == "Bold" &&
                      ink(ui_text_font(UiFontRole::Choice), "EN") > 1.15f * latin,
              "choice captions use the Bold face");
        check(std::abs(ui_font_height_per_em(UiFontRole::Body) - 2478.f / 2048) < .001f,
              "the size of a face follows its own ascent to descent height per em");

        for (const auto &entry : voyemu::ui::catalogue) {
            const char *text = entry.chinese;

            while (*text) {
                unsigned int codepoint = 0;
                const int bytes = ImTextCharFromUtf8(&codepoint, text, nullptr);
                check(bytes > 0, "Chinese catalogue is valid UTF-8");
                text += bytes;
                check(io.FontDefault->IsGlyphInFont(static_cast<ImWchar>(codepoint)) &&
                              ui_text_font(UiFontRole::Label)
                                      ->IsGlyphInFont(static_cast<ImWchar>(codepoint)) &&
                              ui_text_font(UiFontRole::Emphasis)
                                      ->IsGlyphInFont(static_cast<ImWchar>(codepoint)),
                      "every Chinese catalogue glyph exists in both UI faces");
            }
        }

        check(ImGui::CalcTextSize("4294967295").x <= ParametersLayout::raw_value_width - 20,
              "all ten digits of a raw uint32 maximum fit inside the numeric field padding");

        for (const auto language : {voyemu::ui::Language::russian, voyemu::ui::Language::english,
                                    voyemu::ui::Language::chinese}) {
            voyemu::ui::current_language = language;

            for (const unsigned scale : {canonical_ui_scale, 75u, 100u}) {
                const auto layout = parameters_layout(1294, 360, 0, scale);
                const std::array<const char *, 5> captions = {"Движение", "Кузов", "Свет", "Шины",
                                                              "Все"};
                ImGui::PushFont(tab_font(), layout.tab_font_size());
                std::array<float, 5> widths{};

                for (std::size_t i = 0; i < captions.size(); ++i) {
                    widths[i] = ImGui::CalcTextSize(voyemu::ui::tr(captions[i])).x;
                }

                const auto tabs = parameter_tabs(layout, widths, true);
                check(tabs.font_scale > 0 && tabs.font_scale <= 1,
                      "localized navigation has a finite bounded font scale");

                if (layout.compact50()) {
                    check(tabs.font_scale == 1,
                          "50percent navigation preserves the measured reference size without locale shrinking");
                }

                for (const auto &tab : tabs.items) {
                    check(tab.x >= layout.body.x && tab.right() <= layout.body.right() + .001f,
                          "localized navigation fits the available row");
                }

                ImGui::PopFont();

                for (const auto *label :
                     {"Передняя левая", "Передняя правая", "Задняя левая", "Задняя правая",
                      "Габариты", "Ближний", "Дальний", "Дневные огни", "Туман сзади"}) {
                    const auto extent = ui_text_font(UiFontRole::Label)
                                                ->CalcTextSizeA(layout.label_font_size(), FLT_MAX,
                                                                0, voyemu::ui::tr(label));
                    check(extent.x <= layout.body.width - 49 &&
                                  extent.y == layout.label_font_size(),
                          "full production-font door and lamp captions fit one enlarged line");
                }

                for (const auto *label : {"Капот", "Багажник", "Левый", "Правый"}) {
                    const auto extent = ui_text_font(UiFontRole::Label)
                                                ->CalcTextSizeA(layout.label_font_size(), FLT_MAX,
                                                                0, voyemu::ui::tr(label));
                    check(extent.x <= layout.column(2, 0).width - 49,
                          "short paired captions retain the full enlarged font size");
                }
            }
        }

        ImGui::End();
        ImGui::Render();
        ImGui::DestroyContext();
    } catch (...) {
        ImGui::DestroyContext();
        throw;
    }
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("font test requires the fixture font directory");
        }

        font_guards(argv[1]);
        std::cout << "native font guards: pass\n";

        return 0;
    } catch (const std::exception &error) {
        std::cerr << "native font guards: " << error.what() << '\n';

        return 1;
    }
}
