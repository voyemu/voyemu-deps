#include "model.hpp"
#include "private_json_reader.hpp"
#include "process.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
using namespace voyemu;

static void rejects(const std::function<void()> &action) {
    bool threw = false;

    try {
        action();
    } catch (...) {
        threw = true;
    }

    assert(threw);
}

#ifdef _WIN32
// Production state objects must satisfy the InactiveProfileLease DACL predicate:
// owner is the current user and every effective ACE is an ALLOW for the current
// user, SYSTEM or Administrators. Mirrors the lease acceptance test.
static bool state_user_sid(std::vector<unsigned char> &user) {
    HANDLE token = INVALID_HANDLE_VALUE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);

    if (!bytes || bytes > 65536) {
        CloseHandle(token);

        return false;
    }

    user.resize(bytes);
    const bool loaded = GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes) != 0;
    CloseHandle(token);

    return loaded;
}

static bool state_trusted_sid(PSID sid) {
    std::vector<unsigned char> user;

    if (!state_user_sid(user)) {
        return false;
    }

    BYTE system[SECURITY_MAX_SID_SIZE]{};
    DWORD system_size = sizeof system;
    BYTE administrators[SECURITY_MAX_SID_SIZE]{};
    DWORD administrators_size = sizeof administrators;

    return EqualSid(sid, reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid) ||
           (CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &system_size) &&
            EqualSid(sid, system)) ||
           (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators,
                               &administrators_size) &&
            EqualSid(sid, administrators));
}

static bool state_private(const std::filesystem::path &path, bool directory) {
    struct Handle {
        HANDLE value = INVALID_HANDLE_VALUE;

        ~Handle() {
            if (value != INVALID_HANDLE_VALUE) {
                CloseHandle(value);
            }
        }
    } handle;

    handle.value = CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                       (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0),
                               nullptr);

    if (handle.value == INVALID_HANDLE_VALUE) {
        return false;
    }

    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    if (GetSecurityInfo(handle.value, SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                        &acl, nullptr, &descriptor) != ERROR_SUCCESS) {
        return false;
    }

    struct Release {
        PSECURITY_DESCRIPTOR value;

        ~Release() {
            if (value) {
                LocalFree(value);
            }
        }
    } release{descriptor};
    std::vector<unsigned char> user;

    if (!owner || !acl || !state_user_sid(user) ||
        !EqualSid(owner, reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid)) {
        return false;
    }

    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void *raw = nullptr;

        if (!GetAce(acl, i, &raw)) {
            return false;
        }

        const auto *header = static_cast<const ACE_HEADER *>(raw);

        if (header->AceFlags & INHERIT_ONLY_ACE) {
            continue;
        }

        if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
            continue;
        }

        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
            !state_trusted_sid(
                    const_cast<DWORD *>(&static_cast<const ACCESS_ALLOWED_ACE *>(raw)->SidStart))) {
            return false;
        }
    }

    return true;
}

static void state_privacy_regressions(const std::filesystem::path &root) {
    auto check = [](bool value, const char *reason) {
        if (!value) {
            throw std::runtime_error(reason);
        }
    };
    const auto directory = root / "private-state";

    ensure_private_directory(directory);
    write_private_json(directory / "state.json", {{"schema", 1}});
    check(state_private(directory, true), "production directory DACL is not private");
    check(state_private(directory / "state.json", false),
          "production state file DACL is not private");
}

