#include "process.hpp"
#include "qnx_acceptance.hpp"
#include "qnx_run_budget.hpp"
#include "quickboot.hpp"
#include <cassert>
#include <exception>
#include <fstream>
#include <iostream>
using namespace voyemu;

int main() try {
    assert(qnx_source_acceptance_seconds(QnxEngine::hvf) == 90);
    assert(qnx_source_acceptance_seconds(QnxEngine::tcg_file) == 10800);
    // HVF keeps its host deadlines; TCG stretches them by the ratio of the acceptance budgets.
    assert(qnx_guest_deadline(QnxEngine::hvf, std::chrono::seconds(300)) ==
           std::chrono::seconds(300));
    assert(qnx_guest_deadline(QnxEngine::tcg_file, std::chrono::seconds(300)) ==
           std::chrono::seconds(300 * qnx_source_acceptance_seconds(QnxEngine::tcg_file) / 90));
    assert(qnx_guest_sync(QnxEngine::hvf) == std::chrono::milliseconds(5000) &&
           qnx_cluster_close_extra(QnxEngine::hvf) == std::chrono::milliseconds(0));
    assert(qnx_guest_sync(QnxEngine::tcg_file) ==
                   std::chrono::milliseconds(
                           5000 * qnx_source_acceptance_seconds(QnxEngine::tcg_file) / 90) &&
           qnx_cluster_close_extra(QnxEngine::tcg_file) ==
                   qnx_guest_sync(QnxEngine::tcg_file) - std::chrono::milliseconds(5000));
    assert(qnx_cluster_seconds_valid(QnxEngine::hvf, 3600, Json::object()));
    assert(!qnx_cluster_seconds_valid(QnxEngine::hvf, 3601, Json::object()));
    assert(qnx_cluster_seconds_valid(QnxEngine::tcg_file, 3600, Json::object()));
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 0, Json::object()));
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10801, Json::object()));
    auto rejects = [](auto call) {
        bool rejected = false;
        try {
            call();
        } catch (const std::exception &) {
            rejected = true;
        }

        assert(rejected);
    };
    const std::vector<std::uint64_t> copies{1388636631, 15956};
    const auto required = qnx_acceptance_disk_required(copies);
    assert(required == 19642263595ULL);
    rejects([&] {
        qnx_acceptance_require_disk(required - 1, copies);
    });
    qnx_acceptance_require_disk(required, copies);
    qnx_acceptance_require_disk(required + 1, copies);
    const auto passion_required = qnx_acceptance_disk_required(copies, 11713);
    assert(passion_required - required == (std::uint64_t(11713 - 8192) << 20));
    rejects([&] {
        qnx_acceptance_require_disk(passion_required - 1, copies, 11713);
    });
    qnx_acceptance_require_disk(passion_required, copies, 11713);
    rejects([] {
        qnx_acceptance_disk_required({}, 8193);
    });
    assert(qnx_acceptance_disk_required({}) == (17ULL << 30));
    assert(qnx_acceptance_disk_reserve == (8ULL << 30));
    assert(!qnx_acceptance_disk_reserve_available(qnx_acceptance_disk_reserve - 1));
    assert(qnx_acceptance_disk_reserve_available(qnx_acceptance_disk_reserve));
    assert(qnx_acceptance_disk_reserve_available(qnx_acceptance_disk_reserve + 1));
    assert(qnx_acceptance_disk_required(
                   {std::numeric_limits<std::uint64_t>::max() - (17ULL << 30)}) ==
           std::numeric_limits<std::uint64_t>::max());
    rejects([] {
        qnx_acceptance_disk_required(
                {std::numeric_limits<std::uint64_t>::max() - (17ULL << 30), 1});
    });
    rejects([] {
        qnx_acceptance_disk_required({std::numeric_limits<std::uint64_t>::max()});
    });
    rejects([] {
        accept_qnx_images("relative", "qinggan09", "request.json", "output");
    });
    const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                      ("qnx-acceptance-guards-" + random_token());
    ensure_private_directory(root / "src/app");
    std::ofstream(root / "src/app/qnx/qnx_prepare.cpp") << "source fixture";
    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};
    const auto engine = root / "qemu-system-aarch64.exe";
    {
        std::ofstream file(engine, std::ios::binary);
        file << "owned Windows engine fixture";
    }
    const auto engine_pin = sha256_file(engine);
    Json launch = {{"qemu", {{"executable", engine.string()}}},
                   {"pins", Json::array({{{"path", engine.string()}, {"sha256", engine_pin}}})},
                   {"qnx_file_backend",
                    {{"schema", 1}, {"version", "11.1.2"}, {"engine_sha256", engine_pin}}},
                   {"persistent_storage",
                    {{"schema", 1}, {"system", "system.img"}, {"userdata", "userdata.img"}}}};
    launch["source_acceptance"] = true;
    assert(qnx_cluster_seconds_valid(QnxEngine::tcg_file, 3601, launch, true));
    assert(qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, launch, true));
    assert(!qnx_cluster_seconds_valid(QnxEngine::hvf, 10800, launch));
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10801, launch, true));
    auto bad = launch;
    bad.erase("qnx_file_backend");
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, bad, true));
    // QEMU outside Windows maps the RAM file natively and carries no file backend capability.
    assert(qnx_source_acceptance_seconds(QnxEngine::tcg_file) == 10800);
    assert(qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, bad, false));
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10801, bad, false));
    bad = launch;
    bad["qnx_file_backend"]["engine_sha256"] = std::string(64, '0');
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, bad, true));
    bad = launch;
    bad["qnx_file_backend"]["version"] = "11.1.0";
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, bad, true));
    bad = launch;
    bad["qnx_file_backend"]["schema"] = true;
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, bad, true));
    bad = launch;
    bad["pins"][0]["sha256"] = std::string(64, '0');
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, bad, true));
    bad = launch;
    bad["pins"].push_back(bad["pins"][0]);
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, bad, true));
    bad = launch;
    bad.erase("persistent_storage");
    assert(qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, bad, true));
    bad = launch;
    bad.erase("source_acceptance");
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, bad, true));
    bad = launch;
    bad["source_acceptance"] = 1;
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, bad, true));
    bad = launch;
    bad["qemu"]["executable"] = (root / "missing.exe").string();
    assert(!qnx_cluster_seconds_valid(QnxEngine::tcg_file, 10800, bad, true));
    const auto disk_output = root / "disk-output";
    rejects([&] {
        qnx_acceptance_require_disk(required - 1, copies);
        std::filesystem::create_directory(disk_output);
    });
    assert(!std::filesystem::exists(disk_output));
    rejects([&] {
        accept_qnx_images(root, "../foreign", root / "missing.json", root / ".local/build/output");
    });
    const auto request = root / ".local/build/tmp/request.json";
    write_private_json(request, {{"schema", 2}, {"profile", "qinggan09"}});
    const auto request_pin = sha256_file(request);
    const auto request_identity = cache_file_identity(request);
    rejects([&] {
        accept_qnx_images(root, "qinggan09", request, root / ".local/build/output");
    });
    assert(!std::filesystem::exists(root / ".local/build/output") &&
           sha256_file(request) == request_pin && cache_file_identity(request) == request_identity);
    unsigned build_entries = 0;

    for (const auto &entry : std::filesystem::recursive_directory_iterator(root / ".local/build")) {
        ++build_entries;
        assert(entry.path() == request || entry.path() == request.parent_path());
    }

    assert(build_entries == 2);
    const Json owner = {
            {"schema", 1}, {"source", "original-OEM-Screen-BMP"}, {"producer_address", 0x50010000}};
    const auto projection = qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_731);
    Json delivery = {{"vehicle",
                      {{"status", "guest_acknowledged"},
                       {"fields", projection.at("projected_fields")},
                       {"source_sequence", 2},
                       {"full_wire_verified", false},
                       {"mcu_projection", projection}}},
                     {"presentation", {{"after_timestamp_ns", 100}}}};
    assert(qnx_qinggan11_post_ack_frame(delivery, owner, owner, 101, 2, projection));
    const auto projection_611 = qnx_mcu_projection(QnxGuestAdapter::sa8155_qinggan11_611);
    auto delivery_611 = delivery;
    delivery_611["vehicle"]["fields"] =
            qnx_adapter(QnxGuestAdapter::sa8155_qinggan11_611).mcu_projected_fields;
    delivery_611["vehicle"]["mcu_projection"] = projection_611;
    assert(qnx_qinggan11_post_ack_frame(delivery_611, owner, owner, 101, 2, projection_611));
    // Each adapter names itself, so the equal scopes of the two firmwares stay distinct.
    assert(!qnx_qinggan11_post_ack_frame(delivery_611, owner, owner, 101, 2, projection));
    assert(!qnx_qinggan11_post_ack_frame(delivery, owner, owner, 101, 2, projection_611));

    std::vector<std::byte> sparse(1920 * 720 * 4);
    assert(!qnx_native_frame_nonblank(sparse, 1920, 720));
    std::fill(sparse.begin(), sparse.end(), std::byte{255});
    assert(!qnx_native_frame_nonblank(sparse, 1920, 720));
    std::fill(sparse.begin(), sparse.end(), std::byte{});
    sparse[100] = std::byte{255};
    assert(!qnx_native_frame_nonblank(sparse, 1920, 720));

    for (unsigned y = 20; y < 40; ++y) {
        for (unsigned x = 50; x < 200; ++x) {
            sparse[static_cast<std::size_t>(y * 1920 + x) * 4] =
                    std::byte{static_cast<unsigned char>(65 + x % 190)};
        }
    }

    assert(!qnx_native_frame_nonblank(sparse, 1920, 720));

    for (unsigned y = 490; y < 510; ++y) {
        for (unsigned x = 1400; x < 1500; ++x) {
            sparse[(y * 1920 + x) * 4 + 1] = std::byte{static_cast<unsigned char>(65 + x % 190)};
        }
    }

    assert(qnx_native_frame_nonblank(sparse, 1920, 720));
    assert(!qnx_native_frame_nonblank(sparse, 1919, 720));

    assert(!qnx_qinggan11_post_ack_frame(delivery, owner, owner, 100, 2, projection));
    assert(!qnx_qinggan11_post_ack_frame(delivery, owner, owner, 101, 3, projection));
    auto foreign = owner;
    foreign["producer_address"] = 0x50011000;
    assert(!qnx_qinggan11_post_ack_frame(delivery, foreign, owner, 101, 2, projection));
    auto missing = delivery;
    missing["vehicle"].erase("mcu_projection");
    assert(!qnx_qinggan11_post_ack_frame(missing, owner, owner, 101, 2, projection));
    auto wrong = delivery;
    wrong["vehicle"]["mcu_projection"]["projected_fields"] =
            double(projection.at("projected_fields").get<unsigned>());
    assert(!qnx_qinggan11_post_ack_frame(wrong, owner, owner, 101, 2, projection));
    wrong = delivery;
    wrong["vehicle"]["fields"] = 579;
    assert(!qnx_qinggan11_post_ack_frame(wrong, owner, owner, 101, 2, projection));
    wrong = delivery;
    wrong["vehicle"]["full_wire_verified"] = true;
    assert(!qnx_qinggan11_post_ack_frame(wrong, owner, owner, 101, 2, projection));
    wrong = delivery;
    wrong["vehicle"]["source_sequence"] = -1;
    assert(!qnx_qinggan11_post_ack_frame(wrong, owner, owner, 101, 2, projection));
    std::cout
            << "QNX cold acceptance rejects invalid root/profile/request before VM or output publication\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
