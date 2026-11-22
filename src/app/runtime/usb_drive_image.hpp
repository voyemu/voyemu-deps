#pragma once
#include "usb_drive_media.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

// The whole-disk image of the emulated USB drive: an MBR with one FAT32 partition, as a stick
// formatted for a car.
namespace voyemu::usb_drive {
inline constexpr std::uint64_t image_bytes = 64ULL << 20;
inline constexpr std::uint32_t sector_bytes = 512, partition_start = 2048,
                               volume_serial = 0x20261122;
inline constexpr std::string_view volume_label = "VOYEMU";

// The disk with the files in their folders; a full volume or an invalid path is USB_DRIVE_MEDIA.
std::vector<std::uint8_t> drive_image(std::span<const File> files);
// Writes the disk of the test media to image through a temporary file and an atomic replace;
// failures are USB_DRIVE_WRITE.
void write_test_drive(const std::filesystem::path &image);
} // namespace voyemu::usb_drive
