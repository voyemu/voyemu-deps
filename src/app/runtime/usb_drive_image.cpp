#include "usb_drive_image.hpp"
#include "error.hpp"
#include "os/fs.hpp"
#include <algorithm>
#include <array>
#include <string>

namespace voyemu::usb_drive {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t total_sectors = image_bytes / sector_bytes,
                        partition_sectors = total_sectors - partition_start, reserved_sectors = 32,
                        root_cluster = 2, end_of_chain = 0x0fffffff;
// 2026-11-22 12:00:00 in FAT date and time fields.
constexpr std::uint16_t fat_date = (2026 - 1980) << 9 | 11 << 5 | 22, fat_time = 12 << 11;

struct Node {
    std::string name;
    bool directory{};
    const std::vector<std::uint8_t> *data{};
    std::vector<std::size_t> children;
    std::size_t parent{};
    std::uint32_t cluster{};
    std::vector<Bytes> entries;
};

void put16(Bytes &out, std::size_t at, std::uint32_t value) {
    out[at] = static_cast<std::uint8_t>(value);
    out[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(Bytes &out, std::size_t at, std::uint32_t value) {
    put16(out, at, value & 0xffff);
    put16(out, at + 2, value >> 16);
}

std::u16string utf16(std::string_view text) {
    std::u16string out;

    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const unsigned length = lead < 0x80       ? 1
                                : lead >> 5 == 6  ? 2
                                : lead >> 4 == 14 ? 3
                                : lead >> 3 == 30 ? 4
                                                  : 0;

        if (!length || i + length > text.size()) {
            throw VoyemuError("USB_DRIVE_MEDIA", "file name is not UTF-8");
        }

        char32_t code = length == 1 ? lead : lead & (0x7f >> length);

        for (unsigned k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);

            if (next >> 6 != 2) {
                throw VoyemuError("USB_DRIVE_MEDIA", "file name is not UTF-8");
            }

            code = code << 6 | (next & 0x3f);
        }

        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800 + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00 + (code & 0x3ff)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }

        i += length;
    }

    return out;
}

bool short_character(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           std::string_view("!#$%&'()-@^_`{}~").find(c) != std::string_view::npos;
}

// The name as an exact uppercase 8.3 name, padded to 11 bytes, or empty when it needs a long name.
std::string exact_short_name(std::string_view name) {
    const auto dot = name.find('.');
    const auto base = name.substr(0, dot);
    const auto extension = dot == std::string_view::npos ? std::string_view()
                                                         : name.substr(dot + 1);

    if (base.empty() || base.size() > 8 || extension.size() > 3 ||
        extension.find('.') != std::string_view::npos ||
        (dot != std::string_view::npos && extension.empty()) ||
        !std::all_of(base.begin(), base.end(), short_character) ||
        !std::all_of(extension.begin(), extension.end(), short_character)) {
        return {};
    }

    auto result = std::string(base) + std::string(8 - base.size(), ' ') + std::string(extension);
    result.resize(11, ' ');

    return result;
}

// The basis of a generated short name: uppercase short characters, others replaced by '_'.
std::string short_basis(std::string_view text, std::size_t limit) {
    std::string out;

    for (std::size_t i = 0; i < text.size() && out.size() < limit; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);

        if (c == ' ' || c == '.') {
            continue;
        }

        // Skip the continuation bytes of one non-ASCII character after its replacement.
        if (c >= 0x80) {
            if (c >= 0xc0) {
                out.push_back('_');
            }

            continue;
        }

        const char upper = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
        out.push_back(short_character(upper) ? upper : '_');
    }

    return out;
}

std::uint8_t short_checksum(std::string_view name) {
    std::uint8_t sum = 0;

    for (const auto c : name) {
        sum = static_cast<std::uint8_t>(((sum & 1) << 7) + (sum >> 1) +
                                        static_cast<std::uint8_t>(c));
    }

    return sum;
}

