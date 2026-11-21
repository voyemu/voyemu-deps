#include "qnx_runtime_layout.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace voyemu;

template <class F> static void rejects(F f) {
    bool rejected = false;

    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }

    assert(rejected);
}

int main() try {
    // Synthetic profile ids: the hardware adapter of the metadata selects every layout decision.
    constexpr std::string_view passion_profile = "test-passion",
                               qinggan09_profile = "test-qinggan09",
                               qinggan11_profile = "test-qinggan11",
                               scoped_profile = "test-qinggan11-scoped";
    const auto passion = qnx_runtime_layout("sa8155-passion-21480");

    assert(passion.memory_mib == 11713 && passion.images.size() == 15 &&
           passion.loaders.size() == 16 && passion.projection.is_null() && passion.profile.empty());
    // The vehicle gateway contract is a pinned machine part, never loaded into guest memory.
    assert(passion.images.back() == "someip-gateway.json" &&
           std::none_of(passion.loaders.begin(), passion.loaders.end(), [](const auto &loader) {
               return loader.first == "someip-gateway.json";
           }));
    const auto profiles = std::filesystem::path(__FILE__)
                                  .parent_path()
                                  .parent_path()
                                  .parent_path()
                                  .parent_path() /
                          "profiles";
    unsigned definitions = 0;

    // Every profile-qnx.json of the catalog declares exactly the originals of its adapter layout.
    for (const auto &entry : std::filesystem::directory_iterator(profiles)) {
        const auto file = entry.path() / "profile-qnx.json";

        if (!std::filesystem::is_regular_file(file)) {
            continue;
        }

        std::ifstream input(file);
        const auto definition = Json::parse(input);
        const auto layout =
                qnx_runtime_layout(definition.at("hardware_adapter").get<std::string>());
        assert(definition.at("originals").size() == layout.originals.size());

        for (const auto &original : layout.originals) {
            const auto &item = definition.at("originals").at(original);
            assert(item.at("file") == original &&
                   item.at("sha256").get<std::string>().size() == 64);
        }

        ++definitions;
    }

    assert(definitions > 0);

    Json passion_metadata = {{"guest_arch", "aarch64"}, {"images", Json::object()}};
    qnx_runtime_metadata(passion_metadata, passion);
    assert(passion_metadata.at("hardware_adapter") == passion.adapter);

    for (const auto &image : passion.images) {
        passion_metadata["images"][image] = {
                {"file", "qnx/" + image}, {"bytes", 1}, {"sha256", std::string(64, 'a')}};
    }

    const auto passion_portable = qnx_portable_layout(passion_profile, passion_metadata);
    assert(passion_portable.loaders == passion.loaders &&
           passion_portable.profile == passion_profile);
    assert(qnx_portable_layout(qinggan09_profile, passion_metadata).adapter == passion.adapter);
    assert(qnx_runtime_layout(passion_profile, Json::object()).adapter == "sa8155-qinggan09");
    Json passion_launch;
    qnx_runtime_launch_metadata(passion_launch, passion_portable);
    assert(passion_launch.at("profile") == std::string(passion_profile) &&
           passion_launch.at("hardware_adapter") == passion.adapter);
    // The Passion cluster receives no vehicle fields, so its layout takes no MCU projection.
    auto topics = passion_metadata;
    topics["mcu_projection"] = qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_731);
    rejects([&] {
        qnx_portable_layout(passion_profile, topics);
    });

    auto passion_network = passion_metadata;
    passion_network["interguest_network"] = qnx_interguest_contract();
    assert(qnx_runtime_layout(passion_profile, passion_network).interguest_network ==
           qnx_interguest_contract());
    // The Passion machine carries the same tap-ram contract through its portable metadata.
    Json passion_forwarded;
    qnx_runtime_metadata(passion_forwarded, qnx_portable_layout(passion_profile, passion_network));
    assert(passion_forwarded.at("interguest_network") == qnx_interguest_contract());
    passion_network["interguest_network"]["address"] = "10.0.2.2";
    rejects([&] {
        qnx_runtime_layout(passion_profile, passion_network);
    });

    for (const auto &image : passion.images) {
        auto bad = passion_metadata;
        bad["images"].erase(image);
        rejects([&] {
            qnx_portable_layout(passion_profile, bad);
        });
    }

    auto extra = passion_metadata;
    extra["mcu_projection"] = Json::object();
    rejects([&] {
        qnx_portable_layout(passion_profile, extra);
    });
    extra = passion_metadata;
    extra["memory_mib"] = 8192;
    rejects([&] {
        qnx_portable_layout(passion_profile, extra);
    });
    assert(passion.loaders[13].second == "0x27c000000" &&
           passion.loaders[14].second == "0x2bc000000" &&
           passion.loaders[15].second == "0x2fc000000");
    const auto a9 = qnx_runtime_layout("legacy-fixture", Json::object());
    assert(a9.memory_mib == 8192 && a9.originals.size() == 4 && a9.images.size() == 8 &&
           a9.loaders.size() == 7 && a9.projection.is_null());
    assert((a9.loaders == std::vector<std::pair<std::string, std::string>>{
                                  {"smem.bin", "0x86000000"},
                                  {"host-native-lzo.bin", "0x90000000"},
                                  {"ifs2-native.img", "0xa0000000"},
                                  {"ifs2-native.img", "0xb0000000"},
                                  {"qnx-ifs2-restore.bin", "0x8a000000"},
                                  {"synthetic-gpt.bin", "0x8c000000"},
                                  {"sysla-gpt-ramdisk-v3.img", "0xC0000000"}}));
    Json metadata = {{"guest_arch", "aarch64"}, {"images", Json::object()}}, old = metadata;
    qnx_runtime_metadata(metadata, a9);
    assert(metadata == old);
    auto network = old;
    network["interguest_network"] = qnx_interguest_contract();
    const auto connected = qnx_runtime_layout(qinggan09_profile, network);
    assert(connected.interguest_network == qnx_interguest_contract());
    Json forwarded;
    qnx_runtime_metadata(forwarded, connected);
    assert(forwarded.at("interguest_network") == network.at("interguest_network"));

    for (const auto &image : connected.images) {
        network["images"][image] = {
                {"file", "qnx/" + image}, {"bytes", 1}, {"sha256", std::string(64, 'a')}};
    }

    assert(qnx_portable_layout(qinggan09_profile, network).interguest_network ==
           qnx_interguest_contract());

    for (const auto *key :
         {"schema", "transport", "address", "peer_address", "prefix", "qconn_port"}) {
        auto bad = network;
        bad["interguest_network"].erase(key);
        rejects([&] {
            qnx_portable_layout(qinggan09_profile, bad);
        });
    }

    auto redirected = network;
    redirected["interguest_network"]["address"] = "10.0.2.2";
    rejects([&] {
        qnx_portable_layout(qinggan09_profile, redirected);
    });
    auto persisted = network;
    persisted["persistence"] = {{"schema", 1},
                                {"system_image", "sysla-gpt-ramdisk-v3.img"},
                                {"userdata_image", "userdata-qnx6.img"}};
    persisted["images"]["userdata-qnx6.img"] = {{"file", "qnx/userdata-qnx6.img"},
                                                {"bytes", 33554432},
                                                {"sha256", std::string(64, 'a')}};
    const auto persistent_layout = qnx_portable_layout(qinggan09_profile, persisted);
    assert(persistent_layout.images.size() == 9 && !persistent_layout.persistence.is_null());
    auto missing_userdata = persisted;
    missing_userdata["images"].erase("userdata-qnx6.img");
    rejects([&] {
        qnx_portable_layout(qinggan09_profile, missing_userdata);
    });
    auto renamed_userdata = persisted;
    renamed_userdata["persistence"]["userdata_image"] = "other.img";
    rejects([&] {
        qnx_portable_layout(qinggan09_profile, renamed_userdata);
    });
    Json persistent_metadata;
    qnx_runtime_metadata(persistent_metadata, persistent_layout);
    assert(persistent_metadata.at("persistence") == persisted.at("persistence"));
    auto fractional_network = network;
    fractional_network["interguest_network"]["prefix"] = 24.0;
    rejects([&] {
        qnx_portable_layout(qinggan09_profile, fractional_network);
    });
    Json portable_scoped = {{"guest_arch", "aarch64"}, {"images", Json::object()}};
    const auto layout_scoped = qnx_runtime_layout("sa8155-qinggan11-611");
    qnx_runtime_metadata(portable_scoped, layout_scoped);
    portable_scoped["interguest_network"] = qnx_interguest_contract();
    portable_scoped["simulated_initial_native_state"] = qnx_qinggan11_simulated_initial_state(
            Json{{"eps_calibrationFlag", 1}}, "sa8155-qinggan11-611");

    for (const auto &image : layout_scoped.images) {
        portable_scoped["images"][image] = {
                {"file", "qnx/" + image}, {"bytes", 1}, {"sha256", std::string(64, 'a')}};
    }

    const auto accepted_scoped = qnx_portable_layout(scoped_profile, portable_scoped);
    assert(accepted_scoped.images.size() == 10 &&
           accepted_scoped.projection.at("projected_fields") ==
                   qnx_adapter(QnxGuestAdapter::sa8155_qinggan11_611).mcu_projected_fields &&
           accepted_scoped.projection.at("unsupported_fields") ==
                   736 - qnx_adapter(QnxGuestAdapter::sa8155_qinggan11_611).mcu_projected_fields);
    // A machine of an earlier guest build with a smaller projected field count is rebuilt.
    auto earlier_scope = portable_scoped;
    earlier_scope["mcu_projection"]["projected_fields"] = 493;
    earlier_scope["mcu_projection"]["unsupported_fields"] = 243;
    rejects([&] {
        qnx_portable_layout(scoped_profile, earlier_scope);
    });
    auto fractional_scoped = portable_scoped;
    fractional_scoped["mcu_projection"]["projected_fields"] =
            double(qnx_adapter(QnxGuestAdapter::sa8155_qinggan11_611).mcu_projected_fields);
    rejects([&] {
        qnx_portable_layout(scoped_profile, fractional_scoped);
    });
    auto wrong_scoped = portable_scoped;
    wrong_scoped["mcu_projection"] = qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_731);
    rejects([&] {
        qnx_portable_layout(scoped_profile, wrong_scoped);
    });
    wrong_scoped = portable_scoped;
    wrong_scoped["simulated_initial_native_state"]["hardware_adapter"] = "sa8155-qinggan11-731";
    rejects([&] {
        qnx_portable_layout(scoped_profile, wrong_scoped);
    });
    wrong_scoped = portable_scoped;
    wrong_scoped["mcu_projection"]["settings_feedback_supported"] = true;
    rejects([&] {
        qnx_portable_layout(scoped_profile, wrong_scoped);
    });
    auto free = qnx_runtime_layout("sa8155-qinggan11-731");
    qnx_runtime_metadata(metadata, free);
    const auto actual = qnx_runtime_layout(qinggan11_profile, metadata);
    auto free_network = metadata;
    free_network["interguest_network"] = qnx_interguest_contract();
    const auto connected_free = qnx_runtime_layout(qinggan11_profile, free_network);
    assert(connected_free.interguest_network == qnx_interguest_contract());
    Json free_forwarded;
    qnx_runtime_metadata(free_forwarded, connected_free);
    assert(free_forwarded.at("interguest_network") == qnx_interguest_contract());

    for (const auto &image : connected_free.images) {
        free_network["images"][image] = {
                {"file", "qnx/" + image}, {"bytes", 1}, {"sha256", std::string(64, 'a')}};
    }

    assert(qnx_portable_layout(qinggan11_profile, free_network).interguest_network ==
           qnx_interguest_contract());
    auto redirected_free = free_network;
    redirected_free["interguest_network"]["address"] = "10.0.2.2";
    rejects([&] {
        qnx_portable_layout(qinggan11_profile, redirected_free);
    });
    assert(actual.originals ==
           std::vector<std::string>({"hyp_la.img", "ifs2_la.img", "system_la.img"}));
    assert(actual.images.size() == 10 && qnx_image_roles(actual).size() == 10 &&
           actual.loaders.size() == 9);
    assert(actual.loaders.at(7) ==
           std::make_pair(std::string("gpt-primary.bin"), std::string("0xaffc0000")));
    assert(actual.loaders.at(8) ==
           std::make_pair(std::string("gpt-backup.bin"), std::string("0xbffbe000")));
    assert(actual.loaders.at(2).first == actual.loaders.at(3).first &&
           actual.loaders.at(2).second != actual.loaders.at(3).second);
    // Metadata without a hardware adapter selects the qinggan09 layout under any profile id.
    assert(qnx_runtime_layout(qinggan11_profile, old).adapter == "sa8155-qinggan09");
    auto persistent_free = metadata;
    persistent_free["persistence"] = persisted.at("persistence");
    rejects([&] {
        qnx_runtime_layout(qinggan09_profile, persistent_free);
    });
    rejects([] {
        qnx_runtime_layout("unknown");
    });

    for (const auto *key :
         {"packet_bytes", "wire_fields", "projected_fields", "unsupported_fields"}) {
        auto bad = metadata;
        bad["mcu_projection"][key] = 0;
        rejects([&] {
            qnx_runtime_layout(qinggan11_profile, bad);
        });
    }

    auto fractional = metadata;
    fractional["mcu_projection"]["projected_fields"] =
            double(metadata.at("mcu_projection").at("projected_fields").get<unsigned>());
    rejects([&] {
        qnx_runtime_layout(qinggan11_profile, fractional);
    });
    const auto legacy_projection = metadata.at("mcu_projection");

    for (unsigned value = 0; value < 4; ++value) {
        auto native = metadata;
        native["simulated_initial_native_state"] =
                qnx_qinggan11_simulated_initial_state(Json{{"eps_calibrationFlag", value}});
        const auto layout = qnx_runtime_layout(qinggan11_profile, native);
        Json launch;
        qnx_runtime_metadata(launch, layout);
        assert(launch.at("simulated_initial_native_state") ==
                       native.at("simulated_initial_native_state") &&
               launch.at("mcu_projection") == legacy_projection);
        auto portable = native;

        for (const auto &image : layout.images) {
            portable["images"][image] = {
                    {"file", "qnx/" + image}, {"bytes", 1}, {"sha256", std::string(64, 'a')}};
        }

        assert(qnx_portable_layout(qinggan11_profile, portable).simulated_initial_native_state ==
               layout.simulated_initial_native_state);
        auto prior = portable;
        prior.erase("simulated_initial_native_state");
        assert(prior.size() == 4 && qnx_portable_layout(qinggan11_profile, prior)
                                            .simulated_initial_native_state.is_null());
        auto extra = portable;
        extra["unknown"] = 1;
        rejects([&] {
            qnx_portable_layout(qinggan11_profile, extra);
        });
        auto wrong = native;
        wrong["simulated_initial_native_state"]["fields"]["eps_calibrationFlag"]["width"] = 1;
        rejects([&] {
            qnx_runtime_layout(qinggan11_profile, wrong);
        });
        wrong = native;
        wrong["simulated_initial_native_state"]["fields"]["eps_calibrationFlag"]["value"] = 4;
        rejects([&] {
            qnx_runtime_layout(qinggan11_profile, wrong);
        });
        wrong = old;
        wrong["simulated_initial_native_state"] = native.at("simulated_initial_native_state");
        rejects([&] {
            qnx_runtime_layout("legacy-fixture", wrong);
        });
    }

    assert(qnx_runtime_layout(qinggan11_profile, metadata)
                   .simulated_initial_native_state.is_null());
    std::cout << "qinggan09, qinggan11 and Passion adapter layouts passed; no VM\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