static void state_replace_regressions(const std::filesystem::path &directory) {
    const auto file = directory / "replace-state.json",
               pending = directory / "replace-state.json.pending";
    auto check = [](bool value, const char *reason) {
        if (!value) {
            throw std::runtime_error(reason);
        }
    };
    auto held = [&](bool delete_share) {
        const auto result = CreateFileW(file.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                                                (delete_share ? FILE_SHARE_DELETE : 0),
                                        nullptr, OPEN_EXISTING, 0, nullptr);
        check(result != INVALID_HANDLE_VALUE, "replace fixture reader open");

        return result;
    };
    auto attempted = [&](const Json &value) {
        try {
            write_private_json(file, value);

            return std::string{};
        } catch (const std::exception &error) {
            return std::string(error.what());
        }
    };

    write_private_json(file, {{"epoch", 1}});
    const auto reader = held(true);
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CloseHandle(reader);
    });
    const auto transient = attempted({{"epoch", 2}});
    release.join();
    check(transient.empty(),
          "STATE_REPLACE_TRANSIENT: released ordinary reader aborted publication");
    check(read_private_json(file).at("epoch") == 2 && !std::filesystem::exists(pending),
          "released reader exact publication/cleanup");
    write_private_json(file, {{"epoch", 1}});
    const auto permanent = held(false);
    const auto begin = std::chrono::steady_clock::now();
    const auto blocked = attempted({{"epoch", 2}});
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    CloseHandle(permanent);
    check(!blocked.empty() && elapsed < std::chrono::seconds(1) &&
                  read_private_json(file).at("epoch") == 1 && !std::filesystem::exists(pending),
          "persistent reader bounded refusal preserves state/staging");
    check(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY), "readonly fixture attribute");
    const auto readonly = attempted({{"epoch", 2}});
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
    check(!readonly.empty() && readonly.find("attempts=1") != std::string::npos &&
                  read_private_json(file).at("epoch") == 1 && !std::filesystem::exists(pending),
          "readonly target immediate refusal");
    const auto pinned = held(false);
    const auto retained = directory / "retained-pending.json";
    std::atomic<bool> replaced = false;
    std::thread replacement([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);

        while (std::chrono::steady_clock::now() < deadline) {
            if (MoveFileExW(pending.c_str(), retained.c_str(), 0)) {
                const auto fresh = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr,
                                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

                if (fresh != INVALID_HANDLE_VALUE) {
                    const char bytes[] = "{\"foreign\":true}\n";
                    DWORD count = 0;
                    replaced = WriteFile(fresh, bytes, sizeof(bytes) - 1, &count, nullptr) &&
                               count == sizeof(bytes) - 1;
                    CloseHandle(fresh);
                }

                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const auto changed = attempted({{"epoch", 3}});
    replacement.join();
    CloseHandle(pinned);
    check(replaced && !changed.empty() && read_private_json(file).at("epoch") == 1 &&
                  read_private_json(pending).value("foreign", false),
          "foreign pending incarnation preserved/refused");
    std::filesystem::remove(pending);
    std::filesystem::remove(retained);
    std::atomic<bool> done = false;
    std::atomic<unsigned> metadata_checks = 0;
    std::thread metadata([&] {
        while (!done) {
            const auto burst = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);

            while (!done && std::chrono::steady_clock::now() < burst) {
                try {
                    (void)std::filesystem::exists(file);
                    (void)std::filesystem::file_size(file);
                    ++metadata_checks;
                } catch (...) { /* Ignored: the expected rejection is asserted afterwards. */
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    std::string failed;

    for (int epoch = 4; epoch < 104; ++epoch) {
        failed = attempted({{"epoch", epoch}});

        if (!failed.empty()) {
            break;
        }
    }

    done = true;
    metadata.join();
    check(failed.empty() && metadata_checks > 0 && read_private_json(file).at("epoch") == 103 &&
                  !std::filesystem::exists(pending),
          "transient filesystem metadata exact publication");
    // A writer terminated after creating its staging file leaves it behind.
    { std::ofstream(pending, std::ios::binary) << "{\"epoch\":"; }
    const auto recovered = attempted({{"epoch", 104}});
    check(recovered.empty() && read_private_json(file).at("epoch") == 104 &&
                  !std::filesystem::exists(pending) && state_private(file, false),
          "staging file of a terminated writer blocked publication");
    // A holder whose thread exits without releasing the lock abandons it.
    std::optional<detail::PrivateJsonLock> abandoned;
    std::thread([&] {
        abandoned.emplace(file);
    }).join();
    const auto after_abandon = attempted({{"epoch", 105}});
    check(after_abandon.empty() && read_private_json(file).at("epoch") == 105,
          "abandoned state lock blocked publication");
}
#endif

int main(int argc, char **argv) try {
#ifdef _WIN32
    if (argc == 3 && std::string_view(argv[1]) == "--replace-regression") {
        try {
            const auto directory = std::filesystem::path(argv[2]) /
                                   ("model-replace-" + random_token());
            ensure_private_directory(directory);
            state_replace_regressions(directory);
            std::filesystem::remove_all(directory);
            std::cout << "bounded state replace regressions passed\n";

            return 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';

            return 1;
        }
    }

    if (argc == 3 && std::string_view(argv[1]) == "--state-writer") {
        try {
            for (int epoch = 131; epoch <= 258; ++epoch) {
                write_private_json(argv[2], {{"epoch", epoch}});
            }

            return 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';

            return 1;
        }
    }
#else
    (void)argc;
    (void)argv;
#endif
    assert(valid_profile_name("vehicle-11-language-20260101-1.2.3"));
    assert(valid_profile_name("android-11-1.0.0") && valid_profile_name("free_2021.13"));

    for (const auto &name : std::vector<std::string>{
                 "", ".", "..", "../other", "a/b", "a\\b", ".hidden", "-flag", "_leading", "a\nb",
                 "a\rb", std::string("a\0b", 3), std::string(121, 'a')}) {
        assert(!valid_profile_name(name));
    }

    assert(qnx_theme_display_matches(1, 1) && qnx_theme_display_matches(2, 2) &&
           qnx_theme_display_matches(2, 5) && qnx_theme_display_matches(3, 3) &&
           qnx_theme_display_matches(4, 6));

    for (unsigned requested = 0; requested <= 7; ++requested) {
        assert(!qnx_theme_display_matches(requested,
                                          4)); // Lower display4 is shared by requests1/3/4; it
                                               // cannot prove the selected request.
    }

    assert(!qnx_theme_display_matches(5, 5) && !qnx_theme_display_matches(6, 6) &&
           !qnx_theme_display_matches(4, 4) && !qnx_theme_display_matches(2, 6) &&
           !qnx_theme_display_matches(0, 0));
    assert(catalog(VehicleCatalog::qinggan09).size() == 579);

    for (std::size_t i = 0; i < catalog(VehicleCatalog::qinggan09).size(); ++i) {
        assert(catalog(VehicleCatalog::qinggan09)[i].index == i + 1);
    }

    const auto normal = normal_initial_values(VehicleCatalog::qinggan09);
    assert(normal.size() == 35);
    auto initial = initial_state(VehicleCatalog::qinggan09);
    validate_state(initial);
    const auto &initial_raw = initial.at("raw_oem");
    assert(initial["sequence"] == 1 && initial["map_sha256"] == qinggan09_map_sha256);
    assert(normal["gear_valid"] == 1 && normal["gear"] == 0 && normal["spd_valid"] == 1);
    assert(normal["speed"] == 0 && normal["speed10x"] == 0 && normal["vcu_ExhibitionSts"] == 0);
    assert(normal["ambient_temp"] == 120 && normal["soc"] == 55 && normal["fuel_percent"] == 50);
    assert(normal["fuel_cap"] == 0 && normal["fuel_cap_err"] == 0 &&
           normal["dcdc_systemSts"] == 2 && normal["ic_bat_voltage"] == 1380);
    assert(normal["esc_hhcAvailable"] == 1 && normal["esc_hdcAvailable"] == 1);

    for (const auto *door : {"door_fl", "door_fr", "door_rl", "door_rr"}) {
        assert(normal[door] == 0);
    }

    for (const auto *corner : {"LF", "RF", "LR", "RR"}) {
        const auto name = std::string(corner);
        assert(normal["can_v1_5_bcm_tpms" + name + "Sts"] == 1 &&
               normal["bcm_tpms" + name + "Sts"] == 1);
        assert(normal["bcm_tyre" + name + "Pressure"] == 24 &&
               normal["bcm_tyre" + name + "Temp"] == 81);
    }

    for (const auto &field : catalog(VehicleCatalog::qinggan09)) {
        assert(initial_raw[field.name] ==
               (normal.contains(field.name) ? normal[field.name] : Json(field.initial)));
    }

    auto customized = apply_patch(initial, {{"odo", 12345},
                                            {"cfg_acc", 0},
                                            {"cfg_reserve_14", 72},
                                            {"abs_fault", 1},
                                            {"gear", 3},
                                            {"speed", 65},
                                            {"soc", 42},
                                            {"door_rr", 1}});
    auto normalized = apply_patch(customized, normal);
    validate_state(normalized);
    assert(normalized["sequence"] == customized["sequence"].get<unsigned>() + 1);

    for (const auto &[name, value] : customized["raw_oem"].items()) {
        assert(normalized["raw_oem"][name] == (normal.contains(name) ? normal[name] : value));
    }

    assert(customized["raw_oem"]["gear"] == 3 && customized["raw_oem"]["soc"] == 42);
    // A fresh session must not mutate the intentionally saved driving scenario.
    const auto saved_scenario = customized;
    const auto fresh = select_vehicle_startup(VehicleStartup::fresh, VehicleCatalog::qinggan09,
                                              Json::object(), &saved_scenario, 200);
    assert(fresh["raw_oem"] == initial_raw && fresh["sequence"] == 201);
    assert(saved_scenario == customized);
    const auto resumed = select_vehicle_startup(VehicleStartup::resume, VehicleCatalog::qinggan09,
                                                Json::object(), &saved_scenario, 200);
    assert(resumed["raw_oem"] == saved_scenario["raw_oem"] && resumed["sequence"] == 201);
    assert(saved_scenario == customized);
    const auto configured = select_vehicle_startup(VehicleStartup::fresh, VehicleCatalog::qinggan09,
                                                   {{"soc", 73}}, nullptr);
    assert(configured["raw_oem"]["soc"] == 73 && configured["raw_oem"]["gear"] == 0);
    assert(configured["raw_oem"]["alarm_play_req"] ==
           initial_raw["alarm_play_req"]); // No guessed alarm semantics.
    rejects([&] {
        select_vehicle_startup(VehicleStartup::resume, VehicleCatalog::qinggan09, Json::object(),
                               nullptr);
    });
    rejects([&] {
        select_vehicle_startup(VehicleStartup::fresh, VehicleCatalog::qinggan09, {{"invented", 1}},
                               nullptr);
    });
    rejects([&] {
        select_vehicle_startup(VehicleStartup::fresh, VehicleCatalog::qinggan09, {{"swm_menu", 1}},
                               nullptr);
    });
    rejects([&] {
        select_vehicle_startup(VehicleStartup::fresh, VehicleCatalog::qinggan09, Json::array(),
                               nullptr);
    });
    rejects([&] {
        select_vehicle_startup(VehicleStartup::fresh, VehicleCatalog::qinggan09, Json::object(),
                               nullptr, UINT32_MAX);
    });
    auto malformed_saved = saved_scenario;
    malformed_saved["raw_oem"].erase("ign");
    rejects([&] {
        select_vehicle_startup(VehicleStartup::resume, VehicleCatalog::qinggan09, Json::object(),
                               &malformed_saved);
    });
    rejects([&] {
        select_vehicle_startup(static_cast<VehicleStartup>(42), VehicleCatalog::qinggan09,
                               Json::object(), nullptr);
    });
    // The qinggan11 catalogue holds the 736 wire fields of the qinggan11 MCU packet in descriptor
    // order; shared fields keep the qinggan09 name, width aside, and the default.
    const auto &platform = catalog(VehicleCatalog::qinggan11);
    assert(platform.size() == 736 && vehicle_catalog("qinggan11") == VehicleCatalog::qinggan11 &&
           vehicle_catalog("qinggan09") == VehicleCatalog::qinggan09 &&
           vehicle_catalog("passion") == VehicleCatalog::passion);
    rejects([] {
        vehicle_catalog("unknown");
    });
    std::set<std::string> platform_names;

    for (std::size_t i = 0; i < platform.size(); ++i) {
        assert(platform[i].index == i + 1 && platform_names.insert(platform[i].name).second);
    }

    auto field_of = [](VehicleCatalog which, std::string_view name) {
        const auto &fields = catalog(which);

        return *std::find_if(fields.begin(), fields.end(), [&](const Field &field) {
            return field.name == name;
        });
    };
    assert(field_of(VehicleCatalog::qinggan11, "ign").index == 1 &&
           field_of(VehicleCatalog::qinggan11, "remain_mileage_f").index == 639 &&
           field_of(VehicleCatalog::qinggan11, "remain_mileage_f").width == 11 &&
           field_of(VehicleCatalog::qinggan11, "vcu_ready").index == 476 &&
           field_of(VehicleCatalog::qinggan11, "reserved_04").width == 38 &&
           field_of(VehicleCatalog::qinggan11, "cfg_acc").initial ==
                   field_of(VehicleCatalog::qinggan09, "cfg_acc").initial);
    assert(!platform_names.contains("mrr_vSetDis") && platform_names.contains("ADCU_VsetDis"));
    const auto platform_initial = initial_state(VehicleCatalog::qinggan11);
    validate_state(platform_initial);
    assert(platform_initial["map_sha256"] == qinggan11_map_sha256 &&
           state_catalog(platform_initial) == VehicleCatalog::qinggan11 &&
           platform_initial["raw_oem"].size() == 736 && platform_initial["raw_oem"]["soc"] == 55 &&
           platform_initial["raw_oem"]["vcu_ready"] == 1 &&
           platform_initial["raw_oem"]["ADCU_TargetType_1"] == 0);
    // A field of one catalogue is unknown to the other, and a state names its own catalogue.
    rejects([&] {
        apply_patch(platform_initial, {{"mrr_vSetDis", 1}});
    });
    rejects([&] {
        apply_patch(initial, {{"vcu_ready", 0}});
    });
    auto foreign = platform_initial;
    foreign["map_sha256"] = qinggan09_map_sha256;
    rejects([&] {
        validate_state(foreign);
    });
    rejects([&] {
        select_vehicle_startup(VehicleStartup::resume, VehicleCatalog::qinggan11, Json::object(),
                               &saved_scenario);
    });
    // ready_lamp carries the qinggan11 VCU READY flag; a wide range saturates its narrower copy.
    const auto not_ready = apply_patch(platform_initial, {{"ready_lamp", 0}});
    assert(not_ready["raw_oem"]["vcu_ready"] == 0);
    assert(apply_patch(not_ready, {{"ready_lamp", 1}, {"vcu_ready", 0}})["raw_oem"]["vcu_ready"] ==
           0);
    const auto unknown_range = apply_patch(platform_initial, {{"remain_mileage_f", 2047}});
    assert(unknown_range["raw_oem"]["remain_mileage_f_wltc"] == 2047 &&
           unknown_range["raw_oem"]["remain_mileage_f_nedc"] == 1023);
    const auto wide = apply_patch(platform_initial, {{"reserved_04", 0x3fffffffffULL}});
    assert(wide["raw_oem"]["reserved_04"] == 0x3fffffffffULL);
    rejects([&] {
        apply_patch(platform_initial, {{"reserved_04", 0x4000000000ULL}});
    });
    // The Passion catalogue holds the 1598 cluster input signals of the OEM SOME/IP service table,
    // numbered from 1, each with its table declaration; values stay raw wire integers.
    const auto &passion = catalog(VehicleCatalog::passion);
    const auto passion_signals = catalog_signals(VehicleCatalog::passion);
    assert(passion.size() == 1598 && passion_signals.size() == passion.size() &&
           catalog_signals(VehicleCatalog::qinggan11).empty());
    std::set<std::string> passion_names;

    for (std::size_t i = 0; i < passion.size(); ++i) {
        assert(passion[i].index == i + 1 && passion_names.insert(passion[i].name).second &&
               !passion[i].name.starts_with("8155QNX.") && passion[i].width % 8 == 0 &&
               passion[i].width <= 64 && passion[i].initial <= field_maximum(passion[i].width));
    }

    const auto signal_of = [&](std::string_view name) {
        const auto *field = catalog_field(VehicleCatalog::passion, name);
        assert(field && &passion[field->index - 1] == field);

        return passion_signals[field->index - 1];
    };
    const std::string vehicle_info =
            "VehicleDrivingInfo.Notify_f_VehicleDrivingInform.VehicleInform.VehicleInform.";
    const auto display_speed = signal_of(vehicle_info + "VI_VehicleDisplaySpeed.VehicleSpeed");
    assert(display_speed.id == 212 && display_speed.type == "uint32" &&
           display_speed.resolution == 0.01 && display_speed.offset == -500.0 &&
           display_speed.unit == "kmph" && display_speed.options.empty() &&
           catalog_field(VehicleCatalog::passion,
                         vehicle_info + "VI_VehicleDisplaySpeed.VehicleSpeed")
                           ->width == 32);
    const auto gear = signal_of(vehicle_info + "VI_ActualGear.AG_ActualGear");
    assert(gear.options.size() == 5 && gear.options[3].raw == 3 &&
           gear.options[3].name == "AG_D_GEAR" && gear.options[4].raw == 255);
    const auto inside = signal_of("GeneralACSet.Notify_f_GeneralACSts.GeneralACSts.InsideTemp");
    assert(catalog_field(VehicleCatalog::passion,
                         "GeneralACSet.Notify_f_GeneralACSts.GeneralACSts.InsideTemp")
                           ->initial == 65534 &&
           inside.resolution == 0.1 && inside.offset == -50.0);
    assert(signal_of("AssidDrvgMgr.Notify_f_ADrvgMgrFunOuC.ADrvgMgrFunOuC.ADrvgMgrFunOu."
                     "ACC_TargetSpeed_kph")
                   .invalid == 255U);
    // Get replies and the FF, RR and Set requests of the QNX side are not inputs of the model.
    assert(!catalog_field(VehicleCatalog::passion,
                          "FrontACSet.FF_A_COnOffReqFro.A_COnOffReqFro.OnOffReq"));
    const auto passion_initial = initial_state(VehicleCatalog::passion);
    validate_state(passion_initial);
    assert(passion_initial["map_sha256"] == passion_map_sha256 &&
           state_catalog(passion_initial) == VehicleCatalog::passion &&
           passion_initial["raw_oem"].size() == 1598 &&
           passion_initial.dump(2).size() < vehicle_state_bytes);
    const std::string crc = "DoorStsService.Notify_f_DoorSts.AllDoorSts.E2E_Header.E2E_CRC";
    assert(catalog_field(VehicleCatalog::passion, crc)->width == 64);
    assert(apply_patch(passion_initial, {{crc, UINT64_MAX}})["raw_oem"][crc] == UINT64_MAX);
    rejects([&] {
        apply_patch(passion_initial, {{"soc", 55}});
    });
    rejects([&] {
        apply_patch(initial, {{crc, 0}});
    });
    auto speed = apply_patch(initial, {{"speed", 65}, {"soc", 73}});
    assert(speed["raw_oem"]["speed10x"] == 650);
    auto raw = apply_patch(speed, {{"speed10x", 900}, {"gear", 255}});
    assert(raw["raw_oem"]["speed"] == 90 && raw["raw_oem"]["gear"] == 255);
    assert(initial["raw_oem"]["speed"] == 0);
    // One written field keeps its coupled fields consistent for every writer.
    const auto power = apply_patch(initial, {{"actual_power", 30}, {"allowed_power", 80}});
    assert(power["raw_oem"]["actual_power10x"] == 300 &&
           power["raw_oem"]["allowed_power10x"] == 800);
    const auto ev = apply_patch(initial, {{"remain_mileage_e", 420}});
    assert(ev["raw_oem"]["vcu_remainingMileageReal"] == 420 &&
           ev["raw_oem"]["vcu_remainingMileageCLTCWLTC"] == 420);
    const auto ice = apply_patch(initial, {{"remain_mileage_f", 610}});
    assert(ice["raw_oem"]["remain_mileage_f_nedc"] == 610 &&
           ice["raw_oem"]["remain_mileage_f_wltc"] == 610);
    auto unknown_tyres = initial;

    for (const std::string corner : {"LF", "RF", "LR", "RR"}) {
        unknown_tyres["raw_oem"]["bcm_tpms" + corner + "Sts"] = 0;
        unknown_tyres["raw_oem"]["can_v1_5_bcm_tpms" + corner + "Sts"] = 0;
    }

    const auto pressure = apply_patch(unknown_tyres, {{"bcm_tyreLFPressure", 25}});
    assert(pressure["raw_oem"]["bcm_tpmsLFSts"] == 1 &&
           pressure["raw_oem"]["can_v1_5_bcm_tpmsLFSts"] == 1 &&
           pressure["raw_oem"]["bcm_tpmsRFSts"] == 0);
    const auto temperature = apply_patch(unknown_tyres, {{"bcm_tyreRRTemp", 81}});
    assert(temperature["raw_oem"]["bcm_tpmsRRSts"] == 1 &&
           temperature["raw_oem"]["can_v1_5_bcm_tpmsRRSts"] == 1);
    // A coupled field named in the same patch keeps its own value.
    const auto explicit_target =
            apply_patch(initial, {{"remain_mileage_e", 420}, {"vcu_remainingMileageReal", 400}});
    assert(explicit_target["raw_oem"]["vcu_remainingMileageReal"] == 400 &&
           explicit_target["raw_oem"]["vcu_remainingMileageCLTCWLTC"] == 420);
    const auto explicit_status = apply_patch(unknown_tyres,
                                             {{"bcm_tyreLFPressure", 25}, {"bcm_tpmsLFSts", 2}});
    assert(explicit_status["raw_oem"]["bcm_tpmsLFSts"] == 2 &&
           explicit_status["raw_oem"]["can_v1_5_bcm_tpmsLFSts"] == 1);
    // A target alone changes nothing else, and validation of saved states stays as it was.
    const auto target_only = apply_patch(initial, {{"actual_power10x", 123}});
    assert(target_only["raw_oem"]["actual_power"] == initial["raw_oem"]["actual_power"]);
    validate_state(target_only);
    rejects([&] {
        apply_patch(initial, {{"speed", 10}, {"speed10x", 90}});
    });
    rejects([&] {
        apply_patch(initial, {{"speed10x", 11}});
    });
    rejects([&] {
        apply_patch(initial, {{"soc", 101}});
    });
    rejects([&] {
        apply_patch(initial, {{"soc", true}});
    });
    rejects([&] {
        apply_patch(initial, {{"door_fl", 2}});
    });
    rejects([&] {
        apply_patch(initial, {{"swm_menu", 0}});
    });
    rejects([&] {
        apply_patch(initial, {{"missing", 0}});
    });
    auto missing = initial;
    missing["raw_oem"].erase("ign");
    rejects([&] {
        validate_state(missing);
    });
    auto transient = initial;
    transient["raw_oem"]["swm_menu"] = 1;
    rejects([&] {
        validate_state(transient);
    });
    auto dir = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
               ("voyemu-native-model-test-" + random_token());
    auto created = std::filesystem::create_directory(dir);
    assert(created);

    if (!created) {
        throw std::runtime_error("test directory unexpectedly exists");
    }

    {
        const auto file = dir / "formatted-state.json";
        const std::string pretty = "{\n  \"epoch\": 17,\n  \"retained\": true\n}\n";
        write_private_json_bytes(file, pretty);
        assert(read_private_json_bytes(file) == pretty &&
               read_private_json(file).at("epoch") == 17);
        rejects([&] {
            write_private_json_bytes(file, "not json");
        });
        assert(read_private_json_bytes(file) == pretty);
        rejects([&] {
            write_private_json_bytes(file, pretty, 4);
        });
        assert(read_private_json_bytes(file) == pretty);
        const auto alias = dir / "formatted-state-link.json";
        std::filesystem::create_symlink(file, alias);
        rejects([&] {
            read_private_json_bytes(alias);
        });
        rejects([&] {
            write_private_json_bytes(alias, pretty);
        });
    }
#ifdef _WIN32
    state_replace_regressions(dir);
    state_privacy_regressions(dir);
    const auto shared = dir / "reader-state.json";
    const Json old_record = {{"epoch", 1}}, new_record = {{"epoch", 2}};
    write_private_json(shared, old_record);
    std::promise<void> attempting, finished;
    auto attempted = attempting.get_future(), completed = finished.get_future();
    std::thread blocked_writer;
    bool blocked = false;
    {
        detail::PrivateJsonReader held(shared, 128);
        blocked_writer = std::thread([&] {
            attempting.set_value();
            try {
                write_private_json(shared, new_record);
                finished.set_value();
            } catch (...) {
                finished.set_exception(std::current_exception());
            }
        });
        attempted.get();
        blocked = completed.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
        assert(Json::parse(held.text()) == old_record);
    }
    blocked_writer.join();

    if (!blocked) {
        std::cerr << "held reader failed to fence publication\n";

        return 1;
    }

    completed.get();
    assert(read_private_json(shared) == new_record);
    std::atomic<bool> writer_done = false;
    std::exception_ptr writer_error, reader_error;
    std::thread writer([&] {
        try {
            for (int epoch = 3; epoch <= 130; ++epoch) {
                write_private_json(shared, {{"epoch", epoch}});
            }
        } catch (...) {
            writer_error = std::current_exception();
        }

        writer_done = true;
    });

    do {
        try {
            const auto current = read_private_json(shared, 128);
            assert(current.at("epoch").is_number_integer());
        } catch (...) {
            reader_error = std::current_exception();
            break;
        }
    } while (!writer_done);

    writer.join();

    if (writer_error || reader_error) {
        try {
            std::rethrow_exception(writer_error ? writer_error : reader_error);
        } catch (const std::exception &error) {
            std::cerr << "concurrent state publication failed: " << error.what() << '\n';

            return 1;
        }
    }

    ProcessSpec process;
    process.executable = std::filesystem::canonical(argv[0]);
    process.arguments = {"--state-writer", shared.string()};
    process.directory = dir;
    process.output = dir / "writer.log";
    auto child = ChildProcess::launch(process);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::exception_ptr process_read_error;
    std::size_t reads = 0;

    while (child.alive() && std::chrono::steady_clock::now() < deadline) {
        try {
            read_private_json(shared, 128);
            ++reads;
        } catch (...) {
            process_read_error = std::current_exception();
            break;
        }
    }

    const bool child_terminal = !child.alive();

    if (!child_terminal) {
        child.terminate(1000);
    }

    if (!child_terminal || child.exit_code() != 0 || process_read_error || !reads) {
        if (process_read_error) {
            try {
                std::rethrow_exception(process_read_error);
            } catch (const std::exception &error) {
                std::cerr << error.what() << '\n';
            }
        }

        std::cerr << "cross-process state coordination failed\n";

        return 1;
    }

    const auto alias = dir / "hardlink-state.json";
    std::filesystem::create_hard_link(shared, alias);
    rejects([&] {
        read_private_json(shared);
    });
    rejects([&] {
        detail::PrivateJsonReader held(alias, 128);
    });
    std::filesystem::remove(alias);
    rejects([&] {
        detail::PrivateJsonReader held(shared, 1);
    });
#endif
    VehicleModel model(dir / "state.json", VehicleCatalog::qinggan09);
    assert(model.state()["raw_oem"] == initial_raw);
    model.patch({{"soc", 42},
                 {"gear", 3},
                 {"speed", 65},
                 {"ambient_temp", 130},
                 {"fuel_percent", 60},
                 {"door_rr", 1},
                 {"bcm_tyreLFPressure", 27},
                 {"odo", 12345}});
    const auto saved = model.state();
    VehicleModel restored(dir / "state.json", VehicleCatalog::qinggan09);
    assert(restored.state() == saved && read_private_json(dir / "state.json") == saved);
    auto before_floor = restored.state();
    restored.advance_revision(1);
    assert(restored.state() == before_floor);
    restored.advance_revision(128);
    assert(restored.state()["sequence"] == 129 &&
           restored.state()["raw_oem"] == before_floor["raw_oem"]);
    assert(read_private_json(dir / "state.json") == restored.state());
    auto at_floor = restored.state();
    rejects([&] {
        restored.advance_revision(UINT32_MAX);
    });
    assert(restored.state() == at_floor && read_private_json(dir / "state.json") == at_floor);
    auto original = restored.state();
    rejects([&] {
        restored.patch({{"soc", 999}});
    });
    assert(restored.state() == original);
    // A Passion state exceeds the default bound of a private JSON file.
    VehicleModel passion_model(dir / "passion-state.json", VehicleCatalog::passion);
    assert(VehicleModel(dir / "passion-state.json", VehicleCatalog::passion).state() ==
                   passion_model.state() &&
           std::filesystem::file_size(dir / "passion-state.json") > 128 * 1024);
#ifndef _WIN32
    {
        // A writer terminated after creating its staging file leaves it behind.
        const auto file = dir / "replace-state.json", pending = dir / "replace-state.json.pending";
        { std::ofstream(pending, std::ios::binary) << "{\"epoch\":"; }
        write_private_json(file, {{"epoch", 1}});
        assert(read_private_json(file).at("epoch") == 1 && !std::filesystem::exists(pending));
        assert((std::filesystem::status(file).permissions() & std::filesystem::perms::all) ==
               (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
        // Writers wait for the lock another writer of the directory holds.
        const int held = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        assert(held >= 0 && ::flock(held, LOCK_EX) == 0);
        auto waiting = std::async(std::launch::async, [&] {
            write_private_json(file, {{"epoch", 2}});
        });
        assert(waiting.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout &&
               read_private_json(file).at("epoch") == 1 && !std::filesystem::exists(pending));
        ::close(held);
        waiting.get();
        assert(read_private_json(file).at("epoch") == 2);
        // Concurrent writers of one file publish whole records.
        std::vector<std::future<void>> writers;

        for (int writer = 0; writer < 4; ++writer) {
            writers.push_back(std::async(std::launch::async, [&, writer] {
                for (int epoch = 0; epoch < 50; ++epoch) {
                    write_private_json(file, {{"epoch", writer * 100 + epoch}});
                }
            }));
        }

        for (auto &writer : writers) {
            writer.get();
        }

        assert(read_private_json(file).at("epoch").get<int>() % 100 == 49 &&
               !std::filesystem::exists(pending));
    }
    // Missing parents of a private directory are private as well.
    ensure_private_directory(dir / "missing/parents/leaf");

    for (const auto &created :
         {dir / "missing", dir / "missing/parents", dir / "missing/parents/leaf"}) {
        assert(std::filesystem::status(created).permissions() == std::filesystem::perms::owner_all);
    }

    std::filesystem::create_symlink(dir / "state.json", dir / "link.json");
    rejects([&] {
        read_private_json(dir / "link.json");
    });
#endif
    std::filesystem::remove_all(dir);
    std::cout << "model guards and atomic persistence passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
