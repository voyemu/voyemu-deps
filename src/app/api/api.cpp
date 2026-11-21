#include "api_internal.hpp"
#include "error.hpp"
#include "field_delivery.hpp"
#include "version.hpp"
#include <algorithm>
#include <sstream>

namespace voyemu::api {
using namespace detail;
namespace detail {
std::uint64_t monotonic_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
            .count();
}

bool token(std::string_view value) {
    return hex_text(value, 32);
}

std::string text(const Json &object, const char *key, std::size_t maximum) {
    if (!object.contains(key) || !object.at(key).is_string()) {
        throw VoyemuError("TEST_ARGUMENT", "required string missing");
    }

    auto value = object.at(key).get<std::string>();

    if (value.empty() || value.size() > maximum ||
        value.find_first_of("\r\n") != std::string::npos || value.find('\0') != std::string::npos) {
        throw VoyemuError("TEST_ARGUMENT", "invalid bounded string");
    }

    return value;
}

std::filesystem::path session_active_directory(const std::filesystem::path &root,
                                               const std::string &profile,
                                               const MachineIdentity &machine) {
    try {
        return native_current_active_directory(root, profile, machine);
    } catch (const std::runtime_error &failure) {
        const std::string message = failure.what();

        if (message_code(message) == "SESSION_IDENTITY") {
            throw VoyemuError("TEST_SESSION", message.substr(sizeof "SESSION_IDENTITY: " - 1));
        }

        throw;
    }
}

std::uint64_t number(const Json &object, const char *key, std::uint64_t fallback,
                     std::uint64_t minimum, std::uint64_t maximum) {
    if (!object.contains(key)) {
        if (fallback < minimum || fallback > maximum) {
            throw VoyemuError("TEST_ARGUMENT", "required number missing");
        }

        return fallback;
    }

    const auto &value = object.at(key);

    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
        throw VoyemuError("TEST_ARGUMENT", "unsigned integer required");
    }

    auto n = value.get<std::uint64_t>();

    if (n < minimum || n > maximum) {
        throw VoyemuError("TEST_ARGUMENT", "number outside declared bounds");
    }

    return n;
}

bool boolean(const Json &object, const char *key, bool fallback) {
    if (!object.contains(key)) {
        return fallback;
    }

    if (!object.at(key).is_boolean()) {
        throw VoyemuError("TEST_ARGUMENT", "boolean required");
    }

    return object.at(key).get<bool>();
}

std::string profile(const Json &request) {
    auto value = text(request, "profile");

    if (!valid_profile_name(value)) {
        throw VoyemuError("PROFILE_NAME", "unsafe profile");
    }

    return value;
}

void validate_sources(const Json &request) {
    if (!request.contains("sources")) {
        return;
    }

    const auto &sources = request.at("sources");

    if (!sources.is_array() || sources.empty() || sources.size() > 8) {
        throw VoyemuError("TEST_ARGUMENT", "sources must contain 1..8 unique source IDs");
    }

    std::set<std::string> seen;

    for (const auto &id : sources) {
        if (!id.is_string()) {
            throw VoyemuError("TEST_ARGUMENT", "source ID must be a string");
        }

        const auto value = id.get<std::string>();

        if (value.empty() || value.size() > 64 || value.front() < 'a' || value.front() > 'z' ||
            value.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") !=
                    std::string::npos ||
            !seen.insert(value).second) {
            throw VoyemuError("TEST_ARGUMENT", "invalid or duplicate source ID");
        }
    }
}

// Command completion does not assert a visible UI effect.
std::string terminal_outcome(const Json &response) {
    if (!response.is_object() || !response.contains("ok") || !response.at("ok").is_boolean()) {
        return "unknown";
    }

    if (response.at("ok").get<bool>()) {
        return "completed";
    }

    if (!response.contains("error") || !response.at("error").is_object()) {
        return "unknown";
    }

    const auto &failure = response.at("error");

    if (failure.contains("outcome") &&
        (failure.at("outcome") == "not_started" || failure.at("outcome") == "unknown")) {
        return failure.at("outcome").get<std::string>();
    }

    return "unknown";
}

Json error(const std::exception &exception, bool contacted, bool dispatched) {
    std::string message = exception.what(), code(failure_code(exception));

    // An exception without its own CODE is classified by the progress of the operation. Before
    // any supervisor exchange a JSON exception reads the request shape, which the client
    // corrects; afterwards it reads a response. The other kinds keep their code and cause, as on
    // the console. After an action was dispatched its effect is unknown.
    if (message_code(message).empty()) {
        if (code == "JSON") {
            code = contacted || dispatched ? "IPC_PROTOCOL" : "TEST_ARGUMENT";
            message = contacted || dispatched ? "Invalid JSON response shape"
                                              : "Invalid JSON request shape";
        } else if (code.empty()) {
            code = "INTERNAL_ERROR";
        }

        if (dispatched) {
            code += "_UNKNOWN";
        }
    }

    if (message.size() > 1024) {
        message.resize(1024);
    }

    const bool tree_retry = code == "TREE_TRANSPORT_UNKNOWN" || code == "TREE_TIMEOUT_UNKNOWN" ||
                            code == "TREE_TIMEOUT" || code == "CAN_TRANSPORT_UNKNOWN" ||
                            code == "CAN_TIMEOUT_UNKNOWN" || code == "CAN_TIMEOUT";
    // The _UNKNOWN suffix is the only unknown-outcome classification.
    const bool unknown = code.ends_with("_UNKNOWN");
    Json result = {
            {"code", code},
            {"message", message},
            {"retryable", code == "TEST_BUSY" || code == "CLUSTER_UI_NOT_READY" || tree_retry},
            {"outcome", unknown ? "unknown" : "not_started"}};

    if (const auto *fence = dynamic_cast<const InputFenceError *>(&exception)) {
        result["details"] = fence->details;
    }

    if (const auto *identity = dynamic_cast<const MachineIdentityError *>(&exception);
        identity && !identity->details.empty()) {
        result["details"] = identity->details;
    }

    return result;
}

std::string admission_scope(const Json &request) {
    const auto op = text(request, "op");

    if (op == "session.start") {
        return "profile:" + profile(request);
    }

    if (op == "test.run" && request.contains("start")) {
        return "profile:" + profile(request.at("start"));
    }

    if (request.contains("session")) {
        const auto id = text(request.at("session"), "id", 32);

        if (!token(id)) {
            throw VoyemuError("TEST_ARGUMENT", "valid session ID required for action scope");
        }

        return "session:" + id;
    }

    return "profile:" + profile(request);
}

#ifndef _WIN32
bool owned_wheel_lease_held(const std::filesystem::path &path) {
    LogFile before(path);
    const auto private_mode = std::filesystem::perms::owner_read |
                              std::filesystem::perms::owner_write;

    if (std::filesystem::status(path).permissions() != private_mode || before.size() != 0) {
        throw VoyemuError("TEST_WHEEL_LEASE", "invalid private wheel lease");
    }

    const bool held = !file_lock_released(path);
    LogFile after(path);

    if (before.identity() != after.identity() || after.size() != 0 ||
        std::filesystem::status(path).permissions() != private_mode) {
        throw VoyemuError("TEST_WHEEL_LEASE", "wheel lease identity changed");
    }

    return held;
}

