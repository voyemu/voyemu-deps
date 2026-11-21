#include "ui_resources.hpp"
#include "embedded_data.hpp"
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
namespace voyemu::ui {
namespace {
#include "ui_resources.inc"

struct Deflated {
    std::span<const std::uint8_t> bytes;
    std::size_t size;
};

Deflated deflated(Resource id) {
    switch (id) {
    case Resource::Icon:
        return {application_icon, application_icon_size};
    case Resource::LogoText:
        return {logo_text, logo_text_size};
    case Resource::LogoFigure:
        return {logo_figure, logo_figure_size};
    case Resource::ScreensIcon:
        return {screens_icon, screens_icon_size};
    }

    throw std::invalid_argument("UI_RESOURCE: unknown resource");
}
} // namespace

// Each resource is inflated on first use and kept for the life of the process.
std::span<const std::uint8_t> resource(Resource id) {
    static std::mutex mutex;
    static std::map<Resource, std::string> inflated;
    const std::lock_guard lock(mutex);
    auto found = inflated.find(id);

    if (found == inflated.end()) {
        const auto source = deflated(id);
        found = inflated.emplace(id, inflate_embedded(source.bytes, source.size)).first;
    }

    return {reinterpret_cast<const std::uint8_t *>(found->second.data()), found->second.size()};
}
} // namespace voyemu::ui
