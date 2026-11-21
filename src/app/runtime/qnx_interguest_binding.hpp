#pragma once
#include "error.hpp"
#include "interguest_link.hpp"
#include "qnx_interguest_contract.hpp"
#include <string_view>
namespace voyemu {
struct QnxInterguestBinding {
    InterguestLink::RamGeometry geometry = InterguestLink::RamGeometry::free8g;
    bool passion = false;

    std::uint64_t ram_bytes() const {
        return passion ? 11713ULL << 20 : 8ULL << 30;
    }
};

// Match launch semantics before opening RAM. Never infer geometry from a file's size. The hardware
// adapter selects the Passion geometry; the profile id selects nothing.
inline QnxInterguestBinding qnx_passion_ram_binding(const Json &runtime, bool production) {
    const auto adapter = runtime.value("hardware_adapter", std::string{});
    constexpr std::string_view passion_adapter = "sa8155-passion-21480-developer",
                               production_adapter = "sa8155-passion-21480";
    const bool requested = adapter == passion_adapter || adapter == production_adapter ||
                           runtime.contains("interguest_ram");

    if (!requested) {
        return {};
    }

    auto need = [](bool valid) {
        if (!valid) {
            throw VoyemuError("INTERGUEST_RAM_CONTRACT",
                              "exact Passion adapter, geometry and launch required");
        }
    };
    need(adapter == (production ? production_adapter : passion_adapter));

    if (production) {
        need(!runtime.contains("interguest_ram"));
    } else {
        need(runtime.contains("interguest_ram"));
        const auto &geometry = runtime.at("interguest_ram");
        need(geometry == qnx_passion_interguest_ram_contract());

        for (const char *key :
             {"schema", "backing_bytes", "mailbox_begin", "mailbox_end_exclusive"}) {
            need(geometry.at(key).is_number_integer());
        }
    }

    need(runtime.value("ram_backend", std::string{}) == "file" &&
         runtime.value("ram", std::string{}) == "${SESSION}/qnx-ram.bin");
    need(runtime.contains("qemu") && runtime.at("qemu").is_object() &&
         runtime.at("qemu").contains("args"));
    const auto &qemu = runtime.at("qemu");
    const auto &args = qemu.at("args");
    need(!qemu.contains("arguments") || qemu.at("arguments") == args);
    need(args.is_array() && args.size() <= 512);

    for (const auto &arg : args) {
        need(arg.is_string());
    }

    // QEMU accepts both --foo and -foo, plus -M for -machine.
    auto canonical = [](std::string_view name) {
        if (name.starts_with("--")) {
            name.remove_prefix(2);
        } else if (name.starts_with("-")) {
            name.remove_prefix(1);
        }

        if (name == "M") {
            name = "machine";
        }

        return name;
    };

    for (const auto &arg : args) {
        const auto name = canonical(arg.get_ref<const std::string &>());
        need(name != "readconfig" && name != "set" && name != "mem-path" && name != "numa");
    }

    auto option = [&](std::string_view name) {
        std::string value;
        unsigned count = 0;

        for (std::size_t i = 0; i < args.size(); ++i) {
            if (canonical(args[i].get_ref<const std::string &>()) == canonical(name)) {
                need(i + 1 < args.size());
                value = args[i + 1].get<std::string>();
                ++count;
            }
        }

        need(count == 1);

        return value;
    };
    need(option("-machine") ==
         "virt-11.0,gic-version=3,virtualization=on,secure=off,memory-backend=qnxmem");
    need(option("-m") == "11713");
    need(option("-object") ==
         "memory-backend-file,id=qnxmem,size=11713M,mem-path=${SESSION}/qnx-ram.bin,share=on");
    need(option("-serial") == "file:${SESSION}/qnx-serial.log");

    return {InterguestLink::RamGeometry::passion, true};
}

inline QnxInterguestBinding qnx_interguest_binding(const Json &runtime) {
    validate_qnx_interguest_contract(runtime.at("interguest_network"));

    return qnx_passion_ram_binding(runtime, runtime.value("hardware_adapter", std::string{}) ==
                                                    "sa8155-passion-21480");
}

inline QnxInterguestBinding qnx_ram_binding(const Json &runtime) {
    if (runtime.value("hardware_adapter", std::string{}) == "sa8155-passion-21480") {
        return qnx_passion_ram_binding(runtime, true);
    }

    if (runtime.contains("interguest_network")) {
        return qnx_interguest_binding(runtime);
    }

    return qnx_passion_ram_binding(runtime, false);
}
} // namespace voyemu
