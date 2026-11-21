#include "adas_ecu.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace voyemu {
namespace {
constexpr std::array<std::string_view, 6> buttons{"ica",       "acc",         "distance",
                                                  "cruise_up", "cruise_down", "cancel"};
constexpr std::int64_t minimum_speed = 30, maximum_speed = 130;

// The ADAS status and configuration fields of the Qinggan catalogues; a field the catalogue of the
// platform lacks reads 0 and is never written, so a catalogue without cfg_acc keeps ACC disabled.
unsigned raw(const Json &state, const char *key) {
    const auto &values = state.at("raw_oem");

    return values.contains(key) ? values.at(key).get<unsigned>() : 0;
}

bool known(const Json &state) {
    return raw(state, "acc_mode") <= 5 && raw(state, "mpc_icaSts") <= 3;
}

// A whole km/h set speed within the policy; 0 is no remembered speed.
bool valid_target(double kmh) {
    return kmh == std::round(kmh) && kmh >= minimum_speed && kmh <= maximum_speed;
}

std::string readiness(const Json &state, bool ica) {
    if (raw(state, "cfg_acc") != 1) {
        return "acc-feature-disabled";
    }

    if (ica && raw(state, "cfg_tja_assis") != 1) {
        return "ica-feature-disabled";
    }

    if (concept_value(state, VehicleConcept::gear_valid) != 1 ||
        concept_value(state, VehicleConcept::gear) != 3) {
        return "gear-not-drive";
    }

    if (concept_value(state, VehicleConcept::ready) != 1) {
        return "vehicle-not-ready";
    }

    if (raw(state, "vcu_brakePedalPressed")) {
        return "brake-pressed";
    }

    const auto speed = concept_value(state, VehicleConcept::speed);

    if (concept_value(state, VehicleConcept::speed_valid).value_or(0) == 0 || !speed ||
        *speed < minimum_speed || *speed > maximum_speed) {
        return "speed-outside-policy";
    }

    return {};
}

void changed(Json &patch, const Json &state, const char *name, unsigned value) {
    if (state.at("raw_oem").contains(name) && raw(state, name) != value) {
        patch[name] = value;
    }
}

// The ACC set speed in km/h, which each catalogue encodes in its own field; no speed is 0.
double target_of(const Json &state) {
    const auto *binding = concept_binding(state_catalog(state), VehicleConcept::acc_target_speed);

    if (!binding) {
        return 0;
    }

    const auto &field = binding->fields.front();

    return concept_decode(field.encoding, state.at("raw_oem").at(field.field).get<std::uint64_t>())
            .value_or(0);
}

void set_target(Json &patch, const Json &state, std::int64_t kmh) {
    patch.update(concept_change(state, VehicleConcept::acc_target_speed, kmh));
}

Json base_receipt(const Json &state) {
    return {{"consumer", "voyemu.adas-ecu"},          {"policy_id", "voyemu-own-adas-v1"},
            {"input_kind", "simulated-ecu"},          {"before_revision", state.at("sequence")},
            {"after_revision", state.at("sequence")}, {"changed_fields", Json::object()}};
}

void finish(AdasOutcome &result, const Json &state, std::string outcome, std::string reason) {
    result.receipt["outcome"] = std::move(outcome);
    result.receipt["reason"] = std::move(reason);
    result.receipt["changed_fields"] = result.patch;

    if (!result.patch.empty()) {
        result.receipt["after_revision"] = state.at("sequence").get<std::uint64_t>() + 1;
    }
}
} // namespace

bool AdasEcu::handles(std::string_view id) {
    return std::find(buttons.begin(), buttons.end(), id) != buttons.end();
}

Json AdasEcu::policy() {
    return {{"consumer", "voyemu.adas-ecu"},
            {"policy_id", "voyemu-own-adas-v1"},
            {"input_kind", "simulated-ecu"},
            {"target_kmh",
             {{"minimum", minimum_speed},
              {"maximum", maximum_speed},
              {"short_step", 1},
              {"long_step", 5}}},
            {"hold_threshold_ms", 1000},
            {"hold_repeat", false},
            {"headway_levels", Json::array({1, 2, 3})},
            {"physical_oem_protocol", "unrecovered"}};
}

