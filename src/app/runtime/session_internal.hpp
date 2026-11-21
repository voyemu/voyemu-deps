#pragma once
#include "android_adb_route.hpp"
#include "android_audio.hpp"
#include "android_audio_startup.hpp"
#include "android_clock.hpp"
#include "android_console.hpp"
#include "android_embedded_adb.hpp"
#include "android_engine.hpp"
#include "android_launch_adb.hpp"
#include "android_owned_package.hpp"
#include "android_reboot.hpp"
#include "android_stop_policy.hpp"
#include "android_system_binding.hpp"
#include "can_model_observation.hpp"
#include "can_observation.hpp"
#include "can_restart_boundary.hpp"
#include "check.hpp"
#include "frame_transport.hpp"
#include "full_restart.hpp"
#include "interguest_link.hpp"
#include "logging.hpp"
#include "media_discovery.hpp"
#include "media_readiness.hpp"
#include "nxp_presentation.hpp"
#include "os/fs.hpp"
#include "os/net.hpp"
#include "os/platform.hpp"
#include "os/process.hpp"
#include "passion_mcu_property.hpp"
#include "passion_power_owner.hpp"
#include "passion_presentation.hpp"
#include "passion_wheel.hpp"
#include "phone_control.hpp"
#include "program_modes.hpp"
#include "provider_process.hpp"
#include "qinggan09_owned_packages.hpp"
#include "qinggan09_phone_package.hpp"
#include "qinggan11_startup.hpp"
#include "qnx_cluster_ui_wire.hpp"
#include "qnx_frame_grpc.hpp"
#include "qnx_interguest_binding.hpp"
#include "qnx_interguest_contract.hpp"
#include "qnx_mcu_projection.hpp"
#include "qnx_owned_ram.hpp"
#include "qnx_persistent_storage.hpp"
#include "qnx_process_ram.hpp"
#include "qnx_producer_identity.hpp"
#include "qnx_run_budget.hpp"
#include "qnx_shutdown_monitor.hpp"
#include "session.hpp"
#include "source_registry.hpp"
#include "system_observation.hpp"
#include "ui_observer.hpp"
#include "ui_observer_wire.hpp"
#include "ui_source.hpp"
#include "windows_renderer.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

// Helpers shared by the parts of Session; each part includes this header only.
namespace voyemu::session_detail {
// Sets a value for the rest of the scope and restores the previous one when the scope ends.
template <typename T> class Restore {
public:
    Restore(T &slot, T value) : slot_(slot), previous_(slot) {
        slot_ = std::move(value);
    }

    ~Restore() {
        slot_ = previous_;
    }

    Restore(const Restore &) = delete;
    Restore &operator=(const Restore &) = delete;

private:
    T &slot_;
    T previous_;
};

// The earlier of an Android request deadline and candidate; an unset deadline takes candidate.
inline std::chrono::steady_clock::time_point
tighter_deadline(std::chrono::steady_clock::time_point current,
                 std::chrono::steady_clock::time_point candidate) {
    return current == std::chrono::steady_clock::time_point{} ? candidate
                                                              : std::min(current, candidate);
}

int cluster_ui_timeout(const Json &operation);
// A status carries the vehicle state and the field catalogue of the machine, up to the 1598 fields
// of Passion.
inline constexpr std::size_t maximum_message = 1024 * 1024;
std::size_t response_limit(const Json &operation);
std::filesystem::path session_sdk_registration_directory(const std::filesystem::path &directory,
                                                         bool diagnostic_native);
using ClusterDeadline = std::chrono::steady_clock::time_point;
int cluster_remaining(ClusterDeadline deadline);
std::string bounded_message(const Json &value, std::size_t limit = maximum_message);
// The deadline of one control request; a cluster observation spends one absolute deadline across
// its transport steps.
struct ControlTimeout {
    bool cluster = false;
    ClusterDeadline deadline;
    int timeout_ms = 0;

    int remaining() const {
        return cluster ? cluster_remaining(deadline) : timeout_ms;
    }

