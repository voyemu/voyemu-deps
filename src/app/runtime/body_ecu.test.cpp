#include "body_ecu.hpp"
#include <iostream>
#include <stdexcept>
using namespace voyemu;

static void check(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

int main() try {
    BodyEcu body;
    const auto now = BodyEcu::Clock::time_point{};
    auto logical = initial_state(VehicleCatalog::qinggan09);
    const auto original = logical;

    check(body.update(logical, now), "initial publication");
    check(!body.update(logical, now + std::chrono::hours(1)),
          "idle must not publish or write continuously");
    logical = apply_patch(logical, {{"turn_left", 1}});
    auto selected = logical;
    check(body.update(logical, now), "enable lights immediately");
    check(!body.update(logical, now + std::chrono::milliseconds(499)),
          "no extra ticks within phase");
    check(body.update(logical, now + std::chrono::milliseconds(500)), "falling edge");
    check(body.state()["raw_oem"]["turn_left"] == 0, "lamp goes dark");
    check(logical == selected, "blinking cannot mutate persistent state");
    auto dark_sequence = body.state()["sequence"].get<unsigned>();
    check(body.update(logical, now + std::chrono::milliseconds(1000)), "rising edge");
    check(body.state()["sequence"] == dark_sequence + 1,
          "MCU phase uses its own monotonic sequence");
    logical = apply_patch(logical, {{"turn_right", 1}});
    check(body.update(logical, now + std::chrono::milliseconds(1100)),
          "hazards restart lit in phase");
    check(body.state()["raw_oem"]["turn_left"] == 1 && body.state()["raw_oem"]["turn_right"] == 1,
          "hazards synchronized");
    logical = apply_patch(logical, {{"turn_left", 0}, {"turn_right", 0}, {"speed", 20}});
    check(body.update(logical, now + std::chrono::milliseconds(1150)), "off applies immediately");
    check(body.state()["raw_oem"]["turn_left"] == 0 && body.state()["raw_oem"]["turn_right"] == 0,
          "off stays off");
    check(body.state()["raw_oem"]["speed"] == 20 && body.state()["raw_oem"]["speed10x"] == 200,
          "other fields preserved");
    check(!body.update(logical, now + std::chrono::hours(2)),
          "no replay of missed phases after a pause");
    check(original["raw_oem"]["turn_left"] == 0, "original fixture preserved");
    logical["sequence"] = UINT32_MAX;
    body.update(logical, now);
    logical["raw_oem"]["turn_left"] = 1;
    bool rejected = false;
    try {
        body.update(logical, now);
    } catch (const std::runtime_error &) {
        rejected = true;
    }

    check(rejected, "sequence wrap rejected");
    // On Passion the projection blinks every turn lamp status the concept binds.
    BodyEcu passion;
    auto car = apply_patch(initial_state(VehicleCatalog::passion),
                           concept_patch(VehicleCatalog::passion, VehicleConcept::turn_left, 1));
    const std::string lamps =
            "TurnLampService.Notify_f_TrunLampSts.TrunLampSts.TrunLampSts.TrunLamp";
    check(passion.update(car, now) &&
                  passion.state()["raw_oem"][lamps + "ActualSts.RL_TurnLampActualSts"] == 2,
          "Passion turn lamps lit");
    check(passion.update(car, now + std::chrono::milliseconds(500)) &&
                  passion.state()["raw_oem"][lamps + "ActualSts.RL_TurnLampActualSts"] == 1 &&
                  passion.state()["raw_oem"][lamps + "LogicSts.FL_TurnLampLogicSts"] == 1 &&
                  car["raw_oem"][lamps + "ActualSts.RL_TurnLampActualSts"] == 2,
          "Passion turn lamps dark in the projection only");
    std::cout << "body ECU projection passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