AdasOutcome AdasEcu::input(const Json &state, std::uint32_t sequence, std::string_view id,
                           std::string_view phase, std::string_view owner, Clock::time_point when) {
    AdasOutcome result;

    if (!handles(id)) {
        return result;
    }

    result.handled = true;
    validate_state(state);
    result.receipt = base_receipt(state);
    result.receipt.update(
            {{"event_sequence", sequence}, {"button", id}, {"phase", phase}, {"owner", owner}});
    auto reject = [&](std::string reason) {
        finish(result, state, "rejected", std::move(reason));

        return result;
    };

    if (!sequence || sequence <= last_sequence_) {
        return reject("replayed-sequence");
    }

    if (owner.empty() || owner.size() > 64 ||
        !std::all_of(owner.begin(), owner.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        })) {
        return reject("invalid-owner");
    }

    if (phase != "down" && phase != "up" && phase != "cancel") {
        return reject("invalid-phase");
    }

    last_sequence_ = sequence;
    auto held = held_.find(id);

    if (phase == "down") {
        if (held != held_.end()) {
            return reject("already-held");
        }

        held_.emplace(std::string(id), Held{std::string(owner), when});
        finish(result, state, "held", "awaiting-release");

        return result;
    }

    if (held == held_.end() || held->second.owner != owner) {
        return reject("release-owner-mismatch");
    }

    auto down = held->second.down;
    held_.erase(held);

    if (phase == "cancel") {
        finish(result, state, "cancelled", "release-without-action");

        return result;
    }

    if (when < down) {
        return reject("non-monotonic-time");
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(when - down).count();
    bool long_press = elapsed >= 1000;
    result.receipt["duration_ms"] = elapsed;
    result.receipt["press_kind"] = long_press ? "long" : "short";

    if (!known(state)) {
        finish(result, state, "unavailable", "fault-or-reserved-status");

        return result;
    }

    auto acc = raw(state, "acc_mode"), ica = raw(state, "mpc_icaSts");
    const auto target = target_of(state);

    if (id == "cancel") {
        changed(result.patch, state, "acc_mode", 0);
        changed(result.patch, state, "mpc_icaSts", 1);
    } else if (id == "distance") {
        if (acc < 2) {
            finish(result, state, "no-change", "cruise-not-enabled");

            return result;
        }

        if (raw(state, "cfg_acc") != 1) {
            finish(result, state, "unavailable", "acc-feature-disabled");

            return result;
        }

        auto gap = raw(state, "time_gap_set");

        if (gap > 3) {
            finish(result, state, "unavailable", "reserved-headway");

            return result;
        }

        changed(result.patch, state, "time_gap_set", gap == 0 || gap == 3 ? 1 : gap + 1);
    } else if (id == "acc" || id == "ica") {
        bool requested_ica = id == "ica";
        auto reason = readiness(state, requested_ica);

        if (reason == "acc-feature-disabled" || reason == "ica-feature-disabled") {
            finish(result, state, "unavailable", reason);

            return result;
        }

        bool active = reason.empty();
        changed(result.patch, state, "acc_mode", active ? 3 : 2);
        changed(result.patch, state, "mpc_icaSts", requested_ica ? (active ? 3 : 2) : 1);

        if (active) {
            set_target(result.patch, state, *concept_value(state, VehicleConcept::speed));
        }

        if (raw(state, "time_gap_set") == 0) {
            changed(result.patch, state, "time_gap_set", 1);
        }

        if (!reason.empty()) {
            finish(result, state, result.patch.empty() ? "no-change" : "applied", reason);

            return result;
        }
    } else if (acc == 3) {
        auto reason = readiness(state, ica == 3);

        if (!reason.empty()) {
            changed(result.patch, state, "acc_mode", 2);

            if (ica == 3) {
                changed(result.patch, state, "mpc_icaSts", 2);
            }

            finish(result, state, result.patch.empty() ? "no-change" : "applied", reason);

            return result;
        }

        if (!valid_target(target)) {
            finish(result, state, "unavailable", "target-outside-policy");

            return result;
        }

        const auto kmh = std::llround(target);
        const std::int64_t step = long_press ? 5 : 1;

        if ((id == "cruise_up" && kmh > maximum_speed - step) ||
            (id == "cruise_down" && kmh < minimum_speed + step)) {
            finish(result, state, "no-change", "target-bound");

            return result;
        }

        set_target(result.patch, state, id == "cruise_up" ? kmh + step : kmh - step);
    } else if (id == "cruise_up") {
        if (!valid_target(target)) {
            finish(result, state, "no-change", "no-remembered-target");

            return result;
        }

        bool requested_ica = ica == 2;
        auto reason = readiness(state, requested_ica);

        if (reason == "acc-feature-disabled" || reason == "ica-feature-disabled") {
            finish(result, state, "unavailable", reason);

            return result;
        }

        bool active = reason.empty();
        changed(result.patch, state, "acc_mode", active ? 3 : 2);
        changed(result.patch, state, "mpc_icaSts", requested_ica ? (active ? 3 : 2) : 1);
        finish(result, state, result.patch.empty() ? "no-change" : "applied",
               active ? "resume" : reason);

        return result;
    } else {
        finish(result, state, "no-change", "cruise-not-active");

        return result;
    }

    finish(result, state, result.patch.empty() ? "no-change" : "applied",
           result.patch.empty() ? "unchanged" : "wheel-action");

    return result;
}

AdasOutcome AdasEcu::reconcile(const Json &state) const {
    validate_state(state);
    AdasOutcome result{true};
    result.receipt = base_receipt(state);
    result.receipt["source"] = "vehicle-readiness";

    if (!known(state)) {
        finish(result, state, "unavailable", "fault-or-reserved-status");

        return result;
    }

    bool active = raw(state, "acc_mode") == 3 || raw(state, "mpc_icaSts") == 3;

    if (!active) {
        finish(result, state, "no-change", "not-active");

        return result;
    }

    bool ica = raw(state, "mpc_icaSts") == 3;
    auto reason = readiness(state, ica);

    if (!reason.empty()) {
        bool feature = reason == "acc-feature-disabled" || reason == "ica-feature-disabled";
        changed(result.patch, state, "acc_mode", feature ? 0 : 2);
        changed(result.patch, state, "mpc_icaSts", ica ? (feature ? 1 : 2) : 1);
    }

    finish(result, state, result.patch.empty() ? "no-change" : "applied",
           reason.empty() ? "ready" : reason);

    return result;
}
} // namespace voyemu
