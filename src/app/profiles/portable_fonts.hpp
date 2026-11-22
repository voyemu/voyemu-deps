#pragma once
#include "model.hpp"
#include <filesystem>
#include <string_view>
namespace voyemu {
// Writes the car fonts that the fonts object of profile.json declares into ui/fonts of the machine
// as <role>-<weight>.ttf, primary and fallback by regular, medium and bold, and returns their
// resource records. A font is a declared original or a pinned entry of an original APK; a declared
// corrected identity runs the Cyrillic repair of preparation.
Json export_portable_fonts(const std::filesystem::path &root, const Json &definition,
                           std::string_view profile, const std::filesystem::path &seed);
} // namespace voyemu