    Json parse(const std::string &bytes) const;
};
// One request and its reply over the control endpoint of the host: a Unix-domain socket on macOS
// and Linux, a named pipe on Windows.
Json control_exchange(const std::filesystem::path &endpoint, const std::string &text,
                      const Json &operation, const ControlTimeout &timeout);
// The control endpoint of a session: a private Unix-domain socket, a named pipe on Windows.
std::filesystem::path session_control_endpoint(const std::filesystem::path &directory,
                                               const std::string &session_id);
// The directory of ${RUNTIME} in launch paths; IPC_PATH on Windows, which has no runtime sockets.
std::filesystem::path session_runtime_root();
// The ANGLE libraries that a graphics launch must pin.
extern const std::array<const char *, 2> angle_libraries;
// Platform parts of Android and QNX storage and of the stop budgets; session_posix.cpp and
// session_windows.cpp define them.
bool android_windows_renderer(const Json &specification, bool diagnostic_native);
int android_stop_sync_budget();
void check_restart_can_boundary(bool with_can);
void require_short_pair_transport();
std::chrono::milliseconds source_collector_budget(const std::string &id);
// The path and AVD configuration as the Android Emulator sees them: unchanged, or through the Wine
// mapping of the native diagnostic backend on Windows.
std::string native_backend_path(const std::filesystem::path &path, bool diagnostic_native);
std::string native_backend_avd_config(std::string text, bool diagnostic_native,
                                      const std::filesystem::path &base);
bool native_backend_same_file(const std::filesystem::path &a, const std::filesystem::path &b);
bool owned_symlink(const std::filesystem::path &alias);
void link_overlay(const std::filesystem::path &disk, const std::filesystem::path &alias);
// Reads the owned QNX RAM file at an offset; empty when the file is not a private regular file of
// one link and the expected size.
using OwnedRamReader = std::function<bool(std::uint64_t offset, std::span<std::byte> output)>;
OwnedRamReader owned_ram_reader(const std::filesystem::path &ram, std::uint64_t bytes);
void write_handoff_file(const std::filesystem::path &path, const std::string &text);
// The owned QNX RAM backing of a finished session, claimed before the handoff and removed after
// it; QNX_CLEANUP_RAM for a foreign or invalid backing.
class OwnedRamBacking {
public:
    OwnedRamBacking(const std::filesystem::path &path, bool exists, std::uint64_t bytes);
    ~OwnedRamBacking();
    OwnedRamBacking(const OwnedRamBacking &) = delete;
    OwnedRamBacking &operator=(const OwnedRamBacking &) = delete;
    void remove();

private:
    struct Handle;
    std::filesystem::path path_;
    std::unique_ptr<Handle> handle_;
};
std::tm local_time(std::time_t time);
std::string replace_all(std::string value, const std::string &from, const std::string &to);
void validate_pins(const Json &specification, bool machine_start = false);
ProcessSpec launch_spec(const Json &specification, const std::filesystem::path &directory,
                        const std::string &token);
struct Route {
    const char *id;
    int index;
};
inline constexpr std::array<Route, 17> physical_routes{{{"ica", 0},
                                                        {"night_vision", 329},
                                                        {"acc", 0},
                                                        {"distance", 0},
                                                        {"cruise_up", 0},
                                                        {"cruise_down", 0},
                                                        {"custom", 0},
                                                        {"cancel", 0},
                                                        {"menu", 323},
                                                        {"voice", 331},
                                                        {"volume_up", 325},
                                                        {"ok", 324},
                                                        {"volume_down", 326},
                                                        {"previous", 327},
                                                        {"next", 328},
                                                        {"photo", 322},
                                                        {"phone", 330}}};
int route_index(const std::string &id);
void owner_valid(const std::string &owner);
std::uint64_t u64(const Json &value);
void terminal_path_guard(const std::filesystem::path &path);
std::filesystem::path qnx_project(const std::filesystem::path &directory,
                                  const std::string &session_id);
void exclusive_handoff(const std::filesystem::path &path, const std::string &text);
Json terminal_frame_evidence(const std::filesystem::path &directory);
Json terminal_native_acks(const std::filesystem::path &directory);
void android_regular_input(const std::filesystem::path &path);
Json host_lifecycle_receipt();
void trace_session_stop(std::string_view session, std::string_view phase) noexcept;
std::filesystem::path
prepare_android_avd_impl(const std::filesystem::path &home, const std::filesystem::path &machine,
                         const std::string &avd, const std::filesystem::path &source,
                         const std::string &config_sha256, const std::filesystem::path &base,
                         bool machine_start, unsigned api = 28, bool diagnostic_native = false,
                         const std::filesystem::path &runtime = {}, const Json &binding = Json(),
                         const std::filesystem::path &previous_machine = {});
void verify_android_avd_storage_impl(const std::filesystem::path &home,
                                     const std::filesystem::path &machine, const std::string &avd,
                                     bool machine_start, bool diagnostic_native = false,
                                     const Json &specification = Json());
} // namespace voyemu::session_detail
