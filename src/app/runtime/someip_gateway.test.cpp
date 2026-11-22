#include "someip_gateway.hpp"
#include "someip_e2e.hpp"
#include <cassert>
#include <iostream>
#include <vector>
using namespace voyemu;
namespace {
using Bytes = std::vector<std::uint8_t>;
const std::array<std::uint8_t, 6> guest_mac{0x02, 0x00, 0x00, 0x00, 0x00, 0x22};
const std::array<std::uint8_t, 4> guest_ip{172, 16, 112, 30}, gateway_ip{172, 16, 112, 10},
        group{239, 5, 1, 2};

void put(Bytes &out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * (bytes - 1 - i))));
    }
}

std::uint64_t get(const Bytes &in, std::size_t at, unsigned bytes) {
    std::uint64_t value = 0;

    for (unsigned i = 0; i < bytes; ++i) {
        value = (value << 8) | in.at(at + i);
    }

    return value;
}

Bytes ethernet(unsigned vlan, std::uint16_t type, const Bytes &payload, bool broadcast) {
    Bytes out;

    for (unsigned i = 0; i < 6; ++i) {
        out.push_back(broadcast ? 0xff : SomeipGateway::mac[i]);
    }

    out.insert(out.end(), guest_mac.begin(), guest_mac.end());
    put(out, 0x8100, 2);
    put(out, vlan, 2);
    put(out, type, 2);
    out.insert(out.end(), payload.begin(), payload.end());

    return out;
}

Bytes udp(const std::array<std::uint8_t, 4> &to, std::uint16_t source_port, std::uint16_t port,
          const Bytes &message) {
    Bytes ip{0x45, 0};
    put(ip, 28 + message.size(), 2);
    ip.insert(ip.end(), {0, 0, 0x40, 0, 1, 17, 0, 0});
    ip.insert(ip.end(), guest_ip.begin(), guest_ip.end());
    ip.insert(ip.end(), to.begin(), to.end());
    put(ip, source_port, 2);
    put(ip, port, 2);
    put(ip, 8 + message.size(), 2);
    put(ip, 0, 2);
    ip.insert(ip.end(), message.begin(), message.end());

    return ethernet(12, 0x0800, ip, false);
}

// The guest's someipd: an SD message from 172.16.112.30:30490.
Bytes sd(const std::array<std::uint8_t, 4> &to, const Bytes &entries, const Bytes &options) {
    Bytes body{0xc0, 0, 0, 0};
    put(body, entries.size(), 4);
    body.insert(body.end(), entries.begin(), entries.end());
    put(body, options.size(), 4);
    body.insert(body.end(), options.begin(), options.end());
    Bytes message;
    put(message, 0xffff8100, 4);
    put(message, 8 + body.size(), 4);
    put(message, 0x00000001, 4);
    put(message, 0x01010200, 4);
    message.insert(message.end(), body.begin(), body.end());

    return udp(to, 30490, 30490, message);
}

// A SOME/IP REQUEST from the guest's client 0x1234, session 0x42, port 50101.
Bytes request(std::uint16_t service, std::uint16_t method) {
    Bytes message;
    put(message, service, 2);
    put(message, method, 2);
    put(message, 8, 4);
    put(message, 0x1234, 2);
    put(message, 0x42, 2);
    message.insert(message.end(), {1, 1, 0x00, 0x00});

    return udp(gateway_ip, 50101, SomeipGateway::event_port, message);
}

Bytes entry(std::uint8_t type, std::uint16_t service, std::uint32_t ttl, std::uint16_t group,
            unsigned options) {
    Bytes out{type, 0, 0, static_cast<std::uint8_t>(options << 4)};
    put(out, service, 2);
    put(out, 1, 2);
    out.push_back(1);
    put(out, ttl, 3);
    put(out, 0, 2);
    put(out, group, 2);

    return out;
}

std::vector<Bytes> drain(SomeipGateway &gateway, SomeipGateway::Clock::time_point now) {
    std::vector<Bytes> out;
    Bytes frame;

    while (gateway.next(frame, now)) {
        out.push_back(frame);
    }

    return out;
}

// UDP payload of a VLAN 12 frame: SOME/IP header at 18 + 20 + 8.
Bytes udp_payload(const Bytes &frame) {
    return Bytes(frame.begin() + 46, frame.end());
}
} // namespace

