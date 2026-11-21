#include "profile_catalog.hpp"
#include "can_model_candidate_binding.hpp"
#include "model.hpp"
#include "passion_presentation.hpp"
#include "passion_wheel.hpp"
#include "process.hpp"
#include "qinggan11_startup.hpp"
#include "selinux_patch.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <set>
using namespace voyemu;

static void check(bool v, const char *text) {
    if (!v) {
        throw std::runtime_error(text);
    }
}

static Json normalized_profile_pair(Json base, Json copy) {
    check(!base.contains("local") && !base.contains("capture"), "base identity is not canonical");
    check(copy.contains("id") && copy.at("id").is_string() && copy.contains("title") &&
                  copy.at("title").is_string() && copy.value("local", false),
          "copy identity missing");
    check(copy.at("capture").is_object() && copy.at("capture").size() == 2 &&
                  copy.at("capture").at("profile") == copy.at("id") &&
                  copy.at("capture").at("referenceProfile").is_string(),
          "copy reference invalid");

    if (base.contains("id")) {
        check(base.at("id") == copy.at("capture").at("referenceProfile"),
              "base identity differs from copy reference");
        base.erase("id");
    }

    base.erase("title");

    for (const auto *key : {"id", "title", "local", "capture"}) {
        copy.erase(key);
    }

    check(base.at("emulator").at("port").is_number_integer() &&
                  copy.at("emulator").at("port").is_number_integer(),
          "port missing");
    copy["emulator"]["port"] = base.at("emulator").at("port");
    check(base == copy, "functional profile differs");

    return copy;
}

static std::map<std::string, std::string> parity_inventory(const std::filesystem::path &root) {
    namespace fs = std::filesystem;
    check(fs::is_directory(root) && !fs::is_symlink(fs::symlink_status(root)),
          "profile root invalid");
    std::map<std::string, std::string> files;

    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        const auto status = entry.symlink_status();
        check(!fs::is_symlink(status), "profile symlink forbidden");

        if (fs::is_directory(status)) {
            continue;
        }

        check(fs::is_regular_file(status), "profile nonregular file forbidden");
        const auto rel = entry.path().lexically_relative(root).generic_string();

        if (rel != "profile.json") {
            files.emplace(rel, sha256_file(entry.path()));
        }
    }

    return files;
}

static void verify_profile_pair(const std::filesystem::path &base,
                                const std::filesystem::path &copy) {
    std::ifstream a(base / "profile.json"), b(copy / "profile.json");
    Json x, y;

    a >> x;
    b >> y;
    check(y.at("id") == copy.filename().string() &&
                  y.at("capture").at("referenceProfile") == base.filename().string(),
          "identity or reference does not identify profile");
    (void)normalized_profile_pair(x, y);
    check(parity_inventory(base) == parity_inventory(copy), "profile source inventory differs");
}

static void verify_profile_pair_unit() {
    Json a = {{"emulator", {{"port", 1000}}}, {"original", {{"sample", {{"sha256", "a"}}}}}};
    Json b = a;

    b["id"] = "sample-copy";
    b["title"] = "copy";
    b["local"] = true;
    b["capture"] = {{"profile", "sample-copy"}, {"referenceProfile", "sample"}};
    b["emulator"]["port"] = 1001;
    (void)normalized_profile_pair(a, b);
    b["original"]["sample"]["sha256"] = "b";
    bool denied = false;
    try {
        (void)normalized_profile_pair(a, b);
    } catch (...) {
        denied = true;
    }

    check(denied, "functional mismatch accepted");
    b["original"]["sample"]["sha256"] = "a";
    a["id"] = "sample";
    (void)normalized_profile_pair(a, b);
    a["id"] = "different";
    denied = false;
    try {
        (void)normalized_profile_pair(a, b);
    } catch (...) {
        denied = true;
    }

    check(denied, "mismatching canonical base id accepted");
}

static void verify_profile_properties() {
    const auto rejected = [](const Json &definition, auto read) {
        try {
            (void)read(definition);
        } catch (const VoyemuError &error) {
            return error.code() == "PROFILE_INVALID";
        }

        return false;
    };
    const auto platform = [](const Json &definition) {
        return profile_platform(definition);
    };
    const auto board = [](const Json &definition) {
        return profile_board(definition);
    };
    const auto title = [](const Json &definition) {
        return profile_title(definition);
    };
    const Json synthetic = {{"id", "synthetic-profile"},
                            {"title", "Synthetic 1.0"},
                            {"platform", "qinggan11"},
                            {"board", "8155"}};

    check(profile_platform(synthetic) == ProfilePlatform::qinggan11 &&
                  profile_board(synthetic) == ProfileBoard::sa8155 &&
                  profile_title(synthetic) == "Synthetic 1.0",
          "declared platform, board and title are read");
    check(profile_platform(Json{{"platform", "qinggan09"}}) == ProfilePlatform::qinggan09 &&
                  profile_platform(Json{{"platform", "passion"}}) == ProfilePlatform::passion &&
                  profile_board(Json{{"board", "nxp"}}) == ProfileBoard::nxp,
          "every platform and board value is read");

    for (const auto &value : Json::array({"qinggan10", "QINGGAN11", "", 11, nullptr})) {
        auto changed = synthetic;
        changed["platform"] = value;
        check(rejected(changed, platform), "unknown platform rejected");
        changed = synthetic;
        changed["board"] = value;
        check(rejected(changed, board), "unknown board rejected");
    }

    for (const auto *field : {"platform", "board", "title"}) {
        auto missing = synthetic;
        missing.erase(field);
        check(rejected(missing, platform) || rejected(missing, board) || rejected(missing, title),
              "missing property rejected");
    }

    check(rejected(Json{{"title", ""}}, title) && rejected(Json{{"title", 1}}, title) &&
                  rejected(Json::array(), platform),
          "empty or malformed title and definition rejected");
}

static void verify_can_model_binding() {
    const std::string p(64, 'a'), s(64, 'b'), h(64, 'c'),
            name = "profile", path = "/system/bin/voyemu-can-model-observer",
            artifact = "component:can-model-observer";
    Json row = {{"artifact", artifact}, {"target", path}, {"mode", "0755"},
                {"mutable", false},     {"package", ""},  {"sha256", h}};
    Json proof = {{"id", "artifact:" + artifact + ":" + path},
                  {"passed", true},
                  {"expected", h},
                  {"actual", h}};
    Json c = {{"schema", 1},
              {"kind", "android-candidate"},
              {"profile", name},
              {"profile_sha256", p},
              {"stopped", true},
              {"prepared_VM", true},
              {"cold_boot_verified", true},
              {"old_machine_modified", false},
              {"android", {{"images", {{"system.img", {{"sha256", s}}}}}}},
              {"installed", Json::array({row})},
              {"checks", Json::array({proof})}};

    check(can_model_candidate_hash(c, name, p, s) == h, "valid CAN receipt rejected");
    auto reject = [&](const Json &bad) {
        bool denied = false;
        try {
            can_model_candidate_hash(bad, name, p, s);
        } catch (...) {
            denied = true;
        }

        check(denied, "invalid CAN receipt accepted");
    };
    auto b = c;
    b["installed"].push_back(row);
    reject(b);
    b = c;
    b["checks"].push_back(proof);
    reject(b);
    b = c;
    b["checks"][0]["actual"] = p;
    reject(b);
    b = c;
    b["android"]["images"]["system.img"]["sha256"] = p;
    reject(b);
    b = c;
    b["installed"][0]["mutable"] = true;
    reject(b);
    b = c;
    b["installed"] = Json::array();
    reject(b);
    b = c;
    b["profile_sha256"] = s;
    reject(b);
    b = c;
    b["profile"] = "other";
    reject(b);
}

static void verify_qinggan11_phone_graph(const Json &definition) {
    check(definition.at("components").at("bluetooth") == "components/bluetooth",
          "actual canonical Phone component required");
    check(qinggan11_wheel_capability(definition) ==
                  qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1"),
          "actual canonical Phone graph preserves bounded wheel capability");
    auto legacy = definition;
    legacy["components"].erase("bluetooth");
    legacy["patches"].erase("BluetoothPhone");
    legacy["original"].erase("BluetoothPhone");
    auto &legacy_nodes = legacy["install"];
    legacy_nodes.erase(std::remove_if(legacy_nodes.begin(), legacy_nodes.end(),
                                      [](const auto &n) {
                                          return n.at("artifact") == "component:bluetooth" ||
                                                 n.at("artifact") == "patched:BluetoothPhone";
                                      }),
                       legacy_nodes.end());
    // The producer replaced this exact baseline OEM adapter node; legacy keeps it installed.
    legacy_nodes.push_back({{"artifact", "original:Ivt8BluetoothAdapter"},
                            {"type", "file"},
                            {"target", "/system/framework/Ivt8BluetoothAdapter.jar"},
                            {"mode", "0644"}});
    check(qinggan11_wheel_capability(legacy) ==
                  qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1"),
          "historical graph with original OEM Bluetooth adapter retains exact wheel capability");

    for (unsigned mode = 0; mode < 26; ++mode) {
        auto bad = definition;

        if (mode == 0) {
            bad["components"]["bluetooth"] = "components/foreign";
        }

        if (mode == 1) {
            bad["components"].erase("bluetooth");
        }

        if (mode == 2) {
            bad["original"].erase("BluetoothPhone");
        }

        if (mode == 3) {
            bad["patches"].erase("BluetoothPhone");
        }

        if (mode == 4) {
            bad["patches"]["BluetoothPhone"]["input"] = "original:foreign";
        }

        if (mode == 5) {
            bad["patches"]["BluetoothPhone"]["steps"] = Json::array({"phone-provider.json"});
        }

        if (mode == 6) {
            bad["patches"]["BluetoothPhone"]["sign"] = "foreign";
        }

        if (mode == 7) {
            bad["components"]["foreign"] = "components/foreign";
        }

        if (mode == 8 || mode == 9) {
            bad["install"].erase(std::remove_if(bad["install"].begin(), bad["install"].end(),
                                                [&](const auto &n) {
                                                    return n.at("artifact") ==
                                                           (mode == 8 ? "patched:BluetoothPhone"
                                                                      : "component:bluetooth");
                                                }),
                                 bad["install"].end());
        }

        if (mode == 10 || mode == 11) {
            for (const auto &n : definition.at("install")) {
                if (n.at("artifact") ==
                    (mode == 10 ? "patched:BluetoothPhone" : "component:bluetooth")) {
                    bad["install"].push_back(n);
                }
            }
        }

        for (auto &n : bad["install"]) {
            if (n.at("artifact") == "patched:BluetoothPhone") {
                if (mode == 12) {
                    n["target"] = "/data/app/phone/base.apk";
                }

                if (mode == 13) {
                    n["package"] = "foreign";
                }

                if (mode == 14) {
                    n["mode"] = "0755";
                }

                if (mode == 15) {
                    n.erase("target");
                }

                if (mode == 16) {
                    n["type"] = "file";
                }

                if (mode == 17) {
                    n.erase("mode");
                }
            }

            if (n.at("artifact") == "component:bluetooth") {
                if (mode == 18) {
                    n["target"] = "/system/framework/foreign.jar";
                }

                if (mode == 19) {
                    n["mode"] = "0755";
                }

                if (mode == 20) {
                    n["type"] = "apk";
                }
            }
        }

        if (mode == 21) {
            bad["install"].push_back({{"artifact", "original:Ivt8BluetoothAdapter"},
                                      {"type", "file"},
                                      {"target", "/system/framework/Ivt8BluetoothAdapter.jar"},
                                      {"mode", "0644"}});
        }

        if (mode == 22) {
            bad["install"].push_back(
                    {{"artifact", "original:foreign"},
                     {"type", "file"},
                     {"target",
                      "/system/priv-app/BluetoothPhone-release-signed/BluetoothPhone-release-signed.apk"}});
        }

        if (mode == 23) {
            bad["install"].push_back({{"artifact", "original:BluetoothPhone"}, {"type", "apk"}});
        }

        if (mode == 24) {
            bad["install"].push_back({{"artifact", "original:foreign"},
                                      {"type", "apk"},
                                      {"package", "com.qinggan.bluetoothphone"}});
        }

        if (mode == 25) {
            bad["install"].push_back({{"artifact", "component:foreign"}, {"type", "file"}});
        }

        bool denied = false;
        try {
            (void)qinggan11_wheel_capability(bad);
        } catch (...) {
            denied = true;
        }

        check(denied,
              "optional Phone graph rejects altered, incomplete, duplicate or conflicting source/install nodes");
    }

    auto integrity_only = definition;
    integrity_only["original"]["BluetoothPhone"]["sha256"] = std::string(64, 'a');
    check(qinggan11_wheel_capability(integrity_only) ==
                  qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1"),
          "Phone startup compatibility does not use APK hash gates");
}

