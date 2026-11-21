#include "model.hpp"
#include "os/fs.hpp"
#include "private_json_reader.hpp"
#include "private_state_security.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace voyemu {
bool valid_profile_name(std::string_view name) {
    auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };

    if (name.empty() || name.size() > 120 || !alnum(name.front())) {
        return false;
    }

    for (char c : name) {
        if (!alnum(c) && c != '-' && c != '_' && c != '.') {
            return false;
        }
    }

    return true;
}

namespace detail {
const std::vector<Field> &qinggan11_catalog();
const std::vector<Field> &passion_catalog();
std::span<const SignalDefinition> passion_signals();
void derive_concepts(VehicleCatalog catalog, Json &raw, const Json &values);
} // namespace detail

VehicleCatalog vehicle_catalog(std::string_view platform) {
    if (platform == "qinggan11") {
        return VehicleCatalog::qinggan11;
    }

    if (platform == "qinggan09") {
        return VehicleCatalog::qinggan09;
    }

    if (platform == "passion") {
        return VehicleCatalog::passion;
    }

    throw VoyemuError("VEHICLE_CATALOG", "unknown platform");
}

const char *catalog_map_sha256(VehicleCatalog catalog) {
    switch (catalog) {
    case VehicleCatalog::qinggan11:
        return qinggan11_map_sha256;
    case VehicleCatalog::passion:
        return passion_map_sha256;
    default:
        return qinggan09_map_sha256;
    }
}

VehicleCatalog state_catalog(const Json &state) {
    const auto map = state.is_object() ? state.find("map_sha256") : state.end();

    for (const auto catalog :
         {VehicleCatalog::qinggan09, VehicleCatalog::qinggan11, VehicleCatalog::passion}) {
        if (map != state.end() && *map == catalog_map_sha256(catalog)) {
            return catalog;
        }
    }

    throw VoyemuError("VEHICLE_SCHEMA", "unknown vehicle catalog");
}