bool matching_wheel_writer(const std::filesystem::path &root,
                           const std::filesystem::path &directory, const Json &request) {
    if (profile(request) != qinggan11_731_profile_id) {
        return false;
    }

    const auto marker = directory / "wheel-writer.json";

    if (std::filesystem::symlink_status(marker).type() == std::filesystem::file_type::not_found) {
        return false;
    }

    LogFile file(marker);
    const auto mode = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;

    if (file.size() > 4096 || std::filesystem::status(marker).permissions() != mode) {
        throw VoyemuError("TEST_WHEEL_LEASE", "invalid private wheel writer");
    }

    const auto writer = Json::parse(file.read(0));

    if (!writer.is_object() || !writer.contains("schema") ||
        !writer.at("schema").is_number_integer() || !writer.contains("operation_id") ||
        !writer.at("operation_id").is_string() ||
        !token(writer.at("operation_id").get<std::string>())) {
        return false;
    }

    const auto id = text(request.at("session"), "id"),
               ownership = text(request.at("session"), "ownership_id");
    const Json expected = {{"schema", 1},
                           {"profile", qinggan11_731_profile_id},
                           {"session_id", id},
                           {"ownership_sha256", sha256_bytes(ownership)},
                           {"operation_id", writer.at("operation_id")}};

    if (writer != expected || !owned_wheel_lease_held(directory / "wheel-writer.lock")) {
        return false;
    }

    const auto pending =
            read_private_json(root / ".local/build/automation/operations" /
                                      (writer.at("operation_id").get<std::string>() + ".json"),
                              32768);

    return pending.is_object() && pending.contains("schema") &&
           pending.at("schema").is_number_integer() && pending.at("schema") == 1 &&
           pending.value("operation_id", Json(nullptr)) == writer.at("operation_id") &&
           pending.value("scope", std::string{}) == "session:" + id &&
           pending.value("profile", std::string{}) == qinggan11_731_profile_id &&
           pending.value("op", std::string{}) == "input.wheel.press" &&
           pending.value("status", std::string{}) == "pending";
}
#endif

Json operations() {
    return Json::array({"info",
                        "profile.list",
                        "profile.get",
                        "session.start",
                        "session.attach",
                        "session.status",
                        "session.stop",
                        "session.restart",
                        "session.wait",
                        "session.watch.start",
                        "session.watch.read",
                        "session.watch.stop",
                        "session.ui",
                        "session.screenshot",
                        "session.logs",
                        "session.observe",
                        "multimedia.power",
                        "multimedia.reboot",
                        "multimedia.launch",
                        "multimedia.system",
                        "multimedia.phone.configure",
                        "multimedia.phone.connect",
                        "multimedia.phone.disconnect",
                        "multimedia.phone.incoming",
                        "multimedia.phone.answer",
                        "multimedia.phone.hangup",
                        "multimedia.phone.state",
                        "cluster.power",
                        "cluster.reboot",
                        "cluster.system",
                        "cluster.ipk.history",
                        "vehicle.fields",
                        "vehicle.get",
                        "vehicle.set",
                        "vehicle.can.get",
                        "vehicle.can.model",
                        "vehicle.can.history",
                        "input.tap",
                        "input.drag",
                        "input.gesture",
                        "input.key.press",
                        "input.wheel.buttons",
                        "input.wheel.press",
                        "input.wheel.release",
                        "input.voice.trigger",
                        "operation.get",
                        "test.run"});
}

Json requested_screens(const Json &request, bool android_only) {
    if (request.contains("screen") && request.contains("screens")) {
        throw VoyemuError("TEST_ARGUMENT", "screen and screens are mutually exclusive");
    }

    Json screens = request.contains("screen")
                           ? Json::array({request.at("screen")})
                           : request.value("screens", Json::array({"primary", "secondary"}));

    if (!screens.is_array() || screens.empty() || screens.size() > screen_roles.size() + 1) {
        throw VoyemuError("TEST_ARGUMENT", "one to five unique screens required");
    }

    std::set<std::string> seen;

    for (const auto &value : screens) {
        if (!value.is_string()) {
            throw VoyemuError("TEST_ARGUMENT", "screen must be a logical screen name");
        }

        const auto name = value.get<std::string>();

        const bool known = name == "cluster" ||
                           std::any_of(screen_roles.begin(), screen_roles.end(),
                                       [&](const auto &role) {
                                           return role.first == name;
                                       });

        if (!known || !seen.insert(name).second) {
            throw VoyemuError("TEST_ARGUMENT", "invalid or duplicate screen");
        }

        if (android_only && name == "cluster") {
            throw VoyemuError("TEST_UNSUPPORTED", "cluster has no Android UI backend");
        }

        if (android_only && name == "hud") {
            throw VoyemuError("TEST_UNSUPPORTED", "the screen has no UI observer");
        }
    }

    return screens;
}

// The Android screens of the guest UI observer by slot; the slot is also the SDK display of the
// screen on machines with these screens.
std::size_t observed_slot(std::string_view surface) {
    for (std::size_t slot = 0; slot < observed_screens.size(); ++slot) {
        if (surface == "media_" + std::string(observed_screens[slot])) {
            return slot;
        }
    }

    throw VoyemuError("TREE_PROTOCOL", "wrong logical screen identity");
}

// The screens of the machine from the session status; a status without a list carries the two
// multimedia screens.
std::vector<Screen> status_screens(const Json &status) {
    if (status.contains("screens") && !status.at("screens").is_null()) {
        return profile_screens(status.at("screens"));
    }

    const auto multimedia = [](const char *role) {
        return Json{{"role", role},
                    {"guest", "android"},
                    {"width", 1920},
                    {"height", 720},
                    {"density", 160}};
    };

    return profile_screens(Json::array({multimedia("primary"), multimedia("secondary")}));
}

// The emulator display of an Android screen of the machine.
media::GuestDisplay android_screen(const std::vector<Screen> &screens, std::string_view role) {
    const auto screen = find_screen(screens, role);

    if (!screen) {
        throw VoyemuError("TEST_UNSUPPORTED", "requested screen is not on this machine");
    }

    if (!screen->android_display) {
        throw VoyemuError("TEST_UNSUPPORTED", "requested screen has no guest frames or input");
    }

    return {*screen->android_display, screen->width, screen->height};
}

void validate_ui_filter(const Json &request) {
    if (request.contains("scope") && request.at("scope") != "all_attached" &&
        request.at("scope") != "interactive") {
        throw VoyemuError("TEST_ARGUMENT", "unsupported UI scope");
    }

    if (request.contains("selector")) {
        const auto &selector = request.at("selector");

        if (!selector.is_object()) {
            throw VoyemuError("TEST_ARGUMENT", "tree selector must be an object");
        }

        for (const auto &[key, value] : selector.items()) {
            if (key != "resource_id" && key != "package") {
                throw VoyemuError("TEST_ARGUMENT", "unsupported tree selector key");
            }
        }

        const auto resource = text(selector, "resource_id", 256);
        const auto separator = resource.find(":id/");
        const auto identifier = [](std::string_view value, bool package) {
            return !value.empty() && std::all_of(value.begin(), value.end(), [&](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '_' || (package && c == '.');
            });
        };

        if (separator == std::string::npos ||
            !identifier(std::string_view(resource).substr(0, separator), true) ||
            !identifier(std::string_view(resource).substr(separator + 4), false)) {
            throw VoyemuError("TEST_ARGUMENT", "fully qualified resource ID required");
        }

        if (selector.contains("package") && !identifier(text(selector, "package", 160), true)) {
            throw VoyemuError("TEST_ARGUMENT", "package identifier required");
        }
    }
}

PixelRect requested_rect(const Json &request, std::size_t width, std::size_t height) {
    if (!request.contains("rect")) {
        return {0, 0, static_cast<std::int64_t>(width), static_cast<std::int64_t>(height)};
    }

    const auto &rect = request.at("rect");

    if (!rect.is_object() || rect.size() != 4 || !rect.contains("x") || !rect.contains("y") ||
        !rect.contains("width") || !rect.contains("height")) {
        throw VoyemuError("TEST_ARGUMENT", "rect needs x, y, width and height only");
    }

    PixelRect result{static_cast<std::int64_t>(number(rect, "x", UINT64_MAX, 0, width - 1)),
                     static_cast<std::int64_t>(number(rect, "y", UINT64_MAX, 0, height - 1)),
                     static_cast<std::int64_t>(number(rect, "width", 0, 1, width)),
                     static_cast<std::int64_t>(number(rect, "height", 0, 1, height))};

    if (result.x + result.width > static_cast<std::int64_t>(width) ||
        result.y + result.height > static_cast<std::int64_t>(height)) {
        throw VoyemuError("TEST_ARGUMENT", "rect is outside screen");
    }

    return result;
}