static void verify_qinggan11_view_graph(const std::filesystem::path &profiles) {
    const std::string framework_artifact = "patched:original-navi-framework";
    const auto directory = profiles / "free-11-chinese-20260731-7.3.1";
    std::ifstream input(directory / "profile.json");
    Json definition;

    input >> definition;
    verify_qinggan11_phone_graph(definition);
    std::ifstream component_input(directory / "components/view-observation/component.json");
    Json component;
    component_input >> component;
    check(component ==
                  Json{{"type", "jar"},
                       {"minSdk", 30},
                       {"output", "framework.jar"},
                       {"api30ViewObservation", "profiles/common/components/view-observation/src"},
                       {"guestLibs", Json::array({"/system/framework/framework.jar"})}},
          "actual API30 view bridge component contract remains exact");
    check(qinggan11_wheel_capability(definition) ==
                  qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1"),
          "actual Free view bridge preserves exact wheel capability");
    auto legacy = definition;
    legacy["components"].erase("view-observation");
    auto &nodes = legacy["install"];
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [&](const auto &node) {
                                   return node.at("artifact") == framework_artifact;
                               }),
                nodes.end());
    check(qinggan11_wheel_capability(legacy) ==
                  qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1"),
          "legacy six-node Gallery graph remains supported");

    for (unsigned mode = 0; mode < 7; ++mode) {
        auto bad = definition;

        if (mode == 0) {
            bad["components"]["view-observation"] = "components/foreign";
        }

        if (mode == 1) {
            bad["components"]["unknown"] = "components/unknown";
        }

        for (auto &node : bad["install"]) {
            if (node.at("artifact") == framework_artifact) {
                if (mode == 2) {
                    node["target"] = "/system/framework/foreign.jar";
                }

                if (mode == 3) {
                    node["mode"] = "0755";
                }

                if (mode == 4) {
                    node["type"] = "apk";
                }
            }
        }

        if (mode == 5) {
            bad["install"].push_back(Json{{"artifact", framework_artifact},
                                          {"type", "file"},
                                          {"target", "/system/framework/framework.jar"},
                                          {"mode", "0644"}});
        }

        if (mode == 6) {
            bad["install"].push_back(Json{{"artifact", "original:framework"},
                                          {"type", "file"},
                                          {"target", "/system/framework/framework.jar"},
                                          {"mode", "0644"}});
        }

        bool denied = false;
        try {
            (void)qinggan11_wheel_capability(bad);
        } catch (...) {
            denied = true;
        }

        check(denied, "view bridge cannot broaden graph or replace its exact framework target");
    }
}