static const std::vector<Field> &qinggan09_catalog() {
    static const std::vector<Field> fields = {
            {1, "ign", 1, 1},
            {2, "turn_left", 1, 0},
            {3, "turn_right", 1, 0},
            {4, "pos_lamp", 1, 0},
            {5, "low_beam", 1, 0},
            {6, "high_beam", 1, 0},
            {7, "hcm_adbFuctionStsLt_bak", 2, 0},
            {8, "hcm_adbFuctionStsRt_bak", 2, 0},
            {9, "rear_fog", 1, 0},
            {10, "epb_fault", 2, 0},
            {11, "bcm_DRLStatus", 1, 0},
            {12, "bms_authenticationResult", 2, 0},
            {13, "bcm_tpms_lamp", 2, 0},
            {14, "brake_oil_low", 1, 0},
            {15, "abs_fault", 1, 0},
            {16, "ebd_fault", 1, 0},
            {17, "epb_parking", 2, 0},
            {18, "autohold_lamp", 3, 0},
            {19, "assist_sys_fault", 2, 0},
            {20, "esc_lamp", 2, 0},
            {21, "esc_off", 1, 0},
            {22, "eps_fault", 2, 0},
            {23, "hdc_lamp", 2, 0},
            {24, "acu_driverSeatBeltSts", 2, 0},
            {25, "acu_passengerSeatBeltSts", 2, 0},
            {26, "RearLeftSeatBelt", 2, 0},
            {27, "RearMidSeatBelt", 2, 0},
            {28, "RearRightSeatBelt", 2, 0},
            {29, "acu_warning", 2, 0},
            {30, "fuel_low_alert", 2, 0},
            {31, "mil_lamp", 2, 0},
            {32, "eng_coolant_temp_high", 2, 0},
            {33, "eng_coolant_temp", 8, 0},
            {34, "engine_oil_low", 1, 0},
            {35, "cool_ant_low", 2, 0},
            {36, "dcdc_systemSts", 4, 0},
            {37, "pwr_bat_soc_low", 1, 0},
            {38, "ready_lamp", 1, 1},
            {39, "mcu0_fault", 1, 0},
            {40, "turtle_lamp", 2, 0},
            {41, "vcu_alertLevel", 3, 0},
            {42, "bms_alert", 2, 0},
            {43, "isolationFault", 2, 0},
            {44, "acc_mode", 3, 0},
            {45, "mpc_icaSts", 3, 0},
            {46, "ivi_overspdWarningSet", 8, 0},
            {47, "overspdWarning", 2, 0},
            {48, "mpc_isa_speed_limit", 6, 0},
            {49, "mpc_isaOnOffSts", 2, 0},
            {50, "isa_limit_warning", 1, 0},
            {51, "bcm_pepsPowerMode", 3, 0},
            {52, "reserve_0_5", 2, 0},
            {53, "aeb_sts", 3, 0},
            {54, "mrr_fcwSts", 3, 0},
            {55, "ldws_lamp", 3, 0},
            {56, "esc_hbbSts", 3, 0},
            {57, "asc_ASC_errorSts", 3, 0},
            {58, "vcu_CCStas", 2, 0},
            {59, "charge_lamp", 1, 0},
            {60, "bms_state", 4, 0},
            {61, "mcuf0_overTemperature", 1, 0},
            {62, "vcu_tcmFail", 1, 0},
            {63, "sound_enable", 1, 0},
            {64, "hcm_leftHighBeamFaultSt", 2, 0},
            {65, "hcm_rightHighBeamFaultSt", 1, 0},
            {66, "hcm_leftLowBeamFaultSt", 1, 0},
            {67, "hcm_rightLowBeamFaultSt", 1, 0},
            {68, "hcm_turnLampWarningLt", 1, 0},
            {69, "hcm_turnLampWarningRt", 1, 0},
            {70, "bcm_rllmLeftTurnLampFault", 1, 0},
            {71, "bcm_rrlmRightTurnLampFault", 1, 0},
            {72, "bcm_rmlmleftTurnLampFault", 1, 0},
            {73, "bcm_rmlmrightTurnLampFault", 1, 0},
            {74, "hcm_middleLeftPositionLampFault", 1, 0},
            {75, "hcm_positionLampWarningLt", 1, 0},
            {76, "hcm_middleRightPositionLampFault", 1, 0},
            {77, "hcm_positionLampWarningRt", 1, 0},
            {78, "bcm_rllmPositionLampFault", 1, 0},
            {79, "bcm_rmlmpositionLampFault", 1, 0},
            {80, "bcm_rrlmpositionLampFault", 1, 0},
            {81, "hcm_dayTimeRunningLampWarningLt", 1, 0},
            {82, "hcm_dayTimeRunningLampWarningRt", 1, 0},
            {83, "hcm_alsErrorStsLt", 1, 0},
            {84, "hcm_alsErrorStsRt", 1, 0},
            {85, "bcm_rllmstopLampFault", 2, 0},
            {86, "bcm_rmlmstopLampFault", 1, 0},
            {87, "bcm_rmlmhighStopLampFault", 1, 0},
            {88, "bcm_rrlmstopLampFault", 1, 0},
            {89, "bcm_rllmfogLampFault", 1, 0},
            {90, "bcm_rrlmfogLampFault", 1, 0},
            {91, "bcm_rllmreverseLampFault", 1, 0},
            {92, "bcm_rrlmreverseLampFault", 1, 0},
            {93, "mrr_r1_rearCollisionWarning", 1, 0},
            {94, "esc_hhcActive", 1, 0},
            {95, "bsd_lca_sts", 2, 0},
            {96, "bsd_lca_txt", 2, 0},
            {97, "dow_txt", 2, 0},
            {98, "rcta_txt", 2, 0},
            {99, "rcw_txt", 2, 0},
            {100, "mrr_r1_systemErrorSts", 2, 0},
            {101, "washing_liquid_low", 1, 0},
            {102, "ev_lamp", 2, 2},
            {103, "break_warning", 1, 0},
            {104, "hood", 1, 0},
            {105, "door_fl", 1, 0},
            {106, "door_fr", 1, 0},
            {107, "door_rl", 1, 0},
            {108, "door_rr", 1, 0},
            {109, "tail_gate", 1, 0},
            {110, "fuel_cap", 1, 1},
            {111, "charge_cap", 1, 0},
            {112, "volOrSOCLowAlarm", 2, 0},
            {113, "vcu_batteryOverTemperature", 1, 0},
            {114, "etc_disassemblySts", 1, 0},
            {115, "etc_lockSts", 1, 0},
            {116, "etc_detect", 1, 0},
            {117, "etc_lamp", 1, 0},
            {118, "etc_tranStauts", 2, 0},
            {119, "bms_f003_packOverCrntChg", 1, 0},
            {120, "icm_dmsWearyLeverSts", 3, 0},
            {121, "gsm_warnInformation_bak", 2, 0},
            {122, "gsm_faultLevel", 2, 0},
            {123, "maintenance_sts_km", 1, 0},
            {124, "maintenance_sts_day", 1, 0},
            {125, "remain_maintenance_km", 9, 0},
            {126, "remain_maintenance_days", 4, 0},
            {127, "Maintain_Reminder", 1, 0},
            {128, "maintenance", 2, 0},
            {129, "can_v1_5_bcm_tpmsLFSts", 3, 0},
            {130, "bcm_tyreLFPressure", 8, 0},
            {131, "bcm_tyreLFTemp", 8, 0},
            {132, "can_v1_5_bcm_tpmsRFSts", 3, 0},
            {133, "bcm_tyreRFPressure", 8, 0},
            {134, "bcm_tyreRFTemp", 8, 0},
            {135, "can_v1_5_bcm_tpmsLRSts", 3, 0},
            {136, "bcm_tyreLRPressure", 8, 0},
            {137, "bcm_tyreLRTemp", 8, 0},
            {138, "can_v1_5_bcm_tpmsRRSts", 3, 0},
            {139, "bcm_tyreRRPressure", 8, 0},
            {140, "bcm_tyreRRTemp", 8, 0},
            {141, "reserve_267_5", 5, 0},
            {142, "pos_lamp_tips", 1, 0},
            {143, "bcm_sl_errSts", 4, 0},
            {144, "peps_key_not_incar", 1, 0},
            {145, "peps_key_serch_fail", 1, 0},
            {146, "immo_release", 1, 0},
            {147, "peps_key_close", 1, 0},
            {148, "peps_low_bat", 1, 0},
            {149, "peps_relay_err", 1, 0},
            {150, "epb_text", 4, 0},
            {151, "esc_hhcAvailable", 1, 0},
            {152, "avh_available", 1, 0},
            {153, "avh_stdby", 2, 0},
            {154, "bcm_12VBatterySOC", 8, 0},
            {155, "esc_hdcAvailable", 1, 0},
            {156, "vcu_towMode", 1, 0},
            {157, "vsp_errorIdentification", 1, 0},
            {158, "peps_key_incar", 1, 0},
            {159, "dc_charge_state", 4, 0},
            {160, "ac_charge_state", 4, 0},
            {161, "ac_charge_connect", 1, 0},
            {162, "dc_charge_connect", 1, 0},
            {163, "remain_charge_time", 10, 0},
            {164, "bms_pPack", 14, 0},
            {165, "obc_chargeVoltage", 17, 0},
            {166, "obc_chargeCurrent", 13, 0},
            {167, "vcu_consumpRatio50km", 8, 0},
            {168, "ivi_maxChrgSocSet", 8, 0},
            {169, "bms_chargeFormSts", 3, 0},
            {170, "ivi_ChargeEndTimeHour", 4, 0},
            {171, "ivi_ChargeStartTimeHour", 4, 0},
            {172, "ivi_ChargeEndTimeHalfDay", 1, 0},
            {173, "ivi_ChargeStartTimeHalfDay", 1, 0},
            {174, "ivi_ChargeEndTimeMinute", 6, 0},
            {175, "ivi_ChargeStartTimeMinute", 6, 0},
            {176, "obc_state", 4, 0},
            {177, "reserve_2_8", 4, 0},
            {178, "las_status", 3, 0},
            {179, "left_visualization", 2, 0},
            {180, "right_visualization", 2, 0},
            {181, "lasWarningModeSelection", 3, 0},
            {182, "mpc_laneAssitType", 3, 0},
            {183, "mrr_ccFuntionMode", 2, 0},
            {184, "mpc_LL_Line_Curvature", 7, 0},
            {185, "mpc_RR_Line_Curvature", 7, 0},
            {186, "mpc_LAS_roadCurvature", 7, 0},
            {187, "llane_marker_type", 3, 0},
            {188, "rlane_marker_type", 3, 0},
            {189, "mpc_LL_LaneMarkerType", 3, 0},
            {190, "mpc_RR_LaneMarkerType", 3, 0},
            {191, "llane_distance", 6, 0},
            {192, "rlane_distance", 6, 0},
            {193, "mrr_obj0_wExist", 3, 0},
            {194, "mrr_obj0_TargetType", 2, 0},
            {195, "mrr_obj0_dx", 8, 0},
            {196, "mrr_obj0_dy", 8, 0},
            {197, "mrr_obj0_Vx", 9, 0},
            {198, "mrr_obj1_wExist", 3, 0},
            {199, "mrr_obj1_TargetType", 2, 0},
            {200, "mrr_obj1_dx", 8, 0},
            {201, "mrr_obj1_dy", 8, 0},
            {202, "mrr_obj1_Vx", 9, 0},
            {203, "mrr_obj2_wExist", 3, 0},
            {204, "mrr_obj2_TargetType", 2, 0},
            {205, "mrr_obj2_dx", 8, 0},
            {206, "mrr_obj2_dy", 8, 0},
            {207, "mrr_obj2_Vx", 9, 0},
            {208, "mrr_obj3_wExist", 3, 0},
            {209, "mrr_obj3_TargetType", 2, 0},
            {210, "mrr_obj3_dx", 8, 0},
            {211, "mrr_obj3_dy", 8, 0},
            {212, "mrr_obj3_Vx", 9, 0},
            {213, "mrr_r1_targetStsLR", 1, 0},
            {214, "mrr_r1_targetDangerLevelLR", 2, 0},
            {215, "mrr_r1_targetLongDisLR", 8, 0},
            {216, "mrr_r1_targetLateralDistanceLR", 8, 0},
            {217, "mrr_r1_targetTypeLR", 3, 0},
            {218, "mrr_r1_targetStsRR", 1, 0},
            {219, "mrr_r1_targetDangerLevelRR", 2, 0},
            {220, "mrr_r1_targetLongDisRR", 8, 0},
            {221, "mrr_r1_targetLateralDistanceRR", 8, 0},
            {222, "mrr_r1_targetTypeRR", 3, 0},
            {223, "adas_takeover_req", 2, 0},
            {224, "mpc_text_infor", 5, 0},
            {225, "mrr_vSetDis", 9, 0},
            {226, "time_gap_set", 3, 0},
            {227, "acc_takeover_req", 1, 0},
            {228, "mrr_text_info", 5, 0},
            {229, "pebs_warning", 3, 0},
            {230, "mrr_fcwOnOffSts", 2, 0},
            {231, "vcu_vDisSet", 8, 0},
            {232, "vcu_ccState", 4, 0},
            {233, "wcm_reminder", 1, 0},
            {234, "obc_ccOhm", 4, 0},
            {235, "bcm_readyReq", 1, 0},
            {236, "Tbox_SysErSt", 2, 0},
            {237, "vcu_KL15offReq", 1, 0},
            {238, "reserve_194", 1, 0},
            {239, "ASC_SusTempAdjust", 1, 0},
            {240, "icm_dmsCameraSts", 3, 0},
            {241, "icm_dmsSts", 3, 0},
            {242, "icm_dmsDistractLeverSts", 2, 0},
            {243, "icm_dmsDrvBehaviorMon", 3, 0},
            {244, "bcm_IndicationPressBrake", 2, 0},
            {245, "bcm_LowFrequencyAntennaFault", 2, 0},
            {246, "mpc_isaMode", 3, 0},
            {247, "driving_long_tips", 1, 0},
            {248, "bcm_remoteForbidden", 2, 0},
            {249, "vcu_GearPosNotEqual", 3, 0},
            {250, "pdc_OnOffSts", 2, 0},
            {251, "reserve_3_6", 1, 0},
            {252, "MCU_Software_Project_differ", 3, 0},
            {253, "trip_ax", 24, 0},
            {254, "tripax_avg_pwr", 11, 0},
            {255, "tripax_avg_spd", 8, 0},
            {256, "tripax_avg_fuelconsump", 8, 0},
            {257, "tripax_time_min", 13, 0},
            {258, "trip_fuel", 24, 0},
            {259, "tripfuel_avg_pwr", 11, 0},
            {260, "tripfuel_avg_spd", 8, 0},
            {261, "tripfuel_avg_fuelconsump", 8, 0},
            {262, "tripfuel_time_min", 13, 0},
            {263, "trip_interval_ready", 24, 0},
            {264, "current_trip_avg_pwr", 11, 0},
            {265, "current_trip_avg_spd", 8, 0},
            {266, "current_trip_avg_fuelconsump", 8, 0},
            {267, "current_trip_time_min", 13, 0},
            {268, "trip_interval_charge", 24, 0},
            {269, "last_charge_avg_pwr", 11, 0},
            {270, "last_charge_avg_fuelconsump", 8, 0},
            {271, "last_charge_avg_spd", 8, 0},
            {272, "last_charge_time_min", 13, 0},
            {273, "vcu_consumpRatioInstant", 11, 0},
            {274, "vcu_consumpRatioInstant_last1km", 11, 0},
            {275, "instant_value_unit", 2, 0},
            {276, "fuel_instant", 8, 0},
            {277, "fuel_instant_last1km", 8, 0},
            {278, "instant_value_trip", 10, 0},
            {279, "rpm", 13, 0},
            {280, "bms_stEgyFlowSysDischRgn", 2, 0},
            {281, "bms_stEgyFlowOBCAndBatt", 2, 0},
            {282, "mcuf0_stEgyFlowAndBatt", 2, 0},
            {283, "mcur0_stEgyFlowAndBatt", 2, 0},
            {284, "vcu_stEgyFlowEngAndGenMot", 2, 0},
            {285, "vcu_stEgyFlowGenMotAndBatt", 2, 0},
            {286, "remain_mileage_e", 11, 0},
            {287, "remain_mileage_f", 10, 0},
            {288, "gear_valid", 1, 3},
            {289, "gear", 8, 2},
            {290, "odo", 24, 0},
            {291, "ambient_temp", 8, 0},
            {292, "vcu_driveMode", 3, 0},
            {293, "ivi_naviDirection", 4, 0},
            {294, "ivi_naviAltitude", 7, 0},
            {295, "reserve_281_10", 10, 0},
            {296, "ac_driverTempC_off", 1, 0},
            {297, "ac_driverTempC", 5, 0},
            {298, "ivi_timeInfoSecond", 6, 0},
            {299, "ivi_timeInfoMinute", 6, 0},
            {300, "ivi_timeInfoHour", 5, 0},
            {301, "ivi_timeInfoDay", 5, 0},
            {302, "ivi_timeInfoMonth", 4, 0},
            {303, "ivi_timeInfoYear", 6, 0},
            {304, "ivi_timeValid", 1, 0},
            {305, "nvs_displayReq", 3, 0},
            {306, "nvs_Sts", 2, 0},
            {307, "nvs_cameraSts", 2, 0},
            {308, "nvs_warnningSts", 1, 0},
            {309, "nvs_textPrompt", 4, 0},
            {310, "spd_valid", 1, 1},
            {311, "speed", 8, 50},
            {312, "fuel_percent", 7, 0},
            {313, "soc", 10, 0},
            {314, "soc_warning", 2, 0},
            {315, "allowed_power", 7, 0},
            {316, "actual_power", 7, 0},
            {317, "recovery_power", 7, 0},
            {318, "bcm_sl_currentPos", 2, 1},
            {319, "IHBC_status", 3, 0},
            {320, "poweroff_req", 2, 0},
            {321, "ivi_languageSet", 2, 0},
            {322, "swm_Photo", 1, 0},
            {323, "swm_menu", 1, 0},
            {324, "swm_OK", 1, 0},
            {325, "swm_upAdd", 1, 0},
            {326, "swm_downMinus", 1, 0},
            {327, "swm_leftLast", 1, 0},
            {328, "swm_rightNext", 1, 0},
            {329, "swm_nvsSwitch", 1, 0},
            {330, "swm_phone", 1, 0},
            {331, "swm_voice", 1, 0},
            {332, "ic_bat_voltage", 12, 0},
            {333, "esc_vehicleSpeed", 13, 0},
            {334, "nvs_warningXCoordinate", 12, 0},
            {335, "nvs_warningYCoordinate", 10, 0},
            {336, "nvs_warningType", 3, 0},
            {337, "bcm_tpmsLFSts", 2, 0},
            {338, "bcm_tpmsRFSts", 2, 0},
            {339, "bcm_PressureWarningLF", 2, 0},
            {340, "bcm_PressureWarningRF", 2, 0},
            {341, "bcm_TireTempWarningLF", 1, 0},
            {342, "bcm_TireTempWarningRF", 1, 0},
            {343, "bcm_tpmsLFLOW_BATT_Warning", 1, 0},
            {344, "bcm_tpmsRFLOW_BATT_Warning", 1, 0},
            {345, "bcm_tpmsLFRAPID_LEAK_Warning", 1, 0},
            {346, "bcm_tpmsRFRAPID_LEAK_Warning", 1, 0},
            {347, "bcm_tpmsLRSts", 2, 0},
            {348, "bcm_tpmsRRSts", 2, 0},
            {349, "bcm_PressureWarningLR", 2, 0},
            {350, "bcm_PressureWarningRR", 2, 0},
            {351, "bcm_TireTempWarningLR", 1, 0},
            {352, "bcm_TireTempWarningRR", 1, 0},
            {353, "bcm_tpmsLRLOW_BATT_Warning", 1, 0},
            {354, "bcm_tpmsRRLOW_BATT_Warning", 1, 0},
            {355, "bcm_tpmsLRRAPID_LEAK_Warning", 1, 0},
            {356, "bcm_tpmsRRRAPID_LEAK_Warning", 1, 0},
            {357, "BMS_ThrmlRunawaySt", 2, 0},
            {358, "upgradeMCU", 8, 0},
            {359, "speed10x", 12, 5000},
            {360, "allowed_power10x", 12, 0},
            {361, "actual_power10x", 12, 0},
            {362, "recovery_power10x", 12, 0},
            {363, "IVI_remainingMileageswitch", 2, 0},
            {364, "vcu_remainingMileageReal", 11, 0},
            {365, "vcu_brakePedalPressed", 1, 0},
            {366, "VCU_StandStillPreheatBattWarn", 1, 0},
            {367, "VCU_LowTempInhibitPowerUpWarn", 1, 0},
            {368, "lowBatChargeFault", 2, 0},
            {369, "tbox_vehicleMode", 4, 0},
            {370, "OTA_mode", 2, 0},
            {371, "hcm_adbFuctionStsLt", 3, 0},
            {372, "hcm_adbFuctionStsRt", 3, 0},
            {373, "asc_AirPumpOverHeatWarning", 2, 0},
            {374, "remain_mileage_f_nedc", 10, 0},
            {375, "gsm_warnInformation", 3, 0},
            {376, "asc_ascSysSts", 3, 0},
            {377, "odoRev", 24, 100},
            {378, "etc_dataSts", 8, 0},
            {379, "vcu_ExhibitionSts", 2, 2},
            {380, "fuel_cap_err", 2, 1},
            {381, "alarm_play_req", 12, 1},
            {382, "apa_WarningReq", 1, 0},
            {383, "APA_PDCSysSts", 2, 0},
            {384, "APA_PDCFailureWarning", 1, 0},
            {385, "APA_PDCAudibleFrontBeepRate", 3, 0},
            {386, "APA_PDCAudibleRearBeepRate", 3, 0},
            {387, "pdc_sysSts", 2, 0},
            {388, "pdc_failureWarning", 1, 0},
            {389, "pdc_audibleFrontBeepRate", 3, 0},
            {390, "pdc_audibleRearBeepRate", 3, 0},
            {391, "SeatBeltLamp", 2, 0},
            {392, "SeatBeatFlash", 1, 0},
            {393, "mrr_r1_leftBSDLCAWarning", 2, 0},
            {394, "mrr_r1_rightBSDLCAWarning", 2, 0},
            {395, "mrr_r1_leftDOWWarnning", 2, 0},
            {396, "mrr_r1_rightDOWWarnning", 2, 0},
            {397, "mrr_r1_leftRCTAWarnning", 1, 0},
            {398, "mrr_r1_rightRCTAWarnning", 1, 0},
            {399, "soc_menu_info", 8, 0},
            {400, "icyroad_reminder", 2, 0},
            {401, "vcu_hvilWarningIndicate", 2, 0},
            {402, "vcu_remainingMileageCLTCWLTC", 11, 0},
            {403, "Trailer_EnabState_St", 1, 0},
            {404, "Trailer_EnabState_Prompt", 2, 0},
            {405, "remain_mileage_f_wltc", 10, 0},
            {406, "bms_batteryConditionFail", 1, 0},
            {407, "bms_f054_dcChrgOverTemp", 1, 0},
            {408, "reset_dsp", 1, 0},
            {409, "apa_AbortWarningReq", 1, 0},
            {410, "apa_ActiveWarningReq", 1, 0},
            {411, "apa_avmSts", 4, 0},
            {412, "apa_avmHmiIndex", 4, 0},
            {413, "apa_avmViewMode", 2, 0},
            {414, "apa_avmDisplayReq", 2, 0},
            {415, "apa_avmMODSts", 2, 0},
            {416, "apa_avmTrigByTurnningSts", 1, 0},
            {417, "apa_avmSingleViewSts", 5, 0},
            {418, "apa_avmTrajectory_display", 1, 0},
            {419, "apa_avmAutoswitching_display", 1, 0},
            {420, "apa_avmCarBodyTransparent", 1, 0},
            {421, "apa_avmSwSts", 1, 0},
            {422, "apa_avmMonitVideoOutput", 1, 0},
            {423, "apa_avmPDCfailureWarn", 3, 0},
            {424, "avm_sts", 4, 0},
            {425, "avm_hmiIndex", 4, 0},
            {426, "avm_viewMode", 2, 0},
            {427, "avm_displayReq", 2, 0},
            {428, "avm_MODSts", 2, 0},
            {429, "avm_trigByTurnningSts", 1, 0},
            {430, "avm_singleViewSts", 5, 0},
            {431, "avm_Trajectory_display", 1, 0},
            {432, "avm_Autoswitching_display", 1, 0},
            {433, "avm_CarBodyTransparent", 1, 0},
            {434, "avm_SwSts", 1, 0},
            {435, "avm_QuitSw", 1, 0},
            {436, "OBC_halfConnectWarn", 2, 0},
            {437, "VCU_SOCmodeSet", 3, 0},
            {438, "trip_ax_rev", 20, 0},
            {439, "trip_current_rev", 20, 0},
            {440, "charge_connect_warn", 2, 0},
            {441, "vcu_textinfo", 3, 0},
            {442, "reboot_request", 2, 0},
            {443, "can_msg_timeout_111", 2, 0},
            {444, "can_msg_timeout_114", 2, 0},
            {445, "can_msg_timeout_130", 2, 0},
            {446, "can_msg_timeout_140", 2, 0},
            {447, "can_msg_timeout_144", 2, 0},
            {448, "can_msg_timeout_212", 2, 0},
            {449, "can_msg_timeout_219", 2, 0},
            {450, "can_msg_timeout_21a", 2, 0},
            {451, "can_msg_timeout_239", 2, 0},
            {452, "can_msg_timeout_23b", 2, 0},
            {453, "can_msg_timeout_241", 2, 0},
            {454, "can_msg_timeout_2a1", 2, 0},
            {455, "can_msg_timeout_2a2", 2, 0},
            {456, "can_msg_timeout_2ac", 2, 0},
            {457, "can_msg_timeout_2c1", 2, 0},
            {458, "can_msg_timeout_2c2", 2, 0},
            {459, "can_msg_timeout_301", 2, 0},
            {460, "can_msg_timeout_306", 2, 0},
            {461, "can_msg_timeout_309", 2, 0},
            {462, "can_msg_timeout_319", 2, 0},
            {463, "can_msg_timeout_30e", 2, 0},
            {464, "can_msg_timeout_322", 2, 0},
            {465, "can_msg_timeout_323", 2, 0},
            {466, "can_msg_timeout_324", 2, 0},
            {467, "can_msg_timeout_349", 2, 0},
            {468, "can_msg_timeout_35a", 2, 0},
            {469, "can_msg_timeout_36e", 2, 0},
            {470, "can_msg_timeout_369", 2, 0},
            {471, "can_msg_timeout_37e", 2, 0},
            {472, "can_msg_timeout_380", 2, 0},
            {473, "can_msg_timeout_38d", 2, 0},
            {474, "can_msg_timeout_394", 2, 0},
            {475, "can_msg_timeout_396", 2, 0},
            {476, "can_msg_timeout_3a0", 2, 0},
            {477, "can_msg_timeout_3a1", 2, 0},
            {478, "can_msg_timeout_3a9", 2, 0},
            {479, "can_msg_timeout_3ad", 2, 0},
            {480, "can_msg_timeout_3ae", 2, 0},
            {481, "can_msg_timeout_3b6", 2, 0},
            {482, "can_msg_timeout_3be", 2, 0},
            {483, "can_msg_timeout_3c1", 2, 0},
            {484, "can_msg_timeout_3c2", 2, 0},
            {485, "can_msg_timeout_407", 2, 0},
            {486, "can_msg_timeout_411", 2, 0},
            {487, "can_msg_timeout_421", 2, 0},
            {488, "can_msg_timeout_431", 2, 0},
            {489, "can_msg_timeout_441", 2, 0},
            {490, "can_msg_timeout_511", 2, 0},
            {491, "can_msg_timeout_514", 2, 0},
            {492, "can_msg_timeout_561", 2, 0},
            {493, "can_msg_timeout_07e", 2, 0},
            {494, "can_msg_timeout_0a5", 2, 0},
            {495, "can_msg_timeout_2fa", 2, 0},
            {496, "can_msg_timeout_331", 2, 0},
            {497, "can_msg_timeout_387", 2, 0},
            {498, "can_msg_timeout_3b5", 2, 0},
            {499, "can_msg_timeout_3fe", 2, 0},
            {500, "can_msg_timeout_1A0", 2, 0},
            {501, "can_msg_timeout_1A3", 2, 0},
            {502, "can_msg_timeout_35f", 2, 0},
            {503, "can_msg_timeout_334", 2, 0},
            {504, "can_msg_timeout_203", 2, 0},
            {505, "can_msg_timeout_208", 2, 0},
            {506, "set_byte0", 8, 0},
            {507, "set_byte1", 8, 0},
            {508, "set_byte2", 8, 0},
            {509, "set_byte3", 8, 0},
            {510, "set_byte4", 8, 0},
            {511, "set_byte5", 8, 0},
            {512, "set_byte6", 8, 0},
            {513, "set_byte7", 8, 0},
            {514, "set_byte8", 8, 0},
            {515, "set_byte9", 8, 0},
            {516, "cfg_driver_mode", 2, 1},
            {517, "cfg_tbox", 2, 1},
            {518, "cfg_driver_style", 3, 1},
            {519, "cfg_hud", 1, 1},
            {520, "cfg_driver_source", 4, 1},
            {521, "cfg_wireless_charge", 2, 1},
            {522, "cfg_glass_cover", 2, 1},
            {523, "cfg_sunroof_type", 2, 1},
            {524, "cfg_child_lock", 2, 1},
            {525, "cfg_driver_seat_heat", 1, 1},
            {526, "cfg_driver_seat_air", 1, 1},
            {527, "cfg_driver_seat_massage", 1, 1},
            {528, "cfg_driver_seat_memory", 1, 1},
            {529, "cfg_copilot_seat_heat", 1, 1},
            {530, "cfg_copilot_seat_air", 1, 1},
            {531, "cfg_copilot_seat_massage", 1, 1},
            {532, "cfg_copilot_seat_memory", 1, 1},
            {533, "cfg_seat", 3, 1},
            {534, "cfg_sound_band", 1, 1},
            {535, "cfg_charge_port_type", 2, 1},
            {536, "cfg_gas_port_type", 2, 1},
            {537, "cfg_speaker", 3, 1},
            {538, "cfg_air_suspension", 1, 1},
            {539, "cfg_vehicle_color", 4, 1},
            {540, "cfg_maintain", 2, 1},
            {541, "cfg_auto_beam", 1, 1},
            {542, "cfg_auto_beam_height", 1, 1},
            {543, "cfg_env_lamp", 3, 1},
            {544, "cfg_left_right_hand", 1, 1},
            {545, "cfg_auto_headlight", 1, 1},
            {546, "cfg_matrix_lamp", 1, 1},
            {547, "cfg_auto_parking", 1, 1},
            {548, "cfg_ar", 1, 1},
            {549, "cfg_sound_position", 2, 1},
            {550, "cfg_iaqc", 1, 1},
            {551, "cfg_air_filter", 1, 1},
            {552, "cfg_air_flesh", 1, 1},
            {553, "cfg_dvr", 1, 1},
            {554, "cfg_etc_cfg", 1, 1},
            {555, "cfg_ac_auto_cycle", 1, 1},
            {556, "cfg_nvs", 1, 1},
            {557, "cfg_backmonitor", 1, 1},
            {558, "cfg_blis", 1, 1},
            {559, "cfg_ldw", 1, 1},
            {560, "cfg_pre_crash", 2, 1},
            {561, "cfg_lka", 1, 1},
            {562, "cfg_lca", 1, 1},
            {563, "cfg_acc", 2, 1},
            {564, "cfg_aeb", 1, 1},
            {565, "cfg_door_warning", 1, 1},
            {566, "cfg_traffic_sign", 1, 1},
            {567, "cfg_isa", 1, 1},
            {568, "cfg_rcta", 1, 1},
            {569, "cfg_tja_assis", 1, 1},
            {570, "cfg_dms_id", 1, 1},
            {571, "cfg_gesture", 1, 1},
            {572, "cfg_sale_area", 6, 1},
            {573, "cfg_screen", 2, 1},
            {574, "cfg_mrr", 1, 1},
            {575, "cfg_mpc", 1, 1},
            {576, "cfg_adas_supplier", 4, 0},
            {577, "cfg_reserve_12", 8, 1},
            {578, "cfg_reserve_13", 8, 1},
            {579, "cfg_reserve_14", 8, 1},
    };

    return fields;
}

