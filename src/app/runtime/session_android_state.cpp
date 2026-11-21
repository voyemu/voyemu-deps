#include "error.hpp"
#include "session_internal.hpp"

namespace voyemu {
using namespace session_detail;

void Session::synchronize_android_clock() {
    const bool nxp = nxp_presentation(android_spec_);
    const auto host_ms = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
    };
    auto publish = [&]() {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - android_clock_arrival_)
                                 .count();
        const auto host = host_ms();
        const bool synced = android_clock_synced(android_clock_guest_seconds_,
                                                 android_clock_host_ms_, host, age);
        deliveries_["android"]["clock"] = {
                {"status", synced ? "host_time_verified" : "degraded"},
                {"synced", synced},
                {"corrected", android_clock_correction_attempted_ && synced},
                {"correction_attempted", android_clock_correction_attempted_},
                {"correction_result",
                 android_clock_correction_attempted_
                         ? (android_clock_exit_code_ < 0    ? "unknown"
                            : android_clock_exit_code_ == 0 ? "command_succeeded"
                                                            : "command_failed")
                         : "not_attempted"},
                {"command_exit_code", android_clock_exit_code_},
                {"offset_ms", android_clock_host_ms_ - android_clock_guest_seconds_ * 1000},
                {"observation_age_ms", age},
                {"host_clock_stable",
                 android_clock_arrival_valid(android_clock_host_ms_, host, age)},
                {"source", "owned SDK UTC readback after boot and nonce verification"}};
    };

    if (android_clock_observed_) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - android_clock_arrival_)
                                 .count();

        if (android_clock_observation_current(age) &&
            (!nxp || !android_boot_completed_ || android_clock_correction_attempted_ ||
             android_clock_current(android_clock_guest_seconds_, android_clock_host_ms_))) {
            publish();

            return;
        }

        android_clock_observed_ = false;
        deliveries_["android"].erase("clock");
    }

    auto fence = [&]() {
        assert_android_identity();
        const auto boot = adb({"shell", "cat", "/proc/sys/kernel/random/boot_id"}, 1500);

        if (boot.timed_out || boot.exit_code) {
            throw VoyemuError("ANDROID_IDENTITY_NOT_READY", "clock boot observation unavailable");
        }

        auto value = boot.output;

        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
            value.pop_back();
        }

        if (!android_boot_id_valid(value) || value != android_transport_boot_) {
            throw VoyemuError("ANDROID_IDENTITY_MISMATCH",
                              "guest rebooted during clock observation");
        }

        assert_android_identity();
    };
    auto read_clock = [&]() {
        fence();
        auto reply = adb({"shell", "date", "+%s"}, 1500);

        if (reply.timed_out || reply.exit_code != 0) {
            throw VoyemuError("ANDROID_CLOCK", "epoch read unavailable");
        }

        auto epoch = android_epoch_seconds(reply.output);

        if (!epoch) {
            throw VoyemuError("ANDROID_CLOCK", "bounded epoch read required");
        }

        const auto host = host_ms();
        const auto arrival = std::chrono::steady_clock::now();
        fence();
        android_clock_guest_seconds_ = *epoch;
        android_clock_host_ms_ = host;
        android_clock_arrival_ = arrival;
    };
    read_clock();
    bool correction_ready = !nxp || android_boot_completed_;

    if (nxp && correction_ready && !android_clock_correction_attempted_ &&
        !android_clock_current(android_clock_guest_seconds_, android_clock_host_ms_)) {
        const auto service = adb({"shell", "service", "check", "alarm"}, 1500);
        correction_ready = nxp_clock_service_ready(android_boot_completed_, service.exit_code,
                                                   service.timed_out, service.output);
    }

    if (correction_ready &&
        !android_clock_current(android_clock_guest_seconds_, android_clock_host_ms_) &&
        !android_clock_correction_attempted_) {
        // Mark before dispatch: an exception or ambiguous command result must never replay a
        // mutation.
        android_clock_correction_attempted_ = true;
        android_clock_exit_code_ = -1;
        const auto target = host_ms();
        const auto started = std::chrono::steady_clock::now();
        const auto steady_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(started.time_since_epoch())
                        .count();
        auto evidence = [&](const ProcessResult *result, const char *status) noexcept {
            try {
                logging::write(
                        logging::Level::info, "android", "clock.caller",
                        {{"operation", "alarm.set-time"},
                         {"phase", status},
                         {"target_utc_ms", target},
                         {"host_steady_start_ms", steady_ms},
                         {"host_elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     std::chrono::steady_clock::now() - started)
                                                     .count()},
                         {"sample", "post-spawn-not-atomic"},
                         {"exit_code", result ? result->exit_code : -1},
                         {"timed_out", result && result->timed_out},
                         {"merged_stdout_stderr",
                          result ? result->output.substr(0, 8192) : std::string{}},
                         {"output_bytes", result ? result->output.size() : 0},
                         {"output_truncated", result && result->output.size() > 8192}});
            } catch (...) { /* Ignored: diagnostics cannot change clock delivery. */
            }
        };
        evidence(nullptr, "launch");
        ProcessResult submitted;
        try {
            submitted = adb({"shell", android_clock_caller_command(target)}, 1500);
        } catch (...) {
            evidence(nullptr, "exception");
            throw;
        }

        evidence(&submitted, "result");
        android_clock_exit_code_ = submitted.timed_out ? -1 : submitted.exit_code;
        // OEM exit 255 is ambiguous; only an authenticated readback describes synchronization
        // quality.
        read_clock();
    }

    android_clock_observed_ = true;
    publish();
}

