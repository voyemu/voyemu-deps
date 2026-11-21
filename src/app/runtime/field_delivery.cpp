#include "field_delivery.hpp"
#include "nxp_presentation.hpp"
#include "passion_mcu_property.hpp"
#include "passion_presentation.hpp"
#include "project_files.hpp"
#include "qnx_mcu_projection.hpp"
#include <algorithm>
#include <map>

namespace voyemu {
namespace {
// Catalogue fields that the guest of each qinggan11 adapter writes into the OEM packet.
const std::set<std::string> &qinggan11_projected_fields(const QnxAdapterSpec &spec) {
    static const auto layouts = [] {
        std::map<std::string_view, std::set<std::string>> result;

        for (const auto &adapter : qnx_adapter_registry) {
            if (adapter.platform != QnxGuestPlatform::qinggan11) {
                continue;
            }

            const auto layout = embedded_project_file_json(
                    "src/guest/qnx/" + std::string(adapter.mcu_layout_file), 512 * 1024);
            auto &names = result[adapter.name];

            for (const auto &row : layout.at("fields")) {
                if (!row.contains("unprojected")) {
                    names.insert(row.at("name").get<std::string>());
                }
            }

            if (names.size() != adapter.mcu_projected_fields) {
                throw VoyemuError("FIELD_DELIVERY", "MCU layout differs from its adapter");
            }
        }

        return result;
    }();

    return layouts.at(spec.name);
}

std::set<std::string> cluster_fields(const Json &cluster) {
    if (!cluster.is_object() || cluster.empty()) {
        return {};
    }

    const auto projection = cluster.value("mcu_projection", Json());
    const auto adapter = cluster.value("hardware_adapter", std::string("sa8155-qinggan09"));
    const auto spec = std::find_if(qnx_adapter_registry.begin(), qnx_adapter_registry.end(),
                                   [&](const QnxAdapterSpec &item) {
                                       return item.name == adapter;
                                   });

    if (spec == qnx_adapter_registry.end()) {
        return {};
    }

    // The original mcusimulator of qinggan09 reads every field from its input.
    if (spec->platform == QnxGuestPlatform::qinggan09 && projection.is_null()) {
        std::set<std::string> names;

        for (const auto &field : catalog(VehicleCatalog::qinggan09)) {
            names.insert(field.name);
        }

        return names;
    }

    if (spec->platform == QnxGuestPlatform::qinggan11 &&
        projection == qnx_mcu_projection(spec->adapter)) {
        return qinggan11_projected_fields(*spec);
    }

    return {};
}

std::set<std::string> multimedia_fields(const Json &android) {
    if (!android.is_object() || android.empty() || nxp_presentation(android)) {
        return {};
    }

    // The Passion VHAL provider receives the charge concept of the Passion catalogue.
    if (passion_presentation(android)) {
        return passion_mcu_available(android)
                       ? std::set<std::string>{concept_field(VehicleCatalog::passion,
                                                             VehicleConcept::charge)}
                       : std::set<std::string>{};
    }

    return canbus_multimedia_fields();
}
} // namespace

const std::set<std::string> &canbus_multimedia_fields() {
    static const auto fields = [] {
        std::set<std::string> names = {"speed",
                                       "speed10x",
                                       "soc",
                                       "gear",
                                       "gear_valid",
                                       "ambient_temp",
                                       "odo",
                                       "fuel_percent",
                                       "remain_mileage_e",
                                       "remain_mileage_f",
                                       "vcu_remainingMileageReal",
                                       "vcu_remainingMileageCLTCWLTC",
                                       "hood",
                                       "tail_gate",
                                       "door_fl",
                                       "door_fr",
                                       "door_rl",
                                       "door_rr",
                                       "turn_left",
                                       "turn_right",
                                       "pos_lamp",
                                       "low_beam",
                                       "high_beam",
                                       "rear_fog",
                                       "bcm_DRLStatus",
                                       "mpc_icaSts"};

        for (const std::string corner : {"LF", "RF", "LR", "RR"}) {
            for (const auto &name :
                 {"can_v1_5_bcm_tpms" + corner + "Sts", "bcm_tpms" + corner + "Sts",
                  "bcm_tyre" + corner + "Pressure", "bcm_tpms" + corner + "RAPID_LEAK_Warning",
                  "bcm_tpms" + corner + "LOW_BATT_Warning", "bcm_PressureWarning" + corner}) {
                names.insert(name);
            }
        }

        return names;
    }();

    return fields;
}

FieldDelivery field_delivery(const Json &cluster, const Json &android) {
    return {cluster_fields(cluster), multimedia_fields(android)};
}

Json field_delivery_status(const FieldDelivery &delivery) {
    return {{"cluster", delivery.cluster}, {"multimedia", delivery.multimedia}};
}

FieldDelivery field_delivery_from_status(const Json &status) {
    return {status.at("cluster").get<std::set<std::string>>(),
            status.at("multimedia").get<std::set<std::string>>()};
}

Json field_delivery_json(const FieldDelivery &delivery, const std::string &name) {
    auto guests = Json::array();

    if (delivery.cluster.contains(name)) {
        guests.push_back("cluster");
    }

    if (delivery.multimedia.contains(name)) {
        guests.push_back("multimedia");
    }

    return guests;
}
} // namespace voyemu
