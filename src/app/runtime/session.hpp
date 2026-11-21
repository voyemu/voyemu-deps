#pragma once
#include "adas_ecu.hpp"
#include "android_adb.hpp"
#include "android_identity_probe.hpp"
#include "android_port_lease.hpp"
#include "body_ecu.hpp"
#include "media_discovery.hpp"
#include "model.hpp"
#include "power_ecu.hpp"
#include "process.hpp"
#include "qinggan09_canbus_startup.hpp"
#include "qnx_serial_lines.hpp"
#include "qnx_serial_ready.hpp"
#include "source_registry.hpp"
#include "startup_ignition.hpp"
#include "windows_startup.hpp"
#ifdef _WIN32
#include "android_embedded_adb.hpp"
#endif
#include "interguest_link.hpp"
#include "qnx_checkpoint.hpp"
#include "qnx_ready_collector.hpp"
#include "quickboot.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>

namespace voyemu {
namespace media {
class PassionPowerOwner;
}
namespace frames {
class GrpcServer;
}
class QnxShutdownMonitor;
class AndroidEmbeddedAdb;
class AndroidSdkAdbSink;
bool android_nonce_matches(std::string_view observed, std::string_view session_id,
                           std::string_view retained_nonce, bool resumed);
std::vector<std::string> qnx_startup_markers(std::string_view console, std::set<std::string> &seen);
// The longest timeout of a control request; one that closes a cluster also covers its guest sync.
int request_timeout_limit_ms(std::string_view op);
Json request(const std::filesystem::path &endpoint, const Json &operation, int timeout_ms = 2000);
std::string expand_session_path(std::string value, const std::filesystem::path &directory,
                                const std::string &token);
#ifndef _WIN32
std::filesystem::path native_socket_root();
#endif
// Start identity of a launched SDK for the supervisor receipt; empty where the launch handle alone
// identifies the SDK.
Json android_launch_identity(std::int64_t pid);
#ifndef _WIN32
// Kernel start, executable and command identity of a launched session process.
Json session_process_identity(std::int64_t pid);
void record_session_process(const std::filesystem::path &directory, const std::string &session_id,
                            std::int64_t pid, std::uint64_t nonce = 0,
                            const std::string &launch_intent = {});
// Write-ahead launch intent fences an interrupted spawn until its exact identity is registered.
ChildProcess launch_session_process(const ProcessSpec &spec, const std::filesystem::path &directory,
                                    const std::string &session_id, std::uint64_t nonce = 0);
// After guest and native listener authentication, register only the launcher's exact backend exec.
void refresh_android_launch_identity(const std::filesystem::path &directory,
                                     const std::string &session_id, std::int64_t pid,
                                     std::uint64_t generation,
                                     const std::filesystem::path &launcher,
                                     const std::filesystem::path &backend);
void advance_android_launch_generation(const std::filesystem::path &directory,
                                       const std::string &session_id, std::int64_t pid,
                                       std::uint64_t generation);
// Completion fence after the supervisor released its session: no signal or descendant discovery.
void await_session_resources(const std::filesystem::path &directory,
                             std::chrono::steady_clock::time_point deadline);
// Record the bound control socket while its supervisor holds the exclusive session lease.
void record_session_endpoint(const std::filesystem::path &directory, const std::string &session_id,
                             const std::filesystem::path &endpoint);
// The regular stop of a session whose supervisor exited, under its launch and supervisor locks.
// Signals only exact recorded process identities, verifies RAM is unused and retires its socket.
Json stop_orphaned_session(const std::filesystem::path &directory, const std::string &session_id,
                           const std::string &endpoint);
#endif
class ControlServer {
public:
    using Handler = std::function<Json(const Json &)>;
    using Tick = std::function<void()>;
    ControlServer(std::filesystem::path endpoint, Handler handler, Tick tick = {});
    ~ControlServer();
    ControlServer(const ControlServer &) = delete;
    void serve(std::atomic<bool> &stop);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct WheelEvent {
    std::uint32_t sequence;
    std::string id, phase, owner;
    int qnx_index;
    int value;
    std::chrono::steady_clock::time_point accepted_at = std::chrono::steady_clock::now();
};
class EventQueue {
public:
    WheelEvent submit(const std::string &id, const std::string &phase, const std::string &owner);
    void release(const std::string &owner);
    void release_all();
    WheelEvent force_release(const std::string &id, const std::string &owner);
    void renew(const std::string &owner);
    void expire();
    std::optional<WheelEvent> pop();

