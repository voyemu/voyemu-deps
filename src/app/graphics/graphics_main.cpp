#include "error.hpp"
#include "graphics.hpp"
#include "logging.hpp"
#include "program_modes.hpp"
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
std::uint64_t number(std::string_view value) {
    int base = 10;

    if (value.starts_with("0x")) {
        value.remove_prefix(2);
        base = 16;
    }

    std::uint64_t out = 0;
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), out, base);

    if (error != std::errc{} || end != value.data() + value.size() || value.empty()) {
        throw voyemu::VoyemuError("GRAPHICS", "Invalid numeric worker argument");
    }

    return out;
}
} // namespace

int voyemu::graphics_main(int argc, char **argv) {
    voyemu::graphics::Options options;

    try {
        std::uint64_t seconds = 0;
        bool stdio = false, bounded = false;

        for (int i = 1; i < argc; i++) {
            std::string_view arg = argv[i];

            if (arg == "--stdio-test") {
                stdio = true;
                continue;
            }

            if (arg == "--controls") {
                options.controls = true;
                continue;
            }

            if (arg == "--no-controls") {
                options.controls = false;
                continue;
            }

            if (i + 1 >= argc) {
                throw voyemu::VoyemuError("GRAPHICS", "Missing worker argument value");
            }

            std::string_view value = argv[++i];

            if (arg == "--session-id") {
                options.session_id = value;
            } else if (arg == "--mcu-adapter") {
                options.mcu_adapter = value;
            } else if (arg == "--guest-pid") {
                options.guest_pid = number(value);
            } else if (arg == "--producer-incarnation") {
                options.producer_incarnation = number(value);
            } else if (arg == "--generation") {
                options.generation = number(value);
            } else if (arg == "--worker-nonce") {
                options.worker_nonce = number(value);
            } else if (arg == "--ram-bytes") {
                options.ram_extent = number(value);
            } else if (arg == "--ram") {
                options.ram = value;
            } else if (arg == "--address") {
                options.address = number(value);
            } else if (arg == "--session") {
                options.session = value;
            } else if (arg == "--checkpoint-record") {
                options.record = value;
            } else if (arg == "--checkpoint-replay") {
                options.replay = value;
            } else if (arg == "--angle") {
                options.angle = value;
            } else if (arg == "--frame") {
                options.frame = value;
            } else if (arg == "--seconds") {
                seconds = number(value);
                bounded = true;
            } else {
                throw voyemu::VoyemuError("GRAPHICS", "Unknown worker argument");
            }
        }

        if (options.ram.empty() || options.session.empty() || options.angle.empty() ||
            !options.address || (bounded && (seconds < 1 || seconds > 86400))) {
            throw voyemu::VoyemuError(
                    "GRAPHICS",
                    "Usage: voyemu-graphics --ram PATH --address HEX --session PATH --angle DIRECTORY [--seconds 1..86400] [--frame PATH] [--no-controls] [--ram-bytes BYTES]");
        }

        const char *profile_flag = std::getenv("VOYEMU_RPC_PROFILE");
        options.profile_rpc = profile_flag && std::string_view(profile_flag) == "1";
        voyemu::graphics::Backend backend(options);
        voyemu::logging::write(
                voyemu::logging::Level::info, "graphics", "producer.started",
                {{"address", options.address}, {"bounded", bounded}, {"seconds", seconds}});

        if (stdio) {
            std::string line;

            while (std::getline(std::cin, line)) {
                if (line.size() > 32 * 1024) {
                    throw voyemu::VoyemuError("GRAPHICS", "Stdio request extent");
                }

                auto request = nlohmann::json::parse(line);
                auto values = request.at("args");

                if (!values.is_array() || values.size() > 16) {
                    throw voyemu::VoyemuError("GRAPHICS", "Stdio arguments");
                }

                voyemu::graphics::Arguments args{};

                for (std::size_t i = 0; i < values.size(); i++) {
                    args[i] = values[i].get<std::uint64_t>();
                }

                std::string hex = request.value("data", "");

                if (hex.size() % 2 ||
                    hex.size() > std::size_t{2} * voyemu::graphics::rpc_payload_bytes) {
                    throw voyemu::VoyemuError("GRAPHICS", "Stdio payload extent");
                }

                std::vector<std::byte> bytes;

                for (std::size_t i = 0; i < hex.size(); i += 2) {
                    unsigned byte = 0;
                    auto [end, error] = std::from_chars(hex.data() + i, hex.data() + i + 2, byte,
                                                        16);

                    if (error != std::errc{} || end != hex.data() + i + 2) {
                        throw voyemu::VoyemuError("GRAPHICS", "Stdio payload hex");
                    }

                    bytes.push_back(std::byte(byte));
                }

                auto out = backend.dispatch(request.at("op").get<std::uint32_t>(), args, bytes);
                constexpr char digits[] = "0123456789abcdef";
                std::string payload;

                for (auto b : out.payload) {
                    auto v = std::to_integer<unsigned>(b);
                    payload += digits[v >> 4];
                    payload += digits[v & 15];
                }

                std::cout << nlohmann::json{{"result", out.result},
                                            {"payload", payload},
                                            {"name", out.name}}
                                     .dump()
                          << std::endl;
            }

            voyemu::logging::write(voyemu::logging::Level::info, "graphics", "producer.stopped",
                                   {{"address", options.address}, {"reason", "stdio_eof"}});

            return 0;
        }

        const auto deadline = bounded ? std::chrono::steady_clock::now() +
                                                std::chrono::seconds(seconds)
                                      : std::chrono::steady_clock::time_point::max();
        const auto stop_path = options.session / "stop";
        auto next_stop_check = std::chrono::steady_clock::time_point::min(),
             next_profile = next_stop_check;
        const char *reason = "timeout";

        while (std::chrono::steady_clock::now() < deadline && !backend.finished()) {
            const auto now = std::chrono::steady_clock::now();

            if (now >= next_stop_check) {
                if (std::filesystem::exists(stop_path)) {
                    reason = "stop";
                    break;
                }

                next_stop_check = now + std::chrono::milliseconds(100);
            }

            if (!backend.service()) {
                std::this_thread::sleep_for(std::chrono::microseconds(backend.idle_wait_us()));
            }

            if (options.profile_rpc && now >= next_profile) {
                const auto sampled = std::chrono::steady_clock::now();
                next_profile = sampled + std::chrono::milliseconds(500);
                const auto profile = backend.rpc_profile();

                if (profile.first_received_ns) {
                    // One combined wire+api record of a long run outgrows the 8192-byte native
                    // record bound (core/logging.cpp), and the sample that throws kills the worker
                    // mid-run. The entries are emitted in chunks that respect a fixed item budget,
                    // which keeps a long run profiled instead of dying on its own log.
                    nlohmann::json wire = nlohmann::json::array(), api = nlohmann::json::array();

                    for (const auto &entry : profile.wire) {
                        wire.push_back({{"opcode", entry.opcode},
                                        {"calls", entry.calls},
                                        {"faults", entry.faults},
                                        {"dispatch_ns", entry.dispatch_ns},
                                        {"max_dispatch_ns", entry.max_dispatch_ns}});
                    }

                    for (const auto &[opcode, count] : profile.api) {
                        api.push_back({{"opcode", opcode}, {"calls", count}});
                    }

                    const nlohmann::json scalars = {
                            {"producer_address", options.address},
                            {"sample_monotonic_ns",
                             std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     sampled.time_since_epoch())
                                     .count()},
                            {"first_received_ns", profile.first_received_ns},
                            {"last_acknowledged_ns", profile.last_acknowledged_ns},
                            {"unknown_api_calls", profile.unknown_api_calls}};
                    constexpr std::size_t item_budget = 6000;
                    auto emit = [&](const char *message, const char *key,
                                    const nlohmann::json &items) {
                        std::vector<std::size_t> bounds;
                        std::size_t at = 0;

                        do {
                            std::size_t used = 2;

                            while (at < items.size()) {
                                const auto size = items[at].dump().size();

                                if (used > 2 && used + size + 1 > item_budget) {
                                    break;
                                }

                                used += size + 1;
                                ++at;
                            }

                            bounds.push_back(at);
                        } while (at < items.size());

                        for (std::size_t k = 0; k < bounds.size(); k++) {
                            nlohmann::json chunk = nlohmann::json::array();

                            for (std::size_t i = k ? bounds[k - 1] : 0; i < bounds[k]; i++) {
                                chunk.push_back(items[i]);
                            }

                            auto fields = scalars;
                            fields[key] = chunk;
                            fields["chunk"] = k;
                            fields["chunks"] = bounds.size();
                            voyemu::logging::write(voyemu::logging::Level::debug, "graphics",
                                                   message, fields);
                        }
                    };
                    emit("rpc.profile", "wire", wire);
                    emit("rpc.api-profile", "api", api);
                }
            }
        }

        const auto stats = backend.service_stats();

        if (backend.finished()) {
            reason = "retired";
        }

        voyemu::logging::write(voyemu::logging::Level::info, "graphics", "producer.stopped",
                               {{"address", options.address},
                                {"reason", reason},
                                {"polls", stats.polls},
                                {"identity_checks", stats.identity_checks},
                                {"idle_wait_us", backend.idle_wait_us()}});

        return 0;
    } catch (const voyemu::graphics::RpcChannelInvalidated &e) {
        try {
            voyemu::graphics::write_invalidated_channel_disposition(options);
            voyemu::logging::write(voyemu::logging::Level::error, "graphics", "producer.stopped",
                                   {{"address", options.address},
                                    {"reason", "rpc_channel_invalidated"},
                                    {"error", e.what()}});

            return voyemu::graphics::rpc_channel_invalidated_exit_code;
        } catch (const std::exception &disposition_error) {
            voyemu::logging::write(voyemu::logging::Level::error, "graphics", "producer.stopped",
                                   {{"address", options.address},
                                    {"reason", "error"},
                                    {"error", disposition_error.what()}});

            return 1;
        }
    } catch (const std::exception &e) {
        voyemu::logging::write(
                voyemu::logging::Level::error, "graphics", "producer.stopped",
                {{"address", options.address}, {"reason", "error"}, {"error", e.what()}});

        return 1;
    }
}
