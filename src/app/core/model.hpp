#pragma once
#include "error.hpp"
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace voyemu {
using Json = nlohmann::json;

// The CODE of an exception: its own CODE, else FILESYSTEM, SYSTEM or JSON by the kind of a
// standard filesystem, system or JSON exception, else empty.
inline std::string_view failure_code(const std::exception &error) noexcept {
    if (const auto code = message_code(error.what()); !code.empty()) {
        return code;
    }

    if (dynamic_cast<const std::filesystem::filesystem_error *>(&error)) {
        return "FILESYSTEM";
    }

    if (dynamic_cast<const std::system_error *>(&error)) {
        return "SYSTEM";
    }

    if (dynamic_cast<const Json::exception *>(&error)) {
        return "JSON";
    }

    return {};
}

bool valid_profile_name(std::string_view name);
struct Field {
    std::uint32_t index;
    std::string name;
    unsigned width;
    std::uint32_t initial;
};

// The largest raw value of a field width, up to 64 bits.
constexpr std::uint64_t field_maximum(unsigned width) {
    return width >= 64 ? UINT64_MAX : (std::uint64_t{1} << width) - 1;
}

// The field catalogue of a vehicle platform, taken from the OEM vehicle input of that platform: the
// 579 fields of the qinggan09 mcusimulator contract, the 736 wire fields of the qinggan11
// libdataparser descriptors, and the 1598 cluster input signals of the Passion SOME/IP service
// table. A field qinggan09 and qinggan11 share has one name in both.
enum class VehicleCatalog {
    qinggan09,
    qinggan11,
    passion
};
// The catalogue of a platform value of profile.json.
VehicleCatalog vehicle_catalog(std::string_view platform);
const std::vector<Field> &catalog(VehicleCatalog catalog);
// The field of a catalogue by name, or null.
const Field *catalog_field(VehicleCatalog catalog, std::string_view name);
Json catalog_json(VehicleCatalog catalog);
// One value of an enumerated OEM signal.
struct SignalOption {
    std::uint64_t raw;
    std::string_view name;
};
// The declaration of a field in an OEM signal table: its wire type, physical = raw * resolution +
// offset within minimum..maximum in unit, the raw value of no value and the enumerated values.
struct SignalDefinition {
    std::uint32_t id;
    std::string_view type;
    std::optional<double> resolution, offset, minimum, maximum;
    std::optional<std::uint64_t> invalid;
    std::string_view unit;
    std::span<const SignalOption> options;
};
// The signal table declarations of the fields of a catalogue in catalogue order; empty for a
// catalogue without a signal table.
std::span<const SignalDefinition> catalog_signals(VehicleCatalog catalog);
// The identity of a catalogue in map_sha256 of a vehicle state: for qinggan09 the pinned
// mcusimulator wire contract, for qinggan11 the SHA-256 of the first 16 bytes (index, start bit,
// width, byte order) of each of the 736 libdataparser descriptors, equal in 6.11.1 and 7.3.1, for
// Passion the SHA-256 of the pinned SOME/IP service table.
inline constexpr const char *qinggan09_map_sha256 =
        "255335b86bb5fd782d23086018dcfc3874d088364ad5f15b264cfb12020d800e";
inline constexpr const char *qinggan11_map_sha256 =
        "1d29086a8bb11c1b7c3a1876c49339d769de3fbd76c08bb7ec04c7184cd61f54";
inline constexpr const char *passion_map_sha256 =
        "8018e59b0457f653e0d4925bef95ce7483ad276b70b261e3d38c0f1d91c5cbb4";
const char *catalog_map_sha256(VehicleCatalog catalog);
// The catalogue that map_sha256 of a vehicle state names.
VehicleCatalog state_catalog(const Json &state);
// The bytes a vehicle state file and a message carrying a state may take.
inline constexpr std::size_t vehicle_state_bytes = 512 * 1024;
std::string utc_now();

constexpr bool qnx_theme_display_matches(unsigned requested, std::uint64_t displayed) {
    // Pinned CSettingInfo::setdisplaytheme_lcdup/lcddown; shared display4 cannot identify
    // requests1/3/4.
    switch (requested) {
    case 1:
        return displayed == 1;
    case 2:
        return displayed == 2 || displayed == 5;
    case 3:
        return displayed == 3;
    case 4:
        return displayed == 6;
    default:
        return false;
    }
}

Json normal_initial_values(VehicleCatalog catalog);
Json initial_state(VehicleCatalog catalog);
// Guest quickboot is independent of the host scenario startup policy.
// Pure selection only: never overwrites the caller's saved scenario.
enum class VehicleStartup {
    fresh,
    resume
};
Json select_vehicle_startup(VehicleStartup mode, VehicleCatalog catalog, const Json &profile_values,
                            const Json *saved, std::uint32_t guest_sequence = 0);
void validate_state(const Json &state);
Json apply_patch(const Json &state, const Json &values);
// Bounded validated JSON bytes retain formatting for exact transaction rollback.
std::string read_private_json_bytes(const std::filesystem::path &path,
                                    std::size_t maximum = 128 * 1024);
void write_private_json_bytes(const std::filesystem::path &path, const std::string &text,
                              std::size_t maximum = 128 * 1024);
Json read_private_json(const std::filesystem::path &path, std::size_t maximum = 128 * 1024);
void write_private_json(const std::filesystem::path &path, const Json &value,
                        std::size_t maximum = 128 * 1024);
void ensure_private_directory(const std::filesystem::path &path);
// Recreates the missing ones of the four .local roots of an installation as private directories.
void ensure_local_roots(const std::filesystem::path &root);
class VehicleModel {
public:
    // Reads the state at path, or creates it with the initial state of catalog.
    VehicleModel(std::filesystem::path path, VehicleCatalog catalog);

    const Json &state() const noexcept {
        return state_;
    }

    const Json &patch(const Json &values);
    const Json &advance_revision(std::uint32_t observed);

private:
    std::filesystem::path path_;
    Json state_;
};
} // namespace voyemu