    std::size_t held() const {
        return held_.size();
    }

    static Json routes();

private:
    struct Held {
        std::string owner;
        std::chrono::steady_clock::time_point renewed;
    };
    std::map<std::string, Held> held_;
    std::deque<WheelEvent> queue_;
    std::uint32_t sequence_ = 0;
};
struct ThemeState {
    int mode, skin;
    bool operator==(const ThemeState &) const = default;
};
int binder_integer(const std::string &reply);
Json parse_android_provider_reply(const std::string &reply);
std::optional<unsigned> secondary_display_ready(const std::string &displays,
                                                const std::string &windows);
std::optional<unsigned> passion_secondary_display_ready(const std::string &displays,
                                                        const std::string &windows);
bool passion_secondary_provider_ready(const std::string &displays);
bool passion_secondary_provider_preserves_readiness(const std::string &displays,
                                                    const std::string &established_boot,
                                                    const std::string &current_boot);
Json passion_display_readiness_diagnostic(const std::string &displays,
                                          const std::string &established_boot,
                                          const std::string &current_boot);
bool oem_status_bars_ready(std::string_view windows, unsigned secondary_display_id);
bool multimedia_canvas_ready(const Json &record);
void prepare_android_emulator_home(const std::filesystem::path &project,
                                   const std::filesystem::path &home);
// Seed the selected SDK image version independently of RAM snapshots; preserves existing QCOW data.
void prepare_android_avd_version_marker(const std::filesystem::path &content,
                                        const std::filesystem::path &build_properties,
                                        bool required);
void normalize_android_disk_backings(const std::filesystem::path &machine,
                                     const std::filesystem::path &qemu_img,
                                     const Json &system_binding = Json());
std::filesystem::path
prepare_android_avd(const std::filesystem::path &home, const std::filesystem::path &machine,
                    const std::string &avd, const std::filesystem::path &source,
                    const std::string &config_sha256, const std::filesystem::path &base = {},
                    unsigned api = 28, const Json &system_binding = Json(),
                    const std::filesystem::path &previous_machine = {});
void verify_android_avd_storage(const std::filesystem::path &home,
                                const std::filesystem::path &machine, const std::string &avd);
std::optional<std::string> resolved_home_component(std::string_view response,
                                                   std::string_view package);
bool home_component_declared(std::string_view candidates, std::string_view component,
                             std::string_view package);
bool primary_home_visible(std::string_view windows, std::string_view component);
std::vector<std::string> primary_home_start_command(const std::string &component);
Json home_start_result(const ProcessResult &result, const std::vector<std::string> &secrets = {});
void record_home_start(Json &android, const ProcessResult &result,
                       const std::vector<std::string> &secrets = {});
void clear_android_home_delivery(Json &android);
void clear_android_camera_delivery(Json &android);
bool android_vehicle_receipt_matches(const Json &receipt, const Json &state);
bool android_vehicle_receipt_pending(const Json &receipt, const Json &state);
bool android_wheel_release_reuses_identity(const WheelEvent &down, const WheelEvent &release);
enum class AndroidShortWheelContract {
    qinggan11_731,
    a9
};
std::string android_short_wheel_pair_command(
        const std::string &session, const std::string &token, const std::string &boot,
        const WheelEvent &down, const WheelEvent &up, int hold_ms,
        AndroidShortWheelContract contract = AndroidShortWheelContract::qinggan11_731,
        std::string_view nonce = {});
std::optional<Json> android_wheel_delivery_receipt(const Json &receipt,
                                                   const std::string &session_id,
                                                   const WheelEvent &event);
std::optional<Json> theme_change(std::optional<ThemeState> previous, ThemeState current);
std::optional<ThemeState> qinggan11_cluster_skin_theme(std::string_view reply);
std::optional<Json> qinggan11_theme_change(std::optional<ThemeState> previous, ThemeState current);
// The adb arguments of the Binder queries that read the OEM theme of this Android, in order.
std::vector<std::vector<std::string>> oem_theme_queries(const Json &android);
// The OEM theme from the replies to oem_theme_queries; nullopt for an unknown cluster skin.
std::optional<ThemeState> oem_theme_state(const Json &android,
                                          const std::vector<std::string> &replies);
// The cluster theme command for the observed OEM theme; nullopt when it is unchanged.
std::optional<Json> oem_theme_command(const Json &android, std::optional<ThemeState> previous,
                                      ThemeState current);
std::optional<bool> host_light_theme(std::string_view oem_title);
std::optional<unsigned> oem_language_setting(std::string_view query);
std::optional<Json> oem_language_change(std::optional<unsigned> previous, unsigned current);
// Owned QNX language command emitted when the OEM language setting changes.
std::optional<Json> oem_language_command(std::optional<unsigned> previous, unsigned current);
std::optional<Json> find_qnx_theme_receipt(const std::filesystem::path &directory,
                                           const std::vector<std::uint64_t> &producers,
                                           std::uint32_t sequence, unsigned theme,
                                           unsigned skin = 0);
// Genuine guest receipt for an owned language command; requires the exact sequence and observed
// language.
std::optional<Json> find_qnx_language_receipt(const std::filesystem::path &directory,
                                              const std::vector<std::uint64_t> &producers,
                                              std::uint32_t sequence, unsigned language);
// The producer that owns the original MCU input in one cluster generation.
struct QnxVehicleReceiver {
    std::uint64_t producer_address = 0, generation = 0, worker_nonce = 0, guest_pid = 0,
                  producer_incarnation = 0;
};
// Genuine vehicle receipt of the current receiver for the published MCU sequence; a receipt of
// another producer, worker, guest process, incarnation or generation is never accepted.
std::optional<Json> qnx_vehicle_receipt(const Json &ack, const QnxVehicleReceiver &receiver,
                                        std::uint32_t sequence);
// Terminal QNX-only cleanup; rejects foreign paths, live children and unrecorded RAM.
Json finalize_owned_qnx_session(const std::filesystem::path &directory,
                                const std::string &session_id, const std::filesystem::path &ram,
                                const Json &evidence);
class UiObserver;
class QnxPersistentStorage;
namespace runtime {
class ProviderRegistry;
}
// Shared guards gate startup; vehicle and display failures remain independent.
Json run_android_startup_stages(const std::function<void()> &guard,
                                const std::function<void()> &vehicle,
                                const std::function<void()> &display);
bool observe_android_boot(std::string &retained, const std::string &observed);
struct AndroidNetworkArtifact {
    std::string path, inode, sha256, source_bytes;
    bool execution_started = false;
};
void configure_owned_android_network(
        const std::string &boot, const std::string &dex, const std::string &pin,
        std::string_view payload, const Json &inputs, Json &android,
        std::vector<AndroidNetworkArtifact> &pending,
        const std::function<ProcessResult(const std::vector<std::string> &, int)> &run,
        const std::function<std::string()> &nonce);
void validate_app_launch(const std::string &package, const std::string &screen,
                         const std::string &boot_id);
Json launch_android_app(
        const std::string &package, const std::string &screen, const std::string &boot_id,
        const std::string &component, unsigned display_id,
        const std::function<std::string()> &fenced_boot,
        const std::function<ProcessResult(const std::vector<std::string> &, int)> &run);
void bootstrap_owned_adbd(
        const std::function<ProcessResult(const std::vector<std::string> &, int)> &run,
        const std::function<bool()> &owned, const std::function<void(int)> &identity,
        int timeout_ms);
class Session {
public:
    // A directory without a vehicle state starts from the initial state of catalog.
    Session(std::filesystem::path directory, std::string session_id,
            std::filesystem::path canonical_state = {},
            VehicleCatalog catalog = VehicleCatalog::qinggan09);
    // Ordinary machine consumption reads configuration; explicit preparation/check
    // remains responsible for SHA auditing. Existing Session callers stay audited.
    static std::unique_ptr<Session> for_machine_start(std::filesystem::path directory,
                                                      std::string session_id,
                                                      std::filesystem::path current_program = {});
    ~Session();
    Json handle(const Json &operation);
    void set_test_providers(const Json &manifests, const std::string &profile);
    void set_media_input(std::function<Json(const Json &)> handler);
    void tick();
    void tick_inputs();
    // Bounded physical reads from this Session's retained live Windows QEMU RAM capability.
    bool read_owned_qnx_ram(std::uint64_t physical, std::span<std::byte> output);
    void attach_android(const Json &specification, bool explicit_cold_boot = false);
    void configure_cluster(const Json &specification,
                           std::optional<unsigned> seconds = std::nullopt);
    void attach_cluster(const Json &specification, std::optional<unsigned> seconds = std::nullopt);
    void attach_frontend(const Json &specification, bool cluster_panel, bool background = false);
    void set_ui(const std::filesystem::path &executable, const std::filesystem::path &assets,
                std::string profile = {});
    bool stopped() const;