void Session::schedule_android_state(std::uint32_t previous_revision) {
    // Coalesce changes behind an already-dirty revision; do not defeat a bounded failure backoff.
    if (android_identity_ && android_clock_observed_ && android_revision_ == previous_revision &&
        !deliveries_["android"].contains("last_error")) {
        next_android_ = std::min(next_android_, std::chrono::steady_clock::now());
    }
}

void Session::deliver_android_state() {
    prepare_android_canbus();

    if (nxp_presentation(android_spec_)) {
        deliveries_["android"]["vehicle"] = {{"status", "unsupported"}, {"supported", false}};

        return;
    }

    if (passion_presentation(android_spec_)) {
        if (!passion_mcu_available(android_spec_)) {
            deliveries_["android"]["vehicle"] = {{"status", "unsupported"}, {"supported", false}};

            return;
        }

        deliveries_["android"]["vehicle"] = {{"status", "publication_unverified"},
                                             {"projection_acknowledged", false},
                                             {"guest_acknowledged", false},
                                             {"full_wire_verified", false}};
        const auto delivery = passion_mcu_deliver(
                android_transport_boot_,
                [&] {
                    return model_.state();
                },
                [&](std::uint32_t floor) {
                    model_.advance_revision(floor);
                    write_private_json(directory_ / "vehicle-state.json", model_.state(),
                                       vehicle_state_bytes);
                },
                [&](const std::vector<std::string> &args) {
                    std::string command;

                    for (const auto &arg : args) {
                        if (!command.empty()) {
                            command += ' ';
                        }

                        command += android_shell_quote(arg);
                    }

                    const auto reply = adb({"shell", command}, 2500);

                    if (reply.exit_code || reply.timed_out) {
                        throw VoyemuError("PASSION_MCU", "protected provider unavailable");
                    }

                    return parse_android_provider_reply(reply.output);
                });
        deliveries_["android"]["vehicle"] = delivery;
        android_revision_ = delivery.at("sequence").get<std::uint32_t>();

        return;
    }

    auto state = model_.state();
    auto read_receipt = [&]() {
        auto authority = android_spec_.at("vehicle_adapter").at("authority").get<std::string>();

        if (authority != "com.voyemu.output.vehicle") {
            throw VoyemuError("ANDROID_STATE_PROVIDER", "closed authority required");
        }

        std::vector<std::string> values{"content",  "call",  "--uri", "content://" + authority,
                                        "--method", "status"};
        std::string query;

        for (const auto &value : values) {
            if (!query.empty()) {
                query += ' ';
            }

            query += android_shell_quote(value);
        }

        auto readback = adb({"shell", query}, 2500);

        if (readback.exit_code || readback.timed_out) {
            throw VoyemuError("ANDROID_STATE_RECEIPT", "protected publication unavailable");
        }

        return parse_android_provider_reply(readback.output);
    };

    if (android_revision_ == 0 && android_spec_.contains("vehicle_adapter")) {
        const auto receipt = read_receipt();
        // A valid manual patch may have arrived during the retained receipt read.
        state = model_.state();

        if (receipt.value("schema", 0) == 1 &&
            receipt.value("status", std::string{}) == "applied") {
            auto observed = state;
            observed["sequence"] = receipt.at("sequence");
            observed["raw_oem"] = receipt.at("raw_oem");

            if (!android_vehicle_receipt_matches(receipt, observed)) {
                throw VoyemuError("ANDROID_STATE_RECEIPT",
                                  "invalid retained full-state publication");
            }

            const auto floor = observed.at("sequence").get<std::uint32_t>();

            if (floor >= state.at("sequence").get<std::uint32_t>() &&
                !android_vehicle_receipt_matches(receipt, state)) {
                // The guest persists a monotonic revision, including across Quick Boot. Never
                // import its values.
                state = model_.advance_revision(floor);
                write_private_json(directory_ / "vehicle-state.json", state, vehicle_state_bytes);
                logging::write(
                        logging::Level::info, "android", "state.revision_advanced",
                        {{"retained_sequence", floor}, {"source_sequence", state.at("sequence")}});
            }
        } else if (receipt.value("schema", 0) != 1 ||
                   receipt.value("status", std::string{}) != "not-ready") {
            throw VoyemuError("ANDROID_STATE_RECEIPT", "retained publication unavailable");
        }
    }

    std::vector<std::string> args{"am",        "startservice",
                                  "-n",        qinggan09_canbus_service_component(android_spec_),
                                  "-a",        "com.voyemu.canbus.APPLY_PERSISTENT_VEHICLE_STATE",
                                  "--es",      "state_json",
                                  state.dump()};
    const auto power = android_power_extra();
    args.insert(args.end(), power.begin(), power.end());
    std::string command;

    for (const auto &arg : args) {
        if (!command.empty()) {
            command += ' ';
        }

        command += android_shell_quote(arg);
    }

    auto result = adb({"shell", command}, 2500);

    if (result.exit_code || result.timed_out || result.output.find("Error") != std::string::npos) {
        throw VoyemuError("ANDROID_STATE", "adapter submission failed");
    }

    auto sequence = state.at("sequence").get<std::uint32_t>();
    deliveries_["android"].update({{"status", "submitted_unverified"},
                                   {"sequence", sequence},
                                   {"source", "owned full-input adapter"},
                                   {"guest_acknowledged", false}});

    if (android_spec_.contains("vehicle_adapter")) {
        auto receipt = read_receipt();

        if (!android_vehicle_receipt_matches(receipt, state)) {
            deliveries_["android"]["status"] = "awaiting_state_receipt";
            throw VoyemuError("ANDROID_STATE_RECEIPT",
                              "full published revision does not match source");
        }

        deliveries_["android"].update(
                {{"status", "guest_acknowledged"},
                 {"guest_acknowledged", true},
                 {"fields", state.at("raw_oem").size()},
                 {"transport", "DUMP-protected read-only provider"},
                 {"typed_projection", receipt.value("typed_projection", Json::object())},
                 {"visible_effect_verified", false}});
    }

    android_revision_ = sequence;
}

