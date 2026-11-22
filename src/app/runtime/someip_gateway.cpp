#include "someip_gateway.hpp"
#include "error.hpp"
#include "someip_e2e.hpp"
#include <algorithm>
#include <stdexcept>
namespace voyemu {
namespace {
using Address = std::array<std::uint8_t, 4>;
constexpr Address sd_group{239, 5, 1, 2};
constexpr std::uint32_t ttl_seconds = 5;
constexpr auto offer_period = std::chrono::seconds(1);
constexpr auto event_period = std::chrono::milliseconds(200);

Address own_address(unsigned vlan) {
    return {172, 16, static_cast<std::uint8_t>(100 + vlan), 10};
}

std::uint64_t get(std::span<const std::uint8_t> data, std::size_t at, unsigned bytes) {
    if (at + bytes > data.size()) {
        throw VoyemuError("SOMEIP_FRAME", "truncated field");
    }

    std::uint64_t value = 0;

    for (unsigned i = 0; i < bytes; ++i) {
        value = (value << 8) | data[at + i];
    }

    return value;
}

void put(std::vector<std::uint8_t> &out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * (bytes - 1 - i))));
    }
}

void put_at(std::span<std::uint8_t> out, std::size_t at, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        out[at + i] = static_cast<std::uint8_t>(value >> (8 * (bytes - 1 - i)));
    }
}

// SOME/IP header: service, method, length, client, session, protocol 1, interface, type, code.
std::vector<std::uint8_t> someip(std::uint16_t service, std::uint16_t method, std::uint16_t client,
                                 std::uint16_t session, std::uint8_t interface, std::uint8_t type,
                                 std::uint8_t code, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out;
    put(out, service, 2);
    put(out, method, 2);
    put(out, 8 + payload.size(), 4);
    put(out, client, 2);
    put(out, session, 2);
    out.insert(out.end(), {1, interface, type, code});
    out.insert(out.end(), payload.begin(), payload.end());

    return out;
}

std::uint16_t next_session(std::uint16_t &session) {
    session = session == 0xffff ? 1 : session + 1;

    return session;
}

std::vector<SomeipField> parse_fields(const Json &items, std::size_t bytes) {
    std::vector<SomeipField> fields;

    for (const auto &f : items) {
        SomeipField field{f.at("name").get<std::string>(), f.at("offset").get<std::size_t>(),
                          f.at("bytes").get<unsigned>()};

        if (field.bytes < 1 || field.bytes > 8 || field.offset + field.bytes > bytes) {
            throw VoyemuError("SOMEIP_CONTRACT", "invalid field extent");
        }

        fields.push_back(std::move(field));
    }

    return fields;
}

Json fields_json(const std::vector<SomeipField> &fields) {
    Json out = Json::array();

    for (const auto &field : fields) {
        out.push_back({{"name", field.name}, {"offset", field.offset}, {"bytes", field.bytes}});
    }

    return out;
}

// Raw catalogue values into their fields; returns the number of changed fields.
unsigned write_fields(const Json &raw, const std::vector<SomeipField> &fields,
                      std::vector<std::uint8_t> &payload) {
    unsigned changed = 0;

    for (const auto &field : fields) {
        const auto value = raw.find(field.name);

        if (value == raw.end() ||
            !(value->is_number_unsigned() ||
              (value->is_number_integer() && value->get<std::int64_t>() >= 0)) ||
            field.name.find(".E2E_Header.") != std::string::npos) {
            continue;
        }

        const std::uint64_t limit = field.bytes == 8 ? ~0ULL : (1ULL << (8 * field.bytes)) - 1;
        const auto bounded = std::min(value->get<std::uint64_t>(), limit);
        std::array<std::uint8_t, 8> before{};
        std::copy_n(payload.begin() + field.offset, field.bytes, before.begin());
        put_at(payload, field.offset, bounded, field.bytes);

        if (!std::equal(before.begin(), before.begin() + field.bytes,
                        payload.begin() + field.offset)) {
            ++changed;
        }
    }

    return changed;
}

std::uint16_t checksum(std::span<const std::uint8_t> header) {
    std::uint32_t sum = 0;

    for (std::size_t i = 0; i + 1 < header.size(); i += 2) {
        sum += (std::uint32_t(header[i]) << 8) | header[i + 1];
    }

    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }

    return static_cast<std::uint16_t>(~sum);
}
} // namespace