Bytes short_entry(std::string_view name, std::uint8_t attributes, std::uint32_t cluster,
                  std::uint32_t size) {
    Bytes entry(32);

    std::copy(name.begin(), name.end(), entry.begin());
    entry[11] = attributes;

    if (attributes != 0x08) {
        put16(entry, 14, fat_time);
        put16(entry, 16, fat_date);
        put16(entry, 18, fat_date);
        put16(entry, 20, cluster >> 16);
    }

    put16(entry, 22, fat_time);
    put16(entry, 24, fat_date);
    put16(entry, 26, cluster & 0xffff);
    put32(entry, 28, size);

    return entry;
}

std::vector<Bytes> long_entries(std::string_view name, std::uint8_t checksum) {
    const auto units = utf16(name);

    if (units.size() > 255) {
        throw VoyemuError("USB_DRIVE_MEDIA", "file name longer than 255 characters");
    }

    const std::size_t count = (units.size() + 12) / 13;
    std::vector<Bytes> result;
    constexpr std::array<std::size_t, 13> offsets{1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

    for (std::size_t n = count; n-- > 0;) {
        Bytes entry(32);
        entry[0] = static_cast<std::uint8_t>((n + 1) | (n + 1 == count ? 0x40 : 0));
        entry[11] = 0x0f;
        entry[13] = checksum;

        for (std::size_t k = 0; k < 13; ++k) {
            const std::size_t index = n * 13 + k;
            const std::uint32_t unit = index < units.size()    ? units[index]
                                       : index == units.size() ? 0
                                                               : 0xffff;
            put16(entry, offsets[k], unit);
        }

        result.push_back(std::move(entry));
    }

    return result;
}

std::uint32_t clusters_of(std::uint64_t bytes) {
    return static_cast<std::uint32_t>((bytes + sector_bytes - 1) / sector_bytes);
}
} // namespace

