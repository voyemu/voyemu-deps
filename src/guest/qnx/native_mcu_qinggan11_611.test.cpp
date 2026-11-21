#include "adas_ecu.hpp"
#include "model.hpp"
#include "native_mcu_qinggan11.h"
#include "qnx_qinggan11_mcu_layout.hpp"
#include "qnx_runtime_layout.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
using Json = voyemu::Json;
using u32 = unsigned;
using u64 = unsigned long long;
using usize = unsigned long;
static Qinggan11McuField qinggan11_fields[QINGGAN11_FIELDS];
static unsigned short qinggan11_wheel_fields[10];
static unsigned qinggan11_speed, qinggan11_speed10x, qinggan11_soc;
static unsigned qinggan11_calibration_fixture = 1, qinggan11_calibration_fixture_enabled = 1;
static unsigned qinggan11_projected_fields = 0, qinggan11_settings_feedback_enabled = 1;
#define QGFX_QINGGAN11_LAYOUT_DEFINED 1
static u32 normal[QINGGAN11_FIELDS];
// The catalogue position of a field that the guest does not write.
static unsigned unprojected_position;
static unsigned char original_bytes[650];
struct TestPacket {
    u32 fault, out_len;
    unsigned char data[3840];
};
static TestPacket storage{}, *packet = &storage;
static bool enabled = true, bad = false;
static u64 offered = 2;
static unsigned original_calls = 0, rpc_calls = 0;

static void acquire() {}

static void release() {}

static void copy(void *d, const void *s, usize n) {
    std::memcpy(d, s, n);
}

static void emit(const char *) {}

static char *test_getenv(const char *name) {
    return enabled && !std::strcmp(name, "QGFX_NATIVE_MCU") ? (char *)"1" : nullptr;
}

static int test_usleep(unsigned) {
    return 0;
}

static int test_pthread_create(int *, const void *, void *(*)(void *), void *) {
    return 11;
}

static int test_pthread_join(int, void **) {
    return 3;
}

static int genuine(void *out, unsigned short size) {
    original_calls++;

    if (out && size >= 650) {
        std::memcpy(out, original_bytes, 650);
    }

    return 650;
}

static int serializer(void *, void *, unsigned) {
    return -7;
}

static void *test_dlopen(const char *, int) {
    return (void *)1;
}

static void *test_dlsym(void *, const char *n) {
    return !std::strcmp(n, "dataParser_makedumydata") ? (void *)genuine : (void *)serializer;
}

static u64 call(u32 op, const u64 *args, unsigned count, const void *, unsigned) {
    rpc_calls++;
    storage = {};

    if (op == 363) {
        assert(count == 1);

        return 0;
    }

    assert(op == 364 && count == 4 && args[1] == 650 && args[2] == 736 &&
           args[3] == qinggan11_projected_fields);
    u32 values[QINGGAN11_FIELDS + 2] = {1, QINGGAN11_FIELDS};
    std::memcpy(values + 2, normal, sizeof normal);

    if (bad) {
        values[2 + unprojected_position] ^= 1;
    }

    storage.out_len = sizeof values;
    std::memcpy(storage.data, values, sizeof values);

    return offered;
}

#define getenv test_getenv
#define dlopen test_dlopen
#define dlsym test_dlsym
#define usleep test_usleep
#define pthread_create test_pthread_create
#define pthread_join test_pthread_join
#define QGFX_INITIALIZER
#define QGFX_NATIVE_MCU_DESTRUCTOR
#include "native_mcu_qinggan11.c"
#undef getenv
#undef dlopen
#undef dlsym
#undef usleep
#undef pthread_create
#undef pthread_join

