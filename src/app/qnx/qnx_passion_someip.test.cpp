#include "qnx_passion_someip.hpp"
#include "someip_gateway.hpp"
#include <cassert>
#include <iostream>
#include <string>
using namespace voyemu;
using namespace std::string_literals;
namespace {
void put(std::string &out, std::size_t at, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        out[at + i] = static_cast<char>(value >> (8 * i));
    }
}

// A synthetic aarch64 ELF with one generated client: start_VehicleDrivingInfo_client called with
// the VLAN name, its client data and two event records, one of them absent from the table.
std::string elf(std::string_view vlan) {
    std::string out(0x800, '\0');
    out.replace(0, 4,
                "\x7f"
                "ELF");
    out[4] = 2;
    out[5] = 1;
    out[6] = 1;
    put(out, 16, 3, 2);
    put(out, 18, 183, 2);
    put(out, 20, 1, 4);
    put(out, 32, 0x40, 8);
    put(out, 40, 0x600, 8);
    put(out, 52, 64, 2);
    put(out, 54, 56, 2);
    put(out, 56, 1, 2);
    put(out, 58, 64, 2);
    put(out, 60, 5, 2);
    put(out, 62, 4, 2);
    put(out, 0x40, 1, 4);
    put(out, 0x40 + 32, 0x800, 8);
    put(out, 0x40 + 40, 0x800, 8);
    // .text: start at 0x100 returns; the caller loads the string at 0x280 and calls it.
    put(out, 0x100, 0xd65f03c0, 4);
    put(out, 0x110, 0x90000000, 4);
    put(out, 0x114, 0x91000000 | (0x280 << 10), 4);
    put(out, 0x118, 0x94000000 | ((-6) & 0x3ffffff), 4);
    put(out, 0x11c, 0xd65f03c0, 4);
    put(out, 0x200, 0x2000, 2);
    for (const std::size_t record : {0x240, 0x2c0}) {
        put(out, record, 1, 4);
        put(out, record + 4, 0x2000, 2);
        put(out, record + 0x28, 0x2000, 2);
        put(out, record + 0x2a, 1, 2);
        put(out, record + 0x2c, 1, 2);
        put(out, record + 0x34, record == 0x240 ? 0x8001 : 0x8002, 2);
    }
    out.replace(0x280, vlan.size(), vlan);
    const auto strings = "\0start_VehicleDrivingInfo_client\0VehicleDrivingInfo_client_data\0"
                         "event_field_notifier_Notify_f_VehicleDrivingInform\0"
                         "event_field_notifier_Notify_f_Other\0"s;
    out.replace(0x300, strings.size(), strings);
    const struct {
        std::size_t name, value, size;
    } symbols[] = {{1, 0x100, 4}, {33, 0x200, 0x20}, {64, 0x240, 64}, {115, 0x2c0, 64}};

    for (std::size_t i = 0; i < 4; ++i) {
        const auto at = 0x400 + 24 * (i + 1);
        put(out, at, symbols[i].name, 4);
        put(out, at + 8, symbols[i].value, 8);
        put(out, at + 16, symbols[i].size, 8);
    }

    const auto names = "\0.text\0.symtab\0.strtab\0.shstrtab\0"s;
    out.replace(0x500, names.size(), names);
    auto section = [&](unsigned index, unsigned name, unsigned type, std::size_t offset,
                       std::size_t size, unsigned link) {
        const auto at = 0x600 + 64 * index;
        put(out, at, name, 4);
        put(out, at + 4, type, 4);
        put(out, at + 16, offset, 8);
        put(out, at + 24, offset, 8);
        put(out, at + 32, size, 8);
        put(out, at + 40, link, 4);
    };
    section(1, 1, 1, 0x100, 0x20, 0);
    section(2, 7, 2, 0x400, 24 * 5, 3);
    section(3, 15, 3, 0x300, strings.size(), 0);
    section(4, 23, 3, 0x500, names.size(), 0);

    return out;
}

template <class Function> void rejects(Function action) {
    bool failed = false;

    try {
        action();
    } catch (const std::exception &) {
        failed = true;
    }

    assert(failed);
}
} // namespace

int main() {
    const auto table = Json::parse(R"({"servicedefine":[{"ServiceName":"VehicleDrivingInfo",
        "method_event_define":[{"Method_Event_Name":"Notify_f_VehicleDrivingInform","Signals":[
          {"name":"8155QNX.VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.E2E_Header.E2E_CRC","len":8},
          {"name":"8155QNX.VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.E2E_Header.E2E_Length","len":4},
          {"name":"8155QNX.VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.E2E_Header.E2E_Counter","len":4},
          {"name":"8155QNX.VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.E2E_Header.E2E_DataID","len":4},
          {"name":"8155QNX.VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.Mileage","len":4},
          {"name":"8155QNX.VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.Valid","len":1},
          {"name":"8155QNX.VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.VehicleSpeed","len":4}]}]}]})");
    const auto contract = qnx_passion_someip_contract(elf("VLAN12"), table);
    const auto parsed = someip_contract(contract);
    assert(parsed.services.size() == 1);
    const auto &service = parsed.services.front();
    assert(service.name == "VehicleDrivingInfo" && service.vlan == 12 && service.id == 0x2000 &&
           service.instance == 1 && service.major == 1 && service.events.size() == 1);
    const auto &event = service.events.front();
    // Packed big-endian: the 20-byte E2E header, then the signals back to back.
    assert(event.id == 0x8001 && event.eventgroups == std::vector<std::uint16_t>{0x2000} &&
           event.e2e && event.bytes == 29 && event.fields.size() == 7);
    assert(event.fields[6].name ==
                   "VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.VehicleSpeed" &&
           event.fields[6].offset == 25 && event.fields[6].bytes == 4 &&
           event.fields[5].offset == 24);
    rejects([&] {
        qnx_passion_someip_contract(elf("ETH12\0"), table);
    });
    rejects([&] {
        qnx_passion_someip_contract(elf("VLAN12").substr(0, 0x500), table);
    });
    auto plain = table;
    plain["servicedefine"][0]["method_event_define"][0]["Signals"].erase(0);
    assert(!someip_contract(qnx_passion_someip_contract(elf("VLAN12"), plain))
                    .services.front()
                    .events.front()
                    .e2e);
    std::cout << "Passion SOME/IP contract from synthetic client binary and service table\n";

    return 0;
}