    const std::filesystem::path &endpoint() const {
        return endpoint_;
    }

    void stop();
    // stop() for a source acceptance cluster, whose images are disposable: the owned QEMU and
    // graphics end without the guest sync and the owned RAM is removed. Other sessions keep the
    // guest sync of stop().
    void discard();

private:
    friend struct SessionPersistenceFixture;
    friend struct PrivateLanguageAcceptanceFixture;
#ifdef _WIN32
    friend struct SessionRequestIoFixture;
    friend struct SessionInterguestSnapshotFixture;
#endif
    struct MachineStart {};
    Session(std::filesystem::path directory, std::string session_id, MachineStart);
    bool machine_start_ = false;
    std::filesystem::path machine_program_;
    Json snapshot() const;
    void ensure_test_services();
    std::shared_ptr<UiObserver> test_ui_observer_locked();
    voyemu::runtime::SourceSample collect_test_ui(const voyemu::runtime::SourceContext &context);
    std::shared_ptr<voyemu::runtime::ProviderRegistry> test_providers_;
    std::string test_provider_profile_;
    voyemu::runtime::SourceSample
    collect_test_provider(const std::string &id, const voyemu::runtime::SourceContext &context);
    voyemu::runtime::SourceSample
    collect_test_source(bool can, const voyemu::runtime::SourceContext &context);
    std::shared_ptr<voyemu::runtime::WatchHub> test_watch_, test_can_watch_;
    std::map<std::vector<std::string>, std::shared_ptr<voyemu::runtime::WatchHub>>
            test_source_watches_;
    std::shared_ptr<voyemu::runtime::SourceCoordinator> test_sources_;
    std::atomic<std::int64_t> test_demand_until_{0};
    bool test_services_closed_ = false, test_stop_requested_ = false,
         machine_restart_active_ = false;
    Json test_can_cursor_ = nullptr;
    std::uint64_t test_can_generation_ = 0;
    std::mutex test_stop_mutex_;
    void apply_adas_outcome(AdasOutcome outcome);
    void apply_startup_ignition(const std::optional<StartupIgnition::Change> &change);
    void apply_power_outcome(PowerEcu::Outcome outcome);
    // The am startservice extra that carries the multimedia ACC of the power state.
    std::vector<std::string> android_power_extra() const;
    void assert_android_identity();
    void stop_android(std::unique_lock<std::recursive_mutex> &state, bool save_checkpoint = true);
    Json reboot_android(const std::string &mode);
    Json restart_machine(const Json &operation);
    std::deque<Json> can_history_anchors_;
    bool active_can_boundary_ = false;
    ProcessResult adb(const std::vector<std::string> &arguments, int timeout_ms = 10000);
    ProcessResult execute_android_adb(const Json &, const std::vector<std::string> &, int,
                                      std::size_t, std::int64_t, std::uint64_t);
    std::shared_ptr<AndroidEmbeddedAdb> owned_android_adb(const Json &, std::int64_t, std::uint64_t,
                                                          bool require_backend = true);
    void stop_owned_android_adb();
    void record_android_processes(bool launch_pending);
    void bootstrap_android_transport();
    void prepare_android_audio(bool snapshot = false);
    void prepare_android_navigation_bus();
    Qinggan09CanBusStartup android_navigation_bus_startup_;
    void prepare_android_canbus();
    Qinggan09CanBusStartup android_canbus_startup_;
    void configure_android_interguest_network();
    void synchronize_android_clock();
    void prepare_android_displays();
    bool ensure_android_home();
    void deliver_android_state();
    void schedule_android_state(std::uint32_t previous_revision);
    void deliver_android_event(const WheelEvent &event, bool retained_identity = false);
    void prepare_android_wheel();
    void deliver_android_short_pair(const WheelEvent &down, const WheelEvent &up, int hold_ms,
                                    bool &submitted);
    Json phone_control(const Json &operation);
    std::string phone_control_boot_;
    std::uint64_t phone_control_sequence_ = 0;
    std::int64_t phone_provider_pid_ = 0;
    bool phone_control_unknown_ = false;
    Json wheel_provider(const std::string &method, const std::vector<std::string> &extras = {},
                        const std::string &argument = {});
    void poll_oem_settings();
    // Queues the cluster language command with the ivi_languageSet of the model when it differs
    // from the last queued value, so the command and the MCU word carry one value.
    void queue_cluster_language();
    // Records the observed OEM theme and queues the cluster theme command when it changed.
    void observe_oem_theme(ThemeState current);
    void poll_qnx();
    Json cluster_ui_sample(bool diagnostics, std::chrono::steady_clock::time_point deadline);
    void attach_cluster(const Json &specification, std::optional<unsigned> seconds,
                        bool explicit_cold_boot);
    bool fallback_cluster(std::string_view reason);
    void start_qnx_frame_grpc();
    void close_qnx_frame_grpc();
    std::unique_ptr<frames::GrpcServer> qnx_frame_grpc_;
    void publish_qnx_input();
    void poll_frontend();
    void stop_frontend();
    void set_qnx_persistence_paused(bool paused);
    bool qnx_persistence_paused_ = false;
    void close_cluster(bool stopping_all = false, std::string reason = "ui_request");
    void archive_cluster_generation();
    void flush_android_releases(bool android_stopping);
    std::filesystem::path directory_, endpoint_, android_home_;
    std::string session_id_;
    std::string guest_auth_token_;
    std::string android_boot_nonce_;
    std::unique_ptr<AndroidQuickBoot> android_quickboot_;
    std::shared_ptr<UiObserver> ui_observer_;
    Json test_ui_connection_, test_ui_subscription_;
    voyemu::runtime::SourceSample test_ui_sample_;
    std::int64_t test_ui_sampled_ms_ = 0;
    bool android_resume_ = false, android_powered_ = false;
    std::uint64_t android_generation_ = 0;
    VehicleModel model_;
    EventQueue events_;
    AdasEcu adas_;
    StartupIgnition startup_ignition_;
    PowerEcu power_;
    // A cluster reset keeps the vehicle ignition on, so the next attach plays no startup sequence.
    bool cluster_reset_ = false;
    BodyEcu body_;
    Json deliveries_, android_spec_, cluster_spec_, frontend_spec_, runtime_record_;
    std::function<Json(const Json &)> media_input_;
    std::unique_ptr<media::PassionPowerOwner> passion_display_power_;
    std::string passion_display_power_boot_;
    std::chrono::steady_clock::time_point next_passion_display_power_{};
    void poll_passion_display_power();
    bool close_passion_display_power();
    QnxCheckpoint qnx_checkpoint_;
    std::unique_ptr<QnxPersistentStorage> qnx_persistent_storage_;
    Json cluster_boot_spec_;
    std::unique_ptr<InterguestLink> interguest_link_;
    std::uint64_t interguest_qnx_generation_ = 0;
    // Vehicle state sequence last given to the gateway; 0 when none is attached or fed yet.
    std::uint64_t someip_sequence_ = 0;
    bool qnx_checkpoint_disabled_ = false, qnx_checkpoint_ready_ = false;
    std::set<std::string> startup_markers_;
    QnxSerialLines qnx_serial_;
    QnxSerialReady qnx_serial_ready_;
    bool primary_home_ready_ = false;
    // Declared before android_ so that the lease outlives the SDK child it covers.
    std::unique_ptr<AndroidPortLease> android_port_lease_;
    ChildProcess android_, qnx_, ui_, frontend_;
    std::shared_ptr<AndroidEmbeddedAdb> android_adb_;
    std::unique_ptr<AndroidSdkAdbSink> android_adb_sink_;
    unsigned android_adb_console_port_ = 0;
#ifdef _WIN32
    class RetainedAndroidAction {
    public:
        using Clock = std::chrono::steady_clock;
        struct Arrival {
            std::chrono::system_clock::time_point utc;
            Clock::time_point steady;
            bool before_terminal;
        };
        struct OutputObservation {
            std::size_t bytes{};
            std::string sha256;
            bool sampled = false, complete = false, changed = false;
        };

