#include "interguest_link.hpp"
#include "error.hpp"
#include "interguest_network.hpp"
#include "os/fs.hpp"
#include "qnx_run_budget.hpp"
#include "someip_gateway.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace voyemu {
namespace {
constexpr std::uint64_t base = 0x40000000ULL;
struct Page {
    std::uint32_t magic, version, txlen, rxlen, txseq, rxseq, stop, fault, txpackets, rxpackets;
    std::uint64_t utc_epoch;
    std::uint32_t utc_request, utc_ack;
    std::int32_t utc_status;
    std::uint32_t utc_actual;
    unsigned char tx[1536], rx[1536];
    std::uint32_t flush_request, flush_ack;
    std::int32_t flush_status;
    unsigned char padding[948];
};
static_assert(sizeof(Page) == 4096 && offsetof(Page, tx) == 64 && offsetof(Page, rx) == 1600 &&
              offsetof(Page, utc_epoch) == 40 && offsetof(Page, utc_request) == 48 &&
              offsetof(Page, utc_ack) == 52 && offsetof(Page, utc_status) == 56 &&
              offsetof(Page, utc_actual) == 60);
static_assert(offsetof(Page, flush_request) == 3136 && offsetof(Page, flush_ack) == 3140 &&
              offsetof(Page, flush_status) == 3144);

bool frame(std::uint32_t n) {
    return n >= 14 && n <= 1518;
}

static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);

std::uint32_t load(const std::uint32_t &v) {
    return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t &>(v))
            .load(std::memory_order_acquire);
}

void store(std::uint32_t &v, std::uint32_t n) {
    std::atomic_ref<std::uint32_t>(v).store(n, std::memory_order_release);
}
} // namespace
struct InterguestLink::Impl {
    mutable std::mutex mutex;
    InterguestNetwork network;
    std::unique_ptr<SomeipGateway> gateway;
    std::thread worker;
    std::atomic<bool> stopping = false;
    // Passion storage is inside the backing extent, outside the boot-mapped mailbox window.
    std::uint64_t ram_size = 0x200000000ULL, mailbox_end = 0x240000000ULL;
    GuestCounters guest;
    Status state;
    bool network_client = false;
    std::filesystem::path session, ram;
    std::uint64_t ring = 0;
    Page *page = nullptr;
    std::uint32_t txseq = 0, rxseq = 0;
    // Owned session UART of this incarnation, read append-only from a held handle.
    std::filesystem::path serial;
    std::uint64_t serial_cursor = 0;
    std::string serial_line;
    bool serial_discard = false;
#ifndef _WIN32
    void *mapping = MAP_FAILED;
    std::size_t mapping_size = 0;
    int fd = -1, serial_fd = -1;
    dev_t device = 0;
    ino_t inode = 0;
    struct stat serial_identity{};

    bool serial_bound() const {
        return serial_fd >= 0;
    }

    std::int64_t read_serial(char *data, std::size_t size) {
        return pread(serial_fd, data, size, static_cast<off_t>(serial_cursor));
    }

    void close_serial() {
        if (serial_fd >= 0) {
            close(serial_fd);
        }

        serial_fd = -1;
    }

    bool same_serial() {
        struct stat held{}, path{};

        return serial_fd >= 0 && fstat(serial_fd, &held) == 0 &&
               lstat(serial.c_str(), &path) == 0 && S_ISREG(path.st_mode) &&
               path.st_uid == getuid() && path.st_nlink == 1 && !(path.st_mode & 0022) &&
               held.st_dev == serial_identity.st_dev && held.st_ino == serial_identity.st_ino &&
               path.st_dev == held.st_dev && path.st_ino == held.st_ino && held.st_size >= 0 &&
               std::uint64_t(held.st_size) >= serial_cursor &&
               std::uint64_t(held.st_size) <= (16ULL << 20);
    }

