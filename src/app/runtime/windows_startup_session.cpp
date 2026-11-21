#include "android_adb_route.hpp"
#include "android_clock.hpp"
#include "can_observation.hpp"
#include "error.hpp"
#include "logging.hpp"
#include "profile_catalog.hpp"
#include "qinggan09_canbus_startup.hpp"
#include "qinggan11_startup.hpp"
#include "session.hpp"
#include "startup_frame_metadata.hpp"
#include "ui_observer.hpp"
#include "windows_wheel_pair.hpp"
#include <algorithm>
#include <sstream>
#include <thread>
namespace voyemu {
namespace {
using Phase = WindowsStartupPhase;

std::string startup_line(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }

    return text;
}

std::string startup_shell(const std::vector<std::string> &arguments) {
    std::string text;

    for (const auto &argument : arguments) {
        if (!text.empty()) {
            text += ' ';
        }

        text += android_shell_quote(argument);
    }

    return text;
}

std::optional<Phase> startup_post_write(Phase phase) {
    switch (phase) {
    case Phase::event_submit:
        return Phase::event_verify;
    case Phase::root_submit:
        return Phase::root_verify;
    case Phase::clock_set:
        return Phase::clock_verify;
    case Phase::wheel_bootstrap:
        return Phase::wheel_verify;
    case Phase::state_submit:
        return Phase::state_verify;
    case Phase::home_set:
        return Phase::home_verify;
    case Phase::home_start:
        return Phase::home_windows;
    case Phase::secondary_wake:
    case Phase::secondary_service:
    case Phase::systemui_service:
        return Phase::display_read;
    default:
        return std::nullopt;
    }
}
} // namespace

void Session::RetainedAndroidAction::cancel() {
    stream_.cancel();
    owner_ = {};
    deadline_ = {};
    retry_ = {};
    arrival_.reset();
    observation_ = {};
    output_.clear();
    pending_ = false;
}

void Session::RetainedAndroidAction::launch(const WindowsAndroidOwner &owner,
                                            AndroidEmbeddedAdb::Stream stream,
                                            Clock::time_point now, std::chrono::milliseconds budget,
                                            std::size_t limit) {
    if (!eligible(now) || owner.epoch.empty() || owner.generation == 0 || owner.sdk_pid < 2 ||
        budget < std::chrono::milliseconds(1) || budget > std::chrono::seconds(120) || !limit ||
        limit > 512 * 1024) {
        throw VoyemuError("ANDROID_IDENTITY_PROBE", "bounded owned stream required");
    }

    stream_ = std::move(stream);
    owner_ = owner;
    deadline_ = now + budget;
    limit_ = limit;
    arrival_.reset();
    observation_ = {};
    output_.clear();
    pending_ = true;
}

std::optional<ProcessResult>
Session::RetainedAndroidAction::poll(const WindowsAndroidOwner &owner, Clock::time_point now,
                                     const std::function<bool(const std::string &)> &complete,
                                     const std::function<void(const std::string &)> &observe) {
    if (!pending_) {
        return std::nullopt;
    }

    if (owner != owner_) {
        cancel();
        throw VoyemuError("ANDROID_ADB_OWNER", "stale identity probe completion");
    }

    auto finish = [&] {
        stream_.cancel();
        pending_ = false;
        retry_ = now + std::chrono::seconds(2);
    };
    try {
        auto next = stream_.poll();

        if (next.output.size() > limit_ - output_.size()) {
            throw VoyemuError("ANDROID_IDENTITY_PROBE", "output exceeds bound");
        }

        if (next.terminal || !next.output.empty()) {
            const auto sampled = Arrival{std::chrono::system_clock::now(), Clock::now(),
                                         !next.terminal};

            if (arrival_ && !next.output.empty()) {
                arrival_.reset();
                observation_.changed = true;
            }

            output_ += next.output;

            if (observe) {
                observe(output_);
            }

            observation_.bytes = output_.size();
            observation_.sha256 = sha256_bytes(output_);
            observation_.sampled = true;
            observation_.complete = complete && complete(output_);

            if (!arrival_ && !observation_.changed && observation_.complete) {
                arrival_ = sampled;
            }
        }

        if (next.terminal) {
            finish();

            if (next.terminal->exit_code && output_.empty()) {
                return next.terminal;
            }

            next.terminal->output = std::move(output_);

            return next.terminal;
        }

        if (now >= deadline_) {
            finish();

            return ProcessResult{1, {}, true};
        }

        return std::nullopt;
    } catch (...) {
        finish();
        throw;
    }
}

