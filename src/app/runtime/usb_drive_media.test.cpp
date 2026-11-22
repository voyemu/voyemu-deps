#include "usb_drive_media.hpp"
#include "os/digest.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iostream>
#include <map>
#include <miniz.h>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace voyemu::usb_drive;
using Bytes = std::vector<std::uint8_t>;

namespace {
void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint64_t be(std::span<const std::uint8_t> bytes, std::size_t at, unsigned count) {
    require(at + count <= bytes.size(), "read past end");
    std::uint64_t value = 0;

    for (unsigned i = 0; i < count; ++i) {
        value = value << 8 | bytes[at + i];
    }

    return value;
}

std::uint64_t le(std::span<const std::uint8_t> bytes, std::size_t at, unsigned count) {
    require(at + count <= bytes.size(), "read past end");
    std::uint64_t value = 0;

    for (unsigned i = count; i-- > 0;) {
        value = value << 8 | bytes[at + i];
    }

    return value;
}

std::string text(std::span<const std::uint8_t> bytes, std::size_t at, std::size_t count) {
    require(at + count <= bytes.size(), "read past end");

    return {bytes.begin() + at, bytes.begin() + at + count};
}

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    unsigned bit() {
        require(position_ < bytes_.size() * 8, "bit stream ended");
        const unsigned value = bytes_[position_ / 8] >> (7 - position_ % 8) & 1;
        ++position_;

        return value;
    }

    std::uint64_t bits(unsigned count) {
        std::uint64_t value = 0;

        for (unsigned i = 0; i < count; ++i) {
            value = value << 1 | bit();
        }

        return value;
    }

    std::uint32_t ue() {
        unsigned zeros = 0;

        while (!bit()) {
            ++zeros;
            require(zeros < 32, "Exp-Golomb code too long");
        }

        return static_cast<std::uint32_t>((std::uint64_t{1} << zeros) - 1 + bits(zeros));
    }

    std::int32_t se() {
        const auto value = ue();

        return value & 1 ? static_cast<std::int32_t>((value + 1) / 2)
                         : -static_cast<std::int32_t>(value / 2);
    }

    void align() {
        while (position_ % 8) {
            require(bit() == 0, "nonzero alignment bit");
        }
    }

    std::size_t byte() const {
        return position_ / 8;
    }