Json Session::phone_control(const Json &operation) {
    if (!synthetic_phone_supported(android_spec_)) {
        throw VoyemuError("TEST_UNSUPPORTED",
                          "prepared machine has no synthetic OEM phone adapter");
    }

    const auto method = operation.at("method").get<std::string>();
    const bool mutation = method != "snapshot";

    if (mutation && !operation.contains("ownership_id")) {
        throw VoyemuError("TEST_OWNERSHIP", "phone input requires explicit owned session");
    }

    (void)phone_command(mutation ? "multimedia.phone." + method : "multimedia.phone.state",
                        operation.at("payload"));

    if (stopped_ || !android_powered_ || !android_transport_ready_ ||
        android_transport_boot_.empty()) {
        throw VoyemuError("PHONE_NOT_READY", "owned Android transport is not ready");
    }

    assert_android_identity();
    const auto boot = android_transport_boot_;
    auto fence = [&] {
        assert_android_identity();
        const auto observed = adb({"shell", "cat", "/proc/sys/kernel/random/boot_id"}, 1500);
        auto value = observed.output;
        const auto end = value.find_first_of("\r\n");

        if (end != std::string::npos) {
            value.resize(end);
        }

        if (observed.exit_code || observed.timed_out || value != boot) {
            throw VoyemuError("PHONE_STALE", "Android boot changed during phone control");
        }
    };
    auto exchange = [&](const std::string &verb, const std::string &action, const Json &payload,
                        std::uint64_t sequence) {
        fence();
        const auto args = phone_provider_arguments(verb, session_id_, guest_auth_token_, boot,
                                                   sequence, action, payload,
                                                   verb == "bootstrap" && method == "configure",
                                                   android_spec_.at("api").get<unsigned>());
        std::string command;

        for (const auto &arg : args) {
            if (!command.empty()) {
                command += ' ';
            }

            command += android_shell_quote(arg);
        }

        return phone_provider_exchange(
                verb,
                [&] {
                    const auto result = adb({"shell", command}, 3500);

                    if (result.exit_code || result.timed_out || result.output.size() > 65536 ||
                        result.output.find("SecurityException") != std::string::npos) {
                        throw VoyemuError(
                                "PHONE_DELIVERY_UNKNOWN",
                                "protected peer command did not return a verified receipt");
                    }

                    const auto reply = parse_phone_provider_output(result.output,
                                                                   parse_android_provider_reply);
                    validate_phone_reply(reply, session_id_, boot, sequence, guest_auth_token_,
                                         verb == "bootstrap" ? 0 : phone_provider_pid_);
                    fence();

                    return reply;
                },
                [&] {
                    // Any post-dispatch failure can hide a bootstrap disconnect or an executed
                    // control callback.
                    phone_control_boot_ = boot;
                    phone_control_sequence_ = sequence;
                    phone_control_unknown_ = true;

                    if (verb == "bootstrap") {
                        phone_provider_pid_ = 0;
                    }
                });
    };

    if (mutation) {
        prepare_android_canbus();
    }

    require_phone_outcome_known(boot, phone_control_boot_, phone_control_unknown_, mutation);

    if (phone_control_boot_ != boot) {
        if (!mutation) {
            throw VoyemuError("PHONE_NOT_READY",
                              "synthetic peer has not been configured in this boot");
        }

        if (android_spec_.contains("qinggan09_phone_package")) {
            deliveries_["android"]["qinggan09_phone_package"] = qinggan09_phone_package_exchange(
                    boot, phone_control_boot_, phone_control_unknown_, phone_provider_pid_, [&] {
                        return ensure_qinggan09_phone_package(
                                android_spec_, android_resume_ ? android_boot_nonce_ : session_id_,
                                boot,
                                [&](const auto &args, int timeout) {
                                    return adb(args, timeout);
                                },
                                fence);
                    });
        }

        bootstrap_phone_session(boot, phone_control_boot_, phone_control_sequence_,
                                phone_control_unknown_, phone_provider_pid_, [&] {
                                    return exchange("bootstrap", "snapshot", Json::object(), 0);
                                });
    }

    if (mutation && phone_control_sequence_ == static_cast<std::uint64_t>(INT64_MAX)) {
        throw VoyemuError("PHONE_SEQUENCE", "phone command sequence exhausted");
    }

    // A no-op requires a verified complete snapshot; never dispatch after a failed probe.
    if (mutation && (method == "connect" || method == "disconnect" || method == "hangup")) {
        const auto current = exchange("snapshot", "snapshot", Json::object(),
                                      phone_control_sequence_);

        if (method == "connect") {
            require_phone_connect_power(current.at("data"));
        }

        if (phone_control_idempotent_noop(method, current.at("data"))) {
            auto reply = current;
            reply.erase("session");
            reply["session_id"] = session_id_;
            reply["synthetic"] = true;
            reply["idempotent_noop"] = true;
            reply["visible_effect_verified"] = false;

            return reply;
        }
    }

    // Probe guard for configure: the peer contract (PeerConfiguration.configure) rejects configure
    // with IllegalStateException("PHONE_CONNECTED") when connected||state!=TERMINATED — strictly
    // BEFORE any durable mutation. The guest provider surfaces that as
    // stage=adapter_control/reason=PHONE_ADAPTER_CONTROL, which the generic classifier treats as
    // uncertain and quarantines the boot. Nothing was actually delivered: mirror the peer contract
    // locally — reject before dispatch, no quarantine, sequence unused.
    if (mutation && method == "configure" && phone_control_boot_ == android_transport_boot_) {
        const auto current = exchange("snapshot", "snapshot", Json::object(),
                                      phone_control_sequence_);

        if (phone_control_configure_requires_reject(current.at("data"))) {
            throw VoyemuError(
                    "PHONE_PROVIDER_REJECTED",
                    "configure requires an idle disconnected peer (PHONE_CONNECTED per the synthetic peer contract)");
        }
    }

    // The same reasoning covers the call-path contract (SyntheticPhonePeer):
    //   incoming/dial: requireConnected() then state==TERMINATED (else PHONE_CALL_ACTIVE);
    //   answer: requireConnected() then state INCOMING|DIALING (else PHONE_NOT_RINGING_OR_DIALING);
    //   hangup: requireConnected() then state!=TERMINATED (else PHONE_NO_CALL — but the call moves
    //   are already covered by the idempotent no-op probe);
    // every one of those throws strictly before any durable mutation, so a local mirror keeps the
    // outcome known (no dispatch → nothing delivered → no boot quarantine).
    if (mutation && (method == "incoming" || method == "answer" || method == "hangup") &&
        phone_control_boot_ == android_transport_boot_) {
        const auto current = exchange("snapshot", "snapshot", Json::object(),
                                      phone_control_sequence_);
        const auto &data = current.at("data");

        if (phone_control_call_requires_reject(method, data)) {
            throw VoyemuError(
                    "PHONE_PROVIDER_REJECTED",
                    std::string(method) +
                            " rejected by the synthetic peer call-state contract (peer-internal pre-mutation guard)");
        }
    }

    const auto sequence = mutation ? ++phone_control_sequence_ : phone_control_sequence_;
    try {
        auto reply = exchange(mutation ? "control" : "snapshot", method, operation.at("payload"),
                              sequence);
        reply.erase("session");
        reply["session_id"] = session_id_;
        reply["synthetic"] = true;
        reply["visible_effect_verified"] = false;

        return reply;
    } catch (const std::exception &error) {
        const std::string message = error.what();

        if (!mutation) {
            throw;
        }

        // 1) Clean closed-vocabulary rejection (PHONE_PROVIDER_REJECTED: peer-certified
        //    mutation_started=false at request/identity — PRE-reservation stages). Known outcome:
        //    nothing was delivered. Roll the optimistically incremented counter back so the next
        //    mutation reuses the sequence number (the peer never reserved it).
        if (message.starts_with("PHONE_PROVIDER_REJECTED:")) {
            --phone_control_sequence_;
            throw;
        }

        // A restarted provider cannot prove whether the old mutation ran before its receipt was
        // lost.
        phone_control_unknown_ = true;
        throw;
    } catch (...) {
        if (mutation) {
            phone_control_unknown_ = true;
        }

        throw;
    }
}

