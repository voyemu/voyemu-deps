#include "qnx_passion.hpp"
#include "process.hpp"
#include "qnx_guest_shell.hpp"
#include "qnx_imagefs.hpp"
#include "qnx_lzo.hpp"
#include "qnx_lzo_pack.hpp"
#include "qnx_passion_platform.hpp"
#include "qnx_startup.hpp"
#include <fstream>
#include <iostream>
#include <vector>
using namespace voyemu;
namespace {
void need(bool value) {
    if (!value) {
        throw std::runtime_error("Passion fixture invariant");
    }
}

template <class Function> void rejects(Function action) {
    bool failed = false;

    try {
        action();
    } catch (...) {
        failed = true;
    }

    need(failed);
}

// Synthetic OEM startup functions: one VLAN per line kind, the S32G M-core ARP, unrelated noise.
void network_script() {
    std::string vlans;

    for (const auto *id : {"4", "10", "11", "12", "13", "14"}) {
        vlans += std::string("    ifconfig vlan") + id + " create\n    ifconfig vlan" + id +
                 " vlan " + id + " vlanif emac0\n    ifconfig vlan" + id + " 172.16.1" + id +
                 ".30 netmask 255.255.255.0 up\n\n";
    }

    const std::string oem =
            "ifconfig emac0 up\nstart_config_vlan()\n{\n    log_launch \"config vlan\"\n\n" +
            vlans +
            "}\nstatic_arp_config()\n{\n    log_launch \"arp\"\n    arp -s 172.16.112.10 "
            "02:DF:53:00:00:00\n    arp -s 172.16.112.40 02:DF:53:00:00:04\n}\n"
            "start_config_vlan\n";
    const auto script = qnx_passion_network_script(oem, "172.16.100.22");
    need(script.starts_with(
                 "ifconfig tap0 create up || return 1\nifconfig tap0 172.16.100.22 "
                 "netmask 255.255.255.0 up || return 1\nifconfig vlan4 create || return 1\n") &&
         script.find("ifconfig vlan12 vlan 12 vlanif tap0 || return 1\n") != script.npos &&
         script.find("arp -s 172.16.112.10 02:DF:53:00:00:00 || echo 'QPC NETWORK arp -s "
                     "172.16.112.10 02:DF:53:00:00:00 failed'\n") != script.npos &&
         script.find("emac0") == script.npos && script.find("log_launch") == script.npos &&
         script.find("VOYEMU_QNET_RELAY=1 LD_PRELOAD=$qm_snap ") != script.npos &&
         script.ends_with("echo \"QPC QNET relay_pid=$! vlans=6 arp=2\"\n"));
    auto change = [&](std::string_view before, std::string_view after) {
        auto bad = oem;
        bad.replace(bad.find(before), before.size(), after);
        rejects([&] {
            qnx_passion_network_script(bad, "172.16.100.22");
        });
    };
    change("    ifconfig vlan14 create\n", "");
    change("    ifconfig vlan14 create\n",
           "    ifconfig vlan14 create\n    ifconfig vlan15 create\n");
    change("02:DF:53:00:00:00", "02:DF:53:00:00:01");
    change("    log_launch \"arp\"\n", "    route add default 172.16.110.20\n");
    change("vlan 12 vlanif emac0", "vlan 12 vlanif emac0 up");
    change("static_arp_config()", "static_arp()");
    change("ifconfig emac0 up\n", "start_config_vlan()\n{\n}\n");
}

std::string read(const char *name) {
    std::ifstream input(name, std::ios::binary);

    need(bool(input));

    return {std::istreambuf_iterator<char>(input), {}};
}

// Packs every block anew with every two-byte match widened, so the stream differs from the OEM one.
std::string repack_fixture(std::string_view host) {
    std::string result;

    for (std::size_t at = 0; at < host.size(); at += 65536) {
        const QnxLzoBlock block(host.substr(at, 65536));
        const auto packed = block.emit(block.growable());
        need(packed.size() <= 65535);
        result += char(packed.size() >> 8);
        result += char(packed.size());
        result += packed;
    }

    result.append(2, 0);

    while (result.size() % 4) {
        result += char(0);
    }

    std::uint32_t sum = 0;

    for (std::size_t at = 0; at < result.size(); at += 4) {
        for (unsigned j = 0; j < 4; ++j) {
            sum += std::uint32_t(static_cast<unsigned char>(result[at + j])) << (8 * j);
        }
    }

    for (unsigned j = 0; j < 4; ++j) {
        result += char((0 - sum) >> (8 * j));
    }

    return result;
}

void module_closure_fixture() {
    std::string image(16384, 0);

    image.replace(0, 8, std::string("imagefs\x04", 8));
    auto put = [&](std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) {
            image[at + i] = static_cast<char>(value >> (8 * i));
        }
    };
    put(8, image.size());
    put(16, 92);
    std::size_t cursor = 92;
    const std::array<std::string, 4> names{"bin/Snapdragon.rgb", "lib64/libopenwfd_qnx.so",
                                           "bin/qditest", "bin/ssplash"};