void Session::retire_windows_wheel_pair_for_flush() {
    auto &transaction = windows_android_;

    if (!transaction.wheel_pair_up_sequence) {
        return;
    }

    if (android_events_.size() < 2 ||
        android_events_[1].sequence != transaction.wheel_pair_up_sequence ||
        !android_wheel_release_reuses_identity(android_events_.front(), android_events_[1])) {
        throw VoyemuError("ANDROID_WHEEL",
                          "retained pair cannot be discarded without release identity");
    }

    const auto down = android_events_.front();
    const auto first_attempt = "wheel-event:" + std::to_string(down.sequence);

    if (!transaction.attempted_mutations.contains(first_attempt)) {
        if (android_identity_probe_.pending()) {
            throw VoyemuError("ANDROID_IDENTITY_NOT_READY",
                              "undispatched pair has another retained capability");
        }

        transaction.wheel_pair_up_sequence = 0;
        transaction.wheel_pair_hold_ms = 0;
        transaction.wheel_pair_cleanup = false;
        transaction.wheel_pair_down_receipt_available = false;
        android_possible_pressed_.erase(down.id);

        if (transaction.phase == Phase::event_submit || transaction.phase == Phase::event_verify) {
            transaction.phase = Phase::ready;
        }

        return;
    }

    auto deadline = WindowsAndroidTransaction::Clock::now() + std::chrono::seconds(30);

    if (android_request_deadline_ != WindowsAndroidTransaction::Clock::time_point{}) {
        deadline = std::min(deadline, android_request_deadline_);
    }

    deliveries_["android"]["press_cleanup"] = {{"status", "retained_short_pair_release_pending"},
                                               {"up_sequence", transaction.wheel_pair_up_sequence}};

    while (transaction.wheel_pair_up_sequence) {
        if (WindowsAndroidTransaction::Clock::now() >= deadline) {
            throw VoyemuError("ANDROID_IDENTITY_NOT_READY",
                              "retained short pair release deadline; queue preserved");
        }

        if (!android_identity_probe_.pending() && !transaction.public_operation &&
            (transaction.phase == Phase::ready || transaction.phase == Phase::identity)) {
            transaction.phase = Phase::event_verify;
        }

        try {
            advance_windows_android_transaction();
        } catch (const std::exception &error) {
            const std::string message = error.what();

            if (!message.starts_with("ANDROID_IDENTITY_NOT_READY:") &&
                !message.starts_with("TEST_BUSY:")) {
                throw;
            }
        }

        if (transaction.wheel_pair_up_sequence) {
            auto *state = android_io_state_lock_;
            const bool lent = state && state->owns_lock();

            if (lent) {
                state->unlock();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            if (lent) {
                state->lock();
            }
        }
    }
}

void Session::reset_windows_android_transaction() {
    android_identity_probe_.cancel();
    windows_android_ = WindowsAndroidTransaction{};
    android_identity_proof_operation_ = 0;
}

void Session::observe_windows_android_boot_change(const std::string &boot) {
    const auto previous = android_transport_boot_;
    auto verified = previous;

    if (!observe_android_boot(verified, boot)) {
        android_transport_boot_ = std::move(verified);

        return;
    }

    if (ui_observer_) {
        ui_observer_->retire_for_boot(previous, boot);
        ui_observer_.reset();
    }

    clear_ui_observer_metadata(directory_, session_id_);
    android_transport_boot_ = std::move(verified);
    reset_windows_android_transaction();
    ++android_generation_;
    invalidate_windows_android_readiness();
    android_transport_attempted_ = false;
    android_identity_mismatch_ = false;
    clear_android_home_delivery(deliveries_["android"]);
    clear_android_camera_delivery(deliveries_["android"]);
    theme_.reset();
    oem_language_.reset();
    next_oem_settings_ = {};
    next_display_observation_ = {};
    next_display_activation_ = {};
    test_ui_connection_ = nullptr;
    test_ui_subscription_ = nullptr;
    test_ui_sample_ = {};
    test_ui_sampled_ms_ = 0;
    test_can_cursor_ = nullptr;
    test_can_generation_ = 0;
    media_connection_written_ = false;
    std::filesystem::remove(directory_ / "media-connection.json");
    deliveries_["android"].erase("transport_failure");
    deliveries_["android"]["generation"] = android_generation_;
    deliveries_["android"]["external_reboot"] = {
            {"boot_id", boot}, {"source", "owned framed Linux boot identity change"}};
    windows_android_.started = WindowsAndroidTransaction::Clock::now();
    record_android_processes(false);
}

void Session::invalidate_windows_android_readiness() {
    android_identity_ = false;
    android_identity_proof_operation_ = 0;
    android_transport_ready_ = false;
    android_clock_observed_ = false;
    wheel_provisioned_ = false;
    android_revision_ = 0;
    android_boot_completed_ = false;
    android_displays_ready_ = false;
    android_display_activated_ = false;
    android_canvas_refreshed_ = false;
    primary_home_ready_ = false;
    deliveries_["android"].erase("home");

    for (const char *field : {"transport", "framework", "clock", "wheel_bootstrap", "displays",
                              "systemui", "host_canvas"}) {
        deliveries_["android"].erase(field);
    }

    deliveries_["android"]["guest_acknowledged"] = false;
    deliveries_["android"]["status"] = "booting";
}

void Session::guard_windows_android_transaction() {
    if (!windows_android_.initialized) {
        throw VoyemuError("TEST_BUSY", "Windows startup transaction has not begun");
    }

    const auto &owner = windows_android_.owner;
    const auto route = sha256_bytes(android_spec_.at("owned_adb").dump() + ":" +
                                    android_spec_.at("avd_name").get<std::string>() + ":" +
                                    windows_android_.nonce);
    try {
        if (owner.epoch != session_id_ || owner.route != route) {
            throw VoyemuError("ANDROID_ADB_OWNER", "Windows transaction ownership changed");
        }

        (void)owned_android_adb(android_spec_, owner.sdk_pid, owner.generation);
    } catch (...) {
        android_identity_probe_.cancel();
        invalidate_windows_android_readiness();
        throw VoyemuError("ANDROID_ADB_OWNER", "Windows transaction ownership changed");
    }

    if (test_services_closed_ && !android_identity_stopping_) {
        throw VoyemuError("TEST_BUSY", "Windows guest transaction closed for lifecycle shutdown");
    }
}

std::optional<ProcessResult>
Session::poll_windows_android_action(const std::vector<std::string> &arguments, bool mutation,
                                     const std::string &attempt) {
    auto &transaction = windows_android_;

    guard_windows_android_transaction();

    if (!android_identity_probe_.pending()) {
        if (transaction.launch_reserved) {
            throw VoyemuError("TEST_BUSY", "retained guest launch already reserved");
        }

        if (!android_identity_probe_.eligible()) {
            return std::nullopt;
        }

        transaction.launch_reserved = true;
        struct Reserve {
            bool &value;

            ~Reserve() {
                value = false;
            }
        } reserved{transaction.launch_reserved};

        if (transaction.startup_expired(WindowsAndroidTransaction::Clock::now())) {
            transaction.phase = Phase::failed;
            transaction.failure =
                    "ANDROID_STARTUP_DEADLINE: finite Windows startup budget exhausted";
            invalidate_windows_android_readiness();
            throw std::runtime_error(transaction.failure);
        }

        // An attempted mutation may only advance to its explicit readback phase.
        if (mutation && (attempt.empty() || transaction.attempted_mutations.contains(attempt))) {
            throw VoyemuError("ANDROID_STARTUP_MUTATION", "write attempt cannot be replayed");
        }

        transaction.frame = random_token();
        transaction.host_action = arguments.empty() || arguments.front() != "shell";
        auto dispatched = arguments;

        if (!transaction.host_action) {
            std::vector<std::string> body(arguments.begin() + 1, arguments.end());
            dispatched = {"shell",
                          windows_startup_command(body, transaction.frame, transaction.nonce,
                                                  transaction.phase == Phase::boot_recovery
                                                          ? transaction.candidate_boot
                                                          : transaction.boot,
                                                  mutation,
                                                  transaction.boot.empty() && android_resume_
                                                          ? session_id_
                                                          : std::string{},
                                                  body.size() == 1)};
        }

        if (transaction.host_action && dispatched != std::vector<std::string>{"root"}) {
            throw VoyemuError("ANDROID_STARTUP_ARGUMENT", "fixed retained service required");
        }

        transaction.pending_mutation = false;
        transaction.pending_attempt.clear();
        transaction.action_started = WindowsAndroidTransaction::Clock::now();
        transaction.action_deadline = transaction.action_started +
                                      WindowsAndroidTransaction::child_budget;

        if (transaction.public_operation &&
            android_request_deadline_ != std::chrono::steady_clock::time_point{}) {
            transaction.action_deadline = std::min(transaction.action_deadline,
                                                   android_request_deadline_);
        }

        if (!transaction.ever_ready) {
            transaction.action_deadline =
                    std::min(transaction.action_deadline,
                             transaction.started + WindowsAndroidTransaction::startup_budget);
        }

        auto budget = std::chrono::duration_cast<std::chrono::milliseconds>(
                transaction.action_deadline - WindowsAndroidTransaction::Clock::now());

        if (!transaction.ever_ready) {
            budget = std::min(budget, std::chrono::duration_cast<std::chrono::milliseconds>(
                                              transaction.started +
                                              WindowsAndroidTransaction::startup_budget -
                                              WindowsAndroidTransaction::Clock::now()));
        }

        if (transaction.public_operation &&
            android_request_deadline_ != std::chrono::steady_clock::time_point{}) {
            budget = std::min(
                    budget, std::chrono::duration_cast<std::chrono::milliseconds>(
                                    android_request_deadline_ - std::chrono::steady_clock::now()));
        }

        if (budget < std::chrono::milliseconds(1)) {
            throw VoyemuError("ANDROID_STARTUP_DEADLINE", "guest action has no remaining budget");
        }

        const bool native_diagnostic = android_spec_.contains("diagnostic_native");
        const auto name = adb({"emu", "avd", "name"},
                              windows_startup_avd_timeout(native_diagnostic, budget.count(), true));
        guard_windows_android_transaction();

        if (name.timed_out || name.exit_code) {
            throw VoyemuError("ANDROID_IDENTITY_NOT_READY", "transaction SDK AVD unavailable");
        }

        if (name.output.substr(0, name.output.find_first_of("\r\n")) !=
            android_spec_.at("avd_name").get<std::string>()) {
            throw VoyemuError("ANDROID_IDENTITY_MISMATCH", "transaction SDK AVD differs");
        }

        if (transaction.public_operation &&
            android_request_deadline_ != std::chrono::steady_clock::time_point{}) {
            budget = std::min(
                    budget, std::chrono::duration_cast<std::chrono::milliseconds>(
                                    android_request_deadline_ - std::chrono::steady_clock::now()));
        }

        if (!transaction.ever_ready) {
            budget = std::min(budget, std::chrono::duration_cast<std::chrono::milliseconds>(
                                              transaction.started +
                                              WindowsAndroidTransaction::startup_budget -
                                              WindowsAndroidTransaction::Clock::now()));
        }

        budget = std::min(
                budget,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        transaction.action_deadline - WindowsAndroidTransaction::Clock::now()) -
                        std::chrono::milliseconds(windows_startup_avd_fence_ms(native_diagnostic)));

        if (budget < std::chrono::milliseconds(1)) {
            throw VoyemuError("ANDROID_STARTUP_DEADLINE", "host fence consumed remaining budget");
        }

        transaction.pending_mutation = mutation;
        transaction.pending_public = transaction.public_operation;
        transaction.pending_arguments = arguments;
        transaction.pending_attempt = attempt;

        if (mutation) {
            if (transaction.attempted_mutations.size() >= 32) {
                throw VoyemuError("ANDROID_STARTUP_MUTATION",
                                  "bounded attempts retained for verification");
            }

            transaction.attempted_mutations.insert(attempt);
            // Persist uncertainty before dispatch. No argv, credentials or guest payload.
            write_private_json(directory_ / "android-startup-attempt.json",
                               {{"schema", 1},
                                {"session_id", session_id_},
                                {"generation", android_generation_},
                                {"sdk_pid", android_.pid()},
                                {"transport", "embedded"},
                                {"action_sha256", sha256_bytes(attempt)},
                                {"outcome", "submitted_unverified"}});
        }

        const auto clock_launch_utc_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
        const auto clock_launch_steady_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                        WindowsAndroidTransaction::Clock::now().time_since_epoch())
                        .count();
        const auto transport = owned_android_adb(android_spec_, transaction.owner.sdk_pid,
                                                 transaction.owner.generation);
        const auto launched = RetainedAndroidAction::Clock::now();
        auto stream = transaction.host_action
                              ? transport->open_root(static_cast<int>(budget.count()),
                                                     transaction.capture_limit)
                              : transport->open_shell(dispatched, static_cast<int>(budget.count()),
                                                      transaction.capture_limit);

        if (!transaction.host_action) {
            stream.close_stdin();
        }

        android_identity_probe_.launch(transaction.owner, std::move(stream), launched, budget,
                                       transaction.capture_limit);

        if (!transaction.host_action) {
            try {
                Json fields = {{"stage", "launch"},
                               {"phase", static_cast<unsigned>(transaction.phase)},
                               {"action_sha256", sha256_bytes(transaction.frame)},
                               {"sdk_pid", android_.pid()},
                               {"generation", android_generation_},
                               {"host_launch_utc_ms", clock_launch_utc_ms},
                               {"host_launch_steady_ns", clock_launch_steady_ns},
                               {"timestamp_basis",
                                "host before retained stream open; remote emit instant unknown"}};

                if (arguments.size() == 5 && arguments[0] == "shell" && arguments[1] == "cmd" &&
                    arguments[2] == "alarm" && arguments[3] == "set-time") {
                    fields["target_utc_ms"] = std::stoll(arguments[4]);
                }

                logging::write(logging::Level::debug, "android", "startup.clock", fields);
            } catch (...) { /* Ignored: clock observations never affect dispatch. */
            }
        }

        try {
            logging::write(logging::Level::debug, "android", "startup.action",
                           {{"stage", "launched"},
                            {"phase", static_cast<unsigned>(transaction.phase)},
                            {"sdk_pid", android_.pid()},
                            {"generation", android_generation_},
                            {"public_operation", transaction.public_operation},
                            {"mutation", mutation},
                            {"budget_ms", budget.count()}});
        } catch (...) { /* Ignored: scalar diagnostics never affect dispatch. */
        }

        return std::nullopt;
    }

    if (transaction.pending_arguments != arguments || transaction.pending_mutation != mutation ||
        transaction.pending_attempt != attempt) {
        throw VoyemuError("TEST_BUSY", "another retained Windows guest action is pending");
    }

    Json frame_observation = Json::object();
    auto observe = [&](const std::string &text) {
        if (!transaction.host_action) {
            frame_observation = windows_startup_frame_metadata(
                    text, transaction.frame, transaction.nonce,
                    transaction.phase == Phase::boot_recovery ? transaction.candidate_boot
                                                              : transaction.boot,
                    transaction.boot.empty() && android_resume_ ? session_id_ : std::string{});
        }
    };
    auto complete = [&](const std::string &text) {
        if (transaction.host_action) {
            return false;
        }

        try {
            const auto candidate = parse_windows_startup_frame(
                    {0, text, false}, transaction.frame, transaction.nonce,
                    transaction.phase == Phase::boot_recovery ? transaction.candidate_boot
                                                              : transaction.boot,
                    transaction.boot.empty() && android_resume_ ? session_id_ : std::string{});

            return !candidate.boot_changed;
        } catch (...) {
            return false;
        }
    };
    auto captured = android_identity_probe_.poll(
            transaction.owner, RetainedAndroidAction::Clock::now(), complete, observe);
    guard_windows_android_transaction();

    if (!captured) {
        return std::nullopt;
    }

    const auto clock_receive_utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();
    const auto clock_receive_steady_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    WindowsAndroidTransaction::Clock::now().time_since_epoch())
                    .count();

    if (!transaction.host_action) {
        try {
            logging::write(logging::Level::debug, "android", "startup.clock",
                           {{"stage", "receive"},
                            {"phase", static_cast<unsigned>(transaction.phase)},
                            {"action_sha256", sha256_bytes(transaction.frame)},
                            {"sdk_pid", android_.pid()},
                            {"generation", android_generation_},
                            {"host_receive_utc_ms", clock_receive_utc_ms},
                            {"host_receive_steady_ns", clock_receive_steady_ns},
                            {"outer_exit_code", captured->exit_code},
                            {"timed_out", captured->timed_out},
                            {"output_bytes", captured->output.size()},
                            {"host_owner_verified", true}});
        } catch (...) { /* Ignored: no guest payload or credentials. */
        }
    }

    try {
        logging::write(logging::Level::debug, "android", "startup.action",
                       {{"stage", "stream_terminal"},
                        {"phase", static_cast<unsigned>(transaction.phase)},
                        {"sdk_pid", android_.pid()},
                        {"generation", android_generation_},
                        {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                               WindowsAndroidTransaction::Clock::now() -
                                               transaction.action_started)
                                               .count()},
                        {"exit_code", captured->exit_code},
                        {"timed_out", captured->timed_out},
                        {"output_bytes", captured->output.size()},
                        {"host_owner_verified", true},
                        {"retained_terminal", true}});
    } catch (...) { /* Ignored: no argv, guest output, nonce or credentials. */
    }

    if ((transaction.host_action && arguments == std::vector<std::string>{"root"}) ||
        captured->exit_code || captured->timed_out) {
        try {
            const auto diagnostic = windows_adb_diagnostic(*captured,
                                                           android_adb_serial(android_spec_));
            logging::write(
                    logging::Level::debug, "android", "startup.adb_response",
                    {{"phase", static_cast<unsigned>(transaction.phase)},
                     {"sdk_pid", android_.pid()},
                     {"generation", android_generation_},
                     {"host_action", transaction.host_action},
                     {"outer_exit_code", captured->exit_code},
                     {"timed_out", captured->timed_out},
                     {"output_bytes", captured->output.size()},
                     {"output_sha256", sha256_bytes(captured->output)},
                     {"pending_output_sampled",
                      android_identity_probe_.output_observation().sampled},
                     {"pending_output_bytes", android_identity_probe_.output_observation().bytes},
                     {"pending_output_sha256", android_identity_probe_.output_observation().sha256},
                     {"pending_frame_complete",
                      android_identity_probe_.output_observation().complete},
                     {"pending_frame_changed",
                      android_identity_probe_.output_observation().changed},
                     {"frame_observation", frame_observation},
                     {"diagnostic_code", std::string(diagnostic.code)},
                     {"diagnostic_message", std::string(diagnostic.message)}});
        } catch (...) { /* Ignored: whitelisted diagnostics never affect dispatch or retain payload.
                         */
        }
    }

    const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    transaction.action_deadline - WindowsAndroidTransaction::Clock::now())
                    .count();

    if (remaining < 1) {
        throw VoyemuError("ANDROID_IDENTITY_NOT_READY",
                          "retained action post-fence deadline exhausted");
    }

    const auto name = adb({"emu", "avd", "name"},
                          windows_startup_avd_timeout(android_spec_.contains("diagnostic_native"),
                                                      remaining, false));
    guard_windows_android_transaction();

    if (std::chrono::steady_clock::now() > transaction.action_deadline) {
        throw VoyemuError("ANDROID_IDENTITY_NOT_READY",
                          "retained action post-fence deadline exhausted");
    }

    if (name.timed_out || name.exit_code) {
        throw VoyemuError("ANDROID_IDENTITY_NOT_READY",
                          "transaction SDK AVD unavailable after action");
    }

    if (name.output.substr(0, name.output.find_first_of("\r\n")) !=
        android_spec_.at("avd_name").get<std::string>()) {
        throw VoyemuError("ANDROID_IDENTITY_MISMATCH", "transaction SDK AVD differs after action");
    }

    // The completed exact output is retired only after its captured bytes are retained locally.
    const auto arrival = android_identity_probe_.arrival();
    android_identity_probe_.cancel();

    if (captured->timed_out) {
        return captured;
    }

    if (transaction.host_action) {
        return captured;
    }

    const auto framed = parse_windows_startup_frame(
            *captured, transaction.frame, transaction.nonce,
            transaction.phase == Phase::boot_recovery ? transaction.candidate_boot
                                                      : transaction.boot,
            transaction.boot.empty() && android_resume_ ? session_id_ : std::string{});

    if (framed.boot_changed) {
        // A partial pre-fence is only a candidate. It cannot authorize a new boot
        // or any write; confirm it in a separate complete readonly transaction.
        transaction.candidate_boot = framed.boot_id;
        transaction.phase = Phase::boot_recovery;
        invalidate_windows_android_readiness();
        throw VoyemuError("ANDROID_IDENTITY_NOT_READY",
                          "changed boot candidate requires complete readonly confirmation");
    }

    if (transaction.phase == Phase::boot_recovery) {
        observe_windows_android_boot_change(framed.boot_id);
        throw VoyemuError("ANDROID_IDENTITY_NOT_READY",
                          "complete readonly boot confirmation invalidated startup generation");
    }

    if (transaction.boot.empty()) {
        transaction.boot = framed.boot_id;
        transaction.nonce = framed.nonce;
        transaction.owner.route = sha256_bytes(android_spec_.at("owned_adb").dump() + ":" +
                                               android_spec_.at("avd_name").get<std::string>() +
                                               ":" + transaction.nonce);
    }

    transaction.observed_utc = framed.utc_seconds;
    transaction.observed_arrival =
            arrival ? arrival->steady
                    : WindowsAndroidTransaction::Clock::time_point(
                              std::chrono::nanoseconds(clock_receive_steady_ns));
    transaction.observed_host_utc_ms =
            arrival ? std::chrono::duration_cast<std::chrono::milliseconds>(
                              arrival->utc.time_since_epoch())
                              .count()
                    : clock_receive_utc_ms;
    const auto arrival_age =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    WindowsAndroidTransaction::Clock::now() - transaction.observed_arrival)
                    .count();
    const auto wall_age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count() -
                          transaction.observed_host_utc_ms;

    if (!android_clock_observation_current(arrival_age)) {
        throw VoyemuError("ANDROID_CLOCK", "arrival observation stale");
    }

    if (!android_clock_arrival_valid(transaction.observed_host_utc_ms,
                                     transaction.observed_host_utc_ms + wall_age, arrival_age)) {
        deliveries_["android"]["clock"].update(
                {{"status", "degraded"}, {"synced", false}, {"host_clock_stable", false}});
    }

    if (!transaction.host_action) {
        try {
            logging::write(
                    logging::Level::debug, "android", "startup.clock",
                    {{"stage", "validated"},
                     {"phase", static_cast<unsigned>(transaction.phase)},
                     {"action_sha256", sha256_bytes(transaction.frame)},
                     {"sdk_pid", android_.pid()},
                     {"generation", android_generation_},
                     {"host_receive_utc_ms", clock_receive_utc_ms},
                     {"host_receive_steady_ns", clock_receive_steady_ns},
                     {"host_validated_utc_ms",
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count()},
                     {"host_arrival_utc_ms", transaction.observed_host_utc_ms},
                     {"arrival_age_ms", arrival_age},
                     {"arrival_before_terminal", arrival && arrival->before_terminal},
                     {"guest_tail_utc_seconds", framed.utc_seconds},
                     {"body_exit_code", framed.result.exit_code},
                     {"host_owner_boot_nonce_verified", true},
                     {"timestamp_basis",
                      "complete validated unchanged frame arrival; terminal and owner post-fence required"}});
        } catch (...) { /* Ignored: safe scalar retention after complete identity validation. */
        }
    }

    if (const auto diagnostic = windows_clock_set_diagnostic(transaction.phase, framed)) {
        try {
            logging::write(logging::Level::debug, "android", "startup.clock_set_failure",
                           {{"phase", static_cast<unsigned>(transaction.phase)},
                            {"action_sha256", sha256_bytes(transaction.frame)},
                            {"sdk_pid", android_.pid()},
                            {"generation", android_generation_},
                            {"body_exit_code", framed.result.exit_code},
                            {"body_bytes", framed.result.output.size()},
                            {"body_sha256", sha256_bytes(framed.result.output)},
                            {"diagnostic_code", std::string(diagnostic->code)},
                            {"diagnostic_message", std::string(diagnostic->message)},
                            {"host_owner_boot_nonce_verified", true}});
        } catch (...) { /* Ignored: closed diagnostics never change dispatch or retain guest
                           payload. */
        }
    }

    android_identity_ = true;
    android_identity_mismatch_ = false;
    android_identity_proof_operation_ = android_identity_operation_;

    return framed.result;
}

