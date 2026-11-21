#include "adas_ecu.hpp"
#include <cassert>
#include <exception>
#include <iostream>
using namespace voyemu;
using namespace std::chrono_literals;

int main() try {
    auto ready = apply_patch(initial_state(VehicleCatalog::qinggan09),
                             {{"gear", 3}, {"gear_valid", 1}, {"speed", 80}, {"soc", 62}});
    auto now = AdasEcu::Clock::time_point{} + 10s;
    AdasEcu ecu;
    std::uint32_t sequence = 0;
    Json state = ready;
    auto click = [&](std::string id, std::chrono::milliseconds duration = 100ms) {
        auto down = ecu.input(state, ++sequence, id, "down", "ui-a", now);
        assert(down.receipt["outcome"] == "held" && down.patch.empty());
        auto up = ecu.input(state, ++sequence, id, "up", "ui-a", now + duration);
        now += duration + 10ms;

        if (!up.patch.empty()) {
            state = apply_patch(state, up.patch);
        }

        assert(up.receipt["after_revision"] == state["sequence"]);

        return up;
    };
    auto acc = click("acc");

    assert(acc.receipt["outcome"] == "applied" && state["raw_oem"]["acc_mode"] == 3 &&
           state["raw_oem"]["mpc_icaSts"] == 1 && state["raw_oem"]["mrr_vSetDis"] == 160 &&
           state["raw_oem"]["time_gap_set"] == 1);
    auto before = state;
    click("cruise_up");
    assert(state["raw_oem"]["mrr_vSetDis"] == 162);
    click("cruise_down");
    assert(state["raw_oem"]["mrr_vSetDis"] == 160);
    auto up = click("cruise_up", 1000ms);
    assert(up.receipt["press_kind"] == "long" && state["raw_oem"]["mrr_vSetDis"] == 170);
    click("cruise_down", 1200ms);
    assert(state["raw_oem"]["mrr_vSetDis"] == 160);
    click("distance");
    assert(state["raw_oem"]["time_gap_set"] == 2);
    click("distance");
    assert(state["raw_oem"]["time_gap_set"] == 3);
    click("distance");
    assert(state["raw_oem"]["time_gap_set"] == 1);
    click("ica");
    assert(state["raw_oem"]["mpc_icaSts"] == 3 && state["raw_oem"]["acc_mode"] == 3);
    click("acc");
    assert(state["raw_oem"]["mpc_icaSts"] == 1);
    click("cancel");
    assert(state["raw_oem"]["acc_mode"] == 0 && state["raw_oem"]["mpc_icaSts"] == 1 &&
           state["raw_oem"]["mrr_vSetDis"] == 160);
    auto inactive = click("cruise_down");
    assert(inactive.receipt["outcome"] == "no-change");
    click("cruise_up");
    assert(state["raw_oem"]["acc_mode"] == 3 && state["raw_oem"]["mrr_vSetDis"] == 160);

    for (const auto &[name, value] : before["raw_oem"].items()) {
        if (name != "acc_mode" && name != "mpc_icaSts" && name != "mrr_vSetDis" &&
            name != "time_gap_set") {
            assert(state["raw_oem"][name] == value);
        }
    }

    auto brake = apply_patch(state, {{"vcu_brakePedalPressed", 1}});
    auto reconciled = ecu.reconcile(brake);
    assert(reconciled.receipt["reason"] == "brake-pressed" && reconciled.patch["acc_mode"] == 2 &&
           apply_patch(brake, reconciled.patch)["raw_oem"]["mpc_icaSts"] == 1);
    state = apply_patch(state, {{"gear", 0}});
    auto standby = click("ica");
    assert(standby.receipt["reason"] == "gear-not-drive" && state["raw_oem"]["acc_mode"] == 2 &&
           state["raw_oem"]["mpc_icaSts"] == 2);
    state = apply_patch(state, {{"cfg_tja_assis", 0}});
    auto unavailable = click("ica");
    assert(unavailable.receipt["outcome"] == "unavailable" && unavailable.patch.empty());
    state = apply_patch(state, {{"cfg_acc", 0}});
    assert(click("acc").receipt["reason"] == "acc-feature-disabled");
    assert(click("distance").receipt["outcome"] == "unavailable");
    state = apply_patch(ready, {{"acc_mode", 3}, {"mpc_icaSts", 1}, {"mrr_vSetDis", 260}});
    auto bounded = click("cruise_up");
    assert(bounded.receipt["reason"] == "target-bound" && bounded.patch.empty());
    state = apply_patch(state, {{"mrr_vSetDis", 60}});
    assert(click("cruise_down").receipt["reason"] == "target-bound");
    state = apply_patch(state, {{"mrr_vSetDis", 161}});
    assert(click("cruise_up").receipt["reason"] == "target-outside-policy");
    state = apply_patch(state, {{"acc_mode", 7}});
    auto fault = click("cancel");
    assert(fault.receipt["outcome"] == "unavailable" && state["raw_oem"]["acc_mode"] == 7);
    state = ready;
    auto original = state;
    auto held = ecu.input(state, ++sequence, "acc", "down", "ui-a", now);
    assert(held.receipt["outcome"] == "held");
    auto replay = ecu.input(state, sequence, "acc", "down", "ui-a", now + 10ms);
    assert(replay.receipt["reason"] == "replayed-sequence");
    auto foreign = ecu.input(state, ++sequence, "acc", "up", "ui-b", now + 20ms);
    assert(foreign.receipt["reason"] == "release-owner-mismatch");
    auto cancelled = ecu.input(state, ++sequence, "acc", "cancel", "ui-a", now + 30ms);
    assert(cancelled.receipt["outcome"] == "cancelled" && state == original);
    ecu.input(state, ++sequence, "acc", "down", "ui-a", now);
    auto reverse = ecu.input(state, ++sequence, "acc", "up", "ui-a", now - 1ms);
    assert(reverse.receipt["reason"] == "non-monotonic-time");
    ecu.input(state, ++sequence, "acc", "down", "ui-a", now);
    ecu.cancel_all();
    ++sequence;
    assert(ecu.input(state, sequence, "acc", "up", "ui-a", now + 20ms).receipt["reason"] ==
           "release-owner-mismatch");
    AdasEcu recovered;
    state = apply_patch(
            ready, {{"acc_mode", 0}, {"mpc_icaSts", 1}, {"mrr_vSetDis", 180}, {"time_gap_set", 2}});
    recovered.input(state, 1, "cruise_up", "down", "ui-new", now);
    auto resumed = recovered.input(state, 2, "cruise_up", "up", "ui-new", now + 100ms);
    assert(resumed.patch["acc_mode"] == 3 && !resumed.patch.contains("mrr_vSetDis") &&
           !resumed.patch.contains("speed"));
    ++sequence;
    assert(!ecu.input(state, sequence, "voice", "down", "ui-a", now).handled);
    assert(AdasEcu::policy()["input_kind"] == "simulated-ecu" &&
           AdasEcu::policy()["hold_repeat"] == false);
    // qinggan11 carries the set speed in ADCU_VsetDis in whole km/h; 255 is no speed.
    {
        AdasEcu platform;
        Json q11 = apply_patch(initial_state(VehicleCatalog::qinggan11),
                               {{"gear", 3}, {"gear_valid", 1}, {"speed", 80}});
        std::uint32_t step = 0;
        auto press = [&](std::string id, std::chrono::milliseconds duration = 100ms) {
            assert(platform.input(q11, ++step, id, "down", "ui-q", now).handled);
            auto up = platform.input(q11, ++step, id, "up", "ui-q", now + duration);
            now += duration + 10ms;

            if (!up.patch.empty()) {
                q11 = apply_patch(q11, up.patch);
            }

            return up;
        };
        press("acc");
        assert(q11["raw_oem"]["acc_mode"] == 3 && q11["raw_oem"]["ADCU_VsetDis"] == 80);
        press("cruise_up");
        assert(q11["raw_oem"]["ADCU_VsetDis"] == 81);
        press("cruise_down");
        assert(q11["raw_oem"]["ADCU_VsetDis"] == 80);
        press("cruise_up", 1000ms);
        assert(q11["raw_oem"]["ADCU_VsetDis"] == 85);
        press("cancel");
        assert(q11["raw_oem"]["acc_mode"] == 0 && q11["raw_oem"]["ADCU_VsetDis"] == 85);
        auto resume = press("cruise_up");
        assert(resume.receipt["reason"] == "resume" && q11["raw_oem"]["acc_mode"] == 3);
        press("cancel");
        q11 = apply_patch(q11, {{"ADCU_VsetDis", 255}});
        auto none = press("cruise_up");
        assert(none.receipt["reason"] == "no-remembered-target" && none.patch.empty());
    }
    // The Passion catalogue has no Qinggan ADAS configuration, so its ACC stays disabled; an odd
    // qinggan09 set speed in 0.5 km/h is no whole km/h target.
    {
        AdasEcu passion;
        Json car = apply_patch(initial_state(VehicleCatalog::passion),
                               concept_patch(VehicleCatalog::passion, VehicleConcept::speed, 80));
        assert(passion.input(car, 1, "acc", "down", "ui-p", now).handled);
        auto up = passion.input(car, 2, "acc", "up", "ui-p", now + 100ms);
        assert(up.receipt["reason"] == "acc-feature-disabled" && up.patch.empty());
        AdasEcu odd;
        Json half = apply_patch(ready, {{"acc_mode", 3}, {"mrr_vSetDis", 161}});
        assert(odd.input(half, 1, "cruise_up", "down", "ui-o", now).handled);
        assert(odd.input(half, 2, "cruise_up", "up", "ui-o", now + 100ms).receipt["reason"] ==
               "target-outside-policy");
    }
    std::cout
            << "ADAS physical phases, telemetry encoding, readiness, replay/owner/cancel, limits and recovery passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