    bool same_file() {
        struct stat a{}, b{};

        return fd >= 0 && fstat(fd, &a) == 0 && lstat(ram.c_str(), &b) == 0 && S_ISREG(b.st_mode) &&
               a.st_dev == device && a.st_ino == inode && b.st_dev == device && b.st_ino == inode &&
               a.st_nlink == 1 && a.st_uid == getuid() && !(a.st_mode & 0022) &&
               a.st_size == static_cast<off_t>(ram_size);
    }

    void release(bool signal) {
        release_serial();

        if (page) {
            if (signal && same_file() && load(page->magic) == 0x54454e51 &&
                load(page->version) == 1) {
                store(page->stop, 1);
            }

            munmap(mapping, mapping_size);
            mapping = MAP_FAILED;
            page = nullptr;
        }

        if (fd >= 0) {
            close(fd);
            fd = -1;
        }

        ram.clear();
        guest = {};
        state.ready = false;
        state.utc_expected = state.utc_actual = 0;
        state.utc_status = 0;
        state.utc_complete = false;
        txseq = rxseq = 0;
    }

    bool bound() const {
        return fd >= 0;
    }

    bool read_at(void *data, std::size_t size, std::uint64_t offset) {
        return pread(fd, data, size, static_cast<off_t>(offset)) == static_cast<ssize_t>(size);
    }

    void unmap_page() {
        munmap(mapping, mapping_size);
        mapping = MAP_FAILED;
        page = nullptr;
    }

    void map_page(std::uint64_t offset) {
        const auto granule = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
        const auto aligned = offset - offset % granule;
        mapping_size = static_cast<std::size_t>(offset - aligned + 4096);
        void *mapped = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                            static_cast<off_t>(aligned));

        if (mapped == MAP_FAILED) {
            throw VoyemuError("INTERGUEST_RAM", "page mapping failed");
        }

        mapping = mapped;
        page = reinterpret_cast<Page *>(static_cast<unsigned char *>(mapped) + (offset - aligned));
    }
