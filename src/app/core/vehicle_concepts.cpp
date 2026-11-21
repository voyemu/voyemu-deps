#include "model.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace voyemu {
namespace {
using enum VehicleConcept;

ConceptEncoding linear(double resolution = 1, double offset = 0,
                       std::optional<std::uint64_t> invalid = std::nullopt) {
    return {resolution, offset, {}, invalid};
}

ConceptEncoding enumeration(std::vector<ConceptValue> values,
                            std::optional<std::uint64_t> invalid = std::nullopt) {
    return {1, 0, std::move(values), invalid};
}

ConceptField primary(std::string field, ConceptEncoding encoding = linear()) {
    return {std::move(field), std::move(encoding), ConceptRole::primary};
}

ConceptField follower(std::string field, ConceptEncoding encoding = linear()) {
    return {std::move(field), std::move(encoding), ConceptRole::follower};
}

// The concepts both Qinggan catalogues carry in the same fields with the identity encoding; the
// lamps, doors and belts as 0 and 1, the belts 1 when unbuckled.
std::vector<ConceptBinding> qinggan_bindings() {
    std::vector<ConceptBinding> result = {
            {speed, {primary("speed"), {"speed10x", linear(0.1), ConceptRole::mirror}}},
            {speed_valid, {primary("spd_valid")}},
            {gear, {primary("gear")}},
            {gear_valid, {primary("gear_valid")}},
            {charge, {primary("soc")}},
            {ignition, {primary("ign")}},
            {battery_voltage, {primary("ic_bat_voltage")}},
            {turn_left, {primary("turn_left")}},
            {turn_right, {primary("turn_right")}},
            {position_lamp, {primary("pos_lamp")}},
            {daytime_lamp, {primary("bcm_DRLStatus")}},
            {low_beam, {primary("low_beam")}},
            {high_beam, {primary("high_beam")}},
            {rear_fog, {primary("rear_fog")}},
            {belt_driver, {primary("acu_driverSeatBeltSts")}},
            {belt_passenger, {primary("acu_passengerSeatBeltSts")}},
            {belt_rear_left, {primary("RearLeftSeatBelt")}},
            {belt_rear_middle, {primary("RearMidSeatBelt")}},
            {belt_rear_right, {primary("RearRightSeatBelt")}},
            {door_front_left, {primary("door_fl")}},
            {door_front_right, {primary("door_fr")}},
            {door_rear_left, {primary("door_rl")}},
            {door_rear_right, {primary("door_rr")}},
            {hood, {primary("hood")}},
            {tailgate, {primary("tail_gate")}}};

    return result;
}

std::vector<ConceptBinding> qinggan09_bindings() {
    auto result = qinggan_bindings();

    result.push_back({ready, {primary("ready_lamp")}});
    // mrr_vSetDis carries the ACC set speed in 0.5 km/h.
    result.push_back({acc_target_speed, {primary("mrr_vSetDis", linear(0.5))}});

    return result;
}

std::vector<ConceptBinding> qinggan11_bindings() {
    auto result = qinggan_bindings();

    // The VCU of a qinggan11 vehicle reports READY in its own flag beside the lamp.
    result.push_back({ready, {primary("ready_lamp"), follower("vcu_ready")}});
    // ADCU_VsetDis in whole km/h with 255 as no speed: the OEM qinggan11
    // dataprocesser_interface_ACC_speed shows 255 and a CAN timeout as no speed and clamps every
    // other value to 20..150 km/h.
    result.push_back({acc_target_speed, {primary("ADCU_VsetDis", linear(1, 0, 255))}});

    return result;
}

// The Passion encodings of the SOME/IP service table: OnOffSts 1 OFF and 2 ON with 0 as the
// default before the first value and 255 invalid, SeatBelt 0 UNTIED and 1 TIED, DoorSts 0 CLOSE
// and 1 OPEN. A concept sets the actual and the logic status of every lamp of its function.
std::vector<ConceptBinding> passion_bindings() {
    const auto on_off = enumeration({{0, 1}, {1, 2}}, 255);
    const auto belt = enumeration({{0, 1}, {1, 0}});
    const std::string vehicle =
            "VehicleDrivingInfo.Notify_f_VehicleDrivingInform.VehicleInform.VehicleInform.";
    const std::string turn =
            "TurnLampService.Notify_f_TrunLampSts.TrunLampSts.TrunLampSts.TrunLamp";
    const std::string position = "PositionLampService.Notify_f_PosLampSts.PosLampSts.PosLamp";
    const std::string low = "LowBeamService.Notify_f_LowBeamSts.LowBeamSts.LowBeam";
    const std::string high = "HighBeamService.Notify_f_HighBeamSts.HighBeamSts.HighBeam";
    const std::string belts = "SeatBeltDetection.Notify_f_SeatBeltSts.SeatBeltSts.";
    const std::string doors = "DoorStsService.Notify_f_DoorSts.AllDoorSts.DoorSts.DorStsID_";
    const auto lamps = [&](const std::string &prefix, std::vector<std::string> actual,
                           std::vector<std::string> logic, const std::string &kind) {
        std::vector<ConceptField> fields;

        for (const auto &side : actual) {
            fields.push_back(
                    follower(prefix + "ActualSts." + side + "_" + kind + "ActualSts", on_off));
        }

        for (const auto &side : logic) {
            fields.push_back(
                    follower(prefix + "LogicSts." + side + "_" + kind + "LogicSts", on_off));
        }

        fields.front().role = ConceptRole::primary;

        return fields;
    };
    // VehicleSpeed in 0.01 km/h from -500 km/h; the chassis speed in 0.05625 km/h.
    const auto display_speed = linear(0.01, -500);

    return {{speed,
             {primary(vehicle + "VI_VehicleDisplaySpeed.VehicleSpeed", display_speed),
              follower(vehicle + "VI_VehicleDisplaySpeed.VehicleSpeedAbs", display_speed),
              follower(vehicle + "VI_VehicleDisplaySpeed.VehicleSpeedDisp", display_speed),
              follower("ChassisSysInfo.Notify_f_ChassisSysSts.ChassisSysSts.CSS_VehicleSpeedMsg."
                       "VS_VehicleSpeed",
                       linear(0.05625))}},
            {speed_valid,
             {primary(vehicle + "VI_VehicleDisplaySpeed.VehicleSpeedValidFlg"),
              follower("ChassisSysInfo.Notify_f_ChassisSysSts.ChassisSysSts.CSS_VehicleSpeedMsg."
                       "VS_VehicleSpeedValid")}},
            {gear,
             {primary(vehicle + "VI_ActualGear.AG_ActualGear", linear(1, 0, 255)),
              follower(vehicle + "VI_TargetGear.TG_TargetGear", linear(1, 0, 255))}},
            {gear_valid, {primary(vehicle + "VI_ActualGear.AG_ActualGearValid")}},
            // SOC in 0.1 %.
            {charge, {primary("PowerSysInfo.Notify_f_EnergyInform.EnergyInform.SOC", linear(0.1))}},
            // PowerMode 1 OFF, 2 Local_ON and 3 Remote_ON, 0 the default and 7 invalid.
            {ignition,
             {primary("VI_MN_VehicleModeManager.NotifyCycle_f_PowerMode_VMM.PowerMode",
                      enumeration({{0, 1}, {1, 2}, {1, 3}}, 7))}},
            {ready,
             {primary("PowerSysInfo.Notify_f_PowerSysDisplay.PowerSysDisplay.PowerSysDisplay."
                      "IndicatorLamp.LD_ReadyLamp",
                      on_off)}},
            {turn_left, lamps(turn, {"FL", "ML", "RL", "RML"}, {"FL", "ML", "RL"}, "TurnLamp")},
            {turn_right, lamps(turn, {"FR", "MR", "RR", "RMR"}, {"FR", "MR", "RR"}, "TurnLamp")},
            {position_lamp, lamps(position, {"FL", "ML", "RL", "FR", "MR", "RR", "RM"},
                                  {"FL", "ML", "RL", "FR", "MR", "RR", "F", "R"}, "PosLamp")},
            {daytime_lamp,
             {primary("DRLService.Notify_f_DRL_Sts.DRL_Sts.strt_DRL_ActualSts.DRL_FLActualSts",
                      on_off),
              follower("DRLService.Notify_f_DRL_Sts.DRL_Sts.strt_DRL_ActualSts.DRL_FRActualSts",
                       on_off),
              follower("DRLService.Notify_f_DRL_Sts.DRL_Sts.strt_DRL_LogicSts.DRL_LogicSts",
                       on_off)}},
            {low_beam, lamps(low, {"FL", "FR"}, {"FL", "FR"}, "LowBeam")},
            {high_beam, lamps(high, {"FL", "FR"}, {"FL", "FR"}, "HighBeam")},
            {rear_fog,
             {primary("FogLampsService.Notify_f_FogLampSts.FogLampSts.FogLampActualSts", on_off),
              follower("FogLampsService.Notify_f_FogLampSts.FogLampSts.FogLampLogicSts", on_off)}},
            {belt_driver, {primary(belts + "DriveSeatBeltSts", belt)}},
            {belt_passenger, {primary(belts + "PasseSeatBeltSts", belt)}},
            {belt_rear_left, {primary(belts + "SeconLeftSeatBeltSts", belt)}},
            {belt_rear_middle, {primary(belts + "SeconMidSeatBeltSts", belt)}},
            {belt_rear_right, {primary(belts + "SeconRightSeatBeltSts", belt)}},
            {door_front_left, {primary(doors + "DcuFlDoor")}},
            {door_front_right, {primary(doors + "DcuFrDoor")}},
            {door_rear_left, {primary(doors + "DcuRlDoor")}},
            {door_rear_right, {primary(doors + "DcuRrDoor")}},
            {hood, {primary(doors + "HoodSts")}},
            {tailgate, {primary(doors + "TailgateSts")}},
            // eps_sasSteeringAngle in 0.1 degree from -1000 degrees.
            {steering_angle,
             {primary("SteeringSysInfo.Notify_f_SteeringSysSts.SteeringSysSts.SteeringSysSts."
                      "eps_sasSteeringAngle",
                      linear(1, -10000))}},
            {acc_target_speed,
             {primary("AssidDrvgMgr.Notify_f_ADrvgMgrFunOuC.ADrvgMgrFunOuC.ADrvgMgrFunOu."
                      "ACC_TargetSpeed_kph",
                      linear(1, 0, 255))}}};
}

// Every bound field exists in its catalogue and a concept is bound once.
std::vector<ConceptBinding> checked(VehicleCatalog catalog, std::vector<ConceptBinding> bindings) {
    std::vector<char> bound(vehicle_concepts().size());

    for (const auto &binding : bindings) {
        auto &seen = bound.at(static_cast<std::size_t>(binding.id));

        if (seen || binding.fields.empty() || binding.fields.front().role != ConceptRole::primary) {
            throw VoyemuError("VEHICLE_CONCEPT", "malformed concept binding");
        }

        seen = true;

        for (const auto &field : binding.fields) {
            if (!catalog_field(catalog, field.field)) {
                throw VoyemuError("VEHICLE_CONCEPT", "unknown bound field " + field.field);
            }
        }
    }

    return bindings;
}

const ConceptField *bound_field(const ConceptBinding &binding, std::string_view field) {
    const auto found = std::find_if(binding.fields.begin(), binding.fields.end(),
                                    [&](const ConceptField &item) {
                                        return item.field == field;
                                    });

    return found == binding.fields.end() ? nullptr : &*found;
}

// The raw value of logical before the width check; a primary or a mirror outside the width fails
// validation of the state as any other raw value.
std::int64_t unbounded(const ConceptEncoding &encoding, std::int64_t logical) {
    if (!encoding.values.empty()) {
        const auto found = std::find_if(encoding.values.begin(), encoding.values.end(),
                                        [&](const ConceptValue &value) {
                                            return value.logical == logical;
                                        });

        if (found == encoding.values.end()) {
            throw VoyemuError("VEHICLE_VALUE", "value outside the enumeration of its field");
        }

        return static_cast<std::int64_t>(found->raw);
    }

    return std::llround((static_cast<double>(logical) - encoding.offset) / encoding.resolution);
}

std::uint64_t width_of(VehicleCatalog catalog, const std::string &field) {
    return catalog_field(catalog, field)->width;
}

// A follower saturates to its width.
std::uint64_t saturated(const ConceptField &field, VehicleCatalog catalog, std::int64_t logical) {
    const auto raw = unbounded(field.encoding, logical);
    const auto maximum = field_maximum(static_cast<unsigned>(width_of(catalog, field.field)));

    return raw < 0 ? 0 : std::min(maximum, static_cast<std::uint64_t>(raw));
}

bool whole(double value) {
    return std::abs(value - std::round(value)) < 1e-6;
}

bool in_range(VehicleConcept id, double value) {
    const auto &definition = concept_definition(id);

    return (!definition.minimum || value >= static_cast<double>(*definition.minimum) - 1e-9) &&
           (!definition.maximum || value <= static_cast<double>(*definition.maximum) + 1e-9);
}
} // namespace

