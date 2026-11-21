#include "qnx_passion_someip.hpp"
#include "check.hpp"
#include "error.hpp"
#include "little_endian.hpp"
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
namespace voyemu {
namespace {
constexpr CodeCheck need{"Passion SOME/IP input differs"};

std::uint64_t field(std::string_view bytes, std::uint64_t at, unsigned width) {
    return le_read(bytes, at, width, need, "QNX_PASSION_SOMEIP_ELF");
}

struct Symbol {
    std::uint64_t value, size;
};
struct Elf {
    std::string_view bytes;
    std::map<std::string, Symbol, std::less<>> symbols;
    std::vector<std::array<std::uint64_t, 3>> loads; // vaddr, offset, file size
    std::uint64_t text = 0, text_size = 0, text_offset = 0;

    explicit Elf(std::string_view image) : bytes(image) {
        need(image.substr(0, 4) == "\x7f"
                                   "ELF" &&
                     field(image, 4, 1) == 2 && field(image, 5, 1) == 1 &&
                     field(image, 18, 2) == 183,
             "QNX_PASSION_SOMEIP_ELF");
        const auto phoff = field(image, 32, 8), shoff = field(image, 40, 8);
        const auto phnum = field(image, 56, 2), shnum = field(image, 60, 2),
                   shstrndx = field(image, 62, 2);

        for (std::uint64_t i = 0; i < phnum; ++i) {
            const auto at = phoff + i * 56;

            if (field(image, at, 4) == 1) {
                loads.push_back({field(image, at + 16, 8), field(image, at + 8, 8),
                                 field(image, at + 32, 8)});
            }
        }

        auto section = [&](std::uint64_t index, unsigned at, unsigned width) {
            return field(image, shoff + index * 64 + at, width);
        };
        const auto names = section(shstrndx, 24, 8);

        for (std::uint64_t i = 0; i < shnum; ++i) {
            if (cstring(image, names + section(i, 0, 4)) == ".text") {
                this->text = section(i, 16, 8);
                text_offset = section(i, 24, 8);
                text_size = section(i, 32, 8);
            }

            if (section(i, 4, 4) != 2) {
                continue;
            }

            const auto offset = section(i, 24, 8), size = section(i, 32, 8);
            const auto strings = section(section(i, 40, 4), 24, 8);

            for (std::uint64_t at = offset; at + 24 <= offset + size; at += 24) {
                symbols[cstring(image, strings + field(image, at, 4))] = {field(image, at + 8, 8),
                                                                          field(image, at + 16, 8)};
            }
        }

        need(text_size && !symbols.empty() && !loads.empty(), "QNX_PASSION_SOMEIP_ELF");
    }

    static std::string cstring(std::string_view image, std::uint64_t at) {
        need(at < image.size(), "QNX_PASSION_SOMEIP_ELF");
        const auto end = image.find('\0', at);
        need(end != std::string_view::npos && end - at <= 4096, "QNX_PASSION_SOMEIP_ELF");

        return std::string(image.substr(at, end - at));
    }

    std::uint64_t offset(std::uint64_t vaddr, std::uint64_t width) const {
        for (const auto &[base, file, size] : loads) {
            if (vaddr >= base && vaddr + width <= base + size) {
                return vaddr - base + file;
            }
        }

        need(false, "QNX_PASSION_SOMEIP_ADDRESS");

        return 0;
    }