Json Session::wheel_provider(const std::string &method, const std::vector<std::string> &extras,
                             const std::string &argument) {
    if (method != "bootstrap" && method != "input" && method != "status" && method != "presave") {
        throw VoyemuError("ANDROID_PROVIDER", "closed method required");
    }

    const auto &adapter = android_spec_.at("wheel_adapter");

    if (adapter.value("authority", std::string("com.voyemu.input.wheel")) !=
        "com.voyemu.input.wheel") {
        throw VoyemuError("ANDROID_PROVIDER", "recovered protected authority required");
    }

    std::vector<std::string> args{"content",  "call",
                                  "--uri",    "content://com.voyemu.input.wheel",
                                  "--method", method,
                                  "--extra",  "schema:i:1",
                                  "--extra",  "session:s:" + session_id_,
                                  "--extra",  "token:s:" + guest_auth_token_};

    if (!argument.empty()) {
        args.push_back("--arg");
        args.push_back(argument);
    }

    for (const auto &extra : extras) {
        args.push_back("--extra");
        args.push_back(extra);
    }

    std::string command;

    for (const auto &arg : args) {
        if (!command.empty()) {
            command += ' ';
        }

        command += android_shell_quote(arg);
    }

    const int provider_budget = android_provider_budget();
    auto result = adb({"shell", command}, provider_budget);

    if (result.exit_code || result.timed_out ||
        result.output.find("SecurityException") != std::string::npos) {
        throw VoyemuError("ANDROID_PROVIDER",
                          "protected call failed; " +
                                  can_transport_diagnostic(result, guest_auth_token_));
    }

    auto reply = parse_android_provider_reply(result.output);

    if (reply.dump().find(guest_auth_token_) != std::string::npos) {
        throw VoyemuError("ANDROID_PROVIDER_REPLY", "private credential in response");
    }

    return reply;
}

