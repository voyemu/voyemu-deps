#pragma once
#include "model.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace voyemu {
// One signal of an event payload on the wire, named as in the Passion vehicle catalogue.
struct SomeipField {
    std::string name;
    std::size_t offset = 0;
    unsigned bytes = 0;
};
struct SomeipEvent {
    std::string name;
    std::uint16_t id = 0;
    std::vector<std::uint16_t> eventgroups;
    std::size_t bytes = 0;
    bool e2e = false;
    std::vector<SomeipField> fields;
};
struct SomeipService {
    std::string name;
    unsigned vlan = 0;
    std::uint16_t id = 0, instance = 1;
    std::uint8_t major = 1;
    std::vector<SomeipEvent> events;
};
// Prepared from the cluster's pinned binaries; see qnx_passion_someip_contract.
struct SomeipContract {
    std::vector<SomeipService> services;
};
SomeipContract someip_contract(const Json &prepared);
Json someip_contract_json(const SomeipContract &contract);

// The vehicle gateway core the cluster expects on its SOME/IP VLANs: one MAC, address
// 172.16.(100+VLAN).10 on each VLAN. It answers ARP and service discovery, acknowledges
// eventgroup subscriptions and sends notifications, E2E profile 7 protected where the payload
// carries the header (DataID = service << 16 | event).
class SomeipGateway {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::array<std::uint8_t, 6> mac{0x02, 0xdf, 0x53, 0x00, 0x00, 0x00};
    static constexpr std::uint16_t sd_port = 30490, event_port = 30501;

    explicit SomeipGateway(SomeipContract contract);
    // A guest frame; frames for other stations are ignored.
    void receive(std::span<const std::uint8_t> frame, Clock::time_point now);
    // The next frame for the guest, if one is due.
    bool next(std::vector<std::uint8_t> &frame, Clock::time_point now);
    // Raw catalogue values of a vehicle state (raw_oem); changed events are sent at once. Returns
    // the number of changed fields. The E2E header is the gateway's own.
    unsigned apply(const Json &state);
    std::size_t subscriptions() const;

private:
    struct Endpoint {
        std::array<std::uint8_t, 4> ip{};
        std::uint16_t port = 0;
        auto operator<=>(const Endpoint &) const = default;
    };
    struct Subscriber {
        unsigned vlan = 0;
        std::size_t service = 0;
        std::uint16_t eventgroup = 0;
        Endpoint endpoint;
        Clock::time_point expires;
    };
    struct Station {
        std::array<std::uint8_t, 6> mac{};
        bool known = false;
    };
    SomeipContract contract_;
    std::map<std::pair<std::size_t, std::size_t>, std::vector<std::uint8_t>> payloads_;
    std::map<std::pair<std::size_t, std::size_t>, std::uint32_t> counters_;
    std::vector<Subscriber> subscribers_;
    std::map<std::pair<unsigned, std::array<std::uint8_t, 4>>, Station> stations_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::map<unsigned, Clock::time_point> next_offer_;
    Clock::time_point next_cycle_{};
    std::uint16_t sd_session_ = 0, event_session_ = 0, ip_id_ = 0;

    void sd(unsigned vlan, const std::array<std::uint8_t, 4> &source, std::uint16_t port,
            std::span<const std::uint8_t> message, Clock::time_point now);
    void send_udp(unsigned vlan, const std::array<std::uint8_t, 4> &destination,
                  std::uint16_t source_port, std::uint16_t port,
                  std::span<const std::uint8_t> payload);
    void send_sd(unsigned vlan, const std::array<std::uint8_t, 4> &destination, std::uint16_t port,
                 std::span<const std::uint8_t> entries, std::span<const std::uint8_t> options,
                 bool unicast);
    void offer(unsigned vlan, std::size_t service, const std::array<std::uint8_t, 4> &destination,
               std::uint16_t port, bool unicast);
    void notify(const Subscriber &subscriber);
};
} // namespace voyemu