    std::uint64_t read(std::uint64_t vaddr, unsigned width) const {
        return field(bytes, offset(vaddr, width), width);
    }
};

std::int64_t sign(std::uint64_t value, unsigned bits) {
    return static_cast<std::int64_t>(value << (64 - bits)) >> (64 - bits);
}

// The string the x0 argument of each `bl target` carries, from adrp x0 + add x0, x0, #imm right
// before it.
std::map<std::uint64_t, std::set<std::string>>
call_strings(const Elf &elf, const std::set<std::uint64_t> &targets) {
    std::map<std::uint64_t, std::set<std::string>> found;
    auto word = [&](std::uint64_t at) {
        return static_cast<std::uint32_t>(field(elf.bytes, elf.text_offset + at, 4));
    };

    for (std::uint64_t at = 8; at + 4 <= elf.text_size; at += 4) {
        const auto bl = word(at);

        if ((bl & 0xfc000000U) != 0x94000000U) {
            continue;
        }

        const auto pc = elf.text + at;
        const auto target = pc + sign(bl & 0x3ffffff, 26) * 4;

        if (!targets.contains(target)) {
            continue;
        }

        for (std::uint64_t back = at - 4; back + 20 >= at && back >= 4; back -= 4) {
            const auto adrp = word(back - 4), add = word(back);

            if ((adrp & 0x9f00001fU) == 0x90000000U && (add & 0xffc003ffU) == 0x91000000U) {
                const auto page = ((elf.text + back - 4) & ~0xfffULL) +
                                  sign((((adrp >> 5) & 0x7ffff) << 2) | ((adrp >> 29) & 3), 21) *
                                          4096;
                const auto address = page + ((add >> 10) & 0xfff);
                found[target].insert(Elf::cstring(elf.bytes, elf.offset(address, 1)));
                break;
            }
        }
    }

    return found;
}
} // namespace

Json qnx_passion_someip_contract(std::string_view ic_service, const Json &service_table) {
    const Elf elf(ic_service);
    std::map<std::string, std::uint16_t> clients;
    std::map<std::uint64_t, std::string> starts;

    for (const auto &[name, symbol] : elf.symbols) {
        constexpr std::string_view suffix = "_client_data";

        if (name.ends_with(suffix) && symbol.size >= 0x20) {
            const auto service = name.substr(0, name.size() - suffix.size());
            const auto start = elf.symbols.find("start_" + service + "_client");

            if (start != elf.symbols.end()) {
                clients[service] = static_cast<std::uint16_t>(elf.read(symbol.value, 2));
                starts[start->second.value] = service;
            }
        }
    }

    std::set<std::uint64_t> targets;

    for (const auto &item : starts) {
        targets.insert(item.first);
    }

    const auto strings = call_strings(elf, targets);
    std::map<std::string, Json> services;

    for (const auto &[address, service] : starts) {
        const auto at = strings.find(address);
        need(at != strings.end() && at->second.size() == 1 &&
                     at->second.begin()->starts_with("VLAN"),
             "QNX_PASSION_SOMEIP_VLAN");
        const auto vlan = std::stoul(at->second.begin()->substr(4));
        need(vlan >= 1 && vlan <= 4094, "QNX_PASSION_SOMEIP_VLAN");
        services[service] = {{"name", service}, {"vlan", vlan}, {"service", clients.at(service)},
                             {"instance", 1},   {"major", 1},   {"events", Json::array()}};
    }

    // Service table: service name -> event name -> signals.
    std::map<std::string, std::map<std::string, const Json *>> table;

    for (const auto &service : service_table.at("servicedefine")) {
        for (const auto &event : service.at("method_event_define")) {
            table[service.at("ServiceName").get<std::string>()]
                 [event.at("Method_Event_Name").get<std::string>()] = &event.at("Signals");
        }
    }

    for (const auto &[name, symbol] : elf.symbols) {
        constexpr std::string_view prefix = "event_field_notifier_";

        if (!name.starts_with(prefix) || symbol.size != 64) {
            continue;
        }

        const auto event_name = name.substr(prefix.size());
        const auto groups = elf.read(symbol.value, 4);
        need(groups >= 1 && groups <= 16, "QNX_PASSION_SOMEIP_EVENT");
        Json eventgroups = Json::array();

        for (unsigned i = 0; i < groups; ++i) {
            eventgroups.push_back(elf.read(symbol.value + 4 + 2 * i, 2));
        }

        const auto service_id = elf.read(symbol.value + 0x28, 2),
                   event_id = elf.read(symbol.value + 0x34, 2),
                   instance = elf.read(symbol.value + 0x2a, 2),
                   major = elf.read(symbol.value + 0x2c, 2);
        need(event_id >= 0x8000 && major >= 1 && major <= 0xff, "QNX_PASSION_SOMEIP_EVENT");

        for (auto &[service_name, service] : services) {
            if (service.at("service") != service_id || !table.contains(service_name) ||
                !table.at(service_name).contains(event_name)) {
                continue;
            }

            service["instance"] = instance;
            service["major"] = major;
            Json fields = Json::array();
            std::size_t offset = 0;
            const std::string signal_prefix = "8155QNX.";

            for (const auto &signal : *table.at(service_name).at(event_name)) {
                const auto signal_name = signal.at("name").get<std::string>();
                const auto bytes = signal.at("len").get<unsigned>();
                need(signal_name.starts_with(signal_prefix) && bytes >= 1 && bytes <= 8,
                     "QNX_PASSION_SOMEIP_SIGNAL");
                fields.push_back({{"name", signal_name.substr(signal_prefix.size())},
                                  {"offset", offset},
                                  {"bytes", bytes}});
                offset += bytes;
            }

            const bool e2e =
                    !fields.empty() &&
                    fields.front().at("name").get<std::string>().ends_with(".E2E_Header.E2E_CRC");
            service["events"].push_back({{"name", event_name},
                                         {"event", event_id},
                                         {"eventgroups", eventgroups},
                                         {"bytes", offset},
                                         {"e2e", e2e},
                                         {"fields", fields}});
        }
    }

    Json list = Json::array();

    for (auto &[name, service] : services) {
        if (!service.at("events").empty()) {
            list.push_back(std::move(service));
        }
    }

    need(!list.empty(), "QNX_PASSION_SOMEIP_CONTRACT");

    return {{"schema", 1}, {"services", list}};
}
} // namespace voyemu
