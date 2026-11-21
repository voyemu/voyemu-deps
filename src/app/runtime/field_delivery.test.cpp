#include "field_delivery.hpp"
#include "nxp_presentation.hpp"
#include "passion_mcu_property.hpp"
#include "passion_presentation.hpp"
#include "qnx_mcu_projection.hpp"
#include <iostream>
#include <stdexcept>
using namespace voyemu;

namespace {
void check(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::set<std::string> cluster(const Json &launch) {
    return field_delivery(launch, Json::object()).cluster;
}

std::set<std::string> multimedia(const Json &launch) {
    return field_delivery(Json::object(), launch).multimedia;
}
} // namespace

int main() try {
    std::set<std::string> names;

    for (const auto &field : catalog(VehicleCatalog::qinggan09)) {
        names.insert(field.name);
    }

    check(field_delivery(Json::object(), Json::object()).cluster.empty() &&
                  field_delivery(Json::object(), Json::object()).multimedia.empty(),
          "a machine without guests delivers no field");
    check(cluster({{"schema", 1}}) == names &&
                  cluster({{"hardware_adapter", "sa8155-qinggan09"}}) == names,
          "the qinggan09 mcusimulator reads every field");
    const auto qinggan11_731 = cluster(
            {{"hardware_adapter", "sa8155-qinggan11-731"},
             {"mcu_projection", qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_731)}});
    check(qinggan11_731.size() ==
                          qnx_adapter(QnxGuestAdapter::sa8155_qinggan11_731).mcu_projected_fields &&
                  qinggan11_731.contains("speed") && qinggan11_731.contains("epb_parking") &&
                  qinggan11_731.contains("acc_mode") && qinggan11_731.contains("vcu_ready") &&
                  qinggan11_731.contains("ADCU_TargetType_18") &&
                  !qinggan11_731.contains("set_byte0") && !qinggan11_731.contains("reserved_03") &&
                  !qinggan11_731.contains("eps_calibrationFlag"),
          "qinggan11 7.3.1 delivers the fields of its MCU layout");
    const auto qinggan11_611 = cluster(
            {{"hardware_adapter", "sa8155-qinggan11-611"},
             {"mcu_projection", qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_611)}});
    check(qinggan11_611.size() ==
                          qnx_adapter(QnxGuestAdapter::sa8155_qinggan11_611).mcu_projected_fields &&
                  qinggan11_611.contains("remain_mileage_e") &&
                  qinggan11_611.contains("mrr_fcwSts") && qinggan11_611.contains("acc_mode") &&
                  qinggan11_611.contains("ivi_languageSet") &&
                  qinggan11_611.contains("Driver_SeatBeltSts") &&
                  !qinggan11_611.contains("set_byte9"),
          "qinggan11 6.11.1 delivers the fields of its MCU layout");
    auto earlier = qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_611);
    earlier["projected_fields"] = 493;
    check(cluster({{"hardware_adapter", "sa8155-qinggan11-611"}, {"mcu_projection", earlier}})
                  .empty(),
          "an earlier scope without its own layout names no field");
    check(cluster({{"hardware_adapter", "sa8155-qinggan11-731"}}).empty(),
          "a qinggan11 launch without its projection names no field");
    check(cluster({{"hardware_adapter", "sa8155-passion-21480"}}).empty(),
          "the Passion cluster receives no field");

    const auto canbus = multimedia({{"api", 30}});
    check(canbus == canbus_multimedia_fields() && canbus.contains("speed") &&
                  canbus.contains("bcm_tyreLFPressure") && !canbus.contains("bcm_tyreLFTemp") &&
                  !canbus.contains("rpm"),
          "a Free multimedia delivers the fields its canbus component reads");

    for (const auto &name : canbus) {
        check(names.contains(name), "every canbus field is a catalog field");
    }

    check(multimedia({{"presentation_adapter", nxp_presentation_contract("test-nxp")},
                      {"reference_profile", "test-nxp"},
                      {"platform", "qinggan09"},
                      {"board", "nxp"},
                      {"api", 28},
                      {"home", {{"package", "com.qinggan.app.launcher"}, {"component", nxp_home}}}})
                  .empty(),
          "the NXP multimedia receives no field");
    const Json passion = {
            {"presentation_adapter", passion_presentation_contract("test-passion")},
            {"reference_profile", "test-passion"},
            {"platform", "passion"},
            {"board", "8155"},
            {"api", 30},
            {"home", {{"package", "com.voyah.os.carlauncher"}, {"component", passion_home}}}};
    check(multimedia(passion).empty(), "Passion without its MCU property receives no field");
#ifndef _WIN32
    auto passion_mcu = passion;
    passion_mcu["presentation_adapter"] = passion_presentation_contract("test-passion", true);
    passion_mcu["mcu_property_adapter"] = passion_mcu_capability();
    check(multimedia(passion_mcu) == std::set<std::string>{concept_field(VehicleCatalog::passion,
                                                                         VehicleConcept::charge)} &&
                  catalog_field(VehicleCatalog::passion,
                                "PowerSysInfo.Notify_f_EnergyInform.EnergyInform.SOC"),
          "Passion with its MCU property receives the SOC signal");
#endif

    FieldDelivery delivery{{"speed", "soc"}, {"soc"}};
    const auto status = field_delivery_status(delivery);
    check(status == Json{{"cluster", Json::array({"soc", "speed"})},
                         {"multimedia", Json::array({"soc"})}},
          "a status lists the fields of each guest");
    const auto parsed = field_delivery_from_status(status);
    check(parsed.cluster == delivery.cluster && parsed.multimedia == delivery.multimedia,
          "a status reads back as the same delivery");
    check(field_delivery_json(delivery, "soc") == Json::array({"cluster", "multimedia"}) &&
                  field_delivery_json(delivery, "speed") == Json::array({"cluster"}) &&
                  field_delivery_json(delivery, "odo") == Json::array(),
          "each field names its guests and a model-only field none");
    std::cout << "field delivery: pass\n";

    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
