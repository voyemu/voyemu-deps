#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace voyemu {
struct QnxPassionPlacement {
    std::uint64_t ifs_a = 0xa0000000, ifs_b = 0xb0000000, ifs_reserved = 0x0c7de000;
    std::uint64_t system = 0xc0000000, system_bytes = 2684354560ULL, system_sectors = 5242978,
                  system_raw_sectors = 5242880;
    std::uint64_t gpt_prefix = 0xaffc0000, gpt_backup = 0xbffbe000, gpt_cache = 0x8c000000,
                  gpt_blocks = 65536;
    std::uint64_t ipl_map_end = 0x180000000ULL;
    bool original_android_vm_allowed = false;
};
QnxPassionPlacement qnx_passion_placement();
inline constexpr std::uint64_t qnx_passion_source_reserve = 8ULL << 30,
                               qnx_passion_source_generated_cap = 224ULL << 20;

inline bool qnx_passion_guest_space_available(std::uint64_t available) {
    return available >= qnx_passion_source_reserve + qnx_passion_source_generated_cap;
}

std::uint64_t qnx_passion_source_disk_required(std::uint64_t system_bytes);
void qnx_passion_require_source_disk(std::uint64_t available, std::uint64_t system_bytes);
struct QnxPassionModuleClosure {
    std::string display, manager;
};
// Retains existing module slots, runtime, carrier and verifier; verifies a full inverse.
QnxPassionModuleClosure
update_qnx_passion_display_modules(std::string_view display, std::string_view manager,
                                   std::string_view front, std::string_view wfd,
                                   std::string_view runtime = {},
                                   std::string_view expected_runtime_sha256 = {});
void validate_qnx_passion_runtime(std::string_view runtime);
std::string prepare_qnx_passion_runtime(std::string_view source);
// Guest network script from the pinned OEM /scripts/startup.sh: the cluster's SOME/IP VLANs and
// static ARP on tap0 in place of the absent emac0, then the tap-ram relay; the runtime sources it.
std::string qnx_passion_network_script(std::string_view oem_startup, std::string_view address);
std::string adapt_qnx_passion_startupmgr_entry(std::string_view original, std::string_view reader);
std::string adapt_qnx_passion_boot_script(std::string_view original);
std::string adapt_qnx_passion_procnto_environment(std::string_view original);
std::string adapt_qnx_passion_startupmgr(std::string_view original, std::string_view reader);
std::string adapt_qnx_passion_startupmgr_dispatch(std::string_view original,
                                                  std::string_view reader);
std::string adapt_qnx_passion_startupmgr_gpt(std::string_view original);
void validate_qnx_passion_ram_parent_source(std::string_view original);
std::string prepare_qnx_passion_startup(std::string_view original);
std::string assemble_qnx_passion_kernel(std::string_view original, std::string_view packed,
                                        std::string_view expected_host_sha256);
std::string encode_qnx_passion_host(std::string_view original, std::string_view host);
std::string decode_qnx_passion_host(std::string_view original);
// Developer packing primitive only: preserves the original startup instructions.
// No hardware boot or service readiness is implied by a successful host roundtrip.
std::string pack_qnx_passion_kernel(std::string_view original, std::string_view packed,
                                    std::string_view expected_host_sha256);
} // namespace voyemu
