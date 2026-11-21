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
// A vehicle concept that the shared logic reads and writes once for every platform; each catalogue
// binds it to its own fields.
enum class VehicleConcept {
    speed,
    speed_valid,
    gear,
    gear_valid,
    charge,
    ignition,
    ready,
    battery_voltage,
    turn_left,
    turn_right,
    position_lamp,
    daytime_lamp,
    low_beam,
    high_beam,
    rear_fog,
    belt_driver,
    belt_passenger,
    belt_rear_left,
    belt_rear_middle,
    belt_rear_right,
    door_front_left,
    door_front_right,
    door_rear_left,
    door_rear_right,
    hood,
    tailgate,
    steering_angle,
    acc_target_speed
};
// The logical value of a concept is an integer in unit, within minimum..maximum when given.
struct ConceptDefinition {
    VehicleConcept id;
    std::string_view name, unit;
    std::optional<std::int64_t> minimum, maximum;
};
const std::vector<ConceptDefinition> &vehicle_concepts();
const ConceptDefinition &concept_definition(VehicleConcept id);
struct ConceptValue {
    std::int64_t logical;
    std::uint64_t raw;
};
// How one field carries a concept: logical = raw * resolution + offset, or the logical value that
// values lists for raw, the first entry of a logical value being its encoding; invalid is the raw
// value of no value.
struct ConceptEncoding {
    double resolution = 1, offset = 0;
    std::vector<ConceptValue> values;
    std::optional<std::uint64_t> invalid;
};
// The primary field holds the concept; a mirror carries the same value in another encoding and
// sets the primary when a writer names it alone; a follower takes the value of the primary when a
// writer names the primary and not the follower.
enum class ConceptRole {
    primary,
    mirror,
    follower
};
struct ConceptField {
    std::string field;
    ConceptEncoding encoding;
    ConceptRole role;
};
struct ConceptBinding {
    VehicleConcept id;
    std::vector<ConceptField> fields;
};
// The concepts a catalogue binds, its primary field first.
const std::vector<ConceptBinding> &concept_bindings(VehicleCatalog catalog);
const ConceptBinding *concept_binding(VehicleCatalog catalog, VehicleConcept id);
// The binding of a field of a catalogue, or null.
const ConceptBinding *field_binding(VehicleCatalog catalog, std::string_view field);
// The raw value of logical in a field of width bits; null when the encoding cannot carry it.
std::optional<std::uint64_t> concept_encode(const ConceptEncoding &encoding, std::int64_t logical,
                                            unsigned width);
// The logical value of raw before rounding; null for the invalid value or an unlisted raw value.
std::optional<double> concept_decode(const ConceptEncoding &encoding, std::uint64_t raw);
// The primary field of a concept; empty when the catalogue does not bind it.
std::string concept_field(VehicleCatalog catalog, VehicleConcept id);
// The value of a concept in a state, rounded to its unit; null when the catalogue does not bind
// it or its primary field holds no value.
std::optional<std::int64_t> concept_value(const Json &state, VehicleConcept id);
// A patch setting a concept through its primary field, from which apply_patch derives mirrors and
// followers; empty when the catalogue does not bind the concept.
Json concept_patch(VehicleCatalog catalog, VehicleConcept id, std::int64_t logical);
// concept_patch without the fields that already hold their value in state.
Json concept_change(const Json &state, VehicleConcept id, std::int64_t logical);
// Sets a concept in every bound field of raw, for a projection that bypasses apply_patch.
void concept_assign(VehicleCatalog catalog, Json &raw, VehicleConcept id,
                    std::int64_t logical);
// Whether raw in a field keeps its concept valid: within the range of the concept for a primary
// field, a whole logical value within the range for a mirror.
bool concept_raw_allowed(VehicleCatalog catalog, std::string_view field, std::uint64_t raw);
// Whether the primary field of each concept of raw lies within the range of its concept and each
// mirror carries the value of its primary.
bool concepts_consistent(VehicleCatalog catalog, const Json &raw);
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