SomeipContract someip_contract(const Json &prepared) {
    if (!prepared.is_object() || prepared.value("schema", 0) != 1 ||
        !prepared.contains("services") || !prepared.at("services").is_array()) {
        throw VoyemuError("SOMEIP_CONTRACT", "prepared gateway contract required");
    }

    SomeipContract contract;

    for (const auto &item : prepared.at("services")) {
        SomeipService service{
                item.at("name").get<std::string>(),      item.at("vlan").get<unsigned>(),
                item.at("service").get<std::uint16_t>(), item.at("instance").get<std::uint16_t>(),
                item.at("major").get<std::uint8_t>(),    {}};

        if (service.vlan < 1 || service.vlan > 4094) {
            throw VoyemuError("SOMEIP_CONTRACT", "invalid VLAN");
        }

        for (const auto &e : item.at("events")) {
            SomeipEvent event{e.at("name").get<std::string>(),
                              e.at("event").get<std::uint16_t>(),
                              e.at("eventgroups").get<std::vector<std::uint16_t>>(),
                              e.at("bytes").get<std::size_t>(),
                              e.at("e2e").get<bool>(),
                              {}};

            if (event.bytes > 1400 || (event.e2e && event.bytes < 20) ||
                event.eventgroups.empty()) {
                throw VoyemuError("SOMEIP_CONTRACT", "invalid event extent");
            }

            event.fields = parse_fields(e.at("fields"), event.bytes);
            service.events.push_back(std::move(event));
        }

        for (const auto &m : item.at("methods")) {
            SomeipMethod method{m.at("name").get<std::string>(),
                                m.at("method").get<std::uint16_t>(),
                                m.at("bytes").get<std::size_t>(),
                                {}};

            if (method.bytes > 1400 || method.id >= 0x8000) {
                throw VoyemuError("SOMEIP_CONTRACT", "invalid method");
            }

            method.fields = parse_fields(m.at("fields"), method.bytes);
            service.methods.push_back(std::move(method));
        }

        contract.services.push_back(std::move(service));
    }

    return contract;
}

Json someip_contract_json(const SomeipContract &contract) {
    Json services = Json::array();

    for (const auto &service : contract.services) {
        Json events = Json::array();

        Json methods = Json::array();

        for (const auto &event : service.events) {
            events.push_back({{"name", event.name},
                              {"event", event.id},
                              {"eventgroups", event.eventgroups},
                              {"bytes", event.bytes},
                              {"e2e", event.e2e},
                              {"fields", fields_json(event.fields)}});
        }

        for (const auto &method : service.methods) {
            methods.push_back({{"name", method.name},
                               {"method", method.id},
                               {"bytes", method.bytes},
                               {"fields", fields_json(method.fields)}});
        }

        services.push_back({{"name", service.name},
                            {"vlan", service.vlan},
                            {"service", service.id},
                            {"instance", service.instance},
                            {"major", service.major},
                            {"events", events},
                            {"methods", methods}});
    }

    return {{"schema", 1}, {"services", services}};
}

SomeipGateway::SomeipGateway(SomeipContract contract) : contract_(std::move(contract)) {
    for (std::size_t s = 0; s < contract_.services.size(); ++s) {
        for (std::size_t e = 0; e < contract_.services[s].events.size(); ++e) {
            payloads_[{s, e}].assign(contract_.services[s].events[e].bytes, 0);
        }

        for (std::size_t m = 0; m < contract_.services[s].methods.size(); ++m) {
            returns_[{s, m}].assign(contract_.services[s].methods[m].bytes, 0);
        }
    }
}

std::size_t SomeipGateway::subscriptions() const {
    return subscribers_.size();
}

unsigned SomeipGateway::apply(const Json &state) {
    if (!state.is_object() || state.value("map_sha256", std::string{}) != passion_map_sha256 ||
        !state.contains("raw_oem") || !state.at("raw_oem").is_object()) {
        throw VoyemuError("SOMEIP_STATE", "Passion catalogue state required");
    }

    const auto &raw = state.at("raw_oem");
    unsigned changed = 0;

    for (auto &[key, payload] : returns_) {
        write_fields(raw, contract_.services[key.first].methods[key.second].fields, payload);
    }

    for (auto &[key, payload] : payloads_) {
        const auto &event = contract_.services[key.first].events[key.second];
        const auto event_changes = write_fields(raw, event.fields, payload);
        changed += event_changes;

        // A changed event is sent at once to every subscriber of one of its groups.
        for (const auto &subscriber : event_changes ? subscribers_ : std::vector<Subscriber>{}) {
            if (subscriber.service == key.first &&
                std::find(event.eventgroups.begin(), event.eventgroups.end(),
                          subscriber.eventgroup) != event.eventgroups.end()) {
                notify(subscriber);
            }
        }
    }

    return changed;
}

