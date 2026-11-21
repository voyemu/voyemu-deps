#pragma once
#include "frame_transport.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace voyemu::graphics {
inline constexpr std::uint64_t ram_base = 0x40000000ULL, ram_bytes = 8ULL * 1024 * 1024 * 1024;
inline constexpr std::uint32_t rpc_magic = 0x58464751, rpc_version = 3, rpc_payload_bytes = 3840;
inline constexpr std::uint64_t input_wake_magic =
        0x31454b41574e4951ULL; // Optional QINWAKE1 in reserved packet words176/184/192.
using Arguments = std::array<std::uint64_t, 16>;
struct Reply {
    std::uint64_t result = 0;
    std::vector<std::byte> payload;
    std::string name;
};
struct Options {
    std::filesystem::path ram, session, angle, frame;
    std::uint64_t address = 0;
    bool controls = true, profile_rpc = false;
    std::filesystem::path record, replay;
    std::uint64_t ram_extent = ram_bytes;
    std::uint64_t generation = 0, worker_nonce = 0;
    std::uint64_t guest_pid = 0, producer_incarnation = 0;
    std::string session_id;
    // hardware_adapter of a qinggan11 cluster, which names the MCU layout of its requests.
    std::string mcu_adapter;
};
inline constexpr int rpc_channel_invalidated_exit_code = 70;
inline constexpr std::uint32_t owner_liveness_request_opcode = 367,
                               owner_liveness_report_opcode = 368;
class RpcChannelInvalidated final : public std::runtime_error {
public:
    explicit RpcChannelInvalidated(const char *reason = "Published RPC page header")
        : std::runtime_error(reason) {}
};
void write_invalidated_channel_disposition(const Options &);
// Pure protocol guards also exercised independently of ANGLE/guest execution.
void validate_ram_extent(std::uint64_t address, std::uint64_t size,
                         std::uint64_t extent = ram_bytes);
void validate_mcd_batch(std::uint64_t count, std::uint64_t dropped, std::span<const std::byte>);
void validate_inline_client(const Arguments &, std::span<const std::byte>);
void validate_inline_uniform(std::string_view name, const Arguments &, std::span<const std::byte>);
struct BatchRecord {
    std::uint32_t opcode;
    Arguments arguments;
    std::vector<std::byte> payload;
};
std::vector<BatchRecord> validate_batch(std::uint64_t count, std::span<const std::byte>);
// Preserve 100 us RPC bursts; only a worker idle for 100 ms may wait up to 500 us.
class RpcIdleWait {
public:
    void observe(bool request, std::uint64_t now_ns) {
        if (request) {
            idle_ = false;
            elapsed_ = 0;

            return;
        }

        if (!idle_) {
            idle_ = true;
            since_ = now_ns;
        }

        elapsed_ = now_ns >= since_ ? now_ns - since_ : 0;
    }

    unsigned microseconds(bool input_only = false) const {
        if (input_only) {
            return 1000;
        }

        return elapsed_ >= 100000000 ? 500 : 100;
    }

private:
    std::uint64_t since_ = 0, elapsed_ = 0;
    bool idle_ = false;
};
struct ServiceStats {
    std::uint64_t polls = 0, identity_checks = 0;
};
struct RpcProfileEntry {
    std::uint32_t opcode = 0;
    std::uint64_t calls = 0, faults = 0, dispatch_ns = 0, max_dispatch_ns = 0;
};
struct RpcProfileSnapshot {
    bool enabled = false;
    std::uint64_t first_received_ns = 0, last_acknowledged_ns = 0;
    std::vector<RpcProfileEntry> wire;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> api;
    std::uint64_t unknown_api_calls = 0;
};
class Backend {
public:
    explicit Backend(Options);
    ~Backend();
    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;
    Reply dispatch(std::uint32_t, const Arguments &, std::span<const std::byte>);
    bool service(); // false means no pending packet; finished() distinguishes producer exit.
    bool finished() const;
    ServiceStats service_stats() const;
    unsigned idle_wait_us() const;
    RpcProfileSnapshot rpc_profile() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace voyemu::graphics