#else
    HANDLE fd = INVALID_HANDLE_VALUE, mapping_file = nullptr, serial_file = INVALID_HANDLE_VALUE;
    void *mapping = nullptr;
    std::size_t mapping_size = 0;
    BY_HANDLE_FILE_INFORMATION identity{}, serial_identity{};

    static bool owned(HANDLE file) {
        HANDLE token = nullptr;

        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return false;
        }

        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> user(size);
        const bool valid = size && GetTokenInformation(token, TokenUser, user.data(), size, &size);
        CloseHandle(token);

        if (!valid) {
            return false;
        }

        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const auto result = GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                            &owner, nullptr, nullptr, nullptr, &descriptor);
        const bool same = result == ERROR_SUCCESS && owner &&
                          EqualSid(owner, reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid);

        if (descriptor) {
            LocalFree(descriptor);
        }

        return same;
    }

    static std::uint64_t extent(const BY_HANDLE_FILE_INFORMATION &info) {
        return (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    }

    // One owned link to a plain disk file, never a reparse point.
    static bool ordinary(HANDLE file, BY_HANDLE_FILE_INFORMATION &info) {
        return file != INVALID_HANDLE_VALUE && GetFileType(file) == FILE_TYPE_DISK &&
               GetFileInformationByHandle(file, &info) &&
               !(info.dwFileAttributes &
                 (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
               info.nNumberOfLinks == 1 && owned(file);
    }

    static bool regular(HANDLE file, BY_HANDLE_FILE_INFORMATION &info, std::uint64_t size) {
        return ordinary(file, info) && extent(info) == size;
    }

    static bool same(const BY_HANDLE_FILE_INFORMATION &a, const BY_HANDLE_FILE_INFORMATION &b) {
        return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
               a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
    }

    bool bound() const {
        return fd != INVALID_HANDLE_VALUE;
    }

    bool same_file() {
        BY_HANDLE_FILE_INFORMATION held{}, path{};

        if (!regular(fd, held, ram_size) || !same(held, identity)) {
            return false;
        }

        HANDLE current = CreateFileW(ram.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        const bool valid = regular(current, path, ram_size) && same(path, identity);

        if (current != INVALID_HANDLE_VALUE) {
            CloseHandle(current);
        }

        return valid;
    }

    bool read_at(void *data, std::size_t size, std::uint64_t offset) {
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(offset);
        DWORD count = 0;

        return offset <= ram_size - size && SetFilePointerEx(fd, at, nullptr, FILE_BEGIN) &&
               ReadFile(fd, data, static_cast<DWORD>(size), &count, nullptr) && count == size;
    }

    bool serial_bound() const {
        return serial_file != INVALID_HANDLE_VALUE;
    }

    std::int64_t read_serial(char *data, std::size_t size) {
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(serial_cursor);
        DWORD count = 0;

        if (!SetFilePointerEx(serial_file, at, nullptr, FILE_BEGIN) ||
            !ReadFile(serial_file, data, static_cast<DWORD>(size), &count, nullptr)) {
            return -1;
        }

        return count;
    }

    void close_serial() {
        if (serial_file != INVALID_HANDLE_VALUE) {
            CloseHandle(serial_file);
        }

        serial_file = INVALID_HANDLE_VALUE;
    }

    bool same_serial() {
        BY_HANDLE_FILE_INFORMATION held{}, path{};

        if (!ordinary(serial_file, held) || !same(held, serial_identity) ||
            extent(held) < serial_cursor || extent(held) > (16ULL << 20)) {
            return false;
        }

        HANDLE current = CreateFileW(serial.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        const bool valid = ordinary(current, path) && same(path, serial_identity);

        if (current != INVALID_HANDLE_VALUE) {
            CloseHandle(current);
        }

        return valid;
    }

    void unmap_page() {
        if (mapping) {
            UnmapViewOfFile(mapping);
        }

        mapping = nullptr;
        page = nullptr;

        if (mapping_file) {
            CloseHandle(mapping_file);
        }

        mapping_file = nullptr;
    }

    void map_page(std::uint64_t offset) {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto granule = std::uint64_t(system.dwAllocationGranularity);

        if (!granule || granule > 65536) {
            throw VoyemuError("INTERGUEST_RAM", "unsupported mapping granularity");
        }

        const auto aligned = offset - offset % granule;
        mapping_size = static_cast<std::size_t>(offset - aligned + 4096);
        mapping_file = CreateFileMappingW(fd, nullptr, PAGE_READWRITE, 0, 0, nullptr);

        if (!mapping_file) {
            throw VoyemuError("INTERGUEST_RAM", "mapping handle failed");
        }

        mapping = MapViewOfFile(mapping_file, FILE_MAP_READ | FILE_MAP_WRITE,
                                static_cast<DWORD>(aligned >> 32), static_cast<DWORD>(aligned),
                                mapping_size);

        if (!mapping) {
            CloseHandle(mapping_file);
            mapping_file = nullptr;
            throw VoyemuError("INTERGUEST_RAM", "bounded page mapping failed");
        }

        page = reinterpret_cast<Page *>(static_cast<unsigned char *>(mapping) + (offset - aligned));
    }

    void release(bool signal) {
        release_serial();

        if (page && signal && same_file() && load(page->magic) == 0x54454e51 &&
            load(page->version) == 1) {
            store(page->stop, 1);
        }

        unmap_page();

        if (fd != INVALID_HANDLE_VALUE) {
            CloseHandle(fd);
        }

        fd = INVALID_HANDLE_VALUE;
        ram.clear();
        guest = {};
        state.ready = false;
        state.utc_expected = state.utc_actual = 0;
        state.utc_status = 0;
        state.utc_complete = false;
        txseq = rxseq = 0;
    }
#endif

    void release_serial() {
        close_serial();
        serial.clear();
        serial_cursor = 0;
        serial_line.clear();
        serial_discard = false;
    }

    std::string uart_marker() {
        if (!same_serial()) {
            throw VoyemuError("INTERGUEST_UART", "source incarnation or extent changed");
        }

        std::array<char, 65536> bytes{};
        const auto count = read_serial(bytes.data(), bytes.size());

        if (count < 0) {
            throw VoyemuError("INTERGUEST_UART", "read failed");
        }

        serial_cursor += static_cast<std::uint64_t>(count);
        std::string found;

        for (std::int64_t i = 0; i < count; ++i) {
            const char c = bytes[std::size_t(i)];

            if (c == '\n') {
                if (!serial_discard && serial_line.starts_with("QNET READY ")) {
                    if (!found.empty()) {
                        throw VoyemuError("INTERGUEST_UART", "repeated relay announcement");
                    }

                    found = serial_line;
                }

                serial_line.clear();
                serial_discard = false;
            } else if (!serial_discard) {
                if (serial_line.size() == 256) {
                    if (serial_line.starts_with("QNET READY ")) {
                        throw VoyemuError("INTERGUEST_UART", "announcement byte bound");
                    }

                    serial_line.clear();
                    serial_discard = true;
                } else {
                    serial_line.push_back(c);
                }
            }
        }

        if (!same_serial()) {
            throw VoyemuError("INTERGUEST_UART", "source incarnation or extent changed");
        }

        return found;
    }

    void discover() {
        if (!same_file()) {
            throw VoyemuError("INTERGUEST_RAM", "RAM incarnation changed");
        }

        std::string line;

        if (serial_bound()) {
            line = uart_marker();

            if (line.empty()) {
                return;
            }
        } else {
            std::array<std::uint32_t, 8> before{}, after{};

            if (!read_at(before.data(), 32, ring - base)) {
                return;
            }

            if (before[0] != 0xbeefeeee) {
                return;
            }

            const auto capacity = before[1], cursor = before[2];

            if (capacity != 16352 || cursor >= capacity) {
                throw VoyemuError("INTERGUEST_RING", "invalid A9 ring");
            }

            std::string text(capacity, '\0');

            if (!read_at(text.data(), capacity, ring - base + 32)) {
                return;
            }

            if (!read_at(after.data(), 32, ring - base) || before != after) {
                return;
            }

            text = text.substr(cursor) + text.substr(0, cursor);
            const auto at = text.rfind("QNET READY pid=");

            if (at == std::string::npos) {
                return;
            }

            const auto end = text.find('\n', at);

            if (end == std::string::npos || end - at > 140) {
                return;
            }

            line = text.substr(at, end - at);
        }

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        unsigned pid = 0;
        unsigned long long physical = 0;
        constexpr std::string_view prefix = "QNET READY pid=", middle = " paddr=0x",
                                   suffix = " length=4096 version=1";
        const auto parsed = [&] {
            if (!line.starts_with(prefix)) {
                return false;
            }

            const char *end = line.data() + line.size();
            const auto decimal = std::from_chars(line.data() + prefix.size(), end, pid);

            if (decimal.ec != std::errc{} ||
                !std::string_view(decimal.ptr, end).starts_with(middle)) {
                return false;
            }

            const auto hex = std::from_chars(decimal.ptr + middle.size(), end, physical, 16);

            return hex.ec == std::errc{} && std::string_view(hex.ptr, end) == suffix;
        };

        if (!parsed() || pid == 0 || physical < base || physical > mailbox_end - 4096 ||
            (physical & 4095)) {
            throw VoyemuError("INTERGUEST_RING", "malformed relay announcement");
        }

        map_page(physical - base);

        if (load(page->magic) != 0x54454e51 || load(page->version) != 1 || load(page->stop) ||
            load(page->fault)) {
            unmap_page();
            throw VoyemuError("INTERGUEST_PAGE", "relay header invalid");
        }

        if (load(page->flush_request) || load(page->flush_ack)) {
            throw VoyemuError("INTERGUEST_FLUSH", "mailbox is not fresh");
        }

        if (load(page->utc_request) || load(page->utc_ack)) {
            throw VoyemuError("INTERGUEST_UTC", "mailbox is not fresh");
        }

        if (!same_file()) {
            throw VoyemuError("INTERGUEST_RAM", "RAM incarnation changed");
        }

        if (serial_bound() && !same_serial()) {
            throw VoyemuError("INTERGUEST_UART", "source incarnation or extent changed");
        }

        const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();

        if (epoch < 946684800LL || epoch >= 4102444800LL) {
            throw VoyemuError("INTERGUEST_UTC", "host epoch outside supported range");
        }

        state.utc_expected = static_cast<std::uint64_t>(epoch);
        page->utc_epoch = state.utc_expected;
        store(page->utc_request, 1);
        state.ready = true;
    }

    void run() {
        try {
            while (!stopping) {
                {
                    std::lock_guard lock(mutex);

                    if (bound()) {
                        if (serial_bound() && !same_serial()) {
                            throw VoyemuError("INTERGUEST_UART",
                                              "source incarnation or extent changed");
                        }

                        if (!same_file()) {
                            throw VoyemuError("INTERGUEST_RAM", "RAM incarnation changed");
                        }

                        if (!page) {
                            discover();
                        }
                    }

                    try {
                        if (!network.status().connected) {
#ifdef _WIN32
                            if (network_client ? network.connect(1) : network.accept(1)) {
                                ++state.reconnects;
                            }
#else
                            if (network.accept(1)) {
                                ++state.reconnects;
                            }
#endif
                        }

                        if (page) {
                            if (load(page->magic) != 0x54454e51 || load(page->version) != 1 ||
                                load(page->fault) || load(page->stop)) {
                                throw VoyemuError("INTERGUEST_PAGE", "relay stopped or invalid");
                            }

                            if (!state.utc_complete) {
                                const auto ack = load(page->utc_ack);

                                if (ack) {
                                    if (ack != 1) {
                                        throw VoyemuError("INTERGUEST_UTC",
                                                          "invalid acknowledgement");
                                    }

                                    state.utc_actual = page->utc_actual;
                                    state.utc_status = page->utc_status;
                                    state.utc_complete = true;

                                    if (state.utc_status || state.utc_actual < state.utc_expected ||
                                        state.utc_actual > state.utc_expected + 2) {
                                        throw VoyemuError("INTERGUEST_UTC",
                                                          "clock set/readback failed");
                                    }
                                }
                            }

                            const auto tx = load(page->txlen), rx = load(page->rxlen);

                            if ((tx && !frame(tx)) || (rx && !frame(rx))) {
                                throw VoyemuError("INTERGUEST_PAGE", "invalid frame extent");
                            }

                            const auto now = std::chrono::steady_clock::now();

                            if ((network.status().connected || gateway) && tx) {
                                const auto seq = load(page->txseq);
                                const auto next = txseq == 0xffffffffU ? 1U : txseq + 1U;

                                if (!seq || (txseq && seq != next)) {
                                    throw VoyemuError("INTERGUEST_PAGE", "repeated TX sequence");
                                }

                                const std::span<const std::uint8_t> frame(page->tx, tx);
                                // Unicast to the gateway stays local; everything else also
                                // reaches Android.
                                const bool local = gateway && std::equal(SomeipGateway::mac.begin(),
                                                                         SomeipGateway::mac.end(),
                                                                         frame.begin());

                                if (gateway) {
                                    gateway->receive(frame, now);
                                }

                                if (network.status().connected && !local) {
                                    network.send(frame, 10);
                                }

                                txseq = seq;
                                store(page->txlen, 0);
                                ++state.sent;
                            }

                            std::vector<std::uint8_t> bytes;

                            if (!rx && gateway && gateway->next(bytes, now)) {
                                std::memcpy(page->rx, bytes.data(), bytes.size());

                                if (++rxseq == 0) {
                                    ++rxseq;
                                }

                                store(page->rxseq, rxseq);
                                store(page->rxlen, static_cast<std::uint32_t>(bytes.size()));
                                ++state.received;
                            } else if (network.status().connected && !rx) {
                                if (network.receive(bytes, 1)) {
                                    if (!frame(static_cast<std::uint32_t>(bytes.size()))) {
                                        throw VoyemuError("INTERGUEST_PAGE",
                                                          "oversized peer frame");
                                    }

                                    std::memcpy(page->rx, bytes.data(), bytes.size());

                                    if (++rxseq == 0) {
                                        ++rxseq;
                                    }

                                    store(page->rxseq, rxseq);
                                    store(page->rxlen, static_cast<std::uint32_t>(bytes.size()));
                                    ++state.received;
                                }
                            }

                            // A guest reset can invalidate the page while the network poll is
                            // pending. Publish TAP completions only after validating their sampled
                            // mailbox header.
                            const GuestCounters observed{load(page->txpackets),
                                                         load(page->rxpackets)};

                            if (load(page->magic) != 0x54454e51 || load(page->version) != 1 ||
                                load(page->fault) || load(page->stop)) {
                                throw VoyemuError("INTERGUEST_PAGE", "relay stopped or invalid");
                            }

                            guest = observed;
                        }
                    } catch (const std::runtime_error &e) {
                        const std::string fault = e.what();

                        if (!fault.starts_with("INTERGUEST_SOCKET:") &&
                            !fault.starts_with("INTERGUEST_TIMEOUT:") &&
                            !fault.starts_with("INTERGUEST_FRAME:")) {
                            throw;
                        }
                    }

                    state.connected = network.status().connected;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } catch (const std::exception &e) {
            std::lock_guard lock(mutex);
            state.fault = e.what();
            state.ready = false;
            guest = {};
        } catch (...) {
            std::lock_guard lock(mutex);
            state.fault = "INTERGUEST_THREAD: unknown failure";
            state.ready = false;
            guest = {};
        }
    }
};

InterguestLink::InterguestLink() : impl_(std::make_unique<Impl>()) {}

InterguestLink::~InterguestLink() {
    stop();
}

unsigned InterguestLink::start(const std::filesystem::path &session, const std::string &token,
                               bool native_diagnostic) {
    std::lock_guard lock(impl_->mutex);

    if (impl_->worker.joinable()) {
        throw VoyemuError("INTERGUEST_STATE", "already started");
    }

    if (token.size() != 32 ||
        !std::all_of(token.begin(), token.end(),
                     [](char c) {
                         return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                     }) ||
        session.filename() != token || !std::filesystem::is_directory(session) ||
        std::filesystem::is_symlink(session)) {
        throw VoyemuError("INTERGUEST_OWNER", "owned session token required");
    }

#ifndef _WIN32
    struct stat session_info{};

    if (lstat(session.c_str(), &session_info) || session_info.st_uid != getuid() ||
        (session_info.st_mode & 0022)) {
        throw VoyemuError("INTERGUEST_OWNER", "unsafe session directory");
    }
#endif
    impl_->session = std::filesystem::canonical(session);
    impl_->state = {};
#ifdef _WIN32
    impl_->network_client = !native_diagnostic;
    impl_->state.port = impl_->network_client ? impl_->network.start_client()
                                              : impl_->network.start();
#else
    (void)native_diagnostic;
    impl_->state.port = impl_->network.start();
#endif
    impl_->stopping = false;
    try {
        impl_->worker = std::thread([this] {
            impl_->run();
        });
    } catch (...) {
        impl_->network.stop();
        throw;
    }

    return impl_->state.port;
}

void InterguestLink::bind_ram(const std::filesystem::path &ram, std::uint64_t ring,
                              const std::filesystem::path &serial, RamGeometry geometry) {
    std::lock_guard lock(impl_->mutex);

    if (!impl_->worker.joinable() || !impl_->state.fault.empty()) {
        throw VoyemuError("INTERGUEST_STATE", "inactive bridge");
    }

    if (geometry != RamGeometry::free8g && geometry != RamGeometry::passion) {
        throw VoyemuError("INTERGUEST_RAM", "unsupported geometry");
    }

    const bool passion = geometry == RamGeometry::passion;

    if (passion && serial.empty()) {
        throw VoyemuError("INTERGUEST_UART", "Passion requires owned UART discovery");
    }

    const std::uint64_t ram_size = passion ? (11713ULL << 20) : 0x200000000ULL;
    const std::uint64_t mailbox_end = passion ? 0x180000000ULL : 0x240000000ULL;

    if (ring != 0x50010000) {
        throw VoyemuError("INTERGUEST_RING", "unsupported A9 ring");
    }

    const auto path = std::filesystem::weakly_canonical(ram);
    auto relative = path.lexically_relative(impl_->session);

    if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
        std::filesystem::is_symlink(ram)) {
        throw VoyemuError("INTERGUEST_OWNER", "RAM outside session");
    }

#ifndef _WIN32
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat info{};

    if (fd < 0) {
        throw VoyemuError("INTERGUEST_RAM", "cannot open owned RAM");
    }

    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
        info.st_nlink != 1 || (info.st_mode & 0022) ||
        info.st_size != static_cast<off_t>(ram_size)) {
        close(fd);
        throw VoyemuError("INTERGUEST_OWNER", "invalid RAM identity");
    }

    int serial_fd = -1;
    struct stat serial_info{};

    if (!serial.empty()) {
        if (path.filename() != "qnx-ram.bin" || serial != impl_->session / "qnx-serial.log") {
            close(fd);
            throw VoyemuError("INTERGUEST_UART", "exact session UART path required");
        }

        serial_fd = open(serial.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

        if (serial_fd < 0 || fstat(serial_fd, &serial_info) || !S_ISREG(serial_info.st_mode) ||
            serial_info.st_uid != getuid() || serial_info.st_nlink != 1 ||
            (serial_info.st_mode & 0022) || serial_info.st_size < 0 ||
            std::uint64_t(serial_info.st_size) > (16ULL << 20)) {
            if (serial_fd >= 0) {
                close(serial_fd);
            }

            close(fd);
            throw VoyemuError("INTERGUEST_UART", "invalid owned UART");
        }
    }

    impl_->release(false);
    impl_->ram_size = ram_size;
    impl_->mailbox_end = mailbox_end;
    impl_->fd = fd;
    impl_->device = info.st_dev;
    impl_->inode = info.st_ino;
    impl_->ram = path;
    impl_->ring = ring;
    impl_->serial_fd = serial_fd;
    impl_->serial = serial;
    impl_->serial_identity = serial_info;
    ++impl_->state.generation;
#else
    if (path != ram || path.filename() != "qnx-ram.bin") {
        throw VoyemuError("INTERGUEST_OWNER", "exact canonical RAM path required");
    }

    for (const auto &at : os::ancestors(path)) {
        const auto attributes = GetFileAttributesW(at.c_str());

        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw VoyemuError("INTERGUEST_OWNER", "reparse RAM path rejected");
        }
    }

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION identity{};

    if (!Impl::regular(file, identity, ram_size)) {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }

        throw VoyemuError("INTERGUEST_OWNER", "invalid RAM identity");
    }

    HANDLE serial_file = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION serial_identity{};

    if (!serial.empty()) {
        if (serial != impl_->session / "qnx-serial.log") {
            CloseHandle(file);
            throw VoyemuError("INTERGUEST_UART", "exact session UART path required");
        }

        // The owned QEMU keeps the file open for writing, so the read handle shares writes.
        serial_file = CreateFileW(serial.c_str(), GENERIC_READ | READ_CONTROL,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

        if (!Impl::ordinary(serial_file, serial_identity) ||
            Impl::extent(serial_identity) > (16ULL << 20)) {
            if (serial_file != INVALID_HANDLE_VALUE) {
                CloseHandle(serial_file);
            }

            CloseHandle(file);
            throw VoyemuError("INTERGUEST_UART", "invalid owned UART");
        }
    }

    impl_->release(false);
    impl_->ram_size = ram_size;
    impl_->mailbox_end = mailbox_end;
    impl_->fd = file;
    impl_->identity = identity;
    impl_->ram = path;
    impl_->ring = ring;
    impl_->serial_file = serial_file;
    impl_->serial = serial;
    impl_->serial_identity = serial_identity;
    ++impl_->state.generation;
#endif
}

void InterguestLink::flush(int timeout_ms) {
    if (timeout_ms < 0 ||
        timeout_ms >
                qnx_guest_deadline(host_qnx_engine, std::chrono::milliseconds(30000)).count()) {
        throw VoyemuError("INTERGUEST_FLUSH", "invalid deadline");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::uint64_t generation = 0;
    std::uint32_t sequence = 0;
    // After publication the guest may already sync and remount, so failure is not "not started".
    const auto fail = [&](const char *reason) {
        throw VoyemuError(sequence ? "INTERGUEST_FLUSH_UNKNOWN" : "INTERGUEST_FLUSH", reason);
    };

    for (;;) {
        {
            std::lock_guard lock(impl_->mutex);

            if (!impl_->state.fault.empty() || !impl_->page || !impl_->state.ready ||
                !impl_->same_file()) {
                fail("owned relay unavailable");
            }

            if (!generation) {
                generation = impl_->state.generation;
            }

            if (generation != impl_->state.generation) {
                fail("RAM generation changed");
            }

            auto *page = impl_->page;

            if (load(page->magic) != 0x54454e51 || load(page->version) != 1 || load(page->stop) ||
                load(page->fault)) {
                fail("invalid relay");
            }

            if (!sequence) {
                const auto previous = load(page->flush_request);

                if (previous == 0xffffffffU || load(page->flush_ack) > previous) {
                    fail("invalid sequence");
                }

                sequence = previous + 1;
                store(page->flush_request, sequence);
            }

            const auto ack = load(page->flush_ack);

            if (ack > sequence) {
                fail("invalid acknowledgement");
            }

            if (ack == sequence) {
                if (page->flush_status) {
                    fail("guest quiesce failed");
                }

                return;
            }
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            fail("guest sync deadline");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void InterguestLink::clear_ram() {
    // A guest terminal page can end the worker before Session observes QEMU exit.
    // Join before taking its mutex; keep the Android transport and peer owner intact.
    const bool started = impl_->worker.joinable();

    impl_->stopping = true;

    if (started) {
        impl_->worker.join();
    }

    std::lock_guard lock(impl_->mutex);
    impl_->release(false);
    ++impl_->state.generation;
    const auto &fault = impl_->state.fault;
    const bool ram_fault =
            fault.starts_with("INTERGUEST_RAM:") || fault.starts_with("INTERGUEST_UART:") ||
            fault.starts_with("INTERGUEST_RING:") || fault.starts_with("INTERGUEST_PAGE:") ||
            fault.starts_with("INTERGUEST_UTC:") || fault.starts_with("INTERGUEST_FLUSH:");

    // Transport ownership and unexpected worker failures remain fatal for this session.
    if (!started || (!fault.empty() && !ram_fault)) {
        return;
    }

    impl_->state.fault.clear();
    impl_->stopping = false;
    try {
        impl_->worker = std::thread([this] {
            impl_->run();
        });
    } catch (...) {
        impl_->state.fault = "INTERGUEST_THREAD: restart failed";
        throw;
    }
}

InterguestLink::GuestCounters InterguestLink::guest_counters() const {
    std::lock_guard lock(impl_->mutex);

    return impl_->guest;
}

InterguestLink::Status InterguestLink::status() const {
    std::lock_guard lock(impl_->mutex);

    return impl_->state;
}

void InterguestLink::attach_gateway(const Json &contract) {
    auto gateway = std::make_unique<SomeipGateway>(someip_contract(contract));
    std::lock_guard lock(impl_->mutex);
    impl_->gateway = std::move(gateway);
}

unsigned InterguestLink::apply_vehicle_state(const Json &state) {
    std::lock_guard lock(impl_->mutex);

    if (!impl_->gateway) {
        throw VoyemuError("INTERGUEST_GATEWAY", "no vehicle gateway attached");
    }

    return impl_->gateway->apply(state);
}

void InterguestLink::bind_android(std::uint64_t pid) {
    std::lock_guard lock(impl_->mutex);

    impl_->network.bind_peer(pid);
}

void InterguestLink::check() const {
    const auto s = status();

    if (!s.fault.empty()) {
        throw std::runtime_error(s.fault);
    }
}

void InterguestLink::stop() noexcept {
    impl_->stopping = true;

    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    std::lock_guard lock(impl_->mutex);
    impl_->network.stop();
    impl_->release(true);
    impl_->state.port = 0;
    impl_->state.connected = false;
}
} // namespace voyemu