void SomeipGateway::receive(std::span<const std::uint8_t> frame, Clock::time_point now) {
    if (frame.size() < 18 || get(frame, 12, 2) != 0x8100) {
        return;
    }

    const unsigned vlan = get(frame, 14, 2) & 0x0fff;
    const auto type = get(frame, 16, 2);
    std::array<std::uint8_t, 6> source{};
    std::copy(frame.begin() + 6, frame.begin() + 12, source.begin());
    const auto payload = frame.subspan(18);
    const auto own = own_address(vlan);

    if (type == 0x0806 && payload.size() >= 28 && get(payload, 6, 2) == 1) {
        Address sender{payload[14], payload[15], payload[16], payload[17]},
                target{payload[24], payload[25], payload[26], payload[27]};
        stations_[{vlan, sender}] = {source, true};

        if (target == own) {
            std::vector<std::uint8_t> out(source.begin(), source.end());
            out.insert(out.end(), mac.begin(), mac.end());
            put(out, 0x8100, 2);
            put(out, (vlan >= 7 ? vlan - 7 : 0) << 13 | vlan, 2);
            put(out, 0x0806, 2);
            out.insert(out.end(), {0, 1, 8, 0, 6, 4, 0, 2});
            out.insert(out.end(), mac.begin(), mac.end());
            out.insert(out.end(), own.begin(), own.end());
            out.insert(out.end(), source.begin(), source.end());
            out.insert(out.end(), sender.begin(), sender.end());
            queue_.push_back(std::move(out));
        }

        return;
    }

    if (type != 0x0800 || payload.size() < 28 || (payload[0] >> 4) != 4 || payload[9] != 17) {
        return;
    }

    const std::size_t ihl = std::size_t(payload[0] & 0x0f) * 4;
    const auto total = get(payload, 2, 2);

    if (ihl < 20 || total > payload.size() || total < ihl + 8) {
        return;
    }

    Address from{payload[12], payload[13], payload[14], payload[15]},
            to{payload[16], payload[17], payload[18], payload[19]};
    stations_[{vlan, from}] = {source, true};
    const auto udp = payload.subspan(ihl, total - ihl);
    const auto source_port = static_cast<std::uint16_t>(get(udp, 0, 2)),
               port = static_cast<std::uint16_t>(get(udp, 2, 2));

    if (port == sd_port && (to == own || to == sd_group)) {
        sd(vlan, from, source_port, udp.subspan(8), now);
    } else if (port == event_port && to == own) {
        request(vlan, from, source_port, udp.subspan(8));
    }
}

void SomeipGateway::request(unsigned vlan, const Address &source, std::uint16_t port,
                            std::span<const std::uint8_t> message) {
    // A REQUEST to an offered service; an unknown method gets E_UNKNOWN_METHOD.
    if (message.size() < 16 || message[12] != 1 || message[14] != 0x00 ||
        get(message, 4, 4) + 8 > message.size()) {
        return;
    }

    const auto id = get(message, 0, 2), method = get(message, 2, 2);
    const auto client = static_cast<std::uint16_t>(get(message, 8, 2)),
               session = static_cast<std::uint16_t>(get(message, 10, 2));

    for (std::size_t s = 0; s < contract_.services.size(); ++s) {
        const auto &service = contract_.services[s];

        if (service.vlan != vlan || service.id != id) {
            continue;
        }

        for (std::size_t m = 0; m < service.methods.size(); ++m) {
            if (service.methods[m].id == method) {
                send_udp(vlan, source, event_port, port,
                         someip(service.id, service.methods[m].id, client, session, service.major,
                                0x80, 0x00, returns_.at({s, m})));

                return;
            }
        }

        send_udp(vlan, source, event_port, port,
                 someip(service.id, static_cast<std::uint16_t>(method), client, session,
                        service.major, 0x81, 0x03, {}));

        return;
    }
}

