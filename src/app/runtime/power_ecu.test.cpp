#include "power_ecu.hpp"
#include <iostream>
#include <stdexcept>
using namespace voyemu;

static void check(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

static unsigned raw(const Json &state, const char *name) {
    return state.at("raw_oem").at(name).get<unsigned>();
}

int main() try {
    using State = PowerEcu::State;
    PowerEcu power;
    auto state = initial_state(VehicleCatalog::qinggan09);
    auto apply = [&](const PowerEcu::Outcome &outcome) {
        if (!outcome.patch.empty()) {
            state = apply_patch(state, outcome.patch);
        }

        return outcome.receipt.at("outcome").get<std::string>();
    };

    check(PowerEcu::parse("ready") == State::ready && PowerEcu::parse("acc") == State::acc &&
                  !PowerEcu::parse("start") && !PowerEcu::parse(2),
          "four named states");
    power.observe(state);
    check(power.state() == State::ready, "default model with ign and READY lamp is READY");

    // Each state sets ign and ready_lamp for both guests and the multimedia ACC.
    check(raw(state, "ic_bat_voltage") == 1380, "the default READY model charges the battery");
    check(apply(power.request(state, State::on)) == "applied" && power.state() == State::on &&
                  raw(state, "ign") == 1 && raw(state, "ready_lamp") == 0 &&
                  raw(state, "ic_bat_voltage") == 1240 && PowerEcu::android_acc(power.state()) == 2,
          "ON");
    check(apply(power.request(state, State::acc)) == "applied" && raw(state, "ign") == 0 &&
                  raw(state, "ready_lamp") == 0 && PowerEcu::android_acc(power.state()) == 2,
          "ACC keeps the multimedia on");
    check(apply(power.request(state, State::off)) == "applied" && raw(state, "ign") == 0 &&
                  raw(state, "ready_lamp") == 0 && raw(state, "ic_bat_voltage") == 1240 &&
                  PowerEcu::android_acc(power.state()) == 0,
          "OFF switches the multimedia ACC off");
    const auto revision = state.at("sequence");
    const auto same = power.request(state, State::off);
    check(same.patch.empty() && same.receipt.at("outcome") == "applied" &&
                  state.at("sequence") == revision,
          "a repeated state changes no field");
    check(apply(power.request(state, State::ready)) == "applied" && raw(state, "ign") == 1 &&
                  raw(state, "ready_lamp") == 1 && raw(state, "ic_bat_voltage") == 1380 &&
                  PowerEcu::android_acc(power.state()) == 2,
          "READY from OFF in P");

    // READY needs P or N; a moving vehicle stays at ON or above.
    state = apply_patch(state, {{"gear", 3}, {"speed", 40}});
    check(apply(power.request(state, State::on)) == "applied" && raw(state, "ign") == 1,
          "ON while moving");
    const auto drive = power.request(state, State::ready);
    check(drive.patch.empty() && drive.receipt.at("reason") == "gear-not-park-or-neutral" &&
                  power.state() == State::on,
          "READY rejected in D");
    const auto moving = power.request(state, State::off);
    check(moving.patch.empty() && moving.receipt.at("reason") == "vehicle-moving" &&
                  power.state() == State::on,
          "OFF rejected while moving");
    state = apply_patch(state, {{"gear", 2}, {"speed", 0}});
    check(apply(power.request(state, State::ready)) == "applied" && raw(state, "ready_lamp") == 1,
          "READY in N");

    // The cold start of a cluster holds READY at ACC and restores it.
    const auto hold = power.startup(state, false, "cluster_cold_start");
    check(apply(hold) == "applied" && power.state() == State::acc && raw(state, "ign") == 0 &&
                  raw(state, "ready_lamp") == 0 && PowerEcu::android_acc(power.state()) == 2 &&
                  hold.receipt.at("reason") == "cluster_cold_start",
          "startup holds at ACC");
    check(apply(power.startup(state, true, "runtime_gauge-check")) == "applied" &&
                  power.state() == State::ready && raw(state, "ign") == 1 &&
                  raw(state, "ready_lamp") == 1,
          "startup restores READY");
    check(apply(power.request(state, State::off)) == "applied" &&
                  power.startup(state, false, "cluster_cold_start").patch.empty() &&
                  power.state() == State::off,
          "an OFF vehicle has no startup hold");
    check(power.startup(state, true, "cluster_closed").patch.empty() && power.state() == State::off,
          "a release without a hold keeps the state");

    // Direct writes of the power fields stay for firmware tests; the state follows them.
    state = apply_patch(state, {{"ign", 1}});
    power.observe(state);
    check(power.state() == State::on, "raw ign selects ON");
    state = apply_patch(state, {{"ign", 0}});
    power.observe(state);
    check(power.state() == State::acc, "raw ign off keeps the multimedia on");

    // On Passion the states set PowerMode and LD_ReadyLamp; its battery voltage stays unbound.
    PowerEcu passion;
    auto car = initial_state(VehicleCatalog::passion);
    const auto mode = concept_field(VehicleCatalog::passion, VehicleConcept::ignition),
               lamp = concept_field(VehicleCatalog::passion, VehicleConcept::ready);
    passion.observe(car);
    check(passion.state() == State::ready, "the Passion model starts at READY");
    const auto on = passion.request(car, State::on);
    check(on.patch == Json{{lamp, 1}}, "ON switches LD_ReadyLamp to OFF only");
    car = apply_patch(car, on.patch);
    const auto off = passion.request(car, State::off);
    check(off.patch == Json{{mode, 1}}, "OFF sets PowerMode OFF");
    car = apply_patch(car, off.patch);
    passion.observe(car);
    check(passion.state() == State::off, "a switched off Passion model stays OFF");
    car = apply_patch(car, passion.request(car, State::on).patch);
    car = apply_patch(car, concept_patch(VehicleCatalog::passion, VehicleConcept::speed, 40));
    check(passion.request(car, State::off).receipt.at("reason") == "vehicle-moving",
          "a moving Passion vehicle stays on");
    car = apply_patch(car, concept_patch(VehicleCatalog::passion, VehicleConcept::speed, 0));
    car = apply_patch(car, concept_patch(VehicleCatalog::passion, VehicleConcept::gear, 3));
    check(passion.request(car, State::ready).receipt.at("reason") == "gear-not-park-or-neutral",
          "Passion READY needs P or N");
    std::cout << "power ECU: states, fields, rules, cold start and raw writes PASS\n";

    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