std::vector<std::uint8_t> drive_image(std::span<const File> files) {
    std::vector<Node> nodes(1);

    nodes[0].directory = true;

    for (const auto &file : files) {
        std::size_t current = 0, start = 0;

        for (;;) {
            const auto slash = file.path.find('/', start);
            const auto name = file.path.substr(start, slash == std::string::npos ? std::string::npos
                                                                                 : slash - start);
            const bool leaf = slash == std::string::npos;

            if (name.empty() || name == "." || name == ".." || name.back() == ' ' ||
                name.back() == '.' || name.find_first_of("\\:*?\"<>|") != std::string::npos ||
                std::any_of(name.begin(), name.end(), [](char c) {
                    return static_cast<unsigned char>(c) < 0x20;
                })) {
                throw VoyemuError("USB_DRIVE_MEDIA", "invalid drive path " + file.path);
            }

            const auto same = [&](std::size_t child) {
                const auto &other = nodes[child].name;

                return std::equal(other.begin(), other.end(), name.begin(), name.end(),
                                  [](char a, char b) {
                                      const auto lower = [](char c) {
                                          return c >= 'A' && c <= 'Z' ? char(c + 32) : c;
                                      };

                                      return lower(a) == lower(b);
                                  });
            };
            const auto &children = nodes[current].children;
            const auto found = std::find_if(children.begin(), children.end(), same);

            if (found != children.end()) {
                if (leaf || !nodes[*found].directory) {
                    throw VoyemuError("USB_DRIVE_MEDIA", "duplicate drive path " + file.path);
                }

                current = *found;
            } else {
                Node node;
                node.name = name;
                node.directory = !leaf;
                node.data = leaf ? &file.bytes : nullptr;
                node.parent = current;
                nodes.push_back(std::move(node));
                nodes[current].children.push_back(nodes.size() - 1);
                current = nodes.size() - 1;
            }

            if (leaf) {
                break;
            }

            start = slash + 1;
        }
    }

    // Directory entries other than cluster numbers depend only on names: count them first.
    std::vector<std::vector<std::pair<std::string, std::vector<Bytes>>>> names(nodes.size());

    for (std::size_t d = 0; d < nodes.size(); ++d) {
        std::vector<std::string> taken;

        for (const auto child : nodes[d].children) {
            const auto &name = nodes[child].name;
            auto short_name = exact_short_name(name);
            std::vector<Bytes> long_name;

            if (short_name.empty() ||
                std::find(taken.begin(), taken.end(), short_name) != taken.end()) {
                const auto dot = name.rfind('.');
                const auto extension = dot == std::string::npos || dot == 0
                                               ? std::string()
                                               : short_basis(name.substr(dot + 1), 3);
                const auto base = short_basis(
                        name.substr(0, dot == std::string::npos || dot == 0 ? name.size() : dot),
                        8);

                for (unsigned n = 1;; ++n) {
                    const auto tail = "~" + std::to_string(n);
                    auto candidate =
                            (base.empty() ? std::string("_") : base).substr(0, 8 - tail.size()) +
                            tail;
                    candidate.resize(8, ' ');
                    candidate += extension;
                    candidate.resize(11, ' ');

                    if (std::find(taken.begin(), taken.end(), candidate) == taken.end()) {
                        short_name = candidate;
                        break;
                    }
                }

                long_name = long_entries(name, short_checksum(short_name));
            }

            taken.push_back(short_name);
            names[d].emplace_back(short_name, std::move(long_name));
        }
    }

    const auto directory_bytes = [&](std::size_t d) {
        std::uint64_t count = 1 + (d == 0 ? 0 : 1);

        for (const auto &[short_name, long_name] : names[d]) {
            count += 1 + long_name.size();
        }

        return count * 32;
    };
    std::uint32_t fat_sectors = 1, clusters = 0;

    for (;;) {
        clusters = partition_sectors - reserved_sectors - 2 * fat_sectors;
        const auto needed = (std::uint64_t{clusters} + 2) * 4 / sector_bytes +
                            ((std::uint64_t{clusters} + 2) * 4 % sector_bytes != 0);

        if (needed == fat_sectors) {
            break;
        }

        fat_sectors = static_cast<std::uint32_t>(needed);
    }

    // Contiguous allocation in pre-order: a directory, then its files and subdirectories.
    std::vector<std::uint32_t> lengths(nodes.size());
    std::uint32_t next = root_cluster;
    const auto allocate = [&](auto &self, std::size_t d) -> void {
        const auto length = clusters_of(directory_bytes(d));
        nodes[d].cluster = next;
        lengths[d] = length;
        next += length;

        for (const auto child : nodes[d].children) {
            if (nodes[child].directory) {
                self(self, child);
            } else if (!nodes[child].data->empty()) {
                if (nodes[child].data->size() > UINT32_MAX) {
                    throw VoyemuError("USB_DRIVE_MEDIA", "file larger than FAT32 allows");
                }

                const auto length = clusters_of(nodes[child].data->size());
                nodes[child].cluster = next;
                lengths[child] = length;
                next += length;
            }
        }
    };
    allocate(allocate, 0);

    if (next - root_cluster > clusters) {
        throw VoyemuError("USB_DRIVE_MEDIA", "test media exceed the drive");
    }

    Bytes image(image_bytes);
    const std::size_t partition = std::size_t{partition_start} * sector_bytes;
    const std::size_t fat = partition + std::size_t{reserved_sectors} * sector_bytes;
    const std::size_t data = fat + 2 * std::size_t{fat_sectors} * sector_bytes;
    const auto cluster_offset = [&](std::uint32_t cluster) {
        return data + std::size_t{cluster - root_cluster} * sector_bytes;
    };
    // MBR: disk signature, one FAT32 LBA partition with CHS of the 255/63 geometry.
    put32(image, 440, volume_serial);
    const std::array<std::uint8_t, 16> entry{0x00, 0x20, 0x21, 0x00, 0x0c, 0xfe, 0xff, 0xff};
    std::copy(entry.begin(), entry.end(), image.begin() + 446);
    put32(image, 446 + 8, partition_start);
    put32(image, 446 + 12, partition_sectors);
    image[510] = 0x55;
    image[511] = 0xaa;
    // Boot sector, FSInfo and their backups at sectors 6 and 7.
    Bytes boot(sector_bytes);
    boot[0] = 0xeb;
    boot[1] = 0x58;
    boot[2] = 0x90;
    std::copy_n("MSWIN4.1", 8, boot.begin() + 3);
    put16(boot, 11, sector_bytes);
    boot[13] = 1;
    put16(boot, 14, reserved_sectors);
    boot[16] = 2;
    boot[21] = 0xf8;
    put16(boot, 24, 63);
    put16(boot, 26, 255);
    put32(boot, 28, partition_start);
    put32(boot, 32, partition_sectors);
    put32(boot, 36, fat_sectors);
    put32(boot, 44, root_cluster);
    put16(boot, 48, 1);
    put16(boot, 50, 6);
    boot[64] = 0x80;
    boot[66] = 0x29;
    put32(boot, 67, volume_serial);
    auto label = std::string(volume_label);
    label.resize(11, ' ');
    std::copy(label.begin(), label.end(), boot.begin() + 71);
    std::copy_n("FAT32   ", 8, boot.begin() + 82);
    boot[510] = 0x55;
    boot[511] = 0xaa;
    Bytes info(sector_bytes);
    put32(info, 0, 0x41615252);
    put32(info, 484, 0x61417272);
    put32(info, 488, clusters - (next - root_cluster));
    put32(info, 492, next);
    put32(info, 508, 0xaa550000);

    for (const std::uint32_t base : {0u, 6u}) {
        std::copy(boot.begin(), boot.end(),
                  image.begin() + partition + std::size_t{base} * sector_bytes);
        std::copy(info.begin(), info.end(),
                  image.begin() + partition + (std::size_t{base} + 1) * sector_bytes);
    }

    Bytes table(std::size_t{fat_sectors} * sector_bytes);
    put32(table, 0, 0x0ffffff8);
    put32(table, 4, end_of_chain);

    for (std::size_t n = 0; n < nodes.size(); ++n) {
        for (std::uint32_t k = 0; k < lengths[n]; ++k) {
            const auto cluster = nodes[n].cluster + k;
            put32(table, std::size_t{cluster} * 4,
                  k + 1 == lengths[n] ? end_of_chain : cluster + 1);
        }

        if (!nodes[n].directory) {
            if (!nodes[n].data->empty()) {
                std::copy(nodes[n].data->begin(), nodes[n].data->end(),
                          image.begin() + cluster_offset(nodes[n].cluster));
            }

            continue;
        }

        Bytes directory;
        const auto add = [&](const Bytes &record) {
            directory.insert(directory.end(), record.begin(), record.end());
        };

        if (n == 0) {
            add(short_entry(label, 0x08, 0, 0));
        } else {
            add(short_entry(".          ", 0x10, nodes[n].cluster, 0));
            add(short_entry("..         ", 0x10,
                            nodes[n].parent == 0 ? 0 : nodes[nodes[n].parent].cluster, 0));
        }

        for (std::size_t i = 0; i < nodes[n].children.size(); ++i) {
            const auto &child = nodes[nodes[n].children[i]];
            const auto &[short_name, long_name] = names[n][i];

            for (const auto &record : long_name) {
                add(record);
            }

            add(short_entry(short_name, child.directory ? 0x10 : 0x20, child.cluster,
                            child.directory ? 0 : static_cast<std::uint32_t>(child.data->size())));
        }

        std::copy(directory.begin(), directory.end(),
                  image.begin() + cluster_offset(nodes[n].cluster));
    }

    for (std::uint32_t copy = 0; copy < 2; ++copy) {
        std::copy(table.begin(), table.end(),
                  image.begin() + fat + std::size_t{copy} * table.size());
    }

    return image;
}

void write_test_drive(const std::filesystem::path &image) {
    const auto media = test_media();
    const auto bytes = drive_image(media);
    auto pending = image;

    pending += ".pending";
    std::error_code ignored;
    std::filesystem::remove(pending, ignored);
    {
        os::ExclusiveFile file(pending);

        if (!file.open() || !file.write(bytes.data(), bytes.size()) || !file.sync() ||
            !file.close()) {
            std::filesystem::remove(pending, ignored);
            throw VoyemuError("USB_DRIVE_WRITE", "drive image not written");
        }
    }

    if (!os::replace_file(pending, image)) {
        std::filesystem::remove(pending, ignored);
        throw VoyemuError("USB_DRIVE_WRITE", "drive image not published");
    }
}
} // namespace voyemu::usb_drive