int main() try {
    const auto contract = someip_contract(Json::parse(R"({"schema":1,"services":[
        {"name":"VehicleDrivingInfo","vlan":12,"service":8192,"instance":1,"major":1,"events":[
          {"name":"Notify_f_VehicleDrivingInform","event":32769,"eventgroups":[8192],"bytes":40,"e2e":true,
           "fields":[{"name":"VehicleDrivingInfo.Notify_f_VehicleDrivingInform.VehicleInform.E2E_Header.E2E_Counter","offset":12,"bytes":4},
             {"name":"VehicleSpeed","offset":32,"bytes":4}]}],"methods":[
          {"name":"Get_f_VehicleDrivingInform","method":1,"bytes":20,
           "fields":[{"name":"VehicleSpeed","offset":12,"bytes":4}]}]},
        {"name":"VI_MN_VehicleModeManager","vlan":14,"service":16384,"instance":1,"major":1,"events":[
          {"name":"NotifyCycle_f_PowerMode_VMM","event":32771,"eventgroups":[16384],"bytes":2,"e2e":false,
           "fields":[{"name":"PowerMode","offset":0,"bytes":1}]}],"methods":[]}]})"));
    auto state = [](std::uint64_t speed) {
        return Json{{"map_sha256", passion_map_sha256},
                    {"raw_oem",
                     {{"VehicleSpeed", speed},
                      {"VehicleDrivingInfo.Notify_f_VehicleDrivingInform.VehicleInform.E2E_Header."
                       "E2E_Counter",
                       77},
                      {"Unrelated", 1}}}};
    };
    assert(someip_contract(someip_contract_json(contract)).services.size() == 2);
    SomeipGateway gateway(contract);
    auto now = SomeipGateway::Clock::now();

    // Cyclic multicast offers, one per VLAN, from 172.16.1XX.10 to 239.5.1.2:30490.
    auto frames = drain(gateway, now);
    assert(frames.size() == 2);
    assert(get(frames[0], 0, 3) == 0x01005e && get(frames[0], 14, 2) == (5 << 13 | 12) &&
           get(frames[0], 30, 4) == 0xac10700a && get(frames[0], 34, 4) == 0xef050102);
    auto offer = udp_payload(frames[0]);
    assert(get(offer, 0, 4) == 0xffff8100 && offer[16] == 0x80 && get(offer, 20, 4) == 16 &&
           offer[24] == 0x01 && get(offer, 28, 2) == 0x2000 && get(offer, 33, 3) == 5);
    assert(get(offer, 40, 4) == 12 && get(offer, 44, 4) == 0x00090400 &&
           get(offer, 48, 4) == 0xac10700a && get(offer, 54, 2) == SomeipGateway::event_port);
    assert(drain(gateway, now).empty());

    // ARP for the gateway on VLAN 12 is answered with its MAC.
    Bytes arp{0, 1, 8, 0, 6, 4, 0, 1};
    arp.insert(arp.end(), guest_mac.begin(), guest_mac.end());
    arp.insert(arp.end(), guest_ip.begin(), guest_ip.end());
    arp.insert(arp.end(), 6, 0);
    arp.insert(arp.end(), gateway_ip.begin(), gateway_ip.end());
    gateway.receive(ethernet(12, 0x0806, arp, true), now);
    frames = drain(gateway, now);
    assert(frames.size() == 1 && get(frames[0], 16, 2) == 0x0806 && get(frames[0], 24, 2) == 2 &&
           Bytes(frames[0].begin() + 26, frames[0].begin() + 32) ==
                   Bytes(SomeipGateway::mac.begin(), SomeipGateway::mac.end()) &&
           get(frames[0], 0, 6) == 0x020000000022);

    // FindService gets a unicast offer back to the finder's SD port.
    gateway.receive(sd(group, entry(0x00, 0x2000, 3, 0, 0), {}), now);
    frames = drain(gateway, now);
    assert(frames.size() == 1 && get(frames[0], 34, 4) == 0xac10701e &&
           udp_payload(frames[0])[16] == 0xc0 && get(frames[0], 0, 6) == 0x020000000022);

    // SubscribeEventgroup with the client's UDP endpoint: ACK, then the initial notification.
    Bytes endpoint{0x00, 0x09, 0x04, 0x00};
    endpoint.insert(endpoint.end(), guest_ip.begin(), guest_ip.end());
    endpoint.insert(endpoint.end(), {0x00, 0x11});
    put(endpoint, 50101, 2);
    assert(gateway.apply(state(53400)) == 1);
    gateway.receive(sd(gateway_ip, entry(0x06, 0x2000, 5, 0x2000, 1), endpoint), now);
    frames = drain(gateway, now);
    assert(frames.size() == 2 && gateway.subscriptions() == 1);
    const auto ack = udp_payload(frames[0]);
    assert(ack[24] == 0x07 && get(ack, 33, 3) == 5 && get(ack, 38, 2) == 0x2000);
    const auto event = udp_payload(frames[1]);
    assert(get(frames[1], 38, 2) == SomeipGateway::event_port && get(frames[1], 40, 2) == 50101);
    assert(get(event, 0, 2) == 0x2000 && get(event, 2, 2) == 0x8001 && get(event, 4, 4) == 48 &&
           event[13] == 1 && event[14] == 0x02);
    Bytes payload(event.begin() + 16, event.end());
    assert(payload.size() == 40 && someip_e2e_p07_valid(payload, 0x20008001) &&
           get(payload, 12, 4) == 0 && get(payload, 32, 4) == 53400);

    // A change is sent at once with the next E2E counter; an unknown group is refused.
    assert(gateway.apply(state(56000)) == 1 && gateway.apply(state(56000)) == 0);
    frames = drain(gateway, now);
    payload.assign(frames.at(0).begin() + 62, frames.at(0).end());
    assert(frames.size() == 1 && get(payload, 12, 4) == 1 && get(payload, 32, 4) == 56000 &&
           someip_e2e_p07_valid(payload, 0x20008001));
    gateway.receive(sd(gateway_ip, entry(0x06, 0x2000, 5, 0x2001, 1), endpoint), now);
    frames = drain(gateway, now);
    assert(frames.size() == 1 && get(udp_payload(frames[0]), 33, 3) == 0);

    // A getter request is answered from the same state to the requester's port with its client and
    // session; an unknown method gets E_UNKNOWN_METHOD, another VLAN's service no answer.
    gateway.receive(request(0x2000, 1), now);
    frames = drain(gateway, now);
    assert(frames.size() == 1 && get(frames[0], 38, 2) == SomeipGateway::event_port &&
           get(frames[0], 40, 2) == 50101 && get(frames[0], 0, 6) == 0x020000000022);
    auto response = udp_payload(frames[0]);
    assert(get(response, 0, 4) == 0x20000001 && get(response, 4, 4) == 28 &&
           get(response, 8, 4) == 0x12340042 && get(response, 12, 4) == 0x01018000 &&
           response.size() == 36 && get(response, 28, 4) == 56000);
    gateway.receive(request(0x2000, 9), now);
    frames = drain(gateway, now);
    response = udp_payload(frames.at(0));
    assert(frames.size() == 1 && get(response, 2, 2) == 9 && get(response, 4, 4) == 8 &&
           get(response, 12, 4) == 0x01018103);
    gateway.receive(request(0x4000, 1), now);
    assert(drain(gateway, now).empty());

    // Cyclic notifications follow the period; the subscription lapses after its TTL.
    frames = drain(gateway, now + std::chrono::milliseconds(250));
    assert(frames.size() == 1 && get(udp_payload(frames[0]), 2, 2) == 0x8001);
    frames = drain(gateway, now + std::chrono::seconds(6));
    assert(gateway.subscriptions() == 0 && frames.size() == 2);
    // Only a Passion catalogue state is applied.
    auto other = state(1);
    other["map_sha256"] = qinggan09_map_sha256;
    bool refused = false;

    try {
        gateway.apply(other);
    } catch (const std::exception &) {
        refused = true;
    }

    assert(refused);
    // Frames for other VLANs, untagged frames and garbage are ignored.
    gateway.receive(Bytes(10, 0), now);
    gateway.receive(Bytes(64, 0xff), now);
    std::cout << "SOME/IP gateway: ARP, SD offer/subscribe/ack, E2E notifications and getters\n";

    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