        bool pending() const {
            return pending_;
        }

        bool eligible(Clock::time_point now = Clock::now()) const {
            return !pending_ && now >= retry_;
        }

        const WindowsAndroidOwner &owner() const {
            return owner_;
        }

        const std::optional<Arrival> &arrival() const {
            return arrival_;
        }

        const OutputObservation &output_observation() const {
            return observation_;
        }

        void cancel();
        void launch(const WindowsAndroidOwner &, AndroidEmbeddedAdb::Stream, Clock::time_point,
                    std::chrono::milliseconds, std::size_t);
        std::optional<ProcessResult>
        poll(const WindowsAndroidOwner &, Clock::time_point now = Clock::now(),
             const std::function<bool(const std::string &)> &complete = {},
             const std::function<void(const std::string &)> &observe = {});

    private:
        AndroidEmbeddedAdb::Stream stream_;
        WindowsAndroidOwner owner_;
        Clock::time_point deadline_{}, retry_{};
        std::optional<Arrival> arrival_;
        OutputObservation observation_;
        std::string output_;
        std::size_t limit_{};
        bool pending_ = false;
    } android_identity_probe_;
    WindowsAndroidTransaction windows_android_;
    void reset_windows_android_transaction();
    void observe_windows_android_boot_change(const std::string &boot);
    void guard_windows_android_transaction();
    void advance_windows_android_transaction();
    void retire_windows_wheel_pair_for_flush();
    void invalidate_windows_android_readiness();
    std::optional<ProcessResult>
    poll_windows_android_action(const std::vector<std::string> &arguments, bool mutation = false,
                                const std::string &attempt = {});
    ProcessResult execute_windows_android_operation(const Json &specification,
                                                    const std::vector<std::string> &arguments,
                                                    int timeout_ms, std::size_t limit,
                                                    std::int64_t expected_pid,
                                                    std::uint64_t expected_generation);
    void assert_windows_android_identity(std::chrono::steady_clock::time_point deadline = {});
#endif
    // The request deadline and identity proof epochs of Android I/O; only the framed Windows
    // transaction reads them, the SDK ADB endpoints of macOS and Linux bound each call alone.
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization): time_point{} is constexpr.
    inline static thread_local std::chrono::steady_clock::time_point android_request_deadline_{};
    unsigned android_identity_scope_depth_ = 0;
    std::uint64_t android_identity_operation_ = 0, android_identity_proof_operation_ = 0;
    bool android_identity_stopping_ = false, android_identity_mismatch_ = false;
    // Lends the state lock to the Android I/O of one request until the loan ends; an empty loan
    // where the platform bounds each Android call alone.
    class IoLockLoan {
    public:
        IoLockLoan() = default;

