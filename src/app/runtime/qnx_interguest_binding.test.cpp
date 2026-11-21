#include "qnx_interguest_binding.hpp"
#include "qnx_owned_ram.hpp"
#include "qnx_process_ram.hpp"
#include "qnx_runtime_layout.hpp"
#include <filesystem>
#include <unistd.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <exception>
#include <functional>
#include <iostream>
using namespace voyemu;

static void rejects(const std::function<void()> &f) {
    bool rejected = false;

    try {
        f();
    } catch (...) {
        rejected = true;
    }

    assert(rejected);
}

static Json passion() {
    return {{"profile", "test-passion-profile"},
            {"hardware_adapter", "sa8155-passion-21480-developer"},
            {"interguest_network", qnx_interguest_contract()},
            {"interguest_ram", qnx_passion_interguest_ram_contract()},
            {"ram_backend", "file"},
            {"ram", "${SESSION}/qnx-ram.bin"},
            {"qemu",
             {{"args",
               Json::array(
                       {"-machine",
                        "virt-11.0,gic-version=3,virtualization=on,secure=off,memory-backend=qnxmem",
                        "-m", "11713", "-object",
                        "memory-backend-file,id=qnxmem,size=11713M,mem-path=${SESSION}/qnx-ram.bin,share=on",
                        "-serial", "file:${SESSION}/qnx-serial.log"})}}}};
}