const std::vector<ConceptDefinition> &vehicle_concepts() {
    static const std::vector<ConceptDefinition> concepts = {
            {speed, "speed", "km/h", 0, 255},
            {speed_valid, "speed_valid", "flag", {}, {}},
            {gear, "gear", "P=0 R=1 N=2 D=3", {}, {}},
            {gear_valid, "gear_valid", "flag", {}, {}},
            {charge, "charge", "percent", 0, 100},
            {ignition, "ignition", "flag", {}, {}},
            {ready, "ready", "flag", {}, {}},
            {battery_voltage, "battery_voltage", "0.01 V", {}, {}},
            {turn_left, "turn_left", "flag", {}, {}},
            {turn_right, "turn_right", "flag", {}, {}},
            {position_lamp, "position_lamp", "flag", {}, {}},
            {daytime_lamp, "daytime_lamp", "flag", {}, {}},
            {low_beam, "low_beam", "flag", {}, {}},
            {high_beam, "high_beam", "flag", {}, {}},
            {rear_fog, "rear_fog", "flag", {}, {}},
            {belt_driver, "belt_driver", "unbuckled flag", {}, {}},
            {belt_passenger, "belt_passenger", "unbuckled flag", {}, {}},
            {belt_rear_left, "belt_rear_left", "unbuckled flag", {}, {}},
            {belt_rear_middle, "belt_rear_middle", "unbuckled flag", {}, {}},
            {belt_rear_right, "belt_rear_right", "unbuckled flag", {}, {}},
            {door_front_left, "door_front_left", "open flag", {}, {}},
            {door_front_right, "door_front_right", "open flag", {}, {}},
            {door_rear_left, "door_rear_left", "open flag", {}, {}},
            {door_rear_right, "door_rear_right", "open flag", {}, {}},
            {hood, "hood", "open flag", {}, {}},
            {tailgate, "tailgate", "open flag", {}, {}},
            {steering_angle, "steering_angle", "0.1 deg", {}, {}},
            {acc_target_speed, "acc_target_speed", "km/h", {}, {}}};

    return concepts;
}

