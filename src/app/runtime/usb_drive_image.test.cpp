#include "usb_drive_image.hpp"
#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace voyemu::usb_drive;
using Bytes = std::vector<std::uint8_t>;

namespace {
void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint32_t le(std::span<const std::uint8_t> bytes, std::size_t at, unsigned count) {
    require(at + count <= bytes.size(), "read past end");
    std::uint32_t value = 0;

    for (unsigned i = count; i-- > 0;) {
        value = value << 8 | bytes[at + i];
    }

    return value;
}

std::string utf8(const std::u16string &units) {
    std::string out;

    for (std::size_t i = 0; i < units.size(); ++i) {
        char32_t code = units[i];

        if (code >= 0xd800 && code < 0xdc00 && i + 1 < units.size()) {
            code = 0x10000 + ((code - 0xd800) << 10) + (units[++i] - 0xdc00);
        }

        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xc0 | code >> 6));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | code >> 12));
            out.push_back(static_cast<char>(0x80 | (code >> 6 & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | code >> 18));
            out.push_back(static_cast<char>(0x80 | (code >> 12 & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code >> 6 & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    return out;
}

// A FAT32 reader: MBR, BPB, both FATs, cluster chains and long file names.
struct Volume {
    std::span<const std::uint8_t> disk;
    std::size_t partition{}, fat{}, data{};
    std::uint32_t cluster_bytes{}, clusters{}, root{};
    std::string label;
    std::uint32_t serial{};
    std::vector<std::uint32_t> used;

    std::vector<std::uint32_t> chain(std::uint32_t first) const {
        std::vector<std::uint32_t> result;

        for (auto cluster = first; cluster < 0x0ffffff8;) {
            require(cluster >= 2 && cluster < clusters + 2, "cluster in the volume");
            require(result.size() <= clusters, "cluster chain loops");
            result.push_back(cluster);
            cluster = le(disk, fat + std::size_t{cluster} * 4, 4) & 0x0fffffff;
        }

        return result;
    }

    Bytes read(std::uint32_t first, std::uint64_t size) const {
        Bytes out;

        for (const auto cluster : chain(first)) {
            const auto at = data + std::size_t{cluster - 2} * cluster_bytes;
            out.insert(out.end(), disk.begin() + at, disk.begin() + at + cluster_bytes);
        }

        require(out.size() >= size, "chain covers the file");
        out.resize(size);

        return out;
    }
};

Volume open_volume(std::span<const std::uint8_t> disk) {
    require(disk.size() == image_bytes && disk[510] == 0x55 && disk[511] == 0xaa, "MBR signature");
    require(disk[446 + 4] == 0x0c, "FAT32 LBA partition type");
    Volume volume;
    volume.disk = disk;
    const auto start = le(disk, 446 + 8, 4), sectors = le(disk, 446 + 12, 4);
    require(start == partition_start && std::uint64_t{start + sectors} * 512 == image_bytes,
            "partition fills the disk from LBA 2048");
    volume.partition = std::size_t{start} * 512;
    const auto boot = disk.subspan(volume.partition, 512);
    require(boot[510] == 0x55 && boot[511] == 0xaa && le(boot, 11, 2) == 512,
            "boot sector of 512-byte sectors");
    require(le(boot, 17, 2) == 0 && le(boot, 22, 2) == 0 && le(boot, 32, 4) == sectors,
            "FAT32 BPB");
    require(std::string(boot.begin() + 82, boot.begin() + 90) == "FAT32   ", "FAT32 type");
    const unsigned per_cluster = boot[13], reserved = le(boot, 14, 2), fats = boot[16];
    const auto fat_sectors = le(boot, 36, 4);
    require(fats == 2, "two FATs");
    volume.cluster_bytes = per_cluster * 512;
    volume.fat = volume.partition + std::size_t{reserved} * 512;
    volume.data = volume.fat + std::size_t{fats} * fat_sectors * 512;
    volume.clusters = (sectors - reserved - fats * fat_sectors) / per_cluster;
    require(volume.clusters >= 65525, "enough clusters to be FAT32");
    require(std::uint64_t{volume.clusters + 2} * 4 <= std::uint64_t{fat_sectors} * 512,
            "FAT covers every cluster");
    volume.root = le(boot, 44, 4);
    volume.serial = le(boot, 67, 4);
    volume.label = std::string(boot.begin() + 71, boot.begin() + 82);
    require(std::equal(disk.begin() + volume.fat,
                       disk.begin() + volume.fat + std::size_t{fat_sectors} * 512,
                       disk.begin() + volume.fat + std::size_t{fat_sectors} * 512),
            "FAT copies equal");
    const auto backup = le(boot, 50, 2), info_sector = le(boot, 48, 2);
    require(std::equal(boot.begin(), boot.end(),
                       disk.begin() + volume.partition + std::size_t{backup} * 512),
            "backup boot sector");
    const auto info = disk.subspan(volume.partition + std::size_t{info_sector} * 512, 512);
    require(le(info, 0, 4) == 0x41615252 && le(info, 484, 4) == 0x61417272 &&
                    le(info, 508, 4) == 0xaa550000,
            "FSInfo signatures");
    std::uint32_t free = 0;

    for (std::uint32_t cluster = 2; cluster < volume.clusters + 2; ++cluster) {
        free += (le(disk, volume.fat + std::size_t{cluster} * 4, 4) & 0x0fffffff) == 0;
    }

    require(le(info, 488, 4) == free, "FSInfo free cluster count");

    return volume;
}

std::uint8_t checksum(std::span<const std::uint8_t> name) {
    std::uint8_t sum = 0;

    for (const auto c : name) {
        sum = static_cast<std::uint8_t>(((sum & 1) << 7) + (sum >> 1) + c);
    }

    return sum;
}

void walk(const Volume &volume, std::uint32_t cluster, std::uint32_t parent,
          const std::string &prefix, std::map<std::string, Bytes> &files,
          std::string *label = nullptr) {
    const auto directory = volume.read(cluster,
                                       volume.chain(cluster).size() * volume.cluster_bytes);
    std::u16string long_name;
    int expected_order = -1;
    std::uint8_t long_checksum = 0;
    std::vector<std::string> shorts;

    for (std::size_t at = 0; at < directory.size() && directory[at]; at += 32) {
        const auto entry = std::span(directory).subspan(at, 32);
        const unsigned attributes = entry[11];

        if (attributes == 0x0f) {
            const int order = entry[0] & 0x1f;

            if (entry[0] & 0x40) {
                long_name.clear();
                expected_order = order;
                long_checksum = entry[13];
            }

            require(order == expected_order && entry[13] == long_checksum, "long name sequence");
            std::u16string part;

            for (const std::size_t offset : {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30}) {
                const auto unit = static_cast<char16_t>(le(entry, offset, 2));

                if (unit == 0 || unit == 0xffff) {
                    break;
                }

                part.push_back(unit);
            }

            long_name = part + long_name;
            --expected_order;
            continue;
        }

        const std::string short_name(entry.begin(), entry.begin() + 11);
        require(std::find(shorts.begin(), shorts.end(), short_name) == shorts.end(),
                "unique short names");
        shorts.push_back(short_name);

        if (attributes == 0x08) {
            require(label, "volume label only in the root");
            *label = short_name;
            continue;
        }

        const auto first = le(entry, 20, 2) << 16 | le(entry, 26, 2);
        require(le(entry, 24, 2) == ((2026 - 1980) << 9 | 11 << 5 | 22) &&
                        le(entry, 22, 2) == 12 << 11,
                "fixed timestamps");

        if (short_name == ".          ") {
            require(first == cluster, "dot entry");
            continue;
        }

        if (short_name == "..         ") {
            require(first == parent, "dot-dot entry");
            continue;
        }

        std::string name;

        if (!long_name.empty()) {
            require(expected_order == 0 && long_checksum == checksum(entry.first(11)),
                    "long name checksum");
            name = utf8(long_name);
        } else {
            auto base = short_name.substr(0, 8), extension = short_name.substr(8);
            base.erase(base.find_last_not_of(' ') + 1);
            extension.erase(extension.find_last_not_of(' ') + 1);
            name = extension.empty() ? base : base + "." + extension;
        }

        long_name.clear();

        if (attributes & 0x10) {
            walk(volume, first, cluster == volume.root ? 0 : cluster, prefix + name + "/", files);
        } else {
            require(attributes == 0x20, "archive file");
            const auto size = le(entry, 28, 4);
            files[prefix + name] = size ? volume.read(first, size) : Bytes();
        }
    }
}

std::map<std::string, Bytes> read_files(std::span<const std::uint8_t> disk, std::string &label,
                                        std::uint32_t &serial) {
    const auto volume = open_volume(disk);
    std::map<std::string, Bytes> files;

    walk(volume, volume.root, 0, "", files, &label);
    require(label == volume.label, "root label equals the boot sector label");
    serial = volume.serial;

    return files;
}

template <typename Action> void expect_failure(Action action, const char *message) {
    try {
        action();
    } catch (const std::runtime_error &error) {
        require(std::string(error.what()).starts_with("USB_DRIVE_MEDIA:"), message);

        return;
    }

    throw std::runtime_error(message);
}
} // namespace

int main(int argc, char **argv) try {
    require(argc == 2, "usage: usb_drive_image_test TEMPORARY_DIRECTORY");
    const std::vector<File> synthetic{{"README.TXT", Bytes{'h', 'i'}},
                                      {"Music/Album/01 - Long name track.flac", Bytes(1500, 7)},
                                      {"Music/Album/01 - Long name track two.flac", Bytes(10, 8)},
                                      {"Music/Альбом/Песня 𠀀.flac", Bytes(513, 9)},
                                      {"Empty/zero.bin", {}},
                                      {"Video/clip.mp4", Bytes(70000, 1)}};
    const auto image = drive_image(synthetic);
    std::string label;
    std::uint32_t serial = 0;
    const auto files = read_files(image, label, serial);
    require(label == "VOYEMU     " && serial == volume_serial, "label and serial");
    require(files.size() == synthetic.size(), "every file present");

    for (const auto &file : synthetic) {
        require(files.contains(file.path) && files.at(file.path) == file.bytes,
                "file content " + file.path);
    }

    require(drive_image(synthetic) == image, "deterministic image");
    expect_failure(
            [] {
                drive_image(std::vector<File>{{"a/b", {}}, {"A/B", {}}});
            },
            "case-insensitive duplicate rejected");
    expect_failure(
            [] {
                drive_image(std::vector<File>{{"a/../b", {}}});
            },
            "dot-dot rejected");
    expect_failure(
            [] {
                drive_image(std::vector<File>{{"bad:name", {}}});
            },
            "invalid character rejected");
    expect_failure(
            [] {
                drive_image(std::vector<File>{{"big.bin", Bytes(image_bytes, 0)}});
            },
            "overfull drive rejected");
    const std::filesystem::path directory = argv[1];
    std::filesystem::create_directories(directory);
    const auto path = directory / "usb-drive-image-test.img";
    std::filesystem::remove(path);
    write_test_drive(path);
    require(!std::filesystem::exists(directory / "usb-drive-image-test.img.pending"),
            "no staging file left");
    Bytes written(std::filesystem::file_size(path));
    std::ifstream(path, std::ios::binary)
            .read(reinterpret_cast<char *>(written.data()), std::streamsize(written.size()));
    std::filesystem::remove(path);
    const auto media = test_media();
    const auto drive_files = read_files(written, label, serial);
    require(drive_files.size() == media.size(), "test drive file count");

    for (const auto &file : media) {
        require(drive_files.contains(file.path) && drive_files.at(file.path) == file.bytes,
                "test drive file " + file.path);
    }

    std::cout << "USB drive image passed\n";

    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