const std::vector<Field> &catalog(VehicleCatalog catalog) {
    switch (catalog) {
    case VehicleCatalog::qinggan11:
        return detail::qinggan11_catalog();
    case VehicleCatalog::passion:
        return detail::passion_catalog();
    default:
        return qinggan09_catalog();
    }
}

const Field *catalog_field(VehicleCatalog catalog, std::string_view name) {
    static const auto indexes = [] {
        std::array<std::map<std::string, const Field *, std::less<>>, 3> result;

        for (const auto which :
             {VehicleCatalog::qinggan09, VehicleCatalog::qinggan11, VehicleCatalog::passion}) {
            for (const auto &field : voyemu::catalog(which)) {
                result[static_cast<std::size_t>(which)].emplace(field.name, &field);
            }
        }

        return result;
    }();
    const auto &index = indexes[static_cast<std::size_t>(catalog)];
    const auto found = index.find(name);

    return found == index.end() ? nullptr : found->second;
}

std::span<const SignalDefinition> catalog_signals(VehicleCatalog catalog) {
    return catalog == VehicleCatalog::passion ? detail::passion_signals()
                                              : std::span<const SignalDefinition>{};
}

Json catalog_json(VehicleCatalog catalog) {
    Json output = Json::array();

    for (const auto &field : voyemu::catalog(catalog)) {
        output.push_back({{"index", field.index},
                          {"name", field.name},
                          {"width", field.width},
                          {"default", field.initial}});
    }

    return output;
}

