#include "capture_png.hpp"
#include "frame_transport.hpp"
#include "qnx_capture.hpp"
#include "session.hpp"
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
using namespace voyemu;
using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t interrupted = 0;

static void interrupt(int) {
    interrupted = 1;
}

struct Sample {
    rusage_info_v2 usage{};
    std::uint64_t wall{};
};

static Sample sample(int pid) {
    Sample s;

    if (proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t *>(&s.usage))) {
        throw std::runtime_error("QNX_CAPTURE_CPU: owned usage unavailable");
    }

    s.wall = mach_absolute_time();

    return s;
}

static std::uint64_t cpu(const Sample &s) {
    return s.usage.ri_user_time + s.usage.ri_system_time;
}

static double percent(const Sample &a, const Sample &b) {
    if (a.usage.ri_proc_start_abstime != b.usage.ri_proc_start_abstime || b.wall <= a.wall ||
        cpu(b) < cpu(a)) {
        throw std::runtime_error("QNX_CAPTURE_CPU: process epoch changed");
    }

    return 100.0 * double(cpu(b) - cpu(a)) / double(b.wall - a.wall);
}

static double seconds(const rusage &s) {
    return s.ru_utime.tv_sec + s.ru_stime.tv_sec + (s.ru_utime.tv_usec + s.ru_stime.tv_usec) / 1e6;
}

static void png(const std::filesystem::path &path, const frames::Frame &frame) {
    voyemu::output::write_artifact(
            path,
            voyemu::output::png_frame(
                    {reinterpret_cast<const std::uint8_t *>(frame.bgra.data()), frame.bgra.size()},
                    frame.width, frame.height, true));
}