void Session::deliver_android_event(const WheelEvent &event, bool retained_identity) {
    if (!android_spec_.contains("wheel_adapter")) {
        deliveries_["android"]["wheel"] = !passion_presentation(android_spec_) &&
                                                          qinggan11_wheel_unsupported(android_spec_)
                                                  ? android_spec_.at("wheel_capability")
                                                  : Json{{"event_sequence", event.sequence},
                                                         {"status", "adapter_unavailable"}};

        return;
    }

    const auto &adapter = android_spec_.at("wheel_adapter");

    if (!adapter.contains("routes") || !adapter["routes"].contains(event.id) ||
        adapter["routes"][event.id].is_null()) {
        deliveries_["android"]["wheel"] = {
                {"event_sequence", event.sequence}, {"button", event.id}, {"status", "unmapped"}};

        return;
    }

    if (!retained_identity) {
        assert_android_identity();
    }

    prepare_android_wheel();

    // The provider may consume a down before a host-side transport timeout.
    if (event.phase == "down") {
        android_possible_pressed_[event.id] = event.owner;
    }

    Json receipt;
    try {
        receipt = wheel_provider("input", {"sequence:l:" + std::to_string(event.sequence),
                                           "button:s:" + event.id, "owner:s:" + event.owner,
                                           "phase:s:" + event.phase});
    } catch (const std::exception &) {
        receipt = wheel_provider("status", {}, "input");
    }

    auto delivery = android_wheel_delivery_receipt(receipt, session_id_, event);

    if (!delivery) {
        receipt = wheel_provider("status", {}, "input");
        delivery = android_wheel_delivery_receipt(receipt, session_id_, event);
    }

    if (!delivery) {
        throw VoyemuError("ANDROID_WHEEL", "matching processed provider receipt unavailable");
    }

    android_input_sequence_ = event.sequence;
    deliveries_["android"]["wheel"] = std::move(*delivery);

    if (receipt.at("status") == "terminal-unavailable") {
        try {
            logging::write(logging::Level::error, "android", "wheel.terminal_unavailable",
                           {{"event_sequence", event.sequence},
                            {"button", event.id},
                            {"phase", event.phase}});
        } catch (...) { /* Ignored: diagnostics must not turn a processed phase into a retry. */
        }
    }

    if (event.phase != "down" &&
        (!android_spec_.contains("wheel_capability") ||
         (!passion_presentation(android_spec_) &&
          !qinggan11_partial_wheel_capability_matches(android_spec_.at("wheel_capability"))) ||
         receipt.value("submission_confirmed", false))) {
        android_possible_pressed_.erase(event.id);
    }
}