const ConceptDefinition &concept_definition(VehicleConcept id) {
    return vehicle_concepts().at(static_cast<std::size_t>(id));
}

const std::vector<ConceptBinding> &concept_bindings(VehicleCatalog catalog) {
    static const auto bindings = [] {
        return std::array<std::vector<ConceptBinding>, 3>{
                checked(VehicleCatalog::qinggan09, qinggan09_bindings()),
                checked(VehicleCatalog::qinggan11, qinggan11_bindings()),
                checked(VehicleCatalog::passion, passion_bindings())};
    }();

    return bindings.at(static_cast<std::size_t>(catalog));
}

const ConceptBinding *concept_binding(VehicleCatalog catalog, VehicleConcept id) {
    const auto &bindings = concept_bindings(catalog);
    const auto found = std::find_if(bindings.begin(), bindings.end(),
                                    [&](const ConceptBinding &binding) {
                                        return binding.id == id;
                                    });

    return found == bindings.end() ? nullptr : &*found;
}

const ConceptBinding *field_binding(VehicleCatalog catalog, std::string_view field) {
    static const auto indexes = [] {
        std::array<std::map<std::string, const ConceptBinding *, std::less<>>, 3> result;

        for (const auto which :
             {VehicleCatalog::qinggan09, VehicleCatalog::qinggan11, VehicleCatalog::passion}) {
            for (const auto &binding : concept_bindings(which)) {
                for (const auto &item : binding.fields) {
                    result[static_cast<std::size_t>(which)].emplace(item.field, &binding);
                }
            }
        }

        return result;
    }();
    const auto &index = indexes[static_cast<std::size_t>(catalog)];
    const auto found = index.find(field);

    return found == index.end() ? nullptr : found->second;
}