std::string utc_now() {
    const auto value = std::time(nullptr);
    std::tm tm{};

#ifdef _WIN32
    gmtime_s(&tm, &value);
#else
    gmtime_r(&value, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%S+00:00", &tm);

    return buffer;
}

static bool integer(const Json &value) {
    return value.is_number_unsigned() ||
           (value.is_number_integer() && value.get<std::int64_t>() >= 0);
}

void validate_state(const Json &state) {
    const std::set<std::string> keys{"schema",     "sequence", "updated_utc",
                                     "map_sha256", "origin",   "raw_oem"};

    if (!state.is_object() || state.size() != keys.size()) {
        throw VoyemuError("VEHICLE_SCHEMA", "complete state required");
    }

    for (const auto &key : keys) {
        if (!state.contains(key)) {
            throw VoyemuError("VEHICLE_SCHEMA", "missing " + key);
        }
    }

    if (!integer(state["schema"]) || state["schema"] != 1 || state["origin"] != "manual-emulator") {
        throw VoyemuError("VEHICLE_SCHEMA", "schema/map/origin mismatch");
    }

    const auto catalog = state_catalog(state);
    const auto &fields = voyemu::catalog(catalog);

    if (!integer(state["sequence"]) || state["sequence"].get<std::uint64_t>() == 0 ||
        state["sequence"].get<std::uint64_t>() > UINT32_MAX) {
        throw VoyemuError("VEHICLE_SEQUENCE", "out of range");
    }

    if (!state["updated_utc"].is_string()) {
        throw VoyemuError("VEHICLE_TIMESTAMP", "UTC string required");
    }

    const auto stamp = state["updated_utc"].get<std::string>();

    if (stamp.size() < 20 || stamp.size() > 40 ||
        !(stamp.ends_with("Z") || stamp.ends_with("+00:00"))) {
        throw VoyemuError("VEHICLE_TIMESTAMP", "UTC required");
    }

    const auto &raw = state["raw_oem"];

    if (!raw.is_object() || raw.size() != fields.size()) {
        throw VoyemuError("VEHICLE_FIELDS", "all catalog fields required");
    }

    for (const auto &field : fields) {
        if (!raw.contains(field.name) || !integer(raw[field.name]) ||
            raw[field.name].get<std::uint64_t>() > field_maximum(field.width)) {
            throw VoyemuError("VEHICLE_WIDTH", field.name);
        }

        if (field.name.starts_with("swm_") && raw[field.name] != 0) {
            throw VoyemuError("VEHICLE_EVENT", "wheel presses cannot persist");
        }
    }

    if (!concepts_consistent(catalog, raw)) {
        throw VoyemuError("VEHICLE_COUPLED", "speed/SOC invalid");
    }
}

Json normal_initial_values(VehicleCatalog catalog) {
    using enum VehicleConcept;
    // Emulator fixture values; SOC, fuel and tyre readings are not OEM recommended defaults.
    Json values{{"vcu_ExhibitionSts", 0}, {"ambient_temp", 120},  {"fuel_percent", 50},
                {"fuel_cap", 0},          {"fuel_cap_err", 0},    {"dcdc_systemSts", 2},
                {"esc_hhcAvailable", 1},  {"esc_hdcAvailable", 1}};

    for (const auto *corner : {"LF", "RF", "LR", "RR"}) {
        const auto name = std::string(corner);
        values["can_v1_5_bcm_tpms" + name + "Sts"] = 1;
        values["bcm_tpms" + name + "Sts"] = 1;
        values["bcm_tyre" + name + "Pressure"] = 24;
        values["bcm_tyre" + name + "Temp"] = 81;
    }

    // The Passion belt and steering angle readings are valid.
    for (const auto *name :
         {"SeatBeltDetection.Notify_f_SeatBeltSts.SeatBeltSts.DriverSeatBeltStsValid",
          "SeatBeltDetection.Notify_f_SeatBeltSts.SeatBeltSts.PassengerSeatBeltStsValid",
          "SteeringSysInfo.Notify_f_SteeringSysSts.SteeringSysSts.SteeringSysSts."
          "eps_sasSteeringAngleValid"}) {
        values[name] = 1;
    }

    Json result = Json::object();

    for (const auto &field : voyemu::catalog(catalog)) {
        if (values.contains(field.name)) {
            result[field.name] = values.at(field.name);
        }
    }

    // A parked vehicle at READY with closed doors, lamps off and buckled belts; 13.80 V is the
    // DC-DC converter charging the battery at READY.
    const std::vector<std::pair<VehicleConcept, std::int64_t>> concepts = {{speed, 0},
                                                                           {speed_valid, 1},
                                                                           {gear, 0},
                                                                           {gear_valid, 1},
                                                                           {charge, 55},
                                                                           {ignition, 1},
                                                                           {ready, 1},
                                                                           {battery_voltage, 1380},
                                                                           {turn_left, 0},
                                                                           {turn_right, 0},
                                                                           {position_lamp, 0},
                                                                           {daytime_lamp, 0},
                                                                           {low_beam, 0},
                                                                           {high_beam, 0},
                                                                           {rear_fog, 0},
                                                                           {belt_driver, 0},
                                                                           {belt_passenger, 0},
                                                                           {belt_rear_left, 0},
                                                                           {belt_rear_middle, 0},
                                                                           {belt_rear_right, 0},
                                                                           {door_front_left, 0},
                                                                           {door_front_right, 0},
                                                                           {door_rear_left, 0},
                                                                           {door_rear_right, 0},
                                                                           {hood, 0},
                                                                           {tailgate, 0},
                                                                           {steering_angle, 0}};

    for (const auto &[id, logical] : concepts) {
        concept_assign(catalog, result, id, logical);
    }

    return result;
}

Json initial_state(VehicleCatalog catalog) {
    Json raw = Json::object();

    for (const auto &field : voyemu::catalog(catalog)) {
        raw[field.name] = field.initial;
    }

    raw.update(normal_initial_values(catalog));
    Json state{{"schema", 1},
               {"sequence", 1},
               {"updated_utc", utc_now()},
               {"map_sha256", catalog_map_sha256(catalog)},
               {"origin", "manual-emulator"},
               {"raw_oem", raw}};
    validate_state(state);

    return state;
}

Json select_vehicle_startup(VehicleStartup mode, VehicleCatalog catalog, const Json &profile_values,
                            const Json *saved, std::uint32_t guest_sequence) {
    if (mode != VehicleStartup::fresh && mode != VehicleStartup::resume) {
        throw VoyemuError("VEHICLE_STARTUP", "unsupported policy");
    }

    Json selected;

    if (mode == VehicleStartup::resume) {
        if (!saved) {
            throw VoyemuError("VEHICLE_STARTUP", "saved scenario required");
        }

        validate_state(*saved);

        if (state_catalog(*saved) != catalog) {
            throw VoyemuError("VEHICLE_STARTUP", "saved scenario of another vehicle catalog");
        }

        selected = *saved;
    } else {
        if (!profile_values.is_object()) {
            throw VoyemuError("VEHICLE_STARTUP", "profile values required");
        }

        selected = initial_state(catalog);

        if (!profile_values.empty()) {
            selected = apply_patch(selected, profile_values);
        }
    }

    const auto sequence = selected.at("sequence").get<std::uint32_t>();

    if (guest_sequence >= sequence) {
        if (guest_sequence == UINT32_MAX) {
            throw VoyemuError("VEHICLE_SEQUENCE", "startup guest sequence exhausted");
        }

        selected["sequence"] = std::uint64_t{guest_sequence} + 1;
        selected["updated_utc"] = utc_now();
    }

    validate_state(selected);

    return selected;
}

// A Qinggan field that a patch of its source field also sets unless the patch names it itself: to
// the source value times scale, or to value when scale is 0. Writers send one field and every
// writer keeps the coupled fields consistent; the bound fields of a concept couple through its
// binding.
namespace {
struct FieldCoupling {
    std::string source, target;
    std::uint64_t scale, value;
};

const std::vector<FieldCoupling> &field_couplings() {
    static const auto couplings = [] {
        std::vector<FieldCoupling> result = {
                {"actual_power", "actual_power10x", 10, 0},
                {"allowed_power", "allowed_power10x", 10, 0},
                {"remain_mileage_e", "vcu_remainingMileageReal", 1, 0},
                {"remain_mileage_e", "vcu_remainingMileageCLTCWLTC", 1, 0},
                {"remain_mileage_f", "remain_mileage_f_nedc", 1, 0},
                {"remain_mileage_f", "remain_mileage_f_wltc", 1, 0}};

        // A tyre reading makes the measurement of its corner valid.
        for (const std::string corner : {"LF", "RF", "LR", "RR"}) {
            for (const std::string reading : {"Pressure", "Temp"}) {
                result.push_back(
                        {"bcm_tyre" + corner + reading, "bcm_tpms" + corner + "Sts", 0, 1});
                result.push_back({"bcm_tyre" + corner + reading,
                                  "can_v1_5_bcm_tpms" + corner + "Sts", 0, 1});
            }
        }

        return result;
    }();

    return couplings;
}
} // namespace

Json apply_patch(const Json &original, const Json &values) {
    validate_state(original);

    const auto catalog = state_catalog(original);
    const auto &fields = voyemu::catalog(catalog);

    if (!values.is_object() || values.empty() || values.size() > fields.size()) {
        throw VoyemuError("VEHICLE_PATCH", "nonempty keyed values required");
    }

    Json next = original;
    auto &raw = next["raw_oem"];

    for (const auto &[name, value] : values.items()) {
        if (!raw.contains(name)) {
            throw VoyemuError("VEHICLE_FIELD", "unknown " + name);
        }

        if (name.starts_with("swm_")) {
            throw VoyemuError("VEHICLE_EVENT", "use wheel operation");
        }

        if (!integer(value)) {
            throw VoyemuError("VEHICLE_VALUE", "unsigned integer required");
        }

        raw[name] = value;
    }

    // A catalogue without the target leaves the coupling out; a value wider than the target, such
    // as the all-ones unknown range of a wider slot, saturates to the all-ones of the target.
    for (const auto &coupling : field_couplings()) {
        if (values.contains(coupling.source) && !values.contains(coupling.target) &&
            raw.contains(coupling.target)) {
            const auto target = std::find_if(fields.begin(), fields.end(), [&](const Field &field) {
                return field.name == coupling.target;
            });
            const auto maximum = (std::uint64_t{1} << target->width) - 1;
            raw[coupling.target] = std::min(
                    maximum, coupling.scale
                                     ? raw[coupling.source].get<std::uint64_t>() * coupling.scale
                                     : coupling.value);
        }
    }

    detail::derive_concepts(catalog, raw, values);
    next["sequence"] = original["sequence"].get<std::uint64_t>() + 1;
    next["updated_utc"] = utc_now();
    validate_state(next);

    return next;
}

static void reject_symlinks(const std::filesystem::path &path) {
    auto absolute = std::filesystem::absolute(path).lexically_normal();

    for (const auto &current : os::ancestors(absolute)) {
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(current))) {
            throw VoyemuError("STATE_PATH", "symlink rejected");
        }
    }
}

