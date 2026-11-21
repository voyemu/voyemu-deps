#include "passion_mcu_property.hpp"
#include <cassert>
#include <exception>
#include <iostream>
using namespace voyemu;
constexpr char boot[] = "12345678-1234-1234-1234-123456789abc";

template <class F> void rejects(F f) {
    bool bad = false;

    try {
        f();
    } catch (...) {
        bad = true;
    }

    assert(bad);
}

Json receipt(const Json &state, bool callback = true) {
    const auto p = passion_mcu_projection(state, boot);

    return {{"schema", 1},
            {"boot", boot},
            {"service_uid", 1000},
            {"service_pid", 123},
            {"full_wire_verified", false},
            {"visible_effect_verified", false},
            {"supported_fields", Json::array({"soc"})},
            {"status", "snapshot_published"},
            {"sequence", p.at("sequence")},
            {"soc", p.at("soc")},
            {"source_sha256", p.at("source_sha256")},
            {"callback_returned", callback},
            {"property", 0x0500001d},
            {"area", 0},
            {"type", "Float"},
            {"snapshot_readback", p.at("soc")}};
}

Json notready() {
    return {{"schema", 1},
            {"boot", boot},
            {"service_uid", 1000},
            {"service_pid", 123},
            {"full_wire_verified", false},
            {"visible_effect_verified", false},
            {"supported_fields", Json::array({"soc"})},
            {"status", "not-ready"}};
}

// The SOC signal of the Passion catalogue in 0.1 %.
static Json charge(int percent) {
    return concept_patch(VehicleCatalog::passion, VehicleConcept::charge, percent);
}

static const std::string soc = concept_field(VehicleCatalog::passion, VehicleConcept::charge);