std::optional<std::uint64_t> concept_encode(const ConceptEncoding &encoding, std::int64_t logical,
                                            unsigned width) {
    try {
        const auto raw = unbounded(encoding, logical);

        if (raw < 0 || static_cast<std::uint64_t>(raw) > field_maximum(width)) {
            return std::nullopt;
        }

        return static_cast<std::uint64_t>(raw);
    } catch (const VoyemuError &) {
        return std::nullopt;
    }
}

std::optional<double> concept_decode(const ConceptEncoding &encoding, std::uint64_t raw) {
    if (encoding.invalid == raw) {
        return std::nullopt;
    }

    if (!encoding.values.empty()) {
        const auto found = std::find_if(encoding.values.begin(), encoding.values.end(),
                                        [&](const ConceptValue &value) {
                                            return value.raw == raw;
                                        });

        return found == encoding.values.end()
                       ? std::nullopt
                       : std::optional<double>(static_cast<double>(found->logical));
    }

    return static_cast<double>(raw) * encoding.resolution + encoding.offset;
}

std::string concept_field(VehicleCatalog catalog, VehicleConcept id) {
    const auto *binding = concept_binding(catalog, id);

    return binding ? binding->fields.front().field : std::string{};
}

std::optional<std::int64_t> concept_value(const Json &state, VehicleConcept id) {
    const auto *binding = concept_binding(state_catalog(state), id);

    if (!binding) {
        return std::nullopt;
    }

    const auto &field = binding->fields.front();
    const auto decoded = concept_decode(field.encoding,
                                        state.at("raw_oem").at(field.field).get<std::uint64_t>());

    return decoded ? std::optional<std::int64_t>(std::llround(*decoded)) : std::nullopt;
}