int main(int argc, char **argv) {
    try {
        std::signal(SIGTERM, interrupt);
        std::signal(SIGINT, interrupt);

        if (argc == 4 && std::string_view(argv[1]) == "--roms") {
            const auto specification = read_private_json(argv[2]);
            const std::filesystem::path path = argv[3];

            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)) ||
                std::filesystem::file_size(path) > 16384) {
                throw std::runtime_error("QNX_CAPTURE_ROMS: bounded ordinary observation required");
            }

            std::ifstream file(path);
            std::string observation{std::istreambuf_iterator<char>(file), {}};
            std::cout << qnx_checkpoint_boot_roms(observation, specification).dump() << '\n';

            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--verify") {
            const auto specification = read_private_json(argv[2]);
            const auto cache = std::filesystem::path(
                    specification.at("quickboot").at("directory").get<std::string>());
            const auto verified = inspect_qnx_checkpoint(specification,
                                                         cache.parent_path().filename().string());

            if (!verified.resume || !remember_qnx_checkpoint_verification(verified)) {
                throw std::runtime_error("QNX_CAPTURE_VERIFY: checkpoint content/epoch rejected");
            }

            std::cout << Json{{"phase", "checkpoint_verified"},
                              {"verification", verified.verification},
                              {"external_ram", verified.external_ram},
                              {"files", verified.verified_files.at("files").size()}}
                                 .dump()
                      << '\n';

            return 0;
        }

        if (argc == 2 && std::string_view(argv[1]) == "--calibrate") {
            std::cout << Json{{"phase", "calibration_started"}}.dump() << '\n' << std::flush;
            rusage a{}, b{};
            getrusage(RUSAGE_SELF, &a);
            auto first = sample(getpid());
            auto start = Clock::now();
            std::uint64_t sink = 1;

            while (!interrupted && Clock::now() - start < std::chrono::milliseconds(250)) {
                sink = sink * 6364136223846793005ULL + 1;
            }

            if (interrupted) {
                return 130;
            }

            auto last = sample(getpid());
            getrusage(RUSAGE_SELF, &b);
            mach_timebase_info_data_t base{};
            mach_timebase_info(&base);
            double mach = double(cpu(last) - cpu(first)) * base.numer / base.denom / 1e9,
                   posix = seconds(b) - seconds(a);

            if (mach <= 0 || std::abs(mach - posix) > .02) {
                throw std::runtime_error("QNX_CAPTURE_CPU: Mach/POSIX calibration mismatch");
            }

            std::cout << Json{{"cpu_percent", percent(first, last)},
                              {"mach_seconds", mach},
                              {"posix_seconds", posix},
                              {"sink", sink}}
                                 .dump()
                      << '\n';

            return 0;
        }

        if (argc != 5) {
            throw std::runtime_error(
                    "QNX_CAPTURE_ARGUMENT: CONFIG SESSION OUTPUT RECORD_WRAPPER_OR_MEASURE required");
        }

        const bool measure_only = std::string_view(argv[4]) == "--measure";
        const std::filesystem::path directory = argv[2], output = argv[3], wrapper = argv[4];
        const auto root = qnx_capture_project(directory, output), token = directory.filename();

        if (std::filesystem::canonical(root) != root || std::filesystem::exists(directory) ||
            std::filesystem::exists(output) ||
            (!measure_only && (std::filesystem::is_symlink(wrapper) ||
                               !std::filesystem::is_regular_file(wrapper)))) {
            throw std::runtime_error("QNX_CAPTURE_PATH: fresh ordinary owned paths required");
        }

        auto cold = read_private_json(argv[1]);

        if (!measure_only && cold.contains("quickboot")) {
            throw std::runtime_error("QNX_CAPTURE_CONFIG: explicit cold specification required");
        }

        const auto profile = directory.parent_path().parent_path().filename().string();
        const auto key = qnx_checkpoint_key(cold, profile);
        const auto graphics = cold.at("graphics").at("executable").get<std::string>();

        if (std::none_of(cold.at("pins").begin(), cold.at("pins").end(), [&](const Json &pin) {
                return pin.at("path") == graphics && pin.at("sha256") == sha256_file(graphics);
            })) {
            throw std::runtime_error("QNX_CAPTURE_CONFIG: original renderer pin required");
        }

        if (!measure_only) {
            const auto journal = std::getenv("VOYEMU_QNX_CAPTURE_JOURNAL");

            if (!journal) {
                throw std::runtime_error("QNX_CAPTURE_PATH: explicit journal required");
            }

            qnx_capture_journal(root, profile, journal);

            if (std::filesystem::is_symlink(journal) || !std::filesystem::is_directory(journal) ||
                !std::filesystem::is_empty(journal)) {
                throw std::runtime_error("QNX_CAPTURE_PATH: fresh ordinary journal required");
            }

            const auto local_wrapper = wrapper.lexically_relative(root / ".local/build");

            if (!wrapper.is_absolute() || wrapper.lexically_normal() != wrapper ||
                local_wrapper.empty() || *local_wrapper.begin() == "..") {
                throw std::runtime_error("QNX_CAPTURE_PATH: project source-built wrapper required");
            }

            setenv("VOYEMU_QNX_CAPTURE_GRAPHICS", graphics.c_str(), 1);
            cold["graphics"]["executable"] = wrapper.string();
            cold["pins"].push_back({{"path", wrapper.string()}, {"sha256", sha256_file(wrapper)}});
        }

        ensure_private_directory(output);
        const auto definition = root / "profiles" / profile / "profile.json";
        const auto profile_pin = sha256_file(definition);
        const auto seed = qnx_capture_initial_state(read_private_json(definition));
        ensure_private_directory(directory);
        write_private_json(directory / "vehicle-state.json", seed, vehicle_state_bytes);
        write_private_json(output / "seed.json",
                           {{"profile_sha256", profile_pin}, {"state", seed}});
        auto start = Clock::now();
        auto elapsed = [&] {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        };
        Session session(directory, token.string());
        session.attach_cluster(cold, 100);

        if (measure_only && cold.contains("quickboot") &&
            session.handle({{"op", "status"}})
                            .at("deliveries")
                            .at("qnx")
                            .at("quickboot")
                            .at("mode") != "restore_requested") {
            throw std::runtime_error("QNX_MEASURE_CACHE: requested cache was not selected");
        }

        const auto attach_ms = elapsed();
        frames::Reader reader(directory / "frame.bgra");
        frames::Frame frame;
        std::uint64_t sequence = 0, first_sequence = 0, steady_sequence = 0;
        double first_ms = -1, ack_ms = -1, scene_ms = -1;
        int pid = 0;
        Sample begin{};
        Clock::time_point measure{}, finished{};
        bool complete = false, updated = false;

        while (!interrupted && elapsed() < 90000) {
            if (std::filesystem::space(root).available < 20ULL * 1024 * 1024 * 1024) {
                throw std::runtime_error("QNX_CAPTURE_SPACE: reserve reached");
            }

            session.tick();
            auto status = session.handle({{"op", "status"}});
            const auto qnx = status.at("deliveries").at("qnx");
            pid = status.at("qnx_pid").get<int>();

            if (qnx.value("status", std::string{}) == "exited" ||
                qnx.value("status", std::string{}) == "graphics_worker_exited") {
                throw std::runtime_error("QNX_CAPTURE_EXIT: owned QNX terminated");
            }

            if (reader.read_latest(frame, sequence)) {
                sequence = frame.sequence;

                if (first_ms < 0) {
                    first_ms = elapsed();
                    first_sequence = sequence;
                    png(output / "first-original.png", frame);
                }
            }

            const auto acknowledged =
                    qnx.value("vehicle", Json::object()).value("status", std::string{}) ==
                    "guest_acknowledged";

            if (acknowledged && ack_ms < 0) {
                ack_ms = elapsed();
            }

            if (acknowledged && sequence && scene_ms < 0) {
                auto lit = qnx_capture_scene_geometry(frame);

                if (*std::min_element(lit.begin(), lit.end()) > .04) {
                    scene_ms = elapsed();
                    png(output / "scene-original.png", frame);
                    measure = Clock::now() + std::chrono::seconds(measure_only ? 2 : 30);
                    write_private_json(output / "ready.json",
                                       {{"pid", pid},
                                        {"session", directory.string()},
                                        {"qmp", expand_session_path(cold.at("qmp"), directory,
                                                                    token.string())},
                                        {"production_key", key},
                                        {"scene_ms", scene_ms},
                                        {"quickboot", qnx.at("quickboot")},
                                        {"scene_lit_fractions", lit},
                                        {"original_frame_verified", true},
                                        {"visual_correctness_verified", false}});
                }
            }

            if (measure_only && scene_ms >= 0 && !updated) {
                const auto patch =
                        session.handle({{"op", "patch"}, {"values", {{"ivi_languageSet", 1}}}});

                if (!patch.value("ok", false)) {
                    throw std::runtime_error(
                            "QNX_MEASURE_INPUT: current language request rejected");
                }

                write_private_json(output / "input-applied.json",
                                   {{"values", {{"ivi_languageSet", 1}}},
                                    {"revision", patch.at("revision")},
                                    {"visible_effect_verified", false}});
                updated = true;
            }

            if (scene_ms >= 0 && !begin.wall && Clock::now() >= measure) {
                begin = sample(pid);
                measure = Clock::now();
                steady_sequence = sequence;
            }

            if (begin.wall && !complete && Clock::now() - measure >= std::chrono::seconds(10)) {
                const auto end = sample(pid);
                const double duration =
                        std::chrono::duration<double>(Clock::now() - measure).count();
                png(output / "steady-original.png", frame);
                write_private_json(
                        output / "steady.json",
                        {{"attach_ms", attach_ms},
                         {"first_original_frame_ms", first_ms},
                         {"first_sequence", first_sequence},
                         {"vehicle_ack_ms", ack_ms},
                         {"scene_geometry_ms", scene_ms},
                         {"cpu_percent", percent(begin, end)},
                         {"steady_seconds", duration},
                         {"export_sequence_rate_hz", double(sequence - steady_sequence) / duration},
                         {"rss_bytes", end.usage.ri_resident_size},
                         {"phys_footprint_bytes", end.usage.ri_phys_footprint},
                         {"scope", "QNX-only original frame transport; not visible window FPS"},
                         {"producers", qnx.at("producers")},
                         {"quickboot", qnx.at("quickboot")},
                         {"current_input_exercised", updated}});
                complete = true;
                finished = Clock::now();
                std::cout << Json{{"phase", "capture_steady"},
                                  {"scene_geometry_ms", scene_ms},
                                  {"cpu_percent", percent(begin, end)}}
                                     .dump()
                          << '\n'
                          << std::flush;
            }

            if (measure_only && complete) {
                break;
            }

            if (complete && (std::filesystem::exists(output / "finish") ||
                             Clock::now() - finished >= std::chrono::seconds(25))) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (sha256_file(definition) != profile_pin) {
            throw std::runtime_error("QNX_CAPTURE_SEED: source profile changed during capture");
        }

        session.stop();
        write_private_json(output / "terminal.json", session.handle({{"op", "status"}}));

        if (interrupted) {
            return 130;
        }

        if (!complete || std::filesystem::exists(directory / "qnx-ram.bin")) {
            throw std::runtime_error("QNX_CAPTURE_INCOMPLETE: steady scene or cleanup unavailable");
        }

        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';

        return 1;
    }
}