Json rect_json(PixelRect rect) {
    return {{"x", rect.x}, {"y", rect.y}, {"width", rect.width}, {"height", rect.height}};
}

Json normalize_request(Json request, bool suite_case) {
    const auto op = text(request, "op");
    const auto supported = operations();

    if (std::find(supported.begin(), supported.end(), op) == supported.end()) {
        throw VoyemuError("TEST_UNSUPPORTED", "unknown closed API operation");
    }

    if (request.contains("surface") || request.contains("surfaces")) {
        throw VoyemuError("TEST_ARGUMENT",
                          "use screen or screens; legacy surface names are unsupported");
    }

    if (op == "cluster.system" || op == "multimedia.power") {
        for (const auto &[key, value] : request.items()) {
            (void)value;

            if (key != "schema" && key != "request_id" && key != "op" && key != "profile" &&
                key != "session" && key != "machine" && key != "timeout_ms" &&
                !(op == "multimedia.power" && key == "enabled") && !(suite_case && key == "id")) {
                throw VoyemuError("TEST_ARGUMENT", "unsupported " + op + " field");
            }
        }
    }

    if (op == "session.ui") {
        for (const auto &[key, value] : request.items()) {
            (void)value;

            if (key != "schema" && key != "request_id" && key != "op" && key != "profile" &&
                key != "session" && key != "machine" && key != "timeout_ms" && key != "screen" &&
                key != "screens" && key != "scope" && key != "selector" &&
                !(suite_case && key == "id")) {
                throw VoyemuError("TEST_ARGUMENT", "unsupported session.ui field");
            }
        }
    }

    // The screen of the tap bounds its coordinates once its size is read from the session.
    if (op == "input.tap" && !request.contains("selector")) {
        const auto x = number(request, "x", UINT64_MAX, 0, 4095),
                   y = number(request, "y", UINT64_MAX, 0, 4095);
        request["points"] = Json::array({{{"x", x}, {"y", y}, {"at_ms", 0}}});
    }

    if (op == "test.run" && request.contains("create")) {
        throw VoyemuError("TEST_ARGUMENT", "test.run takes start or an owned session");
    }

    if (op == "operation.get") {
        if (!request.contains("target") || !request.at("target").is_object() ||
            request.at("target").size() != 1) {
            throw VoyemuError("TEST_ARGUMENT", "operation.get needs one explicit target");
        }

        const auto &target = request.at("target");

        if (target.contains("operation_id")) {
            request["id"] = text(target, "operation_id", 32);
        } else if (target.contains("request_id")) {
            request["lookup_request_id"] = text(target, "request_id");
        } else {
            throw VoyemuError("TEST_ARGUMENT", "unknown operation lookup target");
        }
    }

    return request;
}

bool writer(std::string_view op) {
    return phone_input_operation(op) || op == "multimedia.launch" || op == "multimedia.reboot" ||
           op == "multimedia.power" || op == "session.restart" || op == "session.start" ||
           op == "session.stop" || op == "vehicle.set" || op == "input.wheel.press" ||
           op == "input.wheel.release" || op == "input.voice.trigger" || op == "input.tap" ||
           op == "input.drag" || op == "input.gesture" || op == "input.key.press" ||
           op == "cluster.power" || op == "cluster.reboot";
}

bool recorded(std::string_view op) {
    return writer(op) || op == "session.screenshot" || op == "test.run";
}

Json public_snapshot(Json snapshot) {
    snapshot.erase("endpoint");
    snapshot.erase("request_id");
    snapshot.erase("catalog");

    return snapshot;
}

std::filesystem::path artifacts(const std::filesystem::path &root, const Json &request) {
    auto path = std::filesystem::path(text(request, "artifact_dir", 4096));
    const auto base = root / ".local/build/tmp";

    if (!path.is_absolute() || path != path.lexically_normal() || path == base ||
        path.lexically_relative(base).empty() ||
        path.lexically_relative(base).generic_string().starts_with("..")) {
        throw VoyemuError("TEST_ARTIFACT_PATH", "use a new directory below root/.local/build/tmp");
    }

    if (std::filesystem::exists(path) || std::filesystem::is_symlink(path)) {
        throw VoyemuError("TEST_ARTIFACT_EXISTS", "artifact directory must be new");
    }

    ensure_private_directory(path);

    return path;
}

