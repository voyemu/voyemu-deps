#include "usb_drive_media.hpp"
#include "error.hpp"
#include "os/digest.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <miniz.h>
#include <string_view>

namespace voyemu::usb_drive {
namespace {
using Bytes = std::vector<std::uint8_t>;

// MSB-first bit writer of the FLAC, H.264 and AAC bit streams.
class BitWriter {
public:
    void bits(std::uint64_t value, unsigned count) {
        for (unsigned i = count; i-- > 0;) {
            bit((value >> i) & 1);
        }
    }

    void bit(unsigned value) {
        current_ = static_cast<std::uint8_t>(current_ << 1 | (value & 1));

        if (++used_ == 8) {
            bytes_.push_back(current_);
            current_ = 0;
            used_ = 0;
        }
    }

    void ue(std::uint32_t value) {
        const std::uint64_t coded = std::uint64_t{value} + 1;
        unsigned width = 0;

        while ((coded >> width) > 1) {
            ++width;
        }

        bits(0, width);
        bits(coded, width + 1);
    }

    void se(std::int32_t value) {
        ue(value > 0 ? 2 * std::uint32_t(value) - 1 : 2 * std::uint32_t(-value));
    }

    bool aligned() const {
        return used_ == 0;
    }

    void align_zero() {
        while (used_) {
            bit(0);
        }
    }

    // rbsp_trailing_bits: a stop bit and zero alignment.
    void trailing() {
        bit(1);
        align_zero();
    }

    Bytes take() {
        align_zero();

        return std::move(bytes_);
    }

