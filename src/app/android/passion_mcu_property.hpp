#pragma once
#include "android_reboot.hpp"
#include "check.hpp"
#include "model.hpp"
#include "process.hpp"
#include <chrono>
#include <cmath>
#include <functional>
#include <stdexcept>
namespace voyemu {
inline constexpr Check passion_mcu_need{"PASSION_MCU"};

inline Json passion_mcu_capability() {
    return {{"schema", 1},
            {"authority", "com.voyemu.passion.mcu"},
            {"supported_fields", Json::array({"soc"})},
            {"property", 0x0500001d},
            {"area", 0},
            {"type", "Float"},
            {"factory_dex_sha256",
             "bf28b1b80de564392575335b29c59f62fc383bc74001eee3e3b1dbd59459733d"},
            {"manifest_sha256", "d202150ee9301eded31efb9f28645af1dd9c9988875905bc07becc655e2fea85"},
            {"full_wire_verified", false}};
}

inline bool passion_mcu_available(const Json &android) {
    if (!android.contains("mcu_property_adapter")) {
        return false;
    }

#ifdef _WIN32
    throw VoyemuError("PASSION_MCU", "Windows partial projection is unsupported");
#endif
    passion_mcu_need(android.at("api") == 30 &&
                             android.at("mcu_property_adapter") == passion_mcu_capability(),
                     "exact prepared partial SOC capability required");

    return true;
}

// The provider takes the charge concept as soc in whole percent: the SOC signal of the Passion
// catalogue in 0.1 % rounds to the nearest percent.
inline Json passion_mcu_projection(const Json &state, const std::string &boot) {
    validate_state(state);
    passion_mcu_need(android_boot_id_valid(boot), "actual boot required");
    const auto charge = concept_value(state, VehicleConcept::charge);
    passion_mcu_need(charge.has_value(), "charge concept required");

    return {{"schema", 1},
            {"boot", boot},
            {"sequence", state.at("sequence")},
            {"soc", *charge},
            {"source_sha256", sha256_bytes(state.dump())}};
}

inline bool passion_mcu_status(const Json &receipt, const std::string &boot) {
    try {
        if (!receipt.is_object() || !android_boot_id_valid(boot) || receipt.at("schema") != 1 ||
            !receipt.at("schema").is_number_integer() || receipt.at("boot") != boot ||
            receipt.at("service_uid") != 1000 || !receipt.at("service_uid").is_number_integer() ||
            !receipt.at("service_pid").is_number_integer() ||
            receipt.at("service_pid").get<std::int64_t>() <= 0 ||
            receipt.at("full_wire_verified") != false ||
            receipt.at("visible_effect_verified") != false ||
            receipt.at("supported_fields") != Json::array({"soc"})) {
            return false;
        }

        if (receipt.at("status") == "not-ready") {
            return receipt.size() == 8;
        }

        if (receipt.at("status") != "snapshot_published" || receipt.size() != 16 ||
            !receipt.at("sequence").is_number_integer() ||
            receipt.at("sequence").get<std::uint64_t>() < 1 ||
            receipt.at("sequence").get<std::uint64_t>() > UINT32_MAX ||
            !receipt.at("soc").is_number_integer() || receipt.at("soc").get<std::int64_t>() < 0 ||
            receipt.at("soc").get<std::int64_t>() > 100 ||
            !receipt.at("callback_returned").is_boolean() || receipt.at("property") != 0x0500001d ||
            !receipt.at("property").is_number_integer() || receipt.at("area") != 0 ||
            !receipt.at("area").is_number_integer() || receipt.at("type") != "Float") {
            return false;
        }

        const auto sha = receipt.at("source_sha256").get<std::string>();

        if (!sha256_text(sha)) {
            return false;
        }

        return receipt.at("snapshot_readback").is_number() &&
               std::isfinite(receipt.at("snapshot_readback").get<double>()) &&
               receipt.at("snapshot_readback").get<double>() == receipt.at("soc").get<double>();
    } catch (...) {
        return false;
    }
}

inline bool passion_mcu_receipt_matches(const Json &receipt, const Json &state,
                                        const std::string &boot) {
    try {
        const auto input = passion_mcu_projection(state, boot);

        return passion_mcu_status(receipt, boot) && receipt.at("status") == "snapshot_published" &&
               receipt.at("sequence") == input.at("sequence") &&
               receipt.at("soc") == input.at("soc") &&
               receipt.at("source_sha256") == input.at("source_sha256");
    } catch (...) {
        return false;
    }
}

inline std::vector<std::string> passion_mcu_call(const Json *input = nullptr) {
    std::vector<std::string> args{"content",  "call",
                                  "--uri",    "content://com.voyemu.passion.mcu",
                                  "--method", input ? "submit" : "status"};

    if (input) {
        passion_mcu_need(input->is_object() && input->size() == 5, "exact projection required");

        for (const auto &value : std::vector<std::string>{
                     "schema:i:1", "boot:s:" + input->at("boot").get<std::string>(),
                     "sequence:l:" + input->at("sequence").dump(),
                     "soc:i:" + input->at("soc").dump(),
                     "source_sha256:s:" + input->at("source_sha256").get<std::string>()}) {
            args.push_back("--extra");
            args.push_back(value);
        }
    }

    return args;
}

inline std::int64_t passion_mcu_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

inline bool passion_mcu_delivery_current(const Json &value, const std::string &boot,
                                         std::uint32_t sequence, std::int64_t now) {
    try {
        return value.at("projection_acknowledged") == true &&
               value.at("status") == "partial_snapshot_published" && value.at("boot") == boot &&
               value.at("sequence") == sequence &&
               value.at("verified_steady_ms").is_number_integer() &&
               value.at("verified_steady_ms").get<std::int64_t>() >= 0 &&
               now >= value.at("verified_steady_ms").get<std::int64_t>() &&
               now - value.at("verified_steady_ms").get<std::int64_t>() < 4000;
    } catch (...) {
        return false;
    }
}

inline void passion_mcu_expire(Json &android, const std::string &boot, std::uint32_t sequence,
                               std::int64_t now) {
    if (passion_mcu_delivery_current(android.value("vehicle", Json::object()), boot, sequence,
                                     now)) {
        return;
    }

    android["vehicle"] = {
            {"status", "publication_unverified"}, {"supported_fields", Json::array({"soc"})},
            {"projection_acknowledged", false},   {"guest_acknowledged", false},
            {"full_wire_verified", false},        {"visible_effect_verified", false}};

    if (android.value("status", std::string{}) == "presentation_ready") {
        android["status"] = "awaiting_mcu_projection";
    }

    android["guest_acknowledged"] = false;
}

// Every bounded host Android stage rechecks the actual provider incarnation; no source patch is
// needed for recovery.
inline Json passion_mcu_deliver(const std::string &boot, const std::function<Json()> &current,
                                const std::function<void(std::uint32_t)> &advance,
                                const std::function<Json(const std::vector<std::string> &)> &call) {
    auto receipt = call(passion_mcu_call());

    passion_mcu_need(passion_mcu_status(receipt, boot), "invalid retained partial publication");
    auto state = current();

    if (!passion_mcu_receipt_matches(receipt, state, boot)) {
        if (receipt.at("status") == "snapshot_published" &&
            receipt.at("sequence") >= state.at("sequence")) {
            advance(receipt.at("sequence").get<std::uint32_t>());
            state = current();
        }

        const auto input = passion_mcu_projection(state, boot);
        try {
            receipt = call(passion_mcu_call(&input));
        } catch (const std::exception &) {
            receipt = call(passion_mcu_call());
        }
    }

    const auto input = passion_mcu_projection(state, boot);
    passion_mcu_need(passion_mcu_receipt_matches(receipt, state, boot),
                     "exact partial snapshot readback required");

    return {{"status", "partial_snapshot_published"},
            {"sequence", state.at("sequence")},
            {"source", "canonical manual MCU model"},
            {"supported_fields", Json::array({"soc"})},
            {"soc", input.at("soc")},
            {"source_sha256", input.at("source_sha256")},
            {"boot", boot},
            {"service_pid", receipt.at("service_pid")},
            {"verified_steady_ms", passion_mcu_now_ms()},
            {"maximum_age_ms", 4000},
            {"projection_acknowledged", true},
            {"guest_acknowledged", false},
            {"callback_returned", receipt.at("callback_returned")},
            {"client_delivery_verified", false},
            {"visible_effect_verified", false},
            {"full_wire_verified", false},
            {"transport", "original Monza IProperty with protected publication readback"}};
}
} // namespace voyemu