ProcessResult Session::execute_windows_android_operation(const Json &specification,
                                                         const std::vector<std::string> &arguments,
                                                         int timeout_ms, std::size_t limit,
                                                         std::int64_t expected_pid,
                                                         std::uint64_t expected_generation) {
    std::unique_lock state(mutex_);
    auto &transaction = windows_android_;
    auto *borrowed = android_io_state_lock_;
    const bool lent = borrowed && borrowed->owns_lock();

    if (lent) {
        borrowed->unlock(); // Retain exactly this local recursive acquisition while waiting.
    }

    struct ReturnBorrowed {
        std::unique_lock<std::recursive_mutex> *value;
        bool lent;

        ~ReturnBorrowed() {
            if (lent && !value->owns_lock()) {
                value->lock();
            }
        }
    } return_borrowed{borrowed, lent};
    struct StateScope {
        std::unique_lock<std::recursive_mutex> *&slot;
        std::unique_lock<std::recursive_mutex> *previous;

        ~StateScope() {
            slot = previous;
        }
    } state_scope{android_io_state_lock_, android_io_state_lock_};
    android_io_state_lock_ = &state;

    if (arguments.empty() || arguments.front() != "shell" || timeout_ms < 1 ||
        timeout_ms > 120000 || limit > 128 * 1024) {
        throw VoyemuError("ANDROID_STARTUP_ARGUMENT", "bounded guest operation required");
    }

    if (!transaction.initialized || transaction.boot.empty() ||
        (!transaction.ever_ready && !android_identity_stopping_ && !transaction.startup_services)) {
        throw VoyemuError("TEST_BUSY", "Windows initial startup transaction not ready");
    }

    if (expected_pid != android_.pid() || expected_generation != android_generation_ ||
        specification.at("owned_adb") != android_spec_.at("owned_adb")) {
        throw VoyemuError("ANDROID_ADB_OWNER", "operation generation changed");
    }

    guard_windows_android_transaction();

    if (transaction.public_operation || transaction.launch_reserved) {
        throw VoyemuError("TEST_BUSY", "another owned guest operation is pending");
    }

    if (android_identity_probe_.pending() && transaction.pending_mutation) {
        throw VoyemuError("TEST_BUSY", "protected startup mutation awaiting verification");
    }

    // Read-only background observations can yield to an explicitly bounded operation.
    android_identity_probe_.cancel();
    const auto old_limit = transaction.capture_limit;
    transaction.capture_limit = std::min<std::size_t>(512 * 1024,
                                                      std::max<std::size_t>(4096, limit + 2048));
    transaction.public_operation = true;
    std::string public_attempt;
    struct Restore {
        WindowsAndroidTransaction &transaction;
        RetainedAndroidAction &probe;
        std::string &attempt;
        std::size_t limit;

        ~Restore() {
            if (!probe.pending() && !attempt.empty()) {
                transaction.attempted_mutations.erase(attempt);
            }

            transaction.public_operation = false;
            transaction.capture_limit = limit;
        }
    } restore{transaction, android_identity_probe_, public_attempt, old_limit};
    // The caller's bound is a liveness floor, not a ceiling: the framed transaction still needs
    // both AVD identity fences, so a bound below that floor would be refused before any guest
    // dispatch.
    const auto began = std::chrono::steady_clock::now();
    auto deadline = began + std::chrono::milliseconds(windows_guest_operation_timeout(
                                    timeout_ms, specification.contains("diagnostic_native")));

    if (android_request_deadline_ != std::chrono::steady_clock::time_point{}) {
        deadline = std::min(deadline, android_request_deadline_);
    }

    if (deadline <= began) {
        throw VoyemuError("TEST_BUSY", "operation deadline exhausted before guest dispatch");
    }

    const auto old_deadline = android_request_deadline_;
    android_request_deadline_ = deadline;
    struct RestoreDeadline {
        std::chrono::steady_clock::time_point &value;
        std::chrono::steady_clock::time_point previous;

        ~RestoreDeadline() {
            value = previous;
        }
    } restore_deadline{android_request_deadline_, old_deadline};
    bool mutation = true;

    if (arguments.size() > 1) {
        const auto &verb = arguments[1];
        mutation = !(verb == "getprop" || verb == "cat" || verb == "id" || verb == "getenforce" ||
                     verb == "dumpsys" ||
                     (verb == "date" && arguments.size() == 3 && arguments[2] == "+%s") ||
                     (verb == "content" && arguments.size() > 2 && arguments[2] == "query") ||
                     (verb == "cmd" && arguments.size() > 3 && arguments[2] == "package" &&
                      (arguments[3] == "resolve-activity" || arguments[3] == "query-activities")));
    }

    if (arguments.size() == 2) {
        for (const char *authority : {"com.voyemu.input.wheel", "com.voyemu.output.vehicle"}) {
            const auto prefix = "'content' 'call' '--uri' 'content://" + std::string(authority) +
                                "' '--method' 'status'";

            if (arguments[1] == prefix || arguments[1].starts_with(prefix + " ")) {
                mutation = false;
            }
        }
    }

    const auto attempt = mutation ? "operation:" + random_token() : std::string{};
    public_attempt = attempt;

    for (;;) {
        guard_windows_android_transaction();

        if (expected_pid != android_.pid() || expected_generation != android_generation_) {
            throw VoyemuError("ANDROID_ADB_OWNER", "operation generation changed while waiting");
        }

        auto result = poll_windows_android_action(arguments, mutation, attempt);
        guard_windows_android_transaction();

        if (result) {
            if (result->output.size() > limit) {
                throw VoyemuError("ANDROID_STARTUP_FRAME", "operation output exceeds caller bound");
            }

            if (!result->timed_out && mutation) {
                transaction.attempted_mutations.erase(attempt);
            }

            return *result;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            android_identity_probe_.cancel();

            return {1, {}, true};
        }

        state.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        state.lock();
        // stop/reboot can cancel during the unlocked sleep; never turn that into a new launch.
        guard_windows_android_transaction();

        if (!android_identity_probe_.pending()) {
            throw VoyemuError("TEST_BUSY", "owned guest operation was cancelled");
        }
    }
}