    for (unsigned i = 0; i < names.size(); ++i) {
        const auto bytes = (32 + names[i].size() + 1 + 3) & ~std::size_t(3);
        put(cursor, bytes);
        put(cursor + 8, 0100000);
        put(cursor + 24, 4096 + i * 1024);
        put(cursor + 28, 512);
        image.replace(cursor + 32, names[i].size(), names[i]);
        image.replace(4096 + i * 1024, 512, std::string(512, char('a' + i)));
        cursor += bytes;
    }

    put(12, cursor + 4);
    std::uint32_t sum = 0;

    for (std::size_t i = 0; i < image.size() - 4; i += 4) {
        for (unsigned j = 0; j < 4; ++j) {
            sum += std::uint32_t(static_cast<unsigned char>(image[i + j])) << (8 * j);
        }
    }

    put(image.size() - 4, 0 - sum);
    std::string manager(0x2a700, 'm');
    manager.replace(0x2a5fc, 64, sha256_bytes(image));
    const auto result = update_qnx_passion_display_modules(image, manager, "new front", "new wfd");
    need(result.display.size() == image.size() &&
         read_qnx_imagefs_file(result.display, names[2]) == std::string(512, 'c') &&
         read_qnx_imagefs_file(result.display, names[3]) == std::string(512, 'd'));
    auto stored = std::string("new front");
    stored.resize(512, 0);
    need(read_qnx_imagefs_file(result.display, names[0]) == stored);
    auto inverse = result.manager;
    inverse.replace(0x2a5fc, 64, sha256_bytes(image));
    need(inverse == manager && result.manager.substr(0x2a5fc, 64) == sha256_bytes(result.display));
    rejects([&] {
        update_qnx_passion_display_modules(image, manager, std::string(513, 'x'), "wfd");
    });
    rejects([&] {
        update_qnx_passion_display_modules(image, manager, "front", std::string(513, 'x'));
    });
    rejects([&] {
        update_qnx_passion_display_modules(image, manager, "", "wfd");
    });
    rejects([&] {
        update_qnx_passion_display_modules(image, std::string(manager.size(), 'x'), "front", "wfd");
    });
    auto corrupt = image;
    corrupt[8192] ^= 1;
    rejects([&] {
        update_qnx_passion_display_modules(corrupt, manager, "front", "wfd");
    });
    const auto runtime_pin = sha256_bytes(std::string(512, 'd'));

    for (const auto &runtime : {std::string("short script\n"), std::string(511, 'x') + "\n",
                                std::string(600, 'x') + "\n"}) {
        const auto changed = update_qnx_passion_display_modules(image, manager, "front", "wfd",
                                                                runtime, runtime_pin);
        need(read_qnx_imagefs_file(changed.display, names[3]) == runtime &&
             read_qnx_imagefs_file(changed.display, names[2]) == std::string(512, 'c') &&
             changed.manager.substr(0x2a5fc, 64) == sha256_bytes(changed.display));
    }

    rejects([&] {
        update_qnx_passion_display_modules(image, manager, "front", "wfd", "script\n",
                                           std::string(64, '0'));
    });
    rejects([&] {
        update_qnx_passion_display_modules(image, manager, "front", "wfd", "script", runtime_pin);
    });
    rejects([&] {
        update_qnx_passion_display_modules(image, manager, "front", "wfd", std::string("s\0\n", 3),
                                           runtime_pin);
    });
    rejects([&] {
        update_qnx_passion_display_modules(image, manager, "front", "wfd", "", runtime_pin);
    });
    rejects([&] {
        update_qnx_passion_display_modules(image, manager, "front", "wfd", "script\n", "");
    });
    rejects([&] {
        update_qnx_passion_display_modules(image, manager, "front", "wfd",
                                           std::string(512 + 4096, 'x') + "\n", runtime_pin);
    });
    auto occupied = image;
    occupied[7168 + 512] = 1;

    for (unsigned i = 0; i < 4; ++i) {
        occupied[occupied.size() - 4 + i] = 0;
    }

    sum = 0;

    for (std::size_t i = 0; i < occupied.size(); i += 4) {
        for (unsigned j = 0; j < 4; ++j) {
            sum += std::uint32_t(static_cast<unsigned char>(occupied[i + j])) << (8 * j);
        }
    }

    for (unsigned i = 0; i < 4; ++i) {
        occupied[occupied.size() - 4 + i] = static_cast<char>((0 - sum) >> (8 * i));
    }

    auto occupied_manager = manager;
    occupied_manager.replace(0x2a5fc, 64, sha256_bytes(occupied));
    rejects([&] {
        update_qnx_passion_display_modules(occupied, occupied_manager, "front", "wfd",
                                           std::string(600, 'x') + "\n", runtime_pin);
    });
}
} // namespace