void SomeipGateway::sd(unsigned vlan, const Address &source, std::uint16_t port,
                       std::span<const std::uint8_t> message, Clock::time_point now) {
    if (message.size() < 28 || get(message, 0, 4) != 0xffff8100) {
        return;
    }

    const auto body = message.subspan(16);
    const auto entries_bytes = get(body, 4, 4);

    if (8 + entries_bytes + 4 > body.size() || entries_bytes % 16) {
        return;
    }

    const auto entries = body.subspan(8, entries_bytes);
    const auto options_bytes = get(body, 8 + entries_bytes, 4);
    const auto options =
            body.subspan(12 + entries_bytes,
                         std::min<std::size_t>(options_bytes, body.size() - 12 - entries_bytes));
    auto endpoint = [&](unsigned index, unsigned count) {
        std::optional<Endpoint> found;
        std::size_t at = 0;

        for (unsigned i = 0; at + 3 <= options.size(); ++i) {
            const auto length = get(options, at, 2);

            if (i >= index && i < index + count && options[at + 2] == 0x04 && length == 9 &&
                at + 12 <= options.size() && options[at + 9] == 0x11) {
                found = Endpoint{
                        {options[at + 4], options[at + 5], options[at + 6], options[at + 7]},
                        static_cast<std::uint16_t>(get(options, at + 10, 2))};
            }

            at += 3 + length;
        }

        return found;
    };

    for (std::size_t at = 0; at < entries.size(); at += 16) {
        const auto entry = entries.subspan(at, 16);
        const auto type = entry[0];
        const auto id = get(entry, 4, 2), instance = get(entry, 6, 2);
        const auto ttl = get(entry, 9, 3);

        for (std::size_t s = 0; s < contract_.services.size(); ++s) {
            const auto &service = contract_.services[s];

            if (service.vlan != vlan || (id != service.id && !(type == 0x00 && id == 0xffff)) ||
                (instance != service.instance && instance != 0xffff)) {
                continue;
            }

            if (type == 0x00) {
                offer(vlan, s, source, port, true);
            } else if (type == 0x06) {
                const auto group = static_cast<std::uint16_t>(get(entry, 14, 2));
                const auto client = endpoint(entry[1], entry[3] >> 4);
                std::erase_if(subscribers_, [&](const Subscriber &item) {
                    return item.service == s && item.eventgroup == group && client &&
                           item.endpoint == *client;
                });
                bool known = false;

                for (const auto &event : service.events) {
                    known = known || std::find(event.eventgroups.begin(), event.eventgroups.end(),
                                               group) != event.eventgroups.end();
                }

                if (ttl == 0) {
                    continue;
                }

                // Acknowledgement echoes the entry; an unknown group or endpoint gets TTL 0 (NACK).
                std::vector<std::uint8_t> ack(entry.begin(), entry.end());
                ack[0] = 0x07;
                ack[1] = ack[2] = ack[3] = 0;

                if (!known || !client) {
                    ack[9] = ack[10] = ack[11] = 0;
                }

                send_sd(vlan, source, port, ack, {}, true);

                if (known && client) {
                    subscribers_.push_back(
                            {vlan, s, group, *client, now + std::chrono::seconds(ttl)});
                    notify(subscribers_.back());
                }
            }
        }
    }
}

void SomeipGateway::offer(unsigned vlan, std::size_t index, const Address &destination,
                          std::uint16_t port, bool unicast) {
    const auto &service = contract_.services[index];
    std::vector<std::uint8_t> entry{0x01, 0, 0, 0x10};
    put(entry, service.id, 2);
    put(entry, service.instance, 2);
    entry.push_back(service.major);
    put(entry, ttl_seconds, 3);
    put(entry, 0, 4);
    const auto own = own_address(vlan);
    std::vector<std::uint8_t> option{0x00, 0x09, 0x04, 0x00};
    option.insert(option.end(), own.begin(), own.end());
    option.insert(option.end(), {0x00, 0x11});
    put(option, event_port, 2);
    send_sd(vlan, destination, port, entry, option, unicast);
}