int main() try {
#ifndef NDEBUG
    auto state = apply_patch(initial_state(VehicleCatalog::passion), charge(23));
    auto p = passion_mcu_projection(state, boot);
    assert(state.at("raw_oem").at(soc) == 230);
    // A charge between whole percent rounds to the nearest percent.
    assert(passion_mcu_projection(apply_patch(state, Json{{soc, 236}}), boot).at("soc") == 24);
    assert(p.at("soc") == 23 && p.size() == 5 &&
           p.at("source_sha256") == sha256_bytes(state.dump()));
    assert(!p.contains("target"));
    auto missing = state;
    missing["raw_oem"].erase(soc);
    rejects([&] {
        passion_mcu_projection(missing, boot);
    });
    rejects([&] {
        passion_mcu_projection(state, "unknown");
    });
    assert(passion_mcu_status(notready(), boot));
    auto r = receipt(state, false);
    assert(passion_mcu_receipt_matches(r, state, boot));

    for (const auto &patch : std::vector<Json>{
                 Json{{"schema", 1.0}}, Json{{"sequence", 2.0}},
                 Json{{"boot", "22345678-1234-1234-1234-123456789abc"}}, Json{{"service_uid", 0}},
                 Json{{"service_pid", 0}},
                 Json{{"supported_fields", Json::array({"soc", "target"})}},
                 Json{{"full_wire_verified", true}}, Json{{"visible_effect_verified", true}},
                 Json{{"soc", 61}}, Json{{"source_sha256", std::string(64, 'b')}},
                 Json{{"callback_returned", "true"}}, Json{{"property", 0x05000007}},
                 Json{{"area", 1}}, Json{{"type", "Integer"}}, Json{{"snapshot_readback", -1}}}) {
        auto bad = r;
        bad.update(patch);
        assert(!passion_mcu_receipt_matches(bad, state, boot));
    }

    auto changed = apply_patch(state, charge(61));
    assert(!passion_mcu_receipt_matches(r, changed, boot));
    assert(passion_mcu_receipt_matches(receipt(changed), changed, boot));
    int calls = 0;
    auto result = passion_mcu_deliver(
            boot,
            [&] {
                return state;
            },
            [](std::uint32_t) {
                assert(false);
            },
            [&](const auto &args) {
                ++calls;
                assert(args[0] == "content" && args[3] == "content://com.voyemu.passion.mcu");

                return calls == 1 ? notready() : receipt(state, false);
            });
    assert(calls == 2 && result.at("projection_acknowledged") == true &&
           result.at("guest_acknowledged") == false && result.at("callback_returned") == false &&
           result.at("full_wire_verified") == false);
    calls = 0;
    result = passion_mcu_deliver(
            boot,
            [&] {
                return state;
            },
            [](std::uint32_t) {
                assert(false);
            },
            [&](const auto &) -> Json {
                if (++calls == 1) {
                    return notready();
                }

                if (calls == 2) {
                    throw std::runtime_error("late transport reply");
                }

                return receipt(state);
            });
    assert(calls == 3 && result.at("sequence") == state.at("sequence"));
    auto retained = state;
    retained["sequence"] = 9;
    retained["raw_oem"][soc] = 990;
    calls = 0;
    result = passion_mcu_deliver(
            boot,
            [&] {
                return state;
            },
            [&](std::uint32_t floor) {
                assert(floor == 9);
                state["sequence"] = floor + 1;
            },
            [&](const auto &) {
                return ++calls == 1 ? receipt(retained) : receipt(state);
            });
    assert(state.at("sequence") == 10 && state.at("raw_oem").at(soc) == 230 &&
           result.at("soc") == 23);
    auto old = state;
    calls = 0;
    result = passion_mcu_deliver(
            boot,
            [&] {
                return state;
            },
            [](std::uint32_t) {
                assert(false);
            },
            [&](const auto &) {
                if (++calls == 1) {
                    return notready();
                }

                state = apply_patch(state, charge(61));

                return receipt(old);
            });
    assert(result.at("sequence") == old.at("sequence") &&
           result.at("sequence") != state.at("sequence"));
    rejects([&] {
        passion_mcu_deliver(
                boot,
                [&] {
                    return state;
                },
                [](std::uint32_t) {},
                [&](const auto &) {
                    return receipt(old);
                });
    });
    // Sameboot Monza restart with unchanged host sequence must be observed and republished.
    auto unchanged = state;
    calls = 0;
    result = passion_mcu_deliver(
            boot,
            [&] {
                return state;
            },
            [](std::uint32_t) {
                assert(false);
            },
            [&](const auto &args) {
                ++calls;
                auto observed = calls == 1 ? notready() : receipt(state);
                observed["service_pid"] = 456;

                if (calls == 2) {
                    assert(args.at(5) == "submit");
                }

                return observed;
            });
    assert(calls == 2 && result.at("service_pid") == 456 && state == unchanged);
    calls = 0;
    auto healthy = receipt(state);
    healthy["service_pid"] = 457;
    result = passion_mcu_deliver(
            boot,
            [&] {
                return state;
            },
            [](std::uint32_t) {
                assert(false);
            },
            [&](const auto &args) {
                ++calls;
                assert(args.at(5) == "status");

                return healthy;
            });
    assert(calls == 1 && result.at("service_pid") == 457);
    rejects([&] {
        passion_mcu_deliver(
                boot,
                [&] {
                    return state;
                },
                [](std::uint32_t) {
                    assert(false);
                },
                [&](const auto &) {
                    return notready();
                });
    });
    result["verified_steady_ms"] = 100;
    auto seq = state.at("sequence").get<std::uint32_t>();
    assert(passion_mcu_delivery_current(result, boot, seq, 4099));
    assert(!passion_mcu_delivery_current(result, boot, seq, 4100));
    assert(!passion_mcu_delivery_current(result, boot, seq, 99));
    assert(!passion_mcu_delivery_current(result, boot, seq + 1, 101));
    Json observed = {
            {"status", "presentation_ready"}, {"guest_acknowledged", false}, {"vehicle", result}};
    passion_mcu_expire(observed, boot, seq, 4100);
    assert(observed.at("status") == "awaiting_mcu_projection" &&
           observed.at("vehicle").at("projection_acknowledged") == false &&
           observed.at("guest_acknowledged") == false);
    assert(!passion_mcu_capability().contains("helper_dex_sha256"));
    Json android = {{"api", 30}, {"mcu_property_adapter", passion_mcu_capability()}};
#ifdef _WIN32
    // Windows refuses the partial projection even for the exact prepared capability.
    rejects([&] {
        passion_mcu_available(android);
    });
#else
    assert(passion_mcu_available(android));
#endif
    android["mcu_property_adapter"]["supported_fields"].push_back("target");
    rejects([&] {
        passion_mcu_available(android);
    });
    std::cout
            << "PASS full-model partial SOC projection, typed submit, exact generation/readback, retained floor, late reply and no full ACK\n";
#else
    throw std::runtime_error("assertions required");
#endif
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