int main(int argc, char **argv) {
    try {
        verify_profile_pair_unit();
        verify_profile_properties();
        verify_can_model_binding();

        if (argc == 4 && std::string_view(argv[1]) == "--profile-parity") {
            verify_profile_pair(argv[2], argv[3]);
            std::cout << "profile parity passed\n";

            return 0;
        }

        if (argc == 3 && std::string_view(argv[2]) == "--qinggan11-731-wheel-only") {
            verify_qinggan11_view_graph(argv[1]);
            std::cout << "actual Free view/Gallery/wheel graph passed\n";

            return 0;
        }

        if (argc == 2) {
            verify_qinggan11_view_graph(argv[1]);
        }

        check(argc == 2, "profile source root required");
        auto names = embedded_profile_names();
        check(!names.empty(), "bundled catalogue cannot be empty");
        check(std::set<std::string>(names.begin(), names.end()).size() == names.size(),
              "bundled ids unique");

        for (const auto &item : embedded_profile_catalog()) {
            auto id = item.at("id").get<std::string>();
            check(valid_profile_name(id), "valid bundled id");
            check(item.at("definition").at("schema") == 1, "supported profile schema");
            std::ifstream source(std::filesystem::path(argv[1]) / id / "profile.json");
            Json original;
            source >> original;
            check(item.at("definition") == original,
                  "complete definition survives embedding without escaping changes");
            const auto fonts = original.value("fonts", Json::object());

            // The window draws with the car fonts that every machine carries.
            for (const auto *script : {"latin", "cjk"}) {
                for (const auto *weight : {"regular", "medium", "bold"}) {
                    check(fonts.size() == 2 && fonts.contains(script) &&
                                  fonts.at(script).size() == 3 &&
                                  fonts.at(script).contains(weight) &&
                                  (fonts.at(script).at(weight).contains("original") ||
                                   fonts.at(script).at(weight).contains("qnx")),
                          "every profile declares the car fonts of the window");
                }
            }

            check(item.at("sha256") ==
                          sha256_file(std::filesystem::path(argv[1]) / id / "profile.json"),
                  "embedded profile matches exact source bytes");
            check(embedded_profile(id) == item, "lookup returns complete definition");
            const auto &definition = item.at("definition");
            const auto platform = profile_platform(definition);
            const auto board = profile_board(definition);
            check(!profile_title(definition).empty(), "bundled profile has a title");
            const auto same = embedded_profile_names(platform, board);
            check(std::find(same.begin(), same.end(), id) != same.end() &&
                          embedded_profile_declares(id, platform, board) &&
                          !embedded_profile_declares(id, platform,
                                                     board == ProfileBoard::nxp
                                                             ? ProfileBoard::sa8155
                                                             : ProfileBoard::nxp),
                  "catalog selects profiles by their declared platform and board");
            check(record_declares(definition, platform, board) &&
                          !record_declares(Json{{"schema", 1}, {"id", id}}, platform, board),
                  "a record without a platform declares none, whatever profile it names");
        }

        const Json declared = {{"platform", "passion"}, {"board", "8155"}};
        check(record_declares(declared, ProfilePlatform::passion, ProfileBoard::sa8155) &&
                      !record_declares(declared, ProfilePlatform::qinggan11,
                                       ProfileBoard::sa8155) &&
                      !record_declares(declared, ProfilePlatform::passion, ProfileBoard::nxp) &&
                      !record_declares(Json::object(), ProfilePlatform::passion,
                                       ProfileBoard::sa8155),
              "the declared platform and board of a record decide");

        check(!embedded_profile_declares("synthetic-local-copy", ProfilePlatform::qinggan09,
                                         ProfileBoard::sa8155),
              "an id outside the catalog declares nothing");

        const std::string passion_id = "passion-11-russian-20260403-2.1.4.80";

        for (const auto *supported :
             {"free-09-chinese-20260106-6.0.3", "free-11-chinese-20250710-6.11.1",
              "free-11-chinese-20260731-7.3.1", "free-09-chinese-20221230-nxp-rc45",
              "passion-11-russian-20260403-2.1.4.80"}) {
            check(std::find(names.begin(), names.end(), supported) != names.end(),
                  "every supported profile is advertised");
        }

        for (const auto *free_id :
             {"free-09-chinese-20260106-6.0.3", "free-11-chinese-20250710-6.11.1",
              "free-11-chinese-20260731-7.3.1"}) {
            const auto &configure = embedded_profile(free_id).at("definition").at("configure");
            const auto &properties = configure.at("properties");
            check(!properties.contains("persist.sys.qg.currenttheme") &&
                          !properties.contains("persist.sys.qg.currentskin") &&
                          properties.at("persist.app.skin.support") == "false",
                  "fresh Free machine starts in the OEM default Classic theme");

            for (const auto &table : configure.value("settings", Json::object())) {
                check(!table.contains("ui_night_mode"),
                      "fresh Free machine keeps the OEM default night mode");
            }
        }

        std::ifstream passion_source(std::filesystem::path(argv[1]) / passion_id / "profile.json");
        Json passion;
        passion_source >> passion;
        check(passion.at("schema") == 1 && passion.at("id") == passion_id &&
                      passion.at("status") == "ready",
              "H53 identity/status required");
        check(passion.at("identity").at("verifiedModel") == "VOYAH H53" &&
                      passion.at("identity").at("device") == "monza" &&
                      passion.at("identity").at("sdk") == 30,
              "H53 identity must follow actual build.prop");
        check(!passion.contains("referenceProfile") &&
                      passion.at("wheel").at("assets") == "profiles/common/wheels/passion-2024" &&
                      !passion.at("wheel").contains("androidRoutes") &&
                      passion.at("components") ==
                              Json{{"car-availability", "components/car-availability"},
                                   {"platform-callbacks", "components/platform-callbacks"},
                                   {"window-compat", "components/window-compat"},
                                   {"mega-compat", "components/mega-compat"},
                                   {"bootstrap", "components/bootstrap"},
                                   {"wheel-input", "components/wheel-input"},
                                   {"phone", "components/phone"},
                                   {"view-observation", "components/view-observation"},
                                   {"statusbar-overlay", "components/statusbar-overlay"}} &&
                      passion.at("patches").size() == 17 && passion.at("install").size() == 36,
              "H53 finite developer graph must contain only its named OEM, display and synthetic phone adapters");
        check(passion_wheel_capability(passion).at("routes") == passion_wheel_routes(),
              "Passion standard Android wheel routes are exact");
        {
            auto bad = passion;
            bad["components"]["wheel-input"] = "components/foreign";
            bool denied = false;
            try {
                (void)passion_wheel_capability(bad);
            } catch (...) {
                denied = true;
            }

            check(denied, "Passion foreign wheel provider rejected");
        }
        {
            auto old = passion;
            old["components"].erase("wheel-input");
            check(passion_wheel_capability(old).at("supported") == false,
                  "older Passion generations report unsupported wheel");
        }
        {
            auto bad = passion;

            for (auto &node : bad["install"]) {
                if (node.value("artifact", std::string{}) == "component:wheel-input") {
                    node["target"] = "/system/foreign.apk";
                }
            }

            bool denied = false;
            try {
                (void)passion_wheel_capability(bad);
            } catch (...) {
                denied = true;
            }

            check(denied, "Passion foreign wheel install rejected");
        }
        {
            std::map<std::string, std::string> config{{"hw.camera.back", "webcam0"},
                                                      {"hw.camera.front", "webcam0"}};
            passion_camera_config(passion, config);
            check(config.at("hw.camera.back") == "emulated" &&
                          config.at("hw.camera.front") == "none",
                  "Passion must force virtual camera config");
            auto other = passion;
            other["platform"] = "qinggan11";
            config["hw.camera.back"] = "webcam0";
            passion_camera_config(other, config);
            check(config.at("hw.camera.back") == "webcam0",
                  "other profiles must retain camera config");
            Json launch = {{"reference_profile", "test-passion"},
                           {"platform", "passion"},
                           {"board", "8155"},
                           {"presentation_adapter", {{"family", "test-passion"}}},
                           {"args", Json::array()}};
            const std::string good = "hw.camera.back=emulated\nhw.camera.front=none\n";
            passion_camera_validate(launch, good);

            for (const auto &bad :
                 {std::string(""), std::string("hw.camera.back=webcam0\nhw.camera.front=none\n"),
                  good + "hw.camera.back=webcam0\n"}) {
                bool denied = false;
                try {
                    passion_camera_validate(launch, bad);
                } catch (...) {
                    denied = true;
                }

                check(denied, "unsafe Passion camera config accepted");
            }

            for (const char *bad : {"-camera-back", "-camera-front=webcam0"}) {
                launch["args"] = Json::array({bad});
                bool denied = false;
                try {
                    passion_camera_validate(launch, good);
                } catch (...) {
                    denied = true;
                }

                check(denied, "Passion camera override accepted");
            }

            launch["args"] = Json::array();
            launch.erase("presentation_adapter");
            bool denied = false;
            try {
                passion_camera_validate(launch, good);
            } catch (...) {
                denied = true;
            }

            check(denied, "Passion identity without presentation accepted");
        }
        {
            Json launch = {{"wheel_capability", passion_wheel_capability(passion)},
                           {"wheel_adapter",
                            {{"authority", "com.voyemu.input.wheel"},
                             {"routes", passion_wheel_routes()}}}};
            passion_wheel_validate(launch);
            launch["wheel_adapter"]["routes"]["ok"] = 6;
            bool denied = false;
            try {
                passion_wheel_validate(launch);
            } catch (...) {
                denied = true;
            }

            check(denied, "Passion stale foreign media route rejected");
        }
        check(passion.at("install").at(
                      0) == Json{{"artifact", "component:statusbar-overlay"},
                                 {"type", "file"},
                                 {"target",
                                  "/system/app/VoyemuPassionStatusbar/VoyemuPassionStatusbar.apk"},
                                 {"mode", "0644"}} &&
                      passion.at("install").at(1) ==
                              Json{{"artifact", "component:view-observation"},
                                   {"type", "file"},
                                   {"target", "/system/framework/framework.jar"},
                                   {"mode", "0644"}},
              "H53 stock statusbar overlay and framework observation precede its original graph");
        check(passion.at("install").at(6).at("artifact") == "files:framework-libraries",
              "H53 own library XML uses the existing files artifact namespace");
        check(passion.at("install").at(18) ==
                              Json{{"artifact", "component:phone"},
                                   {"type", "file"},
                                   {"target",
                                    "/system/priv-app/VoyemuPassionPhone/VoyemuPassionPhone.apk"},
                                   {"mode", "0644"}} &&
                      std::count_if(passion.at("install").begin(), passion.at("install").end(),
                                    [](const auto &node) {
                                        return node.at("artifact") == "component:phone";
                                    }) == 1,
              "H53 standalone synthetic Telecom phone installs exactly once at its own privileged target");
        check(passion.at("install").at(22) ==
                      Json{{"artifact", "patched:passion-api30-secondary-navigation"},
                           {"type", "file"},
                           {"target", "/system/framework/services.jar"},
                           {"mode", "0644"}},
              "Passion clean policy appends only its system JAR");
        check(passion.at("patches").at("passion-api30-statusbar") ==
                              Json{{"input", "android:/system_ext/priv-app/SystemUI/SystemUI.apk"},
                                   {"type", "apk"},
                                   {"steps", Json::array({"passion-api30-statusbar.json"})}} &&
                      !passion.at("patches").contains("MonzaSystemUI"),
              "Passion patches actual SDK stock SystemUI without substituting unused Monza");
        check(passion.at("install").at(21) ==
                      Json{{"artifact", "patched:passion-api30-statusbar"},
                           {"type", "file"},
                           {"target", "/system/system_ext/priv-app/SystemUI/SystemUI.apk"},
                           {"mode", "0644"}},
              "Stock SystemUI uses the verified system_ext guest alias");
        check(passion.at("displays").at(1).at("flag") == 0,
              "Passion secondary geometry and right OEM icons retain their original display flag");
        check(passion.at("original").at("LY_Car").at("acquisition") ==
                              Json{{"image", "vendor"},
                                   {"path", "/vendor/app/LY_Car/LY_Car.apk"}} &&
                      passion.at("install").at(23) ==
                              Json{{"artifact", "patched:LY_Car"},
                                   {"type", "file"},
                                   {"target", "/system/app/LY_Car/LY_Car.apk"},
                                   {"mode", "0644"}},
              "Passion installs its exact owner vehicle package without replacing OEM icon routing");
        const auto passion_plan = declared_patch_plan(passion);
        unsigned passion_jars = 0;

        for (const auto &node : passion_plan.at("patches")) {
            if (node.at("type") == "jar") {
                ++passion_jars;
                check(node.at("id") == "passion-api30-secondary-navigation" &&
                              node.at("dependencies") ==
                                      Json::array({"android:/system/framework/services.jar"}),
                      "Passion exact clean JAR dependency");
            }
        }

        check(passion_jars == 1, "Passion declares one clean navigation policy");
        check(passion.at("patches").at("passion-api30-secondary-navigation").at("steps") ==
                              Json::array({"passion-api30-secondary-navigation.json",
                                           "passion-api30-display-power.json",
                                           "passion-api30-startup-volume.json"}) &&
                      passion.at("patches").at("MegaNexusService").at("steps").back() ==
                              "passion-api30-nexus-power.json",
              "Power hooks bind exactly the SDK JAR and genuine Nexus package");

        for (unsigned mode = 0; mode < 3; ++mode) {
            auto bad = passion;
            auto &steps = bad["patches"]["passion-api30-secondary-navigation"]["steps"];

            if (mode == 0) {
                steps = Json::array({"passion-api30-display-power.json",
                                     "passion-api30-secondary-navigation.json"});
            }

            if (mode == 1) {
                steps.push_back("foreign.json");
            }

            if (mode == 2) {
                steps[1] = "api30-secondary-navigation.json";
            }

            bool denied = false;
            try {
                (void)declared_patch_plan(bad);
            } catch (...) {
                denied = true;
            }

            check(denied, "Power JAR rejects reversed, extra and foreign recipe chains");
        }

        for (unsigned mode = 0; mode < 4; ++mode) {
            auto bad = passion;
            auto &steps = bad["patches"]["passion-api30-secondary-navigation"]["steps"];

            if (mode == 0) {
                std::swap(steps[1], steps[2]);
            }

            if (mode == 1) {
                steps[2] = "foreign-startup-volume.json";
            }

            if (mode == 2) {
                steps.push_back("passion-api30-startup-volume.json");
            }

            if (mode == 3) {
                steps = Json::array({"passion-api30-secondary-navigation.json",
                                     "passion-api30-startup-volume.json"});
            }

            bool denied = false;
            try {
                (void)declared_patch_plan(bad);
            } catch (...) {
                denied = true;
            }

            check(denied,
                  "Passion startup-volume JAR rejects reordered, foreign, duplicate and incomplete chains");
        }

        auto legacy_power_passion = passion;
        legacy_power_passion["patches"]["passion-api30-secondary-navigation"]["steps"] =
                Json::array({"passion-api30-secondary-navigation.json",
                             "passion-api30-display-power.json"});
        check(!declared_patch_plan(legacy_power_passion).empty(),
              "Legacy exact two-step Passion power graph remains supported");
        auto legacy_passion = passion;
        legacy_passion["patches"]["passion-api30-secondary-navigation"]["steps"] =
                Json::array({"passion-api30-secondary-navigation.json"});
        check(!declared_patch_plan(legacy_passion).empty(),
              "Legacy exact single-step Passion graph remains supported");
        unsigned screen_files = 0;

        for (auto it = passion.at("original").begin(); it != passion.at("original").end(); ++it) {
            if (it.key().starts_with("screen-control-")) {
                ++screen_files;
                const auto path = it.value().at("source").at("path");
                check(it.value().at("bytes") == 6111u &&
                              it.value().at("acquisition") ==
                                      Json{{"image", "system"}, {"path", path}} &&
                              std::count_if(
                                      passion.at("install").begin(), passion.at("install").end(),
                                      [&](const auto &node) {
                                          return node == Json{{"artifact", "original:" + it.key()},
                                                              {"type", "file"},
                                                              {"target", path},
                                                              {"mode", "0644"}};
                                      }) == 1,
                      "Every genuine screen policy is pinned and installed exactly once at its OEM path");
            }
        }

        check(screen_files == 8, "All eight OEM policies are required");

        for (int mode = 0; mode < 6; ++mode) {
            auto bad = passion;

            if (mode == 0) {
                bad["platform"] = "qinggan11";
            }

            if (mode == 1) {
                bad["android"]["package"] = "system-images;android-28;default;arm64-v8a";
            }

            if (mode == 2) {
                bad["patches"]["passion-api30-secondary-navigation"]["input"] = "original:services";
            }

            if (mode == 3) {
                bad["patches"]["passion-api30-secondary-navigation"]["steps"] =
                        Json::array({"api30-secondary-navigation.json"});
            }

            if (mode == 4) {
                bad["patches"]["foreign-jar"] =
                        bad["patches"]["passion-api30-secondary-navigation"];
            }

            if (mode == 5) {
                bad["patches"]["passion-api30-secondary-navigation"]["uses"] = Json::array();
            }

            bool denied = false;
            try {
                (void)declared_patch_plan(bad);
            } catch (...) {
                denied = true;
            }

            check(denied, "Passion foreign JAR routes fail closed");
        }

        check(passion.at("configure").at("settings").at("global").at("device_provisioned") == "1" &&
                      passion.at("configure").at("settings").at("global").at("policy_control") ==
                              "immersive.navigation=*" &&
                      passion.at("configure")
                                      .at("settings")
                                      .at("global")
                                      .at("heads_up_notifications_enabled") == "0",
              "Passion retains persisted global policies alongside the pinned stock SystemUI window guard");
        check(passion.at("configure").at("disabled") == Json::array({"com.android.provision"}),
              "H53 must disable only the actual clean API30 competing Provision HOME");
        check(passion.at("compatibility").at("privapp_whitelist").at("packages").size() == 6 &&
                      passion.at("compatibility")
                                      .at("privapp_whitelist")
                                      .at("packages")
                                      .at("com.voyah.btphone") ==
                              Json::array({"android.permission.CONTROL_INCALL_EXPERIENCE"}),
              "H53 finite privileged permission source must be pinned without claiming guest acceptance");
        const auto phone_grants = Json::array({"android.permission.MODIFY_PHONE_STATE"});
        check(passion.at("compatibility")
                              .at("privapp_whitelist")
                              .at("packages")
                              .at("com.voyemu.passion.phone") == phone_grants,
              "Passion synthetic Telecom has only its approved MODIFY_PHONE_STATE privileged grant");

        for (const auto *role : {"LY_Video", "LY_Camera"}) {
            check(passion.at("patches").at(role).at("steps") ==
                          (std::string(role) == "LY_Video"
                                   ? Json::array({"passion-api30-media-libraries.json",
                                                  "passion-api30-service-foreground.json",
                                                  "passion-api30-bluetooth-volume.json"})
                                   : Json::array({"passion-api30-media-libraries.json",
                                                  "passion-api30-service-foreground.json",
                                                  "passion-api30-camera-virtual.json"})),
                  "Only pinned Video/Camera service foreground channels add the quiet operation");
        }

        std::size_t service_quiet_nodes = 0;

        for (const auto &node : passion.at("patches")) {
            for (const auto &step : node.at("steps")) {
                if (step == "passion-api30-service-foreground.json") {
                    ++service_quiet_nodes;
                }
            }
        }

        check(service_quiet_nodes == 2,
              "Service quiet operation must not spread to other OEM apps");
        std::size_t ear_nodes = 0;

        for (auto it = passion.at("patches").begin(); it != passion.at("patches").end(); ++it) {
            for (const auto &step : it.value().at("steps")) {
                if (step == "passion-api30-bluetooth-volume.json") {
                    ++ear_nodes;
                    check(it.key() == "LY_Launcher" || it.key() == "LY_Car" ||
                                  it.key() == "LY_Video",
                          "Ear stream guard must remain scoped to three pinned apps");
                    check(it.value().at("steps").back() == step ||
                                  ((it.key() == "LY_Car" &&
                                    it.value().at("steps").at(it.value().at("steps").size() - 4) ==
                                            step &&
                                    it.value().at("steps").at(it.value().at("steps").size() - 3) ==
                                            "passion-api30-ambient-light.json" &&
                                    it.value().at("steps").at(it.value().at("steps").size() - 2) ==
                                            "passion-api30-ecu-hardware.json" &&
                                    it.value().at("steps").back() ==
                                            "passion-api30-vehicle-animation.json") ||
                                   (it.key() == "LY_Launcher" &&
                                    it.value().at("steps").at(it.value().at("steps").size() - 4) ==
                                            step &&
                                    it.value().at("steps").at(it.value().at("steps").size() - 3) ==
                                            "passion-api30-launcher-audio.json" &&
                                    it.value().at("steps").at(it.value().at("steps").size() - 2) ==
                                            "passion-api30-navigation-ready.json" &&
                                    it.value().at("steps").back() ==
                                            "passion-api30-status-ready.json")),
                          "Ear guard consumes prior transforms before optional Launcher audio");
                }
            }
        }

        check(ear_nodes == 3, "Three exact ear-volume input chains required");
        std::size_t ambient_nodes = 0;

        for (auto it = passion.at("patches").begin(); it != passion.at("patches").end(); ++it) {
            for (const auto &step : it.value().at("steps")) {
                if (step == "passion-api30-ambient-light.json") {
                    ++ambient_nodes;
                    check(it.key() == "LY_Car" &&
                                  it.value().at("steps").at(it.value().at("steps").size() - 3) ==
                                          step &&
                                  it.value().at("steps").at(it.value().at("steps").size() - 2) ==
                                          "passion-api30-ecu-hardware.json" &&
                                  it.value().at("steps").back() ==
                                          "passion-api30-vehicle-animation.json",
                          "Ambient guard consumes exact Car prior chains only");
                }
            }
        }

        check(ambient_nodes == 1, "Only pinned Car gets unavailable ambient service guard");
        check(passion.at("patches").at("LY_Launcher").at("steps") ==
                      Json::array({"framework-libraries.json", "passion-api30-sidebars.json",
                                   "passion-api30-bluetooth-volume.json",
                                   "passion-api30-launcher-audio.json",
                                   "passion-api30-navigation-ready.json",
                                   "passion-api30-status-ready.json"}),
              "Exact Launcher status readiness chain preserves all predecessors");
        std::size_t launcher_audio_nodes = 0;

        for (auto it = passion.at("patches").begin(); it != passion.at("patches").end(); ++it) {
            for (const auto &step : it.value().at("steps")) {
                if (step == "passion-api30-launcher-audio.json") {
                    ++launcher_audio_nodes;
                    check(it.key() == "LY_Launcher" &&
                                  it.value().at("steps").at(it.value().at("steps").size() - 3) ==
                                          step &&
                                  it.value().at("steps").at(it.value().at("steps").size() - 2) ==
                                          "passion-api30-navigation-ready.json" &&
                                  it.value().at("steps").back() ==
                                          "passion-api30-status-ready.json",
                          "Optional Launcher audio guard consumes its exact ear-prepared DEX only");
                }
            }
        }

        check(launcher_audio_nodes == 1,
              "Only pinned Launcher gets the nullable automotive UI operation");
        const auto car_grants = Json::array(
                {"android.permission.BACKUP", "android.permission.BLUETOOTH_PRIVILEGED",
                 "android.permission.DELETE_CACHE_FILES", "android.permission.DUMP",
                 "android.permission.FORCE_STOP_PACKAGES",
                 "android.permission.GET_ACCOUNTS_PRIVILEGED", "android.permission.MANAGE_USERS",
                 "android.permission.MASTER_CLEAR", "android.permission.MOUNT_UNMOUNT_FILESYSTEMS",
                 "android.permission.OVERRIDE_WIFI_CONFIG", "android.permission.REBOOT",
                 "android.permission.WRITE_SECURE_SETTINGS"});
        check(passion.at("compatibility")
                              .at("privapp_whitelist")
                              .at("packages")
                              .at("com.android.car.settings") == car_grants,
              "CarSettings grants are only the exact12 observed manifest-requested API30 requirements");
        const auto permission_file = std::filesystem::path(argv[1]) / passion_id /
                                     "files/voyemu-framework.xml";
        std::ifstream permission_source(permission_file);
        const std::string permission_xml((std::istreambuf_iterator<char>(permission_source)), {});
        const auto car_begin =
                permission_xml.find("<privapp-permissions package=\"com.android.car.settings\">");
        check(car_begin != std::string::npos, "CarSettings privileged package declaration missing");
        const auto car_end = permission_xml.find("</privapp-permissions>", car_begin);
        check(car_end != std::string::npos, "CarSettings privileged package declaration unclosed");
        const auto car_block = permission_xml.substr(car_begin, car_end - car_begin);
        std::size_t entries = 0, pos = 0;

        while ((pos = car_block.find("<permission name=", pos)) != std::string::npos) {
            ++entries;
            ++pos;
        }

        check(entries == car_grants.size(), "No additional CarSettings privileged grant permitted");

        for (const auto &permission : car_grants) {
            check(car_block.find("name=\"" + permission.get<std::string>() + "\"") !=
                          std::string::npos,
                  "CarSettings XML/metadata grant differs");
        }

        const std::string phone_tag = "<privapp-permissions package=\"com.voyemu.passion.phone\">";
        const auto phone_begin = permission_xml.find(phone_tag);
        check(phone_begin != std::string::npos &&
                      permission_xml.find(phone_tag, phone_begin + phone_tag.size()) ==
                              std::string::npos,
              "Passion synthetic phone has exactly one scoped privileged XML declaration");
        const auto phone_end = permission_xml.find("</privapp-permissions>", phone_begin);
        check(phone_end != std::string::npos,
              "Passion synthetic phone privileged XML declaration unclosed");
        const auto phone_block = permission_xml.substr(phone_begin, phone_end - phone_begin);
        std::size_t phone_entries = 0, phone_pos = 0;

        while ((phone_pos = phone_block.find("<permission ", phone_pos)) != std::string::npos) {
            ++phone_entries;
            ++phone_pos;
        }

        check(phone_entries == phone_grants.size(),
              "No additional Passion synthetic phone privileged grant permitted");
        std::ifstream phone_manifest_source(std::filesystem::path(argv[1]) / passion_id /
                                            "components/phone/AndroidManifest.xml");
        const std::string phone_manifest((std::istreambuf_iterator<char>(phone_manifest_source)),
                                         {});
        check(phone_manifest.find("package=\"com.voyemu.passion.phone\"") != std::string::npos,
              "Passion synthetic phone manifest package differs");

        for (const auto &permission : phone_grants) {
            const auto name = permission.get<std::string>();
            check(phone_block.find("<permission name=\"" + name + "\" />") != std::string::npos &&
                          phone_manifest.find("<uses-permission android:name=\"" + name +
                                              "\" />") != std::string::npos,
                  "Passion synthetic phone XML, metadata and requested permission differ");
        }

        check(passion.at("compatibility").at("developer_trial").at("partial") == true &&
                      passion.at("compatibility").at("developer_trial").at("canonical_ready") ==
                              false &&
                      passion.at("displays").size() == 3 &&
                      passion.at("displays").at(0).at("width") == 1920 &&
                      passion.at("displays").at(2) == Json{{"role", "hud"},
                                                           {"guest", "qnx"},
                                                           {"width", 1280},
                                                           {"height", 720}},
              "H53 test geometry must remain explicitly partial and unverified");
        check(passion.at("ownerUpdate").at("sha256") ==
                              "a1b651223453ed23d1835a70a5cac64c43247a21165ccd88d56a21cc3215ddb9" &&
                      passion.at("acquisition").at("system").at("sha256") ==
                              "218b99296c6963e0c0644dd76d444ea953fcc25b9d187bb65c281e3c9eedff80" &&
                      passion.at("acquisition").at("system").at("segments").size() == 18 &&
                      passion.at("acquisition").at("implemented").get<bool>(),
              "H53 native acquisition retains exact source pins without claiming guest readiness");
        std::ifstream passion_capture_source(std::filesystem::path(argv[1]) / passion_id /
                                             "capture.json");
        Json passion_capture;
        passion_capture_source >> passion_capture;
        check(passion.at("original").size() == 26 && passion_capture.at("profile") == passion_id &&
                      passion_capture.at("android").size() == 26,
              "H53 capture covers twelve APKs, five framework contracts, eight genuine screen-control files and the boot animation");

        for (const auto &role : std::vector<std::array<std::string, 4>>{
                     {"LY_Multimedia", "com.voyah.multimedia",
                      "a3c788860d5df27277d53ffbf838daaefddb6ab098ad52b636671dd70b34765d",
                      "/system/app/LY_Multimedia/LY_Multimedia.apk"},
                     {"LY_BtPhone", "com.voyah.btphone",
                      "5bc0f97b94009b2e5aa6cb9759ea2547ab17cdadfbb8021d5e9cb6d6c6589047",
                      "/system/priv-app/LY_BtPhone/LY_BtPhone.apk"}}) {
            const auto &original = passion.at("original").at(role[0]);
            check(original.at("source").at("package") == role[1] &&
                          original.at("sha256") == role[2] &&
                          original.at("acquisition").at("path") == role[3] &&
                          original.at("acquisition").at("image") == "system",
                  "H53 Music/Phone bind actual owner packages and image paths");
            check(passion.at("patches").at(role[0]).at("steps") ==
                          (role[0] == "LY_Multimedia"
                                   ? Json::array({"passion-api30-bluetooth-role.json",
                                                  "passion-api30-music-foreground.json",
                                                  "passion-api30-music-day-drawables.json",
                                                  "passion-api30-music-insets.json",
                                                  "passion-api30-usb-media-session.json"})
                                   : Json::array({"passion-api30-bluetooth-role.json",
                                                  "passion-api30-phone.json"})),
                  "H53 Music/Phone retain exact ordered API30 role, media and OEM phone injection recipes");
            check(std::any_of(passion.at("install").begin(), passion.at("install").end(),
                              [&](const auto &node) {
                                  return node.at("artifact") == "patched:" + role[0] &&
                                         node.at("target") == role[3];
                              }),
                  "H53 genuine Music/Phone target retains owner app/priv-app placement");
        }

        for (const auto &item : passion_capture.at("android")) {
            const auto &original = passion.at("original").at(item.at("name").get<std::string>());
            check(item.at("file") == original.at("file") &&
                          item.at("source") == original.at("source") &&
                          original.at("sha256").get<std::string>().size() == 64,
                  "H53 capture pins and actual sources agree");
        }

        std::ifstream passion_qnx_source(std::filesystem::path(argv[1]) / passion_id /
                                         "profile-qnx.json");
        Json passion_qnx;
        passion_qnx_source >> passion_qnx;
        check(passion_qnx.at("status") == "incomplete" &&
                      passion_qnx.at("hardware_adapter") == "sa8155-passion-21480",
              "H53 QNX publishes its own adapter and stays incomplete");
        const std::string qinggan11_id = "free-11-chinese-20260731-7.3.1";
        check(std::find(names.begin(), names.end(), qinggan11_id) != names.end(),
              "accepted Free A11 7.3.1 profile must be included in the runnable catalogue");
        std::ifstream qinggan11_source(std::filesystem::path(argv[1]) / qinggan11_id /
                                       "profile.json");
        Json a11;
        qinggan11_source >> a11;
        check(a11.at("schema") == 1 && a11.at("id") == qinggan11_id && a11.at("status") == "ready",
              "accepted A11 source profile identity/status required");
        check(a11.at("displays") == Json::array({{{"role", "primary"},
                                                  {"guest", "android"},
                                                  {"width", 1920},
                                                  {"height", 720},
                                                  {"density", 160}},
                                                 {{"role", "secondary"},
                                                  {"guest", "android"},
                                                  {"width", 1920},
                                                  {"height", 720},
                                                  {"density", 160},
                                                  {"xOffset", 1920},
                                                  {"yOffset", 0},
                                                  {"flag", 1225}},
                                                 {{"role", "rear"},
                                                  {"guest", "android"},
                                                  {"width", 1902},
                                                  {"height", 540},
                                                  {"density", 160},
                                                  {"xOffset", 0},
                                                  {"yOffset", 720},
                                                  {"flag", 0}}}),
              "Free A11 7.3.1 secondary must retain public/touch/trusted flags without forced stock system decorations");

        check(a11.at("android").at("package") == "system-images;android-30;default;arm64-v8a" &&
                      a11.at("android").at("revision") == "1",
              "A11 clean base identity must match actual archive source.properties revision1");
        std::ifstream qinggan11_capture_source(std::filesystem::path(argv[1]) / qinggan11_id /
                                               "capture.json");
        Json qinggan11_capture;
        qinggan11_capture_source >> qinggan11_capture;
        check(qinggan11_capture.at("profile") == qinggan11_id &&
                      qinggan11_capture.at("android").size() == 34 &&
                      a11.at("original").size() == 35,
              "A11 capture retains 34 roles plus the separately acquired Phone original");

        for (const auto &capture : qinggan11_capture.at("android")) {
            const auto &original = a11.at("original").at(capture.at("name").get<std::string>());
            auto pins = original.at("sha256");

            if (pins.is_string()) {
                pins = Json::array({pins});
            } else {
                check(original.value("type", std::string{}) == "tree" && pins.size() == 23,
                      "A11 tree requires exact acquired inventory");
            }

            for (const auto &value : pins) {
                const auto hash = value.get<std::string>();
                check(hash.size() == 64 &&
                              hash.find_first_not_of("0123456789abcdef") == std::string::npos &&
                              hash != std::string(64, '0'),
                      "A11 originals require actual nonplaceholder SHA256");
            }

            check(capture.at("file") == original.at("file") &&
                          capture.at("source") == original.at("source") &&
                          original.at("source").size() == 1,
                  "A11 capture/pin device source must agree");
        }

        check(a11.at("original").at("AudioPolicy") ==
                      Json{{"file", "AudioPolicy-release.apk"},
                           {"sha256",
                            "2152a99acdbee41626c6b661fe6509dd2c9487d3cad6a66aad6320bbb00cc039"},
                           {"source",
                            {{"path",
                              "/system/priv-app/AudioPolicy-release/AudioPolicy-release.apk"}}},
                           {"bytes", 1754811u},
                           {"acquisition",
                            {{"image", "system"},
                             {"path",
                              "/system/priv-app/AudioPolicy-release/AudioPolicy-release.apk"}}}},
              "AudioPolicy must bind its exact acquired privileged APK");
        {
            const auto plan = declared_patch_plan(a11);
            unsigned audio_jars = 0;

            for (const auto &node : plan.at("patches")) {
                if (node.at("id") == "QGAPI") {
                    ++audio_jars;
                    check(node.at("type") == "jar" &&
                                  node.at("dependencies") == Json::array({"original:QGAPI"}),
                          "audio consumer JAR retains exact acquired owner dependency");
                }
            }

            check(audio_jars == 1, "one acquired QGAPI audio consumer JAR required");

            for (int mode = 0; mode < 8; ++mode) {
                auto bad = a11;

                if (mode == 0) {
                    bad["platform"] = "passion";
                }

                if (mode == 1) {
                    bad["android"]["package"] = "system-images;android-28;default;arm64-v8a";
                }

                if (mode == 2) {
                    bad["original"]["QGAPI"]["sha256"] = "unpinned";
                }

                if (mode == 3) {
                    bad["patches"]["QGAPI"]["steps"] = Json::array({"framework-libraries.json"});
                }

                if (mode == 4) {
                    bad["patches"]["QGAPI"]["input"] = "original:QGMOD";
                }

                if (mode == 5) {
                    bad["original"]["QGAPI"].erase("acquisition");
                }

                if (mode == 6) {
                    bad["original"]["QGAPI"]["acquisition"]["path"] = "/vendor/framework/QGAPI.jar";
                }

                if (mode == 7) {
                    bad["patches"]["QGAPI"]["uses"] = Json::array({"original:QGMOD"});
                }

                bool denied = false;
                try {
                    (void)declared_patch_plan(bad);
                } catch (...) {
                    denied = true;
                }

                if (!denied) {
                    throw std::runtime_error(
                            "foreign QGAPI JAR shape, input, owner or API rejected: " +
                            std::to_string(mode));
                }
            }
        }

        for (unsigned mode = 0; mode < 4; ++mode) {
            auto bad = a11;
            auto &donor = bad["original"]["QGAPI"];

            // The bytes, pin and path come from the profile; only an unpinned entry fails.
            if (mode == 0) {
                donor["bytes"] = 1968299.0;
            }

            if (mode == 1) {
                donor["acquisition"]["image"] = "vendor";
            }

            if (mode == 2) {
                donor.erase("acquisition");
            }

            if (mode == 3) {
                donor["sha256"] = "unpinned";
            }

            bool denied = false;
            try {
                (void)declared_patch_plan(bad);
            } catch (...) {
                denied = true;
            }

            check(denied, "original Navi QGAPI requires a profile-pinned system original");
        }

        for (const auto *artifact :
             {"patched:original-navi-framework", "patched:original-navi-services", "patched:QGAPI",
              "patched:original-navi-vehicle"}) {
            for (unsigned mode = 0; mode < 3; ++mode) {
                auto bad = a11;

                for (auto &node : bad["install"]) {
                    if (node.at("artifact") == artifact) {
                        if (mode == 0) {
                            node["target"] = "/system/framework/foreign.jar";
                        }

                        if (mode == 1) {
                            node["mode"] = "0755";
                        }

                        if (mode == 2) {
                            node["type"] = "apk";
                        }
                    }
                }

                bool denied = false;
                try {
                    (void)declared_patch_plan(bad);
                } catch (...) {
                    denied = true;
                }

                check(denied,
                      "every original Navi JAR retains its exact system target, mode and type");
            }
        }

        check(!a11.at("patches").contains("Navi") &&
                      std::count(a11.at("install").begin(), a11.at("install").end(),
                                 Json{{"artifact", "original:Navi"},
                                      {"type", "apk"},
                                      {"target", "/system/priv-app/dueros/Dueros-h97c-signed.apk"},
                                      {"package", "com.baidu.naviauto"}}) == 1,
              "original Navi is installed once without a resigning patch");
        check(a11.at("original").at("Hiboard") ==
                      Json{{"file", "Hiboard.apk"},
                           {"sha256",
                            "902562587355d9e48d3878773da7955cd5a793c03e5ba6e5b595a317c7ec0f39"},
                           {"source",
                            {{"path",
                              "/vendor/app/Hiboard-release-signed/Hiboard-release-signed.apk"}}},
                           {"bytes", 26132593u},
                           {"acquisition",
                            {{"image", "vendor"},
                             {"path",
                              "/vendor/app/Hiboard-release-signed/Hiboard-release-signed.apk"}}}},
              "Hiboard must bind its exact acquired vendor APK");
        check(std::count_if(qinggan11_capture.at("android").begin(),
                            qinggan11_capture.at("android").end(),
                            [](const auto &entry) {
                                return entry.at("name") == "Hiboard";
                            }) == 1,
              "Hiboard capture occurs exactly once");
        check(std::count(
                      a11.at("install").begin(), a11.at("install").end(),
                      Json{{"artifact", "patched:Hiboard"},
                           {"type", "apk"},
                           {"target",
                            "/system/priv-app/Hiboard-release-signed/Hiboard-release-signed.apk"},
                           {"package", "com.qinggan.app.hiboard"}}) == 1,
              "genuine Hiboard retains its exact privileged package target");

        for (const auto &install : a11.at("install")) {
            const auto reference = install.at("artifact").get<std::string>();
            check((reference.starts_with("original:") &&
                   a11.at("original").contains(reference.substr(9))) ||
                          (reference.starts_with("patched:") &&
                           a11.at("patches").contains(reference.substr(8))) ||
                          reference == "files:qinggan-framework" ||
                          reference == "component:canbus" || reference == "component:qg-runtime" ||
                          reference == "component:bootstrap" ||
                          reference == "component:wheel-input" ||
                          reference == "component:wheel-platform" ||
                          reference == "component:oem-power" ||
                          reference == "component:view-observation" ||
                          reference == "component:bluetooth" ||
                          reference == "component:can-simulator" ||
                          reference == "component:early-inputs" ||
                          reference == "component:can-model-observer" ||
                          reference == "files:secondary-display-feature" ||
                          reference == "files:launcher-privapp" || reference == "files:nvram" ||
                          reference == "files:can-simulator-fixture" ||
                          reference == "files:can-simulator-init" ||
                          reference == "files:hybrid-nvram" ||
                          reference == "files:qg-settings-guest" ||
                          reference == "files:early-inputs-init" ||
                          install.at("type") == "directory" && reference == "files:vendor-overlay",
                  "A11 graph must reference its own acquired inputs and manifest/Binder/JNI/display adapters");
        }

        bool nvram_seed = false;

        for (const auto &item : a11.at("install")) {
            if (item.at("artifact") == "files:nvram") {
                check(item.at("target") == "/system/etc/nvram" && item.at("mode") == "0644" &&
                              !item.contains("uid") && !item.value("mutable", false),
                      "A11 own seed must be a read-only system file");
                nvram_seed = true;
            }
        }

        check(nvram_seed, "A11 own NVRAM seed missing");
        const auto active_fixture = std::filesystem::path(argv[1]) / qinggan11_id /
                                    "files/nvram-active.txt";
        check(a11.at("files").at("nvram").at("file") == "nvram-active.txt",
              "Free simulation must use its own active fixture");
        std::ifstream active_source(active_fixture);
        std::string active_row;
        unsigned active_rows = 0;

        while (std::getline(active_source, active_row)) {
            if (active_row.starts_with("8=")) {
                ++active_rows;
                check(active_row == "8=true", "own fixture must expose raw string true at NVRAM8");
            }
        }

        check(active_rows == 1, "own activation fixture requires exactly one NVRAM8 row");
        check(a11.at("configure").at("bootProperties") ==
                      Json({{"ro.build.product", "msmnile_gvmq"},
                            {"ro.build.product_3rdparty", "f6802h"},
                            {"ro.qg.presentation", "VirtualDisplay"}}),
              "A11 boot identity uses only its exact owner-system build properties");
        check(a11.at("configure").at("start") ==
                      Json::array(
                              {Json({{"component", "com.voyemu.oem/.RegistrationService"},
                                     {"mode", "service"}}),
                               Json({{"component", "com.qinggan.canbus.service/.CanBusService"},
                                     {"mode", "service"}}),
                               Json({{"component",
                                      "com.qinggan.systemservice/.displaymanager.DisplayManagerService"},
                                     {"mode", "service"}}),
                               Json({{"component",
                                      "com.qinggan.keymanager.service/.KeyManagerService"},
                                     {"mode", "service"}}),
                               Json({{"component",
                                      "com.qinggan.app.setting/.service.MainSettingService"},
                                     {"mode", "service"}}),
                               Json({{"component",
                                      "com.voyemu/com.voyemu.output.SecondScreenService"},
                                     {"mode", "service"}}),
                               Json({{"component", "com.qinggan.systempolicy/.SystemPolicyService"},
                                     {"mode", "service"}})}),
              "A11 initial startup uses exact ordinary service dependencies");
        check(a11.at("verify").at("services") ==
                      Json::array({"qg.canbus", "qg.cluster.service", "qg.setting",
                                   "qg.display_manager", "multidisplay", "multiwindow",
                                   "qg.key_manager", "qg.power_manager", "qg.carsignal",
                                   "qg.system_policy", "qg.qgbus_service"}),
              "A11 cold acceptance requires every source-published cockpit Binder service");
        check(a11.at("verify").at("activities") ==
                      Json::array(
                              {Json({{"component",
                                      "com.qinggan.app.launcher/com.qinggan.app.launcher.activity.MainActivity"},
                                     {"display", 0}}),
                               Json({{"component",
                                      "com.qinggan.app.launcher/com.qinggan.secondlauncher.activity.SecondMainActivity"},
                                     {"display", 1}})}),
              "A11 cold acceptance requires both exact OEM HOME activities");
        check(a11.at("patches").size() == 33 && a11.at("components").size() == 12 &&
                      a11.at("components").at("view-observation") ==
                              "components/view-observation" &&
                      a11.at("components").at("oem-power") == "components/oem-power" &&
                      a11.at("components").at("canbus") == "components/canbus" &&
                      a11.at("components").at("qg-runtime") == "components/qg-runtime",
              "A11 must contain acquired APK adapters and exact native policy graph");
        check(a11.at("original").at("KeyManagerService") ==
                              Json{{"file", "KeyManagerService.apk"},
                                   {"sha256",
                                    "951abb4f86540663fd654138b7e58cd0226d5dfa0470a4f5e85384bcdedbb78e"},
                                   {"source",
                                    {{"path",
                                      "/system/priv-app/KeyManagerService-release/KeyManagerService-release.apk"}}},
                                   {"bytes", 4898541u},
                                   {"acquisition",
                                    {{"image", "system"},
                                     {"path",
                                      "/system/priv-app/KeyManagerService-release/KeyManagerService-release.apk"}}}} &&
                      a11.at("original").size() == 35,
              "Free wheel must bind actual acquired OEM KeyManager");
        check(a11.at("components").at("wheel-input") == "components/wheel-input" &&
                      a11.at("components").at("wheel-platform") == "components/wheel-platform" &&
                      a11.at("install").size() == 54 &&
                      std::count(a11.at("install").begin(), a11.at("install").end(),
                                 Json{{"artifact", "patched:original-navi-framework"},
                                      {"type", "file"},
                                      {"target", "/system/framework/framework.jar"},
                                      {"mode", "0644"}}) == 1 &&
                      !a11.at("components").contains("wheel"),
              "Only selected thin OEM wheel route may be installed; backup broker inactive");
        check(a11.at("original").at("QGMedia") ==
                              Json{{"file", "QGMedia.apk"},
                                   {"sha256",
                                    "533a0b0dc5ccbe91a42847fac20c5735bd8d78a28bbb980a6bf365dd5ed0fba0"},
                                   {"source", {{"package", "com.qinggan.media"}}},
                                   {"bytes", 67016156u},
                                   {"acquisition",
                                    {{"image", "system"},
                                     {"path",
                                      "/system/priv-app/qgmedia-release-signed/qgmedia-release-signed.apk"}}}} &&
                      a11.at("configure").at("properties").at("persist.app.kuwo.support") ==
                              "false",
              "Free Music must bind genuine provider and disable unsupported ARM32 Kuwo before its first boot");
        check(a11.at("original").at("Gallery").at("sha256") ==
                              "c30633d39bb4127ac7fcd79300f00f2be608ff6b59cb24ebf855471e967be537" &&
                      a11.at("patches").at("Gallery") ==
                              Json{{"type", "apk"},
                                   {"input", "original:Gallery"},
                                   {"uses", Json::array({"component:gallery-arm64-libraries"})},
                                   {"steps", Json::array({"api30-gallery-arm64.json"})}} &&
                      a11.at("components").at("gallery-arm64-libraries") ==
                              "components/gallery-arm64-libraries",
              "Gallery uses only its exact OEM original and explicit official ARM64 archive component");
        check(std::count(a11.at("install").begin(), a11.at("install").end(),
                         Json{{"artifact", "patched:Gallery"}, {"type", "apk"}}) == 1 &&
                      std::none_of(a11.at("install").begin(), a11.at("install").end(),
                                   [](const auto &item) {
                                       return item.at("artifact") == "original:Gallery";
                                   }),
              "Gallery installs its prepared APK exactly once; no direct ARM32 original installation");
        std::ifstream gallery_source(std::filesystem::path(argv[1]) / qinggan11_id /
                                     "components/gallery-arm64-libraries/component.json");
        Json gallery;
        gallery_source >> gallery;
        check(gallery.at("type") == "native-library-archive" && gallery.at("minSdk") == 30 &&
                      gallery.at("output") == "gallery-arm64.zip" &&
                      gallery.at("libraries").size() == 10 &&
                      gallery.at("provenance").at("distributionLegalClosure") == false,
              "Gallery bounded official native archive preserves explicit unresolved distribution provenance");
        check(gallery.at("externalSources").at("ijk.aar") ==
                      Json::parse(
                              R"json({"kind":"archive","format":"aar","url":"https://repo.maven.apache.org/maven2/xyz/doikki/android/dkplayer/player-ijk/3.3.7/player-ijk-3.3.7.aar","sha256":"36306493fb1037e76a4fd36c088ad4ee1872465b8bfd1d05ad297974a945b411","bytes":14753321})json"),
              "Gallery official AAR descriptor URL/SHA/bytes is exact");
        check(gallery.at("externalSources").at("fmmr.aar") ==
                      Json::parse(
                              R"json({"kind":"archive","format":"aar","url":"https://repo.maven.apache.org/maven2/com/github/wseemann/FFmpegMediaMetadataRetriever-native-arm64-v8a/1.0.16/FFmpegMediaMetadataRetriever-native-arm64-v8a-1.0.16.aar","sha256":"dcbda007c35b496b3aa243bbdc27113feb8a9e4e2949927f61d39abe1e19e3fd","bytes":5761118})json"),
              "Gallery official AAR descriptor URL/SHA/bytes is exact");
        check(gallery.at("libraries") ==
                      Json::parse(
                              R"json({"lib/arm64-v8a/libijkffmpeg.so":{"archive":"ijk.aar","entry":"jni/arm64-v8a/libijkffmpeg.so","sha256":"4a8f36dee25d0d84094a595aec9ac0d0322c78be74748f30501ea705b24d5019","bytes":5619008},"lib/arm64-v8a/libijkplayer.so":{"archive":"ijk.aar","entry":"jni/arm64-v8a/libijkplayer.so","sha256":"9e29ce5dbaf7b2b888c77027a337b6c6cd66f382b3650d827292b3eff0c1534c","bytes":418392},"lib/arm64-v8a/libijksdl.so":{"archive":"ijk.aar","entry":"jni/arm64-v8a/libijksdl.so","sha256":"94268f8db4e08712d7060ce84cf48d52dc35d4dccceccd7cbac964c0ae7dda83","bytes":481352},"lib/arm64-v8a/libavcodec.so":{"archive":"fmmr.aar","entry":"jni/arm64-v8a/libavcodec.so","sha256":"96e6d55842b6c6ec0d29e80cbbb577e2b1dedac5dad1e5694d38b16e7feeffc9","bytes":6513680},"lib/arm64-v8a/libavformat.so":{"archive":"fmmr.aar","entry":"jni/arm64-v8a/libavformat.so","sha256":"fac2cf935f5e7bf1d5658a24aa871f9300de2a832cbbc9ceb11b9f8f261bac9a","bytes":1187520},"lib/arm64-v8a/libavutil.so":{"archive":"fmmr.aar","entry":"jni/arm64-v8a/libavutil.so","sha256":"f1c054f13d895b534c18d9f27bc24af2e3d5d6204bc8f5c2621154a7af2fb44c","bytes":285016},"lib/arm64-v8a/libcrypto.so":{"archive":"fmmr.aar","entry":"jni/arm64-v8a/libcrypto.so","sha256":"51d754da3e6e78271875b9704093c7de6f94aa03696b007cc8c35dfdeea766ea","bytes":2363056},"lib/arm64-v8a/libffmpeg_mediametadataretriever_jni.so":{"archive":"fmmr.aar","entry":"jni/arm64-v8a/libffmpeg_mediametadataretriever_jni.so","sha256":"5d5cfe6e79672c1d41e6928b7f97f8fc8fd738cf8bae4fa9c684189511ed6b43","bytes":51536},"lib/arm64-v8a/libssl.so":{"archive":"fmmr.aar","entry":"jni/arm64-v8a/libssl.so","sha256":"81eadaf08032fa12c3bb20480493f88046c504f88b684bae2e53e201db3c0c0c","bytes":503408},"lib/arm64-v8a/libswscale.so":{"archive":"fmmr.aar","entry":"jni/arm64-v8a/libswscale.so","sha256":"af96bf88dd3c08019bc7420d85aca1e71224a1e86990df2268b145c589051fdf","bytes":264024}})json"),
              "Gallery adds exactly ten pinned official ARM64 ELF entries");
        check(qinggan11_wheel_capability(a11) == Json{{"schema", 1},
                                                      {"profile", "free-11-chinese-20260731-7.3.1"},
                                                      {"status", "partial"},
                                                      {"supported", true},
                                                      {"authority", "com.voyemu.input.wheel"},
                                                      {"routes",
                                                       {{"volume_up", 2},
                                                        {"volume_down", 1},
                                                        {"next", 3},
                                                        {"previous", 4},
                                                        {"ok", 6},
                                                        {"photo", 173},
                                                        {"voice", 130},
                                                        {"phone", 128}}}},
              "exact authenticated Free ingress exposes only verified OEM handler routes");
        {
            auto legacy = a11;
            legacy["components"].erase("oem-power");
            auto &nodes = legacy["install"];
            nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                                       [](const auto &n) {
                                           return n.at("artifact") == "component:oem-power";
                                       }),
                        nodes.end());
            legacy["configure"]["start"].erase(legacy["configure"]["start"].begin());
            check(qinggan11_wheel_capability(legacy) ==
                          qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1"),
                  "historical portable Free without OEM power retains exact wheel routes");

            for (int mode = 0; mode < 8; ++mode) {
                auto bad = a11;

                if (mode == 0) {
                    bad["components"]["oem-power"] = "components/foreign";
                }

                if (mode == 1) {
                    bad["original"]["QGAPI"]["sha256"] = "unpinned";
                }

                if (mode == 2) {
                    bad["install"].erase(
                            std::remove_if(bad["install"].begin(), bad["install"].end(),
                                           [](const auto &n) {
                                               return n.at("artifact") == "component:oem-power";
                                           }),
                            bad["install"].end());
                }

                if (mode == 3) {
                    for (const auto &n : a11.at("install")) {
                        if (n.at("artifact") == "component:oem-power") {
                            bad["install"].push_back(n);
                        }
                    }
                }

                if (mode >= 4 && mode <= 6) {
                    for (auto &n : bad["install"]) {
                        if (n.at("artifact") == "component:oem-power") {
                            if (mode == 4) {
                                n["type"] = "file";
                            }

                            if (mode == 5) {
                                n["target"] = "/system/framework/foreign.apk";
                            }

                            if (mode == 6) {
                                n["package"] = "com.voyemu.foreign";
                            }
                        }
                    }
                }

                if (mode == 7) {
                    std::swap(bad["configure"]["start"].at(0), bad["configure"]["start"].at(1));
                }

                bool denied = false;
                try {
                    (void)qinggan11_wheel_capability(bad);
                } catch (...) {
                    denied = true;
                }

                check(denied,
                      "OEM power cannot broaden component, donor, install or startup graph");
            }
        }

        for (int mode = 0; mode < 5; ++mode) {
            auto invalid = a11;

            if (mode == 0) {
                invalid["original"]["KeyManagerService"]["sha256"] = "unpinned";
            }

            if (mode == 1) {
                invalid["components"]["wheel"] = "components/wheel";
            }

            if (mode == 2) {
                invalid["components"].erase("wheel-platform");
            }

            if (mode == 3) {
                invalid["patches"]["KeyManagerService"]["steps"] =
                        Json::array({"framework-libraries.json"});
            }

            if (mode == 4) {
                for (auto &node : invalid["install"]) {
                    if (node.at("artifact") == "component:wheel-platform") {
                        node["target"] = "/system/framework/QGAPI.jar";
                    }
                }
            }

            bool denied = false;
            try {
                (void)qinggan11_wheel_capability(invalid);
            } catch (...) {
                denied = true;
            }

            check(denied, "OEM wheel graph pin/dependency/target/backup negatives reject");
        }

        for (int mode = 0; mode < 11; ++mode) {
            auto bad = a11;

            if (mode == 0) {
                bad["components"]["gallery-arm64-libraries"] = "components/foreign";
            }

            if (mode == 1) {
                bad["patches"]["Gallery"]["uses"] = Json::array();
            }

            if (mode == 2) {
                bad["original"]["Gallery"]["sha256"] = "unpinned";
            }

            if (mode == 3) {
                bad["patches"]["Gallery"]["steps"] = Json::array({"framework-libraries.json"});
            }

            if (mode == 4) {
                bad["install"].push_back(Json{{"artifact", "patched:Gallery"}, {"type", "apk"}});
            }

            if (mode == 5) {
                bad["components"]["foreign"] = "components/foreign";
            }

            if (mode == 6) {
                bad["install"].push_back(
                        Json{{"artifact", "component:gallery-arm64-libraries"}, {"type", "file"}});
            }

            if (mode == 7) {
                bad["install"].erase(std::remove_if(bad["install"].begin(), bad["install"].end(),
                                                    [](const auto &n) {
                                                        return n.at("artifact") ==
                                                               "patched:Gallery";
                                                    }),
                                     bad["install"].end());
            }

            if (mode == 8) {
                for (auto &n : bad["install"]) {
                    if (n.at("artifact") == "patched:Gallery") {
                        n["type"] = "file";
                    }
                }
            }

            if (mode == 9) {
                bad["install"].push_back(Json{
                        {"artifact", "patched:Gallery"}, {"type", "file"}, {"target", "/foreign"}});
            }

            if (mode == 10) {
                bad["install"].push_back(Json{{"artifact", "original:Gallery"}, {"type", "apk"}});
            }

            bool denied = false;
            try {
                (void)qinggan11_wheel_capability(bad);
            } catch (...) {
                denied = true;
            }

            check(denied,
                  "Gallery cannot widen wheel routes, accept altered dependencies, duplicate APK or install build-only archive");
        }

        {
            auto legacy = a11;
            legacy["components"].erase("oem-power");
            legacy["configure"]["start"].erase(legacy["configure"]["start"].begin());
            legacy["components"].erase("gallery-arm64-libraries");
            legacy["patches"].erase("Gallery");
            legacy["install"].erase(
                    std::remove_if(legacy["install"].begin(), legacy["install"].end(),
                                   [](const auto &n) {
                                       return n.at("artifact") == "patched:Gallery" ||
                                              n.at("artifact") == "component:oem-power";
                                   }),
                    legacy["install"].end());
            check(qinggan11_wheel_capability(legacy) ==
                          qinggan11_partial_wheel_capability("free-11-chinese-20260731-7.3.1"),
                  "Legacy five-component Free wheel capability remains byte-identical");
        }
        {
            auto legacy = a11;
            legacy["components"].erase("oem-power");
            legacy["configure"]["start"].erase(legacy["configure"]["start"].begin());
            legacy["components"].erase("gallery-arm64-libraries");
            legacy["components"].erase("wheel-input");
            legacy["components"].erase("wheel-platform");
            legacy["install"].erase(
                    std::remove_if(legacy["install"].begin(), legacy["install"].end(),
                                   [](const auto &n) {
                                       return n.at("artifact") == "patched:Gallery" ||
                                              n.at("artifact") == "component:oem-power" ||
                                              n.at("artifact") == "component:wheel-input" ||
                                              n.at("artifact") == "component:wheel-platform";
                                   }),
                    legacy["install"].end());
            check(qinggan11_wheel_capability(legacy).at("reason") == "provider_not_installed",
                  "Legacy three-component graph retains unsupported provider contract");
        }

        for (int mode = 0; mode < 3; ++mode) {
            auto bad = a11;
            auto &steps = bad["patches"]["KeyManagerService"]["steps"];

            if (mode == 0) {
                steps = Json::array({"api30-key-ingress.json", "api30-keymanager.json"});
            }

            if (mode == 1) {
                steps = Json::array({"api30-key-ingress.json"});
            }

            if (mode == 2) {
                steps.push_back("api30-key-ingress.json");
            }

            bool denied = false;
            try {
                (void)qinggan11_wheel_capability(bad);
            } catch (...) {
                denied = true;
            }

            check(denied, "authenticated ingress requires exact ordered predecessor, no repeats");
        }

        {
            auto legacy = a11;
            legacy["patches"]["KeyManagerService"]["steps"] =
                    Json::array({"api30-keymanager.json"});
            check(!qinggan11_wheel_capability(legacy).at("supported").get<bool>(),
                  "historical wheel graph stays unsupported");
        }
        std::ifstream canbus_source(std::filesystem::path(argv[1]) / qinggan11_id /
                                    "components/canbus/component.json");
        Json canbus;
        canbus_source >> canbus;
        check(canbus.at("donorDex").at("original") == "MusicConcate" &&
                      canbus.at("donorDex").at("dex") == "classes2.dex" &&
                      canbus.at("donorDex").at("classes").size() == 67,
              "A11 Binder donor must use actual complete Music DEX and new DTOs");
        check(a11.at("initial_state").at("aeb_sts") == 1,
              "A11 fresh simulated AEB input selects the OEM no-warning branch");
        std::ifstream bootstrap_source(std::filesystem::path(argv[1]) / qinggan11_id /
                                       "components/bootstrap/component.json");
        Json bootstrap;
        bootstrap_source >> bootstrap;
        check(bootstrap.at("minSdk") == 30 && bootstrap.at("targetSdk") == 30 &&
                      !bootstrap.contains("donorDex") && !bootstrap.contains("frameworkBase") &&
                      !bootstrap.contains("upstreamEdits"),
              "A11 display bootstrap must be own API30 public source configuration");
        std::ifstream bootstrap_manifest_source(std::filesystem::path(argv[1]) / qinggan11_id /
                                                "components/bootstrap/AndroidManifest.xml");
        const std::string bootstrap_manifest(
                (std::istreambuf_iterator<char>(bootstrap_manifest_source)), {});
        check(bootstrap_manifest.find(
                      "<uses-library android:name=\"QGAPI\" android:required=\"true\" />") !=
                      std::string::npos,
              "activation publisher bootstrap requires the installed original QGAPI library");
        check(a11.at("configure").at("homeActivity") ==
                      "com.qinggan.app.launcher/com.qinggan.app.launcher.activity.MainActivity",
              "accepted A11 bootstrap must retain exact primary OEM HOME");
        const auto qinggan11_declared = declared_patch_plan(a11);
        const Json jar_dependencies = {
                {"QGAPI", Json::array({"original:QGAPI"})},
                {"api30-secondary-navigation",
                 Json::array({"android:/system/framework/services.jar"})},
                {"original-navi-framework", Json::array({"component:view-observation"})},
                {"original-navi-services", Json::array({"patched:api30-secondary-navigation"})},
                {"original-navi-vehicle", Json::array({"original:QGVehicle"})}};
        unsigned clean_jars = 0;

        for (const auto &node : qinggan11_declared.at("patches")) {
            if (node.at("type") == "jar") {
                ++clean_jars;
                const auto id = node.at("id").get<std::string>();
                check(jar_dependencies.contains(id) &&
                              node.at("dependencies") == jar_dependencies.at(id),
                      "original Navi JAR has its exact reviewed dependency");
            }
        }

        check(clean_jars == jar_dependencies.size(),
              "all five original Navi JAR dependencies required");
        check(a11.at("install").at(0) ==
                      Json{{"artifact", "patched:keymanager-contexts-api30"},
                           {"type", "file"},
                           {"target", "/system/etc/selinux/plat_service_contexts"},
                           {"mode", "0644"}},
              "clean service contexts install must be the ordered first system file");

        for (int mode = 0; mode < 6; ++mode) {
            auto bad = a11;

            if (mode == 0) {
                bad["platform"] = "passion";
            }

            if (mode == 1) {
                bad["android"]["package"] = "system-images;android-28;default;arm64-v8a";
            }

            if (mode == 2) {
                bad["patches"]["api30-secondary-navigation"]["input"] = "original:services";
            }

            if (mode == 3) {
                bad["patches"]["api30-secondary-navigation"]["steps"] =
                        Json::array({"framework-libraries.json"});
            }

            if (mode == 4) {
                bad["patches"]["foreign-jar"] = bad["patches"]["api30-secondary-navigation"];
            }

            if (mode == 5) {
                bad["patches"]["api30-secondary-navigation"]["uses"] =
                        Json::array({"original:services"});
            }

            bool denied = false;
            try {
                (void)declared_patch_plan(bad);
            } catch (...) {
                denied = true;
            }

            check(denied, "foreign JAR route must fail closed");
        }

        const auto qinggan11_recipes = api30_file_recipes_for("qinggan11",
                                                              original_navi_requested(a11));

        for (auto it = a11.at("patches").begin(); it != a11.at("patches").end(); ++it) {
            if (it.key() == "original-navi-framework" || it.key() == "original-navi-services" ||
                it.key() == "original-navi-vehicle") {
                const auto input = it.key() == "original-navi-framework"
                                           ? "component:view-observation"
                                   : it.key() == "original-navi-services"
                                           ? "patched:api30-secondary-navigation"
                                           : "original:QGVehicle";
                const auto step = it.key() == "original-navi-framework" ? "original-navi-egl.json"
                                  : it.key() == "original-navi-services"
                                          ? "original-navi-libraries.json"
                                          : "original-navi-vehicle.json";
                check(it.value() == Json{{"type", "jar"},
                                         {"input", input},
                                         {"steps", Json::array({step})}},
                      "original Navi JAR recipe is exact");
                continue;
            }

            if (it.key() == "original-navi-selinux") {
                check(it.value() == Json{{"type", "file"},
                                         {"input",
                                          "android:/system/etc/selinux/plat_mac_permissions.xml"},
                                         {"steps", Json::array({"original-navi-selinux.json"})}},
                      "original Navi certificate policy recipe is exact");
                continue;
            }

            if (it.key() == "BluetoothPhone") {
                check(a11.at("original").contains("BluetoothPhone") &&
                              it.value() ==
                                      Json{{"type", "apk"},
                                           {"input", "original:BluetoothPhone"},
                                           {"steps", Json::array({"api30-phone-provider.json"})},
                                           {"sign", "platform"}},
                      "Free Phone provider binds its actual original through the exact API30 recipe and platform signer");
                continue;
            }

            if (it.key() == "api30-stock-statusbar") {
                check(it.value() ==
                              Json{{"type", "apk"},
                                   {"input", "android:/system_ext/priv-app/SystemUI/SystemUI.apk"},
                                   {"steps", Json::array({"api30-stock-statusbar.json"})}},
                      "Free stock statusbar patch binds only the exact clean SDK APK and reviewed guard");
                continue;
            }

            if (it.key() == "QGAPI") {
                check(it.value() == Json{{"type", "jar"},
                                         {"input", "original:QGAPI"},
                                         {"steps", Json::array({"api30-audio-consumer.json",
                                                                "original-navi-vin.json"})}},
                      "Free QGAPI audio guard uses exact acquired JAR recipe");
                continue;
            }

            if (it.key() == "api30-secondary-navigation") {
                check(it.value() ==
                              Json{{"type", "jar"},
                                   {"input", "android:/system/framework/services.jar"},
                                   {"steps", Json::array({"api30-secondary-navigation.json"})}},
                      "Free secondary policy must use exact clean services JAR");
                continue;
            }

            if (it.value().at("type") == "file") {
                if (it.key() == "original-navi-selinux") {
                    continue;
                }

                const auto spec = std::find_if(qinggan11_recipes.begin(), qinggan11_recipes.end(),
                                               [&](const auto &item) {
                                                   return item.id == it.key();
                                               });

                if (spec == qinggan11_recipes.end()) {
                    check(it.value().at("input").get<std::string>().rfind("android:", 0) == 0 &&
                                  it.value().at("steps").size() >= 1 &&
                                  it.value().at("steps").back() == it.key() + ".json",
                          "A11 composed native policy node anchors a clean Android input and its own final step");
                    continue;
                }

                Json expected = {{"type", "file"},
                                 {"input", "android:" + std::string(spec->target)},
                                 {"steps", it.value().at("steps")}};

                if (it.key() == "selinux-hash-api30") {
                    expected["uses"] =
                            Json::array({"patched:can-early-policy-api30",
                                         "android:/system/etc/selinux/mapping/30.0.cil"});
                }

                if (it.key() == "can-simulator-policy-api30" ||
                    it.key() == "can-early-policy-api30") {
                    expected["uses"] =
                            Json::array({"original:SystemExtSepolicy", "original:VendorSepolicy"});
                }

                check(it.value() == expected && it.value().at("steps").size() >= 1 &&
                              it.value().at("steps").back() == it.key() + ".json",
                      "A11 native policy declaration exact");
                continue;
            }

            check(it.value().at("input") == "original:" + it.key() &&
                          a11.at("original").contains(it.key()) && it.value().at("type") == "apk" &&
                          it.value().at("steps") ==
                                  (it.key() == "AudioPolicy"
                                           ? Json::array({"api30-audio-policy.json"})
                                   : it.key() == "Hiboard"
                                           ? Json::array({"api30-hiboard-libraries.json",
                                                          "api30-hiboard-routing.json",
                                                          "api30-hiboard-startup.json"})
                                   : it.key() == "KeyManagerService"
                                           ? Json::array({"api30-keymanager.json",
                                                          "api30-key-ingress.json"})
                                   : it.key() == "Camera"
                                           ? Json::array({"api30-navigation-camera-libraries.json",
                                                          "api30-camera-virtual.json"})
                                   : it.key() == "Gallery"
                                           ? Json::array({"api30-gallery-arm64.json"})
                                   : it.key() == "QGMedia"
                                           ? Json::array({"api30-media-runtime.json",
                                                          "api30-media-cloud-availability.json",
                                                          "api30-media-notification-actions.json",
                                                          "api30-usb-player.json"})
                                   : it.key() == "MusicConcate"
                                           ? Json::array({"framework-libraries.json",
                                                          "music-optional-arm32.json",
                                                          "api30-music-page.json",
                                                          "api30-music-fullscreen.json"})
                                   : it.key() == "SystemService"
                                           ? Json::array({"framework-libraries.json",
                                                          "api30-network.json"})
                                   : it.key() == "Setting"
                                           ? Json::array({"framework-libraries.json",
                                                          "api30-str-power.json",
                                                          "api30-settings-voice-clone.json"})
                                   : it.key() == "Launcher"
                                           ? Json::array({"framework-libraries.json",
                                                          "api30-launcher-power.json",
                                                          "api30-aqy-provider.json",
                                                          "api30-launcher-canbus.json",
                                                          "api30-launcher-dvr.json"})
                                   : it.key() == "SystemUI"
                                           ? Json::array({"framework-libraries.json",
                                                          "api30-systemui-routing.json"})
                                   : (it.key() == "VehicleAir" || it.key() == "VehicleSetting")
                                           ? Json::array({"api30-vehicle-libraries.json"})
                                   : it.key() == "ThirdScreen"
                                           ? Json::array({"framework-libraries.json",
                                                          "api30-thirdscreen-rear.json"})
                                   : it.key() == "SystemPolicy"
                                           ? Json::array({"api30-systempolicy-hardware.json"})
                                   : it.key() == "QGBus"
                                           ? Json::array({"api30-qgbus-libraries.json"})
                                           : Json::array({"framework-libraries.json"})),
                  "A11 generic adapter must bind its actual original APK");
        }

        for (const auto *role : {"VehicleAir", "VehicleSetting"}) {
            const auto &input = a11.at("original").at(role);
            check(input.at("sha256") ==
                          (std::string(role) == "VehicleAir"
                                   ? "b7b80ed1ff6b7b054d6035c6e39e7c04d97e9f2d906cd42f82f9da3a61e2fa2b"
                                   : "f37a53f1397678af472a87d89d903ba8f6ed6ac892d83ae3117b57bbe3b21360"),
                  "Free main apps bind genuine system APK pins, never theme overlays");
            check(std::count(a11.at("install").begin(), a11.at("install").end(),
                             Json{{"artifact", std::string("patched:") + role}, {"type", "apk"}}) ==
                          1,
                  "Free genuine Vehicle/Air installed once through normal APK graph");
        }

        std::ifstream recipe_source(std::filesystem::path(argv[1]) / qinggan11_id /
                                    "patches/framework-libraries.json");
        Json recipe;
        recipe_source >> recipe;
        check(recipe.at("api") == 30 && recipe.at("calls").size() == 1 &&
                      recipe.at("calls").at(0).at("operation") == "useLibraries" &&
                      recipe.at("calls").at(0).at("args") ==
                              Json::array({Json::array({"QGAPI", "QGMOD", "QGVehicle"})}) &&
                      recipe.at("constants").at("original_sha256_allowlist").size() == 7,
              "A11 recipe cannot inherit guessed API28 code offsets");
        const auto qinggan09 =
                embedded_profile_names(ProfilePlatform::qinggan09, ProfileBoard::sa8155).at(0);
        const auto fresh = select_vehicle_startup(
                VehicleStartup::fresh, VehicleCatalog::qinggan09,
                embedded_profile(qinggan09).at("definition").at("initial_state"), nullptr);
        check(fresh.at("raw_oem").at("aeb_sts") == 1 && fresh.at("raw_oem").at("cfg_aeb") == 1,
              "fresh first-vehicle fixture keeps AEB configured and selects the verified no-OFF input");
        check(initial_state(VehicleCatalog::qinggan09).at("raw_oem").at("aeb_sts") == 0,
              "profile display fixture must not change the recovered raw protocol defaults");
        check(declared_patch_plan(Json::object()).at("patches").empty(),
              "profiles without patch operations have an empty plan");
        auto plan = declared_patch_plan(embedded_profile(qinggan09).at("definition"));
        check(plan.at("patches").size() ==
                              embedded_profile(qinggan09).at("definition").at("patches").size() &&
                      !plan.at("executable").get<bool>(),
              "real profile declares all patches without pretending to implement preparation");
        unsigned native_recipes = 0;

        for (const auto &node : plan.at("patches")) {
            if (node.contains("native_recipe")) {
                ++native_recipes;
                check(node.at("native_recipe").at("api") == 28,
                      "native init recipes retain their API guard");
            }
        }

        check(native_recipes == 12,
              "first vehicle exposes twelve native pinned-file recipes including guarded codec allocation");
        auto android11 = embedded_profile(qinggan09).at("definition");
        android11["android"]["package"] = "system-images;android-30;default;arm64-v8a";
        const auto android11_plan = declared_patch_plan(android11);

        for (const auto &node : android11_plan.at("patches")) {
            check(!node.contains("native_recipe"),
                  "Android 11 cannot claim Android 9 file handlers");
        }

        auto missing_api = android11;
        missing_api.erase("android");
        const auto missing_api_plan = declared_patch_plan(missing_api);

        for (const auto &node : missing_api_plan.at("patches")) {
            check(!node.contains("native_recipe"),
                  "unknown API cannot claim a native file handler");
        }

        std::set<std::string> completed;

        for (const auto &node : plan.at("patches")) {
            for (const auto &ref : node.at("dependencies")) {
                auto text = ref.get<std::string>();

                if (text.starts_with("patched:")) {
                    check(completed.contains(text.substr(8)),
                          "patched dependency precedes its consumer");
                }
            }

            completed.insert(node.at("id").get<std::string>());
        }

        Json fixture = {{"original", {{"input", Json::object()}}},
                        {"components", Json::object()},
                        {"patches",
                         {{"a",
                           {{"type", "file"},
                            {"input", "original:input"},
                            {"uses", Json::array({"patched:b"})}}},
                          {"b", {{"type", "file"}, {"input", "patched:a"}}}}}};
        bool invalid = false;
        try {
            declared_patch_plan(fixture);
        } catch (const std::exception &e) {
            invalid = std::string(e.what()).find("PREPARATION_CYCLE") != std::string::npos;
        }

        check(invalid, "cyclic graph rejected");
        fixture["patches"]["a"]["uses"] = Json::array({"component:missing"});
        invalid = false;
        try {
            declared_patch_plan(fixture);
        } catch (const std::exception &e) {
            invalid = std::string(e.what()).find("PREPARATION_REFERENCE") != std::string::npos;
        }

        check(invalid, "undeclared component rejected");
        fixture["patches"]["a"]["uses"] = Json::array();
        fixture["patches"]["b"]["input"] = "patched:missing";
        invalid = false;
        try {
            declared_patch_plan(fixture);
        } catch (const std::exception &e) {
            invalid = std::string(e.what()).find("PREPARATION_REFERENCE") != std::string::npos;
        }

        check(invalid, "undeclared patch rejected");
        bool rejected = false;
        try {
            embedded_profile("not-bundled");
        } catch (...) {
            rejected = true;
        }

        check(rejected, "unknown bundled profile rejected");
        std::cout << "embedded profile catalogue passed\n";

        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';

        return 1;
    }
}
