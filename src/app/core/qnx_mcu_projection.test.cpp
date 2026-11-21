#include "qnx_mcu_projection.hpp"
#include <array>
#include <cassert>
#include <exception>
#include <iostream>
using namespace voyemu;

int main() try {
    assert(qnx_mcu_request_projection(0, 0, 0).is_null());
    const auto expected = qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_731);
    const auto &qinggan11_731 = qnx_adapter(QnxGuestAdapter::sa8155_qinggan11_731);
    const auto &qinggan11_611 = qnx_adapter(QnxGuestAdapter::sa8155_qinggan11_611);
    const auto projected = qinggan11_731.mcu_projected_fields;
    // The adapter of the worker selects the layout among adapters of one projected count.
    assert(qnx_mcu_request_projection(650, 736, projected, qinggan11_731.name) == expected);
    assert(qnx_mcu_request_projection(650, 736, qinggan11_611.mcu_projected_fields,
                                      qinggan11_611.name) ==
           qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_611));

    for (const auto &args : {std::array<std::uint64_t, 3>{280, 579, 579},
                             {650, 736, 579},
                             {650, 736, 1},
                             {650, 0, 462},
                             {0, 736, 462},
                             {650, 736, 0}}) {
        bool rejected = false;
        try {
            qnx_mcu_request_projection(args[0], args[1], args[2]);
        } catch (...) {
            rejected = true;
        }

        assert(rejected);
    }

    const auto legacy = qnx_mcu_delivery_scope(Json::object());
    assert(legacy && legacy->at("fields") == 579 && !legacy->contains("mcu_projection"));
    assert(!qnx_mcu_delivery_scope(Json::object(), expected));
    assert(qnx_mcu_delivery_scope(Json{{"mcu_projection", expected}}, expected));
    auto float_expected = expected;
    float_expected["projected_fields"] = double(projected);
    assert(!qnx_mcu_delivery_scope(Json{{"mcu_projection", expected}}, float_expected));
    const auto free = qnx_mcu_delivery_scope(Json{{"mcu_projection", expected}});
    assert(free && free->at("fields") == projected && free->at("full_wire_verified") == false);

    for (const auto &patch :
         {Json{{"projected_fields", double(projected)}}, Json{{"projected_fields", 579}},
          Json{{"wire_fields", 579}}, Json{{"packet_bytes", 280}}, Json{{"unsupported_fields", 0}},
          Json{{"unmapped_wire_bits", "overwritten"}}}) {
        auto wrong = expected;
        wrong.update(patch);
        assert(!qnx_mcu_delivery_scope(Json{{"mcu_projection", wrong}}));
    }

    assert(!qnx_mcu_delivery_scope(Json::array()));
    assert(!qnx_mcu_delivery_scope(Json{{"mcu_projection", nullptr}}));
    auto absent = expected;
    absent.erase("wire_fields");
    assert(!qnx_mcu_delivery_scope(Json{{"mcu_projection", absent}}));

    // No adapter requests a scope without packet and wire geometry.
    for (const auto &args : {std::array<std::uint64_t, 3>{0, 0, 3}, {650, 736, 462}}) {
        bool rejected = false;
        try {
            qnx_mcu_request_projection(args[0], args[1], args[2]);
        } catch (const VoyemuError &) {
            rejected = true;
        }

        assert(rejected);
    }

    std::cout << "A9 full and Free ACK scope guards passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