int main() try {
    for (const auto *adapter : {"", "sa8155-qinggan09", "sa8155-qinggan11-731"}) {
        const Json legacy = {{"hardware_adapter", adapter},
                             {"interguest_network", qnx_interguest_contract()}};
        const auto binding = qnx_interguest_binding(legacy);
        assert(!binding.passion && binding.geometry == InterguestLink::RamGeometry::free8g &&
               binding.ram_bytes() == (8ULL << 30));
        auto wrong = legacy;
        wrong["interguest_ram"] = qnx_passion_interguest_ram_contract();
        rejects([&] {
            qnx_interguest_binding(wrong);
        });
    }

    {
        auto alias = passion();
        alias["qemu"]["arguments"] = alias["qemu"]["args"];
        assert(qnx_interguest_binding(alias).passion);
    }
    {
        auto bad = passion();
        bad["qemu"]["arguments"] = bad["qemu"]["args"];
        bad["qemu"]["arguments"][3] = "8192";
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }
    {
        auto bad = passion();
        bad["qemu"]["arguments"] = bad["qemu"]["args"];
        bad["qemu"].erase("args");
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }
    {
        const auto layout = qnx_runtime_layout("sa8155-passion-21480");
        const auto ram_launch = qnx_ram_launch(QnxEngine::hvf, layout.memory_mib);
        std::vector<std::string> args{"-machine", ram_launch.machine,
                                      "-m",       std::to_string(layout.memory_mib),
                                      "-serial",  "file:${SESSION}/qnx-serial.log"};
        args.insert(args.end(), ram_launch.options.begin(), ram_launch.options.end());
        Json launch{{"ram_backend", ram_launch.backend},
                    {"ram", "${SESSION}/qnx-ram.bin"},
                    {"qemu", {{"args", args}}}};
        qnx_runtime_launch_metadata(launch, layout);
        const auto binding = qnx_ram_binding(launch);
        assert(binding.passion && binding.ram_bytes() == (11713ULL << 20));
        assert(qnx_owned_ram_bytes(Json{{"ram_geometry", qnx_owned_ram_geometry(binding)}}) ==
               (11713ULL << 20));
        Json seed;
        qnx_runtime_metadata(seed, layout);
        assert(!seed.contains("profile"));
        const auto free = qnx_runtime_layout("sa8155-qinggan09");
        Json legacy = Json::object();
        qnx_runtime_launch_metadata(legacy, free);
        assert(!legacy.contains("profile") && !qnx_ram_binding(legacy).passion);
    }
    {
        auto production = passion();
        production["hardware_adapter"] = "sa8155-passion-21480";
        production.erase("interguest_network");
        production.erase("interguest_ram");
        const auto ram = qnx_ram_binding(production);
        assert(ram.passion && ram.ram_bytes() == (11713ULL << 20) &&
               qnx_owned_ram_geometry(ram) == "Passion11713MiB");
        assert(qnx_owned_ram_bytes(Json{{"ram_geometry", qnx_owned_ram_geometry(ram)}}) ==
               (11713ULL << 20));

        // Without its adapter the launch selects no Passion geometry, whatever else it carries.
        auto unadapted = production;
        unadapted.erase("hardware_adapter");
        assert(!qnx_ram_binding(unadapted).passion);

        for (const auto *key : {"ram_backend", "ram", "qemu"}) {
            auto bad = production;
            bad.erase(key);
            rejects([&] {
                qnx_ram_binding(bad);
            });
        }

        for (const auto index : {1, 3, 5, 7}) {
            auto bad = production;
            bad["qemu"]["args"][index] = "wrong";
            rejects([&] {
                qnx_ram_binding(bad);
            });
        }

        auto bad = production;
        bad["qemu"]["args"].push_back("--m");
        bad["qemu"]["args"].push_back("8192");
        rejects([&] {
            qnx_ram_binding(bad);
        });
        bad = production;
        bad["interguest_ram"] = qnx_passion_interguest_ram_contract();
        rejects([&] {
            qnx_ram_binding(bad);
        });
        // The production adapter carries the network without the developer RAM contract.
        auto network = production;
        network["interguest_network"] = qnx_interguest_contract();
        assert(qnx_interguest_binding(network).passion &&
               qnx_ram_binding(network).geometry == InterguestLink::RamGeometry::passion);
        bad = network;
        bad["interguest_ram"] = qnx_passion_interguest_ram_contract();
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }
    {
        const auto ram = qnx_ram_binding(Json::object());
        assert(!ram.passion &&
               qnx_owned_ram_bytes(Json{{"ram_geometry", qnx_owned_ram_geometry(ram)}}) ==
                       (8ULL << 30));
    }
    {
        // The pinned gateway contract of a cluster launch; none without the field.
        assert(!qnx_someip_gateway(passion()));
        const auto directory = std::filesystem::current_path() / ".local/build/tmp" /
                               ("someip-gateway-" + std::to_string(::getpid()));
        std::filesystem::create_directories(directory);
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
        const auto file = directory / "someip-gateway.json";
        write_private_json(file, Json{{"schema", 1}, {"services", Json::array()}});
        auto pinned = passion();
        pinned["someip_gateway"] = {{"file", file.string()}, {"sha256", sha256_file(file)}};
        assert(qnx_someip_gateway(pinned)->at("schema") == 1);
        auto changed = pinned;
        changed["someip_gateway"]["sha256"] = std::string(64, '0');
        rejects([&] {
            qnx_someip_gateway(changed);
        });
        auto alone = pinned;
        alone.erase("interguest_network");
        rejects([&] {
            qnx_someip_gateway(alone);
        });
        auto relative = pinned;
        relative["someip_gateway"]["file"] = file.filename().string();
        rejects([&] {
            qnx_someip_gateway(relative);
        });
        std::filesystem::remove_all(directory);
    }
    const auto spec = passion();
    const auto binding = qnx_interguest_binding(spec);
    assert(binding.passion && binding.geometry == InterguestLink::RamGeometry::passion &&
           binding.ram_bytes() == (11713ULL << 20));

    for (const char *key : {"hardware_adapter", "interguest_ram", "ram_backend", "ram", "qemu"}) {
        auto bad = spec;
        bad.erase(key);
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }

    {
        auto bad = spec;
        bad["hardware_adapter"] = "another";
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }
    {
        // The adapter selects the geometry; another or absent profile id changes nothing.
        auto other = spec;
        other["profile"] = "another";
        assert(qnx_interguest_binding(other).passion);
        other.erase("profile");
        assert(qnx_interguest_binding(other).passion);
    }

    for (const auto *key : {"schema", "backing_bytes", "mailbox_begin", "mailbox_end_exclusive"}) {
        auto bad = spec;
        bad["interguest_ram"][key] = bad["interguest_ram"][key].get<double>();
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }

    {
        auto bad = spec;
        bad["interguest_ram"]["extra"] = 1;
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }

    for (const auto index : {1, 3, 5, 7}) {
        auto bad = spec;
        bad["qemu"]["args"][index] = "wrong";
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }

    for (const auto *option : {"-machine", "-m", "-object", "-serial"}) {
        auto bad = spec;
        bad["qemu"]["args"].push_back(option);
        bad["qemu"]["args"].push_back(spec["qemu"]["args"][std::string(option) == "-machine"  ? 1
                                                           : std::string(option) == "-m"      ? 3
                                                           : std::string(option) == "-object" ? 5
                                                                                              : 7]);
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }

    for (const auto *alias : {"-M", "--machine", "--m", "--object", "--serial"}) {
        auto bad = spec;
        bad["qemu"]["args"].push_back(alias);
        bad["qemu"]["args"].push_back("override");
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }

    for (const auto *option : {"-readconfig", "-set", "-mem-path", "-numa"}) {
        auto bad = spec;
        bad["qemu"]["args"].push_back(option);
        bad["qemu"]["args"].push_back("override");
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }

    {
        auto bad = spec;
        bad["ram_backend"] = "process";
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }
    {
        auto bad = spec;
        bad["interguest_network"]["address"] = "172.16.110.30";
        rejects([&] {
            qnx_interguest_binding(bad);
        });
    }
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