    bool rest_is_trailing() {
        if (bit() != 1) {
            return false;
        }

        while (position_ < bytes_.size() * 8) {
            if (bit()) {
                return false;
            }
        }

        return true;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_{};
};

std::uint8_t crc8(std::span<const std::uint8_t> bytes) {
    std::uint8_t crc = 0;

    for (const auto byte : bytes) {
        crc ^= byte;

        for (int i = 0; i < 8; ++i) {
            crc = static_cast<std::uint8_t>(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
        }
    }

    return crc;
}

std::uint16_t crc16(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0;

    for (const auto byte : bytes) {
        crc ^= static_cast<std::uint16_t>(byte << 8);

        for (int i = 0; i < 8; ++i) {
            crc = static_cast<std::uint16_t>(crc & 0x8000 ? (crc << 1) ^ 0x8005 : crc << 1);
        }
    }

    return crc;
}

std::int32_t sign_extend(std::uint64_t value, unsigned bits) {
    return static_cast<std::int32_t>(value & (1ULL << (bits - 1)) ? value - (1ULL << bits) : value);
}

struct FlacStream {
    unsigned rate{}, channels{}, bits{};
    std::uint64_t total{};
    std::array<std::uint8_t, 16> md5{};
    std::map<std::string, std::string> tags;
    Bytes picture;
    unsigned picture_width{}, picture_height{};
    std::vector<std::int16_t> samples;
};

// A FLAC decoder for FIXED and VERBATIM subframes with independent channels.
FlacStream decode_flac(std::span<const std::uint8_t> file) {
    require(text(file, 0, 4) == "fLaC", "FLAC marker");
    FlacStream stream;
    std::size_t at = 4;

    for (bool last = false; !last;) {
        const unsigned header = file[at];
        last = header & 0x80;
        const auto length = static_cast<std::size_t>(be(file, at + 1, 3));
        const auto body = file.subspan(at + 4, length);
        at += 4 + length;

        if ((header & 0x7f) == 0) {
            BitReader info(body);
            info.bits(16);
            info.bits(16);
            info.bits(24);
            info.bits(24);
            stream.rate = static_cast<unsigned>(info.bits(20));
            stream.channels = static_cast<unsigned>(info.bits(3)) + 1;
            stream.bits = static_cast<unsigned>(info.bits(5)) + 1;
            stream.total = info.bits(36);

            for (auto &byte : stream.md5) {
                byte = static_cast<std::uint8_t>(info.bits(8));
            }
        } else if ((header & 0x7f) == 4) {
            std::size_t p = 4 + le(body, 0, 4);
            const auto count = le(body, p, 4);
            p += 4;

            for (std::uint64_t i = 0; i < count; ++i) {
                const auto size = le(body, p, 4);
                const auto field = text(body, p + 4, size);
                const auto equals = field.find('=');
                stream.tags[field.substr(0, equals)] = field.substr(equals + 1);
                p += 4 + size;
            }
        } else if ((header & 0x7f) == 6) {
            require(be(body, 0, 4) == 3, "front cover picture type");
            const auto mime = be(body, 4, 4);
            require(text(body, 8, mime) == "image/jpeg", "cover MIME type");
            std::size_t p = 8 + mime;
            p += 4 + be(body, p, 4);
            stream.picture_width = static_cast<unsigned>(be(body, p, 4));
            stream.picture_height = static_cast<unsigned>(be(body, p + 4, 4));
            const auto size = be(body, p + 16, 4);
            stream.picture.assign(body.begin() + p + 20, body.begin() + p + 20 + size);
        }
    }

    std::uint64_t expected_number = 0;

    while (at < file.size()) {
        const auto frame = file.subspan(at);
        BitReader reader(frame);
        require(reader.bits(16) == 0xfff8, "FLAC frame sync");
        const auto block_code = reader.bits(4);
        require(reader.bits(4) == 0b1001, "44.1 kHz frame");
        require(reader.bits(4) == 0b0001, "independent stereo frame");
        require(reader.bits(3) == 0b100, "16-bit frame");
        require(reader.bit() == 0, "reserved frame bit");
        auto number = reader.bits(8);

        if (number >= 0xc0) {
            number = (number & 0x1f) << 6 | (reader.bits(8) & 0x3f);
        }

        require(number == expected_number++, "frame number order");
        unsigned block = block_code == 0b1100 ? 4096 : 0;

        if (block_code == 0b0111) {
            block = static_cast<unsigned>(reader.bits(16)) + 1;
        }

        require(block, "block size code");
        const auto header_bytes = reader.byte();
        require(reader.bits(8) == crc8(frame.first(header_bytes)), "frame header CRC-8");
        std::vector<std::vector<std::int32_t>> channels(2);

        for (auto &channel : channels) {
            require(reader.bit() == 0, "subframe padding");
            const auto type = reader.bits(6);
            require(reader.bit() == 0, "no wasted bits");

            if (type == 1) {
                for (unsigned i = 0; i < block; ++i) {
                    channel.push_back(sign_extend(reader.bits(16), 16));
                }

                continue;
            }

            require((type & 0b111000) == 0b001000 && (type & 7) <= 4, "FIXED subframe");
            const unsigned order = type & 7;

            for (unsigned i = 0; i < order; ++i) {
                channel.push_back(sign_extend(reader.bits(16), 16));
            }

            require(reader.bits(2) == 0, "4-bit Rice parameters");
            require(reader.bits(4) == 0, "one partition");
            const auto parameter = static_cast<unsigned>(reader.bits(4));
            require(parameter < 15, "no escape partition");

            for (unsigned n = order; n < block; ++n) {
                std::uint64_t quotient = 0;

                while (!reader.bit()) {
                    ++quotient;
                }

                const auto folded = quotient << parameter | reader.bits(parameter);
                const auto residual = static_cast<std::int64_t>(
                        folded & 1 ? -(std::int64_t(folded) >> 1) - 1 : std::int64_t(folded) >> 1);
                const auto *x = channel.data() + n;
                const std::int64_t predicted = order == 0   ? 0
                                               : order == 1 ? x[-1]
                                               : order == 2 ? 2LL * x[-1] - x[-2]
                                               : order == 3 ? 3LL * x[-1] - 3LL * x[-2] + x[-3]
                                                            : 4LL * x[-1] - 6LL * x[-2] +
                                                                      4LL * x[-3] - x[-4];
                channel.push_back(static_cast<std::int32_t>(predicted + residual));
            }
        }

        reader.align();
        const auto body_bytes = reader.byte();
        require(be(frame, body_bytes, 2) == crc16(frame.first(body_bytes)), "frame CRC-16");
        at += body_bytes + 2;

        for (unsigned i = 0; i < block; ++i) {
            stream.samples.push_back(static_cast<std::int16_t>(channels[0][i]));
            stream.samples.push_back(static_cast<std::int16_t>(channels[1][i]));
        }
    }

    return stream;
}

std::array<std::uint8_t, 16> md5_of(std::span<const std::int16_t> samples) {
    voyemu::os::Md5 md5;

    for (const auto sample : samples) {
        const std::array<std::uint8_t, 2> bytes{
                static_cast<std::uint8_t>(sample),
                static_cast<std::uint8_t>(std::uint16_t(sample) >> 8)};
        md5.update(bytes.data(), 2);
    }

    return md5.finish();
}

void test_tone_and_flac() {
    const auto samples = tone(440000, 3);

    require(samples.size() == std::size_t{3} * audio_rate * audio_channels, "tone length");
    require(samples[0] == 0 && samples[samples.size() - 1] == 0, "tone fades from and to silence");
    int peak = 0;
    std::size_t crossings = 0;

    for (std::size_t n = audio_rate; n < std::size_t{2} * audio_rate; ++n) {
        peak = std::max(peak, std::abs(int(samples[2 * n])));
        crossings += (samples[2 * n - 2] < 0) != (samples[2 * n] < 0);
        require(samples[2 * n] == samples[2 * n + 1], "both channels carry the tone");
    }

    require(peak > 9700 && peak <= 9830, "tone level about -10 dBFS");
    require(crossings >= 878 && crossings <= 882, "440 Hz has 880 zero crossings per second");
    const AudioTags tags{"A440", "Voyemu", "Album", "1", "2026", "Test"};
    const auto cover = jpeg_file(cover_image(64), 90);
    const auto file = flac_file(samples, tags, cover);
    const auto stream = decode_flac(file);
    require(stream.rate == audio_rate && stream.channels == 2 && stream.bits == 16,
            "STREAMINFO format");
    require(stream.total == samples.size() / 2, "STREAMINFO sample count");
    require(stream.samples == samples, "FLAC decodes to the generated samples");
    require(stream.md5 == md5_of(samples), "STREAMINFO MD5 of the samples");
    require(stream.tags.at("TITLE") == "A440" && stream.tags.at("ARTIST") == "Voyemu" &&
                    stream.tags.at("ALBUM") == "Album" && stream.tags.at("TRACKNUMBER") == "1" &&
                    stream.tags.at("DATE") == "2026" && stream.tags.at("GENRE") == "Test",
            "Vorbis comments");
    require(stream.picture == cover && stream.picture_width == 64 && stream.picture_height == 64,
            "front cover picture");
    require(file.size() < samples.size(), "fixed prediction compresses a tone below 8 bits");
}

void test_wav() {
    const auto samples = tone(783991, 1);
    const AudioTags tags{"G784", "Voyemu", "Album", "4", "2026", "Test"};
    const auto file = wav_file(samples, tags);

    require(text(file, 0, 4) == "RIFF" && le(file, 4, 4) == file.size() - 8 &&
                    text(file, 8, 4) == "WAVE",
            "RIFF WAVE header");
    std::map<std::string, std::string> info;
    bool format = false, data = false;

    for (std::size_t at = 12; at < file.size();) {
        const auto id = text(file, at, 4);
        const auto size = le(file, at + 4, 4);

        if (id == "fmt ") {
            format = le(file, at + 8, 2) == 1 && le(file, at + 10, 2) == 2 &&
                     le(file, at + 12, 4) == audio_rate && le(file, at + 22, 2) == 16;
        } else if (id == "LIST") {
            require(text(file, at + 8, 4) == "INFO", "LIST INFO");

            for (std::size_t p = at + 12; p < at + 8 + size;) {
                const auto length = le(file, p + 4, 4);
                info[text(file, p, 4)] = text(file, p + 8, length - 1);
                p += 8 + length + length % 2;
            }
        } else if (id == "data") {
            require(size == samples.size() * 2, "data size");

            for (std::size_t i = 0; i < samples.size(); ++i) {
                require(static_cast<std::int16_t>(le(file, at + 8 + 2 * i, 2)) == samples[i],
                        "PCM sample");
            }

            data = true;
        }

        at += 8 + size + size % 2;
    }

    require(format && data, "fmt and data chunks");
    require(info.at("INAM") == "G784" && info.at("IART") == "Voyemu" &&
                    info.at("IPRD") == "Album" && info.at("ITRK") == "4" &&
                    info.at("ICRD") == "2026" && info.at("IGNR") == "Test",
            "INFO tags");
}

// A baseline JPEG decoder: Huffman tables and quantization from the file, float IDCT.
RgbImage decode_jpeg(std::span<const std::uint8_t> file) {
    require(be(file, 0, 2) == 0xffd8, "SOI");
    std::array<std::array<unsigned, 64>, 4> quantization{};
    struct Table {
        std::map<std::pair<unsigned, unsigned>, unsigned> codes;
    };
    std::map<unsigned, Table> tables;
    unsigned width = 0, height = 0;
    std::array<unsigned, 3> component_quantization{};
    std::array<unsigned, 3> component_tables{};
    std::size_t at = 2;
    std::set<unsigned> seen;

    for (;;) {
        require(file[at] == 0xff, "marker");
        const unsigned marker = file[at + 1];
        const auto length = static_cast<std::size_t>(be(file, at + 2, 2));
        const auto body = file.subspan(at + 4, length - 2);
        seen.insert(marker);

        if (marker == 0xdb) {
            for (std::size_t p = 0; p < body.size(); p += 65) {
                require(body[p] >> 4 == 0, "8-bit quantization");

                for (unsigned k = 0; k < 64; ++k) {
                    quantization[body[p] & 3][k] = body[p + 1 + k];
                }
            }
        } else if (marker == 0xc4) {
            Table table;
            unsigned code = 0;
            std::size_t value = 17;

            for (unsigned length_bits = 1; length_bits <= 16; ++length_bits) {
                for (unsigned i = 0; i < body[length_bits]; ++i) {
                    table.codes[{length_bits, code++}] = body[value++];
                }

                code <<= 1;
            }

            tables[body[0]] = table;
        } else if (marker == 0xc0) {
            require(body[0] == 8 && body[5] == 3, "8-bit three-component frame");
            height = static_cast<unsigned>(be(body, 1, 2));
            width = static_cast<unsigned>(be(body, 3, 2));

            for (unsigned c = 0; c < 3; ++c) {
                require(body[6 + 3 * c + 1] == 0x11, "4:4:4 sampling");
                component_quantization[c] = body[6 + 3 * c + 2];
            }
        } else if (marker == 0xda) {
            for (unsigned c = 0; c < 3; ++c) {
                component_tables[c] = body[2 + 2 * c];
            }

            at += 2 + length;
            break;
        }

        at += 2 + length;
    }

    require(seen.contains(0xe0) && seen.contains(0xdb) && seen.contains(0xc0) &&
                    seen.contains(0xc4),
            "JFIF, DQT, SOF0 and DHT segments");
    Bytes entropy;

    for (; !(file[at] == 0xff && file[at + 1] != 0); ++at) {
        entropy.push_back(file[at]);

        if (file[at] == 0xff) {
            ++at;
        }
    }

    require(be(file, at, 2) == 0xffd9 && at + 2 == file.size(), "EOI ends the file");
    BitReader reader(entropy);
    const auto decode = [&](const Table &table) {
        unsigned code = 0;

        for (unsigned length_bits = 1; length_bits <= 16; ++length_bits) {
            code = code << 1 | reader.bit();
            const auto found = table.codes.find({length_bits, code});

            if (found != table.codes.end()) {
                return found->second;
            }
        }

        throw std::runtime_error("invalid Huffman code");
    };
    const auto extend = [&](unsigned size) {
        if (!size) {
            return 0;
        }

        const auto value = static_cast<int>(reader.bits(size));

        return value < (1 << (size - 1)) ? value - (1 << size) + 1 : value;
    };
    constexpr std::array<unsigned, 64> zigzag{
            0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
            41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
            30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
    RgbImage image{width, height, Bytes(std::size_t{width} * height * 3)};
    std::array<int, 3> dc{};
    const double pi = 3.14159265358979323846;
    std::array<std::array<double, 8>, 8> basis{};

    for (unsigned u = 0; u < 8; ++u) {
        for (unsigned x = 0; x < 8; ++x) {
            basis[u][x] = (u ? 1 : std::sqrt(.5)) * std::cos((2 * x + 1) * u * pi / 16);
        }
    }

    for (unsigned by = 0; by < (height + 7) / 8; ++by) {
        for (unsigned bx = 0; bx < (width + 7) / 8; ++bx) {
            std::array<std::array<double, 64>, 3> planes{};

            for (unsigned c = 0; c < 3; ++c) {
                std::array<double, 64> coefficients{};
                dc[c] += extend(decode(tables.at(component_tables[c] >> 4)));
                coefficients[0] = dc[c] * double(quantization[component_quantization[c]][0]);

                for (unsigned k = 1; k < 64;) {
                    const auto symbol = decode(tables.at(0x10 | (component_tables[c] & 15)));

                    if (symbol == 0) {
                        break;
                    }

                    k += symbol >> 4;
                    require(k < 64, "AC run inside the block");
                    coefficients[zigzag[k]] = extend(symbol & 15) *
                                              double(quantization[component_quantization[c]][k]);
                    ++k;
                }

                for (unsigned y = 0; y < 8; ++y) {
                    for (unsigned x = 0; x < 8; ++x) {
                        double sum = 0;

                        for (unsigned v = 0; v < 8; ++v) {
                            for (unsigned u = 0; u < 8; ++u) {
                                sum += coefficients[v * 8 + u] * basis[u][x] * basis[v][y];
                            }
                        }

                        planes[c][y * 8 + x] = sum / 4;
                    }
                }
            }

            for (unsigned y = 0; y < 8; ++y) {
                for (unsigned x = 0; x < 8; ++x) {
                    const unsigned px = bx * 8 + x, py = by * 8 + y;

                    if (px >= width || py >= height) {
                        continue;
                    }

                    const double luma = planes[0][y * 8 + x] + 128, cb = planes[1][y * 8 + x],
                                 cr = planes[2][y * 8 + x];
                    const std::array<double, 3> rgb{luma + 1.402 * cr,
                                                    luma - .344136 * cb - .714136 * cr,
                                                    luma + 1.772 * cb};

                    for (unsigned k = 0; k < 3; ++k) {
                        image.rgb[(std::size_t{py} * width + px) * 3 + k] =
                                static_cast<std::uint8_t>(
                                        std::clamp(std::lround(rgb[k]), 0L, 255L));
                    }
                }
            }
        }
    }

    return image;
}

double psnr(const RgbImage &a, const RgbImage &b) {
    double error = 0;

    for (std::size_t i = 0; i < a.rgb.size(); ++i) {
        const double d = double(a.rgb[i]) - b.rgb[i];
        error += d * d;
    }

    return 10 * std::log10(255.0 * 255.0 / (error / a.rgb.size() + 1e-9));
}

void test_jpeg_png() {
    for (const auto &source : {cover_image(100), gradient_image(77, 45), checker_image(64, 48),
                               colour_bars_image(96, 40)}) {
        const auto decoded = decode_jpeg(jpeg_file(source, 90));
        require(decoded.width == source.width && decoded.height == source.height,
                "JPEG dimensions");
        require(psnr(decoded, source) > 30, "JPEG decodes close to its source");
    }

    const auto source = rings_image(120, 80);
    const auto png = png_file(source);
    require(be(png, 0, 8) == 0x89504e470d0a1a0aULL, "PNG signature");
    Bytes compressed;
    unsigned width = 0, height = 0;

    for (std::size_t at = 8; at < png.size();) {
        const auto length = static_cast<std::size_t>(be(png, at, 4));
        const auto type = text(png, at + 4, 4);
        require(be(png, at + 8 + length, 4) ==
                        mz_crc32(MZ_CRC32_INIT, png.data() + at + 4, length + 4),
                "PNG chunk CRC");

        if (type == "IHDR") {
            width = static_cast<unsigned>(be(png, at + 8, 4));
            height = static_cast<unsigned>(be(png, at + 12, 4));
            require(png[at + 16] == 8 && png[at + 17] == 2, "8-bit RGB");
        } else if (type == "IDAT") {
            compressed.insert(compressed.end(), png.begin() + at + 8,
                              png.begin() + at + 8 + length);
        }

        at += 12 + length;
    }

    Bytes raw(std::size_t{width} * height * 3 + height);
    auto size = static_cast<mz_ulong>(raw.size());
    require(width == 120 && height == 80 &&
                    mz_uncompress(raw.data(), &size, compressed.data(),
                                  static_cast<mz_ulong>(compressed.size())) == MZ_OK &&
                    size == raw.size(),
            "PNG image data inflates");

    for (std::size_t y = 0; y < height; ++y) {
        require(raw[y * (width * 3 + 1)] == 0, "PNG filter none");
        require(std::equal(source.rgb.begin() + y * width * 3,
                           source.rgb.begin() + (y + 1) * width * 3,
                           raw.begin() + y * (width * 3 + 1) + 1),
                "PNG pixels");
    }
}

struct BoxView {
    std::string type;
    std::span<const std::uint8_t> body;
    std::size_t offset;
};

std::vector<BoxView> boxes(std::span<const std::uint8_t> data, std::size_t base = 0) {
    std::vector<BoxView> result;

    for (std::size_t at = 0; at < data.size();) {
        const auto size = static_cast<std::size_t>(be(data, at, 4));
        require(size >= 8 && at + size <= data.size(), "box size");
        result.push_back({text(data, at + 4, 4), data.subspan(at + 8, size - 8), base + at + 8});
        at += size;
    }

    return result;
}

BoxView child(std::span<const std::uint8_t> data, std::string_view type) {
    for (const auto &item : boxes(data)) {
        if (item.type == type) {
            return item;
        }
    }

    throw std::runtime_error("missing box " + std::string(type));
}

Bytes unescape(std::span<const std::uint8_t> nal) {
    Bytes out;
    unsigned zeros = 0;

    for (std::size_t i = 1; i < nal.size(); ++i) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }

        out.push_back(nal[i]);
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }

    return out;
}

// Decodes the subset of Constrained Baseline the generator uses: I_PCM, Intra 16x16 vertical and
// horizontal prediction without residual, and P_Skip.
VideoFrame decode_picture(std::span<const std::uint8_t> nal, const VideoFrame *reference,
                          unsigned columns, unsigned rows) {
    const unsigned type = nal[0] & 31;
    const bool idr = type == 5;

    require(idr || type == 1, "slice NAL type");
    require(idr == (reference == nullptr), "IDR starts each second");
    const auto rbsp = unescape(nal);
    BitReader r(rbsp);
    require(r.ue() == 0, "one slice");
    require(r.ue() == (idr ? 7u : 5u), "slice type");
    require(r.ue() == 0, "PPS id");
    r.bits(8);

    if (idr) {
        r.ue();
        require(r.bits(2) == 0, "IDR reference marking");
    } else {
        require(r.bits(3) == 0, "default references and sliding window");
    }

    require(r.se() == 0, "slice QP");
    require(r.ue() == 1, "deblocking disabled");
    const std::size_t total_blocks = std::size_t{columns} * rows;
    VideoFrame frame{columns * 16, rows * 16, Bytes(total_blocks * 256), Bytes(total_blocks * 64),
                     Bytes(total_blocks * 64)};
    std::vector<bool> pcm(total_blocks);
    const auto planes = [&](VideoFrame &f) {
        return std::array<std::pair<Bytes *, unsigned>, 3>{{{&f.y, 16}, {&f.cb, 8}, {&f.cr, 8}}};
    };
    const auto read_pcm = [&](unsigned mb) {
        r.align();

        for (const auto &[plane, size] : planes(frame)) {
            for (unsigned y = 0; y < size; ++y) {
                for (unsigned x = 0; x < size; ++x) {
                    (*plane)[((mb / columns) * size + y) * columns * size + (mb % columns) * size +
                             x] = static_cast<std::uint8_t>(r.bits(8));
                }
            }
        }

        pcm[mb] = true;
    };
    const auto copy = [&](unsigned mb) {
        auto source = planes(const_cast<VideoFrame &>(*reference));
        auto target = planes(frame);

        for (unsigned p = 0; p < 3; ++p) {
            const unsigned size = target[p].second;

            for (unsigned y = 0; y < size; ++y) {
                for (unsigned x = 0; x < size; ++x) {
                    const auto at = ((mb / columns) * size + y) * columns * size +
                                    (mb % columns) * size + x;
                    (*target[p].first)[at] = (*source[p].first)[at];
                }
            }
        }
    };
    const unsigned total = columns * rows;

    if (idr) {
        for (unsigned mb = 0; mb < total; ++mb) {
            const auto mb_type = r.ue();

            if (mb_type == 25) {
                read_pcm(mb);
                continue;
            }

            require(mb_type == 1 || mb_type == 2, "Intra 16x16 without coded blocks");
            const bool vertical = mb_type == 1;
            require(r.ue() == (vertical ? 2u : 1u), "chroma prediction follows luma");
            require(r.se() == 0, "macroblock QP delta");
            const unsigned mx = mb % columns, my = mb / columns;
            const bool left = mx > 0, top = my > 0;
            require(vertical ? top : left, "prediction neighbour available");
            const unsigned na = left && pcm[mb - 1] ? 16 : 0,
                           nb = top && pcm[mb - columns] ? 16 : 0;
            const unsigned nc = left && top ? (na + nb + 1) / 2 : left ? na : nb;
            require(nc >= 8 ? r.bits(6) == 0b000011 : r.bit() == 1, "empty DC coeff_token");

            for (const auto &[plane, size] : planes(frame)) {
                const unsigned width = columns * size;

                for (unsigned y = 0; y < size; ++y) {
                    for (unsigned x = 0; x < size; ++x) {
                        const unsigned px = mx * size + x, py = my * size + y;
                        (*plane)[py * width + px] = vertical
                                                            ? (*plane)[(my * size - 1) * width + px]
                                                            : (*plane)[py * width + mx * size - 1];
                    }
                }
            }
        }
    } else {
        for (unsigned mb = 0; mb < total;) {
            for (auto run = r.ue(); run > 0; --run) {
                require(mb < total, "skip run inside the picture");
                copy(mb++);
            }

            if (mb == total) {
                break;
            }

            require(r.ue() == 30, "I_PCM in a P slice");
            read_pcm(mb++);
        }
    }

    require(r.rest_is_trailing(), "slice trailing bits");

    return frame;
}

void test_mp4(VideoPattern pattern) {
    constexpr unsigned seconds = 2;
    const auto file = mp4_file(pattern, seconds);
    const auto top = boxes(file);

    require(top.size() == 3 && top[0].type == "ftyp" && top[1].type == "moov" &&
                    top[2].type == "mdat",
            "ftyp, moov before mdat");
    const auto &mdat = top[2];
    unsigned tracks = 0;
    std::optional<std::pair<Bytes, Bytes>> parameter_sets;

    for (const auto &trak : boxes(top[1].body)) {
        if (trak.type != "trak") {
            continue;
        }

        ++tracks;
        const auto mdia = child(trak.body, "mdia");
        const auto handler = text(child(mdia.body, "hdlr").body, 8, 4);
        const auto stbl = child(child(mdia.body, "minf").body, "stbl");
        const auto stsd = child(stbl.body, "stsd").body;
        const auto stsz = child(stbl.body, "stsz").body;
        const auto stco = child(stbl.body, "stco").body;
        const auto stts = child(stbl.body, "stts").body;
        const auto count = be(stsz, 8, 4);
        require(be(stco, 4, 4) == count && be(stts, 8, 4) == count, "sample tables agree");
        std::vector<std::span<const std::uint8_t>> samples;

        for (std::uint64_t i = 0; i < count; ++i) {
            const auto offset = static_cast<std::size_t>(be(stco, 8 + 4 * i, 4));
            const auto size = static_cast<std::size_t>(be(stsz, 12 + 4 * i, 4));
            require(offset >= mdat.offset && offset + size <= mdat.offset + mdat.body.size(),
                    "sample inside mdat");
            samples.push_back(std::span(file).subspan(offset, size));
        }

        if (handler == "vide") {
            require(count == std::uint64_t{seconds} * video_fps && be(stts, 12, 4) == 512,
                    "25 fps video samples");
            const auto entry = boxes(stsd.subspan(8));
            require(entry.size() == 1 && entry[0].type == "avc1", "avc1 entry");
            require(be(entry[0].body, 24, 2) == video_width &&
                            be(entry[0].body, 26, 2) == video_height,
                    "avc1 dimensions");
            const auto avcc = child(entry[0].body.subspan(78), "avcC").body;
            const auto sps_size = be(avcc, 6, 2);
            const Bytes sps(avcc.begin() + 8, avcc.begin() + 8 + sps_size);
            const auto pps_size = be(avcc, 9 + sps_size, 2);
            const Bytes pps(avcc.begin() + 11 + sps_size, avcc.begin() + 11 + sps_size + pps_size);
            require((sps[0] & 31) == 7 && (pps[0] & 31) == 8, "SPS and PPS NAL types");
            const auto sps_rbsp = unescape(sps);
            BitReader r(sps_rbsp);
            require(r.bits(8) == 66 && (r.bits(8) & 0xc0) == 0xc0 && r.bits(8) == 30,
                    "Constrained Baseline level 3.0");
            r.ue();
            const auto frame_bits = r.ue() + 4;
            require(frame_bits == 8 && r.ue() == 2, "frame_num width and POC type 2");
            r.ue();
            r.bit();
            const auto columns = r.ue() + 1, rows = r.ue() + 1;
            require(columns * 16 == video_width && rows * 16 == video_coded_height,
                    "SPS macroblock size");
            require(r.bit() == 1 && r.bit() == 1 && r.bit() == 1, "frames, cropping");
            require(r.ue() == 0 && r.ue() == 0 && r.ue() == 0 &&
                            rows * 16 - 2 * r.ue() == video_height,
                    "SPS crops to the display height");
            const auto stss = child(stbl.body, "stss").body;
            require(be(stss, 4, 4) == seconds && be(stss, 8, 4) == 1 &&
                            be(stss, 12, 4) == video_fps + 1,
                    "one sync sample per second");
            VideoFrame previous;

            for (std::size_t i = 0; i < samples.size(); ++i) {
                const auto &sample = samples[i];
                const auto length = be(sample, 0, 4);
                require(length + 4 == sample.size(), "one length-prefixed NAL per sample");
                const auto decoded = decode_picture(
                        sample.subspan(4), i % video_fps ? &previous : nullptr, columns, rows);
                const auto expected = video_frame(pattern, static_cast<unsigned>(i));
                require(decoded.y == expected.y && decoded.cb == expected.cb &&
                                decoded.cr == expected.cr,
                        "decoded frame " + std::to_string(i) + " equals the pattern");
                previous = decoded;
            }

            parameter_sets.emplace(sps, pps);
        } else {
            require(handler == "soun", "sound handler");
            require(be(stts, 12, 4) == 1024 && count == (seconds * audio_rate + 1023) / 1024,
                    "AAC frames cover the duration");
            const auto entry = boxes(stsd.subspan(8));
            require(entry.size() == 1 && entry[0].type == "mp4a", "mp4a entry");
            const auto esds = child(entry[0].body.subspan(28), "esds").body;
            require(esds[4] == 3, "ES descriptor");
            const Bytes config{5, 2, 0x12, 0x08};
            require(std::search(esds.begin(), esds.end(), config.begin(), config.end()) !=
                            esds.end(),
                    "AAC LC 44.1 kHz mono configuration");

            for (const auto &sample : samples) {
                BitReader r(sample);
                require(r.bits(3) == 0 && r.bits(4) == 0, "single channel element");
                r.bits(8);
                require(r.bit() == 0 && r.bits(2) == 0 && r.bit() == 0 && r.bits(6) == 0,
                        "long window without scale factor bands");
                require(r.bits(4) == 0, "no prediction, pulse, TNS or gain control");
                require(r.bits(3) == 7, "END element");
            }
        }
    }

    require(tracks == 2 && parameter_sets, "video and audio tracks");
}

void test_media_set() {
    const auto first = test_media(), second = test_media();

    require(first.size() == second.size(), "stable file count");
    std::size_t total = 0;

    for (std::size_t i = 0; i < first.size(); ++i) {
        require(first[i].path == second[i].path && first[i].bytes == second[i].bytes,
                "deterministic file " + first[i].path);
        total += first[i].bytes.size();
        std::cout << first[i].path << " " << first[i].bytes.size() << "\n";
    }

    require(total < 40 * 1024 * 1024, "test media stay below 40 MiB");
    const auto has = [&](std::string_view path) {
        return std::any_of(first.begin(), first.end(), [&](const File &file) {
            return file.path == path;
        });
    };
    require(has("Music/Voyemu Test Tones/03 - Ми 659.flac") &&
                    has("Music/Voyemu Test Tones/cover.jpg") &&
                    has("Video/Voyemu Colour Bars.mp4") && has("Photos/Voyemu Grid.png"),
            "folders of a user's stick");
    const auto stream = decode_flac(first[2].bytes);
    require(stream.tags.at("TITLE") == "Ми 659" && stream.total == std::uint64_t{20} * audio_rate,
            "Cyrillic tag of a 20 s track");
    const auto photo = decode_jpeg(first[7].bytes);
    require(photo.width == 1920 && photo.height == 1080 &&
                    psnr(photo, colour_bars_image(1920, 1080)) > 30,
            "full-size colour bars photo");
}
} // namespace

int main() try {
    test_tone_and_flac();
    test_wav();
    test_jpeg_png();
    test_mp4(VideoPattern::colour_bars);
    test_mp4(VideoPattern::checker);
    test_media_set();
    std::cout << "USB drive media passed\n";

    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
