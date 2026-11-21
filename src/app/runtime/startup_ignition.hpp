#pragma once
#include "model.hpp"
#include "qnx_adapter.hpp"
#include "qnx_run_budget.hpp"
#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
namespace voyemu {
// Cold start of a Qinggan QNX cluster as in the vehicle: the model switches ign off while the
// cluster plays its OEM startup sequence and on again when the guest runtime asks for it. The
// guest asks inside the 6000 ms animation state of the OEM powermgr; a later ignition would leave
// the cluster in standby_on, so without the request the guest timeline bounds the wait.
// Each bound runs from the guest marker it follows; the guest waits it covers take longer on a
// slower engine, so they scale through qnx_guest_deadline.
class StartupIgnition {
public:
    using Clock = std::chrono::steady_clock;

    struct Change {
        Json values;
        std::string reason;
    };

    // HVF values. A cold boot that never reaches the cluster frames: no new startup marker.
    static constexpr std::chrono::seconds boot_limit{300};
    // The runtime waits up to 60 s for the gauge window and 30 s for the animation window.
    static constexpr std::chrono::seconds frames_limit{90};
    // Up to 20 s for the animation application to load and 4 s for the gauge emergence.
    static constexpr std::chrono::seconds gauge_limit{25};
    // The OEM powermgr leaves its animation state after 6000 ms of guest time; one second covers
    // delivery. The TCG guest clock (icount shift=0) never runs ahead of the host, so this bound
    // stays inside the OEM window on every engine and does not scale.
    static constexpr std::chrono::seconds animation_limit{5};

    explicit StartupIgnition(QnxEngine engine = host_qnx_engine) : engine_(engine) {}

    // The qinggan09 and qinggan11 adapters project the ignition into the OEM power items of
    // powermgr.
    static bool applies(std::string_view adapter) {
        return std::any_of(qnx_adapter_registry.begin(), qnx_adapter_registry.end(),
                           [&](const QnxAdapterSpec &spec) {
                               return spec.name == adapter &&
                                      spec.platform != QnxGuestPlatform::passion;
                           });
    }

    // A model with the ignition already off keeps it off and starts no sequence.
    std::optional<Change> begin(const Json &state, Clock::time_point now) {
        catalog_ = state_catalog(state);
        pending_ = concept_value(state, VehicleConcept::ignition) == 1;
        markers_ = false;
        boot_ = now + qnx_guest_deadline(engine_, boot_limit);
        deadline_ = Clock::time_point::max();

        if (!pending_) {
            return std::nullopt;
        }

        return Change{concept_patch(catalog_, VehicleConcept::ignition, 0), "cluster_cold_start"};
    }

    // `QIP STARTUP ignition reason=R` is the request of the runtime with the startup sequence.
    // The gauge window and the animation-ready child bound the wait for a lost request.
    std::optional<Change> marker(std::string_view line, Clock::time_point now) {
        constexpr std::string_view request = "QIP STARTUP ignition reason=";

        markers_ = true;
        boot_ = now + qnx_guest_deadline(engine_, boot_limit);

        if (line.starts_with(request)) {
            return on("runtime_" + std::string(line.substr(request.size())));
        }

        if (line.starts_with("QIP GAUGE window-ready=")) {
            bound(now + qnx_guest_deadline(engine_, gauge_limit), "gauge_window_timeout");
        } else if (line == "QMC CHILD begin=animation-ready") {
            bound(now + animation_limit, "animation_ready_timeout");
        }

        return std::nullopt;
    }

    // Frames without a single runtime marker mean the request cannot arrive.
    std::optional<Change> graphics(Clock::time_point now) {
        if (!markers_) {
            return on("no_runtime_markers");
        }

        bound(now + qnx_guest_deadline(engine_, frames_limit), "frames_timeout");

        return std::nullopt;
    }

    std::optional<Change> expire(Clock::time_point now) {
        if (now < std::min(boot_, deadline_)) {
            return std::nullopt;
        }

        return on(boot_ < deadline_ ? "boot_timeout" : timeout_);
    }

    // A closed cluster never asks, so the vehicle returns to its state before the cold start.
    std::optional<Change> finish() {
        return on("cluster_closed");
    }

    // A client value for the ignition ends the sequence and stays.
    void observe_patch(const Json &values) {
        if (values.contains(concept_field(catalog_, VehicleConcept::ignition))) {
            pending_ = false;
        }
    }

    bool pending() const noexcept {
        return pending_;
    }

private:
    void bound(Clock::time_point deadline, const char *reason) {
        if (deadline < deadline_) {
            deadline_ = deadline;
            timeout_ = reason;
        }
    }

    std::optional<Change> on(std::string reason) {
        if (!pending_) {
            return std::nullopt;
        }

        pending_ = false;

        return Change{concept_patch(catalog_, VehicleConcept::ignition, 1), std::move(reason)};
    }

    QnxEngine engine_;
    VehicleCatalog catalog_ = VehicleCatalog::qinggan09;
    bool pending_ = false, markers_ = false;
    // The boot bound follows the latest startup marker; the frames, gauge and animation bounds
    // only tighten.
    Clock::time_point boot_{}, deadline_{};
    std::string timeout_;
};
} // namespace voyemu