void ensure_private_directory(const std::filesystem::path &path) {
    reject_symlinks(path);
#ifdef _WIN32
    detail::PrivateStateSecurity security;
    auto current = path.root_path();

    for (const auto &component : path.relative_path()) {
        current /= component;

        if (!CreateDirectoryW(os::win32_file_path(current).c_str(), &security.attributes) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            throw VoyemuError("STATE_PATH", "cannot create private directory");
        }
    }
#else
    auto current = path.root_path();

    for (const auto &component : path.relative_path()) {
        current /= component;

        if (!std::filesystem::exists(current) && ::mkdir(current.c_str(), 0700) != 0 &&
            errno != EEXIST) {
            throw VoyemuError("STATE_PATH", "cannot create private directory");
        }
    }
#endif
    std::filesystem::create_directories(path);

    if (!std::filesystem::is_directory(path)) {
        throw VoyemuError("STATE_PATH", "directory required");
    }

#ifndef _WIN32
    struct stat status{};

    if (::stat(path.c_str(), &status) != 0 || status.st_uid != ::geteuid()) {
        throw VoyemuError("STATE_OWNER", "foreign directory");
    }

    if (::chmod(path.c_str(), 0700) != 0) {
        throw VoyemuError("STATE_OWNER", "cannot set directory permissions");
    }
#endif
}