int main(int argc, char **argv) {
    try {
        assert(argc == 2);
        qinggan11_projected_fields =
                voyemu::qnx_adapter(voyemu::QnxGuestAdapter::sa8155_qinggan11_611)
                        .mcu_projected_fields;
        const auto root = std::filesystem::canonical(argv[1]);
        const auto source = voyemu::read_private_json(
                root / "src/guest/qnx/native_mcu_qinggan11_611.json", 512 * 1024);
        const auto generated = voyemu::qnx_qinggan11_mcu_layout(source,
                                                                Json{{"eps_calibrationFlag", 1}});
        assert(generated.find(
                       "qinggan11_projected_fields=" + std::to_string(qinggan11_projected_fields) +
                       "U") != std::string::npos &&
               generated.find("qinggan11_settings_feedback_enabled=1U") != std::string::npos);
        const auto &fields = voyemu::catalog(voyemu::VehicleCatalog::qinggan11);
        const auto initial = voyemu::initial_state(voyemu::VehicleCatalog::qinggan11).at("raw_oem");
        auto pos = [&](std::string_view name) {
            const auto at = std::find_if(fields.begin(), fields.end(), [&](const auto &f) {
                return f.name == name;
            });
            assert(at != fields.end());

            return static_cast<unsigned>(at - fields.begin());
        };
        unsigned mapped = 0;

        for (unsigned i = 0; i < QINGGAN11_FIELDS; i++) {
            normal[i] = initial.at(fields[i].name).get<unsigned>();
            qinggan11_fields[i] = {65535, 0, normal[i]};
            const auto &row = source.at("fields").at(i);

            if (!row.contains("unprojected")) {
                qinggan11_fields[i].bit = row.at("wire").at("start_bit");
                qinggan11_fields[i].width = row.at("wire").at("width");
                mapped++;
            }
        }

        const char *buttons[] = {"swm_Photo",     "swm_menu",     "swm_OK",        "swm_upAdd",
                                 "swm_downMinus", "swm_leftLast", "swm_rightNext", "swm_nvsSwitch",
                                 "swm_phone",     "swm_voice"};

        for (unsigned i = 0; i < 10; i++) {
            qinggan11_wheel_fields[i] = static_cast<unsigned short>(pos(buttons[i]));
        }

        qinggan11_speed = pos("speed");
        qinggan11_speed10x = pos("speed10x");
        qinggan11_soc = pos("soc");
        unprojected_position = pos("set_byte0");
        const auto hdc = pos("esc_hdcAvailable");
        assert(mapped == qinggan11_projected_fields &&
               qinggan11_fields[pos("ready_lamp")].bit == 69 &&
               qinggan11_fields[pos("gear_valid")].bit == 1111 &&
               qinggan11_fields[pos("gear")].bit == 1112 &&
               qinggan11_fields[pos("ambient_temp")].bit == 1144 &&
               qinggan11_fields[hdc].bit == 335 && qinggan11_fields[hdc].width == 1);

        for (unsigned i = 0; i < 650; i++) {
            original_bytes[i] = static_cast<unsigned char>(i * 73 + 19);
        }

        qinggan11_settings.have = 1;
        std::memset(qinggan11_settings.bytes, 0xab, 10);
        unsigned char out[650];
        assert(dataParser_makedumydata(out, 650) == 650 && qinggan11_ack == offered);

        for (unsigned bit = 0; bit < 5200; bit++) {
            bool supported = bit == 465 || bit == 466 || (bit >= 2000 && bit < 2080);

            for (const auto &field : qinggan11_fields) {
                if (field.width && bit >= field.bit && bit < field.bit + field.width) {
                    supported = true;
                }
            }

            if (!supported) {
                assert(((out[bit / 8] ^ original_bytes[bit / 8]) & (1U << (bit % 8))) == 0);
            }
        }

        // The settings feedback of the OEM engine, the same code as on 7.3.1, owns bytes 250..259.
        const unsigned char settings[10] = {0xab, 0xab, 0xab, 0xab, 0xab,
                                            0xab, 0xab, 0xab, 0xab, 0xab};
        assert(((out[58] >> 1) & 3) == 1 && !std::memcmp(out + 250, settings, 10));
        assert(((out[335 / 8] >> (335 % 8)) & 1) == 1);
        offered++;
        normal[hdc] = 0;
        assert(dataParser_makedumydata(out, 650) == 650 && qinggan11_ack == offered &&
               ((out[335 / 8] >> (335 % 8)) & 1) == 0);
        offered++;
        normal[hdc] = 1;
        assert(dataParser_makedumydata(out, 650) == 650 && qinggan11_ack == offered &&
               ((out[335 / 8] >> (335 % 8)) & 1) == 1);
        // The model ignition reaches bit 0, the OEM power item of powermgr, in both states.
        assert(qinggan11_fields[0].bit == 0 && qinggan11_fields[0].width == 1 && (out[0] & 1) == 1);
        offered++;
        normal[0] = 0;
        assert(dataParser_makedumydata(out, 650) == 650 && qinggan11_ack == offered &&
               (out[0] & 1) == 0);
        offered++;
        normal[0] = 1;
        assert(dataParser_makedumydata(out, 650) == 650 && qinggan11_ack == offered &&
               (out[0] & 1) == 1);
        // Verify the packed bits independently of the table used by the projector.
        auto packed = [&](unsigned bit, unsigned width) {
            unsigned value = 0;

            for (unsigned i = 0; i < width; ++i) {
                value |= unsigned((out[(bit + i) / 8] >> ((bit + i) % 8)) & 1U) << i;
            }

            return value;
        };
        struct Case {
            std::string_view name;
            unsigned bit, width, value;
        };
        // Each written field reaches its OEM wire bits and leaves on the next revision.
        auto deliver_cases = [&](std::initializer_list<Case> cases) {
            for (const auto &signal : cases) {
                const auto field = std::find_if(fields.begin(), fields.end(), [&](const auto &f) {
                    return f.name == signal.name;
                });
                assert(field != fields.end());
                const auto position = static_cast<unsigned>(field - fields.begin());
                const auto previous = normal[position];
                normal[position] = signal.value;
                ++offered;
                assert(dataParser_makedumydata(out, 650) == 650 && qinggan11_ack == offered &&
                       packed(signal.bit, signal.width) == signal.value);
                normal[position] = previous;
                ++offered;
                assert(dataParser_makedumydata(out, 650) == 650 && qinggan11_ack == offered &&
                       packed(signal.bit, signal.width) == previous);
            }
        };
        deliver_cases({{"pos_lamp", 3, 1, 1},
                       {"low_beam", 4, 1, 1},
                       {"high_beam", 5, 1, 1},
                       {"rear_fog", 10, 1, 1},
                       {"bcm_DRLStatus", 13, 1, 1},
                       {"hood", 184, 1, 1},
                       {"door_fr", 186, 1, 1},
                       {"door_rl", 187, 1, 1},
                       {"door_rr", 188, 1, 1},
                       {"tail_gate", 189, 1, 1},
                       {"remain_mileage_e", 1090, 11, 280},
                       {"remain_mileage_f", 2218, 11, 420},
                       {"odo", 1120, 24, 12346},
                       {"fuel_percent", 1236, 7, 68},
                       {"actual_power", 1262, 7, 12},
                       {"allowed_power", 1255, 7, 80},
                       {"actual_power10x", 1407, 12, 120},
                       {"allowed_power10x", 1395, 12, 800},
                       {"vcu_remainingMileageReal", 1433, 11, 280},
                       {"vcu_remainingMileageCLTCWLTC", 1571, 11, 280},
                       {"remain_mileage_f_nedc", 1463, 10, 420},
                       {"remain_mileage_f_wltc", 2229, 11, 420}});
        // Start bits and widths from the OEM 6.11.1 parser descriptors, one list per group.
        deliver_cases({
                {"overspdWarning", 94, 2, 3},
                {"mrr_fcwSts", 113, 3, 3},
                {"esc_hbbSts", 119, 3, 3},
                {"mrr_r1_rearCollisionWarning", 166, 1, 1},
                {"mrr_r1_systemErrorSts", 178, 2, 3},
                {"icm_dmsWearyLeverSts", 202, 3, 3},
                {"gsm_faultLevel", 207, 2, 3},
                {"las_status", 452, 3, 3},
                {"mpc_laneAssitType", 459, 2, 3},
                {"mpc_text_infor", 678, 5, 11},
                {"mrr_text_info", 696, 5, 11},
                {"pebs_warning", 701, 3, 3},
                {"icm_dmsCameraSts", 729, 3, 3},
                {"icm_dmsSts", 732, 3, 3},
                {"icm_dmsDistractLeverSts", 735, 2, 3},
                {"icm_dmsDrvBehaviorMon", 737, 3, 3},
                {"nvs_displayReq", 1215, 3, 3},
                {"nvs_Sts", 1218, 2, 3},
                {"nvs_cameraSts", 1220, 2, 3},
                {"nvs_warnningSts", 1222, 1, 1},
                {"nvs_textPrompt", 1223, 4, 11},
                {"IHBC_status", 6, 3, 3},
                {"nvs_warningXCoordinate", 1320, 12, 2731},
                {"nvs_warningYCoordinate", 1332, 10, 683},
                {"nvs_warningType", 1342, 3, 3},
                {"gsm_warnInformation", 1473, 3, 3},
                {"mrr_r1_leftBSDLCAWarning", 1549, 2, 3},
                {"mrr_r1_rightBSDLCAWarning", 1551, 2, 3},
                {"mrr_r1_leftDOWWarnning", 1553, 2, 3},
                {"mrr_r1_rightDOWWarnning", 1555, 2, 3},
                {"mrr_r1_leftRCTAWarnning", 1557, 1, 1},
                {"mrr_r1_rightRCTAWarnning", 1558, 1, 1},
        });

        deliver_cases({
                {"asc_ASC_errorSts", 122, 3, 3},
                {"sound_enable", 134, 1, 1},
                {"hcm_leftHighBeamFaultSt", 135, 2, 3},
                {"hcm_rightHighBeamFaultSt", 137, 1, 1},
                {"hcm_leftLowBeamFaultSt", 138, 1, 1},
                {"hcm_rightLowBeamFaultSt", 139, 1, 1},
                {"hcm_turnLampWarningLt", 140, 1, 1},
                {"hcm_turnLampWarningRt", 141, 1, 1},
                {"bcm_rllmLeftTurnLampFault", 142, 1, 1},
                {"bcm_rrlmRightTurnLampFault", 143, 1, 1},
                {"bcm_rmlmleftTurnLampFault", 144, 1, 1},
                {"bcm_rmlmrightTurnLampFault", 145, 1, 1},
                {"hcm_middleLeftPositionLampFault", 146, 1, 1},
                {"hcm_positionLampWarningLt", 147, 1, 1},
                {"hcm_middleRightPositionLampFault", 148, 1, 1},
                {"hcm_positionLampWarningRt", 149, 1, 1},
                {"bcm_rllmPositionLampFault", 150, 1, 1},
                {"bcm_rmlmpositionLampFault", 151, 1, 1},
                {"bcm_rrlmpositionLampFault", 152, 1, 1},
                {"hcm_dayTimeRunningLampWarningLt", 153, 1, 1},
                {"hcm_dayTimeRunningLampWarningRt", 154, 1, 1},
                {"hcm_alsErrorStsLt", 155, 1, 1},
                {"hcm_alsErrorStsRt", 156, 1, 1},
                {"bcm_rllmstopLampFault", 157, 2, 3},
                {"bcm_rmlmstopLampFault", 159, 1, 1},
                {"bcm_rmlmhighStopLampFault", 160, 1, 1},
                {"bcm_rrlmstopLampFault", 161, 1, 1},
                {"bcm_rllmfogLampFault", 162, 1, 1},
                {"bcm_rrlmfogLampFault", 163, 1, 1},
                {"bcm_rllmreverseLampFault", 164, 1, 1},
                {"bcm_rrlmreverseLampFault", 165, 1, 1},
                {"washing_liquid_low", 180, 1, 1},
                {"fuel_cap", 190, 1, 1},
                {"etc_disassemblySts", 195, 1, 1},
                {"etc_lockSts", 196, 1, 1},
                {"etc_detect", 197, 1, 1},
                {"etc_lamp", 198, 1, 1},
                {"etc_tranStauts", 199, 2, 3},
                {"maintenance_sts_km", 209, 1, 1},
                {"maintenance_sts_day", 210, 1, 1},
                {"remain_maintenance_km", 211, 9, 171},
                {"remain_maintenance_days", 220, 4, 11},
                {"Maintain_Reminder", 224, 1, 1},
                {"maintenance", 225, 2, 3},
                {"pos_lamp_tips", 308, 1, 1},
                {"bcm_sl_errSts", 309, 4, 11},
                {"peps_key_not_incar", 313, 1, 1},
                {"peps_key_serch_fail", 314, 1, 1},
                {"immo_release", 315, 1, 1},
                {"peps_key_close", 316, 1, 1},
                {"peps_low_bat", 317, 1, 1},
                {"peps_relay_err", 318, 1, 1},
                {"bcm_12VBatterySOC", 327, 8, 171},
                {"vsp_errorIdentification", 337, 1, 1},
                {"peps_key_incar", 338, 1, 1},
                {"wcm_reminder", 718, 1, 1},
                {"bcm_readyReq", 723, 1, 1},
                {"Tbox_SysErSt", 724, 2, 3},
                {"ASC_SusTempAdjust", 728, 1, 1},
                {"bcm_IndicationPressBrake", 740, 2, 3},
                {"bcm_LowFrequencyAntennaFault", 742, 2, 3},
                {"driving_long_tips", 747, 1, 1},
                {"bcm_remoteForbidden", 748, 2, 3},
                {"ivi_naviDirection", 1155, 4, 11},
                {"ivi_naviAltitude", 1159, 7, 43},
                {"ac_driverTempC_off", 1176, 1, 1},
                {"ac_driverTempC", 1177, 5, 11},
                {"ivi_timeInfoSecond", 1182, 6, 43},
                {"ivi_timeInfoMinute", 1188, 6, 43},
                {"ivi_timeInfoHour", 1194, 5, 11},
                {"ivi_timeInfoDay", 1199, 5, 11},
                {"ivi_timeInfoMonth", 1204, 4, 11},
                {"ivi_timeInfoYear", 1208, 6, 43},
                {"ivi_timeValid", 1214, 1, 1},
                {"bcm_sl_currentPos", 1276, 2, 2},
                {"bcm_PressureWarningLF", 1349, 2, 3},
                {"bcm_PressureWarningRF", 1351, 2, 3},
                {"bcm_TireTempWarningLF", 1353, 1, 1},
                {"bcm_TireTempWarningRF", 1354, 1, 1},
                {"bcm_tpmsLFLOW_BATT_Warning", 1355, 1, 1},
                {"bcm_tpmsRFLOW_BATT_Warning", 1356, 1, 1},
                {"bcm_tpmsLFRAPID_LEAK_Warning", 1357, 1, 1},
                {"bcm_tpmsRFRAPID_LEAK_Warning", 1358, 1, 1},
                {"bcm_PressureWarningLR", 1363, 2, 3},
                {"bcm_PressureWarningRR", 1365, 2, 3},
                {"bcm_TireTempWarningLR", 1367, 1, 1},
                {"bcm_TireTempWarningRR", 1368, 1, 1},
                {"bcm_tpmsLRLOW_BATT_Warning", 1369, 1, 1},
                {"bcm_tpmsRRLOW_BATT_Warning", 1370, 1, 1},
                {"bcm_tpmsLRRAPID_LEAK_Warning", 1371, 1, 1},
                {"bcm_tpmsRRRAPID_LEAK_Warning", 1372, 1, 1},
                {"upgradeMCU", 1375, 8, 171},
                {"OTA_mode", 1453, 2, 3},
                {"hcm_adbFuctionStsLt", 1455, 3, 3},
                {"hcm_adbFuctionStsRt", 1458, 3, 3},
                {"asc_AirPumpOverHeatWarning", 1461, 2, 3},
                {"asc_ascSysSts", 1476, 3, 3},
                {"etc_dataSts", 1503, 8, 171},
                {"fuel_cap_err", 1513, 2, 3},
                {"alarm_play_req", 1515, 12, 2730},
                {"soc_menu_info", 1559, 8, 171},
                {"icyroad_reminder", 1567, 2, 3},
                {"reset_dsp", 1597, 1, 1},
                {"reboot_request", 1838, 2, 3},
        });

        deliver_cases({
                {"epb_fault", 11, 2, 3},
                {"brake_oil_low", 18, 1, 1},
                {"abs_fault", 19, 1, 1},
                {"ebd_fault", 20, 1, 1},
                {"epb_parking", 21, 2, 3},
                {"autohold_lamp", 23, 3, 3},
                {"assist_sys_fault", 26, 2, 3},
                {"esc_lamp", 28, 2, 3},
                {"esc_off", 30, 1, 1},
                {"eps_fault", 31, 2, 3},
                {"hdc_lamp", 33, 2, 3},
                {"esc_hhcActive", 167, 1, 1},
                {"break_warning", 183, 1, 1},
                {"epb_text", 319, 4, 11},
                {"esc_hhcAvailable", 323, 1, 0},
                {"avh_available", 324, 1, 1},
                {"avh_stdby", 325, 2, 3},
                {"esc_vehicleSpeed", 1307, 13, 2731},
                {"vcu_brakePedalPressed", 1444, 1, 1},
        });

        deliver_cases({
                {"acu_driverSeatBeltSts", 35, 2, 3},
                {"acu_passengerSeatBeltSts", 37, 2, 3},
                {"RearLeftSeatBelt", 39, 2, 3},
                {"RearMidSeatBelt", 41, 2, 3},
                {"RearRightSeatBelt", 43, 2, 3},
                {"acu_warning", 45, 2, 3},
                {"SeatBeltLamp", 1546, 2, 3},
        });

        deliver_cases({
                {"fuel_low_alert", 47, 2, 3},
                {"mil_lamp", 49, 2, 3},
                {"eng_coolant_temp_high", 51, 2, 3},
                {"eng_coolant_temp", 53, 8, 171},
                {"engine_oil_low", 61, 1, 1},
                {"cool_ant_low", 62, 2, 3},
                {"fuel_instant", 1039, 8, 171},
                {"fuel_instant_last1km", 1047, 8, 171},
                {"rpm", 1065, 13, 2731},
                {"vcu_stEgyFlowEngAndGenMot", 1086, 2, 3},
        });

        deliver_cases({
                {"bms_authenticationResult", 14, 2, 3},
                {"pwr_bat_soc_low", 68, 1, 1},
                {"mcu0_fault", 70, 1, 1},
                {"turtle_lamp", 71, 2, 3},
                {"vcu_alertLevel", 73, 3, 3},
                {"bms_alert", 76, 2, 3},
                {"isolationFault", 78, 2, 3},
                {"vcu_CCStas", 125, 2, 3},
                {"charge_lamp", 127, 1, 1},
                {"bms_state", 128, 4, 11},
                {"mcuf0_overTemperature", 132, 1, 1},
                {"vcu_tcmFail", 133, 1, 1},
                {"ev_lamp", 181, 2, 1},
                {"charge_cap", 191, 1, 1},
                {"volOrSOCLowAlarm", 192, 2, 3},
                {"vcu_batteryOverTemperature", 194, 1, 1},
                {"bms_f003_packOverCrntChg", 201, 1, 1},
                {"vcu_towMode", 336, 1, 1},
                {"dc_charge_state", 339, 4, 11},
                {"ac_charge_state", 343, 4, 11},
                {"ac_charge_connect", 347, 1, 1},
                {"dc_charge_connect", 348, 1, 1},
                {"remain_charge_time", 349, 10, 683},
                {"bms_pPack", 359, 14, 10923},
                {"obc_chargeVoltage", 373, 17, 43691},
                {"obc_chargeCurrent", 390, 13, 2731},
                {"ivi_maxChrgSocSet", 411, 8, 171},
                {"bms_chargeFormSts", 419, 3, 3},
                {"ivi_ChargeEndTimeHour", 422, 4, 11},
                {"ivi_ChargeStartTimeHour", 426, 4, 11},
                {"ivi_ChargeEndTimeHalfDay", 430, 1, 1},
                {"ivi_ChargeStartTimeHalfDay", 431, 1, 1},
                {"ivi_ChargeEndTimeMinute", 432, 6, 43},
                {"ivi_ChargeStartTimeMinute", 438, 6, 43},
                {"obc_state", 602, 4, 11},
                {"vcu_vDisSet", 706, 8, 171},
                {"vcu_ccState", 714, 4, 11},
                {"obc_ccOhm", 719, 4, 11},
                {"vcu_KL15offReq", 177, 1, 1},
                {"vcu_GearPosNotEqual", 750, 3, 3},
                {"bms_stEgyFlowSysDischRgn", 1078, 2, 3},
                {"bms_stEgyFlowOBCAndBatt", 1080, 2, 3},
                {"mcuf0_stEgyFlowAndBatt", 1082, 2, 3},
                {"mcur0_stEgyFlowAndBatt", 1084, 2, 3},
                {"vcu_stEgyFlowGenMotAndBatt", 1088, 2, 3},
                {"vcu_driveMode", 1152, 3, 3},
                {"soc_warning", 1253, 2, 3},
                {"recovery_power", 1269, 7, 43},
                {"BMS_ThrmlRunawaySt", 1373, 2, 3},
                {"recovery_power10x", 1419, 12, 2731},
                {"VCU_StandStillPreheatBattWarn", 1445, 1, 1},
                {"VCU_LowTempInhibitPowerUpWarn", 1446, 1, 1},
                {"lowBatChargeFault", 1447, 2, 3},
                {"vcu_ExhibitionSts", 1511, 2, 3},
                {"vcu_hvilWarningIndicate", 1569, 2, 3},
                {"bms_batteryConditionFail", 1595, 1, 1},
                {"bms_f054_dcChrgOverTemp", 1596, 1, 1},
                {"OBC_halfConnectWarn", 1653, 2, 3},
                {"VCU_SOCmodeSet", 745, 2, 3},
                {"charge_connect_warn", 1796, 2, 3},
        });

        deliver_cases({
                {"vcu_consumpRatio50km", 403, 8, 171},
                {"trip_ax", 759, 24, 11184811},
                {"tripax_avg_pwr", 783, 11, 683},
                {"tripax_avg_spd", 794, 8, 171},
                {"tripax_avg_fuelconsump", 802, 8, 171},
                {"tripax_time_min", 810, 13, 2731},
                {"trip_fuel", 823, 24, 11184811},
                {"tripfuel_avg_pwr", 847, 11, 683},
                {"tripfuel_avg_spd", 858, 8, 171},
                {"tripfuel_avg_fuelconsump", 866, 8, 171},
                {"tripfuel_time_min", 874, 13, 2731},
                {"trip_interval_ready", 887, 24, 11184811},
                {"current_trip_avg_pwr", 911, 11, 683},
                {"current_trip_avg_spd", 922, 8, 171},
                {"current_trip_avg_fuelconsump", 930, 8, 171},
                {"current_trip_time_min", 938, 13, 2731},
                {"trip_interval_charge", 951, 24, 11184811},
                {"last_charge_avg_pwr", 975, 11, 683},
                {"last_charge_avg_fuelconsump", 986, 8, 171},
                {"last_charge_avg_spd", 994, 8, 171},
                {"last_charge_time_min", 1002, 13, 2731},
                {"vcu_consumpRatioInstant", 1015, 11, 683},
                {"vcu_consumpRatioInstant_last1km", 1026, 11, 683},
                {"instant_value_unit", 1037, 2, 3},
                {"instant_value_trip", 1055, 10, 683},
                {"odoRev", 1479, 24, 11184847},
                {"trip_ax_rev", 1756, 20, 699051},
                {"trip_current_rev", 1776, 20, 699051},
        });

        deliver_cases({
                {"cfg_driver_mode", 2080, 2, 2},
                {"cfg_tbox", 2082, 2, 2},
                {"cfg_driver_style", 2084, 3, 2},
                {"cfg_hud", 2087, 1, 0},
                {"cfg_driver_source", 2088, 4, 10},
                {"cfg_wireless_charge", 2092, 2, 2},
                {"cfg_glass_cover", 2094, 2, 2},
                {"cfg_sunroof_type", 2096, 2, 2},
                {"cfg_child_lock", 2098, 2, 2},
                {"cfg_driver_seat_heat", 2100, 1, 0},
                {"cfg_driver_seat_air", 2101, 1, 0},
                {"cfg_driver_seat_massage", 2102, 1, 0},
                {"cfg_driver_seat_memory", 2103, 1, 0},
                {"cfg_copilot_seat_heat", 2104, 1, 0},
                {"cfg_copilot_seat_air", 2105, 1, 0},
                {"cfg_copilot_seat_massage", 2106, 1, 0},
                {"cfg_copilot_seat_memory", 2107, 1, 0},
                {"cfg_seat", 2108, 3, 2},
                {"cfg_sound_band", 2111, 1, 0},
                {"cfg_charge_port_type", 2112, 2, 2},
                {"cfg_gas_port_type", 2114, 2, 2},
                {"cfg_speaker", 2116, 3, 2},
                {"cfg_air_suspension", 2119, 1, 0},
                {"cfg_vehicle_color", 2120, 4, 10},
                {"cfg_maintain", 2124, 2, 2},
                {"cfg_auto_beam", 2126, 1, 0},
                {"cfg_auto_beam_height", 2127, 1, 0},
                {"cfg_env_lamp", 2128, 3, 2},
                {"cfg_left_right_hand", 2131, 1, 0},
                {"cfg_auto_headlight", 2132, 1, 0},
                {"cfg_matrix_lamp", 2133, 1, 0},
                {"cfg_auto_parking", 2134, 1, 0},
                {"cfg_ar", 2135, 1, 0},
                {"cfg_sound_position", 2136, 2, 2},
                {"cfg_iaqc", 2138, 1, 0},
                {"cfg_air_filter", 2139, 1, 0},
                {"cfg_air_flesh", 2140, 1, 0},
                {"cfg_dvr", 2141, 1, 0},
                {"cfg_etc_cfg", 2142, 1, 0},
                {"cfg_ac_auto_cycle", 2143, 1, 0},
                {"cfg_nvs", 2144, 1, 0},
                {"cfg_backmonitor", 2145, 1, 0},
                {"cfg_blis", 2146, 1, 0},
                {"cfg_ldw", 2147, 1, 0},
                {"cfg_pre_crash", 2148, 2, 2},
                {"cfg_lka", 2150, 1, 0},
                {"cfg_lca", 2151, 1, 0},
                {"cfg_acc", 2152, 2, 2},
                {"cfg_aeb", 2154, 1, 0},
                {"cfg_door_warning", 2155, 1, 0},
                {"cfg_traffic_sign", 2156, 1, 0},
                {"cfg_isa", 2157, 1, 0},
                {"cfg_rcta", 2158, 1, 0},
                {"cfg_tja_assis", 2159, 1, 0},
                {"cfg_dms_id", 2160, 1, 0},
                {"cfg_gesture", 2161, 1, 0},
                {"cfg_sale_area", 2162, 6, 42},
                {"cfg_screen", 2168, 2, 2},
                {"cfg_mrr", 2170, 1, 0},
                {"cfg_mpc", 2171, 1, 0},
        });

        deliver_cases({
                {"can_msg_timeout_111", 1840, 2, 3}, {"can_msg_timeout_114", 1842, 2, 3},
                {"can_msg_timeout_130", 1844, 2, 3}, {"can_msg_timeout_140", 1846, 2, 3},
                {"can_msg_timeout_144", 1848, 2, 3}, {"can_msg_timeout_212", 1850, 2, 3},
                {"can_msg_timeout_219", 1852, 2, 3}, {"can_msg_timeout_21a", 1854, 2, 3},
                {"can_msg_timeout_239", 1856, 2, 3}, {"can_msg_timeout_23b", 1858, 2, 3},
                {"can_msg_timeout_241", 1860, 2, 3}, {"can_msg_timeout_2a1", 1862, 2, 3},
                {"can_msg_timeout_2a2", 1864, 2, 3}, {"can_msg_timeout_2ac", 1866, 2, 3},
                {"can_msg_timeout_2c1", 1868, 2, 3}, {"can_msg_timeout_2c2", 1870, 2, 3},
                {"can_msg_timeout_301", 1872, 2, 3}, {"can_msg_timeout_306", 1874, 2, 3},
                {"can_msg_timeout_309", 1876, 2, 3}, {"can_msg_timeout_319", 1958, 2, 3},
                {"can_msg_timeout_30e", 1880, 2, 3}, {"can_msg_timeout_322", 1882, 2, 3},
                {"can_msg_timeout_323", 1884, 2, 3}, {"can_msg_timeout_324", 1886, 2, 3},
                {"can_msg_timeout_36e", 1892, 2, 3}, {"can_msg_timeout_369", 1894, 2, 3},
                {"can_msg_timeout_37e", 1896, 2, 3}, {"can_msg_timeout_380", 1898, 2, 3},
                {"can_msg_timeout_38d", 1900, 2, 3}, {"can_msg_timeout_394", 1902, 2, 3},
                {"can_msg_timeout_396", 1904, 2, 3}, {"can_msg_timeout_3a0", 1906, 2, 3},
                {"can_msg_timeout_3a1", 1908, 2, 3}, {"can_msg_timeout_3a9", 1910, 2, 3},
                {"can_msg_timeout_3ad", 1912, 2, 3}, {"can_msg_timeout_3ae", 1914, 2, 3},
                {"can_msg_timeout_3b6", 1916, 2, 3}, {"can_msg_timeout_3be", 1918, 2, 3},
                {"can_msg_timeout_3c1", 1920, 2, 3}, {"can_msg_timeout_3c2", 1922, 2, 3},
                {"can_msg_timeout_407", 1924, 2, 3}, {"can_msg_timeout_411", 1926, 2, 3},
                {"can_msg_timeout_421", 1928, 2, 3}, {"can_msg_timeout_431", 1930, 2, 3},
                {"can_msg_timeout_441", 1932, 2, 3}, {"can_msg_timeout_511", 1934, 2, 3},
                {"can_msg_timeout_514", 1936, 2, 3}, {"can_msg_timeout_561", 1938, 2, 3},
                {"can_msg_timeout_07e", 1940, 2, 3}, {"can_msg_timeout_2fa", 1944, 2, 3},
                {"can_msg_timeout_331", 1946, 2, 3}, {"can_msg_timeout_387", 1948, 2, 3},
                {"can_msg_timeout_1A3", 1956, 2, 3}, {"can_msg_timeout_334", 1960, 2, 3},
                {"can_msg_timeout_203", 1962, 2, 3}, {"can_msg_timeout_208", 1964, 2, 3},
        });

        // The power manager and AVM items of the OEM parser: bcm_pepsPowerMode, ic_bat_voltage and
        // the AVM states reach _mcumiscitems_struct, poweroff_req and cfg_reserve_13 the power
        // manager item _pwrmgritems_struct.
        deliver_cases({
                {"bcm_pepsPowerMode", 105, 3, 3},
                {"poweroff_req", 1281, 2, 1},
                {"ic_bat_voltage", 1295, 12, 1240},
                {"apa_avmSts", 1600, 4, 11},
                {"apa_avmHmiIndex", 1604, 4, 11},
                {"apa_avmViewMode", 1608, 2, 3},
                {"apa_avmDisplayReq", 1610, 2, 3},
                {"apa_avmMODSts", 1612, 2, 3},
                {"apa_avmTrigByTurnningSts", 1614, 1, 1},
                {"apa_avmSingleViewSts", 1615, 5, 21},
                {"apa_avmTrajectory_display", 1620, 1, 1},
                {"apa_avmAutoswitching_display", 1621, 1, 1},
                {"apa_avmCarBodyTransparent", 1622, 1, 1},
                {"apa_avmSwSts", 1623, 1, 1},
                {"apa_avmMonitVideoOutput", 1624, 1, 1},
                {"apa_avmPDCfailureWarn", 1625, 3, 5},
                {"avm_sts", 1628, 4, 11},
                {"avm_hmiIndex", 1632, 4, 11},
                {"avm_viewMode", 1636, 2, 3},
                {"avm_displayReq", 1638, 2, 3},
                {"avm_MODSts", 1640, 2, 3},
                {"avm_trigByTurnningSts", 1642, 1, 1},
                {"avm_singleViewSts", 1643, 5, 21},
                {"avm_Trajectory_display", 1648, 1, 1},
                {"avm_Autoswitching_display", 1649, 1, 1},
                {"avm_CarBodyTransparent", 1650, 1, 1},
                {"avm_SwSts", 1651, 1, 1},
                {"avm_QuitSw", 1652, 1, 1},
                {"cfg_reserve_13", 2184, 8, 171},
        });

        // The language of the model reaches the MCU word of the OEM cluster beside its mcd command.
        deliver_cases({{"ivi_languageSet", 1283, 2, 1}});

        // Fields that reach the OEM ADAS, APA/PDC and power manager items or that no parser case of
        // this firmware reads.
        deliver_cases({
                {"gsm_warnInformation_bak", 205, 2, 3},
                {"can_v1_5_bcm_tpmsLFSts", 227, 3, 5},
                {"can_v1_5_bcm_tpmsRFSts", 246, 3, 5},
                {"can_v1_5_bcm_tpmsLRSts", 265, 3, 5},
                {"can_v1_5_bcm_tpmsRRSts", 284, 3, 5},
                {"tbox_vehicleMode", 1449, 4, 11},
                {"apa_WarningReq", 1527, 1, 1},
                {"APA_PDCSysSts", 1528, 2, 3},
                {"APA_PDCFailureWarning", 1530, 1, 1},
                {"APA_PDCAudibleFrontBeepRate", 1531, 3, 5},
                {"APA_PDCAudibleRearBeepRate", 1534, 3, 5},
                {"pdc_sysSts", 1537, 2, 3},
                {"pdc_failureWarning", 1539, 1, 1},
                {"pdc_audibleFrontBeepRate", 1540, 3, 5},
                {"pdc_audibleRearBeepRate", 1543, 3, 5},
                {"apa_AbortWarningReq", 1598, 1, 1},
                {"apa_ActiveWarningReq", 1599, 1, 1},
                {"cfg_reserve_14", 2192, 8, 171},
                {"acc_mode", 80, 3, 2},
                {"mpc_icaSts", 83, 3, 2},
                {"mpc_isa_speed_limit", 96, 6, 12},
                {"mpc_isaOnOffSts", 102, 2, 1},
                {"isa_limit_warning", 104, 1, 1},
                {"left_visualization", 455, 2, 2},
                {"right_visualization", 457, 2, 2},
                {"time_gap_set", 692, 3, 3},
                {"SeatBeatFlash", 1548, 1, 1},
        });
        // The wire fields that only the qinggan11 catalogue has, some of each group.
        // ADAS lanes, targets and assistance states.
        deliver_cases({
                {"ADCU_ANP_TxtInfo", 86, 8, 171},
                {"ADCU_WarningDis", 108, 2, 3},
                {"ADCU_LateralDistFlag_18", 2789, 1, 1},
        });
        // Driver monitoring and multimedia camera settings.
        deliver_cases({
                {"ivi_dvrPhotoReceive", 1673, 1, 1},
                {"ivi_dmsCameraCtl", 1724, 2, 3},
                {"ivi_FaceIDEmptydataResp", 1752, 1, 1},
        });
        // Body, steering, powertrain and range states.
        deliver_cases({
                {"EMS_GPFLmp", 9, 1, 1},
                {"bcm_rsm_lightSWReason", 461, 3, 7},
                {"VCU_SOCmodesetnew", 1798, 3, 7},
        });
        // CAN message timeouts.
        deliver_cases({
                {"can_msg_timeout_346", 1878, 2, 3},
                {"can_msg_timeout_379", 1888, 2, 3},
                {"can_msg_timeout_37B", 1998, 2, 3},
        });
        // Vehicle configuration.
        deliver_cases({
                {"cfg_reserve_11", 2172, 4, 11},
                {"cfg_steer_heat", 2176, 2, 3},
                {"cfg_battery_pack", 2182, 2, 3},
        });

        // All ten wheel signals have wire bits for the wheel phases.
        for (unsigned index = 322; index <= 331; ++index) {
            assert(qinggan11_mcu_wheel_supported(qinggan11_fields, qinggan11_wheel_fields, index,
                                                 1));
        }

        // AdasEcu acc and cancel write acc_mode, mpc_icaSts and time_gap_set, which reach their OEM
        // ADAS items, and the set speed ADCU_VsetDis in km/h. A settings byte, which the
        // settings feedback owns, makes the guest reject the logical revision as a whole, and the
        // host state for the guest carries its initial value beside the new speed and ADAS states.
        {
            auto state =
                    voyemu::apply_patch(voyemu::initial_state(voyemu::VehicleCatalog::qinggan11),
                                        {{"speed", 80}, {"gear", 3}, {"gear_valid", 1}});
            voyemu::AdasEcu adas;
            const auto now = voyemu::AdasEcu::Clock::now();
            std::uint32_t sequence = 0;
            auto press = [&](const char *button) {
                const bool pressed =
                        adas.input(state, ++sequence, button, "down", "test", now).handled;
                assert(pressed);
                const auto outcome = adas.input(state, ++sequence, button, "up", "test", now);
                state = voyemu::apply_patch(state, outcome.patch);
            };
            press("acc");
            assert(state.at("raw_oem").at("acc_mode") == 3);
            press("cancel");
            state = voyemu::apply_patch(state, {{"speed", 60}, {"set_byte3", 1}});
            const auto &raw = state.at("raw_oem");
            assert(raw.at("acc_mode") == 0 && raw.at("mpc_icaSts") == 1 &&
                   raw.at("ADCU_VsetDis") == 80 && raw.at("time_gap_set") == 1);
            const auto &speed = qinggan11_fields[pos("speed")];
            auto deliver = [&](const Json &values) {
                for (unsigned i = 0; i < QINGGAN11_FIELDS; i++) {
                    normal[i] = values.at(fields[i].name).get<unsigned>();
                }

                ++offered;

                return dataParser_makedumydata(out, 650) == 650 && qinggan11_ack == offered &&
                       packed(speed.bit, speed.width) == 60;
            };
            const auto rejects = qinggan11_rejects;
            assert(!deliver(raw) && qinggan11_rejects == rejects + 1);
            const auto guest = voyemu::qnx_qinggan11_guest_state(source, raw);
            assert(guest.at("set_byte3") == 0 && guest.at("mpc_icaSts") == 1 &&
                   guest.at("time_gap_set") == 1);
            assert(deliver(guest) && qinggan11_rejects == rejects + 1 && packed(83, 3) == 1 &&
                   packed(692, 3) == 1);

            for (unsigned i = 0; i < QINGGAN11_FIELDS; i++) {
                normal[i] = initial.at(fields[i].name).get<unsigned>();
            }

            ++offered;
            assert(dataParser_makedumydata(out, 650) == 650 && qinggan11_ack == offered);
        }

        // The ICE range keeps the 11-bit OEM slot, whose all-ones the 6.11.1 engine reads as an
        // unknown range, and reaches the 10-bit NEDC slot saturated.
        const auto ice =
                voyemu::apply_patch(voyemu::initial_state(voyemu::VehicleCatalog::qinggan11),
                                    {{"remain_mileage_f", 2047}})
                        .at("raw_oem");

        for (unsigned i = 0; i < QINGGAN11_FIELDS; i++) {
            normal[i] = ice.at(fields[i].name).get<unsigned>();
        }

        ++offered;
        assert(dataParser_makedumydata(out, 650) == 650 && qinggan11_ack == offered);
        assert(packed(2218, 11) == 2047 && packed(1463, 10) == 1023 && packed(2229, 11) == 2047);

        for (unsigned i = 0; i < QINGGAN11_FIELDS; i++) {
            normal[i] = initial.at(fields[i].name).get<unsigned>();
        }

        offered++;
        bad = true;
        qinggan11_have = 0;
        qinggan11_settings.have = 0;
        assert(dataParser_makedumydata(out, 650) == 650 && qinggan11_ack == offered - 1 &&
               !std::memcmp(out, original_bytes, 650));
        enabled = false;
        const auto calls = rpc_calls;
        assert(dataParser_makedumydata(out, 650) == 650 && rpc_calls == calls &&
               !std::memcmp(out, original_bytes, 650));
        auto reject = [&](Json definition) {
            bool rejected = false;
            try {
                voyemu::qnx_qinggan11_mcu_layout(definition);
            } catch (...) {
                rejected = true;
            }

            assert(rejected);
        };
        auto wrong = source;
        wrong["hardware_adapter"] = "sa8155-qinggan11-731";
        reject(wrong);
        wrong = source;
        wrong["source_parser_sha256"] = std::string(64, '0');
        reject(wrong);
        wrong = source;
        wrong["fields"][pos("ready_lamp")]["wire"]["start_bit"] = 0;
        reject(wrong);
        wrong = source;
        wrong["fields"][pos("set_byte0")].erase("unprojected");
        reject(wrong);
        // A row that names its engine item states the case and the output bits it writes; a row
        // without one states neither.
        wrong = source;
        wrong["fields"][pos("ready_lamp")]["wire"].erase("output_width");
        reject(wrong);
        wrong = source;
        wrong["fields"][pos("ready_lamp")]["wire"]["oem_name"] = "ready_lamp";
        reject(wrong);
        wrong = source;
        wrong["fields"][pos("gsm_warnInformation_bak")]["wire"]["case_address"] = "0x0";
        reject(wrong);
        bool old_scope_rejected = false;
        try {
            voyemu::qnx_mcu_request_projection(650, 736, 19);
        } catch (...) {
            old_scope_rejected = true;
        }

        assert(old_scope_rejected);

        // The scopes of earlier guest builds are no longer served.
        for (const unsigned earlier : {33U, 34U, 493U}) {
            bool earlier_rejected = false;
            try {
                voyemu::qnx_mcu_request_projection(650, 736, earlier);
            } catch (...) {
                earlier_rejected = true;
            }

            assert(earlier_rejected);
        }

        const auto projection =
                voyemu::qnx_mcu_projection(voyemu::QnxGuestAdapter::sa8155_qinggan11_611);
        assert(voyemu::qnx_mcu_request_projection(650, 736, qinggan11_projected_fields,
                                                  "sa8155-qinggan11-611") == projection);
        assert(voyemu::qnx_mcu_delivery_scope(Json{{"mcu_projection", projection}}, projection));
        assert(!voyemu::qnx_mcu_delivery_scope(
                Json{{"mcu_projection", projection}},
                voyemu::qnx_mcu_projection(voyemu::QnxGuestAdapter::sa8155_qinggan11_731)));
        const Json metadata = {{"hardware_adapter", "sa8155-qinggan11-611"},
                               {"mcu_projection", projection}};
        assert(voyemu::qnx_runtime_layout("test-profile", metadata).projection == projection);
        auto foreign = metadata;
        foreign["hardware_adapter"] = "sa8155-qinggan11-731";
        bool rejected = false;
        try {
            voyemu::qnx_runtime_layout("test-profile", foreign);
        } catch (...) {
            rejected = true;
        }

        assert(rejected);
        assert(projection.at("hardware_adapter") == "sa8155-qinggan11-611" &&
               projection.at("catalog_fields") == QINGGAN11_FIELDS &&
               projection.at("unsupported_fields") ==
                       QINGGAN11_FIELDS - qinggan11_projected_fields);
        std::cout
                << "sa8155-qinggan11-611 source layout, stock controls, range sentinels, settings feedback, scoped RPC and unchanged unsupported bits passed\n";

        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';

        return 1;
    }
}
