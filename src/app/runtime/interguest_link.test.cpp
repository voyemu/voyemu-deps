#include "interguest_link.hpp"
#include "qnx_run_budget.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
using voyemu::host_qnx_engine;
using voyemu::InterguestLink;
using voyemu::qnx_guest_deadline;

void wait(const std::function<bool()> &f) {
    for (int i = 0; i < 2000; ++i) {
        if (f()) {
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    assert(false);
}

void fail(const std::function<void()> &f) {
    bool failed = false;

    try {
        f();
    } catch (const std::exception &) {
        failed = true;
    }

    assert(failed);
}

// Returns the error code prefix; asserts that the action failed.
std::string code(const std::function<void()> &f) {
    try {
        f();
    } catch (const std::exception &error) {
        const std::string message = error.what();

        return message.substr(0, message.find(':'));
    }

    assert(false);

    return {};
}

int peer(unsigned port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);

    assert(s >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(s, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0);
    timeval tv{2, 0};
    assert(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);

    return s;
}

int main(int argc, char **argv) try {
    assert(argc == 2);
    const std::filesystem::path root = std::string(argv[1]) + "-" + std::to_string(getpid());
    const std::string token = "0123456789abcdef0123456789abcdef";
    const auto dir = root / token;
    std::filesystem::create_directories(dir);
    const auto ram = dir / "ram";
    int fd = open(ram.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, 0x200000000ULL) == 0);
    auto *p = static_cast<unsigned char *>(
            mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(p != MAP_FAILED);
    auto *mapping = p;
    p += 4096;
    auto *w = reinterpret_cast<std::uint32_t *>(p);
    w[0] = 0x54454e51;
    w[1] = 1;
    std::array<std::uint32_t, 8> ring{};
    ring[0] = 0xbeefeeee;
    ring[1] = 16352;
    const std::string marker = "QNET READY pid=42 paddr=0x40001000 length=4096 version=1\r\n";
    ring[2] = marker.size();
    assert(pwrite(fd, ring.data(), 32, 0x10010000) == 32);
    assert(pwrite(fd, marker.data(), marker.size(), 0x10010020) ==
           static_cast<ssize_t>(marker.size()));
    InterguestLink link;
    fail([&] {
        link.start(dir, "owned-test");
    });
    fail([&] {
        link.start(dir, "0123456789ABCDEF0123456789ABCDEF");
    });
    fail([&] {
        link.start(root, token);
    });
    assert(chmod(dir.c_str(), 0777) == 0);
    fail([&] {
        link.start(dir, token);
    });
    assert(chmod(dir.c_str(), 0700) == 0);
    auto port = link.start(dir, token);
    link.bind_android(static_cast<std::uint64_t>(getpid()));
    assert(chmod(ram.c_str(), 0620) == 0);
    fail([&] {
        link.bind_ram(ram);
    });
    assert(chmod(ram.c_str(), 0600) == 0);
    fail([&] {
        link.bind_ram(dir / "../outside");
    });
    fail([&] {
        link.bind_ram(ram, 0x50011000);
    });
    std::filesystem::create_hard_link(ram, dir / "alias");
    fail([&] {
        link.bind_ram(ram);
    });
    std::filesystem::remove(dir / "alias");
    std::filesystem::create_symlink(ram.filename(), dir / "symlink");
    fail([&] {
        link.bind_ram(dir / "symlink");
    });
    std::filesystem::remove(dir / "symlink");
    link.bind_ram(ram);
    wait([&] {
        return link.status().ready;
    });
    assert(w[12] == 1 && w[13] == 0);
    const auto expected = link.status().utc_expected;
    assert(expected >= 946684800ULL && expected < 4102444800ULL);
    assert(*reinterpret_cast<std::uint64_t *>(p + 40) == expected);
    w[15] = static_cast<std::uint32_t>(expected);
    w[14] = 0;
    __atomic_store_n(&w[13], 1, __ATOMIC_RELEASE);
    wait([&] {
        return link.status().utc_complete;
    });
    assert(link.status().utc_actual == expected && link.status().utc_status == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(w[12] == 1);
    int s = peer(port);
    wait([&] {
        return link.status().connected;
    });
    // Fragmented socket header and body cannot publish a partial guest packet.
    const unsigned char header[4] = {0, 0, 0, 60};
    assert(send(s, header, 2, 0) == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(__atomic_load_n(&w[3], __ATOMIC_ACQUIRE) == 0);
    assert(send(s, header + 2, 2, 0) == 2);
    unsigned char frame[60];
    std::memset(frame, 0xab, sizeof frame);
    assert(send(s, frame, 13, 0) == 13);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(__atomic_load_n(&w[3], __ATOMIC_ACQUIRE) == 0);
    assert(send(s, frame + 13, 47, 0) == 47);
    wait([&] {
        return __atomic_load_n(&w[3], __ATOMIC_ACQUIRE) == 60;
    });
    assert(std::memcmp(p + 1600, frame, 60) == 0 && w[5] == 1);
    // Publishing a receive mailbox is not a guest TAP-write receipt.
    wait([&] {
        return link.status().received == 1;
    });
    assert(link.guest_counters().received == 0);
    w[9] = 1;
    __atomic_store_n(&w[3], 0, __ATOMIC_RELEASE);
    wait([&] {
        return link.guest_counters().received == 1;
    });
    std::memcpy(p + 64, frame, 60);
    w[4] = 1;
    __atomic_store_n(&w[2], 60, __ATOMIC_RELEASE);
    unsigned char wire[64];
    unsigned n = 0;

    while (n < 64) {
        int got = recv(s, wire + n, 64 - n, 0);
        assert(got > 0);
        n += got;
    }

    assert(std::memcmp(wire, header, 4) == 0 && std::memcmp(wire + 4, frame, 60) == 0);
    wait([&] {
        return link.status().sent == 1;
    });
    assert(link.status().received == 1);
    close(s);
    wait([&] {
        return !link.status().connected;
    });
    s = peer(port);
    wait([&] {
        return link.status().connected;
    });
    assert(link.status().reconnects == 2);
    // A terminal QNX relay must not retire the Android socket or poison its next RAM generation.
    const auto terminal_generation = link.status().generation;
    __atomic_store_n(&w[6], 1, __ATOMIC_RELEASE);
    wait([&] {
        return !link.status().fault.empty();
    });
    fail([&] {
        link.check();
    });
    assert(link.guest_counters().sent == 0 && link.guest_counters().received == 0);
    link.clear_ram();
    assert(link.status().port == port && link.status().connected && !link.status().ready &&
           !link.status().utc_complete && link.status().fault.empty());
    std::memset(p, 0, 4096);
    w[0] = 0x54454e51;
    w[1] = 1;
    link.bind_ram(ram);
    wait([&] {
        return link.status().ready;
    });
    assert(link.status().generation == terminal_generation + 2 && link.status().port == port &&
           link.status().connected && link.status().reconnects == 2 && !link.status().utc_complete);
    const auto restarted_epoch = link.status().utc_expected;
    w[15] = static_cast<std::uint32_t>(restarted_epoch);
    __atomic_store_n(&w[13], 1, __ATOMIC_RELEASE);
    wait([&] {
        return link.status().utc_complete;
    });
    assert(send(s, header, 4, 0) == 4 && send(s, frame, 60, 0) == 60);
    wait([&] {
        return __atomic_load_n(&w[3], __ATOMIC_ACQUIRE) == 60;
    });
    assert(std::memcmp(p + 1600, frame, 60) == 0 && w[5] == 1);
    std::memcpy(p + 64, frame, 60);
    w[4] = 1;
    __atomic_store_n(&w[2], 60, __ATOMIC_RELEASE);
    n = 0;

    while (n < 64) {
        int got = recv(s, wire + n, 64 - n, 0);
        assert(got > 0);
        n += got;
    }

    assert(std::memcmp(wire, header, 4) == 0 && std::memcmp(wire + 4, frame, 60) == 0);
    link.check();
    close(s);
    link.clear_ram();
    assert(!link.status().ready && !link.status().utc_complete && !link.guest_counters().received &&
           !link.guest_counters().sent);
    std::memset(p + 40, 0, 24);
    link.bind_ram(ram);
    wait([&] {
        return link.status().ready;
    });
    assert(link.status().generation == 5 && w[12] == 1 && !link.status().utc_complete);
    // Bounded flush preserves the guest on timeout and accepts only a successful release ACK.
    // Once a request is published the guest may be quiescing, so the outcome is unknown.
    assert(code([&] {
               link.flush(0);
           }) == "INTERGUEST_FLUSH_UNKNOWN");
    assert(w[784] == 1 && w[785] == 0 && w[6] == 0);
    assert(code([&] {
               link.flush(-1);
           }) == "INTERGUEST_FLUSH");
    // The deadline covers at most 30 s of HVF guest sync, scaled for a slower engine.
    assert(code([&] {
               link.flush(static_cast<int>(
                       qnx_guest_deadline(host_qnx_engine, std::chrono::milliseconds(30000))
                               .count() +
                       1));
           }) == "INTERGUEST_FLUSH");
    // A stale success cannot authorize a new attempt; the second attempt times out.
    w[786] = 0;
    __atomic_store_n(&w[785], 1, __ATOMIC_RELEASE);
    assert(code([&] {
               link.flush(0);
           }) == "INTERGUEST_FLUSH_UNKNOWN");
    assert(w[784] == 2 && w[6] == 0);
    std::thread guest([&] {
        wait([&] {
            return __atomic_load_n(&w[784], __ATOMIC_ACQUIRE) == 3;
        });
        w[786] = 0;
        __atomic_store_n(&w[785], 3, __ATOMIC_RELEASE);
    });
    link.flush(1000);
    guest.join();
    assert(w[784] == 3 && w[785] == 3);
    std::thread failed_guest([&] {
        wait([&] {
            return __atomic_load_n(&w[784], __ATOMIC_ACQUIRE) == 4;
        });
        w[786] = static_cast<std::uint32_t>(-2);
        __atomic_store_n(&w[785], 4, __ATOMIC_RELEASE);
    });
    // A failed quiesce ACK may follow a partial remount in the same generation.
    assert(code([&] {
               link.flush(1000);
           }) == "INTERGUEST_FLUSH_UNKNOWN");
    failed_guest.join();
    assert(w[6] == 0);
    w[785] = 6;
    assert(code([&] {
               link.flush(10);
           }) == "INTERGUEST_FLUSH");
    assert(w[784] == 4);
    w[785] = 4;
    // Replacing a pathname must not retain access to an old RAM incarnation.
    std::filesystem::rename(ram, dir / "old-ram");
    {
        std::ofstream replacement(ram);
        replacement << "replacement";
    }
    wait([&] {
        return !link.status().fault.empty();
    });
    fail([&] {
        link.check();
    });
    assert(code([&] {
               link.flush(10);
           }) == "INTERGUEST_FLUSH");
    link.stop();
    assert(w[6] == 0);
    munmap(mapping, 16384);
    std::filesystem::remove(ram);
    std::filesystem::rename(dir / "old-ram", ram);

    // Exactly one CR is accepted; extra trailing CR or text is rejected.
    for (const auto &suffix : {std::string("\r\r\n"), std::string(" extra\r\n")}) {
        const auto bad = std::string("QNET READY pid=42 paddr=0x40001000 length=4096 version=1") +
                         suffix;
        ring[2] = bad.size();
        assert(pwrite(fd, ring.data(), 32, 0x10010000) == 32);
        assert(pwrite(fd, bad.data(), bad.size(), 0x10010020) == static_cast<ssize_t>(bad.size()));
        link.start(dir, token);
        link.bind_ram(ram);
        wait([&] {
            return !link.status().fault.empty();
        });
        assert(link.status().fault.starts_with("INTERGUEST_RING:"));
        link.stop();
    }

    ring[2] = marker.size();
    assert(pwrite(fd, ring.data(), 32, 0x10010000) == 32);
    assert(pwrite(fd, marker.data(), marker.size(), 0x10010020) ==
           static_cast<ssize_t>(marker.size()));
    // Actual host mailbox rejects failed or fabricated clock acknowledgements.
    mapping = static_cast<unsigned char *>(
            mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(mapping != MAP_FAILED);
    w = reinterpret_cast<std::uint32_t *>(mapping + 4096);

    for (int scenario = 0; scenario < 3; ++scenario) {
        std::memset(w, 0, 4096);
        w[0] = 0x54454e51;
        w[1] = 1;
        link.start(dir, token);
        link.bind_ram(ram);
        wait([&] {
            return link.status().ready;
        });
        const auto epoch = link.status().utc_expected;
        w[14] = scenario == 0 ? static_cast<std::uint32_t>(-2) : 0;
        w[15] = static_cast<std::uint32_t>(epoch + (scenario == 1 ? 3 : 0));
        __atomic_store_n(&w[13], scenario == 2 ? 2U : 1U, __ATOMIC_RELEASE);
        wait([&] {
            return !link.status().fault.empty();
        });
        assert(link.status().fault.starts_with("INTERGUEST_UTC:"));

        if (scenario < 2) {
            assert(link.status().utc_complete && link.status().utc_actual == w[15]);
        }

        link.stop();
    }

    std::memset(w, 0, 4096);
    w[0] = 0x54454e51;
    w[1] = 1;
    munmap(mapping, 16384);
    // Free A11 UART discovery does not require the A9 RAM ring and keeps append/incarnation
    // fences.
    const auto uart_ram = dir / "qnx-ram.bin",
               uart = std::filesystem::absolute(dir / "qnx-serial.log");
    std::filesystem::rename(ram, uart_ram);
    ring[0] = 0;
    assert(pwrite(fd, ring.data(), 32, 0x10010000) == 32);
    auto reset_page = [&] {
        std::array<unsigned char, 4096> bytes{};
        auto *words = reinterpret_cast<std::uint32_t *>(bytes.data());
        words[0] = 0x54454e51;
        words[1] = 1;
        assert(pwrite(fd, bytes.data(), bytes.size(), 4096) == 4096);
    };
    auto serial_write = [&](const std::string &value, bool append = false) {
        std::ofstream out(uart, append ? std::ios::app : std::ios::trunc);
        out << value;
        out.close();
        assert(chmod(uart.c_str(), 0600) == 0);
    };
    reset_page();
    serial_write(std::string(20000, 'x') + "\nnoise " + marker + marker.substr(0, 20));
    link.start(dir, token);
    fail([&] {
        link.bind_ram(uart_ram, 0x50010000, dir / "other.log");
    });
    std::filesystem::create_hard_link(uart, dir / "uart-alias");
    fail([&] {
        link.bind_ram(uart_ram, 0x50010000, uart);
    });
    std::filesystem::remove(dir / "uart-alias");
    link.bind_ram(uart_ram, 0x50010000, uart);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!link.status().ready && link.status().fault.empty());
    serial_write(marker.substr(20), true);
    wait([&] {
        return link.status().ready;
    });
    assert(link.status().utc_expected >= 946684800ULL && !link.status().utc_complete);
    fail([&] {
        link.flush(0);
    });
    std::filesystem::rename(uart, dir / "old-uart");
    serial_write(marker);
    wait([&] {
        return !link.status().fault.empty();
    });
    assert(link.status().fault.starts_with("INTERGUEST_UART:"));
    link.stop();
    std::filesystem::remove(dir / "old-uart");

    for (const auto &invalid :
         {std::string("QNET READY pid=0 paddr=0x40001000 length=4096 version=1\n"),
          std::string("QNET READY pid=42 paddr=0x40001001 length=4096 version=1\n"),
          std::string("QNET READY pid=42 paddr=0x240000000 length=4096 version=1\n"),
          std::string("QNET READY pid=42 paddr=0x40001000 length=4096 version=1 junk\n"),
          marker + marker}) {
        reset_page();
        serial_write(invalid);
        link.start(dir, token);
        link.bind_ram(uart_ram, 0x50010000, uart);
        wait([&] {
            return !link.status().fault.empty();
        });
        assert(!link.status().ready);
        link.stop();
    }

    reset_page();
    serial_write(marker);
    link.start(dir, token);
    link.bind_ram(uart_ram, 0x50010000, uart);
    wait([&] {
        return link.status().ready;
    });
    assert(truncate(uart.c_str(), 0) == 0);
    wait([&] {
        return !link.status().fault.empty();
    });
    link.stop();
    // Passion backing includes OEM storage; only the boot-mapped prefix may own a mailbox.
    const auto passion_geometry = InterguestLink::RamGeometry::passion;
    reset_page();
    serial_write(marker);
    assert(ftruncate(fd, 11713ULL << 20) == 0);
    link.start(dir, token);
    fail([&] {
        link.bind_ram(uart_ram, 0x50010000, uart);
    });
    link.bind_ram(uart_ram, 0x50010000, uart, passion_geometry);
    wait([&] {
        return link.status().ready;
    });
    link.stop();
    // The physical window is narrower than the backing file; reject its first excluded page.
    reset_page();
    serial_write("QNET READY pid=42 paddr=0x180000000 length=4096 version=1\n");
    link.start(dir, token);
    link.bind_ram(uart_ram, 0x50010000, uart, passion_geometry);
    wait([&] {
        return !link.status().fault.empty();
    });
    assert(!link.status().ready);
    link.stop();
    // Geometry must be explicit and UART-bound; exact extent and ownership fences remain.
    link.start(dir, token);
    fail([&] {
        link.bind_ram(uart_ram, 0x50010000, {}, passion_geometry);
    });
    assert(ftruncate(fd, (11713ULL << 20) - 4096) == 0);
    fail([&] {
        link.bind_ram(uart_ram, 0x50010000, uart, passion_geometry);
    });
    link.stop();
    assert(ftruncate(fd, 0x200000000ULL) == 0);
    std::filesystem::remove(uart);
    std::filesystem::rename(uart_ram, ram);
    ring[0] = 0xbeefeeee;
    assert(pwrite(fd, ring.data(), 32, 0x10010000) == 32);
    {
        // The vehicle gateway answers guest frames without an Android peer: guest TX is consumed
        // and its ARP reply and cyclic offers arrive in the guest RX mailbox.
        auto *g = static_cast<unsigned char *>(
                mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        assert(g != MAP_FAILED);
        auto *gp = g + 4096;
        auto *gw = reinterpret_cast<std::uint32_t *>(gp);
        std::memset(gp, 0, 4096);
        gw[0] = 0x54454e51;
        gw[1] = 1;
        InterguestLink gateway_link;
        fail([&] {
            gateway_link.apply_vehicle_state(voyemu::Json{{"raw_oem", voyemu::Json::object()}});
        });
        gateway_link.start(dir, token);
        gateway_link.attach_gateway(voyemu::Json::parse(R"({"schema":1,"services":[
            {"name":"VehicleDrivingInfo","vlan":12,"service":8192,"instance":1,"major":1,"events":[
              {"name":"Notify_f_VehicleDrivingInform","event":32769,"eventgroups":[8192],"bytes":24,
               "e2e":true,"fields":[{"name":"VehicleSpeed","offset":20,"bytes":4}]}]}]})"));
        assert(gateway_link.apply_vehicle_state(
                       voyemu::Json{{"map_sha256", voyemu::passion_map_sha256},
                                    {"raw_oem", {{"VehicleSpeed", 1}}}}) == 1);
        gateway_link.bind_ram(ram);
        wait([&] {
            return gateway_link.status().ready;
        });
        const unsigned char arp[46] = {
                0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2, 0, 0, 0, 0,   0x22, 0x81, 0, 0, 12,
                0x08, 0x06, 0,    1,    8,    0,    6, 4, 0, 1, 2,   0,    0,    0, 0, 0x22,
                172,  16,   112,  30,   0,    0,    0, 0, 0, 0, 172, 16,   112,  10};
        std::memcpy(gp + 64, arp, sizeof arp);
        gw[4] = 1;
        __atomic_store_n(&gw[2], sizeof arp, __ATOMIC_RELEASE);
        wait([&] {
            return __atomic_load_n(&gw[2], __ATOMIC_ACQUIRE) == 0;
        });
        bool reply = false, offer = false;

        for (int i = 0; i < 3 && !(reply && offer); ++i) {
            wait([&] {
                return __atomic_load_n(&gw[3], __ATOMIC_ACQUIRE) != 0;
            });
            const auto *rx = gp + 1600;
            reply = reply || (rx[16] == 0x08 && rx[17] == 0x06 && rx[25] == 2 && rx[6] == 0x02 &&
                              rx[7] == 0xdf && rx[8] == 0x53);
            offer = offer || (rx[0] == 0x01 && rx[1] == 0x00 && rx[2] == 0x5e);
            __atomic_store_n(&gw[3], 0, __ATOMIC_RELEASE);
        }

        assert(reply && offer && gateway_link.status().sent == 1 &&
               gateway_link.status().fault.empty());
        gateway_link.stop();
        munmap(g, 16384);
    }
    // Malformed mailbox is surfaced, while stop joins before releasing RAM.
    close(fd);
    fd = open(ram.c_str(), O_RDWR);
    mapping = static_cast<unsigned char *>(
            mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(mapping != MAP_FAILED);
    w = reinterpret_cast<std::uint32_t *>(mapping + 4096);
    w[2] = 7;
    port = link.start(dir, token);
    link.bind_ram(ram);
    wait([&] {
        return !link.status().fault.empty();
    });
    fail([&] {
        link.check();
    });
    link.stop();
    assert(w[6] == 1);
    munmap(mapping, 16384);
    close(fd);
    std::filesystem::remove(ram);
    std::filesystem::remove(dir);
    std::filesystem::remove(root);
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