void ensure_local_roots(const std::filesystem::path &root) {
    for (const auto *name : {"profiles", "toolchains", "build", "machines"}) {
        ensure_private_directory(root / ".local" / name);
    }
}

static std::string read_private_json_text(const std::filesystem::path &path, std::size_t maximum) {
#ifdef _WIN32
    detail::PrivateJsonLock lock(path);
#endif
    reject_symlinks(path);

    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > maximum) {
        throw VoyemuError("STATE_EXTENT", "invalid file");
    }

#ifndef _WIN32
    struct stat status{};

    if (::stat(path.c_str(), &status) != 0 || status.st_uid != ::geteuid()) {
        throw VoyemuError("STATE_OWNER", "foreign file");
    }
#endif
#ifdef _WIN32
    detail::PrivateJsonReader file(path, maximum);

    return file.text();
#else
    std::ifstream file(path, std::ios::binary);

    if (!file) {
        throw VoyemuError("STATE_READ", "cannot open file");
    }

    std::string text{std::istreambuf_iterator<char>(file), {}};

    if (text.size() > maximum) {
        throw VoyemuError("STATE_EXTENT", "file grew");
    }
    return text;
#endif
}

std::string read_private_json_bytes(const std::filesystem::path &path, std::size_t maximum) {
    auto text = read_private_json_text(path, maximum);

    (void)Json::parse(text);

    return text;
}

