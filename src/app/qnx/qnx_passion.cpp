#include "qnx_passion.hpp"
#include "check.hpp"
#include "error.hpp"
#include "little_endian.hpp"
#include "process.hpp"
#include "qnx_guest_shell.hpp"
#include "qnx_imagefs.hpp"
#include "qnx_lzo.hpp"
#include "qnx_startup.hpp"
#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <vector>
namespace voyemu {
namespace {
constexpr std::size_t startup = 0x3000, startup_bytes = 0x66108, stream = 0x69108;
constexpr std::string_view original_sha =
        "1e7c487aae4f4353621f8d0301b352cc974e9037d114ff3ec2556d048512ad28";

constexpr CodeCheck need{"Passion QNX input differs"};

std::uint64_t field(std::string_view bytes, std::size_t at, unsigned width) {
    return le_read(bytes, at, width, need, "QNX_PASSION_FIELD");
}

void put(std::string &bytes, std::size_t at, std::uint64_t value, unsigned width) {
    le_write(bytes, at, value, width, need, "QNX_PASSION_FIELD");
}

void original_layout(std::string_view bytes) {
    need(bytes.size() == 2254664 && sha256_bytes(bytes) == original_sha, "QNX_PASSION_SOURCE_SHA");
    need(bytes.substr(0, 6) == std::string_view("\x7f"
                                                "ELF\2\1",
                                                6) &&
                 field(bytes, 18, 2) == 183 && field(bytes, 24, 8) == 0x85701000 &&
                 field(bytes, 32, 8) == 64 && field(bytes, 54, 2) == 56 && field(bytes, 56, 2) == 3,
         "QNX_PASSION_ELF_LAYOUT");
    need(field(bytes, 0xb0, 4) == 1 && field(bytes, 0xb4, 4) == 7 &&
                 field(bytes, 0xb8, 8) == startup && field(bytes, 0xc0, 8) == 0x85700200 &&
                 field(bytes, 0xc8, 8) == 0x85700200 &&
                 field(bytes, 0xd0, 8) == bytes.size() - startup &&
                 field(bytes, 0xd8, 8) == bytes.size() - startup,
         "QNX_PASSION_LOAD_SEGMENT");
    need(field(bytes, startup, 4) == 0x00ff7eeb && field(bytes, 0x3020, 4) == startup_bytes &&
                 field(bytes, 0x3024, 4) + startup == bytes.size() &&
                 field(bytes, 0x302c, 4) == 4454104,
         "QNX_PASSION_STARTUP_LAYOUT");
    std::uint32_t sum = 0;

    for (std::size_t at = startup; at < stream; at += 4) {
        sum += static_cast<std::uint32_t>(field(bytes, at, 4));
    }

    need(sum == 0, "QNX_PASSION_STARTUP_CHECKSUM");
}
} // namespace
namespace {
using namespace std::string_view_literals;
struct EnvironmentField {
    std::string_view before, after;
    unsigned count;
};

std::string environment_fields(std::string_view original,
                               std::initializer_list<EnvironmentField> fields) {
    // All fields retain their original width, terminators and binary-record geometry.
    std::vector<std::pair<std::size_t, std::string>> changes;

    for (const auto &field : fields) {
        need(field.after.size() <= field.before.size() && field.before.back() == 0 &&
                     field.after.back() == 0,
             "QNX_PASSION_ENVIRONMENT_WIDTH");
        unsigned count = 0;

        for (std::size_t at = 0; (at = original.find(field.before, at)) != std::string_view::npos;
             at += field.before.size()) {
            auto after = std::string(field.after);

            if (after.size() < field.before.size()) {
                after.insert(after.size() - 1, field.before.size() - after.size(), '/');
            }

            for (const auto &[previous, bytes] : changes) {
                need(at + after.size() <= previous || previous + bytes.size() <= at,
                     "QNX_PASSION_ENVIRONMENT_OVERLAP");
            }

            changes.emplace_back(at, std::move(after));
            ++count;
        }

        need(count == field.count, "QNX_PASSION_ENVIRONMENT_COUNT");
    }

    std::string output(original);

    for (const auto &[at, bytes] : changes) {
        output.replace(at, bytes.size(), bytes);
    }

    need(output.size() == original.size(), "QNX_PASSION_ENVIRONMENT_EXTENT");

    return output;
}
} // namespace

std::string adapt_qnx_passion_boot_script(std::string_view original) {
    return environment_fields(
            original,
            {{"-D\0SLOG2_PHYS\0"sv,
              "-s\0"
              "64000     \0"sv,
              1},
             {"/dev/ser1\0"sv, "/dev/text\0"sv, 2},
             {"/dev/slog2\0"sv, "/proc/boot\0"sv, 1},
             {"/dev/bmetrics\0"sv, "/proc/boot/./\0"sv, 1},
             {"/bin/startupmgr\0-i\0ifs2\0-m\0/\0"sv, "/bin/startupmgr\0-i\0ifs2\0-m\0/\0"sv, 1},
             {"PATH=/bin:/usr/bin:/usr/sbin:/usr/lib64:/etc:/sbin:/scripts\0"sv,
              "PATH=/proc/boot/bin:/proc/boot:/bin:/usr/bin:/sbin\0"sv, 12},
             {"LD_LIBRARY_PATH=/lib64/:/usr/lib64:/lib64/dll:/lib/:/usr/lib:/lib64/dll/pci\0"sv,
              "LD_LIBRARY_PATH=/lib64/:/usr/lib64:/lib64/dll:/lib/:/usr/lib:/lib64/dll/pci\0"sv,
              12}});
}

std::string adapt_qnx_passion_procnto_environment(std::string_view original) {
    return environment_fields(
            original, {{"PATH=/bin:/usr/bin:/usr/sbin:/sbin:/scripts\0"sv,
                        "PATH=/proc/boot/bin/:/bin/:/usr/bin/:/sbin/\0"sv, 1},
                       {"LD_LIBRARY_PATH=/lib64:/lib64/dll:/usr/lib:/usr/lib64:/lib::\0"sv,
                        "LD_LIBRARY_PATH=/lib64:/lib64/dll:/usr/lib:/usr/lib64:/lib::\0"sv, 1}});
}

void validate_qnx_passion_runtime(std::string_view runtime) {
    // This contract is shared by the OEM cluster script and this project's runtime, whose device
    // capacities differ on purpose: the OEM one sizes the real 2.5 GiB flash partition, this one
    // the RAM device fs-qnx6 accepts, so only the address token and the shape are pinned here.
    const std::string_view marker = "\nqm_source_capture=1\n";
    const auto at = runtime.find(marker);

    need(!runtime.empty() && at != std::string_view::npos &&
                 runtime.find(marker, at + 1) == std::string_view::npos &&
                 runtime.find("capacity=") != std::string_view::npos &&
                 runtime.find(",address=0x7c008000") != std::string_view::npos &&
                 runtime.find(",address=0x27c008000") != std::string_view::npos &&
                 runtime.find("/slm/ic_qt/ic_qt.xml") != std::string_view::npos &&
                 runtime.find("/usr/bin/ic_service") != std::string_view::npos &&
                 runtime.find(". ${qm_s}n/display_debug_tools.sh") != std::string_view::npos,
         "QNX_PASSION_RUNTIME_CONTRACT");
}

std::string prepare_qnx_passion_runtime(std::string_view source) {
    // The image writer checks slot capacity.
    need(source.starts_with("#!/proc/boot/bin/ksh\n"), "QNX_PASSION_RUNTIME_SOURCE_SHAPE");
    const auto runtime = prepare_qnx_guest_shell(source);
    validate_qnx_passion_runtime(runtime);

    return runtime;
}

std::string qnx_passion_network_script(std::string_view oem_startup, std::string_view address) {
    // Only the bodies of the two OEM functions are replayed; emac0 is the controller the VM lacks.
    std::string commands;
    std::set<std::string> vlans;
    unsigned arp = 0;

    for (const std::string_view function :
         {"start_config_vlan()\n{\n", "static_arp_config()\n{\n"}) {
        const auto begin = oem_startup.find(function);
        need(begin != std::string_view::npos &&
                     oem_startup.find(function, begin + 1) == std::string_view::npos,
             "QNX_PASSION_NETWORK_SOURCE");
        const auto body = begin + function.size();
        const auto end = oem_startup.find("\n}\n", body);
        need(end != std::string_view::npos, "QNX_PASSION_NETWORK_SOURCE");

        for (std::size_t at = body; at < end;) {
            auto next = oem_startup.find('\n', at);
            auto line = oem_startup.substr(at, next - at);
            at = next + 1;
            line.remove_prefix(std::min(line.find_first_not_of(' '), line.size()));

            if (line.starts_with("ifconfig vlan")) {
                std::string text(line);
                const auto parent = text.find(" vlanif emac0");

                if (parent != std::string::npos) {
                    need(text.size() == parent + 13, "QNX_PASSION_NETWORK_SOURCE");
                    text.replace(parent, 13, " vlanif tap0");
                }

                vlans.insert(text.substr(9, text.find(' ', 9) - 9));
                commands += text + " || return 1\n";
            } else if (line.starts_with("arp -s ")) {
                // An address outside the VLAN subnets (the OEM emac0 one) has no route here.
                commands += std::string(line) + " || echo 'QPC NETWORK " + std::string(line) +
                            " failed'\n";
                ++arp;
            } else {
                need(line.empty() || line.starts_with("log_launch "), "QNX_PASSION_NETWORK_SOURCE");
            }
        }
    }

    need(vlans == std::set<std::string>{"vlan4", "vlan10", "vlan11", "vlan12", "vlan13",
                                        "vlan14"} &&
                 commands.find("\narp -s 172.16.112.10 02:DF:53:00:00:00 ||") !=
                         std::string::npos &&
                 arp > 0 && commands.find("emac0") == std::string::npos,
         "QNX_PASSION_NETWORK_SOURCE");

    for (const auto *id : {"4", "10", "11", "12", "13", "14"}) {
        const auto vlan = std::string("ifconfig vlan") + id;
        need(commands.find(vlan + " create ||") != std::string::npos &&
                     commands.find(vlan + " vlan " + id + " vlanif tap0 ||") != std::string::npos,
             "QNX_PASSION_NETWORK_SOURCE");
    }

    return "ifconfig tap0 create up || return 1\nifconfig tap0 " + std::string(address) +
           " netmask 255.255.255.0 up || return 1\n" + commands +
           "VOYEMU_QNET_RELAY=1 LD_PRELOAD=$qm_snap /bin/ls -d / >&2 &\n"
           "echo \"QPC QNET relay_pid=$! vlans=6 arp=" +
           std::to_string(arp) + "\"\n";
}

std::string adapt_qnx_passion_startupmgr_dispatch(std::string_view original,
                                                  std::string_view reader) {
    need(original.size() == 217630, "QNX_PASSION_STARTUPMGR_SIZE");
    need(reader.size() == 176, "QNX_PASSION_READER_SIZE");
    constexpr std::size_t slot = 0x30b1c, rw = 0x30e78;
    need(field(original, 56, 2) == 8 && field(original, 0xb0, 4) == 1 &&
                 field(original, 0xb4, 4) == 5 && field(original, 0xb8, 8) == 0 &&
                 field(original, 0xc0, 8) == 0 && field(original, 0xd0, 8) == slot &&
                 field(original, 0xd8, 8) == slot && field(original, 0xe0, 8) == 0x10000,
         "QNX_PASSION_RX_SEGMENT");
    need(field(original, 0xe8, 4) == 1 && field(original, 0xec, 4) == 6 &&
                 field(original, 0xf0, 8) == rw && field(original, 0xf8, 8) == 0x40e78 &&
                 field(original, 0x118, 8) == 0x10000 && slot + reader.size() < rw &&
                 slot + reader.size() < 0x40e78,
         "QNX_PASSION_RX_CAPACITY");
    need(original.substr(slot, rw - slot) == std::string(rw - slot, 0), "QNX_PASSION_RX_PADDING");
    // Verify the retained ABI prologue and real mmap PLT before selecting UFS dispatch.
    need(field(original, 0x14878, 4) == 0xa9be7bfd && field(original, 0x14880, 4) == 0x910003fd &&
                 field(original, 0x14890, 4) == 0xf9000bf3 &&
                 field(original, 0x14898, 4) == 0x97ffdf8a,
         "QNX_PASSION_DISPATCH_PROLOGUE");
    need(field(original, 0xc2a0, 4) == 0xd00001b0 && field(original, 0xc2a4, 4) == 0xf9459611 &&
                 field(original, 0xc2a8, 4) == 0x912ca210 &&
                 field(original, 0xc2ac, 4) == 0xd61f0220,
         "QNX_PASSION_MMAP_PLT");
    need(field(original, 0x1dab8, 4) == 0x52800000 && field(original, 0x1dabc, 4) == 0xd65f03c0 &&
                 field(original, 0x1dac4, 4) == 0xa9bf7bfd &&
                 field(original, 0x1dac8, 4) == 0x910003fd,
         "QNX_PASSION_UFS_GEOMETRY");
    struct Relocation {
        std::size_t at;
        std::uint64_t target, before, after;
    };
    constexpr std::array relas{Relocation{0x9908, 0x42e78, 0x1d9c4, slot},
                               Relocation{0x9920, 0x42e80, 0x1da94, 0x1dab8},
                               Relocation{0x99e0, 0x42ef0, 0x1dac0, 0x1dab8},
                               Relocation{0x99f8, 0x42f00, 0x1da00, 0x1dab8},
                               Relocation{0x9a28, 0x42f10, 0x1dae8, 0x1dab8},
                               Relocation{0x9a70, 0x42f28, 0x1da98, 0x1dab8},
                               Relocation{0x9ad0, 0x42f68, 0x1d9c0, 0x1dab8},
                               Relocation{0x9ae8, 0x42f70, 0x1db10, 0x1dab8},
                               Relocation{0x9b60, 0x42fa8, 0x1da78, 0x1dab8},
                               Relocation{0x9b78, 0x42fb0, 0x1da3c, slot},
                               Relocation{0x9bc0, 0x42fc8, 0x1da8c, 0x1dab8},
                               Relocation{0x9c08, 0x42fe0, 0x1d970, 0x1dab8},
                               Relocation{0x9c20, 0x42ff8, 0x1d998, 0x1dab8}};

    for (const auto &r : relas) {
        need(field(original, r.at, 8) == r.target && field(original, r.at + 8, 8) == 1027 &&
                     field(original, r.at + 16, 8) == r.before,
             "QNX_PASSION_DISPATCH_RELOCATION");
    }

    // All guards precede mutation of a new buffer; no original function is a code cave.
    std::string output(original), code(reader);
    put(code, 0x58,
        0x94000000U | (static_cast<std::uint32_t>((0xc2a0LL - slot - 0x58) / 4) & 0x3ffffffU), 4);
    output.replace(slot, code.size(), code);
    put(output, 0xd0, slot + code.size(), 8);
    put(output, 0xd8, slot + code.size(), 8);

    for (const auto &r : relas) {
        put(output, r.at + 16, r.after, 8);
    }

    put(output, 0x14898, 0x14000009, 4);
    put(output, 0x1dac4, 0x52a00020, 4);
    put(output, 0x1dac8, 0xd65f03c0, 4);
    need(output.substr(rw) == original.substr(rw) &&
                 output.substr(slot + code.size(), rw - slot - code.size()) ==
                         original.substr(slot + code.size(), rw - slot - code.size()),
         "QNX_PASSION_RW_UNCHANGED");

    return output;
}

std::string adapt_qnx_passion_startupmgr_entry(std::string_view original, std::string_view reader) {
    need(original.size() == 217630, "QNX_PASSION_STARTUPMGR_SIZE");
    struct Reference {
        std::size_t relocation, target, string;
        std::string_view before, after;
        std::size_t capacity;
    };
    using namespace std::string_view_literals;
    constexpr std::array refs{Reference{0x9080, 0x41e68, 0x2acee, "/scripts/startup.sh\0"sv,
                                        "/bin/ssplash\0"sv, sizeof("/proc/boot/bin/ssplash")},
                              Reference{0xa838, 0x434e0, 0x2ab02, "/etc/graphics.conf\0"sv,
                                        "/etc/graphics.conf\0"sv,
                                        sizeof("/proc/boot/etc/graphics.conf")}};

    for (const auto &r : refs) {
        need(field(original, r.relocation, 8) == r.target &&
                     field(original, r.relocation + 8, 8) == 1027 &&
                     field(original, r.relocation + 16, 8) == r.string &&
                     original.substr(r.string, r.before.size()) == r.before,
             "QNX_PASSION_ENTRY_REFERENCE");
    }

    // Preserve actual openwfd_server launch and its interior wfd_server reference.
    need(field(original, 0x8eb8, 8) == 0x41b68 && field(original, 0x8eb8 + 8, 8) == 1027 &&
                 field(original, 0x8eb8 + 16, 8) == 0x2ab29 &&
                 field(original, 0xa8e0, 8) == 0x43538 && field(original, 0xa8e0 + 8, 8) == 1027 &&
                 field(original, 0xa8e0 + 16, 8) == 0x2ab2d,
         "QNX_PASSION_WFD_REFERENCE");
    auto output = adapt_qnx_passion_startupmgr(original, reader);
    std::size_t cursor = 0x30b1c + reader.size();

    for (const auto &r : refs) {
        need(r.after.size() <= r.capacity && cursor + r.capacity <= 0x30e78,
             "QNX_PASSION_ENTRY_CAPACITY");
        std::string own_slot(r.capacity, 0);
        own_slot.replace(0, r.after.size(), r.after);
        output.replace(cursor, r.capacity, own_slot);
        put(output, r.relocation + 16, cursor, 8);
        cursor += r.capacity;
    }

    put(output, 0xd0, cursor, 8);
    put(output, 0xd8, cursor, 8);
    need(output.substr(0x30e78) == original.substr(0x30e78), "QNX_PASSION_ENTRY_RW");

    return output;
}

std::string adapt_qnx_passion_startupmgr_gpt(std::string_view original) {
    need(original.size() == 217630, "QNX_PASSION_STARTUPMGR_SIZE");
    struct Site {
        std::size_t at;
        std::uint32_t before, after;
    };
    constexpr std::array sites{
            Site{0x26fe8, 0x35000580, 0x52820001}, Site{0x26fec, 0xb9404fa1, 0xb90002c1},
            Site{0x27018, 0xf9402ba5, 0xd2b18005}, Site{0x2703c, 0x94000499, 0x9400003f}};
    constexpr std::array<std::uint32_t, 7> before{0xd1000442, 0xd2800003, 0x78637825, 0x2a0303e6,
                                                  0x340000e5, 0x93407c64, 0x91000463};
    constexpr std::array<std::uint32_t, 7> helper{0x52820006, 0xb9000046, 0xd2b18006, 0x914004c6,
                                                  0xf9000026, 0x52800000, 0xd65f03c0};

    for (const auto &site : sites) {
        need(field(original, site.at, 4) == site.before, "QNX_PASSION_GPT_INSTRUCTION");
    }

    for (unsigned i = 0; i < before.size(); ++i) {
        need(field(original, 0x27138 + i * 4, 4) == before[i], "QNX_PASSION_GPT_HELPER_SLOT");
    }

    // Preserve the two-LUN loop, mmap failure handling, and set-active implementation.
    // Own helper supplies the second fixed RAM-backed GPT address and size outputs.
    std::string output(original);

    for (const auto &site : sites) {
        put(output, site.at, site.after, 4);
    }

    for (unsigned i = 0; i < helper.size(); ++i) {
        put(output, 0x27138 + i * 4, helper[i], 4);
    }

    need(output.substr(0x270d0, 0x68) == original.substr(0x270d0, 0x68),
         "QNX_PASSION_GPT_ACTIVE_UNCHANGED");

    return output;
}

std::string adapt_qnx_passion_startupmgr(std::string_view original, std::string_view reader) {
    auto output = adapt_qnx_passion_startupmgr_dispatch(original, reader);
    const auto gpt = adapt_qnx_passion_startupmgr_gpt(original);

    for (std::size_t at = 0; at < original.size(); ++at) {
        if (gpt[at] != original[at]) {
            need(output[at] == original[at], "QNX_PASSION_STARTUPMGR_OVERLAP");
            output[at] = gpt[at];
        }
    }

    return output;
}

void validate_qnx_passion_ram_parent_source(std::string_view original) {
    // ABI preimages only; the caller authenticates the complete Passion original first.
    need(original.size() == 2254664, "QNX_PASSION_RAM_PARENT_SIZE");
    need(field(original, 0xe06c, 4) == 0x9135e084 && field(original, 0xe080, 4) == 0x94000aa4,
         "QNX_PASSION_RAM_PARENT_INSTRUCTION");
    // Empty asinfo state makes E030 the first root at index zero; B300 retains below4G.
    need(original.substr(0x67a60, 16) == std::string(16, 0) &&
                 field(original, 0x4ed0, 4) == 0xd65f03c0,
         "QNX_PASSION_RAM_PARENT_INITIAL_STATE");
    need(original.substr(0x2db78, 7) == std::string_view("memory\0", 7) &&
                 original.substr(0x2de90, 4) == std::string_view("ram\0", 4) &&
                 original.substr(0x2f448, 8) == std::string_view("below4G\0", 8),
         "QNX_PASSION_RAM_PARENT_NAMES");
}

std::string prepare_qnx_passion_startup(std::string_view original) {
    original_layout(original);
    validate_qnx_passion_ram_parent_source(original);
    struct PassionSite {
        std::size_t offset;
        std::uint32_t before, after;
    };
    constexpr std::array local_sites{
            PassionSite{0x322c, 0x94008a25, 0x52802d40},
            PassionSite{0x339c, 0x940089c9, 0x52802d40},
            PassionSite{0x3ec4, 0x940086ff, 0x52802d40},
            PassionSite{0x3fc0, 0x940086c0, 0x52802d40},
            PassionSite{0x4d20, 0x94008368, 0x52802d40},
            PassionSite{0x5488, 0x9400818e, 0x52802d40},
            PassionSite{0x3510, 0x34000377, 0x1400001b},
            PassionSite{0x253bc, 0x54fffee3, 0xd503201f},
            PassionSite{0x54c8, 0xd2a2f4c1, 0xd2a10141},
            PassionSite{0x54cc, 0xd2a2f400, 0xd2a10000},
            PassionSite{0x5608, 0xd2a2f4c1, 0xd2a10141},
            PassionSite{0x560c, 0xd2a2f400, 0xd2a10000},
            PassionSite{0x5664, 0xd2a2f4c1, 0xd2a10141},
            PassionSite{0x5668, 0xd2a2f400, 0xd2a10000},
            PassionSite{0x5bd8, 0x52800263, 0x52800363},
            PassionSite{0x44b4, 0xd503201f, 0xf9400661},
            PassionSite{0x44b8, 0xd503201f, 0xd360fc21},
            PassionSite{0x44bc, 0xd503201f, 0xd65f03c0},
            PassionSite{0xa860, 0xb9400e61,
                        0x94000000U |
                                (static_cast<std::uint32_t>((0x44b4 - 0xa860) / 4) & 0x3ffffffU)},
            PassionSite{0xa898, 0xb9400e60, 0xf9400660},
            PassionSite{0xa89c, 0xeb20405f, 0xeb40805f},
            PassionSite{0x6720, 0x94003712, 0xd2a20016},
            PassionSite{0x6724, 0x35001320, 0xd503201f},
            PassionSite{0x6730, 0xf94057f6, 0xd503201f},
            PassionSite{0x6754, 0x94003d5f, 0x52800000},
            PassionSite{0x35e0, 0x94000c24,
                        0x94000000U |
                                (static_cast<std::uint32_t>((0x8a000008ULL - 0x857007e0ULL) / 4) &
                                 0x3ffffffU)},
            PassionSite{0x6854, 0x94003d25,
                        0x94000000U |
                                (static_cast<std::uint32_t>((0x8a000000ULL - 0x85703a54ULL) / 4) &
                                 0x3ffffffU)},
            PassionSite{0x1677c, 0x52848000, 0x52800000}};

    for (const auto &site : local_sites) {
        need(field(original, site.offset, 4) == site.before, "QNX_PASSION_STARTUP_INSTRUCTION");
    }

    const auto ram = qnx_a11_ram_identity_mapping(original);
    const auto misc = qnx_a11_misc_failure_guard(original);
    qnx_a11_boot_storage_query_guard(original);
    const auto device = qnx_a11_gic_device_mapping(original);
    qnx_a11_cpu_name_guard(original);
    auto be = [&](std::size_t at) {
        std::uint32_t value = 0;

        for (unsigned i = 0; i < 4; ++i) {
            value = (value << 8) | static_cast<unsigned char>(original[at + i]);
        }

        return value;
    };
    // Actual Passion FDT offsets, not offsets inferred from another profile.
    const std::array<std::array<std::uint32_t, 5>, 3> regions{
            {{209120, 1, 0x01f00000, 0, 0x100000},
             {210412, 0, 0xb0000000, 0, 0x08000000},
             {211120, 1, 0, 0, 0x400000}}};

    for (const auto &region : regions) {
        need(be(region[0] - 8) == 16, "QNX_PASSION_FDT_REG_LENGTH");

        for (unsigned i = 0; i < 4; ++i) {
            need(be(region[0] + 4 * i) == region[i + 1], "QNX_PASSION_FDT_REG_SOURCE");
        }
    }

    need(be(209436) == 0 && be(209440) == 0xa0000000 && be(209444) == 0 && be(209448) == 0x0c7de000,
         "QNX_PASSION_IFS_RESERVATION");
    std::string output(original.substr(startup, startup_bytes));

    for (const auto &site : local_sites) {
        put(output, site.offset - startup, site.after, 4);
    }

    auto put_be = [&](std::size_t at, std::uint32_t value) {
        need(at >= startup && at + 4 <= stream, "QNX_PASSION_FDT_WRITE_BOUNDS");

        for (unsigned i = 0; i < 4; ++i) {
            output[at - startup + i] = static_cast<char>(value >> (24 - 8 * i));
        }
    };
    // Reconcile the former 128MiB Android guest region BEFORE allocator startup.
    // The separate Android emulator owns multimedia; this adapter forbids original VM launch.
    need(!qnx_passion_placement().original_android_vm_allowed, "QNX_PASSION_FOREIGN_VM_POLICY");
    put_be(210416, 0xaffc0000);
    put_be(210424, 0x10040000);

    for (const auto &region : std::array<std::array<std::uint32_t, 3>, 2>{
                 {{209120, 0x8c001000, 0x4000}, {211120, 0x8c000000, 0x1000}}}) {
        put_be(region[0], 0);
        put_be(region[0] + 4, region[1]);
        put_be(region[0] + 8, 0);
        put_be(region[0] + 12, region[2]);
    }

    // Keep the real RAM extent and allocator; register its metadata directly under memory.
    put(output, 0xe06c - startup, 0x2a1f03e4, 4);
    put(output, 0xe080 - startup, 0x97ffebb8, 4);
    put(output, 0x4f34 - startup, 0xb40001d6, 4);
    put(output, 0x4f38 - startup, 0x582dead4, 4);
    put(output, 0x45000 - startup, device, 8);
    put(output, 0x60938 - startup, 0x8a000080, 8);
    put(output, 0x60948 - startup, 0x8a0000c0, 8);
    put(output, 0x13178 - startup, misc, 4);
    put(output, 0x44080 - startup, 0x8a00041d, 8);
    put(output, 0x44a00 - startup, 0xb000041d, 8);
    put(output, 0x63e08 - startup, ram, 8);
    put(output, startup_bytes - 4, 0, 4);
    std::uint32_t sum = 0;

    for (std::size_t at = 0; at < startup_bytes - 4; at += 4) {
        sum += static_cast<std::uint32_t>(field(output, at, 4));
    }

    put(output, startup_bytes - 4, 0 - sum, 4);

    return output;
}

std::uint64_t qnx_passion_source_disk_required(std::uint64_t system_bytes) {
    // Splitting overlaps the complete wrapped source and its complete output parts.
    // All other generated files have a separately enforced aggregate cap.
    need(system_bytes == 2684354560ULL, "QNX_PASSION_SOURCE_SYSTEM_EXTENT");

    return 2 * (system_bytes + 98 * 512) + qnx_passion_source_generated_cap +
           qnx_passion_source_reserve;
}

void qnx_passion_require_source_disk(std::uint64_t available, std::uint64_t system_bytes) {
    need(available >= qnx_passion_source_disk_required(system_bytes),
         "QNX_PASSION_DEVELOPER_RESERVE");
}

QnxPassionPlacement qnx_passion_placement() {
    QnxPassionPlacement p;

    need(p.ifs_a + p.ifs_reserved <= p.ifs_b && p.ifs_b + p.ifs_reserved <= p.system &&
                 p.system + p.system_sectors * 512 <= p.ipl_map_end &&
                 p.system_sectors == p.system_bytes / 512 + 98 &&
                 p.system_raw_sectors == p.system_bytes / 512 &&
                 p.gpt_prefix + 64 * 4096 == p.ifs_b && p.ifs_b + p.ifs_reserved <= p.gpt_backup &&
                 p.gpt_backup == p.gpt_prefix + 65534 * 4096 && p.gpt_backup + 8192 <= p.system,
         "QNX_PASSION_PLACEMENT");

    return p;
}

std::string decode_qnx_passion_host(std::string_view original) {
    original_layout(original);

    return decode_qnx_declared_boot_stream(original.substr(stream), 68, 4454104);
}

std::string encode_qnx_passion_host(std::string_view original, std::string_view host) {
    original_layout(original);

    return encode_qnx_declared_boot_stream(original.substr(stream), host, 68, 4454104);
}

std::string pack_qnx_passion_kernel(std::string_view original, std::string_view packed,
                                    std::string_view host_sha) {
    original_layout(original);
    (void)qnx_passion_placement();
    need(sha256_text(host_sha), "QNX_PASSION_HOST_SHA_FORMAT");
    const auto host = decode_qnx_declared_boot_stream(packed, 68, 4454104);
    need(sha256_bytes(host) == host_sha, "QNX_PASSION_HOST_SHA");
    // The physical HYP reservation starts 0x200 bytes before this PT_LOAD.
    constexpr std::size_t load_capacity = 0x600000 - 0x200;
    need(!packed.empty() && startup_bytes + packed.size() <= load_capacity,
         "QNX_PASSION_KERNEL_GROWTH");
    std::string output(original.substr(0, stream));
    output.append(packed);
    const auto payload = output.size() - startup;
    put(output, 0xd0, payload, 8);
    put(output, 0xd8, payload, 8);
    put(output, 0x3024, payload, 4);
    put(output, stream - 4, 0, 4);
    std::uint32_t sum = 0;

    for (std::size_t at = startup; at < stream - 4; at += 4) {
        sum += static_cast<std::uint32_t>(field(output, at, 4));
    }

    put(output, stream - 4, 0 - sum, 4);
    need(output.substr(stream) == packed && field(output, 0xd0, 8) == output.size() - startup &&
                 field(output, 0xd8, 8) == output.size() - startup &&
                 field(output, 0x3024, 4) == output.size() - startup,
         "QNX_PASSION_PACKED_BINDING");

    return output;
}

std::string assemble_qnx_passion_kernel(std::string_view original, std::string_view packed,
                                        std::string_view host_sha) {
    auto output = pack_qnx_passion_kernel(original, packed, host_sha);
    const auto prepared = prepare_qnx_passion_startup(original);

    output.replace(startup, startup_bytes, prepared);
    put(output, 0x3024, output.size() - startup, 4);
    put(output, stream - 4, 0, 4);
    std::uint32_t sum = 0;

    for (std::size_t at = startup; at < stream - 4; at += 4) {
        sum += static_cast<std::uint32_t>(field(output, at, 4));
    }

    put(output, stream - 4, 0 - sum, 4);
    need(output.substr(stream) == packed, "QNX_PASSION_STARTUP_STREAM_BINDING");

    return output;
}

QnxPassionModuleClosure
update_qnx_passion_display_modules(std::string_view display, std::string_view manager,
                                   std::string_view front, std::string_view wfd,
                                   std::string_view runtime, std::string_view runtime_pin) {
    constexpr std::size_t registry = 0x2a5fc;

    need(manager.size() >= registry + 64 && manager.substr(registry, 64) == sha256_bytes(display),
         "QNX_PASSION_MODULE_REGISTRY_PREIMAGE");
    const auto old_front = read_qnx_imagefs_file(display, "bin/Snapdragon.rgb"),
               old_wfd = read_qnx_imagefs_file(display, "lib64/libopenwfd_qnx.so");
    need(!front.empty() && front.size() <= old_front.size() && !wfd.empty() &&
                 wfd.size() <= old_wfd.size(),
         "QNX_PASSION_MODULE_SLOT");
    auto next = replace_qnx_imagefs_file(display, "bin/Snapdragon.rgb", sha256_bytes(old_front),
                                         front, 4096, true);
    next = replace_qnx_imagefs_file(next, "lib64/libopenwfd_qnx.so", sha256_bytes(old_wfd), wfd, 1,
                                    true);
    auto padded_front = std::string(front), padded_wfd = std::string(wfd);
    padded_front.resize(old_front.size(), 0);
    padded_wfd.resize(old_wfd.size(), 0);
    need(read_qnx_imagefs_file(next, "bin/Snapdragon.rgb") == padded_front &&
                 read_qnx_imagefs_file(next, "lib64/libopenwfd_qnx.so") == padded_wfd,
         "QNX_PASSION_MODULE_READBACK");
    need(runtime.empty() == runtime_pin.empty(), "QNX_PASSION_MODULE_RUNTIME_PAIR");
    auto replace_runtime = [](std::string_view image, std::string_view before,
                              std::string_view after) {
        return after.size() > before.size()
                       ? replace_qnx_imagefs_file_in_zero_padding(image, "bin/ssplash",
                                                                  sha256_bytes(before), after, 4096)
                       : replace_qnx_imagefs_file(image, "bin/ssplash", sha256_bytes(before),
                                                  after);
    };
    std::string old_runtime;

    if (!runtime_pin.empty()) {
        old_runtime = read_qnx_imagefs_file(display, "bin/ssplash");
        need(sha256_bytes(old_runtime) == runtime_pin && !runtime.empty() &&
                     runtime.back() == '\n' && runtime.find('\0') == runtime.npos,
             "QNX_PASSION_MODULE_RUNTIME_SOURCE");
        need(runtime.size() <= old_runtime.size() + 4096 &&
                     old_runtime.size() <= runtime.size() + 4096,
             "QNX_PASSION_MODULE_RUNTIME_DELTA");
        next = replace_runtime(next, old_runtime, runtime);
        need(read_qnx_imagefs_file(next, "bin/ssplash") == runtime,
             "QNX_PASSION_MODULE_RUNTIME_READBACK");
    }

    auto inverse = runtime_pin.empty() ? next : replace_runtime(next, runtime, old_runtime);
    inverse = replace_qnx_imagefs_file(inverse, "lib64/libopenwfd_qnx.so", sha256_bytes(padded_wfd),
                                       old_wfd, 1, true);
    inverse = replace_qnx_imagefs_file(inverse, "bin/Snapdragon.rgb", sha256_bytes(padded_front),
                                       old_front, 4096, true);
    need(inverse == display, "QNX_PASSION_MODULE_FULL_INVERSE");
    auto bound = std::string(manager);
    bound.replace(registry, 64, sha256_bytes(next));
    auto restored = bound;
    restored.replace(registry, 64, sha256_bytes(display));
    need(restored == manager, "QNX_PASSION_MODULE_MANAGER_INVERSE");

    return {next, bound};
}
} // namespace voyemu
