#include "qnx_passion_someip.hpp"
#include "someip_gateway.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
using namespace voyemu;
using namespace std::string_literals;
namespace {
void put(std::string &out, std::size_t at, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        out[at + i] = static_cast<char>(value >> (8 * i));
    }
}

// A synthetic aarch64 ELF with one generated client: start_VehicleDrivingInfo_client called with
// the VLAN name, its client data, two event records and their decoder descriptors. The first event
// is an E2E header structure followed by three basic values; the second one is `other`. Two getters
// pass method ids 1 and 7 to VehicleDrivingInfo_request_send: Get_f_VehicleDrivingInform returns
// the three values, Get_f_Lonely one byte.
std::string elf(std::string_view vlan, unsigned other_kind = 0) {
    std::string out(0x2000, '\0');
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
    put(out, 60, 6, 2);
    put(out, 62, 4, 2);
    put(out, 0x40, 1, 4);
    put(out, 0x40 + 32, 0x1000, 8);
    put(out, 0x40 + 40, 0x1000, 8);
    // .text: start at 0x100 returns; the caller loads the string at 0x280 and calls it.
    put(out, 0x100, 0xd65f03c0, 4);
    put(out, 0x110, 0x90000000, 4);
    put(out, 0x114, 0x91000000 | (0x280 << 10), 4);
    put(out, 0x118, 0x94000000 | ((-6) & 0x3ffffff), 4);
    put(out, 0x11c, 0xd65f03c0, 4);
    put(out, 0x120, 0xd65f03c0, 4);
    put(out, 0x124, 0xd2800001, 4);
    put(out, 0x128, 0x52800020, 4);
    put(out, 0x12c, 0x94000000 | ((-3) & 0x3ffffff), 4);
    put(out, 0x130, 0xd65f03c0, 4);
    put(out, 0x134, 0x528000e0, 4);
    put(out, 0x138, 0x94000000 | ((-6) & 0x3ffffff), 4);
    put(out, 0x13c, 0xd65f03c0, 4);
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
    // Descriptors: kind at 0, type name at 8, size in the high word at 16, members at 32.
    auto record = [&](std::size_t at, unsigned kind, unsigned bytes) {
        put(out, at, kind, 8);
        put(out, at + 16, std::uint64_t(bytes) << 32, 8);
        put(out, at + 24, bytes, 8);
    };
    record(0x800, 3, 33);
    record(0x860, 3, 24);
    record(0x890, 0, 4);
    record(0x8c0, 0, 1);
    record(0x8f0, 0, 4);
    record(0x960, 0, 8);
    record(0x990, 0, 4);
    record(0x9c0, 0, 4);
    record(0x9f0, 0, 4);
    record(0xc00, other_kind, 2);
    record(0xd00, 3, 9);
    record(0xd60, 0, 1);
    const std::size_t relocations[][2] = {
            {0x808, 0xb00}, {0x820, 0x860}, {0x868, 0xb10}, {0x880, 0x960}, {0xd20, 0x890}};

    for (std::size_t i = 0; i < 5; ++i) {
        put(out, 0xa80 + 24 * i, relocations[i][0], 8);
        put(out, 0xa80 + 24 * i + 8, 1027, 8);
        put(out, 0xa80 + 24 * i + 16, relocations[i][1], 8);
    }

    out.replace(0xb00, 8, "Wrapper\0"s);
    out.replace(0xb10, 36, "Basic_strt_E2E_Profile7_MessageType\0"s);
    const struct {
        std::string_view name;
        std::size_t value, size;
    } symbols[] = {{"start_VehicleDrivingInfo_client", 0x100, 4},
                   {"VehicleDrivingInfo_client_data", 0x200, 0x20},
                   {"event_field_notifier_Notify_f_VehicleDrivingInform", 0x240, 64},
                   {"event_field_notifier_Notify_f_Other", 0x2c0, 64},
                   {"field_notifier_Notify_f_VehicleDrivingInform_event_fields", 0x800, 96},
                   {"field_notifier_Notify_f_Other_event_fields", 0xc00, 96},
                   {"VehicleDrivingInfo_request_send", 0x120, 4},
                   {"field_getter_Get_f_VehicleDrivingInform_rpc_call", 0x124, 16},
                   {"field_getter_Get_f_VehicleDrivingInform_returnType_fields", 0xd00, 96},
                   {"field_getter_Get_f_Lonely_rpc_call", 0x134, 12},
                   {"field_getter_Get_f_Lonely_returnType_fields", 0xd60, 96}};
    std::string strings(1, '\0');

    for (std::size_t i = 0; i < std::size(symbols); ++i) {
        const auto at = 0x400 + 24 * (i + 1);
        put(out, at, strings.size(), 4);
        put(out, at + 8, symbols[i].value, 8);
        put(out, at + 16, symbols[i].size, 8);
        strings.append(symbols[i].name).push_back('\0');
    }

    out.replace(0x1000, strings.size(), strings);

    const auto names = "\0.text\0.symtab\0.strtab\0.shstrtab\0.rela.dyn\0"s;
    out.replace(0x5a0, names.size(), names);
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
    section(1, 1, 1, 0x100, 0x40, 0);
    section(2, 7, 2, 0x400, 24 * (std::size(symbols) + 1), 3);
    section(3, 15, 3, 0x1000, strings.size(), 0);
    section(4, 23, 3, 0x5a0, names.size(), 0);
    section(5, 33, 4, 0xa80, 24 * 5, 2);

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

const SomeipEvent &event(const Json &contract, std::string_view name) {
    static SomeipContract parsed;
    parsed = someip_contract(contract);

    for (const auto &item : parsed.services.front().events) {
        if (item.name == name) {
            return item;
        }
    }

    assert(false);

    return parsed.services.front().events.front();
}
} // namespace

