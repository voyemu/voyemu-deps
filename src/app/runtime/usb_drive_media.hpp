#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// The test media of the emulated USB drive, encoded from scratch by Voyemu itself: every byte is
// deterministic and integer-only, so all hosts generate the same files.
namespace voyemu::usb_drive {
struct File {
    // A path relative to the drive root with '/' separators, such as "Music/Album/01 - A.flac".
    std::string path;
    std::vector<std::uint8_t> bytes;
};
struct AudioTags {
    std::string title, artist, album, track, date, genre;
};
struct RgbImage {
    unsigned width{}, height{};
    // Row-major RGB triples.
    std::vector<std::uint8_t> rgb;
};
// One coded 4:2:0 frame of a test video at the coded size, a whole number of macroblocks.
struct VideoFrame {
    unsigned width{}, height{};
    std::vector<std::uint8_t> y, cb, cr;
};
enum class VideoPattern {
    colour_bars,
    checker
};
inline constexpr unsigned audio_rate = 44100, audio_channels = 2;
inline constexpr unsigned video_width = 640, video_height = 360, video_coded_height = 368,
                          video_fps = 25;

// Interleaved 16-bit stereo samples of a sine tone at millihertz with 500 ms fades at both ends.
std::vector<std::int16_t> tone(std::uint32_t millihertz, unsigned seconds);
// FLAC with STREAMINFO, VORBIS_COMMENT and a front cover PICTURE when cover_jpeg is not empty.
std::vector<std::uint8_t> flac_file(std::span<const std::int16_t> samples, const AudioTags &tags,
                                    std::span<const std::uint8_t> cover_jpeg);
// RIFF WAVE PCM with a LIST INFO chunk.
std::vector<std::uint8_t> wav_file(std::span<const std::int16_t> samples, const AudioTags &tags);
// Baseline JPEG, 4:4:4, with the standard tables of ITU-T T.81 Annex K scaled to quality 1..100.
std::vector<std::uint8_t> jpeg_file(const RgbImage &image, unsigned quality);
// 8-bit RGB PNG.
std::vector<std::uint8_t> png_file(const RgbImage &image);
// The frame index of a test video at its coded size.
VideoFrame video_frame(VideoPattern pattern, unsigned index);
// MP4 with an H.264 Constrained Baseline track of the pattern, cropped to video_height, and an
// AAC-LC mono track of silence.
std::vector<std::uint8_t> mp4_file(VideoPattern pattern, unsigned seconds);
// The photo patterns of the drive.
RgbImage colour_bars_image(unsigned width, unsigned height);
RgbImage gradient_image(unsigned width, unsigned height);
RgbImage checker_image(unsigned width, unsigned height);
RgbImage rings_image(unsigned width, unsigned height);
RgbImage grid_image(unsigned width, unsigned height);
RgbImage cover_image(unsigned size);
// The files of the drive in the folders of a user's stick: Music, Video and Photos.
std::vector<File> test_media();
} // namespace voyemu::usb_drive
