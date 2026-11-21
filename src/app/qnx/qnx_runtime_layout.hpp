#pragma once
#include "check.hpp"
#include "error.hpp"
#include "model.hpp"
#include "qnx_interguest_contract.hpp"
#include "qnx_mcu_projection.hpp"
#include "qnx_qinggan11_mcu_layout.hpp"
#include <set>
#include <stdexcept>
#include <vector>

namespace voyemu {
struct QnxRuntimeLayout {
    std::string adapter;
    std::vector<std::string> originals, images;
    std::vector<std::pair<std::string, std::string>> loaders;
    Json projection;
    Json simulated_initial_native_state;
    Json interguest_network;
    Json persistence;
    unsigned memory_mib = 8192;
    // The profile whose machine metadata selected this layout; empty for a bare adapter layout.
    std::string profile{};
};

inline QnxRuntimeLayout qnx_runtime_layout(std::string_view adapter) {
    const QnxAdapterSpec *spec = nullptr;

    for (const auto &item : qnx_adapter_registry) {
        if (item.name == adapter) {
            spec = &item;
        }
    }

    if (spec == nullptr) {
        throw VoyemuError("QNX_LAYOUT", "unsupported hardware adapter");
    }

    if (spec->platform == QnxGuestPlatform::passion) {
        return {std::string(adapter),
                {"mifs_hyp_la.img", "ifs2_la.img", "system_la.img", "mifs_recovery_hyp_la.img",
                 "ifs2_recovery_la.img"},
                {"hyp-hvf.img", "ifs2-native.img", "host-native-lzo.bin", "smem.bin",
                 "early-l2.bin", "config-data-blank.bin", "synthetic-gpt.bin", "gpt-primary.bin",
                 "gpt-backup.bin", "qnx-ipl-virt.bin", "qnx-ifs2-restore.bin",
                 "system-parts/part-0.img", "system-parts/part-1.img", "system-parts/part-2.img"},
                {{"smem.bin", "0x86000000"},
                 {"host-native-lzo.bin", "0x90000000"},
                 {"host-native-lzo.bin", "0xe4d00000"},
                 {"ifs2-native.img", "0xa0000000"},
                 {"ifs2-native.img", "0xb0000000"},
                 {"ifs2-native.img", "0xe5000000"},
                 {"qnx-ifs2-restore.bin", "0x8a000000"},
                 {"synthetic-gpt.bin", "0xe4fb0000"},
                 {"gpt-primary.bin", "0xe4fc0000"},
                 {"gpt-primary.bin", "0xaffc0000"},
                 {"gpt-backup.bin", "0xf4fbe000"},
                 {"early-l2.bin", "0xe4fa0000"},
                 {"config-data-blank.bin", "0xf1000000"},
                 {"system-parts/part-0.img", "0x27c000000"},
                 {"system-parts/part-1.img", "0x2bc000000"},
                 {"system-parts/part-2.img", "0x2fc000000"}},
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                11713};
    }

    QnxRuntimeLayout layout{std::string(adapter),
                            {"hyp_la.img", "ifs2_la.img", "system_la.img", "ifs2_recovery_la.img"},
                            {"hyp-hvf.img", "ifs2-native.img", "host-native-lzo.bin", "smem.bin",
                             "qnx-ifs2-restore.bin", "synthetic-gpt.bin",
                             "sysla-gpt-ramdisk-v3.img", "qnx-ipl-virt.bin"},
                            {{"smem.bin", "0x86000000"},
                             {"host-native-lzo.bin", "0x90000000"},
                             {"ifs2-native.img", "0xa0000000"},
                             {"ifs2-native.img", "0xb0000000"},
                             {"qnx-ifs2-restore.bin", "0x8a000000"},
                             {"synthetic-gpt.bin", "0x8c000000"},
                             {"sysla-gpt-ramdisk-v3.img", "0xC0000000"}},
                            nullptr,
                            nullptr,
                            nullptr,
                            nullptr};

    if (spec->platform == QnxGuestPlatform::qinggan11) {
        layout.originals.pop_back();
        layout.images.insert(layout.images.end(), {"gpt-primary.bin", "gpt-backup.bin"});
        layout.loaders.insert(layout.loaders.end(), {{"gpt-primary.bin", "0xaffc0000"},
                                                     {"gpt-backup.bin", "0xbffbe000"}});
        layout.projection = qnx_mcu_projection(spec->adapter);
    }

    return layout;
}

inline QnxRuntimeLayout qnx_runtime_layout(std::string_view profile, const Json &metadata) {
    // The hardware adapter of the metadata selects the layout; the profile id selects nothing.
    const auto adapter = metadata.value("hardware_adapter", std::string("sa8155-qinggan09"));
    auto layout = qnx_runtime_layout(adapter);

    layout.profile = profile;
    const auto &spec = qnx_adapter(adapter);
    const bool free = spec.platform == QnxGuestPlatform::qinggan11;

    if (free && (!metadata.contains("mcu_projection") ||
                 !qnx_mcu_delivery_scope(Json{{"mcu_projection", metadata.at("mcu_projection")}},
                                         layout.projection))) {
        throw VoyemuError("QNX_LAYOUT", "Free MCU projection required");
    }

    // Only the qinggan11 adapters project vehicle fields; the Passion cluster receives none.
    if (!free && metadata.contains("mcu_projection") && !metadata.at("mcu_projection").is_null()) {
        throw VoyemuError("QNX_LAYOUT", "unexpected MCU projection");
    }

    if (metadata.contains("persistence")) {
        const Json expected = {{"schema", 1},
                               {"system_image", "sysla-gpt-ramdisk-v3.img"},
                               {"userdata_image", "userdata-qnx6.img"}};

        // Only the qinggan09 adapter keeps a persistent system image and QNX6 userdata.
        if (spec.platform != QnxGuestPlatform::qinggan09 ||
            metadata.at("persistence") != expected) {
            throw VoyemuError("QNX_LAYOUT", "unsupported persistent storage contract");
        }

        layout.persistence = expected;
        layout.images.push_back("userdata-qnx6.img");
    }

    if (metadata.contains("interguest_network")) {
        validate_qnx_interguest_contract(metadata.at("interguest_network"));
        layout.interguest_network = metadata.at("interguest_network");
    }

    if (metadata.contains("simulated_initial_native_state") &&
        !metadata.at("simulated_initial_native_state").is_null()) {
        const auto &native = metadata.at("simulated_initial_native_state");

        if (!free || !native.is_object() || !native.contains("fields") ||
            !native.at("fields").is_object() ||
            !native.at("fields").contains("eps_calibrationFlag") ||
            !native.at("fields").at("eps_calibrationFlag").is_object() ||
            !native.at("fields").at("eps_calibrationFlag").contains("value")) {
            throw VoyemuError("QNX_LAYOUT", "invalid simulated native initial state");
        }

        const auto expected = qnx_qinggan11_simulated_initial_state(
                Json{{"eps_calibrationFlag",
                      native.at("fields").at("eps_calibrationFlag").at("value")}},
                adapter);

        if (native != expected) {
            throw VoyemuError("QNX_LAYOUT", "simulated native initial state differs");
        }

        layout.simulated_initial_native_state = native;
    }

    return layout;
}

// Normal start validates configuration metadata only; explicit check audits payload bytes.
inline QnxRuntimeLayout qnx_portable_layout(std::string_view profile, const Json &metadata) {
    auto layout = qnx_runtime_layout(profile, metadata);
    const auto base_fields = qnx_adapter(layout.adapter).platform == QnxGuestPlatform::passion
                                     ? (layout.projection.is_null() ? 3U : 4U)
                                     : (layout.projection.is_null() ? 2U : 4U);

    if (metadata.size() != base_fields +
                                   (layout.simulated_initial_native_state.is_null() ? 0U : 1U) +
                                   (layout.interguest_network.is_null() ? 0U : 1U) +
                                   (layout.persistence.is_null() ? 0U : 1U) ||
        metadata.at("guest_arch") != "aarch64" || !metadata.at("images").is_object() ||
        metadata.at("images").size() != layout.images.size()) {
        throw VoyemuError("QNX_LAYOUT", "exact portable fields and image roles required");
    }

    for (const auto &name : layout.images) {
        if (!metadata.at("images").contains(name)) {
            throw VoyemuError("QNX_LAYOUT", "portable image role missing");
        }

        const auto &item = metadata.at("images").at(name);

        if (!item.is_object() || item.size() != 3 || item.at("file") != "qnx/" + name ||
            !item.at("bytes").is_number_integer()) {
            throw VoyemuError("QNX_LAYOUT", "fixed portable image metadata required");
        }

        const auto bytes = item.at("bytes").get<std::uint64_t>();
        const auto pin = item.value("sha256", std::string{});

        if (!bytes || bytes > (8ULL << 30) || !sha256_text(pin)) {
            throw VoyemuError("QNX_LAYOUT", "portable image metadata bounds differ");
        }
    }

    return layout;
}

inline std::set<std::string> qnx_image_roles(const QnxRuntimeLayout &layout) {
    return {layout.images.begin(), layout.images.end()};
}

inline void qnx_runtime_metadata(Json &target, const QnxRuntimeLayout &layout) {
    if (!layout.interguest_network.is_null()) {
        target["interguest_network"] = layout.interguest_network;
    }

    if (!layout.persistence.is_null()) {
        target["persistence"] = layout.persistence;
    }

    if (qnx_adapter(layout.adapter).platform == QnxGuestPlatform::passion) {
        target["hardware_adapter"] = layout.adapter;
    }

    if (!layout.projection.is_null()) {
        target["hardware_adapter"] = layout.adapter;
        target["mcu_projection"] = layout.projection;

        if (!layout.simulated_initial_native_state.is_null()) {
            target["simulated_initial_native_state"] = layout.simulated_initial_native_state;
        }
    }
}

inline void qnx_runtime_launch_metadata(Json &target, const QnxRuntimeLayout &layout) {
    qnx_runtime_metadata(target, layout);

    if (qnx_adapter(layout.adapter).platform == QnxGuestPlatform::passion &&
        !layout.profile.empty()) {
        target["profile"] = layout.profile;
    }
}
} // namespace voyemu