Json concept_patch(VehicleCatalog catalog, VehicleConcept id, std::int64_t logical) {
    const auto *binding = concept_binding(catalog, id);

    if (!binding) {
        return Json::object();
    }

    const auto &field = binding->fields.front();

    return {{field.field, unbounded(field.encoding, logical)}};
}

Json concept_change(const Json &state, VehicleConcept id, std::int64_t logical) {
    auto patch = concept_patch(state_catalog(state), id, logical);
    const auto &raw = state.at("raw_oem");

    for (auto item = patch.begin(); item != patch.end();) {
        item = raw.at(item.key()) == item.value() ? patch.erase(item) : std::next(item);
    }

    return patch;
}

void concept_assign(VehicleCatalog catalog, Json &raw, VehicleConcept id, std::int64_t logical) {
    const auto *binding = concept_binding(catalog, id);

    if (!binding) {
        return;
    }

    for (const auto &field : binding->fields) {
        raw[field.field] = field.role == ConceptRole::follower
                                   ? Json(saturated(field, catalog, logical))
                                   : Json(unbounded(field.encoding, logical));
    }
}

bool concept_raw_allowed(VehicleCatalog catalog, std::string_view field, std::uint64_t raw) {
    const auto *binding = field_binding(catalog, field);

    if (!binding) {
        return true;
    }

    const auto *item = bound_field(*binding, field);
    const auto decoded = concept_decode(item->encoding, raw);

    if (item->role == ConceptRole::follower || !decoded) {
        return true;
    }

    if (!in_range(binding->id, *decoded)) {
        return false;
    }

    if (item->role == ConceptRole::primary) {
        return true;
    }

    const auto &target = binding->fields.front();

    return whole(*decoded) &&
           concept_encode(target.encoding, std::llround(*decoded),
                          static_cast<unsigned>(width_of(catalog, target.field)));
}

bool concepts_consistent(VehicleCatalog catalog, const Json &raw) {
    for (const auto &binding : concept_bindings(catalog)) {
        const auto &main = binding.fields.front();
        const auto decoded = concept_decode(main.encoding, raw.at(main.field).get<std::uint64_t>());

        if (!decoded) {
            continue;
        }

        if (!in_range(binding.id, *decoded)) {
            return false;
        }

        for (const auto &field : binding.fields) {
            if (field.role == ConceptRole::mirror &&
                raw.at(field.field) != unbounded(field.encoding, std::llround(*decoded))) {
                return false;
            }
        }
    }

    return true;
}

namespace detail {
// Derives the bound fields a patch did not name from the one it named: the primary, else the
// first named mirror, which must carry a whole logical value.
void derive_concepts(VehicleCatalog catalog, Json &raw, const Json &values) {
    for (const auto &binding : concept_bindings(catalog)) {
        const ConceptField *source = nullptr;

        for (const auto &field : binding.fields) {
            if (values.contains(field.field) && field.role != ConceptRole::follower) {
                source = &field;

                break;
            }
        }

        if (!source) {
            continue;
        }

        const auto decoded = concept_decode(source->encoding,
                                            raw.at(source->field).get<std::uint64_t>());

        if (!decoded) {
            continue;
        }

        if (source->role == ConceptRole::mirror && !whole(*decoded)) {
            throw VoyemuError("VEHICLE_COUPLED",
                              "whole " + std::string(concept_definition(binding.id).unit) +
                                      " required");
        }

        const auto logical = std::llround(*decoded);

        for (const auto &field : binding.fields) {
            if (values.contains(field.field)) {
                continue;
            }

            raw[field.field] = field.role == ConceptRole::follower
                                       ? Json(saturated(field, catalog, logical))
                                       : Json(unbounded(field.encoding, logical));
        }
    }
}
} // namespace detail
} // namespace voyemu