void Session::deliver_android_short_pair(const WheelEvent &down, const WheelEvent &up, int hold_ms,
                                         bool &submitted) {
    assert_android_identity();
    prepare_android_wheel();
    android_possible_pressed_[down.id] = down.owner;
    Json down_receipt, up_receipt;

    if (!submitted) {
        const auto command = android_short_wheel_pair_command(
                session_id_, guest_auth_token_, android_transport_boot_, down, up, hold_ms);
        // Once dispatched, transport uncertainty never permits another DOWN.
        submitted = true;
        try {
            const auto result = adb({"shell", command}, 5000);

            if (result.output.find(guest_auth_token_) != std::string::npos) {
                throw VoyemuError("ANDROID_PROVIDER_REPLY", "private credential in response");
            }

            const std::string first = "VOYEMU_DOWN\n", last = "VOYEMU_UP\n";
            const auto split = result.output.find(last);

            if (result.output.starts_with(first)) {
                down_receipt = parse_android_provider_reply(result.output.substr(
                        first.size(),
                        split == std::string::npos ? std::string::npos : split - first.size()));
            }

            if (split != std::string::npos) {
                up_receipt =
                        parse_android_provider_reply(result.output.substr(split + last.size()));
            }
        } catch (const std::exception
                         &) { /* Ignored: recover only receipts; never replay the batch. */
        }
    }

    auto release = android_wheel_delivery_receipt(up_receipt, session_id_, up);
    auto press = android_wheel_delivery_receipt(down_receipt, session_id_, down);

    if (!release) {
        const auto receipt = wheel_provider("status", {}, "input");
        release = android_wheel_delivery_receipt(receipt, session_id_, up);

        if (!press) {
            press = android_wheel_delivery_receipt(receipt, session_id_, down);
            down_receipt = receipt;
        }
    }

    if (release) {
        (*release)["pair_down_receipt_available"] = press.has_value();

        if (press) {
            (*release)["pair_down_receipt"] = down_receipt;
        }

        if (!release->at("receipt").value("submission_confirmed", false)) {
            throw VoyemuError(
                    "ANDROID_WHEEL",
                    "short pair release submission unconfirmed; DOWN will not be replayed");
        }

        android_input_sequence_ = up.sequence;
        deliveries_["android"]["wheel"] = std::move(*release);
        android_possible_pressed_.erase(down.id);

        return;
    }

    if (press && down_receipt.value("submission_confirmed", false) &&
        down_receipt.value("status", std::string{}) == "terminal-requested") {
        // A shell may stop after DOWN. Its receipt permits only the original owned release.
        deliver_android_event(up);

        return;
    }

    throw VoyemuError("ANDROID_WHEEL", "short pair release unconfirmed; DOWN will not be replayed");
}

