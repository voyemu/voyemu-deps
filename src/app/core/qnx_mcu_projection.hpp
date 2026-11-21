#pragma once
#include "model.hpp"
#include "qnx_adapter.hpp"
#include <optional>
#include <stdexcept>
namespace voyemu {
// The MCU projection of a qinggan11 adapter: the guest writes the projected fields of the
// qinggan11 catalogue into each OEM packet and keeps the OEM bits of the others. The adapter name
// keeps the equal scopes of the two firmwares distinct.
inline Json qnx_mcu_projection(QnxGuestAdapter adapter) {
    const auto &spec = qnx_adapter(adapter);

    if (spec.platform != QnxGuestPlatform::qinggan11) {
        throw VoyemuError("QNX_MCU_PROJECTION", "adapter has no MCU projection");
    }

    const auto fields = static_cast<unsigned>(catalog(VehicleCatalog::qinggan11).size());

    return {{"hardware_adapter", spec.name},
            {"packet_bytes", 650},
            {"wire_fields", fields},
            {"catalog_fields", fields},
            {"projected_fields", spec.mcu_projected_fields},
            {"unsupported_fields", fields - spec.mcu_projected_fields},
            {"unmapped_wire_bits", "preserved-original-each-call"}};
}

// Whether fields is the projected field count of the adapter.
inline bool qnx_mcu_runtime_scope(const QnxAdapterSpec &spec, std::uint64_t fields) {
    return spec.platform == QnxGuestPlatform::qinggan11 && fields == spec.mcu_projected_fields;
}

// A qinggan11 request names its scope by the projected field count; adapter, when given, selects
// among adapters whose layouts project the same count.
inline Json qnx_mcu_request_projection(std::uint64_t packet_bytes, std::uint64_t wire_fields,
                                       std::uint64_t projected_fields,
                                       std::string_view adapter = {}) {
    if (!packet_bytes && !wire_fields && !projected_fields) {
        return nullptr;
    }

    if (packet_bytes == 650 && wire_fields == 736) {
        for (const auto &spec : qnx_adapter_registry) {
            if ((adapter.empty() || spec.name == adapter) &&
                qnx_mcu_runtime_scope(spec, projected_fields)) {
                return qnx_mcu_projection(spec.adapter);
            }
        }
    }

    throw VoyemuError("QNX_MCU_PROJECTION", "unsupported guest scope");
}

inline std::optional<Json> qnx_mcu_delivery_scope(const Json &ack, const Json &expected = Json()) {
    if (!ack.is_object()) {
        return std::nullopt;
    }

    if (!expected.is_null() &&
        (!qnx_mcu_delivery_scope(Json{{"mcu_projection", expected}}) ||
         !ack.contains("mcu_projection") || ack.at("mcu_projection") != expected)) {
        return std::nullopt;
    }

    if (!ack.contains("mcu_projection")) {
        return Json{{"fields", 579}};
    }

    const auto &scope = ack.at("mcu_projection");

    if (!scope.is_object()) {
        return std::nullopt;
    }

    for (const auto *key : {"packet_bytes", "wire_fields", "catalog_fields", "projected_fields",
                            "unsupported_fields"}) {
        if (!scope.contains(key) || !scope.at(key).is_number_integer()) {
            return std::nullopt;
        }
    }

    for (const auto &spec : qnx_adapter_registry) {
        if (spec.platform != QnxGuestPlatform::qinggan11) {
            continue;
        }

        if (scope == qnx_mcu_projection(spec.adapter)) {
            return Json{{"fields", scope.at("projected_fields")},
                        {"mcu_projection", scope},
                        {"full_wire_verified", false}};
        }
    }

    return std::nullopt;
}
} // namespace voyemu
