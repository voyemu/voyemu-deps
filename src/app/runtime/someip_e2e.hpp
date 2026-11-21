#pragma once
#include <cstdint>
#include <span>
namespace voyemu {
// CRC-64/XZ (reflected ECMA-182), the CRC of AUTOSAR E2E profile 7.
std::uint64_t someip_crc64(std::span<const std::uint8_t> data);
// AUTOSAR E2E profile 7 header at offset 0: CRC64, Length, Counter and DataID, big-endian. The CRC
// covers the bytes after its own field. Returns the next counter, which saturates at its maximum
// as in the Passion cluster's receiver.
std::uint32_t someip_e2e_p07_protect(std::span<std::uint8_t> data, std::uint32_t data_id,
                                     std::uint32_t counter);
bool someip_e2e_p07_valid(std::span<const std::uint8_t> data, std::uint32_t data_id);
} // namespace voyemu