void Session::prepare_android_wheel() {
    // Both callers assert current identity and retained SDK ownership before provisioning.
    if (wheel_provisioned_) {
        return;
    }

    Json receipt;

    if (nxp_presentation(android_spec_)) {
        const auto started = adb(
                {"shell", "am", "startservice", "-n", "com.voyemu.oem/.RegistrationService"}, 5000);

        if (started.timed_out || started.exit_code ||
            started.output.find("Error") != std::string::npos) {
            throw VoyemuError("NXP_WHEEL_STARTUP", "declared key manager service dispatch failed");
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool registered = false;

        do {
            const auto service = adb({"shell", "service", "check", "qg.key_manager"}, 1500);

            if (nxp_key_manager_service_ready(service.exit_code, service.timed_out,
                                              service.output)) {
                registered = true;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (std::chrono::steady_clock::now() < deadline);

        if (!registered) {
            throw VoyemuError("NXP_WHEEL_STARTUP", "key manager Binder registration unconfirmed");
        }
    }

    if (android_spec_.contains("owned_package")) {
        deliveries_["android"]["owned_package"] = ensure_android_owned_package(
                android_spec_, android_resume_ ? android_boot_nonce_ : session_id_,
                android_transport_boot_,
                [&](const auto &args, int timeout) {
                    return adb(args, timeout);
                },
                [&] {
                    assert_android_identity();
                });
    }

    if (android_spec_.contains("camera_package")) {
        deliveries_["android"]["camera_package"] = ensure_android_camera_package(
                android_spec_, android_resume_ ? android_boot_nonce_ : session_id_,
                android_transport_boot_,
                [&](const auto &args, int timeout) {
                    return adb(args, timeout);
                },
                [&] {
                    assert_android_identity();
                });
    }

    if (android_spec_.contains("prepared_packages")) {
        deliveries_["android"]["prepared_packages"] = ensure_android_prepared_packages(
                android_spec_, android_resume_ ? android_boot_nonce_ : session_id_,
                android_transport_boot_,
                [&](const auto &args, int timeout) {
                    return adb(args, timeout);
                },
                [&] {
                    assert_android_identity();
                });
    }

    try {
        receipt = wheel_provider("bootstrap");
    } catch (const std::exception &bootstrap_error) {
        const std::string first = bootstrap_error.what();
        try {
            receipt = wheel_provider("status", {}, "bootstrap");
        } catch (const std::exception &status_error) {
            throw VoyemuError("ANDROID_WHEEL_BOOTSTRAP",
                              first + "; status: " + status_error.what());
        }
    }

    auto valid = [&](const Json &value) {
        return value.is_object() && value.value("schema", 0) == 1 &&
               value.value("session_id", std::string{}) == session_id_ &&
               value.value("status", std::string{}) == "session-ready";
    };

    if (!valid(receipt)) {
        receipt = wheel_provider("status", {}, "bootstrap");
    }

    if (!valid(receipt)) {
        throw VoyemuError("ANDROID_PROVISION", "matching protected bootstrap receipt unavailable");
    }

    android_input_sequence_ = 0;
    wheel_provisioned_ = true;
    deliveries_["android"]["wheel_bootstrap"] = {
            {"status", "session-ready"},
            {"guest_acknowledged", true},
            {"transport", "protected provider; app writes its own private data"}};
}

std::optional<bool> host_light_theme(std::string_view title) {
    while (!title.empty() && (title.back() == '\n' || title.back() == '\r')) {
        title.remove_suffix(1);
    }

    if (title == "default") {
        return false;
    }

    if (title == "simple") {
        return true;
    }

    return std::nullopt;
}

std::optional<unsigned> oem_language_setting(std::string_view query) {
    while (!query.empty() && (query.back() == '\n' || query.back() == '\r')) {
        query.remove_suffix(1);
    }

    constexpr std::string_view prefix = "Row: 0 value=";

    if (!query.starts_with(prefix)) {
        return std::nullopt;
    }

    query.remove_prefix(prefix.size());

    if (query == "0") {
        return 0u;
    }

    if (query == "1") {
        return 1u;
    }

    return std::nullopt;
}

std::optional<Json> oem_language_change(std::optional<unsigned> previous, unsigned current) {
    if (current > 1 || (previous && *previous > 1)) {
        throw VoyemuError("ANDROID_LANGUAGE", "unsupported OEM value");
    }

    if (previous && *previous == current) {
        return std::nullopt;
    }

    return Json{{"ivi_languageSet", current}};
}

std::optional<Json> oem_language_command(std::optional<unsigned> previous, unsigned current) {
    if (current > 1 || (previous && *previous > 1)) {
        throw VoyemuError("ANDROID_LANGUAGE", "unsupported OEM value");
    }

    if (previous && *previous == current) {
        return std::nullopt;
    }

    return Json{{"value", current}, {"source", "IPP_LANGUAGE_SET"}};
}

void Session::queue_cluster_language() {
    // Only the Qinggan catalogues carry the cluster language.
    if (!model_.state().at("raw_oem").contains("ivi_languageSet")) {
        return;
    }

    const auto current = model_.state().at("raw_oem").at("ivi_languageSet").get<unsigned>();

    // The OEM cluster applies the language through mcd message 24 (setting id 3) besides the MCU
    // word; the command has values 0 and 1 only. A cluster started later receives it again.
    if (!cluster_requested_ || current > 1) {
        return;
    }

    if (auto command = oem_language_command(cluster_language_, current)) {
        (*command)["seq"] = ++language_sequence_;
        write_private_json(directory_ / "language-command.json", *command);
        deliveries_["qnx"]["language"] = {{"sequence", language_sequence_},
                                          {"status", "queued"},
                                          {"value", (*command)["value"]},
                                          {"source", (*command)["source"]}};
    }

    cluster_language_ = current;
}

void Session::poll_oem_settings() {
    if (nxp_presentation(android_spec_)) {
        for (const char *key : {"language", "appearance"}) {
            deliveries_["android"][key] = {{"status", "unsupported"}, {"supported", false}};
        }

        return;
    }

    if (passion_presentation(android_spec_)) {
        deliveries_["android"]["language"] = {{"status", "unsupported"}, {"supported", false}};

        return;
    }

    const auto before = model_.state().at("sequence").get<std::uint32_t>();
    const auto query = adb({"shell", "content", "query", "--uri",
                            "content://qinggan.settings/system/system_language_setting",
                            "--projection", "value"},
                           1500);
    auto &language = deliveries_["android"]["language"];
    language = {{"source", "qinggan.settings/system/system_language_setting"},
                {"visible_effect_verified", false}};

    if (query.exit_code || query.timed_out) {
        language["status"] = "unavailable";
    } else if (const auto current = oem_language_setting(query.output)) {
        language.update({{"status", "observed"}, {"value", *current}});

        // adb lends the mutex. A concurrent manual patch wins this observation;
        // retain the old OEM marker so the next bounded pass retries cleanly.
        if (model_.state().at("sequence") != before) {
            language["status"] = "awaiting_stable_source";
        } else {
            if (auto change = oem_language_change(oem_language_, *current)) {
                if (model_.state().at("raw_oem").at("ivi_languageSet") != *current) {
                    model_.patch(*change);
                    write_private_json(directory_ / "vehicle-state.json", model_.state(),
                                       vehicle_state_bytes);
                    schedule_android_state(before);

                    if (cluster_requested_) {
                        deliveries_["qnx"]["vehicle"] = qnx_vehicle_pending();
                    }

                    logging::write(
                            logging::Level::info, "android", "language.changed",
                            {{"value", *current}, {"source_revision", model_.state()["sequence"]}});
                }

                oem_language_ = current;
            }

            queue_cluster_language();
            language["source_revision"] = model_.state()["sequence"];
            language["canonical_matches"] = model_.state().at("raw_oem").at("ivi_languageSet") ==
                                            *current;
        }
    } else {
        language["status"] = "not_observed";
    }

    const auto title = adb({"shell", "getprop", "persist.sys.qg.currenttheme"}, 1500);

    if (!title.exit_code && !title.timed_out) {
        if (const auto light = host_light_theme(title.output)) {
            const auto observed = Json{{"status", "observed"},
                                       {"scheme", *light ? "light" : "dark"},
                                       {"oem_theme", *light ? "simple" : "default"},
                                       {"source", "persist.sys.qg.currenttheme"}};

            if (deliveries_["android"].value("appearance", Json::object()) != observed) {
                logging::write(logging::Level::info, "android", "appearance.changed",
                               {{"scheme", *light ? "light" : "dark"}});
            }

            deliveries_["android"]["appearance"] = observed;
        } else {
            deliveries_["android"]["appearance"]["status"] = "unsupported";
            deliveries_["android"]["appearance"]["source"] = "persist.sys.qg.currenttheme";
        }
    } else {
        deliveries_["android"]["appearance"]["status"] = "unavailable";
    }

    std::vector<std::string> replies;

    for (const auto &query : oem_theme_queries(android_spec_)) {
        auto result = adb(query, 1500);

        if (result.exit_code || result.timed_out) {
            // An unavailable cluster skin is observed again at the next poll.
            if (qinggan11_presentation(android_spec_)) {
                return;
            }

            throw VoyemuError("ANDROID_THEME", "Binder unavailable");
        }

        replies.push_back(result.output);
    }

    if (const auto current = oem_theme_state(android_spec_, replies)) {
        observe_oem_theme(*current);
    }
}

void Session::observe_oem_theme(ThemeState current) {
    auto change = oem_theme_command(android_spec_, theme_, current);

    theme_ = current;

    if (!change) {
        return;
    }

    (*change)["seq"] = ++theme_sequence_;
    write_private_json(directory_ / "theme-command.json", *change);
    deliveries_["qnx"]["theme"] = {{"sequence", theme_sequence_},
                                   {"status", "queued"},
                                   {"theme_id", (*change)["theme_id"]},
                                   {"source", (*change)["source"]}};

    if (change->contains("theme_skin")) {
        deliveries_["qnx"]["theme"]["theme_skin"] = (*change)["theme_skin"];
    }
}

void Session::flush_android_releases(bool android_stopping) {
    retire_android_transaction_for_flush(android_stopping);
    auto discarded = android_events_.size();
    android_events_.clear();
    android_short_pairs_.clear();

    while (events_.pop()) {
        ++discarded;
    }

    if (android_stopping) {
        android_possible_pressed_.clear();
        deliveries_["android"]["press_cleanup"] = {{"status", "cleared_after_owned_Android_exit"},
                                                   {"discarded_undelivered_phases", discarded}};

        return;
    }

    for (const auto &[id, owner] : android_possible_pressed_) {
        android_events_.push_back(events_.force_release(id, owner));
    }

    deliveries_["android"]["press_cleanup"] = {
            {"status", android_events_.empty() ? "no_possible_held_inputs"
                                               : "cancel_queue_awaiting_private_receipts"},
            {"cancel_phases", android_events_.size()},
            {"discarded_undelivered_phases", discarded}};
}
} // namespace voyemu