void SomeipGateway::send_sd(unsigned vlan, const Address &destination, std::uint16_t port,
                            std::span<const std::uint8_t> entries,
                            std::span<const std::uint8_t> options, bool unicast) {
    std::vector<std::uint8_t> body{static_cast<std::uint8_t>(0x80 | (unicast ? 0x40 : 0)), 0, 0, 0};
    put(body, entries.size(), 4);
    body.insert(body.end(), entries.begin(), entries.end());
    put(body, options.size(), 4);
    body.insert(body.end(), options.begin(), options.end());
    send_udp(vlan, destination, sd_port, port,
             someip(0xffff, 0x8100, 0, next_session(sd_session_), 1, 0x02, 0x00, body));
}

void SomeipGateway::notify(const Subscriber &subscriber) {
    const auto &service = contract_.services[subscriber.service];

    for (std::size_t e = 0; e < service.events.size(); ++e) {
        const auto &event = service.events[e];

        if (std::find(event.eventgroups.begin(), event.eventgroups.end(), subscriber.eventgroup) ==
            event.eventgroups.end()) {
            continue;
        }

        auto payload = payloads_.at({subscriber.service, e});

        if (event.e2e) {
            auto &counter = counters_[{subscriber.service, e}];
            counter = someip_e2e_p07_protect(payload, std::uint32_t(service.id) << 16 | event.id,
                                             counter);
        }

        send_udp(subscriber.vlan, subscriber.endpoint.ip, event_port, subscriber.endpoint.port,
                 someip(service.id, event.id, 0, next_session(event_session_), service.major, 0x02,
                        0x00, payload));
    }
}

void SomeipGateway::send_udp(unsigned vlan, const Address &destination, std::uint16_t source_port,
                             std::uint16_t port, std::span<const std::uint8_t> payload) {
    std::array<std::uint8_t, 6> target{};

    if (destination[0] >= 224 && destination[0] <= 239) {
        target = {0x01,           0x00,
                  0x5e,           static_cast<std::uint8_t>(destination[1] & 0x7f),
                  destination[2], destination[3]};
    } else {
        const auto station = stations_.find({vlan, destination});

        if (station == stations_.end()) {
            return;
        }

        target = station->second.mac;
    }

    std::vector<std::uint8_t> out(target.begin(), target.end());
    out.insert(out.end(), mac.begin(), mac.end());
    put(out, 0x8100, 2);
    put(out, (vlan >= 7 ? vlan - 7 : 0) << 13 | vlan, 2);
    put(out, 0x0800, 2);
    const auto ip = out.size();
    out.insert(out.end(), {0x45, 0x00});
    put(out, 28 + payload.size(), 2);
    put(out, next_session(ip_id_), 2);
    out.insert(out.end(),
               {0x40, 0x00, static_cast<std::uint8_t>(destination[0] >= 224 ? 1 : 64), 17, 0, 0});
    const auto own = own_address(vlan);
    out.insert(out.end(), own.begin(), own.end());
    out.insert(out.end(), destination.begin(), destination.end());
    put_at(out, ip + 10, checksum(std::span(out).subspan(ip, 20)), 2);
    put(out, source_port, 2);
    put(out, port, 2);
    put(out, 8 + payload.size(), 2);
    put(out, 0, 2);
    out.insert(out.end(), payload.begin(), payload.end());

    // A stalled guest relay keeps only the newest backlog.
    if (queue_.size() >= 1024) {
        queue_.pop_front();
    }

    if (out.size() <= 1518) {
        queue_.push_back(std::move(out));
    }
}

bool SomeipGateway::next(std::vector<std::uint8_t> &frame, Clock::time_point now) {
    std::erase_if(subscribers_, [&](const Subscriber &item) {
        return item.expires <= now;
    });

    for (const auto &service : contract_.services) {
        auto &due = next_offer_[service.vlan];

        if (due <= now) {
            due = now + offer_period;

            for (std::size_t s = 0; s < contract_.services.size(); ++s) {
                if (contract_.services[s].vlan == service.vlan) {
                    offer(service.vlan, s, sd_group, sd_port, false);
                }
            }
        }
    }

    if (next_cycle_ <= now) {
        next_cycle_ = now + event_period;

        for (const auto &subscriber : subscribers_) {
            notify(subscriber);
        }
    }

    if (queue_.empty()) {
        return false;
    }

    frame = std::move(queue_.front());
    queue_.pop_front();

    return true;
}
} // namespace voyemu