void new_json(const std::filesystem::path &path, const Json &value) {
    auto bytes = value.dump(2) + "\n";

    voyemu::output::write_artifact(
            path, {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
}

Json observation(const Json &status, std::string_view predicate, const Json &request) {
    const auto deliveries = status.value("deliveries", Json::object()),
               android = deliveries.value("android", Json::object()),
               qnx = deliveries.value("qnx", Json::object());
    bool ready = false;

    if (predicate == "session.reachable") {
        ready = !status.value("stopped", true);
    } else if (predicate == "android.ready") {
        ready = media::android_public_ready(android);
    } else if (predicate == "android.delivery") {
        ready = android.value("guest_acknowledged", false) &&
                android.value("sequence", 0ULL) >= number(request, "revision", 0, 1, UINT32_MAX);
    } else if (predicate == "qnx.delivery") {
        ready = qnx.value("vehicle", Json::object()).value("status", std::string{}) ==
                        "guest_acknowledged" &&
                qnx.value("vehicle", Json::object()).value("source_sequence", 0ULL) >=
                        number(request, "revision", 0, 1, UINT32_MAX);
    } else if (predicate == "inputs.released") {
        ready = status.value("held_buttons", 1u) == 0;
    } else {
        throw VoyemuError("TEST_UNSUPPORTED",
                          "unknown readiness predicate; scene assertions belong to the runner");
    }

    return {{"predicate", predicate},
            {"ready", ready},
            {"last_observation", public_snapshot(status)},
            {"visible_effect_verified", false},
            {"exported_frame_verified", false}};
}

Json read_logs(const std::filesystem::path &path, const Json &request) {
    LogFile file(path);
    const auto identity = file.identity();
    auto cursor = request.value("cursor", Json::object());
    auto offset = number(cursor, "offset", 0, 0, UINT64_MAX);
    bool gap = offset > file.size() ||
               (cursor.contains("file_id") && cursor.at("file_id") != identity);

    if (gap) {
        offset = 0;
    }

    auto buffer = file.read(offset);
    Json events = Json::array();
    std::size_t consumed = 0, skipped = 0;

    while (consumed < buffer.size() && events.size() < 128) {
        const auto end = buffer.find('\n', consumed);

        if (end == std::string::npos) {
            break;
        }

        auto line = buffer.substr(consumed, end - consumed);
        consumed = end + 1;
        auto event = Json::parse(line, nullptr, false);

        if (!event.is_object() || event.value("schemaVersion", 0) != 1) {
            ++skipped;
            continue;
        }

        Json safe = Json::object();

        for (const char *key : {"sequence", "timestamp", "level", "component", "message"}) {
            if (event.contains(key) && event.at(key).dump().size() <= 4096) {
                safe[key] = event.at(key);
            }
        }

        events.push_back(std::move(safe));
    }

    if (consumed == 0 && buffer.size() == 65536) {
        consumed = buffer.size();
        gap = true;
    }

    return {{"events", events},
            {"cursor", {{"file_id", identity}, {"offset", offset + consumed}}},
            {"gap", gap},
            {"unstructured_lines_skipped", skipped},
            {"fields_omitted_for_redaction", true},
            {"complete", false}};
}

// Supervisor control call: public request, internal operation, mutation and budget in ms.
using SupervisorControl = std::function<Json(const Json &, const Json &, bool, int)>;
class SupervisorGuestQnx final : public GuestQnx {
public:
    explicit SupervisorGuestQnx(SupervisorControl control) : control_(std::move(control)) {}

    bool lifecycle_supported(const Json &request) override {
        return control_(request, {{"op", "status"}}, false, 2000)
                .value("deliveries", Json::object())
                .value("qnx", Json::object())
                .value("lifecycle_supported", false);
    }

    Json power(const Json &request, bool enabled) override {
        return control_(request, {{"op", "cluster_power"}, {"enabled", enabled}}, true,
                        request_timeout_limit_ms("cluster_power"));
    }

    Json restart(const Json &request) override {
        return control_(request, {{"op", "cluster_restart"}}, true,
                        request_timeout_limit_ms("cluster_restart"));
    }

    Json ui(const Json &request, int timeout_ms) override {
        return control_(request, {{"op", "test_cluster_ui"}, {"timeout_ms", timeout_ms}}, false,
                        timeout_ms);
    }

    Json system(const Json &request, int timeout_ms) override {
        return control_(request, {{"op", "test_cluster_system"}, {"timeout_ms", timeout_ms}}, false,
                        timeout_ms);
    }

    std::optional<frames::Frame> frame(const std::filesystem::path &session_directory,
                                       std::uint64_t after_sequence, int timeout_ms) override {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        frames::Reader reader(session_directory / "frame.bgra");

        for (;;) {
            if (auto frame = reader.read_latest(after_sequence)) {
                return frame;
            }

            if (std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())
                        .count() < 1) {
                return std::nullopt;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

private:
    SupervisorControl control_;
};
class SupervisorGuestAndroid final : public GuestAndroid {
public:
    explicit SupervisorGuestAndroid(SupervisorControl control) : control_(std::move(control)) {}

    Json reboot(const Json &request, const std::string &mode, int timeout_ms) override {
        return control_(request, {{"op", "android_reboot"}, {"mode", mode}}, true, timeout_ms);
    }

    Json power(const Json &request, bool enabled, int timeout_ms) override {
        return control_(request, {{"op", "test_media_power"}, {"enabled", enabled}}, true,
                        timeout_ms);
    }

    Json launch(const Json &request, const Json &app) override {
        auto operation = app;

        operation["op"] = "test_app_launch";

        return control_(request, operation, true, 20000);
    }

    Json ui_observer(const Json &request, bool recover, int timeout_ms) override {
        Json operation{{"op", "ui_observer"}, {"timeout_ms", timeout_ms}};

        if (recover) {
            operation["recover"] = true;
        }

        return control_(request, operation, false, timeout_ms);
    }

    Json ui_exchange(const Json &request, const Json &connection, const Json &message,
                     int timeout_ms) override {
        try {
            return control_(request,
                            {{"op", "ui_observer_exchange"},
                             {"connection", connection},
                             {"message", message},
                             {"timeout_ms", timeout_ms}},
                            false, timeout_ms)
                    .at("observation");
        } catch (const std::runtime_error &error) {
            const std::string_view reason(error.what());

            if (reason.starts_with("IPC_OPERATION:")) {
                throw VoyemuError("TEST_UNSUPPORTED",
                                  "running native session predates owned UI exchange");
            }

            if (reason.starts_with("IPC_TIMEOUT_UNKNOWN:")) {
                throw VoyemuError("TREE_TIMEOUT_UNKNOWN", "observer control deadline elapsed");
            }

            if (reason.starts_with("IPC_CONNECT:") || reason.starts_with("IPC_READ_UNKNOWN:") ||
                reason.starts_with("IPC_WRITE_UNKNOWN:")) {
                throw VoyemuError("TREE_TRANSPORT_UNKNOWN", "observer control unavailable");
            }

            throw;
        }
    }

    Json boot_identity(const Json &request, int timeout_ms) override {
        return control_(request, {{"op", "test_boot_identity"}}, false, timeout_ms);
    }

    media::Image frame(const std::filesystem::path &session_directory,
                       const std::string &session_id, media::GuestDisplay display,
                       int timeout_ms) override {
        return media::capture_image(
                media::read_connection(session_directory / "media-connection.json", session_id),
                display, timeout_ms);
    }

    Json system(const Json &request, int timeout_ms) override {
        return control_(request, {{"op", "test_output_system"}, {"timeout_ms", timeout_ms}}, false,
                        timeout_ms + 500);
    }

    Json phone(const Json &request, const Json &command) override {
        return control_(request,
                        {{"op", "phone_control"},
                         {"method", command.at("method")},
                         {"payload", command.at("payload")}},
                        command.at("method") != "snapshot", 15000);
    }

    Json logs(const Json &request, const Json &query, int timeout_ms) override {
        auto operation = query;

        operation["op"] = "test_output_logs";

        return control_(request, operation, false, timeout_ms);
    }

private:
    SupervisorControl control_;
};
} // namespace detail

void validate_media_input(const Json &input) {
    const auto kind = text(input, "kind");

    if (kind == "key") {
        const auto key = text(input, "key", 64);

        if (key.find_first_not_of(
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_ ") !=
            std::string::npos) {
            throw VoyemuError("TEST_ARGUMENT", "bounded SDK key name required");
        }

        return;
    }

    if (kind != "pointer") {
        throw VoyemuError("TEST_UNSUPPORTED", "media input kind unavailable");
    }

    number(input, "display_id", android_screen_limit, 0, android_screen_limit - 1);
    const auto width = number(input, "display_width", 0, 1, 4096),
               height = number(input, "display_height", 0, 1, 4096);
    const auto &points = input.at("points");

    if (width * height > screen_pixel_limit) {
        throw VoyemuError("TEST_ARGUMENT", "bounded pointer screen required");
    }

    if (!points.is_array() || points.empty() || points.size() > 64) {
        throw VoyemuError("TEST_ARGUMENT", "pointer needs 1..64 points");
    }

    std::uint64_t previous = 0;

    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto &p = points.at(i);
        number(p, "x", width, 0, width - 1);
        number(p, "y", height, 0, height - 1);
        const auto at = number(p, "at_ms", 2001, 0, 2000);

        if ((i == 0 && at != 0) || (i && at < previous)) {
            throw VoyemuError("TEST_ARGUMENT", "ordered trajectory must begin at 0 ms");
        }

        previous = at;
    }
}

MediaInput media_input(const std::filesystem::path &directory, const std::string &session_id) {
    struct Pointer {
        media::GuestDisplay display;
        int x, y;
    };
    auto held = std::make_shared<std::optional<Pointer>>();

    return [directory, session_id, held](const Json &input) {
        validate_media_input(input);
        auto connection = media::read_connection(directory / "media-connection.json", session_id);
        auto release = [&] {
            if (*held) {
                const auto p = **held;
                media::send_input(connection, "sendTouch",
                                  media::touch_request(p.display, p.x, p.y, false), 1000);
                held->reset();
            }
        };
        try {
            release();
        } catch (const std::exception &) {
            throw VoyemuError("TEST_INPUT_CLEANUP_UNKNOWN", "previous pointer release unverified");
        }

        const auto kind = text(input, "kind");

        if (kind == "key") {
            const auto key = text(input, "key", 64);

            if (key.find_first_not_of(
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_ ") !=
                std::string::npos) {
                throw VoyemuError("TEST_ARGUMENT", "bounded SDK key name required");
            }

            media::send_input(connection, "sendKey", media::key_request(key), 1500);

            return Json{{"ok", true},
                        {"sdk_completed", true},
                        {"visible_effect_verified", false},
                        {"key", key},
                        {"phase", "press"}};
        }

        if (kind != "pointer") {
            throw VoyemuError("TEST_UNSUPPORTED", "media input kind unavailable");
        }

        const media::GuestDisplay display{
                static_cast<unsigned>(number(input, "display_id", android_screen_limit, 0,
                                             android_screen_limit - 1)),
                static_cast<unsigned>(number(input, "display_width", 0, 1, 4096)),
                static_cast<unsigned>(number(input, "display_height", 0, 1, 4096))};
        const auto &points = input.at("points");

        if (!points.is_array() || points.empty() || points.size() > 64) {
            throw VoyemuError("TEST_ARGUMENT", "pointer needs 1..64 points");
        }

        std::uint64_t previous = 0;

        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto &p = points.at(i);
            number(p, "x", display.width, 0, display.width - 1);
            number(p, "y", display.height, 0, display.height - 1);
            const auto at = number(p, "at_ms", 2001, 0, 2000);

            if ((i == 0 && at != 0) || (i && at < previous)) {
                throw VoyemuError("TEST_ARGUMENT", "ordered trajectory must begin at 0 ms");
            }

            previous = at;
        }

        const auto start = Clock::now(), deadline = start + std::chrono::seconds(5);
        Json sent = Json::array();
        try {
            for (const auto &p : points) {
                const auto at = p.at("at_ms").get<unsigned>();
                std::this_thread::sleep_until(start + std::chrono::milliseconds(at));
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                               deadline - Clock::now())
                                               .count();

                if (remaining <= 0) {
                    throw VoyemuError("TEST_INPUT_TIMEOUT", "bounded gesture deadline elapsed");
                }

                *held = Pointer{display, p.at("x").get<int>(), p.at("y").get<int>()};
                const auto point = **held;
                media::send_input(connection, "sendTouch",
                                  media::touch_request(point.display, point.x, point.y, true),
                                  static_cast<int>(std::min<std::int64_t>(1500, remaining)));
                sent.push_back({{"x", point.x},
                                {"y", point.y},
                                {"scheduled_ms", at},
                                {"completed_monotonic_us", monotonic_us()}});
            }

            release();
        } catch (const std::exception &) {
            try {
                release();
            } catch (const std::exception &) {
                throw VoyemuError(
                        "TEST_INPUT_CLEANUP_UNKNOWN",
                        "pointer release unverified; stop own session before further input");
            }

            throw;
        }

        return Json{{"ok", true},
                    {"sdk_completed", true},
                    {"pointer_released", true},
                    {"display_id", display.id},
                    {"sent", sent},
                    {"visible_effect_verified", false}};
    };
}

Json capture_source_start(const ProcessSpec &process, bool windows,
                          const SourceStartCapture &capture) {
    auto result = capture(process, windows ? 300000 : 100000);

    if (result.timed_out) {
        throw VoyemuError("TEST_CREATE_UNKNOWN",
                          "launcher timed out; reconcile operation before retrying");
    }

    auto response = Json::parse(result.output, nullptr, false);

    if (response.is_discarded()) {
        throw VoyemuError("TEST_CREATE_UNKNOWN",
                          "launcher result unavailable; reconcile operation before retrying");
    }

    if (result.exit_code || !response.value("ok", false)) {
        throw std::runtime_error(
                response.value("error", std::string("TEST_CREATE_UNKNOWN: launcher failed")));
    }

    return response;
}

Api::Api(std::filesystem::path root, std::filesystem::path executable, Transports transports)
    : root_(std::move(root)), executable_(std::move(executable)),
      transports_(std::move(transports)) {
    if (!root_.is_absolute() || root_ != root_.lexically_normal()) {
        throw VoyemuError("TEST_ROOT", "explicit absolute runtime root required");
    }

    if (!transports_.control) {
        transports_.control = [](const auto &endpoint, const auto &op, int timeout) {
            return voyemu::request(endpoint, op, timeout);
        };
    }

    if (!transports_.qnx || !transports_.android) {
        const SupervisorControl control = [this](const Json &request, const Json &operation,
                                                 bool mutation, int timeout_ms) {
            return this->control(request, operation, mutation, timeout_ms);
        };

        if (!transports_.qnx) {
            transports_.qnx = std::make_shared<SupervisorGuestQnx>(control);
        }

        if (!transports_.android) {
            transports_.android = std::make_shared<SupervisorGuestAndroid>(control);
        }
    }

    if (!transports_.create) {
        transports_.create = [this](const Json &request) {
            ProcessSpec spec;

            spec.executable = executable_;
            spec.directory = root_;
            spec.arguments = {"_start",         "--root",
                              root_.string(),   "--profile",
                              profile(request), "--exclusive",
                              "--ownership-id", text(request, "ownership_id")};

            if (request.contains("machine_path")) {
                spec.arguments.push_back(text(request, "machine_path", 4096));
            } else if (request.contains("machine")) {
                spec.arguments.push_back("--machine");
                spec.arguments.push_back(text(request, "machine"));
            }

            for (const auto &manifest : request.value("providers", Json::array())) {
                spec.arguments.push_back("--provider");
                spec.arguments.push_back(manifest.get<std::string>());
            }

            if (boolean(request, "refresh_host", false)) {
                spec.arguments.push_back("--refresh-host");
                spec.arguments.push_back("--refresh-session");
                spec.arguments.push_back(text(request.at("session"), "id"));
            }

            if (boolean(request, "background", true)) {
                spec.arguments.push_back("--background");
            }

            if (request.value("scenario", std::string("fresh")) == "resume") {
                spec.arguments.push_back("--resume");
            }

            spec.arguments.push_back(boolean(request, "with_cluster", false) ? "--with-cluster"
                                                                             : "--no-cluster");

            if (request.contains("seconds")) {
                spec.arguments.push_back("--seconds");
                spec.arguments.push_back(std::to_string(number(request, "seconds", 0, 1, 3600)));
            }

            return capture_source_start(spec,
#ifdef _WIN32
                                        true,
#else
                           false,
#endif
                                        [](const ProcessSpec &process, int timeout) {
                                            return execute(process, timeout);
                                        });
        };
    }
}

Json Api::session_record(const Json &request, bool mutation) {
    const auto name = profile(request);
    const auto &session = request.at("session");
    const auto id = text(session, "id");

    if (!token(id)) {
        throw VoyemuError("TEST_SESSION", "valid explicit session ID required");
    }

    auto machine = machine_identity(name, request.value("machine", std::string{}));
    auto directory = native_session_directory(root_, name, machine, id);

    if (!request.contains("machine")) {
        const auto machines = root_ / ".local/machines";
        bool found = false;

        if (std::filesystem::is_directory(machines) && !std::filesystem::is_symlink(machines)) {
            for (const auto &entry : std::filesystem::directory_iterator(machines)) {
                if (entry.is_symlink() || !entry.is_directory() ||
                    !valid_profile_name(entry.path().filename().string())) {
                    continue;
                }

                if (!ordinary_native_launch_file(entry.path() / "active" / (name + ".json"))) {
                    continue;
                }

                const auto candidate = native_session_directory(
                        root_, name,
                        voyemu::machine_identity(name, entry.path().filename().string()), id);

                if (!ordinary_native_launch_file(candidate / "automation-owner.json") &&
                    !ordinary_native_launch_file(candidate / "machine-selection.json")) {
                    continue;
                }

                if (found) {
                    throw VoyemuError("TEST_SESSION", "ambiguous session epoch");
                }

                directory = candidate;
                found = true;
            }
        }
    }

    const auto selection_path = directory / "machine-selection.json";

    if (std::filesystem::exists(selection_path) || std::filesystem::is_symlink(selection_path)) {
        const auto selection = read_private_json(selection_path, 4096);

        if (!selection.contains("machine") || !selection.at("machine").is_string()) {
            throw VoyemuError("TEST_SESSION", "retained machine selection missing");
        }

        const auto selector = selection.at("machine").get<std::string>();
        const auto seed = std::filesystem::path(text(selection, "seed", 4096));
        const auto digest = text(selection, "manifest_sha256");

        if ((!selector.empty() && !valid_profile_name(selector)) || !seed.is_absolute() ||
            seed.lexically_normal() != seed ||
            !machine_selector_matches(root_, MachinePathSelection{seed, name}, selector) ||
            !sha256_text(digest)) {
            throw VoyemuError("TEST_SESSION", "retained machine selection differs");
        }

        machine = machine_identity(name, selector);
    }

    if (request.contains("machine") &&
        machine_identity(name, text(request, "machine")).name != machine.name) {
        throw VoyemuError("TEST_SESSION", "requested machine differs from session");
    }

    auto record = read_private_json(
            session_active_directory(root_, name, machine) / (name + ".json"), 4096);

    if (record.at("session_id") != id) {
        throw VoyemuError("TEST_SESSION", "stale session epoch");
    }

    // The CLI publishes a canonical Windows path; source path joins may retain
    // mixed separators. Compare components without following aliases or parents.
    if (!record.at("directory").is_string() ||
        std::filesystem::path(record.at("directory").get<std::string>()) != directory) {
        throw VoyemuError("TEST_SESSION", "session directory identity differs");
    }

    if (mutation) {
        if (!session.contains("ownership_id")) {
            throw VoyemuError("TEST_OWNERSHIP", "attached sessions are read only");
        }

        const auto ownership = text(session, "ownership_id");

        if (!token(ownership)) {
            throw VoyemuError("TEST_OWNERSHIP", "automation ownership required");
        }

        const auto owner = read_private_json(directory / "automation-owner.json", 4096);

        if (owner.at("session_id") != id || owner.at("ownership_id") != ownership) {
            throw VoyemuError("TEST_OWNERSHIP", "attached sessions are read only");
        }
    }

    return record;
}

Json Api::control(const Json &request, const Json &operation, bool mutation, int timeout_ms) {
    auto record = session_record(request, mutation);
    auto op = operation;

    op["schema"] = 1;
    op["session_id"] = record.at("session_id");
    op["request_id"] = request.at("request_id");
#ifdef _WIN32
    // Internal IPC carries the caller's remaining budget; it cannot enlarge an
    // operation-specific timeout already present in the source request.
    if (timeout_ms < 1) {
        throw VoyemuError("TEST_DEADLINE", "no remaining control budget");
    }

    op["timeout_ms"] =
            op.contains("timeout_ms")
                    ? std::min<std::uint64_t>(number(op, "timeout_ms", timeout_ms, 0, 180000),
                                              static_cast<unsigned>(timeout_ms))
                    : static_cast<unsigned>(timeout_ms);
#endif

    if (mutation) {
        op["ownership_id"] = request.at("session").at("ownership_id");
    }

    for (const char *key : {"expected_revision", "expected_generation"}) {
        if (request.contains(key)) {
            op[key] = number(request, key, 0, 0, UINT32_MAX);
        }
    }

    if (operation.value("op", std::string{}) == "status") {
        const auto directory = std::filesystem::path(record.at("directory").get<std::string>()),
                   terminal_path = directory / "runtime.json";

        if (StoppedAndroidLease::endpoint_absent(record.at("endpoint").get<std::string>()) &&
            ordinary_native_launch_file(terminal_path)) {
            auto terminal = read_private_json(terminal_path);

            if (terminal.value("session_id", std::string{}) ==
                        record.at("session_id").get<std::string>() &&
                terminal.value("stopped", false)) {
                terminal["ok"] = true;
                terminal["state"] = read_private_json(directory / "vehicle-state.json",
                                                      vehicle_state_bytes);
                terminal["held_buttons"] = 0;
                terminal["wheel_routes"] = nullptr;
                terminal["observation_source"] = "owned terminal runtime record";

                return public_snapshot(std::move(terminal));
            }
        }
    }

    const auto name = operation.value("op", std::string{});
    const bool power = name == "test_media_power", restart = name.starts_with("machine_restart");
    const auto endpoint = record.at("endpoint").get<std::string>();
    try {
        contacted_ = true;

        if (mutation) {
            dispatched_ = true;
        }

        auto response = transports_.control(endpoint, op, timeout_ms);

        if (response.value("session_id", std::string{}) !=
            record.at("session_id").get<std::string>()) {
            if (power) {
                throw VoyemuError("ANDROID_POWER_UNKNOWN", "response epoch differs after dispatch");
            }

            if (restart) {
                throw VoyemuError("SESSION_RESTART_UNKNOWN",
                                  "response epoch differs after dispatch");
            }

            throw VoyemuError("TEST_SESSION", "response epoch differs");
        }

        if (power && !response.at("ok").is_boolean()) {
            throw VoyemuError("ANDROID_POWER_UNKNOWN", "invalid lifecycle response after dispatch");
        }

        if (!response.value("ok", false)) {
            throw std::runtime_error(
                    response.value("error", std::string("TEST_CONTROL: request rejected")));
        }

        const auto action_request = response.value("request_id", Json());

        if (power && action_request != request.at("request_id")) {
            throw VoyemuError("ANDROID_POWER_UNKNOWN",
                              "response request identity differs after dispatch");
        }

        auto result = public_snapshot(std::move(response));

        if (restart) {
            result["request_id"] = action_request;
        }

        return result;
    } catch (const Json::exception &) {
        if (power) {
            throw VoyemuError("ANDROID_POWER_UNKNOWN", "malformed lifecycle reply after dispatch");
        }

        if (restart) {
            throw VoyemuError("SESSION_RESTART_UNKNOWN",
                              "malformed lifecycle reply after dispatch");
        }

        throw;
    }
}

Json Api::call(const Json &supplied) {
    const auto received = Clock::now();
    const auto start = monotonic_us();
    Json response{{"schema", 1},
                  {"request_id", nullptr},
                  {"operation_id", random_token()},
                  {"ok", false}};
    std::filesystem::path operation_path;
    std::unique_ptr<Lock> lock;

#ifndef _WIN32
    // Destruction releases the private wheel lease before the global writer.
    std::unique_ptr<Lock> wheel_lease, cancellation_gate;
#endif
    std::unique_ptr<AdmissionStore> admission;
    std::string scope, request_id;
    bool admitted = false, public_ui = false;
    contacted_ = false;
    dispatched_ = false;
    try {
        if (!supplied.is_object() || !supplied.contains("schema") ||
            !supplied.at("schema").is_number_integer() || supplied.at("schema") != 1) {
            throw VoyemuError("TEST_SCHEMA", "expected schema 1 object");
        }

        request_id = text(supplied, "request_id");
        response["request_id"] = request_id;
        auto request = normalize_request(supplied);
        const auto op = text(request, "op");
        public_ui = op == "session.ui";

        if (recorded(op)) {
            scope = admission_scope(request);
            admission =
                    std::make_unique<AdmissionStore>(root_ / ".local/build/automation/admissions");
            auto state = admission->begin(scope, request_id, supplied);
            response["operation_id"] = state.operation_id;

            if (!state.admitted) {
                if (state.response) {
                    const auto &saved = *state.response;

                    if (!saved.is_object() || saved.value("schema", 0) != 1 ||
                        saved.value("request_id", std::string{}) != request_id ||
                        saved.value("operation_id", std::string{}) != state.operation_id) {
                        throw VoyemuError("OPERATION_UNKNOWN", "saved action response malformed");
                    }

                    return saved;
                }

                if (state.status == "pending") {
                    throw VoyemuError("OPERATION_PENDING_UNKNOWN",
                                      "action is already admitted; query operation.get");
                }

                throw VoyemuError("OPERATION_UNKNOWN", "interrupted admission cannot be replayed");
            }

            admitted = true;
        }

        request["operation_id"] = response.at("operation_id");

        if (recorded(op) || op == "session.wait") {
            operation_path = root_ / ".local/build/automation/operations" /
                             (response.at("operation_id").get<std::string>() + ".json");

            if (op == "session.start") {
                request["ownership_id"] = boolean(request, "refresh_host", false)
                                                  ? text(request.at("session"), "ownership_id")
                                                  : random_token();
            }

            Json pending{{"schema", 1},
                         {"operation_id", response.at("operation_id")},
                         {"request_id", request_id},
                         {"op", op},
                         {"status", "pending"},
                         {"outcome", "unknown"}};

            if (admitted) {
                pending["scope"] = scope;
            }

            for (const char *key : {"profile", "ownership_id", "machine"}) {
                if (request.contains(key)) {
                    pending[key] = request.at(key);
                }
            }

            if (op == "session.restart") {
                pending["session_id"] = text(request.at("session"), "id", 32);

                if (request.at("session").value("ownership_id", Json()).is_string()) {
                    pending["ownership_sha256"] = sha256_bytes(
                            request.at("session").at("ownership_id").get<std::string>());
                }

                if (request.contains("expected_boot")) {
                    pending["expected_boot"] = request.at("expected_boot");
                }

                if (request.contains("expected_can")) {
                    pending["expected_can"] = request.at("expected_can");
                }
            }

            write_private_json(operation_path, pending);

            // Admission receipts remain recoverable even when the execution writer is busy.
            if (recorded(op)) {
                auto execution = root_ / ".local/build/automation";

                if (request.contains("session") && op != "session.start") {
                    const auto record = session_record(request, writer(op));
                    execution = std::filesystem::path(record.at("directory").get<std::string>()) /
                                "automation";
                }

#ifndef _WIN32
                auto gate = std::make_unique<Lock>(execution / "writer-admission.lock");
                bool cancel = false;

                if (op == "input.wheel.release") {
                    (void)session_record(request, true);
                    cancel = matching_wheel_writer(root_, execution, request);
                }

                if (cancel) {
                    cancellation_gate = std::move(gate);
                } else {
                    lock = std::make_unique<Lock>(execution / "writer.lock");
                    std::filesystem::remove(execution / "wheel-writer.json");

                    if (op == "input.wheel.press" && profile(request) == qinggan11_731_profile_id &&
                        number(request, "hold_ms", 100, 1, 2000) > 200) {
                        (void)session_record(request, true);
                        const auto button = text(request, "button");

                        if (button == "volume_up" || button == "volume_down") {
                            wheel_lease = std::make_unique<Lock>(execution / "wheel-writer.lock");
                            write_private_json(
                                    execution / "wheel-writer.json",
                                    {{"schema", 1},
                                     {"profile", qinggan11_731_profile_id},
                                     {"session_id", request.at("session").at("id")},
                                     {"ownership_sha256",
                                      sha256_bytes(text(request.at("session"), "ownership_id"))},
                                     {"operation_id", response.at("operation_id")}});
                        }
                    }
                }
#else
                lock = std::make_unique<Lock>(execution / "writer.lock");
#endif
            }
        }

        const auto ui_deadline =
                op == "session.ui" ? received + std::chrono::milliseconds(number(
                                                        request, "timeout_ms", 5000, 1, 30000))
                                   : Clock::time_point{};
        response["result"] = dispatch(request, ui_deadline);

        response["ok"] = true;

        if (op == "session.ui") {
            response["elapsed_ms"] = (monotonic_us() - start) / 1000.;
            bound_session_ui_response(response, qnx_kanzi_ui_response_bytes, ui_deadline);
        }
    } catch (const std::exception &exception) {
        if (public_ui) {
            response.erase("result");
            response["ok"] = false;
        }

        response["error"] = operation_error(exception);
        logging::write(logging::Level::error, "test-api", "operation.failed",
                       {{"operation_id", response.at("operation_id")},
                        {"code", response.at("error").at("code")}});
    }

    if (!response.contains("elapsed_ms")) {
        response["elapsed_ms"] = (monotonic_us() - start) / 1000.;
    }

    if (!operation_path.empty()) {
        try {
            auto terminal = read_private_json(operation_path);
            terminal["status"] = "terminal";
            terminal["response"] = response;
            terminal["outcome"] = terminal_outcome(response);
            write_private_json(operation_path, terminal, 8 * 1024 * 1024 + 32768);
        } catch (const std::exception &) {
            response["ok"] = false;
            response["error"] = {
                    {"code", "TEST_OPERATION_RECORD"},
                    {"message", "Operation completed but its durable receipt could not be saved"},
                    {"retryable", false},
                    {"outcome", "unknown"}};
        }
    }

    if (admitted) {
        try {
            admission->complete(scope, request_id, response.at("operation_id").get<std::string>(),
                                response);
        } catch (const std::exception &) {
            response["ok"] = false;
            response["error"] = {
                    {"code", "TEST_OPERATION_RECORD"},
                    {"message",
                     "Action receipt could not be committed; this request must not be replayed"},
                    {"retryable", false},
                    {"outcome", "unknown"}};
        }
    }

    return response;
}

Json Api::operation_error(const std::exception &exception) const {
    return error(exception, contacted_, dispatched_);
}

Json Api::dispatch(const Json &request, Clock::time_point deadline) {
    const auto op = text(request, "op");

    // Each domain handler answers its own operations and leaves the others unhandled.
    for (const auto handler :
         {&Api::dispatch_catalog, &Api::dispatch_session, &Api::dispatch_lifecycle,
          &Api::dispatch_input, &Api::dispatch_observe}) {
        if (auto reply = (this->*handler)(op, request, deadline); !reply.is_discarded()) {
            return reply;
        }
    }

    if (op == "test.run") {
        return run(request);
    }

    throw VoyemuError("TEST_UNSUPPORTED", "unknown closed API operation");
}

Json Api::dispatch_catalog(const std::string &op, const Json &request, Clock::time_point deadline) {
    if (op == "info") {
        return {{"api", "voyemu.test"},
                {"schema", 1},
                {"version", std::string(version())},
                {"release_date", std::string(release_date())},
                {"platform", std::string(os::platform_name)},
                {"windows_runtime_supported", os::platform_name == "Windows"},
                {"mcp", false},
                {"operations", operations()},
                {"provider_registration",
                 "session.start.providers: up to four explicit pinned manifest files; read through session.observe sources"},
                {"voice_semantics",
                 "trigger invokes the steering-wheel assistant button; recognized text/audio injection is unavailable"},
                {"screens", Json::array({{{"screen", "primary"},
                                          {"sdk_display_id", 0},
                                          {"width", 1920},
                                          {"height", 720},
                                          {"positive_after_sequence_supported", false}},
                                         {{"screen", "secondary"},
                                          {"sdk_display_id", 1},
                                          {"width", 1920},
                                          {"height", 720},
                                          {"positive_after_sequence_supported", false}},
                                         {{"screen", "cluster"},
                                          {"width", 1920},
                                          {"height", 720},
                                          {"positive_after_sequence_supported", true}}})},
                {"limits",
                 {{"pointer_points", 64},
                  {"pointer_duration_ms", 2000},
                  {"pointer_deadline_ms", 5000},
                  {"wheel_hold_ms", 2000},
                  {"wait_timeout_ms", 60000},
                  {"cases", 64},
                  {"diagnostic_bytes", 65536},
                  {"diagnostic_events", 128},
                  {"artifact_bytes_per_capture", 40 * 1024 * 1024}}},
                {"boot_policies", Json::array({"auto"})},
                {"scenario_policies", Json::array({"fresh", "resume"})},
                {"readiness", Json::array({"session.reachable", "android.ready", "android.delivery",
                                           "qnx.delivery", "inputs.released"})},
                {"frame_semantics",
                 "selected original-screen captures; runtime and boot fenced; host receive skew bounded; guest scanout is not atomic"},
                {"unsupported",
                 Json::array({"text_injection", "current_windows", "editor_text", "scene_oracle",
                              "host_ui_capture", "atomic_capture", "video", "guest_shell",
                              "boot_cold_override", "boot_restore_required",
                              "automatic_mutation_retry", "android_positive_after_sequence",
                              "input.voice.text", "input.voice.audio"})}};
    }

    if (op == "profile.list" || op == "profile.get") {
        std::set<std::string> names;

        if (op == "profile.get") {
            names.insert(profile(request));
        } else {
            for (const auto &name : embedded_profile_names()) {
                names.insert(name);
            }

            for (const auto &name : native_profile_names(root_)) {
                names.insert(name);
            }
        }

        Json output = Json::array();

        for (const auto &name : names) {
            const auto directory = profile_launch_directory(root_, name,
                                                            voyemu::machine_identity(name, {}));
            Json item{{"id", name},
                      {"android_prepared",
                       ordinary_native_launch_file(directory / "android-launch.json")},
                      {"cluster_prepared",
                       ordinary_native_launch_file(directory / "cluster-launch.json")},
                      {"runs_started", false},
                      {"pins_verified", false}};

            for (const auto &bundled : embedded_profile_catalog()) {
                if (bundled.at("id") == name) {
                    item["definition_sha256"] = sha256_bytes(bundled.at("definition").dump());
                }
            }

            output.push_back(std::move(item));
        }

        return op == "profile.get" ? output.at(0) : output;
    }

    if (op == "vehicle.fields") {
        // The catalogue of the platform of the running machine, or of the named profile; with a
        // session each field names the guests of its running machine that receive it.
        std::optional<FieldDelivery> delivery;
        VehicleCatalog catalog = VehicleCatalog::qinggan09;

        if (request.contains("session")) {
            const auto status = control(request, {{"op", "status"}});

            if (!status.contains("field_delivery") || !status.contains("state")) {
                throw VoyemuError("TEST_UNSUPPORTED",
                                  "stopped runtime no longer exposes field delivery");
            }

            catalog = state_catalog(status.at("state"));
            delivery = field_delivery_from_status(status.at("field_delivery"));
        } else if (request.contains("profile")) {
            catalog = vehicle_catalog(embedded_profile(profile(request))
                                              .at("definition")
                                              .at("platform")
                                              .get<std::string>());
        } else {
            throw VoyemuError("TEST_ARGUMENT", "vehicle.fields needs a session or a profile");
        }

        auto fields = catalog_json(catalog);
        const auto signals = catalog_signals(catalog);
        auto encoding_json = [](const ConceptEncoding &encoding) {
            Json result = {{"resolution", encoding.resolution}, {"offset", encoding.offset}};

            if (!encoding.values.empty()) {
                result = {{"values", Json::array()}};

                for (const auto &value : encoding.values) {
                    result["values"].push_back({{"logical", value.logical}, {"raw", value.raw}});
                }
            }

            if (encoding.invalid) {
                result["invalid"] = *encoding.invalid;
            }

            return result;
        };

        for (std::size_t i = 0; i < fields.size(); ++i) {
            auto &field = fields[i];
            const auto name = field.at("name").get<std::string>();
            const auto width = field.at("width").get<unsigned>();
            field["minimum"] = 0;
            field["maximum"] = field_maximum(width);
            field["write"] = name.starts_with("swm_") ? "wheel_event_only" : "raw_patch";
            field["unit"] = "raw_oem";

            // The declaration of the OEM signal table, from which a client converts raw values.
            if (!signals.empty()) {
                const auto &signal = signals[i];
                Json declared = {{"id", signal.id}, {"type", signal.type}};

                for (const auto &[key, value] : {std::pair{"resolution", signal.resolution},
                                                 {"offset", signal.offset},
                                                 {"minimum", signal.minimum},
                                                 {"maximum", signal.maximum}}) {
                    if (value) {
                        declared[key] = *value;
                    }
                }

                if (signal.invalid) {
                    declared["invalid"] = *signal.invalid;
                }

                if (!signal.unit.empty()) {
                    declared["unit"] = signal.unit;
                    field["unit"] = signal.unit;
                }

                if (!signal.options.empty()) {
                    declared["values"] = Json::object();

                    for (const auto &option : signal.options) {
                        declared["values"][std::to_string(option.raw)] = option.name;
                    }
                }

                field["signal"] = std::move(declared);
            }

            // A bound field names its concept, its role and the encoding of the concept in it; the
            // range of a concept limits its primary field.
            if (const auto *binding = field_binding(catalog, name)) {
                const auto &definition = concept_definition(binding->id);
                const auto item = std::find_if(binding->fields.begin(), binding->fields.end(),
                                               [&](const ConceptField &bound) {
                                                   return bound.field == name;
                                               });
                const auto linear = item->encoding.values.empty();
                field["concept"] = {{"name", definition.name},
                                    {"unit", definition.unit},
                                    {"role", item->role == ConceptRole::primary  ? "primary"
                                             : item->role == ConceptRole::mirror ? "mirror"
                                                                                 : "follower"},
                                    {"encoding", encoding_json(item->encoding)}};

                if (signals.empty() && linear && item->encoding.offset == 0 &&
                    definition.unit.find("flag") == std::string_view::npos &&
                    definition.id != VehicleConcept::gear) {
                    std::ostringstream unit;
                    unit << item->encoding.resolution;
                    field["unit"] = item->encoding.resolution == 1
                                            ? std::string(definition.unit)
                                            : unit.str() + " " + std::string(definition.unit);
                }

                if (item->role == ConceptRole::primary && linear) {
                    if (definition.minimum) {
                        if (const auto raw = concept_encode(item->encoding, *definition.minimum,
                                                            width)) {
                            field["minimum"] = *raw;
                        }
                    }

                    if (definition.maximum) {
                        if (const auto raw = concept_encode(item->encoding, *definition.maximum,
                                                            width)) {
                            field["maximum"] = *raw;
                        }
                    }
                }
            }

            if (delivery) {
                field["delivery"] = field_delivery_json(*delivery, name);
            }
        }

        auto concepts = Json::array();

        for (const auto &binding : concept_bindings(catalog)) {
            const auto &definition = concept_definition(binding.id);
            Json item = {{"name", definition.name},
                         {"unit", definition.unit},
                         {"field", binding.fields.front().field}};

            if (definition.minimum) {
                item["minimum"] = *definition.minimum;
            }

            if (definition.maximum) {
                item["maximum"] = *definition.maximum;
            }

            concepts.push_back(std::move(item));
        }

        return {{"map_sha256", catalog_map_sha256(catalog)},
                {"fields", fields},
                {"concepts", concepts},
                {"validation", "atomic OEM widths and coupled fields; SOC <= 100; whole km/h"},
                {"raw_adas_injection", true}};
    }

    return unhandled_operation();
}

std::string diagnostic_machine_name(const Json &status, const Json &selection) {
    const auto machine = status.value("machine", std::string{});

    if (machine.empty()) {
        if (!selection.is_null()) {
            throw VoyemuError("TEST_LOGS_STALE", "selected machine absent from live supervisor");
        }

        return {};
    }

    if (!valid_profile_name(machine) || !selection.is_object() ||
        selection.value("machine", std::string{}) != machine ||
        selection.value("seed", std::string{}) != status.value("machine_seed", std::string{}) ||
        selection.value("manifest_sha256", std::string{}) !=
                status.value("machine_manifest_sha256", std::string{})) {
        throw VoyemuError("TEST_LOGS_STALE", "selected machine identity differs");
    }

    return machine;
}

int result_exit_code(const Json &response) {
    if (!response.value("ok", false)) {
        return response.value("error", Json::object()).value("code", std::string{}) ==
                               "TEST_UNSUPPORTED"
                       ? 3
                       : 4;
    }

    const auto result = response.value("result", Json::object());

    if (!result.is_object()) {
        return 0;
    }

    if (result.value("timed_out", false) || !result.value("accepted", true)) {
        return 4;
    }

    const auto status = result.find("status");

    if (status == result.end()) {
        return 0;
    }

    if (status->is_string()) {
        return *status == "failed" ? 4 : 0;
    }

    return status->is_number_integer() && *status == 0 &&
                           result.value("schema", Json()).is_number_integer() &&
                           result.at("schema") == Json(1) &&
                           result.value("producer", Json()) == Json("closed_ecu_boundary") &&
                           result.value("valid", Json()) == Json(true)
                   ? 0
                   : 4;
}
} // namespace voyemu::api