int main() try {
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
           service.instance == 1 && service.major == 1 && service.events.size() == 2);
    // The decoder descriptors decide the wire: the 20-byte E2E header, then the values back to
    // back with no structure padding; names come from the table.
    const auto &driving = event(contract, "Notify_f_VehicleDrivingInform");
    assert(driving.id == 0x8001 && driving.eventgroups == std::vector<std::uint16_t>{0x2000} &&
           driving.e2e && driving.bytes == 29 && driving.fields.size() == 7);
    assert(driving.fields[6].name ==
                   "VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.VehicleSpeed" &&
           driving.fields[6].offset == 25 && driving.fields[6].bytes == 4 &&
           driving.fields[5].offset == 24 && driving.fields[3].offset == 16);
    // An event the table lacks is still offered, its values named by position.
    const auto &other = event(contract, "Notify_f_Other");
    assert(other.id == 0x8002 && !other.e2e && other.bytes == 2 &&
           other.fields.front().name == "VehicleDrivingInfo.Notify_f_Other.#0");
    // From the first width the table disagrees on, fields keep their position only.
    auto older = table;
    older["servicedefine"][0]["method_event_define"][0]["Signals"][5]["len"] = 2;
    const auto &renamed = event(qnx_passion_someip_contract(elf("VLAN12"), older),
                                "Notify_f_VehicleDrivingInform");
    assert(renamed.fields[4].name == "VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.Mileage" &&
           renamed.fields[5].name == "VehicleDrivingInfo.Notify_f_VehicleDrivingInform.#5" &&
           renamed.fields[6].name == "VehicleDrivingInfo.Notify_f_VehicleDrivingInform.#6" &&
           renamed.bytes == 29);
    // Getters answer with the event's values without its E2E header, named like the event; a
    // getter without an event names its values by position.
    assert(service.methods.size() == 2);
    const auto &get = service.methods[0].name == "Get_f_VehicleDrivingInform" ? service.methods[0]
                                                                              : service.methods[1];
    const auto &lonely = &get == &service.methods[0] ? service.methods[1] : service.methods[0];
    assert(get.id == 1 && get.bytes == 9 && get.fields.size() == 3 &&
           get.fields[0].name == "VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.Mileage" &&
           get.fields[2].name ==
                   "VehicleDrivingInfo.Notify_f_VehicleDrivingInform.V.VehicleSpeed" &&
           get.fields[2].offset == 5 && get.fields[2].bytes == 4);
    assert(lonely.name == "Get_f_Lonely" && lonely.id == 7 && lonely.bytes == 1 &&
           lonely.fields.front().name == "VehicleDrivingInfo.Get_f_Lonely.#0");
    // Arrays and strings (other descriptor kinds) leave their event out.
    assert(someip_contract(qnx_passion_someip_contract(elf("VLAN12", 8), table))
                   .services.front()
                   .events.size() == 1);
    rejects([&] {
        qnx_passion_someip_contract(elf("ETH12"), table);
    });
    rejects([&] {
        qnx_passion_someip_contract(elf("VLAN12").substr(0, 0x500), table);
    });
    std::cout << "Passion SOME/IP contract from synthetic client binary, descriptors and table\n";

    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