        IoLockLoan(std::unique_lock<std::recursive_mutex> *&slot,
                   std::unique_lock<std::recursive_mutex> &lock)
            : slot_(&slot), previous_(std::exchange(slot, &lock)) {}

        ~IoLockLoan() {
            if (slot_) {
                *slot_ = previous_;
            }
        }

        IoLockLoan(const IoLockLoan &) = delete;
        IoLockLoan &operator=(const IoLockLoan &) = delete;

    private:
        std::unique_lock<std::recursive_mutex> **slot_ = nullptr;
        std::unique_lock<std::recursive_mutex> *previous_ = nullptr;
    };
    // Platform strategy of the Android guest: session_posix.cpp drives it through the SDK ADB
    // endpoints, session_windows.cpp through framed transactions of the embedded ADB.
    void advance_android(bool guest_possible, std::chrono::steady_clock::time_point now);
    [[nodiscard]] IoLockLoan lend_android_request_io(std::unique_lock<std::recursive_mutex> &lock);
    void cancel_android_identity_probe();
    void add_interguest_delivery(Json &android) const;
    void reset_android_transaction();
    void reset_android_identity_for_new_boot();
    void retire_android_transaction_for_flush(bool android_stopping);
    void configure_android_native_backend(ProcessSpec &spec, const Json &specification,
                                          unsigned port, std::uint64_t generation,
                                          bool audio_output);
    std::optional<ProcessResult> execute_android_transaction(
            const Json &specification, const std::vector<std::string> &arguments, int &timeout_ms,
            std::size_t limit, std::int64_t expected_pid, std::uint64_t expected_generation);
    void assert_android_identity_until(std::chrono::steady_clock::time_point deadline);
    std::optional<std::string> android_boot_id();
    void prevalidate_android_reboot();
    std::optional<std::string> await_android_reboot(std::unique_lock<std::recursive_mutex> &lock,
                                                    const std::string &before,
                                                    const std::function<std::string()> &boot);
    bool android_ports_released_after_kill(unsigned port, bool wrapper_alive,
                                           const std::function<bool()> &released);
    bool android_ports_released_at_stop(unsigned port, const std::function<bool()> &released);
    int android_provider_budget() const;
    void bound_android_request(const Json &operation,
                               std::chrono::steady_clock::time_point received);
    void prepare_android_media_input();
    void require_unborrowed_state_lock() const;
    Json observe_can_boundary(const std::function<Json(const std::string &)> &unavailable,
                              std::unique_lock<std::recursive_mutex> &lock,
                              std::chrono::steady_clock::time_point received, int timeout);
    bool android_host_canvas_ready(const std::function<void()> &pending);
    void request_frontend_stop();
    // Platform strategy of the QNX guest: QMP and the shutdown event monitor over Unix-domain
    // sockets on macOS and Linux, over loopback TCP on Windows.
    void configure_qnx_event_monitor(ProcessSpec &qemu, const std::string &ram_backend);
    bool continue_qnx_guest();
    // macOS and Linux also sample the RAM console ring between ticks on a bounded reader thread.
    void start_qnx_ring_collector(const std::filesystem::path &ram, std::uint64_t address,
                                  std::size_t extent, std::uint64_t mailbox_end);
    void add_retained_qnx_producers(
            std::vector<std::uint64_t> &ready_producers,
            std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> &incarnations,
            const std::function<void(std::uint64_t, const char *)> &origin);
    void configure_graphics_environment(ProcessSpec &spec, const Json &graphics);
    void stop_qnx_ready_collector();
    void remove_qmp_socket(Json &evidence);
#ifndef _WIN32
    std::unique_ptr<QnxReadyCollector> qnx_ready_collector_;
#endif
    std::unique_ptr<QnxShutdownMonitor> qnx_shutdown_monitor_;
#ifdef _WIN32
    unsigned qnx_events_port_ = 0;
#endif
    bool media_connection_written_ = false, frontend_releases_verified_ = true;
    SdkDescriptorCache sdk_descriptor_;
    std::vector<ChildProcess> graphics_;
    std::vector<std::uint64_t> producers_;
    struct GraphicsOwner {
        std::uint64_t nonce = 0, pid = 0, incarnation = 0;
    };
    std::vector<GraphicsOwner> graphics_owners_;
    std::set<std::pair<std::uint64_t, std::uint64_t>> retired_producers_;
    Json retired_graphics_ = Json::array();
    std::uint64_t graphics_owner_challenge_ = 0;
    void publish_graphics_owners();
    void retire_dead_graphics(
            const std::function<std::size_t(std::uint64_t, std::span<std::byte>)> &read_ram);
    void retire_graphics_worker(std::size_t index, bool unclean,
                                const char *reason = "rpc_channel_invalidated",
                                bool republish = true);
    // The MCU receiver retired in this generation without a successor, and the last vehicle
    // receipt of the generation; both are null when absent.
    Json qnx_retired_receiver_, qnx_vehicle_acknowledged_;
    Json qnx_vehicle_pending() const;
    void observe_qnx_vehicle();
    std::deque<WheelEvent> qnx_events_;
    std::deque<WheelEvent> android_events_;
    std::map<std::string, std::string> android_possible_pressed_;
    std::map<std::uint32_t, std::pair<int, bool>> android_short_pairs_;
    std::optional<WheelEvent> issued_;
    std::optional<ThemeState> theme_;
    std::optional<unsigned> oem_language_, cluster_language_;
    std::filesystem::path ui_executable_, assets_;
    std::string ui_profile_;
    std::chrono::steady_clock::time_point next_android_, next_oem_settings_,
            next_display_activation_, next_qnx_, qnx_deadline_, qnx_started_;
    std::chrono::steady_clock::time_point android_started_;
    std::uint32_t android_revision_ = 0, theme_sequence_ = 0, language_sequence_ = 0,
                  android_input_sequence_ = 0;
    std::optional<unsigned> cluster_seconds_;
    std::uint64_t cluster_generation_ = 0;
    bool android_identity_ = false, qmp_cont_ = false, stopped_ = false, cluster_requested_ = false;
    bool wheel_provisioned_ = false;
    bool android_clock_observed_ = false, android_clock_correction_attempted_ = false;
    int android_clock_exit_code_ = -1;
    std::int64_t android_clock_guest_seconds_ = 0, android_clock_host_ms_ = 0;
    std::chrono::steady_clock::time_point android_clock_arrival_{};
    std::string android_transport_boot_;
    std::vector<AndroidNetworkArtifact> android_network_artifacts_;
    bool android_transport_ready_ = false;
    bool android_transport_attempted_ = false;
    bool android_seen_guest_ = false;
    // Per owned Android launch epoch; readiness is re-observed by a bounded watchdog.
    bool android_boot_completed_ = false;
    std::chrono::steady_clock::time_point next_display_observation_{};
    bool android_displays_ready_ = false;
    bool android_canvas_refreshed_ = false;
    bool android_display_activated_ = false;
    // Configuring controls creates no QEMU lifetime; attach_cluster opens the cleanup obligation.
    bool cluster_finalized_ = true;
    // Set by discard(): close_cluster skips the guest sync of disposable images.
    bool discard_cluster_ = false;
    // Lifecycle stays serialized while tick lends the state lock during bounded external I/O.
    std::recursive_mutex lifecycle_mutex_;
#ifdef _WIN32
    inline static thread_local std::unique_lock<std::recursive_mutex> *android_io_state_lock_ =
            nullptr;
#else
    std::unique_lock<std::recursive_mutex> *android_io_state_lock_ = nullptr;
#endif
    mutable std::recursive_mutex mutex_;
};
} // namespace voyemu