Json read_private_json(const std::filesystem::path &path, std::size_t maximum) {
    return Json::parse(read_private_json_text(path, maximum));
}

#ifdef _WIN32
namespace {
struct StateHandle {
    HANDLE value = INVALID_HANDLE_VALUE;

    ~StateHandle() {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
};
struct StateFile {
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
};

bool current_state_owner(HANDLE file) {
    StateHandle token;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) {
        return false;
    }

    DWORD bytes = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);

    if (!bytes || bytes > 65536) {
        return false;
    }

    std::vector<unsigned char> user(bytes);

    if (!GetTokenInformation(token.value, TokenUser, user.data(), bytes, &bytes)) {
        return false;
    }

    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result = GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                                        nullptr, nullptr, nullptr, &descriptor);
    const auto sid = reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid;
    const bool valid = result == ERROR_SUCCESS && owner && IsValidSid(owner) && IsValidSid(sid) &&
                       EqualSid(owner, sid);

    if (descriptor) {
        LocalFree(descriptor);
    }

    return valid;
}

bool state_information(HANDLE file, StateFile &value, bool owner = true) {
    return GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &value.info) &&
           GetFileInformationByHandleEx(file, FileBasicInfo, &value.basic, sizeof(value.basic)) &&
           value.info.nNumberOfLinks == 1 &&
           !(value.info.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY)) &&
           (!owner || current_state_owner(file));
}

bool state_incarnation(const StateFile &a, const StateFile &b) {
    return a.info.dwVolumeSerialNumber == b.info.dwVolumeSerialNumber &&
           a.info.nFileIndexHigh == b.info.nFileIndexHigh &&
           a.info.nFileIndexLow == b.info.nFileIndexLow &&
           CompareFileTime(&a.info.ftCreationTime, &b.info.ftCreationTime) == 0;
}