    const Bytes &bytes() const {
        return bytes_;
    }

private:
    Bytes bytes_;
    std::uint8_t current_{};
    unsigned used_{};
};

void be(Bytes &out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = bytes; i-- > 0;) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void le(Bytes &out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void append(Bytes &out, std::span<const std::uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void append(Bytes &out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

// sin(x) for x in [0, pi/2] in Q30 by its Taylor series, integer only.
std::int64_t sine_q30(std::int64_t x) {
    std::int64_t sum = x, term = x;

    for (std::int64_t k = 1; k <= 7; ++k) {
        term = -((term * x) >> 30);
        term = ((term * x) >> 30) / ((2 * k) * (2 * k + 1));
        sum += term;
    }

    return sum;
}

// A quarter sine wave in Q15 with one guard entry for interpolation.
const std::array<std::int32_t, 1025> &quarter_sine() {
    static const auto table = [] {
        std::array<std::int32_t, 1025> result{};
        constexpr std::int64_t half_pi_q30 = 1686629713;

        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = static_cast<std::int32_t>(
                    (sine_q30(half_pi_q30 * std::int64_t(i) / 1024) + (1 << 14)) >> 15);
        }

        return result;
    }();

    return table;
}

// sin of a 32-bit phase in Q15.
std::int32_t sine(std::uint32_t phase) {
    const auto &table = quarter_sine();
    const std::uint32_t quadrant = phase >> 30, within = phase & 0x3fffffff;
    const std::uint32_t position = quadrant & 1 ? 0x40000000 - within : within;
    const std::uint32_t index = position >> 20, fraction = position & 0xfffff;
    const std::int64_t low = table[index], high = table[std::min<std::uint32_t>(index + 1, 1024)];
    const auto value = static_cast<std::int32_t>(low + (((high - low) * fraction) >> 20));

    return quadrant >= 2 ? -value : value;
}

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

// The residual of the FLAC fixed predictor of an order at sample n >= order.
std::int64_t fixed_residual(std::span<const std::int32_t> x, std::size_t n, unsigned order) {
    switch (order) {
    case 0:
        return x[n];
    case 1:
        return std::int64_t{x[n]} - x[n - 1];
    case 2:
        return std::int64_t{x[n]} - 2LL * x[n - 1] + x[n - 2];
    case 3:
        return std::int64_t{x[n]} - 3LL * x[n - 1] + 3LL * x[n - 2] - x[n - 3];
    default:
        return std::int64_t{x[n]} - 4LL * x[n - 1] + 6LL * x[n - 2] - 4LL * x[n - 3] + x[n - 4];
    }
}

// One FIXED subframe of the order with the smallest residual, one Rice partition.
void flac_subframe(BitWriter &out, std::span<const std::int32_t> x) {
    unsigned best_order = 0;
    std::uint64_t best_sum = UINT64_MAX;

    for (unsigned order = 0; order <= std::min<std::size_t>(4, x.size() - 1); ++order) {
        std::uint64_t sum = 0;

        for (std::size_t n = order; n < x.size(); ++n) {
            sum += static_cast<std::uint64_t>(std::llabs(fixed_residual(x, n, order)));
        }

        if (sum < best_sum) {
            best_sum = sum;
            best_order = order;
        }
    }

    std::vector<std::uint32_t> folded;

    for (std::size_t n = best_order; n < x.size(); ++n) {
        const auto r = fixed_residual(x, n, best_order);
        folded.push_back(static_cast<std::uint32_t>(r >= 0 ? 2 * r : -2 * r - 1));
    }

    unsigned parameter = 0;
    std::uint64_t smallest = UINT64_MAX;

    for (unsigned k = 0; k <= 14; ++k) {
        std::uint64_t size = 0;

        for (const auto u : folded) {
            size += (u >> k) + 1 + k;
        }

        if (size < smallest) {
            smallest = size;
            parameter = k;
        }
    }

    out.bit(0);
    out.bits(0b001000 | best_order, 6);
    out.bit(0);

    for (unsigned n = 0; n < best_order; ++n) {
        out.bits(static_cast<std::uint16_t>(x[n]), 16);
    }

    out.bits(0, 2);
    out.bits(0, 4);
    out.bits(parameter, 4);

    for (const auto u : folded) {
        for (std::uint32_t q = u >> parameter; q > 0; --q) {
            out.bit(0);
        }

        out.bit(1);
        out.bits(u, parameter);
    }
}

void flac_metadata(Bytes &out, unsigned type, bool last, std::span<const std::uint8_t> body) {
    out.push_back(static_cast<std::uint8_t>((last ? 0x80 : 0) | type));
    be(out, body.size(), 3);
    append(out, body);
}

Bytes flac_streaminfo(std::size_t samples, unsigned block, std::size_t min_frame,
                      std::size_t max_frame, const std::array<std::uint8_t, 16> &md5) {
    BitWriter info;

    info.bits(block, 16);
    info.bits(block, 16);
    info.bits(min_frame, 24);
    info.bits(max_frame, 24);
    info.bits(audio_rate, 20);
    info.bits(audio_channels - 1, 3);
    info.bits(15, 5);
    info.bits(samples, 36);

    for (const auto byte : md5) {
        info.bits(byte, 8);
    }

    return info.take();
}

// The JPEG sizes of a front cover: its SOF0 height and width.
std::pair<unsigned, unsigned> jpeg_size(std::span<const std::uint8_t> jpeg) {
    for (std::size_t i = 2; i + 9 < jpeg.size();) {
        if (jpeg[i] != 0xff) {
            break;
        }

        const unsigned marker = jpeg[i + 1], length = jpeg[i + 2] << 8 | jpeg[i + 3];

        if (marker == 0xc0) {
            return {jpeg[i + 5] << 8 | jpeg[i + 6], jpeg[i + 7] << 8 | jpeg[i + 8]};
        }

        i += 2 + length;
    }

    throw VoyemuError("USB_DRIVE_MEDIA", "cover is not a baseline JPEG");
}

// ITU-T T.81 Annex K tables and the zigzag order.
constexpr std::array<std::uint8_t, 64> zigzag{
        0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
        41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
        30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
constexpr std::array<std::uint8_t, 64> luminance_quantization{
        16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
        14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
        18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
        49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<std::uint8_t, 64> chrominance_quantization{
        17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
        99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
struct HuffmanSpec {
    std::array<std::uint8_t, 16> counts;
    // The symbols in code order; the counts give how many are used.
    std::array<std::uint8_t, 162> values;
};
constexpr HuffmanSpec dc_luminance{{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
                                   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
constexpr HuffmanSpec dc_chrominance{{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
                                     {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
constexpr HuffmanSpec ac_luminance{
        {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
        {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
         0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
         0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
         0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
         0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
         0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
         0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
         0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
         0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
         0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
         0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};
constexpr HuffmanSpec ac_chrominance{
        {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
        {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
         0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
         0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
         0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
         0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
         0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
         0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
         0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
         0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
         0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
         0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};
// cos(k*pi/16) in Q14 for k = 0..7; the DC basis is cos(pi/4).
constexpr std::array<std::int32_t, 8> cosine_q14{16384, 16069, 15137, 13623,
                                                 11585, 9102,  6270,  3196};

struct HuffmanCode {
    std::array<std::uint16_t, 256> code{};
    std::array<std::uint8_t, 256> length{};
};

HuffmanCode huffman_code(const HuffmanSpec &spec) {
    HuffmanCode result;
    std::uint16_t code = 0;
    std::size_t k = 0;

    for (unsigned length = 1; length <= 16; ++length) {
        for (unsigned i = 0; i < spec.counts[length - 1]; ++i) {
            result.code[spec.values[k]] = code++;
            result.length[spec.values[k]] = static_cast<std::uint8_t>(length);
            ++k;
        }

        code = static_cast<std::uint16_t>(code << 1);
    }

    return result;
}

// Entropy-coded segment writer with 0xFF byte stuffing.
class JpegBits {
public:
    void bits(std::uint32_t value, unsigned count) {
        for (unsigned i = count; i-- > 0;) {
            current_ = static_cast<std::uint8_t>(current_ << 1 | ((value >> i) & 1));

            if (++used_ == 8) {
                flush_byte();
            }
        }
    }

    Bytes finish() {
        while (used_) {
            bits(1, 1);
        }

        return std::move(bytes_);
    }

private:
    void flush_byte() {
        bytes_.push_back(current_);

        if (current_ == 0xff) {
            bytes_.push_back(0);
        }

        current_ = 0;
        used_ = 0;
    }

    Bytes bytes_;
    std::uint8_t current_{};
    unsigned used_{};
};

std::array<std::uint8_t, 64> scaled_table(const std::array<std::uint8_t, 64> &base,
                                          unsigned quality) {
    quality = std::clamp(quality, 1u, 100u);
    const unsigned scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    std::array<std::uint8_t, 64> result{};

    for (std::size_t i = 0; i < 64; ++i) {
        result[i] = static_cast<std::uint8_t>(std::clamp((base[i] * scale + 50) / 100, 1u, 255u));
    }

    return result;
}

// T[u][x] = C(u) cos((2x+1)u pi/16) in Q14.
std::array<std::array<std::int32_t, 8>, 8> dct_basis() {
    std::array<std::array<std::int32_t, 8>, 8> basis{};

    for (unsigned u = 0; u < 8; ++u) {
        for (unsigned x = 0; x < 8; ++x) {
            if (u == 0) {
                basis[u][x] = cosine_q14[4];
                continue;
            }

            unsigned angle = ((2 * x + 1) * u) % 32;
            std::int32_t sign = 1;

            if (angle > 16) {
                angle = 32 - angle;
            }

            if (angle > 8) {
                angle = 16 - angle;
                sign = -1;
            }

            basis[u][x] = angle == 8 ? 0 : sign * cosine_q14[angle];
        }
    }

    return basis;
}

std::int32_t divide_rounded(std::int64_t value, std::int64_t divisor) {
    return static_cast<std::int32_t>(value >= 0 ? (value + divisor / 2) / divisor
                                                : -((-value + divisor / 2) / divisor));
}

void jpeg_segment(Bytes &out, unsigned marker, std::span<const std::uint8_t> body) {
    out.push_back(0xff);
    out.push_back(static_cast<std::uint8_t>(marker));
    be(out, body.size() + 2, 2);
    append(out, body);
}

void huffman_segment(Bytes &out, unsigned table_class, unsigned id, const HuffmanSpec &spec) {
    Bytes body{static_cast<std::uint8_t>(table_class << 4 | id)};

    std::size_t used = 0;

    for (const auto count : spec.counts) {
        used += count;
    }

    append(body, spec.counts);
    append(body, std::span(spec.values).first(used));
    jpeg_segment(out, 0xc4, body);
}

std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

struct Rgb {
    int r, g, b;
};

void fill(RgbImage &image, unsigned x0, unsigned y0, unsigned x1, unsigned y1, Rgb colour) {
    for (unsigned y = y0; y < std::min(y1, image.height); ++y) {
        for (unsigned x = x0; x < std::min(x1, image.width); ++x) {
            auto *pixel = &image.rgb[(std::size_t{y} * image.width + x) * 3];
            pixel[0] = clamp_byte(colour.r);
            pixel[1] = clamp_byte(colour.g);
            pixel[2] = clamp_byte(colour.b);
        }
    }
}

RgbImage blank(unsigned width, unsigned height) {
    return {width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 3)};
}

void png_chunk(Bytes &out, std::string_view type, std::span<const std::uint8_t> body) {
    be(out, body.size(), 4);
    const auto start = out.size();
    append(out, type);
    append(out, body);
    be(out, mz_crc32(MZ_CRC32_INIT, out.data() + start, out.size() - start), 4);
}

// MP4 box writer.
Bytes box(std::string_view type, std::span<const std::uint8_t> body) {
    Bytes out;

    be(out, body.size() + 8, 4);
    append(out, type);
    append(out, body);

    return out;
}

Bytes full_box(std::string_view type, unsigned version, unsigned flags,
               std::span<const std::uint8_t> body) {
    Bytes content;

    content.push_back(static_cast<std::uint8_t>(version));
    be(content, flags, 3);
    append(content, body);

    return box(type, content);
}

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;

    for (const auto &part : parts) {
        append(out, part);
    }

    return out;
}

void matrix(Bytes &out) {
    for (const std::uint32_t value :
         {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) {
        be(out, value, 4);
    }
}

// Adds emulation prevention bytes to an RBSP.
Bytes nal(unsigned reference, unsigned type, std::span<const std::uint8_t> rbsp) {
    Bytes out{static_cast<std::uint8_t>(reference << 5 | type)};
    unsigned zeros = 0;

    for (const auto byte : rbsp) {
        if (zeros >= 2 && byte <= 3) {
            out.push_back(3);
            zeros = 0;
        }

        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }

    return out;
}

// BT.601 limited-range colour of an RGB pixel.
struct Yuv {
    int y, cb, cr;
};

Yuv yuv(Rgb c) {
    return {16 + ((66 * c.r + 129 * c.g + 25 * c.b + 128) >> 8),
            128 + ((-38 * c.r - 74 * c.g + 112 * c.b + 128) >> 8),
            128 + ((112 * c.r - 94 * c.g - 18 * c.b + 128) >> 8)};
}

constexpr std::array<Rgb, 8> bars{{{235, 235, 235},
                                   {235, 235, 16},
                                   {16, 235, 235},
                                   {16, 235, 16},
                                   {235, 16, 235},
                                   {235, 16, 16},
                                   {16, 16, 235},
                                   {16, 16, 16}}};

// The colour of a test video pixel: still backgrounds and one moving square each.
Rgb video_pixel(VideoPattern pattern, unsigned index, unsigned x, unsigned y) {
    const auto bounce = [](unsigned position, unsigned span) {
        const unsigned period = 2 * span, phase = position % period;

        return phase < span ? phase : period - phase;
    };

    if (pattern == VideoPattern::colour_bars) {
        const unsigned square = bounce(index * 8, video_width - 32);

        if (y >= 304 && y < 336 && x >= square && x < square + 32) {
            return {235, 235, 235};
        }

        return y < 288 ? bars[x / (video_width / 8)] : Rgb{40, 40, 48};
    }

    const unsigned sx = bounce(index * 6, video_width - 48),
                   sy = bounce(index * 4, video_coded_height - 48);

    if (x >= sx && x < sx + 48 && y >= sy && y < sy + 48) {
        return {40, 200, 80};
    }

    return (x / 64 + y / 64) % 2 ? Rgb{240, 140, 30} : Rgb{30, 70, 180};
}

constexpr unsigned mb_columns = video_width / 16, mb_rows = video_coded_height / 16;

// Copies one macroblock of a frame: 256 luma samples, then 64 Cb and 64 Cr, raster order.
std::array<std::uint8_t, 384> macroblock(const VideoFrame &frame, unsigned mx, unsigned my) {
    std::array<std::uint8_t, 384> out{};
    std::size_t k = 0;

    for (unsigned y = 0; y < 16; ++y) {
        for (unsigned x = 0; x < 16; ++x) {
            out[k++] = frame.y[(my * 16 + y) * frame.width + mx * 16 + x];
        }
    }

    for (const auto *plane : {&frame.cb, &frame.cr}) {
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                out[k++] = (*plane)[(my * 8 + y) * (frame.width / 2) + mx * 8 + x];
            }
        }
    }

    return out;
}

// Whether a macroblock equals Intra 16x16 vertical (or horizontal) prediction from its neighbour,
// for luma and both chroma planes.
bool predicted(const VideoFrame &frame, unsigned mx, unsigned my, bool vertical) {
    const auto plane_matches = [&](const std::vector<std::uint8_t> &plane, unsigned width,
                                   unsigned size) {
        const unsigned x0 = mx * size, y0 = my * size;

        for (unsigned y = 0; y < size; ++y) {
            for (unsigned x = 0; x < size; ++x) {
                const auto reference = vertical ? plane[(y0 - 1) * width + x0 + x]
                                                : plane[(y0 + y) * width + x0 - 1];

                if (plane[(y0 + y) * width + x0 + x] != reference) {
                    return false;
                }
            }
        }

        return true;
    };

    return plane_matches(frame.y, frame.width, 16) && plane_matches(frame.cb, frame.width / 2, 8) &&
           plane_matches(frame.cr, frame.width / 2, 8);
}

void pcm(BitWriter &out, const VideoFrame &frame, unsigned mx, unsigned my) {
    out.align_zero();

    for (const auto sample : macroblock(frame, mx, my)) {
        out.bits(sample, 8);
    }
}

Bytes sequence_parameter_set() {
    BitWriter out;

    out.bits(66, 8);
    out.bits(0xc0, 8);
    out.bits(30, 8);
    out.ue(0);
    out.ue(4);
    out.ue(2);
    out.ue(1);
    out.bit(0);
    out.ue(mb_columns - 1);
    out.ue(mb_rows - 1);
    out.bit(1);
    out.bit(1);
    out.bit(1);
    out.ue(0);
    out.ue(0);
    out.ue(0);
    out.ue((video_coded_height - video_height) / 2);
    out.bit(0);
    out.trailing();

    return nal(3, 7, out.take());
}

Bytes picture_parameter_set() {
    BitWriter out;

    out.ue(0);
    out.ue(0);
    out.bit(0);
    out.bit(0);
    out.ue(0);
    out.ue(0);
    out.ue(0);
    out.bit(0);
    out.bits(0, 2);
    out.se(0);
    out.se(0);
    out.se(0);
    out.bit(1);
    out.bit(0);
    out.bit(0);
    out.trailing();

    return nal(3, 8, out.take());
}

// One coded picture: an IDR of Intra 16x16 prediction and I_PCM macroblocks, or a P picture of
// P_Skip and I_PCM macroblocks where the frame differs from the previous one.
Bytes coded_picture(const VideoFrame &frame, const VideoFrame *previous, unsigned frame_num,
                    unsigned idr_id) {
    BitWriter out;
    const bool idr = previous == nullptr;

    out.ue(0);
    out.ue(idr ? 7 : 5);
    out.ue(0);
    out.bits(frame_num, 8);

    if (idr) {
        out.ue(idr_id);
    } else {
        out.bit(0);
        out.bit(0);
    }

    if (idr) {
        out.bit(0);
        out.bit(0);
    } else {
        out.bit(0);
    }

    out.se(0);
    out.ue(1);
    std::vector<bool> pcm_blocks(std::size_t{mb_columns} * mb_rows);
    unsigned skip = 0;

    for (unsigned my = 0; my < mb_rows; ++my) {
        for (unsigned mx = 0; mx < mb_columns; ++mx) {
            if (!idr) {
                if (macroblock(frame, mx, my) == macroblock(*previous, mx, my)) {
                    ++skip;
                    continue;
                }

                out.ue(skip);
                skip = 0;
                out.ue(5 + 25);
                pcm(out, frame, mx, my);
                continue;
            }

            const bool vertical = my > 0 && predicted(frame, mx, my, true);
            const bool horizontal = !vertical && mx > 0 && predicted(frame, mx, my, false);

            if (!vertical && !horizontal) {
                out.ue(25);
                pcm(out, frame, mx, my);
                pcm_blocks[my * mb_columns + mx] = true;
                continue;
            }

            out.ue(vertical ? 1 : 2);
            out.ue(vertical ? 2 : 1);
            out.se(0);
            // coeff_token of the empty Intra16x16DCLevel block: nN is 16 beside an I_PCM block.
            const bool left = mx > 0, top = my > 0;
            const unsigned na = left && pcm_blocks[my * mb_columns + mx - 1] ? 16 : 0,
                           nb = top && pcm_blocks[(my - 1) * mb_columns + mx] ? 16 : 0;
            const unsigned nc = left && top ? (na + nb + 1) / 2 : left ? na : nb;

            if (nc >= 8) {
                out.bits(0b000011, 6);
            } else {
                out.bit(1);
            }
        }
    }

    if (skip) {
        out.ue(skip);
    }

    out.trailing();

    return nal(idr ? 3 : 2, idr ? 5 : 1, out.take());
}

// A silent AAC-LC mono raw_data_block: one SCE with max_sfb 0, then END.
Bytes silent_aac_frame() {
    BitWriter out;

    out.bits(0, 3);
    out.bits(0, 4);
    out.bits(100, 8);
    out.bit(0);
    out.bits(0, 2);
    out.bit(0);
    out.bits(0, 6);
    out.bit(0);
    out.bit(0);
    out.bit(0);
    out.bit(0);
    out.bits(7, 3);

    return out.take();
}

Bytes sample_table(std::span<const Bytes> samples, std::uint32_t delta, const Bytes &description,
                   std::span<const std::uint32_t> sync, std::span<const std::uint64_t> offsets) {
    Bytes stts, stsc, stsz, stco, stss, stsd;

    be(stts, 1, 4);
    be(stts, samples.size(), 4);
    be(stts, delta, 4);
    be(stsc, 1, 4);
    be(stsc, 1, 4);
    be(stsc, 1, 4);
    be(stsc, 1, 4);
    be(stsz, 0, 4);
    be(stsz, samples.size(), 4);

    for (const auto &sample : samples) {
        be(stsz, sample.size(), 4);
    }

    be(stco, offsets.size(), 4);

    for (const auto offset : offsets) {
        be(stco, offset, 4);
    }

    be(stsd, 1, 4);
    append(stsd, description);
    auto table = concat({full_box("stsd", 0, 0, stsd), full_box("stts", 0, 0, stts)});

    if (!sync.empty()) {
        be(stss, sync.size(), 4);

        for (const auto number : sync) {
            be(stss, number, 4);
        }

        append(table, full_box("stss", 0, 0, stss));
    }

    append(table, concat({full_box("stsc", 0, 0, stsc), full_box("stsz", 0, 0, stsz),
                          full_box("stco", 0, 0, stco)}));

    return box("stbl", table);
}

Bytes track(unsigned id, bool video, std::uint32_t timescale, std::uint64_t media_duration,
            std::uint64_t movie_duration, const Bytes &stbl) {
    Bytes tkhd, mdhd, hdlr, url, dref;

    be(tkhd, 0, 8);
    be(tkhd, id, 4);
    be(tkhd, 0, 4);
    be(tkhd, movie_duration, 4);
    be(tkhd, 0, 8);
    be(tkhd, 0, 2);
    be(tkhd, video ? 0 : 1, 2);
    be(tkhd, video ? 0 : 0x0100, 2);
    be(tkhd, 0, 2);
    matrix(tkhd);
    be(tkhd, video ? video_width << 16 : 0, 4);
    be(tkhd, video ? video_height << 16 : 0, 4);
    be(mdhd, 0, 8);
    be(mdhd, timescale, 4);
    be(mdhd, media_duration, 4);
    be(mdhd, 0x55c4, 2);
    be(mdhd, 0, 2);
    be(hdlr, 0, 4);
    append(hdlr, video ? "vide" : "soun");
    be(hdlr, 0, 12);
    append(hdlr, video ? std::string_view("VideoHandler") : std::string_view("SoundHandler"));
    hdlr.push_back(0);
    be(dref, 1, 4);
    append(dref, full_box("url ", 0, 1, url));
    Bytes media_header;

    if (video) {
        media_header = full_box("vmhd", 0, 1, Bytes(8));
    } else {
        media_header = full_box("smhd", 0, 0, Bytes(4));
    }

    const auto minf = box("minf",
                          concat({media_header, box("dinf", full_box("dref", 0, 0, dref)), stbl}));
    const auto mdia =
            box("mdia", concat({full_box("mdhd", 0, 0, mdhd), full_box("hdlr", 0, 0, hdlr), minf}));

    return box("trak", concat({full_box("tkhd", 0, 3, tkhd), mdia}));
}

Bytes avc_description(const Bytes &sps, const Bytes &pps) {
    Bytes avcc{1, sps[1], sps[2], sps[3], 0xff, 0xe1};

    be(avcc, sps.size(), 2);
    append(avcc, sps);
    avcc.push_back(1);
    be(avcc, pps.size(), 2);
    append(avcc, pps);
    Bytes entry(6);

    be(entry, 1, 2);
    be(entry, 0, 16);
    be(entry, video_width, 2);
    be(entry, video_height, 2);
    be(entry, 0x00480000, 4);
    be(entry, 0x00480000, 4);
    be(entry, 0, 4);
    be(entry, 1, 2);
    entry.push_back(6);
    append(entry, std::string_view("Voyemu"));
    entry.resize(entry.size() + 25);
    be(entry, 0x18, 2);
    be(entry, 0xffff, 2);
    append(entry, box("avcC", avcc));

    return box("avc1", entry);
}

Bytes descriptor(unsigned tag, std::span<const std::uint8_t> body) {
    Bytes out{static_cast<std::uint8_t>(tag), static_cast<std::uint8_t>(body.size())};

    append(out, body);

    return out;
}

Bytes aac_description() {
    // AudioSpecificConfig: AAC LC, 44100 Hz, one channel.
    const Bytes config{0x12, 0x08};
    Bytes decoder{0x40, 0x15, 0, 0x01, 0};

    be(decoder, 2000, 4);
    be(decoder, 2000, 4);
    append(decoder, descriptor(5, config));
    Bytes es{0, 2, 0};

    append(es, descriptor(4, decoder));
    append(es, descriptor(6, Bytes{2}));
    Bytes entry(6);

    be(entry, 1, 2);
    be(entry, 0, 8);
    be(entry, 1, 2);
    be(entry, 16, 2);
    be(entry, 0, 4);
    be(entry, std::uint64_t{audio_rate} << 16, 4);
    append(entry, full_box("esds", 0, 0, descriptor(3, es)));

    return box("mp4a", entry);
}
} // namespace

std::vector<std::int16_t> tone(std::uint32_t millihertz, unsigned seconds) {
    const std::size_t frames = std::size_t{audio_rate} * seconds, fade = audio_rate / 2;
    const auto increment = static_cast<std::uint32_t>((std::uint64_t{millihertz} << 32) /
                                                      (std::uint64_t{audio_rate} * 1000));
    std::vector<std::int16_t> samples(frames * audio_channels);
    std::uint32_t phase = 0;

    for (std::size_t n = 0; n < frames; ++n, phase += increment) {
        const std::size_t edge = std::min(n, frames - 1 - n);
        const std::int64_t gain = edge < fade ? std::int64_t(edge) * 1024 / fade : 1024;
        const auto value =
                static_cast<std::int16_t>((std::int64_t{sine(phase)} * 9830 * gain) >> 25);

        for (unsigned c = 0; c < audio_channels; ++c) {
            samples[n * audio_channels + c] = value;
        }
    }

    return samples;
}

std::vector<std::uint8_t> flac_file(std::span<const std::int16_t> samples, const AudioTags &tags,
                                    std::span<const std::uint8_t> cover_jpeg) {
    constexpr unsigned block = 4096;
    const std::size_t frames = samples.size() / audio_channels;
    Bytes audio;
    std::size_t min_frame = SIZE_MAX, max_frame = 0;

    for (std::size_t start = 0, number = 0; start < frames; start += block, ++number) {
        const auto count = static_cast<unsigned>(std::min<std::size_t>(block, frames - start));
        BitWriter frame;

        frame.bits(0xfff8, 16);
        frame.bits(count == block ? 0b1100 : 0b0111, 4);
        frame.bits(0b1001, 4);
        frame.bits(0b0001, 4);
        frame.bits(0b100, 3);
        frame.bit(0);

        if (number < 0x80) {
            frame.bits(number, 8);
        } else {
            frame.bits(0xc0 | number >> 6, 8);
            frame.bits(0x80 | (number & 0x3f), 8);
        }

        if (count != block) {
            frame.bits(count - 1, 16);
        }

        frame.bits(crc8(frame.bytes()), 8);

        for (unsigned c = 0; c < audio_channels; ++c) {
            std::vector<std::int32_t> channel(count);

            for (unsigned i = 0; i < count; ++i) {
                channel[i] = samples[(start + i) * audio_channels + c];
            }

            flac_subframe(frame, channel);
        }

        auto bytes = frame.take();
        be(bytes, crc16(bytes), 2);
        min_frame = std::min(min_frame, bytes.size());
        max_frame = std::max(max_frame, bytes.size());
        append(audio, bytes);
    }

    Bytes comments;
    constexpr std::string_view vendor = "Voyemu";
    le(comments, vendor.size(), 4);
    append(comments, vendor);
    const std::array<std::pair<std::string_view, const std::string *>, 6> fields{
            {{"TITLE", &tags.title},
             {"ARTIST", &tags.artist},
             {"ALBUM", &tags.album},
             {"TRACKNUMBER", &tags.track},
             {"DATE", &tags.date},
             {"GENRE", &tags.genre}}};
    le(comments, fields.size(), 4);

    for (const auto &[key, value] : fields) {
        le(comments, key.size() + 1 + value->size(), 4);
        append(comments, key);
        comments.push_back('=');
        append(comments, *value);
    }

    // STREAMINFO MD5 covers the interleaved little-endian samples.
    os::Md5 md5;
    Bytes little;

    for (const auto sample : samples) {
        le(little, static_cast<std::uint16_t>(sample), 2);
    }

    md5.update(little.data(), little.size());
    Bytes out{'f', 'L', 'a', 'C'};
    flac_metadata(out, 0, false,
                  flac_streaminfo(frames, block, min_frame, max_frame, md5.finish()));
    flac_metadata(out, 4, cover_jpeg.empty(), comments);

    if (!cover_jpeg.empty()) {
        const auto [height, width] = jpeg_size(cover_jpeg);
        Bytes picture;
        constexpr std::string_view mime = "image/jpeg";
        be(picture, 3, 4);
        be(picture, mime.size(), 4);
        append(picture, mime);
        be(picture, 0, 4);
        be(picture, width, 4);
        be(picture, height, 4);
        be(picture, 24, 4);
        be(picture, 0, 4);
        be(picture, cover_jpeg.size(), 4);
        append(picture, cover_jpeg);
        flac_metadata(out, 6, true, picture);
    }

    append(out, audio);

    return out;
}

std::vector<std::uint8_t> wav_file(std::span<const std::int16_t> samples, const AudioTags &tags) {
    Bytes info{'I', 'N', 'F', 'O'};
    const std::array<std::pair<std::string_view, const std::string *>, 6> fields{
            {{"INAM", &tags.title},
             {"IART", &tags.artist},
             {"IPRD", &tags.album},
             {"ITRK", &tags.track},
             {"ICRD", &tags.date},
             {"IGNR", &tags.genre}}};

    for (const auto &[id, value] : fields) {
        append(info, id);
        le(info, value->size() + 1, 4);
        append(info, *value);
        info.push_back(0);

        if ((value->size() + 1) % 2) {
            info.push_back(0);
        }
    }

    Bytes body{'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
    le(body, 16, 4);
    le(body, 1, 2);
    le(body, audio_channels, 2);
    le(body, audio_rate, 4);
    le(body, std::uint64_t{audio_rate} * audio_channels * 2, 4);
    le(body, std::uint64_t{audio_channels} * 2, 2);
    le(body, 16, 2);
    append(body, std::string_view("LIST"));
    le(body, info.size(), 4);
    append(body, info);
    append(body, std::string_view("data"));
    le(body, samples.size() * 2, 4);

    for (const auto sample : samples) {
        le(body, static_cast<std::uint16_t>(sample), 2);
    }

    Bytes out{'R', 'I', 'F', 'F'};
    le(out, body.size(), 4);
    append(out, body);

    return out;
}

std::vector<std::uint8_t> jpeg_file(const RgbImage &image, unsigned quality) {
    if (!image.width || !image.height || image.width > 65535 || image.height > 65535 ||
        image.rgb.size() != std::size_t{image.width} * image.height * 3) {
        throw VoyemuError("USB_DRIVE_MEDIA", "invalid JPEG image");
    }

    const std::array<std::array<std::uint8_t, 64>, 2> tables{
            scaled_table(luminance_quantization, quality),
            scaled_table(chrominance_quantization, quality)};
    Bytes out{0xff, 0xd8};
    jpeg_segment(out, 0xe0, Bytes{'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});

    for (unsigned id = 0; id < 2; ++id) {
        Bytes body{static_cast<std::uint8_t>(id)};

        for (const auto index : zigzag) {
            body.push_back(tables[id][index]);
        }

        jpeg_segment(out, 0xdb, body);
    }

    Bytes frame{8};
    be(frame, image.height, 2);
    be(frame, image.width, 2);
    frame.insert(frame.end(), {3, 1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1});
    jpeg_segment(out, 0xc0, frame);
    huffman_segment(out, 0, 0, dc_luminance);
    huffman_segment(out, 1, 0, ac_luminance);
    huffman_segment(out, 0, 1, dc_chrominance);
    huffman_segment(out, 1, 1, ac_chrominance);
    jpeg_segment(out, 0xda, Bytes{3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0});
    const std::array<HuffmanCode, 4> codes{huffman_code(dc_luminance), huffman_code(ac_luminance),
                                           huffman_code(dc_chrominance),
                                           huffman_code(ac_chrominance)};
    const auto basis = dct_basis();
    JpegBits bits;
    std::array<int, 3> previous_dc{};
    const auto magnitude = [](int value) {
        unsigned size = 0;

        for (unsigned v = static_cast<unsigned>(std::abs(value)); v; v >>= 1) {
            ++size;
        }

        return size;
    };
    const auto put = [&](const HuffmanCode &code, unsigned symbol) {
        bits.bits(code.code[symbol], code.length[symbol]);
    };
    const auto put_value = [&](int value, unsigned size) {
        bits.bits(static_cast<std::uint32_t>(value < 0 ? value + (1 << size) - 1 : value), size);
    };

    for (unsigned by = 0; by < (image.height + 7) / 8; ++by) {
        for (unsigned bx = 0; bx < (image.width + 7) / 8; ++bx) {
            std::array<std::array<int, 64>, 3> blocks{};

            for (unsigned y = 0; y < 8; ++y) {
                for (unsigned x = 0; x < 8; ++x) {
                    const unsigned px = std::min(bx * 8 + x, image.width - 1),
                                   py = std::min(by * 8 + y, image.height - 1);
                    const auto *pixel = &image.rgb[(std::size_t{py} * image.width + px) * 3];
                    const int r = pixel[0], g = pixel[1], b = pixel[2];
                    blocks[0][y * 8 + x] = ((77 * r + 150 * g + 29 * b + 128) >> 8) - 128;
                    blocks[1][y * 8 + x] = (-43 * r - 85 * g + 128 * b + 128) >> 8;
                    blocks[2][y * 8 + x] = (128 * r - 107 * g - 21 * b + 128) >> 8;
                }
            }

            for (unsigned component = 0; component < 3; ++component) {
                const auto &block = blocks[component];
                std::array<std::int64_t, 64> rows{};

                for (unsigned y = 0; y < 8; ++y) {
                    for (unsigned u = 0; u < 8; ++u) {
                        std::int64_t sum = 0;

                        for (unsigned x = 0; x < 8; ++x) {
                            sum += std::int64_t{basis[u][x]} * block[y * 8 + x];
                        }

                        rows[y * 8 + u] = sum;
                    }
                }

                const auto &table = tables[component ? 1 : 0];
                std::array<int, 64> coefficients{};

                for (unsigned v = 0; v < 8; ++v) {
                    for (unsigned u = 0; u < 8; ++u) {
                        std::int64_t sum = 0;

                        for (unsigned y = 0; y < 8; ++y) {
                            sum += std::int64_t{basis[v][y]} * rows[y * 8 + u];
                        }

                        // F(u,v) = sum / 4 / 2^28; quantized with symmetric rounding.
                        coefficients[v * 8 + u] =
                                divide_rounded(sum, (std::int64_t{1} << 30) * table[v * 8 + u]);
                    }
                }

                const auto &dc = codes[component ? 2 : 0];
                const auto &ac = codes[component ? 3 : 1];
                const int difference = coefficients[0] - previous_dc[component];
                previous_dc[component] = coefficients[0];
                const auto dc_size = magnitude(difference);
                put(dc, dc_size);
                put_value(difference, dc_size);
                unsigned run = 0;

                for (unsigned k = 1; k < 64; ++k) {
                    const int value = coefficients[zigzag[k]];

                    if (!value) {
                        ++run;
                        continue;
                    }

                    while (run > 15) {
                        put(ac, 0xf0);
                        run -= 16;
                    }

                    const auto size = magnitude(value);
                    put(ac, run << 4 | size);
                    put_value(value, size);
                    run = 0;
                }

                if (run) {
                    put(ac, 0x00);
                }
            }
        }
    }

    append(out, bits.finish());
    out.push_back(0xff);
    out.push_back(0xd9);

    return out;
}

std::vector<std::uint8_t> png_file(const RgbImage &image) {
    if (!image.width || !image.height ||
        image.rgb.size() != std::size_t{image.width} * image.height * 3) {
        throw VoyemuError("USB_DRIVE_MEDIA", "invalid PNG image");
    }

    Bytes raw;
    raw.reserve(image.rgb.size() + image.height);

    for (unsigned y = 0; y < image.height; ++y) {
        raw.push_back(0);
        const auto *row = &image.rgb[std::size_t{y} * image.width * 3];
        raw.insert(raw.end(), row, row + std::size_t{image.width} * 3);
    }

    auto size = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    Bytes compressed(size);

    if (mz_compress2(compressed.data(), &size, raw.data(), static_cast<mz_ulong>(raw.size()), 6) !=
        MZ_OK) {
        throw VoyemuError("USB_DRIVE_MEDIA", "PNG compression failed");
    }

    compressed.resize(size);
    Bytes header;
    be(header, image.width, 4);
    be(header, image.height, 4);
    header.insert(header.end(), {8, 2, 0, 0, 0});
    Bytes out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    png_chunk(out, "IHDR", header);
    png_chunk(out, "IDAT", compressed);
    png_chunk(out, "IEND", {});

    return out;
}

VideoFrame video_frame(VideoPattern pattern, unsigned index) {
    VideoFrame frame{video_width, video_coded_height, {}, {}, {}};

    frame.y.resize(std::size_t{video_width} * video_coded_height);
    frame.cb.resize(frame.y.size() / 4);
    frame.cr.resize(frame.y.size() / 4);

    for (unsigned y = 0; y < video_coded_height; ++y) {
        for (unsigned x = 0; x < video_width; ++x) {
            frame.y[y * video_width + x] =
                    static_cast<std::uint8_t>(yuv(video_pixel(pattern, index, x, y)).y);
        }
    }

    for (unsigned y = 0; y < video_coded_height / 2; ++y) {
        for (unsigned x = 0; x < video_width / 2; ++x) {
            int cb = 0, cr = 0;

            for (unsigned k = 0; k < 4; ++k) {
                const auto c = yuv(video_pixel(pattern, index, 2 * x + k % 2, 2 * y + k / 2));
                cb += c.cb;
                cr += c.cr;
            }

            frame.cb[y * (video_width / 2) + x] = static_cast<std::uint8_t>((cb + 2) / 4);
            frame.cr[y * (video_width / 2) + x] = static_cast<std::uint8_t>((cr + 2) / 4);
        }
    }

    return frame;
}

std::vector<std::uint8_t> mp4_file(VideoPattern pattern, unsigned seconds) {
    const unsigned frames = seconds * video_fps;
    constexpr std::uint32_t video_timescale = 12800, video_delta = video_timescale / video_fps,
                            aac_frame = 1024;
    std::vector<Bytes> video, audio;
    std::vector<std::uint32_t> sync;
    VideoFrame previous;

    for (unsigned i = 0; i < frames; ++i) {
        const auto frame = video_frame(pattern, i);
        const bool idr = i % video_fps == 0;
        const auto picture = coded_picture(frame, idr ? nullptr : &previous, i % video_fps,
                                           (i / video_fps) % 2);
        Bytes sample;
        be(sample, picture.size(), 4);
        append(sample, picture);
        video.push_back(std::move(sample));

        if (idr) {
            sync.push_back(i + 1);
        }

        previous = frame;
    }

    const std::size_t audio_frames = (std::size_t{seconds} * audio_rate + aac_frame - 1) /
                                     aac_frame;
    const auto silence = silent_aac_frame();
    audio.assign(audio_frames, silence);
    // Interleave by decode time: each sample is its own chunk.
    struct Placed {
        bool video;
        std::size_t index;
    };
    std::vector<Placed> order;

    for (std::size_t v = 0, a = 0; v < video.size() || a < audio.size();) {
        const bool take_video = a == audio.size() ||
                                (v < video.size() &&
                                 std::uint64_t{v} * video_delta * audio_rate <=
                                         std::uint64_t{a} * aac_frame * video_timescale);
        order.push_back({take_video, take_video ? v++ : a++});
    }

    const auto sps = sequence_parameter_set(), pps = picture_parameter_set();
    const auto ftyp = box("ftyp",
                          Bytes{'i', 's', 'o', 'm', 0,   0,   2,   0,   'i', 's', 'o', 'm',
                                'i', 's', 'o', '2', 'a', 'v', 'c', '1', 'm', 'p', '4', '1'});
    const std::uint64_t movie_duration = std::uint64_t{seconds} * 1000;
    const auto build_moov = [&](std::span<const std::uint64_t> video_offsets,
                                std::span<const std::uint64_t> audio_offsets) {
        Bytes mvhd;
        be(mvhd, 0, 8);
        be(mvhd, 1000, 4);
        be(mvhd, movie_duration, 4);
        be(mvhd, 0x00010000, 4);
        be(mvhd, 0x0100, 2);
        be(mvhd, 0, 10);
        matrix(mvhd);
        be(mvhd, 0, 24);
        be(mvhd, 3, 4);

        return box("moov",
                   concat({full_box("mvhd", 0, 0, mvhd),
                           track(1, true, video_timescale, std::uint64_t{frames} * video_delta,
                                 movie_duration,
                                 sample_table(video, video_delta, avc_description(sps, pps), sync,
                                              video_offsets)),
                           track(2, false, audio_rate, audio_frames * aac_frame, movie_duration,
                                 sample_table(audio, aac_frame, aac_description(), {},
                                              audio_offsets))}));
    };
    std::vector<std::uint64_t> video_offsets(video.size()), audio_offsets(audio.size());
    const auto moov_size = build_moov(video_offsets, audio_offsets).size();
    std::uint64_t offset = ftyp.size() + moov_size + 8;
    Bytes data;

    for (const auto &[is_video, index] : order) {
        const auto &sample = is_video ? video[index] : audio[index];
        (is_video ? video_offsets : audio_offsets)[index] = offset;
        offset += sample.size();
        append(data, sample);
    }

    auto out = concat({ftyp, build_moov(video_offsets, audio_offsets), box("mdat", data)});

    if (offset > UINT32_MAX) {
        throw VoyemuError("USB_DRIVE_MEDIA", "video exceeds 32-bit chunk offsets");
    }

    return out;
}

RgbImage colour_bars_image(unsigned width, unsigned height) {
    auto image = blank(width, height);
    const unsigned top = height * 2 / 3;

    for (unsigned i = 0; i < bars.size(); ++i) {
        fill(image, width * i / 8, 0, width * (i + 1) / 8, top, bars[i]);
        fill(image, width * i / 8, top, width * (i + 1) / 8, height,
             {int(255 * i / 7), int(255 * i / 7), int(255 * i / 7)});
    }

    return image;
}

RgbImage gradient_image(unsigned width, unsigned height) {
    auto image = blank(width, height);

    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            auto *pixel = &image.rgb[(std::size_t{y} * width + x) * 3];
            pixel[0] = static_cast<std::uint8_t>(255 * x / std::max(1u, width - 1));
            pixel[1] = static_cast<std::uint8_t>(255 * y / std::max(1u, height - 1));
            pixel[2] = static_cast<std::uint8_t>(255 -
                                                 255 * (x + y) / std::max(1u, width + height - 2));
        }
    }

    return image;
}

RgbImage checker_image(unsigned width, unsigned height) {
    auto image = blank(width, height);
    const unsigned cell = std::max(1u, height / 9);

    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const bool dark = (x / cell + y / cell) % 2;
            const bool diagonal = (x * height / std::max(1u, width) + 4 >= y) &&
                                  (x * height / std::max(1u, width) <= y + 4);
            auto *pixel = &image.rgb[(std::size_t{y} * width + x) * 3];
            const Rgb c = diagonal ? Rgb{220, 30, 40} : dark ? Rgb{25, 25, 30} : Rgb{235, 235, 225};
            pixel[0] = clamp_byte(c.r);
            pixel[1] = clamp_byte(c.g);
            pixel[2] = clamp_byte(c.b);
        }
    }

    return image;
}

RgbImage rings_image(unsigned width, unsigned height) {
    auto image = blank(width, height);
    const int cx = int(width / 2), cy = int(height / 2);

    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const int dx = int(x) - cx, dy = int(y) - cy;
            const unsigned ring = static_cast<unsigned>(dx * dx + dy * dy) / 2400 % 6;
            auto *pixel = &image.rgb[(std::size_t{y} * width + x) * 3];
            const auto &c = bars[ring];
            pixel[0] = clamp_byte(c.r);
            pixel[1] = clamp_byte(c.g);
            pixel[2] = clamp_byte(c.b);
        }
    }

    return image;
}

RgbImage grid_image(unsigned width, unsigned height) {
    auto image = blank(width, height);

    fill(image, 0, 0, width, height, {18, 22, 30});

    for (unsigned x = 0; x < width; x += 40) {
        fill(image, x, 0, x + 2, height, {80, 160, 220});
    }

    for (unsigned y = 0; y < height; y += 40) {
        fill(image, 0, y, width, y + 2, {80, 160, 220});
    }

    fill(image, width / 4, height / 4, width * 3 / 4, height * 3 / 4, {240, 180, 40});

    return image;
}

RgbImage cover_image(unsigned size) {
    auto image = blank(size, size);

    for (unsigned y = 0; y < size; ++y) {
        for (unsigned x = 0; x < size; ++x) {
            auto *pixel = &image.rgb[(std::size_t{y} * size + x) * 3];
            pixel[0] = static_cast<std::uint8_t>(20 + 60 * y / size);
            pixel[1] = static_cast<std::uint8_t>(40 + 120 * x / size);
            pixel[2] = static_cast<std::uint8_t>(120 + 100 * (size - y) / size);
        }
    }

    for (unsigned i = 0; i < 4; ++i) {
        const unsigned inset = size / 10 + i * size / 10;
        const auto &c = bars[(i + 1) % bars.size()];
        fill(image, inset, inset, size - inset, inset + size / 40, c);
        fill(image, inset, size - inset - size / 40, size - inset, size - inset, c);
    }

    return image;
}

std::vector<File> test_media() {
    const std::string album = "Music/Voyemu Test Tones/";
    const auto cover = jpeg_file(cover_image(500), 90);
    const auto tags = [](std::string title, unsigned track) {
        return AudioTags{std::move(title),      "Voyemu", "Voyemu Test Tones",
                         std::to_string(track), "2026",   "Test"};
    };
    std::vector<File> files;

    files.push_back(
            {album + "01 - A440.flac", flac_file(tone(440000, 20), tags("A440", 1), cover)});
    files.push_back(
            {album + "02 - C523.flac", flac_file(tone(523251, 20), tags("C523", 2), cover)});
    files.push_back(
            {album + "03 - Ми 659.flac", flac_file(tone(659255, 20), tags("Ми 659", 3), cover)});
    files.push_back({album + "04 - G784.wav", wav_file(tone(783991, 20), tags("G784", 4))});
    files.push_back({album + "cover.jpg", cover});
    files.push_back({"Video/Voyemu Colour Bars.mp4", mp4_file(VideoPattern::colour_bars, 10)});
    files.push_back({"Video/Voyemu Checker.mp4", mp4_file(VideoPattern::checker, 10)});
    files.push_back({"Photos/2026-11-22 12.00.01 Colour Bars.jpg",
                     jpeg_file(colour_bars_image(1920, 1080), 90)});
    files.push_back(
            {"Photos/2026-11-22 12.00.02 Gradient.jpg", jpeg_file(gradient_image(1920, 1080), 90)});
    files.push_back(
            {"Photos/2026-11-22 12.00.03 Checker.jpg", jpeg_file(checker_image(1920, 1080), 90)});
    files.push_back({"Photos/Voyemu Rings.png", png_file(rings_image(1280, 720))});
    files.push_back({"Photos/Voyemu Grid.png", png_file(grid_image(1280, 720))});

    return files;
}
} // namespace voyemu::usb_drive