int main(int argc, char **argv) {
    try {
        module_closure_fixture();
        // Numeric adaptation: W4 becomes the existing root index, while X0/X1/W2/X3 survive.
        const std::uint32_t owner_word = 0x2a1f03e4, call_word = 0x97ffebb8;
        need((owner_word & 0xffe0ffe0U) == 0x2a0003e0U && ((owner_word >> 16) & 31) == 31 &&
             (owner_word & 31) == 4);
        auto imm = std::int64_t(call_word & 0x3ffffffU);

        if (imm & (1LL << 25)) {
            imm -= 1LL << 26;
        }

        need((call_word >> 26) == 0x25 && 0x8570b280LL + 4 * imm == 0x85706160LL);
        const std::array<std::uint64_t, 4> arguments{0x50000000, 0xafffffff, 7, 0x8572b090};
        auto preserved = arguments;
        std::uint32_t owner = 0;
        need(owner == 0 && arguments == preserved);
        struct Metadata {
            std::uint64_t start, end;
            unsigned owner;
            const char *name;
        };
        const std::array<Metadata, 3> old_tree{{{0, 0xffffffffffULL, 0xffff, "memory"},
                                                {0, 0xffffffff, 0, "below4G"},
                                                {0x50000000, 0xafffffff, 0x20, "ram"}}};
        auto tree = old_tree;
        tree[2].owner = owner;
        auto exact_parent = [](const auto &nodes) {
            return nodes[2].owner == 0 && nodes[0].owner == 0xffff;
        };
        need(!exact_parent(old_tree) && exact_parent(tree));
        auto child = [&](std::uint64_t start, std::uint64_t end, unsigned parent) {
            need(start <= end && parent == 0x40 && exact_parent(tree) && tree[2].start <= start &&
                 end <= tree[2].end);

            return Metadata{start, end, parent, "SMEM"};
        };
        const auto smem = child(0x86000000, 0x861fffff, 0x40);
        need(smem.start == 0x86000000 && smem.end == 0x861fffff && smem.owner == 0x40);
        rejects([&] {
            child(0x85700000, 0, 0x40);
        });
        rejects([&] {
            child(0x86000000, 0x861fffff, 0xffff);
        });
        rejects([&] {
            child(0x4fffffff, 0x50000000, 0x40);
        });
        rejects([&] {
            child(0xafffffff, 0xb0000000, 0x40);
        });
        rejects([&] {
            child(0xc0000000, 0xc0000fff, 0x40);
        });
        need(tree[0].start == old_tree[0].start && tree[0].end == old_tree[0].end &&
             tree[0].owner == old_tree[0].owner && tree[1].owner == old_tree[1].owner &&
             tree[1].end == old_tree[1].end && tree[2].start == old_tree[2].start &&
             tree[2].end == old_tree[2].end);

        if (argc == 3 && std::string_view(argv[1]) == "--ram-parent-kernel") {
            const auto kernel = read(argv[2]);
            auto word = [&](std::size_t at) {
                need(at + 4 <= kernel.size());
                std::uint32_t v = 0;

                for (unsigned j = 0; j < 4; ++j) {
                    v |= std::uint32_t(static_cast<unsigned char>(kernel[at + j])) << (8 * j);
                }

                return v;
            };
            need(word(0xe06c) == owner_word && word(0xe080) == call_word);
            std::uint32_t sum = 0;

            for (std::size_t at = 0x3000; at < 0x69108; at += 4) {
                sum += word(at);
            }

            need(sum == 0);
            std::cout
                    << "Passion RAM metadata parent opcodes/checksum passed; no policy or guest acceptance\n";

            return 0;
        }

        if (argc == 5 && std::string_view(argv[1]) == "--mount-namespace") {
            const auto host = read(argv[2]), manager = read(argv[3]);
            need(sha256_bytes(host) ==
                         "b3dfc7b6028ff3d06514638f149cb558454be6ddbba76e2152a83d83e03d8266" &&
                 sha256_bytes(manager) ==
                         "c5a32111a8be792f029ddec51413a98858335ff0aa98fdbed0d04f0f6e579c1b");
            const auto script = read_qnx_imagefs_file(host, "proc/boot/.script"),
                       output = adapt_qnx_passion_boot_script(script);
            using namespace std::string_view_literals;
            const auto original_args = "/bin/startupmgr\0-i\0ifs2\0-m\0/\0"sv,
                       bad_args = "/bin/startupmgr\0-i\0ifs2\0-v\0v\0"sv;
            const auto at = script.find(original_args);
            need(at != std::string::npos &&
                 script.find(original_args, at + 1) == std::string::npos);
            need(output.substr(at, original_args.size()) == original_args &&
                 output.find(bad_args) == std::string::npos);
            // Exact original parser: default /proc/boot, getopt 'm' stores optarg in same field.
            need(manager.substr(0x2b8e2, 11) == "/proc/boot\0"sv &&
                 manager.substr(0x2bf24, 18) == "ci:m:f:k:vt:z:hdE\0"sv);
            need(sha256_bytes(std::string_view(manager).substr(0xe7e0, 0xc)) ==
                         "4ba6f32b2e53acb63976abf2cc05d3040705158afa82163e67573a1b417aef6b" &&
                 sha256_bytes(std::string_view(manager).substr(0xe854, 0x18)) ==
                         "540ed6d7ff754d1f711cae1aa3cd0c14aa108d1d661189e4bf00a63d51445483");
            std::ifstream in(argv[4], std::ios::binary);
            in.seekg(8192);
            std::string core(20543912, '\0');
            in.read(core.data(), core.size());
            need(bool(in) &&
                 sha256_bytes(core) ==
                         "d6023a6679d687fb1a0bcef24f644b9a37c03791844edd4c246f6f797094f08f");
            need(read_qnx_imagefs_file(core, "proc/boot/ifs2.boot").size() == 18 &&
                 read_qnx_imagefs_file(core, "lib64/libclock_client.so").size() == 16934);
            auto visible = [](std::string_view mount, std::string_view name) {
                return std::string(mount) + (mount == "/" ? "" : "/") + std::string(name);
            };
            need(visible("/proc/boot", "proc/boot/ifs2.boot") != "/proc/boot/ifs2.boot" &&
                 visible("/", "proc/boot/ifs2.boot") == "/proc/boot/ifs2.boot");
            need(visible("/proc/boot", "lib64/libclock_client.so") != "/lib64/libclock_client.so" &&
                 visible("/", "lib64/libclock_client.so") == "/lib64/libclock_client.so");

            for (std::size_t i = 0; i < script.size(); ++i) {
                need((script[i] == 0) == (output[i] == 0));
            }

            rejects([&] {
                auto bad = script;
                bad[at + original_args.size() - 2] = 'x';
                adapt_qnx_passion_boot_script(bad);
            });
            rejects([&] {
                adapt_qnx_passion_boot_script(output);
            });
            std::cout
                    << "OLD own default /proc/boot namespace RED; preserved OEM -m / makes actual marker/library paths GREEN; policy/payload unchanged; guest mapping separate\n";

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--environment") {
            const auto host = read(argv[2]);
            need(sha256_bytes(host) ==
                 "b3dfc7b6028ff3d06514638f149cb558454be6ddbba76e2152a83d83e03d8266");
            const auto script = read_qnx_imagefs_file(host, "proc/boot/.script"),
                       procnto = read_qnx_imagefs_file(host, "proc/boot/procnto-smp-instr");
            const auto output = adapt_qnx_passion_boot_script(script),
                       process = adapt_qnx_passion_procnto_environment(procnto);
            const std::string
                    library =
                            "LD_LIBRARY_PATH=/lib64/:/usr/lib64:/lib64/dll:/lib/:/usr/lib:/lib64/dll/pci",
                    process_library =
                            "LD_LIBRARY_PATH=/lib64:/lib64/dll:/usr/lib:/usr/lib64:/lib::",
                    preload = "LD_PRELOAD=secpol-preload.so";
            auto count = [](std::string_view bytes, const std::string &field) {
                const auto token = field + char(0);
                unsigned found = 0;

                for (std::size_t at = 0; (at = bytes.find(token, at)) != std::string_view::npos;
                     at += token.size()) {
                    ++found;
                }

                return found;
            };
            need(count(script, library) == 12 && count(output, library) == 12 &&
                 count(procnto, process_library) == 1 && count(process, process_library) == 1);
            need(count(script, preload) == 10 && count(output, preload) == 10 &&
                 output.size() == script.size() && process.size() == procnto.size());
            const auto policy = read_qnx_imagefs_file(host, "lib64/dll/secpol-preload.so"),
                       libc = read_qnx_imagefs_file(host, "lib64/libc.so.4");
            need(sha256_bytes(policy) ==
                         "5677d37af916df629bf1d490547b5c3e01f26521c6e192330268dff81e4b5040" &&
                 sha256_bytes(libc) ==
                         "2d08b4a414aad277ccb5bf74001b3a4b2fc4090576460feca2f6a927ffdb1eb8");
            // The primary IFS is mounted at root; these are its actual stored target names.
            auto lookup = [&](std::string_view environment, std::string_view name) {
                unsigned found = 0;

                for (std::size_t at = environment.find('=') + 1; at < environment.size();) {
                    auto end = environment.find(':', at);

                    if (end == std::string_view::npos) {
                        end = environment.size();
                    }

                    auto path = std::string(environment.substr(at, end - at));

                    if (!path.empty()) {
                        if (path.back() != '/') {
                            path += '/';
                        }

                        path += name;
                        try {
                            (void)read_qnx_imagefs_file(host, std::string_view(path).substr(1));
                            ++found;
                        } catch (const std::exception
                                         &) { /* Ignored: absent entries are not counted. */
                        }
                    }

                    at = end + 1;
                }

                return found;
            };
            need(lookup(library, "secpol-preload.so") == 1 && lookup(library, "libc.so.4") == 1 &&
                 lookup("LD_LIBRARY_PATH=/proc/boot/lib64/:/proc/boot/lib64/dll/:/usr/lib64/:/lib64/",
                        "secpol-preload.so") == 0);
            rejects([&] {
                adapt_qnx_passion_boot_script(output);
            });
            rejects([&] {
                adapt_qnx_passion_procnto_environment(process);
            });
            auto bad = script;
            bad[bad.find(library)] ^= 1;
            rejects([&] {
                adapt_qnx_passion_boot_script(bad);
            });
            std::cout
                    << "Passion primary root library lookup and all policy preload fields preserved; guest verification separate\n";

            return 0;
        }

        const auto source_path =
                std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                "guest/qnx/qnx-passion-runtime.sh";
        const auto source = read(source_path.string().c_str()),
                   compiled = prepare_qnx_passion_runtime(source);
        need(compiled.starts_with("#!/proc/boot/bin/ksh\n") &&
             compiled == prepare_qnx_guest_shell(source) && compiled.size() < source.size());
        need(compiled.find("QGFX_PASSION_WHEEL_RELAY=1") != compiled.npos &&
             compiled.find("steer_key-bit9") == compiled.npos);
        // The source is not pinned: comments may change, stripping is idempotent, and the size is
        // left to the ImageFS writer, which knows the OEM slot.
        need(prepare_qnx_passion_runtime(source + "# changed\n") == compiled &&
             prepare_qnx_passion_runtime(compiled) == compiled);
        const auto long_line = "true " + std::string(32768, 'x') + "\n";
        need(prepare_qnx_passion_runtime(source + long_line) == compiled + long_line);
        rejects([&] {
            prepare_qnx_passion_runtime(source + "cat <<EOF\n# data\nEOF\n");
        });
        rejects([&] {
            prepare_qnx_passion_runtime(source.substr(1));
        });
        rejects([&] {
            prepare_qnx_passion_runtime(source + "true");
        });
        const auto source_required = qnx_passion_source_disk_required(2684354560ULL);
        need(source_required == 2 * 2684404736ULL + (224ULL << 20) + (8ULL << 30));
        qnx_passion_require_source_disk(source_required, 2684354560ULL);
        qnx_passion_require_source_disk(source_required + 1, 2684354560ULL);
        rejects([&] {
            qnx_passion_require_source_disk(source_required - 1, 2684354560ULL);
        });

        for (const auto invalid : {0ULL, 2684354559ULL, 2684354561ULL, ~0ULL}) {
            rejects([&] {
                qnx_passion_source_disk_required(invalid);
            });
        }

        const auto passion_placement = qnx_passion_11713_placement();
        need(passion_placement.system == 0x27c000000ULL && passion_placement.ifs_b == 0xe5000000 &&
             passion_placement.gpt_cache == 0xe4fb0000 &&
             passion_placement.gpt_prefix == 0xe4fc0000 &&
             passion_placement.gpt_backup == 0xf4fbe000 &&
             passion_placement.ipl_map_end == 0x31c100000ULL);
        const auto root_runtime = prepare_qnx_passion_11713_runtime(source);
        need(root_runtime.starts_with("#!/bin/ksh\n") &&
             root_runtime.find("/proc/boot/bin/") == root_runtime.npos &&
             root_runtime.find("/proc/boot/lib64") == root_runtime.npos);
        // hd0 uses the 32-bit-expressible carrier token 0x7c008000 (the ram64 binding maps it to
        // physical 0x27c008000); devb-ram address= truncates anything wider.
        need(root_runtime.find(
                     "VOYEMU_PASSION_RAM64=passion-20260403-2.1.4.80-system-ram64-v1 \\") !=
                     root_runtime.npos &&
             root_runtime.find("capacity=2462832,address=0x7c008000") != root_runtime.npos);
        // The read-only OEM root mounts first; writable /var is a guest-formatted sysram
        // LUN mounted after it, never a second automount of the OEM image.
        const auto oem_root = root_runtime.find("automount=hd0:/:qnx6:ro"),
                   var_disk = root_runtime.find("devdir=/dev/qvx,memory=sysram"),
                   var_mount = root_runtime.find(R"(qnx6 -o sync=optional "$qm_vx" /var)");
        need(oem_root != root_runtime.npos && var_disk != root_runtime.npos &&
             var_mount != root_runtime.npos && oem_root < var_disk && var_disk < var_mount &&
             root_runtime.find("automount=hd1:") == root_runtime.npos);
        const std::string runtime =
                "\nqm_source_capture=1\ncapacity=2462832,address=0x7c008000 hd2:capacity=2462832,address=0x27c008000 /slm/ic_qt/ic_qt.xml /usr/bin/ic_service\n. ${qm_s}n/display_debug_tools.sh";
        validate_qnx_passion_runtime(runtime);
        rejects([&] {
            validate_qnx_passion_runtime(runtime.substr(0, runtime.rfind('\n')));
        });
        network_script();
        /* The capacity is deliberately not part of the shared contract (the OEM script and this
         * runtime size different devices), so the rejected variants change what the contract does
         * pin. */
        validate_qnx_passion_runtime(runtime + std::string(32768, '\n'));
        rejects([&] {
            validate_qnx_passion_runtime(runtime.substr(2));
        });
        rejects([&] {
            validate_qnx_passion_runtime(runtime + "\nqm_source_capture=1\n");
        });
        rejects([&] {
            auto bad = runtime;
            bad.replace(bad.find(",address=0x27c008000"), 20, ",address=0x27c008001");
            validate_qnx_passion_runtime(bad);
        }); /* The runtime now carries two devb-ram tokens (hd0 and hd2), so a reject must strip
               capacity= from both or the contract's find() still passes. */
        rejects([&] {
            auto bad = runtime;

            for (std::size_t at = bad.find("capacity="); at != std::string::npos;
                 at = bad.find("capacity=")) {
                bad.replace(at, 9, "capacit_=");
            }

            validate_qnx_passion_runtime(bad);
        });
        const auto placement = qnx_passion_placement();
        need(placement.system_sectors == 5242978 &&
             placement.ipl_map_end >= placement.system + placement.system_sectors * 512 &&
             !placement.original_android_vm_allowed &&
             placement.ifs_b + placement.ifs_reserved <= placement.system);
        rejects([] {
            adapt_qnx_passion_startupmgr_gpt({});
        });
        rejects([] {
            adapt_qnx_passion_startupmgr_dispatch({}, {});
        });
        rejects([] {
            prepare_qnx_passion_startup({});
        });
        rejects([] {
            decode_qnx_passion_host({});
        });
        rejects([] {
            pack_qnx_passion_kernel({}, {}, {});
        });

        if (argc == 4 && std::string_view(argv[1]) == "--dispatch") {
            const auto original = read(argv[2]), reader = read(argv[3]),
                       output = adapt_qnx_passion_startupmgr_dispatch(original, reader);
            need(output.size() == original.size());
            auto word = [](std::string_view bytes, std::size_t at) {
                std::uint32_t n = 0;

                for (unsigned j = 0; j < 4; ++j) {
                    n |= std::uint32_t(static_cast<unsigned char>(bytes[at + j])) << (8 * j);
                }

                return n;
            };
            // Static interpretation of the pinned own reader's mmap argument construction.
            // This verifies instruction/data dependencies, not guest execution or mmap ABI.
            need(word(reader, 0) == 0x2a0203e2 && word(reader, 4) == 0x2a0303e3 &&
                 word(reader, 0x10) == 0xab030049 && word(reader, 0x18) == 0xf140413f);
            need(word(reader, 0x34) == 0xd374cc75 && word(reader, 0x38) == 0xd374cc49 &&
                 word(reader, 0x3c) == 0xd2b5ff85 && word(reader, 0x40) == 0x8b0900a5);
            need(word(reader, 0x44) == 0xd2800000 && word(reader, 0x48) == 0xaa1503e1 &&
                 word(reader, 0x4c) == 0x52816002 && word(reader, 0x50) == 0x320083e3 &&
                 word(reader, 0x54) == 0x12800004);
            auto admitted = [](std::uint32_t lba, std::uint32_t blocks) {
                return blocks != 0 && (lba & 0x80000000U) == 0 &&
                       std::uint64_t(lba) + blocks <= 65536;
            };
            need(admitted(0, 1) && admitted(64, 1) && admitted(65535, 1) && !admitted(0, 0) &&
                 !admitted(65536, 1) && !admitted(0xffffffffU, 1) && !admitted(1, 0xffffffffU));
            need(word(reader, 0xa8) == 0x12800000 && word(reader, 0xac) == 0xd65f03c0);
            const auto combined = adapt_qnx_passion_startupmgr(original, reader);
            need(combined != output &&
                 combined.substr(0x270d0, 0x68) == original.substr(0x270d0, 0x68));
            rejects([&] {
                adapt_qnx_passion_startupmgr(combined, reader);
            });
            const auto entry = adapt_qnx_passion_startupmgr_entry(original, reader);
            need(entry.size() == original.size() &&
                 entry.substr(0x30e78) == original.substr(0x30e78));
            need(entry.substr(0x8eb8, 24) == original.substr(0x8eb8, 24) &&
                 entry.substr(0xa8e0, 24) == original.substr(0xa8e0, 24));
            need(entry.find("/bin/ssplash", 0x30bcc) == 0x30bcc);
            need(entry.substr(0x30bcc, 23) ==
                         std::string("/bin/ssplash\0", 13) + std::string(10, 0) &&
                 entry.substr(0x30be3, 29) ==
                         std::string("/etc/graphics.conf\0", 19) + std::string(10, 0));
            rejects([&] {
                adapt_qnx_passion_startupmgr_entry(entry, reader);
            });
            const auto bl = word(output, 0x30b1c + 0x58);
            const auto delta = std::int32_t(bl << 6) >> 4;
            need(std::int64_t(0x30b1c + 0x58) + delta == 0xc2a0);
            need(output.substr(0x14878, 0x20) == original.substr(0x14878, 0x20));
            need(output.substr(0x14ba0, 0x8c) == original.substr(0x14ba0, 0x8c));

            for (std::size_t at = 0; at < original.size(); ++at) {
                if (output[at] != original[at]) {
                    bool allowed = (at >= 0xd0 && at < 0xe0) || (at >= 0x30b1c && at < 0x30bcc) ||
                                   (at >= 0x14898 && at < 0x1489c) ||
                                   (at >= 0x1dac4 && at < 0x1dacc);

                    for (std::size_t r : {0x9908, 0x9920, 0x99e0, 0x99f8, 0x9a28, 0x9a70, 0x9ad0,
                                          0x9ae8, 0x9b60, 0x9b78, 0x9bc0, 0x9c08, 0x9c20}) {
                        allowed |= at >= r + 16 && at < r + 24;
                    }

                    need(allowed);
                }
            }

            rejects([&] {
                adapt_qnx_passion_startupmgr_dispatch(output, reader);
            });
            auto bad = original;
            bad[0x30b1c] = 1;
            rejects([&] {
                adapt_qnx_passion_startupmgr_dispatch(bad, reader);
            });
            std::cout
                    << "Passion RX padding extension/own reader mmap relocation/preserved functions/changed-range/reapply guards passed; derived="
                    << sha256_bytes(output) << " combined=" << sha256_bytes(combined) << '\n';

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--startupmgr") {
            const auto original = read(argv[2]),
                       output = adapt_qnx_passion_startupmgr_gpt(original);
            need(output.size() == original.size() && output != original &&
                 output.substr(0x270d0, 0x68) == original.substr(0x270d0, 0x68));

            for (std::size_t at = 0; at < original.size(); ++at) {
                if (output[at] != original[at]) {
                    need((at >= 0x26fe8 && at < 0x26ff0) || (at >= 0x27018 && at < 0x2701c) ||
                         (at >= 0x2703c && at < 0x27040) || (at >= 0x27138 && at < 0x27154));
                }
            }

            rejects([&] {
                adapt_qnx_passion_startupmgr_gpt(output);
            });
            auto wrong = original;
            wrong[0x2703c] ^= 1;
            rejects([&] {
                adapt_qnx_passion_startupmgr_gpt(wrong);
            });
            std::cout
                    << "Passion actual startupmgr GPT narrow changed-range/source/reapply guards passed; derived="
                    << sha256_bytes(output) << '\n';

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--source") {
            const auto original = read(argv[2]);
            validate_qnx_passion_ram_parent_source(original);

            for (const auto at :
                 {0xe06cU, 0xe080U, 0x67a68U, 0x2db78U, 0x2de90U, 0x2f448U, 0x4ed0U}) {
                auto bad = original;
                bad[at] ^= 1;
                bool rejected = false;
                try {
                    validate_qnx_passion_ram_parent_source(bad);
                } catch (const std::exception &e) {
                    rejected = std::string_view(e.what()).starts_with("QNX_PASSION_RAM_PARENT_");
                }

                need(rejected);
            }

            rejects([&] {
                validate_qnx_passion_ram_parent_source(original.substr(0, 0x69108));
            });
            auto reapplied = original;
            reapplied[0xe06c] ^= 1;
            rejects([&] {
                validate_qnx_passion_ram_parent_source(reapplied);
            });
            rejects([&] {
                prepare_qnx_qinggan11_startup(QnxGuestAdapter::sa8155_qinggan11_731, original,
                                              sha256_bytes(original));
            });
            const auto host = decode_qnx_passion_host(original), hash = sha256_bytes(host),
                       reference = original.substr(0x69108);
            need(host.size() == 4454104);
            need(pack_qnx_passion_kernel(original, reference, hash) == original);
            const auto script = read_qnx_imagefs_file(host, "proc/boot/.script"),
                       procnto = read_qnx_imagefs_file(host, "proc/boot/procnto-smp-instr"),
                       new_script = adapt_qnx_passion_boot_script(script),
                       new_procnto = adapt_qnx_passion_procnto_environment(procnto);
            need(new_script.size() == script.size() && new_procnto.size() == procnto.size() &&
                 new_script != script && new_procnto != procnto);

            for (std::size_t at = 0; at < script.size(); ++at) {
                need((script[at] == 0) == (new_script[at] == 0));
            }

            for (std::size_t at = 0; at < procnto.size(); ++at) {
                need((procnto[at] == 0) == (new_procnto[at] == 0));
            }

            rejects([&] {
                adapt_qnx_passion_boot_script(new_script);
            });
            rejects([&] {
                adapt_qnx_passion_procnto_environment(new_procnto);
            });
            need(encode_qnx_passion_host(original, host) == reference);
            auto changed_host = host;
            changed_host[changed_host.size() - 8] ^= 1;
            const auto encoded = encode_qnx_passion_host(original, changed_host);
            need(encoded != reference &&
                 decode_qnx_declared_boot_stream(encoded, 68, 4454104) == changed_host);
            rejects([&] {
                encode_qnx_passion_host(original, std::string_view(host).substr(1));
            });
            const auto startup = prepare_qnx_passion_startup(original);
            need(startup.size() == 0x66108);
            need(startup.substr(0xe06c - 0x3000, 4) == std::string("\xe4\x03\x1f\x2a", 4) &&
                 startup.substr(0xe080 - 0x3000, 4) == std::string("\xb8\xeb\xff\x97", 4));
            need(startup.substr(0x4f34 - 0x3000, 8) ==
                         std::string("\xd6\x01\0\xb4\xd4\xea\x2d\x58", 8) &&
                 startup.substr(0x4f3c - 0x3000, 0x34) == original.substr(0x4f3c, 0x34));
            need(startup.substr(0x45000 - 0x3000, 8) ==
                         std::string("\x01\x04\0\x08\0\0\x60\0", 8) &&
                 startup.substr(0x44ff8 - 0x3000, 8) == original.substr(0x44ff8, 8) &&
                 startup.substr(0x45008 - 0x3000, 8) == original.substr(0x45008, 8));
            need(startup.substr(0x60938 - 0x3000, 8) == std::string("\x80\0\0\x8a\0\0\0\0", 8) &&
                 startup.substr(0x60948 - 0x3000, 8) == std::string("\xc0\0\0\x8a\0\0\0\0", 8) &&
                 startup.substr(0x60940 - 0x3000, 8) == original.substr(0x60940, 8) &&
                 startup.substr(0x60950 - 0x3000, 8) == original.substr(0x60950, 8));
            need(startup.substr(0x44080 - 0x3000, 8) == std::string("\x1d\x04\0\x8a\0\0\0\0", 8) &&
                 startup.substr(0x44a00 - 0x3000, 8) == std::string("\x1d\x04\0\xb0\0\0\0\0", 8) &&
                 startup.substr(0x44600 - 0x3000, 8) == original.substr(0x44600, 8));
            need(startup.substr(0x13178 - 0x3000, 4) == std::string("\x94\x02\0\xb4", 4));
            need(startup.substr(0x63e08 - 0x3000, 8) == std::string("\x1d\x04\0\x40\0\0\0\0", 8));
            need(startup.substr(0x63e00 - 0x3000, 8) == original.substr(0x63e00, 8) &&
                 startup.substr(0x63e10 - 0x3000, 4080) == original.substr(0x63e10, 4080));
            std::uint32_t checksum = 0;

            for (std::size_t at = 0; at < startup.size(); at += 4) {
                for (unsigned j = 0; j < 4; ++j) {
                    checksum += std::uint32_t(static_cast<unsigned char>(startup[at + j]))
                                << (8 * j);
                }
            }

            need(checksum == 0);
            need(startup.substr(210424 - 0x3000, 4) == std::string("\x10\x04\0\0", 4));
            need(startup.substr(209436 - 0x3000, 16) == original.substr(209436, 16));
            const auto alternate = repack_fixture(host),
                       grown = pack_qnx_passion_kernel(original, alternate, hash);
            const auto hardware = assemble_qnx_passion_kernel(original, alternate, hash);
            need(hardware.substr(0x69108) == alternate && hardware != grown);
            need(hardware.substr(0xd0, 16) == grown.substr(0xd0, 16));
            need(alternate != reference && grown.substr(0x69108) == alternate);
            need(grown.size() != original.size());
            rejects([&] {
                decode_qnx_passion_host(grown);
            });
            auto bad = original;
            bad[0xd0] ^= 1;
            rejects([&] {
                decode_qnx_passion_host(bad);
            });
            bad = reference;
            bad.back() ^= 1;
            rejects([&] {
                pack_qnx_passion_kernel(original, bad, hash);
            });
            rejects([&] {
                pack_qnx_passion_kernel(original, reference, std::string(64, '0'));
            });
            std::cout
                    << "Passion source-pinned 68-frame decode and trimmed ELF identity/growth/negative guards passed; guest untested; derived="
                    << sha256_bytes(grown) << " bytes=" << grown.size() << '\n';

            return 0;
        }

        std::cout << "Passion isolated layout/source guards passed\n";

        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