bool state_snapshot(const std::filesystem::path &path, StateFile &value) {
    StateHandle file{CreateFileW(os::win32_file_path(path).c_str(),
                                 FILE_READ_ATTRIBUTES | READ_CONTROL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};

    return file.value != INVALID_HANDLE_VALUE && state_information(file.value, value);
}

bool same_state(const StateFile &a, const StateFile &b) {
    return state_incarnation(a, b) && a.info.dwFileAttributes == b.info.dwFileAttributes &&
           a.info.nFileSizeHigh == b.info.nFileSizeHigh &&
           a.info.nFileSizeLow == b.info.nFileSizeLow &&
           CompareFileTime(&a.info.ftLastWriteTime, &b.info.ftLastWriteTime) == 0 &&
           a.basic.ChangeTime.QuadPart == b.basic.ChangeTime.QuadPart;
}

bool pending_state(const std::filesystem::path &path, const StateFile &expected,
                   const std::string &text, bool remove = false) {
    StateHandle file{CreateFileW(os::win32_file_path(path).c_str(),
                                 GENERIC_READ | READ_CONTROL | (remove ? DELETE : 0),
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    StateFile current;

    if (file.value == INVALID_HANDLE_VALUE || !state_information(file.value, current) ||
        !state_incarnation(expected, current) ||
        (std::uint64_t(current.info.nFileSizeHigh) << 32 | current.info.nFileSizeLow) !=
                text.size()) {
        return false;
    }

    std::size_t offset = 0;
    char buffer[8192];

    for (;;) {
        DWORD count = 0;

        if (!ReadFile(file.value, buffer, sizeof(buffer), &count, nullptr) ||
            count > text.size() - offset) {
            return false;
        }

        if (!count) {
            break;
        }

        if (!std::equal(buffer, buffer + count, text.begin() + offset)) {
            return false;
        }

        offset += count;
    }

    if (offset != text.size()) {
        return false;
    }

    if (remove) {
        FILE_DISPOSITION_INFO disposition{TRUE};

        return SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition,
                                          sizeof(disposition));
    }

    return true;
}
} // namespace
#endif

static void write_private_json_text(const std::filesystem::path &path, const std::string &text,
                                    std::size_t maximum) {
    ensure_private_directory(path.parent_path());
#ifdef _WIN32
    detail::PrivateJsonLock lock(path);
#endif
    reject_symlinks(path);

    if (text.size() > maximum) {
        throw VoyemuError("STATE_EXTENT", "output too large");
    }

    auto pending = path;
    pending += ".pending";
    reject_symlinks(pending);
#ifdef _WIN32
    StateFile previous;
    const bool previous_valid = state_snapshot(path, previous);
    detail::PrivateStateSecurity security;
    HANDLE file = CreateFileW(os::win32_file_path(pending).c_str(), GENERIC_WRITE, 0,
                              &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

    // Every writer of path holds its lock, so an existing staging file is the remainder of a
    // writer that died before publishing it.
    if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS &&
        DeleteFileW(os::win32_file_path(pending).c_str())) {
        file = CreateFileW(os::win32_file_path(pending).c_str(), GENERIC_WRITE, 0,
                           &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    if (file == INVALID_HANDLE_VALUE) {
        throw VoyemuError("STATE_WRITE", "exclusive temporary file failed");
    }

    DWORD count = 0;
    bool success = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &count, nullptr) &&
                   count == text.size() && FlushFileBuffers(file);

    if (!success) {
        const auto error = GetLastError();
        CloseHandle(file);
        DeleteFileW(os::win32_file_path(pending).c_str());
        throw VoyemuError("STATE_WRITE", "flush failed; win32=" + std::to_string(error));
    }

    StateFile staged;
    const bool staged_valid = state_information(file, staged, false);
    CloseHandle(file);
    // The same-path reader/writer lock keeps the destination closed while the
    // filesystem atomically publishes this flushed incarnation.
    // Noncooperating metadata readers can briefly retain this destination under
    // Wine even with delete sharing. Retry only this unchanged owned publication.
    const auto started = std::chrono::steady_clock::now(),
               deadline = started + std::chrono::milliseconds(250);
    unsigned attempts = 0;
    DWORD first_error = 0, error = 0, attributes = INVALID_FILE_ATTRIBUTES;
    const char *guard = "not_transient";

    for (;;) {
        ++attempts;
        success = MoveFileExW(pending.c_str(), path.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

        if (success) {
            break;
        }

        error = GetLastError();

        if (!first_error) {
            first_error = error;
        }

        attributes = GetFileAttributesW(path.c_str());

        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) {
            break;
        }

        if (!previous_valid) {
            guard = "initial_target";
            break;
        }

        StateFile current;

        if (!state_snapshot(path, current) || !same_state(previous, current)) {
            guard = "target_changed";
            break;
        }

        if (!staged_valid || !pending_state(pending, staged, text)) {
            guard = "pending_changed";
            break;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            guard = "deadline";
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        if (std::chrono::steady_clock::now() >= deadline) {
            guard = "deadline";
            break;
        }
    }

    if (!success) {
        const bool cleaned = staged_valid && pending_state(pending, staged, text, true);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
        throw VoyemuError("STATE_WRITE",
                          "atomic replace failed; win32=" + std::to_string(error) +
                                  "; target=" + path.filename().string() +
                                  "; first_win32=" + std::to_string(first_error) +
                                  "; attributes=" + std::to_string(attributes) +
                                  "; attempts=" + std::to_string(attempts) +
                                  "; elapsed_ms=" + std::to_string(elapsed) + "; guard=" + guard +
                                  "; pending_cleaned=" + (cleaned ? "true" : "false"));
    }
#else
    // Writers of the files of one directory exclude each other through an exclusive lock on the
    // directory, so an existing staging file is the remainder of a writer that died before
    // publishing it. The lock adds no file, and readers need none: rename keeps an open file.
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".")
                                                   : path.parent_path();
    struct DirectoryLock {
        int fd;

        ~DirectoryLock() {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    } directory{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};

    if (directory.fd < 0) {
        throw VoyemuError("STATE_LOCK", "state directory unavailable");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (::flock(directory.fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK && errno != EINTR) {
            throw VoyemuError("STATE_LOCK", "coordination failed");
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            throw VoyemuError("STATE_LOCK_TIMEOUT", "state operation exceeded five seconds");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (::unlink(pending.c_str()) != 0 && errno != ENOENT) {
        throw VoyemuError("STATE_WRITE", "staging file of a terminated writer remains");
    }

    int fd = ::open(pending.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);

    if (fd < 0) {
        throw VoyemuError("STATE_WRITE", "exclusive temporary file failed");
    }

    bool success = true;
    std::size_t offset = 0;

    while (offset < text.size()) {
        auto written = ::write(fd, text.data() + offset, text.size() - offset);

        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written <= 0) {
            success = false;
            break;
        }

        offset += static_cast<std::size_t>(written);
    }

    if (::fsync(fd) != 0) {
        success = false;
    }

    ::close(fd);

    if (!success || ::rename(pending.c_str(), path.c_str()) != 0) {
        ::unlink(pending.c_str());
        throw VoyemuError("STATE_WRITE", "atomic replace failed");
    }

    fd = ::open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

void write_private_json(const std::filesystem::path &path, const Json &value, std::size_t maximum) {
    write_private_json_text(path, value.dump() + "\n", maximum);
}

void write_private_json_bytes(const std::filesystem::path &path, const std::string &text,
                              std::size_t maximum) {
    if (text.size() > maximum) {
        throw VoyemuError("STATE_EXTENT", "output too large");
    }

    (void)Json::parse(text);
    write_private_json_text(path, text, maximum);
}

VehicleModel::VehicleModel(std::filesystem::path path, VehicleCatalog catalog)
    : path_(std::move(path)) {
    ensure_private_directory(path_.parent_path());

    if (std::filesystem::exists(path_) || std::filesystem::is_symlink(path_)) {
        state_ = read_private_json(path_, vehicle_state_bytes);
        validate_state(state_);
    } else {
        state_ = initial_state(catalog);
        write_private_json(path_, state_, vehicle_state_bytes);
    }
}

const Json &VehicleModel::advance_revision(std::uint32_t observed) {
    if (observed < state_.at("sequence").get<std::uint32_t>()) {
        return state_;
    }

    auto next = state_;
    next["sequence"] = std::uint64_t{observed} + 1;
    next["updated_utc"] = utc_now();
    validate_state(next);
    write_private_json(path_, next, vehicle_state_bytes);
    state_ = std::move(next);

    return state_;
}

const Json &VehicleModel::patch(const Json &values) {
    auto next = apply_patch(state_, values);

    write_private_json(path_, next, vehicle_state_bytes);
    state_ = std::move(next);

    return state_;
}
} // namespace voyemu
