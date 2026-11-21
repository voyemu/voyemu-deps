#include "someip_e2e.hpp"
#include "error.hpp"
#include <array>
#include <stdexcept>
namespace voyemu {
namespace {
constexpr std::size_t header_bytes = 20;

constexpr std::array<std::uint64_t, 256> crc_table() {
    std::array<std::uint64_t, 256> table{};

    for (unsigned i = 0; i < 256; ++i) {
        std::uint64_t value = i;

        for (unsigned bit = 0; bit < 8; ++bit) {
            value = (value & 1) ? (value >> 1) ^ 0xc96c5795d7870f42ULL : value >> 1;
        }

        table[i] = value;
    }

    return table;
}

constexpr auto table = crc_table();

void put(std::span<std::uint8_t> data, std::size_t at, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        data[at + i] = static_cast<std::uint8_t>(value >> (8 * (bytes - 1 - i)));
    }
}

std::uint64_t get(std::span<const std::uint8_t> data, std::size_t at, unsigned bytes) {
    std::uint64_t value = 0;

    for (unsigned i = 0; i < bytes; ++i) {
        value = (value << 8) | data[at + i];
    }

    return value;
}
} // namespace

std::uint64_t someip_crc64(std::span<const std::uint8_t> data) {
    std::uint64_t crc = ~0ULL;

    for (const auto byte : data) {
        crc = (crc >> 8) ^ table[(crc ^ byte) & 0xff];
    }

    return ~crc;
}

std::uint32_t someip_e2e_p07_protect(std::span<std::uint8_t> data, std::uint32_t data_id,
                                     std::uint32_t counter) {
    if (data.size() < header_bytes || data.size() > 0xffffffffULL) {
        throw VoyemuError("SOMEIP_E2E", "payload shorter than the profile 7 header");
    }

    put(data, 8, data.size(), 4);
    put(data, 12, counter, 4);
    put(data, 16, data_id, 4);
    put(data, 0, someip_crc64(data.subspan(8)), 8);

    return counter == 0xffffffffU ? counter : counter + 1;
}

bool someip_e2e_p07_valid(std::span<const std::uint8_t> data, std::uint32_t data_id) {
    return data.size() >= header_bytes && get(data, 8, 4) == data.size() &&
           get(data, 16, 4) == data_id && get(data, 0, 8) == someip_crc64(data.subspan(8));
}
} // namespace voyemu
