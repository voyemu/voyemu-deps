#pragma once
#include "model.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
namespace voyemu {
// Owned QNX RAM mailbox <-> isolated QEMU Ethernet socket. No host networking.
class InterguestLink {
public:
    // UTC completion includes failures; utc_status and fault distinguish verified success.
    struct Status {
        unsigned port = 0;
        bool connected = false, ready = false;
        std::uint64_t generation = 0, received = 0, sent = 0, reconnects = 0;
        std::uint64_t utc_expected = 0, utc_actual = 0;
        int utc_status = 0;
        bool utc_complete = false;
        std::string fault;
    };
    enum class RamGeometry {
        free8g,
        passion
    };
    InterguestLink();
    ~InterguestLink();
    InterguestLink(const InterguestLink &) = delete;
    InterguestLink &operator=(const InterguestLink &) = delete;
    unsigned start(const std::filesystem::path &session, const std::string &token,
                   bool native_diagnostic = false);
    // Call for every owned QNX incarnation, even when the pathname is unchanged.
    void bind_ram(const std::filesystem::path &ram, std::uint64_t console_ring = 0x50010000,
                  const std::filesystem::path &serial = {},
                  RamGeometry geometry = RamGeometry::free8g);
    void clear_ram();
    // The vehicle gateway on the guest Ethernet (see SomeipGateway): guest frames reach it and
    // Android; its frames go to the guest before Android's. The contract is a prepared one.
    void attach_gateway(const Json &contract);
    // Raw catalogue values of a vehicle state for the gateway; returns the changed fields.
    unsigned apply_vehicle_state(const Json &state);
    void bind_android(std::uint64_t pid);
    // Guest sync completion within timeout_ms, at most 30 s scaled by qnx_guest_deadline for the
    // QNX engine of the host; failure leaves the owned guest running.
    void flush(int timeout_ms);
    struct GuestCounters {
        std::uint64_t sent = 0, received = 0;
    };
    GuestCounters guest_counters() const;
    Status status() const;
    void check() const;
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace voyemu