void Session::assert_windows_android_identity(std::chrono::steady_clock::time_point deadline) {
    guard_windows_android_transaction();

    if (android_identity_ && android_identity_proof_operation_ == android_identity_operation_) {
        return;
    }

    const auto timeout = windows_android_identity_timeout(std::chrono::steady_clock::now(),
                                                          android_request_deadline_, deadline);
    const auto token = adb({"shell", "getprop", "qemu.voyemu.token"}, timeout);

    if (token.timed_out || token.exit_code ||
        startup_line(token.output) != windows_android_.nonce) {
        invalidate_windows_android_readiness();
        throw VoyemuError("ANDROID_IDENTITY_NOT_READY",
                          "fresh owned operation identity unavailable");
    }

    android_identity_ = true;
    android_identity_proof_operation_ = android_identity_operation_;
}

void Session::advance_windows_android_transaction() {
    windows_camera_delivery_guard(android_spec_.contains("camera_package") ||
                                  android_spec_.contains("prepared_packages"));

    if (android_spec_.contains("mcu_property_adapter")) {
        deliveries_["android"]["guest_acknowledged"] = false;
        deliveries_["android"]["status"] = "unsupported";
        throw VoyemuError("PASSION_MCU", "Windows partial projection is unsupported");
    }

    auto &transaction = windows_android_;
    auto &android = deliveries_["android"];
    const auto now = WindowsAndroidTransaction::Clock::now();
    auto update_clock = [&] {
        const auto age =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        WindowsAndroidTransaction::Clock::now() - transaction.observed_arrival)
                        .count();
        const auto host_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
        const bool stable = android_clock_arrival_valid(transaction.observed_host_utc_ms, host_ms,
                                                        age);
        const bool synced = android_clock_synced(transaction.observed_utc,
                                                 transaction.observed_host_utc_ms, host_ms, age);
        const bool attempted = transaction.attempted_mutations.contains("clock");
        const auto unattempted =
                android.value("clock", Json::object()).value("correction_result", std::string{}) ==
                                "service_unavailable"
                        ? "service_unavailable"
                        : "not_attempted";
        android["clock"] = {
                {"status", synced ? "host_time_verified" : "degraded"},
                {"synced", synced},
                {"corrected", attempted && synced},
                {"correction_attempted", attempted},
                {"correction_result",
                 attempted ? (transaction.clock_exit_code < 0    ? "unknown"
                              : transaction.clock_exit_code == 0 ? "command_succeeded"
                                                                 : "command_failed")
                           : unattempted},
                {"command_exit_code", transaction.clock_exit_code},
                {"offset_at_arrival_ms",
                 transaction.observed_host_utc_ms - transaction.observed_utc * 1000},
                {"observation_age_ms", age},
                {"host_clock_stable", stable},
                {"source",
                 "owned UTC tail at frame arrival after terminal boot and nonce verification"}};
    };

    if (!transaction.initialized) {
        const auto started = transaction.started == WindowsAndroidTransaction::Clock::time_point{}
                                     ? android_started_
                                     : transaction.started;
        const auto nonce = android_resume_ ? android_boot_nonce_ : session_id_;
        const WindowsAndroidOwner owner{
                session_id_, android_generation_, android_.pid(),
                sha256_bytes(android_spec_.at("owned_adb").dump() + ":" +
                             android_spec_.at("avd_name").get<std::string>() + ":" + nonce)};
        transaction.initialize(owner, nonce, started);
    }

    guard_windows_android_transaction();

    // An interrupted public caller may leave only its retained child, never an
    // automatically replayable command. Drain that exact capability to terminal.
    if (android_identity_probe_.pending() && !transaction.public_operation &&
        transaction.pending_public) {
        const auto attempt = transaction.pending_attempt;
        const auto arguments = transaction.pending_arguments;
        const bool mutation = transaction.pending_mutation;
        try {
            if (!poll_windows_android_action(arguments, mutation, attempt)) {
                return;
            }
        } catch (...) {
            if (!android_identity_probe_.pending()) {
                transaction.attempted_mutations.erase(attempt);
            }

            invalidate_windows_android_readiness();
            throw;
        }

        transaction.attempted_mutations.erase(attempt);
        invalidate_windows_android_readiness();
        transaction.phase = Phase::identity;

        return;
    }

    if (transaction.public_operation || transaction.launch_reserved) {
        if (transaction.ever_ready && !transaction.observation_current(now)) {
            invalidate_windows_android_readiness();
        }

        return; // The single capability belongs to the waiting public operation.
    }

    if (transaction.phase == Phase::failed) {
        throw std::runtime_error(transaction.failure);
    }

    if (transaction.ever_ready) {
        android["startup_observation"] = {
                {"age_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - transaction.last_observation)
                                   .count()},
                {"maximum_age_ms", 63000},
                {"source", "owned framed Windows guest transaction"}};

        if (!transaction.observation_current(now)) {
            invalidate_windows_android_readiness();
        }
    }

    if (transaction.phase == Phase::ready) {
        android["startup_observation"] = {
                {"age_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - transaction.last_observation)
                                   .count()},
                {"maximum_age_ms", 63000},
                {"source", "owned framed Windows guest transaction"}};

        if (!transaction.observation_current(now)) {
            invalidate_windows_android_readiness();
        }

        if (!android_events_.empty()) {
            const bool a9 = android_spec_.value("api", 0) == 28 &&
                            embedded_profile_declares(
                                    android_spec_.value("reference_profile", std::string{}),
                                    ProfilePlatform::qinggan09, ProfileBoard::sa8155) &&
                            android_spec_.contains("qinggan09_owned_packages");

            if (a9 &&
                windows_wheel_dispatch(android_events_, WindowsAndroidTransaction::Clock::now())
                                .kind == WindowsWheelDispatch::wait) {
                return;
            }

            transaction.phase = Phase::event_submit;
        } else if (now < transaction.next_health &&
                   android_revision_ == model_.state().at("sequence").get<std::uint32_t>()) {
            return;
        } else {
            transaction.phase = Phase::health;
        }

        transaction.health_marker = random_token();
        transaction.settings_revision = model_.state().at("sequence").get<std::uint32_t>();
        transaction.capture_limit = 512 * 1024;
    }

    auto provider = [&](const std::string &authority, const std::string &method,
                        const std::string &argument = std::string{}) {
        std::vector<std::string> values{"content",  "call", "--uri", "content://" + authority,
                                        "--method", method};

        if (authority == "com.voyemu.input.wheel") {
            values.insert(values.end(),
                          {"--extra", "schema:i:1", "--extra", "session:s:" + session_id_,
                           "--extra", "token:s:" + guest_auth_token_});

            if (method == "status") {
                values.insert(values.end(), {"--arg", argument.empty() ? "bootstrap" : argument});
            }
        }

        return std::vector<std::string>{"shell", startup_shell(values)};
    };
    auto passed = [](const ProcessResult &result) {
        return !result.timed_out && result.exit_code == 0;
    };
    auto read = [&](const std::vector<std::string> &arguments) -> std::optional<ProcessResult> {
        auto result = poll_windows_android_action(arguments);

        if (result && !passed(*result)) {
            throw VoyemuError("ANDROID_IDENTITY_NOT_READY", "retained startup read unavailable");
        }

        return result;
    };
    auto observe_language = [&](const std::string &output) {
        auto &language = android["language"];
        language = {{"source", "qinggan.settings/system/system_language_setting"},
                    {"visible_effect_verified", false}};

        if (const auto value = oem_language_setting(output)) {
            language.update({{"status", "observed"}, {"value", *value}});

            if (model_.state().at("sequence") != transaction.settings_revision) {
                language["status"] = "awaiting_stable_source";
            } else {
                if (auto change = oem_language_change(oem_language_, *value)) {
                    if (model_.state().at("raw_oem").at("ivi_languageSet") != *value) {
                        model_.patch(*change);
                        write_private_json(directory_ / "vehicle-state.json", model_.state(),
                                           vehicle_state_bytes);

                        if (cluster_requested_) {
                            deliveries_["qnx"]["vehicle"] = qnx_vehicle_pending();
                        }
                    }

                    oem_language_ = value;
                }

                queue_cluster_language();
                language["source_revision"] = model_.state()["sequence"];
                language["canonical_matches"] =
                        model_.state().at("raw_oem").at("ivi_languageSet") == *value;
            }
        } else {
            language["status"] = "not_observed";
        }
    };
    auto require_reply = [&](const ProcessResult &result) {
        if (!passed(result) || result.output.find("SecurityException") != std::string::npos) {
            throw VoyemuError("ANDROID_PROVISION", "retained protected response unavailable");
        }

        const auto value = parse_android_provider_reply(result.output);

        if (value.dump().find(guest_auth_token_) != std::string::npos) {
            throw VoyemuError("ANDROID_PROVIDER_REPLY", "private credential in response");
        }

        return value;
    };
    // Mirrors run_android_startup_stages: a guard failure is retained and retried, never fatal.
    auto record_guard = [&](const std::exception &error) {
        std::string message = error.what();

        if (message.size() > 2048) {
            message.resize(2048);
        }

        for (char &c : message) {
            if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) >= 127) {
                c = '?';
            }
        }

        if (!android.contains("stage_errors") || !android.at("stage_errors").is_object()) {
            android["stage_errors"] = Json::object();
        }

        android["stage_errors"]["guard"] = message;
        android["last_error"] = message;
    };
    auto clear_guard = [&] {
        if (android.contains("stage_errors") && android.at("stage_errors").is_object()) {
            android["stage_errors"].erase("guard");
        }

        android.erase("last_error");
    };
    auto home_args = [&](bool scoped) {
        std::vector<std::string> arguments{"shell",
                                           "cmd",
                                           "package",
                                           "resolve-activity",
                                           "--brief",
                                           "--user",
                                           "0",
                                           "-a",
                                           "android.intent.action.MAIN",
                                           "-c",
                                           "android.intent.category.HOME"};

        if (scoped) {
            arguments.insert(arguments.end(),
                             {"-p", android_spec_.at("home").at("package").get<std::string>()});
        }

        return arguments;
    };
    try {
        // At most a completed action plus one following launch per tick. No waiting here.
        for (unsigned progress = 0; progress < 2; ++progress) {
            switch (transaction.phase) {
            case Phase::boot_recovery: {
                if (transaction.candidate_boot.empty()) {
                    throw VoyemuError("ANDROID_STARTUP_FRAME", "missing boot recovery candidate");
                }

                auto result = read({"shell", "id", "-u"});

                if (!result) {
                    return;
                }

                throw VoyemuError("ANDROID_STARTUP_FRAME",
                                  "boot recovery confirmation did not invalidate generation");
            }
            case Phase::event_submit:
            case Phase::event_verify: {
                if (android_events_.empty()) {
                    throw VoyemuError("ANDROID_WHEEL",
                                      "retained event disappeared before verification");
                }

                const auto event = android_events_.front();

                if (!android_spec_.contains("wheel_adapter") ||
                    !android_spec_.at("wheel_adapter").contains("routes") ||
                    !android_spec_.at("wheel_adapter").at("routes").contains(event.id) ||
                    android_spec_.at("wheel_adapter").at("routes").at(event.id).is_null()) {
                    android["wheel"] = {{"event_sequence", event.sequence}, {"status", "unmapped"}};
                    android_events_.pop_front();
                    transaction.phase = Phase::ready;
                    break;
                }

                if (transaction.phase == Phase::event_submit &&
                    !android_identity_probe_.pending() && !transaction.wheel_pair_up_sequence &&
                    !transaction.attempted_mutations.contains("wheel-event:" +
                                                              std::to_string(event.sequence))) {
                    const bool a9 = android_spec_.value("api", 0) == 28 &&
                                    embedded_profile_declares(
                                            android_spec_.value("reference_profile", std::string{}),
                                            ProfilePlatform::qinggan09, ProfileBoard::sa8155) &&
                                    android_spec_.contains("qinggan09_owned_packages");
                    const auto plan = windows_wheel_dispatch(
                            android_events_, WindowsAndroidTransaction::Clock::now());

                    if (a9 && plan.kind == WindowsWheelDispatch::pair) {
                        transaction.wheel_pair_up_sequence = android_events_[1].sequence;
                        transaction.wheel_pair_hold_ms = plan.hold_ms;
                    }
                }

                const bool paired = transaction.wheel_pair_up_sequence != 0;

                if (paired && (android_events_.size() < 2 ||
                               android_events_[1].sequence != transaction.wheel_pair_up_sequence ||
                               !android_wheel_release_reuses_identity(event, android_events_[1]) ||
                               android_events_[1].phase != "up")) {
                    throw VoyemuError("ANDROID_WHEEL", "frozen short pair identity changed");
                }

                const auto release = paired ? android_events_[1] : event;
                const auto first_attempt = "wheel-event:" + std::to_string(event.sequence);
                const auto cleanup_attempt = "wheel-pair-release:" +
                                             std::to_string(release.sequence);
                const auto attempt = transaction.wheel_pair_cleanup ? cleanup_attempt
                                                                    : first_attempt;

                if (transaction.phase == Phase::event_submit) {
                    if (!android_identity_probe_.pending() &&
                        transaction.attempted_mutations.contains(attempt)) {
                        transaction.phase = Phase::event_verify;
                        break;
                    }

                    const auto submitted = paired && transaction.wheel_pair_cleanup ? release
                                                                                    : event;
                    std::vector<std::string> values{
                            "content",  "call",
                            "--uri",    "content://com.voyemu.input.wheel",
                            "--method", "input",
                            "--extra",  "schema:i:1",
                            "--extra",  "session:s:" + session_id_,
                            "--extra",  "token:s:" + guest_auth_token_,
                            "--extra",  "sequence:l:" + std::to_string(submitted.sequence),
                            "--extra",  "button:s:" + submitted.id,
                            "--extra",  "owner:s:" + submitted.owner,
                            "--extra",  "phase:s:" + submitted.phase};
                    const auto command =
                            paired && !transaction.wheel_pair_cleanup
                                    ? android_short_wheel_pair_command(
                                              session_id_, guest_auth_token_, transaction.boot,
                                              event, release, transaction.wheel_pair_hold_ms,
                                              AndroidShortWheelContract::a9, transaction.nonce)
                                    : startup_shell(values);

                    if (event.phase == "down") {
                        android_possible_pressed_[event.id] = event.owner;
                    }

                    auto result = poll_windows_android_action({"shell", command}, true, attempt);

                    if (!result) {
                        return;
                    }

                    if (paired && !transaction.wheel_pair_cleanup) {
                        const std::string begin = "VOYEMU_DOWN\n", end = "VOYEMU_UP\n";
                        const auto split = result->output.find(end);
                        try {
                            if (result->output.starts_with(begin) && split != std::string::npos) {
                                transaction.wheel_pair_down_receipt_available =
                                        android_wheel_delivery_receipt(
                                                parse_android_provider_reply(result->output.substr(
                                                        begin.size(), split - begin.size())),
                                                session_id_, event)
                                                .has_value();
                            }
                        } catch (const std::exception &) {
                            transaction.wheel_pair_down_receipt_available = false;
                        }
                    }

                    transaction.phase = Phase::event_verify;
                    break;
                }

                auto result = read(provider("com.voyemu.input.wheel", "status", "input"));

                if (!result) {
                    return;
                }

                const auto receipt = require_reply(*result);
                auto delivery = android_wheel_delivery_receipt(receipt, session_id_, release);

                if (!delivery) {
                    if (paired && !transaction.wheel_pair_cleanup &&
                        android_wheel_delivery_receipt(receipt, session_id_, event)) {
                        transaction.wheel_pair_cleanup = true;
                        transaction.phase = Phase::event_submit;
                        break;
                    }

                    throw VoyemuError("ANDROID_IDENTITY_NOT_READY",
                                      "matching processed wheel release receipt unavailable");
                }

                if (paired) {
                    const bool short_action = receipt.value("actions", Json::array()) ==
                                                      Json::array({8192, 65536}) &&
                                              !receipt.contains("hold_actions");
                    (*delivery)["short_pair"] = {
                            {"status", short_action ? "verified_short" : "unexpected_actions"},
                            {"host_hold_ms", transaction.wheel_pair_hold_ms},
                            {"down_receipt_available",
                             transaction.wheel_pair_down_receipt_available},
                            {"release_cleanup_attempted", transaction.wheel_pair_cleanup}};
                }

                android["wheel"] = *delivery;

                if (release.phase != "down") {
                    android_possible_pressed_.erase(release.id);
                }

                android_events_.pop_front();

                if (paired) {
                    android_events_.pop_front();
                }

                transaction.attempted_mutations.erase(first_attempt);
                transaction.attempted_mutations.erase(cleanup_attempt);
                transaction.wheel_pair_up_sequence = 0;
                transaction.wheel_pair_hold_ms = 0;
                transaction.wheel_pair_cleanup = false;
                transaction.wheel_pair_down_receipt_available = false;
                transaction.phase = Phase::ready;
                break;
            }
            case Phase::health: {
                if (transaction.health_marker.empty()) {
                    transaction.settings_revision =
                            model_.state().at("sequence").get<std::uint32_t>();
                    transaction.health_marker = random_token();
                }

                const bool multiple =
                        android_spec_.contains("features") &&
                        android_spec_.at("features").value("MultiDisplay", std::string{}) == "on";
                const auto default_home = home_args(false);
                const auto theme_queries = oem_theme_queries(android_spec_);
                auto theme_query = [&](std::size_t index) {
                    const auto &query = theme_queries.at(index);

                    return startup_shell(std::vector<std::string>(query.begin() + 1, query.end()));
                };
                std::vector<std::pair<std::string, std::string>> actions{
                        {"uid", startup_shell({"id", "-u"})},
                        {"selinux", startup_shell({"getenforce"})},
                        {"boot", startup_shell({"getprop", "sys.boot_completed"})},
                        {"home", android_spec_.contains("home")
                                         ? startup_shell(std::vector<std::string>(
                                                   default_home.begin() + 1, default_home.end()))
                                         : "true"},
                        {"display", multiple ? startup_shell({"dumpsys", "display"}) : "true"},
                        {"windows", multiple || android_spec_.contains("home")
                                            ? startup_shell({"dumpsys", "window", "windows"})
                                            : "true"},
                        {"state", android_spec_.contains("vehicle_adapter")
                                          ? provider("com.voyemu.output.vehicle", "status").at(1)
                                          : "true"},
                        {"wheel", android_spec_.contains("wheel_adapter")
                                          ? provider("com.voyemu.input.wheel", "status").at(1)
                                          : "true"},
                        {"language",
                         startup_shell({"content", "query", "--uri",
                                        "content://qinggan.settings/system/system_language_setting",
                                        "--projection", "value"})},
                        {"appearance", startup_shell({"getprop", "persist.sys.qg.currenttheme"})},
                        {"theme_first", theme_queries.size() > 1 ? theme_query(0) : "true"},
                        {"theme_second", theme_query(theme_queries.size() - 1)}};
                std::vector<std::string> names;
                std::string body;

                for (const auto &[name, command] : actions) {
                    names.push_back(name);
                    body += "print -r -- " +
                            android_shell_quote("VOYEMU_SECTION_" + transaction.health_marker +
                                                ":" + name) +
                            "; " + command + "; r=$?; [ \"$r\" -eq 0 ] || exit \"$r\"; ";
                }

                transaction.capture_limit = 512 * 1024;
                auto result = read({"shell", "sh", "-c", body});

                if (!result) {
                    return;
                }

                const auto sections = windows_startup_sections(result->output,
                                                               transaction.health_marker, names);

                if (startup_line(sections[0]) != "0" || startup_line(sections[1]) != "Enforcing") {
                    invalidate_windows_android_readiness();
                    transaction.phase = Phase::identity;
                    break;
                }

                if (startup_line(sections[2]) != "1") {
                    invalidate_windows_android_readiness();
                    transaction.phase = Phase::boot_completed;
                    break;
                }

                if (android_spec_.contains("wheel_adapter")) {
                    const auto receipt = require_reply({0, sections[7], false});

                    if (receipt.value("schema", 0) != 1 ||
                        receipt.value("session_id", std::string{}) != session_id_ ||
                        receipt.value("status", std::string{}) != "session-ready") {
                        invalidate_windows_android_readiness();
                        transaction.phase = Phase::wheel_verify;
                        break;
                    }

                    wheel_provisioned_ = true;
                    transaction.attempted_mutations.erase("wheel-bootstrap");
                    android["wheel_bootstrap"] = {
                            {"status", "session-ready"},
                            {"guest_acknowledged", true},
                            {"transport", "protected provider; app writes its own private data"}};
                }

                const auto state = model_.state();

                if (android_spec_.contains("vehicle_adapter")) {
                    const auto receipt = require_reply({0, sections[6], false});

                    if (!android_vehicle_receipt_matches(receipt, state)) {
                        invalidate_windows_android_readiness();
                        transaction.phase = Phase::state_prior;
                        break;
                    }

                    android_revision_ = state.at("sequence").get<std::uint32_t>();
                    android.update({{"guest_acknowledged", true},
                                    {"sequence", android_revision_},
                                    {"fields", state.at("raw_oem").size()},
                                    {"typed_projection",
                                     receipt.value("typed_projection", Json::object())}});
                }

                if (android_spec_.contains("home")) {
                    if (resolved_home_component(
                                sections[3],
                                android_spec_.at("home").at("package").get<std::string>()) !=
                        std::optional<std::string>{transaction.component}) {
                        invalidate_windows_android_readiness();
                        transaction.phase = Phase::home_default;
                        break;
                    }

                    if (!primary_home_visible(sections[5], transaction.component)) {
                        invalidate_windows_android_readiness();
                        transaction.phase = Phase::home_windows;
                        break;
                    }

                    primary_home_ready_ = true;
                    transaction.attempted_mutations.erase("home-default");
                    transaction.attempted_mutations.erase("home-start");
                    android["home"] = {{"status", "visible_on_primary"},
                                       {"component", transaction.component},
                                       {"display_id", 0}};
                }

                if (multiple) {
                    const auto secondary = secondary_display_ready(sections[4], sections[5]);

                    if (!secondary || (android_spec_.contains("systemui_adapter") &&
                                       !oem_status_bars_ready(sections[5], *secondary))) {
                        invalidate_windows_android_readiness();
                        transaction.phase = Phase::display_read;
                        break;
                    }

                    android_displays_ready_ = true;
                    android["displays"] = {
                            {"status", "both_guest_displays_ready"},
                            {"secondary_display_id", *secondary},
                            {"size", Json::array({3840, 720})},
                            {"source", "matching visible secondary launcher and display geometry"}};

                    if (android_spec_.contains("systemui_adapter")) {
                        android["systemui"] = {
                                {"status", "both_OEM_bars_visible"},
                                {"primary_display_id", 0},
                                {"secondary_display_id", *secondary},
                                {"bar_size", Json::array({1920, 50})},
                                {"source", "com.qinggan.systemui; exact visible windows"}};
                    }

                    for (const char *attempt :
                         {"secondary-wake", "secondary-service", "systemui-service"}) {
                        transaction.attempted_mutations.erase(attempt);
                    }
                }

                observe_language(sections[8]);

                if (const auto light = host_light_theme(sections[9])) {
                    android["appearance"] = {{"status", "observed"},
                                             {"scheme", *light ? "light" : "dark"},
                                             {"oem_theme", *light ? "simple" : "default"},
                                             {"source", "persist.sys.qg.currenttheme"}};
                }

                std::vector<std::string> theme_replies{sections[11]};

                if (oem_theme_queries(android_spec_).size() > 1) {
                    theme_replies.insert(theme_replies.begin(), sections[10]);
                }

                if (const auto value = oem_theme_state(android_spec_, theme_replies)) {
                    observe_oem_theme(*value);
                }

                if (android_revision_ != model_.state().at("sequence").get<std::uint32_t>()) {
                    transaction.phase = Phase::state_prior;
                    break;
                }

                transaction.attempted_mutations.erase("root");
                android["transport"] = {{"status", "root_ready"},
                                        {"uid", 0},
                                        {"selinux", "Enforcing"},
                                        {"source", "owned framed maintenance observation"}};
                update_clock();
                android["status"] = "guest_acknowledged";
                android["guest_acknowledged"] = true;
                android_identity_ = true;
                android_clock_observed_ = true;
                android_boot_completed_ = true;
                android_transport_ready_ = true;
                transaction.last_observation = transaction.observed_arrival;
                transaction.next_health = WindowsAndroidTransaction::Clock::now() +
                                          std::chrono::seconds(3);
                transaction.phase = Phase::ready;
                transaction.health_marker.clear();
                android.erase("last_error");
                android["stage_errors"] = Json::object();

                return;
            }
            case Phase::identity:
            case Phase::root_verify: {
                const auto phase = transaction.phase;
                auto result = read({"shell", "id", "-u"});

                if (!result) {
                    return;
                }

                const auto uid = startup_line(result->output);

                if (uid != "0" && uid != "2000") {
                    throw VoyemuError("ANDROID_ROOT", "unexpected SDK UID");
                }

                if (phase == Phase::root_verify && uid != "0") {
                    throw VoyemuError("ANDROID_ROOT", "ambiguous root submission not verified");
                }

                if (android_transport_boot_.empty()) {
                    android_transport_boot_ = transaction.boot;
                } else if (android_transport_boot_ != transaction.boot) {
                    throw VoyemuError("ANDROID_IDENTITY_MISMATCH", "startup boot changed");
                }

                if (android_resume_) {
                    android["quickboot"]["mode"] =
                            transaction.nonce == session_id_ ? "sdk_cold_fallback" : "restored";
                }

                android_boot_nonce_ = transaction.nonce;
                android_resume_ = transaction.nonce != session_id_;
                android_identity_ = true;
                android_seen_guest_ = true;
                android_identity_mismatch_ = false;
                android_identity_proof_operation_ = android_identity_operation_;
                android["native_listener_ownership"] = true;
                transaction.root_proof_operation = android_identity_operation_;
                transaction.phase = uid == "0" ? Phase::enforcing : Phase::root_submit;
                break;
            }
            case Phase::root_submit: {
                if (!android_identity_probe_.pending() &&
                    transaction.root_proof_operation != android_identity_operation_) {
                    transaction.phase = Phase::identity;
                    break;
                }

                auto result = poll_windows_android_action({"root"}, true, "root");

                if (!result) {
                    return;
                }

                // Regardless of submission status, only a readback may follow; root is never
                // replayed.
                transaction.phase = Phase::root_verify;
                break;
            }
            case Phase::enforcing: {
                auto result = read({"shell", "getenforce"});

                if (!result) {
                    return;
                }

                if (startup_line(result->output) != "Enforcing") {
                    throw VoyemuError("ANDROID_ROOT", "Enforcing required");
                }

                android_transport_ready_ = true;
                android["transport"] = {{"status", "root_ready"},
                                        {"uid", 0},
                                        {"selinux", "Enforcing"},
                                        {"source", "owned framed startup bootstrap"}};
                transaction.phase = Phase::framework_ready;
                break;
            }
            case Phase::clock_read:
            case Phase::clock_verify: {
                const auto phase = transaction.phase;
                auto result = read({"shell", "date", "+%s"});

                if (!result) {
                    return;
                }

                if (!android_clock_current(transaction.observed_utc,
                                           transaction.observed_host_utc_ms) &&
                    phase == Phase::clock_read &&
                    !transaction.attempted_mutations.contains("clock")) {
                    transaction.phase = Phase::clock_service;
                    break;
                }

                if (!android_transport_ready_) {
                    transaction.phase = Phase::identity;
                    break;
                }

                android_clock_observed_ = true;
                update_clock();
                transaction.phase = windows_framework_after_clock(
                        android.value("framework", Json::object()).value("status", std::string{}) ==
                                "ready",
                        android_spec_.contains("wheel_adapter"),
                        transaction.ever_ready || wheel_provisioned_ ||
                                transaction.attempted_mutations.contains("wheel-bootstrap"));
                break;
            }
            case Phase::clock_service: {
                auto result = read({"shell", "service", "check", "alarm"});

                if (!result) {
                    return;
                }

                if (!windows_alarm_service_available(*result)) {
                    update_clock();
                    android["clock"]["correction_result"] = "service_unavailable";
                    android_clock_observed_ = true;
                    transaction.phase = windows_framework_after_clock(
                            android.value("framework", Json::object())
                                            .value("status", std::string{}) == "ready",
                            android_spec_.contains("wheel_adapter"),
                            transaction.ever_ready || wheel_provisioned_ ||
                                    transaction.attempted_mutations.contains("wheel-bootstrap"));
                    break;
                }

                transaction.phase = Phase::clock_set;
                break;
            }
            case Phase::framework_ready: {
                auto result =
                        poll_windows_android_action({"shell", windows_framework_probe_command()});

                if (!result) {
                    return;
                }

                const auto ready = windows_framework_readiness(*result);
                android["framework"] = {
                        {"status", ready.state == WindowsFrameworkState::ready     ? "ready"
                                   : ready.state == WindowsFrameworkState::invalid ? "invalid"
                                                                                   : "pending"},
                        {"activity", ready.activity},
                        {"package", ready.package},
                        {"boot_completed", ready.boot_completed},
                        {"source", "owned framed native service/property read"}};
                transaction.phase = windows_framework_next(ready);

                if (transaction.phase == Phase::framework_ready) {
                    android["last_error"] =
                            "ANDROID_FRAMEWORK_NOT_READY: waiting for activity, package and boot completion";

                    return;
                }

                if (transaction.phase == Phase::clock_read) {
                    transaction.phase = windows_after_framework_ready(
                            windows_startup_services_required(android_spec_));
                }

                android.erase("last_error");
                break;
            }
            case Phase::startup_audio: {
                transaction.startup_services = true;
                struct Restore {
                    bool &value;

                    ~Restore() {
                        value = false;
                    }
                } restore{transaction.startup_services};
                try {
                    prepare_android_audio();
                } catch (const std::exception &error) {
                    record_guard(error);

                    return;
                }

                clear_guard();
                transaction.phase =
                        windows_after_startup_audio(android_spec_.contains("interguest_network"));
                break;
            }
            case Phase::startup_interguest: {
                transaction.startup_services = true;
                struct Restore {
                    bool &value;

                    ~Restore() {
                        value = false;
                    }
                } restore{transaction.startup_services};
                try {
                    configure_android_interguest_network();
                } catch (const std::exception &error) {
                    record_guard(error);

                    return;
                }

                clear_guard();
                transaction.phase = Phase::clock_read;
                break;
            }
            case Phase::clock_set: {
                if (transaction.pending_arguments.empty() || !android_identity_probe_.pending()) {
                    transaction.pending_arguments = {
                            "shell", "cmd", "alarm", "set-time",
                            std::to_string(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count())};
                }

                auto result = poll_windows_android_action(transaction.pending_arguments, true,
                                                          "clock");

                if (!result) {
                    return;
                }

                transaction.clock_exit_code = result->timed_out ? -1 : result->exit_code;
                transaction.phase = Phase::clock_verify;
                break;
            }
            case Phase::wheel_bootstrap: {
                auto result = poll_windows_android_action(
                        provider("com.voyemu.input.wheel", "bootstrap"), true, "wheel-bootstrap");

                if (!result) {
                    return;
                }

                transaction.phase = Phase::wheel_verify;
                break;
            }
            case Phase::wheel_verify: {
                auto result = read(provider("com.voyemu.input.wheel", "status"));

                if (!result) {
                    return;
                }

                const auto value = require_reply(*result);

                if (!value.is_object() || value.value("schema", 0) != 1 ||
                    value.value("session_id", std::string{}) != session_id_ ||
                    value.value("status", std::string{}) != "session-ready") {
                    throw VoyemuError("ANDROID_PROVISION",
                                      "matching protected bootstrap receipt unavailable");
                }

                wheel_provisioned_ = true;
                android["wheel_bootstrap"] = {
                        {"status", "session-ready"},
                        {"guest_acknowledged", true},
                        {"transport", "protected provider; app writes its own private data"}};
                transaction.phase = Phase::state_prior;
                break;
            }
            case Phase::state_prior: {
                transaction.submitted_state = model_.state();

                if (!android_spec_.contains("vehicle_adapter")) {
                    transaction.phase = Phase::state_submit;
                    break;
                }

                auto result = read(provider("com.voyemu.output.vehicle", "status"));

                if (!result) {
                    return;
                }

                const auto receipt = require_reply(*result);

                if (receipt.value("schema", 0) == 1 &&
                    receipt.value("status", std::string{}) == "applied") {
                    auto observed = model_.state();
                    observed["sequence"] = receipt.at("sequence");
                    observed["raw_oem"] = receipt.at("raw_oem");
                    validate_state(observed);

                    if (!android_vehicle_receipt_matches(receipt, observed)) {
                        throw VoyemuError("ANDROID_STATE_RECEIPT",
                                          "invalid retained full-state publication");
                    }

                    transaction.submitted_state = model_.state();
                    const auto floor = observed.at("sequence").get<std::uint32_t>();

                    if (floor >= transaction.submitted_state.at("sequence").get<std::uint32_t>() &&
                        !android_vehicle_receipt_matches(receipt, transaction.submitted_state)) {
                        transaction.submitted_state = model_.advance_revision(floor);
                        write_private_json(directory_ / "vehicle-state.json",
                                           transaction.submitted_state, vehicle_state_bytes);
                    }

                    if (android_vehicle_receipt_matches(receipt, transaction.submitted_state)) {
                        transaction.phase = Phase::state_verify;
                        break;
                    }
                } else if (receipt.value("schema", 0) != 1 ||
                           receipt.value("status", std::string{}) != "not-ready") {
                    throw VoyemuError("ANDROID_STATE_RECEIPT", "retained publication unavailable");
                }

                transaction.phase = Phase::state_submit;
                break;
            }
            case Phase::state_submit: {
                const auto sequence =
                        transaction.submitted_state.at("sequence").get<std::uint32_t>();
                const auto attempt = "state:" + std::to_string(sequence);

                if (!android_identity_probe_.pending() &&
                    transaction.attempted_mutations.contains(attempt)) {
                    transaction.phase = Phase::state_verify;
                    break;
                }

                // The model-backed APK declares CanBusService and keeps RegistrationService as its
                // base class, so the component follows the installed helper exactly as it does on
                // the native path.
                std::vector<std::string> submission{
                        "am",
                        "startservice",
                        "-n",
                        qinggan09_canbus_service_component(android_spec_),
                        "-a",
                        "com.voyemu.canbus.APPLY_PERSISTENT_VEHICLE_STATE",
                        "--es",
                        "state_json",
                        transaction.submitted_state.dump()};
                const auto power = android_power_extra();
                submission.insert(submission.end(), power.begin(), power.end());
                auto result = poll_windows_android_action({"shell", startup_shell(submission)},
                                                          true, attempt);

                if (!result) {
                    return;
                }

                android.update({{"status", "submitted_unverified"},
                                {"sequence", sequence},
                                {"guest_acknowledged", false},
                                {"source", "owned full-input adapter"}});
                transaction.phase = Phase::state_verify;
                break;
            }
            case Phase::state_verify: {
                if (android_spec_.contains("vehicle_adapter")) {
                    auto result = read(provider("com.voyemu.output.vehicle", "status"));

                    if (!result) {
                        return;
                    }

                    const auto receipt = require_reply(*result);

                    if (!android_vehicle_receipt_matches(receipt, transaction.submitted_state)) {
                        if (android_vehicle_receipt_pending(receipt, transaction.submitted_state)) {
                            return;
                        }

                        throw VoyemuError("ANDROID_STATE_RECEIPT",
                                          "malformed or foreign submitted publication");
                    }

                    android.update({{"status", "protected_state_verified"},
                                    {"guest_acknowledged", false},
                                    {"fields", transaction.submitted_state.at("raw_oem").size()},
                                    {"transport", "DUMP-protected read-only provider"},
                                    {"typed_projection",
                                     receipt.value("typed_projection", Json::object())},
                                    {"visible_effect_verified", false}});
                }

                android_revision_ = transaction.submitted_state.at("sequence").get<std::uint32_t>();
                android["sequence"] = android_revision_;

                for (auto it = transaction.attempted_mutations.begin();
                     it != transaction.attempted_mutations.end();) {
                    if (it->starts_with("state:")) {
                        it = transaction.attempted_mutations.erase(it);
                    } else {
                        ++it;
                    }
                }

                transaction.phase = transaction.ever_ready ? Phase::health : Phase::boot_completed;
                break;
            }
            case Phase::boot_completed: {
                auto result = read({"shell", "getprop", "sys.boot_completed"});

                if (!result) {
                    return;
                }

                if (startup_line(result->output) != "1") {
                    return;
                }

                android_boot_completed_ = true;
                transaction.phase = android_spec_.contains("home") ? Phase::home_qualified
                                                                   : Phase::display_read;
                break;
            }
            case Phase::home_qualified: {
                auto result = read(home_args(true));

                if (!result) {
                    return;
                }

                const auto package = android_spec_.at("home").at("package").get<std::string>();
                auto component = resolved_home_component(result->output, package);

                if (android_spec_.at("home").contains("component")) {
                    const auto configured = resolved_home_component(
                            android_spec_.at("home").at("component").get<std::string>(), package);

                    if (!configured) {
                        throw VoyemuError("HOME_MISSING", "invalid configured primary component");
                    }

                    transaction.component = *configured;

                    if (component != configured) {
                        transaction.phase = Phase::home_declared;
                        break;
                    }
                } else {
                    if (!component) {
                        throw VoyemuError("HOME_MISSING",
                                          "primary does not resolve in its package");
                    }

                    transaction.component = *component;
                }

                transaction.phase = Phase::home_default;
                break;
            }
            case Phase::home_declared: {
                auto arguments = home_args(true);
                arguments[3] = "query-activities";
                auto result = read(arguments);

                if (!result) {
                    return;
                }

                if (!home_component_declared(
                            result->output, transaction.component,
                            android_spec_.at("home").at("package").get<std::string>())) {
                    throw VoyemuError("HOME_MISSING",
                                      "configured primary is not a declared HOME activity");
                }

                transaction.phase = Phase::home_default;
                break;
            }
            case Phase::home_default:
            case Phase::home_verify: {
                const auto phase = transaction.phase;
                auto result = read(home_args(false));

                if (!result) {
                    return;
                }

                if (resolved_home_component(
                            result->output,
                            android_spec_.at("home").at("package").get<std::string>()) !=
                    std::optional<std::string>{transaction.component}) {
                    if (phase == Phase::home_verify) {
                        throw VoyemuError("HOME_NOT_PERSISTED", "primary restoration not verified");
                    }

                    transaction.phase = Phase::home_set;
                } else {
                    transaction.phase = Phase::home_windows;
                }

                break;
            }
            case Phase::home_set: {
                auto result =
                        poll_windows_android_action({"shell", "cmd", "package", "set-home-activity",
                                                     "--user", "0", transaction.component},
                                                    true, "home-default");

                if (!result) {
                    return;
                }

                transaction.phase = Phase::home_verify;
                break;
            }
            case Phase::home_windows: {
                auto result = read({"shell", "dumpsys", "window", "windows"});

                if (!result) {
                    return;
                }

                if (!primary_home_visible(result->output, transaction.component)) {
                    if (transaction.attempted_mutations.contains("home-start")) {
                        return;
                    }

                    transaction.phase = Phase::home_start;
                    break;
                }

                primary_home_ready_ = true;
                android["home"] = {{"status", "visible_on_primary"},
                                   {"component", transaction.component},
                                   {"display_id", 0}};
                transaction.phase = Phase::display_read;
                break;
            }
            case Phase::home_start: {
                auto result = poll_windows_android_action(
                        primary_home_start_command(transaction.component), true, "home-start");

                if (!result) {
                    return;
                }

                record_home_start(android, *result, {session_id_, android_boot_nonce_});
                transaction.phase = Phase::home_windows;
                break;
            }
            case Phase::display_read: {
                const bool multiple =
                        android_spec_.contains("features") &&
                        android_spec_.at("features").value("MultiDisplay", std::string{}) == "on";

                if (!multiple) {
                    transaction.phase = Phase::language;
                    break;
                }

                auto result = read({"shell", "dumpsys", "display"});

                if (!result) {
                    return;
                }

                transaction.display_text = result->output;
                transaction.phase = Phase::display_windows;
                break;
            }
            case Phase::display_windows: {
                auto result = read({"shell", "dumpsys", "window", "windows"});

                if (!result) {
                    return;
                }

                transaction.window_text = result->output;
                const auto secondary = secondary_display_ready(transaction.display_text,
                                                               transaction.window_text);

                if (!secondary || (android_spec_.contains("systemui_adapter") &&
                                   !oem_status_bars_ready(transaction.window_text, *secondary))) {
                    android_displays_ready_ = false;
                    android.erase("displays");
                    android.erase("systemui");

                    if (!transaction.attempted_mutations.contains("secondary-wake")) {
                        transaction.phase = Phase::secondary_wake;
                    } else {
                        transaction.phase = Phase::display_read;
                    }

                    return;
                }

                android_displays_ready_ = true;
                android["displays"] = {
                        {"status", "both_guest_displays_ready"},
                        {"secondary_display_id", *secondary},
                        {"size", Json::array({3840, 720})},
                        {"source", "matching visible secondary launcher and display geometry"}};

                if (android_spec_.contains("systemui_adapter")) {
                    android["systemui"] = {
                            {"status", "both_OEM_bars_visible"},
                            {"primary_display_id", 0},
                            {"secondary_display_id", *secondary},
                            {"bar_size", Json::array({1920, 50})},
                            {"source", "com.qinggan.systemui; exact visible windows"}};
                }

                transaction.phase = Phase::language;
                break;
            }
            case Phase::secondary_wake: {
                auto result = poll_windows_android_action(
                        {"shell", "am", "broadcast", "--user", "0", "-a",
                         "com.android.emulator.multidisplay.START", "-n",
                         "com.android.emulator.multidisplay/.MultiDisplayServiceReceiver"},
                        true, "secondary-wake");

                if (!result) {
                    return;
                }

                transaction.phase = Phase::secondary_service;
                break;
            }
            case Phase::secondary_service: {
                auto result = poll_windows_android_action(
                        {"shell", "am", "startservice", "-n",
                         "com.voyemu/com.voyemu.output.SecondScreenService"},
                        true, "secondary-service");

                if (!result) {
                    return;
                }

                transaction.phase = android_spec_.contains("systemui_adapter")
                                            ? Phase::systemui_service
                                            : Phase::display_read;
                break;
            }
            case Phase::systemui_service: {
                auto result = poll_windows_android_action({"shell", "am", "startservice", "-n",
                                                           "com.qinggan.systemui/.SystemUIService"},
                                                          true, "systemui-service");

                if (!result) {
                    return;
                }

                transaction.phase = Phase::display_read;
                break;
            }
            case Phase::language: {
                if (transaction.pending_arguments.empty() || !android_identity_probe_.pending()) {
                    transaction.settings_revision =
                            model_.state().at("sequence").get<std::uint32_t>();
                }

                auto result = read({"shell", "content", "query", "--uri",
                                    "content://qinggan.settings/system/system_language_setting",
                                    "--projection", "value"});

                if (!result) {
                    return;
                }

                observe_language(result->output);

                transaction.phase = Phase::appearance;
                break;
            }
            case Phase::appearance: {
                auto result = read({"shell", "getprop", "persist.sys.qg.currenttheme"});

                if (!result) {
                    return;
                }

                if (const auto light = host_light_theme(result->output)) {
                    android["appearance"] = {{"status", "observed"},
                                             {"scheme", *light ? "light" : "dark"},
                                             {"oem_theme", *light ? "simple" : "default"},
                                             {"source", "persist.sys.qg.currenttheme"}};
                } else {
                    android["appearance"] = {{"status", "unsupported"},
                                             {"source", "persist.sys.qg.currenttheme"}};
                }

                transaction.phase = Phase::theme_first;
                break;
            }
            case Phase::theme_first: {
                const auto queries = oem_theme_queries(android_spec_);
                transaction.theme_replies.clear();

                if (queries.size() == 1) {
                    transaction.phase = Phase::theme_second;
                    break;
                }

                auto result = read(queries.front());

                if (!result) {
                    return;
                }

                transaction.theme_replies.push_back(result->output);
                transaction.phase = Phase::theme_second;
                break;
            }
            case Phase::theme_second: {
                auto result = read(oem_theme_queries(android_spec_).back());

                if (!result) {
                    return;
                }

                transaction.theme_replies.push_back(result->output);

                if (const auto value = oem_theme_state(android_spec_, transaction.theme_replies)) {
                    observe_oem_theme(*value);
                }

                if (android_revision_ != model_.state().at("sequence").get<std::uint32_t>()) {
                    transaction.phase = Phase::state_prior;
                    break;
                }

                update_clock();
                android["guest_acknowledged"] = true;
                android["status"] = "guest_acknowledged";
                transaction.ever_ready = true;
                transaction.last_observation = transaction.observed_arrival;
                transaction.next_health = WindowsAndroidTransaction::Clock::now() +
                                          std::chrono::seconds(3);
                transaction.phase = Phase::ready;
                android.erase("last_error");
                android["stage_errors"] = Json::object();
                android["startup_observation"] = {
                        {"age_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                           WindowsAndroidTransaction::Clock::now() -
                                           transaction.last_observation)
                                           .count()},
                        {"maximum_age_ms", 63000},
                        {"source", "owned framed Windows guest transaction"}};

                return;
            }
            case Phase::ready:
                return;
            case Phase::failed:
                throw std::runtime_error(transaction.failure);
            }
        }
    } catch (const std::exception &error) {
        invalidate_windows_android_readiness();
        const std::string message = error.what();

        if (message.starts_with("ANDROID_IDENTITY_MISMATCH:")) {
            android_identity_mismatch_ = true;
        }

        if (message.starts_with("ANDROID_IDENTITY_MISMATCH:") ||
            message.starts_with("ANDROID_IDENTITY_NOT_READY:") ||
            message.starts_with("TEST_BUSY:")) {
            if (const auto post = startup_post_write(transaction.phase);
                transaction.pending_mutation && post) {
                transaction.phase = *post;
            } else if (!android_identity_probe_.pending() &&
                       transaction.phase != Phase::boot_recovery) {
                transaction.phase = Phase::identity;
            }
        } else {
            transaction.phase = Phase::failed;
            transaction.failure = message;
        }

        throw;
    }
}
} // namespace voyemu
