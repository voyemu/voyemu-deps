#pragma once
#include <cstdint>
#include <span>
namespace voyemu::ui {
enum class Resource {
    Icon,
    LogoText,
    LogoFigure,
    ScreensIcon
};
std::span<const std::uint8_t> resource(Resource id);
} // namespace voyemu::ui
