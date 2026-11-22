#include "someip_e2e.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
using namespace voyemu;
namespace {
std::vector<std::uint8_t> bytes(std::string_view hex) {
    std::vector<std::uint8_t> out;

    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(
                static_cast<std::uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
    }

    return out;
}

std::vector<std::uint8_t> payload(std::size_t size) {
    std::vector<std::uint8_t> data(size);

    for (std::size_t i = 20; i < size; ++i) {
        data[i] = static_cast<std::uint8_t>(i - 20);
    }

    return data;
}
} // namespace

int main() try {
    const std::string check = "123456789";
    assert(someip_crc64({reinterpret_cast<const std::uint8_t *>(check.data()), check.size()}) ==
           0x995dc9bbdf1939faULL);
    // Expected bytes come from the cluster's own E2E_P07Protect machine code run on these inputs.
    auto data = payload(32);
    assert(someip_e2e_p07_protect(data, 0x0a0b0c0d, 5) == 6);
    assert(data == bytes("a5382b91aeaad58900000020000000050a0b0c0d000102030405060708090a0b"));
    assert(someip_e2e_p07_valid(data, 0x0a0b0c0d) && !someip_e2e_p07_valid(data, 0x0a0b0c0e));
    data[31] ^= 1;
    assert(!someip_e2e_p07_valid(data, 0x0a0b0c0d));
    auto last = payload(24);
    assert(someip_e2e_p07_protect(last, 0x12345678, 0xffffffffU) == 0xffffffffU);
    assert(last == bytes("04bef62e9c74ee9f00000018ffffffff1234567800010203"));
    auto shorter = payload(19);
    bool rejected = false;

    try {
        someip_e2e_p07_protect(shorter, 1, 0);
    } catch (const std::exception &) {
        rejected = true;
    }

    assert(rejected);
    std::cout << "E2E profile 7 matches the CRC-64/XZ check value and the cluster's protect\n";

    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
