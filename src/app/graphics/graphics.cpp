#include "graphics.hpp"
#include "check.hpp"
#include "egl_handles.hpp"
#include "error.hpp"
#include "graphics_protocol.hpp"
#include "graphics_ram.hpp"
#include "logging.hpp"
#include "model.hpp"
#include "os/fs.hpp"
#include "os/library.hpp"
#include "os/platform.hpp"
#include "os/process.hpp"
#include "process.hpp"
#include "project_files.hpp"
#include "qnx_mcu_projection.hpp"
#include "qnx_qinggan11_mcu_layout.hpp"
#include "toolchain_manifest.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#define GL_CALL __stdcall
#else
#define GL_CALL
#endif
namespace voyemu::graphics {
namespace {
using Json = nlohmann::json;
using U = std::uint32_t;
using I = std::int32_t;
using V = void *;
using GLenum = U;
using GLuint = U;
using GLint = I;
using GLsizei = I;
using GLfloat = float;
using GLboolean = std::uint8_t;
using GLbitfield = U;
using GLintptr = std::intptr_t;
using GLsizeiptr = std::intptr_t;
using GLchar = char;
using detail::clear_vector;
using detail::max_blob;
using detail::u32;
using detail::u64;
using detail::validate_inline_clear;

std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

float float_bits(std::uint64_t v) {
    return std::bit_cast<float>(U(v));
}

I signed_bits(std::uint64_t v) {
    return std::bit_cast<I>(U(v));
}

template <class T> std::uint64_t encode_result(T v) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<std::uintptr_t>(v);
    } else {
        return static_cast<std::uint64_t>(v);
    }
}

constexpr Check need{"GRAPHICS"};

template <class T> void append(std::vector<std::byte> &out, T value) {
    auto at = out.size();

    out.resize(at + sizeof(value));
    std::memcpy(out.data() + at, &value, sizeof(value));
}

std::vector<std::byte> bytes_of(std::string_view value) {
    std::vector<std::byte> out(value.size());

    std::memcpy(out.data(), value.data(), value.size());

    return out;
}

std::string hex_address(std::uint64_t v) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;

    do {
        out.insert(out.begin(), digits[v & 15]);
        v >>= 4;
    } while (v);

    return out;
}

struct Dyn {
    os::Library library;
    std::unordered_map<std::string, void *> symbols;

    explicit Dyn(const std::filesystem::path &path) : library(path) {
        need(library.loaded(), "Pinned ANGLE load failed");
    }

    template <class Fn> Fn symbol(const char *name) {
        auto [it, inserted] = symbols.try_emplace(name, nullptr);

        if (inserted) {
            it->second = library.symbol(name);
        }

        need(it->second != nullptr, "ANGLE symbol unavailable");

        return reinterpret_cast<Fn>(it->second);
    }

    template <class R, class... A> R call(const char *name, A... args) {
        return symbol<R(GL_CALL *)(A...)>(name)(args...);
    }
};
using GeneratedSpec = detail::CommandSpec;
struct GeneratedInvoker {
    Dyn &gl;

    template <class F> F symbol(const char *n) {
        return gl.symbol<F>(n);
    }

    std::uint64_t invoke(U, const Arguments &);
};
#include "generated_graphics.inc"

const GeneratedSpec &spec_for(U op) {
    need(op >= 100 && op <= 195, "Unsupported generated opcode");

    return generated_specs[op - 100];
}

std::size_t count_bytes(std::uint64_t count, std::size_t unit) {
    need(count <= max_blob / unit, "Typed input extent");

    return std::size_t(count) * unit;
}

std::string file_sha256(const std::filesystem::path &path) {
    need(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path),
         "ANGLE library ownership");

    return sha256_file(path);
}

Json read_json(const std::filesystem::path &path, std::size_t maximum) {
    need(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path),
         "Control file ownership");
    const auto size = std::filesystem::file_size(path);
    need(size > 0 && size <= maximum, "Control file extent");

    return Json::parse(detail::read_control_file(path, size));
}

// Exclusive file creation makes producer claiming atomic across worker processes.
bool exclusive_json(const std::filesystem::path &path, const Json &value) {
    const auto text = value.dump() + "\n";
    os::ExclusiveFile file(path, true);

    if (!file.open()) {
        need(file.already_exists(), "Owner marker create");

        return false;
    }

    const bool okay = file.write(text.data(), text.size()) && file.sync();
    file.close();
    need(okay, "Owner marker write");

    return true;
}

void replace_json(const std::filesystem::path &path, const Json &value, std::uint64_t address) {
    need(!std::filesystem::is_symlink(path), "Receipt symlink");
    const auto pending = std::filesystem::path(path.string() + ".pending-" + hex_address(address));
    need(exclusive_json(pending, value), "Receipt pending ownership");

    if (!os::replace_file(pending, path)) {
        std::filesystem::remove(pending);
        throw VoyemuError("GRAPHICS", "Receipt replace");
    }
}

bool claim(const std::filesystem::path &path, std::uint64_t address, const char *source) {
    if (exclusive_json(path,
                       Json{{"schema", 1}, {"producer_address", address}, {"source", source}})) {
        return true;
    }

    // Another exclusive creator may still be writing; never consume its commands.
    need(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path),
         "Owner marker ownership");

    if (std::filesystem::file_size(path) == 0) {
        return false;
    }

    try {
        auto value = read_json(path, 512);

        return value.at("schema") == 1 && value.at("producer_address") == address &&
               value.at("source") == source;
    } catch (const nlohmann::json::exception &) {
        return false;
    }
}

using detail::Ram;
} // namespace

const detail::CommandSpec &detail::command_spec(std::uint32_t op) {
    return spec_for(op);
}

struct Backend::Impl {
    Options options;
    Ram ram;
    std::unique_ptr<Dyn> vulkan, egl, gl;
    std::unique_ptr<GeneratedInvoker> generated;
    RpcIdleWait idle_poll;
    ServiceStats service_stats;
    std::uint64_t next_idle_identity_check_ns = 0, next_input_wake_ns = 0, last_input_wake = 0;
    bool input_wake_error = false;
    struct Profile {
        std::array<RpcProfileEntry, 513> wire{};
        std::uint64_t first_received_ns = 0, last_acknowledged_ns = 0;
    };
    std::unique_ptr<Profile> profile;
    std::optional<U> last_opcode, last_successful_sequence, last_state;
    bool input_request_seen = false, other_request_seen = false;
    bool done = false, control_claimed = false, canonical_screen = false;
    std::uint32_t last_seq = 0;
    std::uint64_t exported = 0, live_exported = 0, screen_sequence = 0, next_blob = 1;
    std::uint32_t clear_bits[4] = {0, 0, 0, 0};
    std::uint64_t clear_events = 0, live_swaps = 0;
    std::set<I> seen_fbs;
    std::map<I, I> draw_calls_by_fb;
    // The compositor clears and draws its six layer framebuffers once at startup, and every later
    // frame only clears the window framebuffer and swaps, so the composite draws are the ones that
    // must be explained. Their target includes framebuffer 0, and a draw can produce nothing
    // because of the state it runs under, so both are recorded here.
    std::map<I, I> draw_calls_all;
    std::map<I, std::array<I, 4>> viewport_by_fb;
    std::uint64_t draw_probe_events = 0, upload_probe_events = 0, draw_state_events = 0,
                  uniform_probe_events = 0, matrix_probe_events = 0, texture_reads = 0,
                  buffer_probe_events = 0, bind_probe_events = 0, shader_probe_events = 0,
                  teximage_probe_events = 0, uniform1_probe_events = 0, uniform2_probe_events = 0,
                  state_probe_events = 0;
    // The glyph atlas takes hundreds of sub-images per frame: logging those would exhaust any
    // budget before the objects that matter appear, so only the first sub-image of each object is
    // kept while every definition is, since a definition is what gives an object its level-0 image
    // in the first place.
    std::set<std::pair<U, U>> bound_pairs;
    std::set<I> teximage_sub_objects;
    // Level-0 dimensions of every defined texture object. The level query cannot supply them: on
    // this driver glGetTexLevelParameteriv raises GL_INVALID_OPERATION for every object, including
    // a freshly defined one, so the sizes come from the glTexImage2D calls that define them.
    std::map<U, std::pair<I, I>> texture_level0_size;
    bool pending_draw = false;
    I pending_draw_target = 0;
    bool depth_reissue_done = false;
    bool composite_probe_done = false, geometry_replay_done = false;
    std::uint64_t geometry_replay_skips = 0;
    std::uint64_t offered_owner_challenge = 0, consumed_owner_challenge = 0;
    Json offered_owners;
    std::filesystem::path producer;
    std::unique_ptr<frames::Writer> frame_writer;
    std::set<std::uint64_t> displays;
    std::set<std::pair<std::uint64_t, std::uint64_t>> contexts, surfaces;
    struct Blob {
        std::uint64_t size, direction, position = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
        std::vector<std::byte> bytes;
        bool texture_only = false;
        std::uint64_t valid_bytes = 0;
    };
    std::map<std::uint64_t, Blob> blobs;
    std::size_t allocated = 0;
    std::ofstream journal;
    bool replaying = false;
    std::vector<std::byte> replay_extra, recorded_extra;
    std::size_t replay_at = 0;
    EglHandles egl_handles;
    std::vector<std::uint64_t> restored_handles;
    std::size_t restored_handle_at = 0;
    std::uint64_t journal_bytes = 0, replayed_records = 0, replay_readbacks_skipped = 0,
                  replay_readback_bytes_skipped = 0;

    V native_handle(EglKind kind, std::uint64_t value) {
        return reinterpret_cast<V>(egl_handles.resolve(kind, value));
    }

    std::uint64_t expose_handle(EglKind kind, V native) {
        std::optional<std::uint64_t> restored;

        if (replaying) {
            need(restored_handle_at < restored_handles.size(), "Replay EGL handle count");
            restored = restored_handles[restored_handle_at++];
        }

        return egl_handles.expose(kind, reinterpret_cast<std::uintptr_t>(native), restored);
    }

    void read_input(std::byte *destination, std::uint64_t physical, std::size_t size) {
        if (replaying) {
            need(replay_at <= replay_extra.size() && size <= replay_extra.size() - replay_at,
                 "Replay upload extent");
            std::memcpy(destination, replay_extra.data() + replay_at, size);
            replay_at += size;
        } else {
            auto src = ram.extent(physical, size);
            std::memcpy(destination, src.data(), size);

            if (journal.is_open()) {
                need(recorded_extra.size() <= max_blob - size, "Journal upload budget");
                recorded_extra.insert(recorded_extra.end(), src.begin(), src.end());
            }
        }
    }

    /* Black-frame probes: the compositor can present opaque black while its scene graph runs, so
     * the probes name the framebuffer each clear targets and whether the bound framebuffer and
     * the Screen framebuffer hold pixels at present time. Read-only apart from the Screen rebind,
     * which is restored to the exact draw/read pair that was bound. */
    struct BufferProbe {
        I draw_fb = 0, read_fb = 0, draw_buffer = 0, read_buffer = 0, width = 0, height = 0;
        std::size_t nonzero_rgb = 0, dark_rgb = 0;
        unsigned sample[12]{};
        I min_x = -1, min_y = -1, max_x = -1, max_y = -1;
    };

    // The probes issue GL calls of their own, and a probe that is refused (an out-of-range
    // readback, say) leaves GL_INVALID_* pending, which the guest then reads as the result of its
    // own glGetError. Draining after every probe keeps the guest's error stream its own.
    void drain_gl_errors() {
        for (int i = 0; i < 8; i++) {
            if (gl->call<U>("glGetError") == 0) {
                return;
            }
        }
    }

    static Json sample_json(const unsigned *sample) {
        Json out = Json::array();

        for (std::size_t k = 0; k < 3; k++) {
            char text[32];
            std::snprintf(text, sizeof(text), "%02x %02x %02x %02x", sample[k * 4],
                          sample[k * 4 + 1], sample[k * 4 + 2], sample[k * 4 + 3]);
            out.push_back(text);
        }

        return out;
    }

    static Json matrix_json(const float *values) {
        Json out = Json::array();

        for (std::size_t k = 0; k < 16; k++) {
            out.push_back(double(values[k]));
        }

        return out;
    }

    BufferProbe probe_framebuffer(bool bind_screen, bool read_pixels) {
        BufferProbe probe;
        I viewport[4] = {0, 0, 0, 0};
        gl->call<void>("glGetIntegerv", U(0x8ca6), &probe.draw_fb);
        gl->call<void>("glGetIntegerv", U(0x8caa), &probe.read_fb);
        // 0x0C01 is the desktop GL_DRAW_BUFFER name; ES 3.0 has GL_DRAW_BUFFER0 (0x8825), and ANGLE
        // rejects the desktop one with GL_INVALID_ENUM, which the guest would then read as its own
        // error.
        gl->call<void>("glGetIntegerv", U(0x8825), &probe.draw_buffer);
        gl->call<void>("glGetIntegerv", U(0x0c02), &probe.read_buffer);
        gl->call<void>("glGetIntegerv", U(0x0ba2), viewport);
        // Read only what the drawable has: a read outside it is undefined behaviour and ANGLE
        // reports it as an error, which would then surface as the guest's own glGetError result.
        probe.width = std::max<I>(0, std::min<I>(viewport[2], I(frames::width)));
        probe.height = std::max<I>(0, std::min<I>(viewport[3], I(frames::height)));

        if (bind_screen && probe.draw_fb) {
            gl->call<void>("glBindFramebuffer", U(0x8d40), I(0));
        }

        if (read_pixels && probe.width && probe.height) {
            std::vector<std::byte> pixels(std::size_t(probe.width) * std::size_t(probe.height) * 4);
            gl->call<void>("glReadPixels", I(0), I(0), probe.width, probe.height, U(0x1908),
                           U(0x1401), pixels.data());

            for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
                if (pixels[i] != std::byte{0} || pixels[i + 1] != std::byte{0} ||
                    pixels[i + 2] != std::byte{0}) {
                    ++probe.nonzero_rgb;
                    // Where the fragments actually landed: a layer drawn at the wrong scale or
                    // offset still leaves pixels, and the bounding box tells a mis-scaled layer
                    // from an absent one.
                    const I row = I(i / 4 / std::size_t(probe.width)),
                            column = I(i / 4 % std::size_t(probe.width));

                    if (probe.min_x < 0 || column < probe.min_x) {
                        probe.min_x = column;
                    }

                    if (probe.max_x < column) {
                        probe.max_x = column;
                    }

                    if (probe.min_y < 0 || row < probe.min_y) {
                        probe.min_y = row;
                    }

                    if (probe.max_y < row) {
                        probe.max_y = row;
                    }
                } else if (pixels[i] == std::byte{0} && pixels[i + 1] == std::byte{0} &&
                           pixels[i + 2] == std::byte{0}) {
                    ++probe.dark_rgb;
                }
            }

            // Three corner/centre pixels of the rectangle, so a zero counter can be told apart from
            // a black-but-opaque frame and from a frame whose alpha alone is nonzero, without a
            // readback of the whole thing.
            for (std::size_t k = 0; k < 3; k++) {
                const std::size_t x = k == 0 ? 0
                                             : (k == 1 ? std::size_t(probe.width) / 2
                                                       : std::size_t(probe.width) - 1),
                                  y = k == 0 ? 0
                                             : (k == 1 ? std::size_t(probe.height) / 2
                                                       : std::size_t(probe.height) - 1);
                const std::size_t at = (y * std::size_t(probe.width) + x) * 4;

                if (at + 3 < pixels.size()) {
                    for (std::size_t c = 0; c < 4; c++) {
                        probe.sample[k * 4 + c] = std::to_integer<unsigned>(pixels[at + c]);
                    }
                }
            }
        }

        if (bind_screen && probe.draw_fb) {
            gl->call<void>("glBindFramebuffer", U(0x8ca9), probe.draw_fb);

            if (probe.read_fb != probe.draw_fb) {
                gl->call<void>("glBindFramebuffer", U(0x8caa), probe.read_fb);
            }
        }

        drain_gl_errors();

        return probe;
    }

    /* What a single draw leaves behind: the composite into the window framebuffer runs five times
     * per frame in the scene's own programs and textures, so each of those draws is followed by a
     * probe of the very framebuffer it targeted -- taken before anything else can clear it --
     * carrying the pixel samples and the bound texture's own size and content. Zero here, right
     * after the draw, means the draw itself produced nothing; content here and zero at swap time
     * means something later erases it. */
    Json probe_vertex_state() {
        Json out = Json::array();
        I active = 0;
        gl->call<void>("glGetIntegerv", U(0x8873), &active);
        out.push_back({{"active_texture", active}});
        I max_attribs = 0;
        gl->call<void>("glGetIntegerv", U(0x8869), &max_attribs);
        // Every buffer binding this probe changes must come back: the guest keeps its own idea of
        // which buffer is bound and never re-binds it, so leaving GL_ARRAY_BUFFER at zero makes the
        // guest's next glVertexAttribPointer look like a client array and makes the host refuse it.
        I saved_array = 0;
        gl->call<void>("glGetIntegerv", U(0x8894), &saved_array);

        for (I i = 0; i < std::min<I>(max_attribs, 8); i++) {
            I enabled = 0, buffer = 0, size = 0, type = 0, stride = 0, offset = 0;
            unsigned current[4] = {0, 0, 0, 0};
            gl->call<void>("glGetVertexAttribiv", i, U(0x8622), &enabled);
            gl->call<void>("glGetVertexAttribiv", i, U(0x889f), &buffer);
            gl->call<void>("glGetVertexAttribiv", i, U(0x8623), &size);
            gl->call<void>("glGetVertexAttribiv", i, U(0x8624), &stride);
            gl->call<void>("glGetVertexAttribiv", i, U(0x8625), &type);
            // The offset has its own entry point: glGetVertexAttribiv with
            // GL_VERTEX_ATTRIB_ARRAY_POINTER raises GL_INVALID_ENUM and leaves the variable at
            // zero, which earlier made every attribute look as if the draw read its data from the
            // start of the buffer.
            void *pointer = nullptr;
            gl->call<void>("glGetVertexAttribPointerv", i, U(0x8645), &pointer);
            offset = static_cast<I>(reinterpret_cast<std::intptr_t>(pointer));
            gl->call<void>("glGetVertexAttribfv", i, U(0x8626), current);
            // The vertex data itself: an attribute array whose buffer holds zeros collapses the
            // whole layer to a single point, which is indistinguishable from an empty frame after
            // the fact. The bytes are read at the attribute's own offset -- reading from zero hid
            // whether the element the draw actually uses holds anything.
            std::size_t nonzero = 0, sampled = 0;
            std::string first;

            if (enabled && buffer && offset >= 0) {
                std::array<std::byte, 32> bytes{};
                gl->call<void>("glBindBuffer", U(0x8892), buffer);
                gl->call<void>("glGetBufferSubData", U(0x8892), offset, I(32), bytes.data());

                for (auto b : bytes) {
                    if (b != std::byte{0}) {
                        ++nonzero;
                    }
                }

                sampled = bytes.size();
                gl->call<void>("glBindBuffer", U(0x8892), U(saved_array));
                char text[3];

                for (std::size_t k = 0; k < 16; k++) {
                    std::snprintf(text, sizeof(text), "%02x", std::to_integer<unsigned>(bytes[k]));
                    first += text;
                }
            }

            out.push_back(
                    {{"attrib", i},
                     {"enabled", enabled},
                     {"buffer", buffer},
                     {"size", size},
                     {"type", type},
                     {"stride", stride},
                     {"offset", offset},
                     {"current", Json::array({current[0], current[1], current[2], current[3]})},
                     {"buffer_nonzero_bytes", nonzero},
                     {"buffer_sampled", sampled},
                     {"buffer_first_hex", first}});
        }

        for (I unit = 0; unit < 8; unit++) {
            I binding = 0, width = 0, height = 0;
            gl->call<void>("glActiveTexture", U(0x84c0 + unit));
            gl->call<void>("glGetIntegerv", U(0x8069), &binding);

            if (binding) {
                gl->call<void>("glGetTexLevelParameteriv", U(0xde1), I(0), U(0x1000), &width);
                gl->call<void>("glGetTexLevelParameteriv", U(0xde1), I(0), U(0x1001), &height);
            }

            out.push_back({{"unit", unit},
                           {"binding_2d", binding},
                           {"width", width},
                           {"height", height}});
        }

        gl->call<void>("glActiveTexture", U(0x84c0 + active));
        I index_buffer = 0;
        gl->call<void>("glGetIntegerv", U(0x8895), &index_buffer);

        if (index_buffer) {
            std::array<std::byte, 32> bytes{};
            gl->call<void>("glBindBuffer", U(0x8893), index_buffer);
            gl->call<void>("glGetBufferSubData", U(0x8893), I(0), I(32), bytes.data());
            std::size_t nonzero = 0;

            for (auto b : bytes) {
                if (b != std::byte{0}) {
                    ++nonzero;
                }
            }

            gl->call<void>("glBindBuffer", U(0x8893), U(index_buffer));
            // The indices themselves: a draw whose indices point past the vertex arrays rasterises
            // nothing and raises no error at all, so the first values are recorded next to the
            // vertex buffers.
            std::string indices;
            char text[8];

            for (std::size_t k = 0; k + 1 < 16; k += 2) {
                std::snprintf(text, sizeof(text), "%u ",
                              unsigned(std::to_integer<unsigned>(bytes[k]) |
                                       (std::to_integer<unsigned>(bytes[k + 1]) << 8)));
                indices += text;
            }

            out.push_back({{"element_buffer", index_buffer},
                           {"nonzero_bytes", nonzero},
                           {"indices_u16", indices}});
        } else {
            out.push_back({{"element_buffer", index_buffer}});
        }

        I binding = 0;
        gl->call<void>("glGetIntegerv", U(0x8894), &binding);
        out.push_back({{"array_buffer", binding}});
        drain_gl_errors();

        return out;
    }

    Json probe_after_draw(I framebuffer) {
        // The draw's own verdict comes first: a refused draw (an incomplete sampler, say) leaves
        // GL_INVALID_OPERATION pending, and any probe call issued before this would hide it.
        Json fields = {{"gl_error_after_draw", gl->call<U>("glGetError")}};
        // Whether the fragments are rejected by the depth test: the composite runs with depth_test
        // 1 and never clears the depth buffer of the window surface, so the state and the buffer
        // itself decide whether every fragment fails the comparison.
        I depth_func = 0, depth_mask = 0;
        float depth_clear = 0.f;
        gl->call<void>("glGetIntegerv", U(0x0b74), &depth_func);
        gl->call<void>("glGetIntegerv", U(0x0b72), &depth_mask);
        gl->call<void>("glGetFloatv", U(0x0b73), &depth_clear);
        Json depth = Json::array();

        for (std::size_t k = 0; k < 3; k++) {
            const I x = k == 0 ? 0 : (k == 1 ? 960 : 1919), y = k == 0 ? 0 : (k == 1 ? 360 : 719);
            float value = -1.f;
            gl->call<void>("glReadPixels", x, y, I(1), I(1), U(0x1902), U(0x1401), &value);
            depth.push_back(double(value));
        }

        fields["depth_func"] = depth_func;
        fields["depth_mask"] = depth_mask;
        fields["depth_clear"] = double(depth_clear);
        fields["depth_samples"] = depth;
        fields["depth_read_error"] = gl->call<U>("glGetError");
        // A colour writemask of zero, or a blend function that contributes nothing with the source
        // alpha the guest supplies, empties the window while every draw still succeeds. The state
        // is read here rather than inferred from the calls, since the guest may never set it at
        // all.
        I writemask[4] = {0, 0, 0, 0};
        gl->call<void>("glGetIntegerv", U(0x0c23), writemask);
        I blend = 0, depth_test = 0, program = 0;
        gl->call<void>("glGetIntegerv", U(0x0be2), &blend);
        gl->call<void>("glGetIntegerv", U(0x0b71), &depth_test);
        gl->call<void>("glGetIntegerv", U(0x8b8d), &program);
        fields["color_writemask"] =
                Json::array({writemask[0], writemask[1], writemask[2], writemask[3]});
        fields["blend"] = blend;
        fields["depth_test"] = depth_test;
        fields["current_program"] = program;
        // A viewport that does not cover the surface, or a scissor box left over from another
        // target, discards every fragment of a draw that is otherwise fully valid. Neither is
        // visible in the call stream, since the guest sets them once for a target it then draws
        // many times.
        I viewport[4] = {0, 0, 0, 0}, scissor_test = 0, scissor_box[4] = {0, 0, 0, 0};
        gl->call<void>("glGetIntegerv", U(0x0ba2), viewport);
        gl->call<void>("glGetIntegerv", U(0x0c11), &scissor_test);
        gl->call<void>("glGetIntegerv", U(0x0c10), scissor_box);
        fields["viewport"] = Json::array({viewport[0], viewport[1], viewport[2], viewport[3]});
        fields["scissor_test"] = scissor_test;
        fields["scissor_box"] =
                Json::array({scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]});
        // Blending is enabled but its function is not: a factor of zero on the source, or a wrong
        // equation, discards every fragment of a draw whose state is otherwise correct.
        I blend_src_rgb = 0, blend_dst_rgb = 0, blend_src_alpha = 0, blend_dst_alpha = 0,
          blend_equation_rgb = 0, blend_equation_alpha = 0;
        gl->call<void>("glGetIntegerv", U(0x80c9), &blend_src_rgb);
        gl->call<void>("glGetIntegerv", U(0x80c8), &blend_dst_rgb);
        gl->call<void>("glGetIntegerv", U(0x80cb), &blend_src_alpha);
        gl->call<void>("glGetIntegerv", U(0x80ca), &blend_dst_alpha);
        gl->call<void>("glGetIntegerv", U(0x8009), &blend_equation_rgb);
        gl->call<void>("glGetIntegerv", U(0x800a), &blend_equation_alpha);
        fields["blend_src_rgb"] = blend_src_rgb;
        fields["blend_dst_rgb"] = blend_dst_rgb;
        fields["blend_src_alpha"] = blend_src_alpha;
        fields["blend_dst_alpha"] = blend_dst_alpha;
        fields["blend_equation_rgb"] = blend_equation_rgb;
        fields["blend_equation_alpha"] = blend_equation_alpha;
        auto probe = probe_framebuffer(framebuffer == 0, true);
        fields["framebuffer"] = framebuffer;
        fields["nonzero_rgb"] = probe.nonzero_rgb;
        fields["sampled"] = std::size_t(probe.width) * std::size_t(probe.height);
        fields["samples"] = sample_json(probe.sample);
        fields["vertex_state"] = probe_vertex_state();
        I texture = 0;
        gl->call<void>("glGetIntegerv", U(0x8069), &texture);
        fields["texture_2d"] = texture;

        if (texture) {
            I width = 0, height = 0, saved = 0;
            gl->call<void>("glGetIntegerv", U(0x8069), &saved);
            gl->call<void>("glBindTexture", U(0xde1), texture);
            gl->call<void>("glGetTexLevelParameteriv", U(0xde1), I(0), U(0x1000), &width);
            gl->call<void>("glGetTexLevelParameteriv", U(0xde1), I(0), U(0x1001), &height);
            fields["texture_width"] = width;
            fields["texture_height"] = height;
            // ES 3.0 refuses the level query with GL_INVALID_OPERATION when the level has no image,
            // so the error distinguishes "this object has no image on the host" from "the query
            // itself is not answered", which a zero width alone cannot.
            fields["texture_level_error"] = gl->call<U>("glGetError");

            // The sampler's own content decides whether an empty composite is a texturing failure
            // or a shading one, and ES 3.0 has no glGetTexImage, so a scratch framebuffer is built
            // for the read and torn down, with the guest's bindings put back exactly as they were.
            if (width > 0 && height > 0 && texture_reads < 4) {
                I saved_fb = 0;
                gl->call<void>("glGetIntegerv", U(0x8ca6), &saved_fb);
                I scratch = 0;
                gl->call<void>("glGenFramebuffers", I(1), &scratch);
                gl->call<void>("glBindFramebuffer", U(0x8d40), scratch);
                gl->call<void>("glFramebufferTexture2D", U(0x8d40), U(0x8ce0), U(0xde1), texture,
                               I(0));

                if (gl->call<U>("glCheckFramebufferStatus", U(0x8d40)) == U(0x8cd5)) {
                    const I w = std::max<I>(1, std::min<I>(width, I(frames::width))),
                            h = std::max<I>(1, std::min<I>(height, I(frames::height)));
                    std::vector<std::byte> pixels(std::size_t(w) * std::size_t(h) * 4);
                    gl->call<void>("glReadPixels", I(0), I(0), w, h, U(0x1908), U(0x1401),
                                   pixels.data());
                    std::size_t nonzero = 0;

                    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
                        if (pixels[i] != std::byte{0} || pixels[i + 1] != std::byte{0} ||
                            pixels[i + 2] != std::byte{0}) {
                            ++nonzero;
                        }
                    }

                    fields["texture_nonzero_rgb"] = nonzero;
                    fields["texture_sampled"] = std::size_t(w) * std::size_t(h);
                } else {
                    fields["texture_status"] = gl->call<U>("glCheckFramebufferStatus", U(0x8d40));
                }

                gl->call<void>("glBindFramebuffer", U(0x8d40), saved_fb);
                gl->call<void>("glDeleteFramebuffers", I(1), &scratch);
                ++texture_reads;
            }

            gl->call<void>("glBindTexture", U(0xde1), saved);
        }

        drain_gl_errors();

        return fields;
    }

    std::uintptr_t current_surface(I which) {
        return reinterpret_cast<std::uintptr_t>(egl->call<V>("eglGetCurrentSurface", which));
    }

    std::uintptr_t current_context() {
        return reinterpret_cast<std::uintptr_t>(egl->call<V>("eglGetCurrentContext"));
    }

    /* Scene-content probe: the default framebuffer can stay fully black while the compositor
     * clears and draws into framebuffers of its own, so the probe finds which of those holds the
     * scene. The read rectangle comes from the attachment itself, and an incomplete or colourless
     * or multisampled one is skipped instead of read: an invalid readback would raise a GL error
     * that the guest's next glGetError (op20) would report as its own. Draw and read framebuffer
     * bindings are restored to the pair that was bound. */
    Json probe_scene_framebuffers() {
        Json out = Json::array();
        I saved_draw = 0, saved_read = 0;
        gl->call<void>("glGetIntegerv", U(0x8ca6), &saved_draw);
        gl->call<void>("glGetIntegerv", U(0x8caa), &saved_read);

        for (I framebuffer : seen_fbs) {
            Json row = {{"framebuffer", framebuffer}};
            gl->call<void>("glBindFramebuffer", U(0x8d40), framebuffer);
            const U status = gl->call<U>("glCheckFramebufferStatus", U(0x8d40));
            row["status"] = status;
            I attachment = 0, width = 0, height = 0, samples = 0;

            if (status == U(0x8cd5)) {
                gl->call<void>("glGetFramebufferAttachmentParameteriv", U(0x8d40), U(0x8ce0),
                               U(0x8cd0), &attachment);

                // 0x8CD1 is the attachment's object name; the previous run asked for 0x8CD4, which
                // is the cube-map/array layer, then bound that value as a texture and read a zero
                // level.
                if (attachment == I(0x1702)) {
                    I object = 0, saved = 0, level = 0, face = 0;
                    gl->call<void>("glGetFramebufferAttachmentParameteriv", U(0x8d40), U(0x8ce0),
                                   U(0x8cd1), &object);
                    gl->call<void>("glGetFramebufferAttachmentParameteriv", U(0x8d40), U(0x8ce0),
                                   U(0x8cd2), &level);
                    gl->call<void>("glGetFramebufferAttachmentParameteriv", U(0x8d40), U(0x8ce0),
                                   U(0x8cd3), &face);
                    gl->call<void>("glGetIntegerv", U(0x8069), &saved);
                    gl->call<void>("glBindTexture", U(0xde1), object);
                    gl->call<void>("glGetTexLevelParameteriv", U(0xde1), level, U(0x1000), &width);
                    gl->call<void>("glGetTexLevelParameteriv", U(0xde1), level, U(0x1001), &height);
                    gl->call<void>("glGetTexLevelParameteriv", U(0xde1), level, U(0x80a9),
                                   &samples);
                    gl->call<void>("glBindTexture", U(0xde1), saved);
                    row["object"] = object;
                    row["level"] = level;
                    row["face"] = face;
                } else if (attachment == I(0x8d41)) {
                    I object = 0, saved = 0;
                    gl->call<void>("glGetFramebufferAttachmentParameteriv", U(0x8d40), U(0x8ce0),
                                   U(0x8cd1), &object);
                    gl->call<void>("glGetIntegerv", U(0x8ca7), &saved);
                    gl->call<void>("glBindRenderbuffer", U(0x8d41), object);
                    gl->call<void>("glGetRenderbufferParameteriv", U(0x8d41), U(0x8d42), &width);
                    gl->call<void>("glGetRenderbufferParameteriv", U(0x8d41), U(0x8d43), &height);
                    gl->call<void>("glGetRenderbufferParameteriv", U(0x8d41), U(0x8cab), &samples);
                    gl->call<void>("glBindRenderbuffer", U(0x8d41), saved);
                    row["object"] = object;
                }
            }

            // An attachment whose size query still fails falls back to the viewport the guest last
            // set while that framebuffer was bound: the layer framebuffers are drawn with their own
            // viewport, so that rectangle is the content the question is about.
            if ((!width || !height) && viewport_by_fb.contains(framebuffer)) {
                const auto &view = viewport_by_fb.at(framebuffer);
                width = view[2];
                height = view[3];
                row["size_source"] = "viewport";
            }

            width = std::max<I>(0, std::min<I>(width, I(frames::width)));
            height = std::max<I>(0, std::min<I>(height, I(frames::height)));
            row["attachment"] = attachment;
            row["width"] = width;
            row["height"] = height;
            row["samples"] = samples;

            if (auto it = draw_calls_by_fb.find(framebuffer); it != draw_calls_by_fb.end()) {
                row["draws"] = it->second;
            }

            if (attachment && !samples && width && height) {
                std::vector<std::byte> pixels(std::size_t(width) * std::size_t(height) * 4);
                gl->call<void>("glReadPixels", I(0), I(0), width, height, U(0x1908), U(0x1401),
                               pixels.data());
                std::size_t nonzero_rgb = 0;

                for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
                    if (pixels[i] != std::byte{0} || pixels[i + 1] != std::byte{0} ||
                        pixels[i + 2] != std::byte{0}) {
                        ++nonzero_rgb;
                    }
                }

                row["sampled"] = std::size_t(width) * std::size_t(height);
                row["nonzero_rgb"] = nonzero_rgb;
            }

            out.push_back(row);
        }

        gl->call<void>("glBindFramebuffer", U(0x8d40), saved_draw);

        if (saved_read != saved_draw) {
            gl->call<void>("glBindFramebuffer", U(0x8caa), saved_read);
        }

        drain_gl_errors();

        return out;
    }

    /* A draw can leave the framebuffer black although it ran, so a draw that reaches framebuffer 0
     * (the one the guest posts to Screen) is recorded together with the state that can suppress it:
     * a scissor box that clips everything, a disabled or zero viewport, blending into an empty
     * destination, and the program/texture actually bound. Read-only queries only. */
    Json probe_draw_state(I framebuffer) {
        I viewport[4] = {0, 0, 0, 0}, scissor[4] = {0, 0, 0, 0}, scissor_test = 0, blend = 0,
          depth = 0, cull = 0, program = 0, texture = 0;
        gl->call<void>("glGetIntegerv", U(0x0ba2), viewport);
        gl->call<void>("glGetIntegerv", U(0x0c10), scissor);
        gl->call<void>("glGetIntegerv", U(0x0c11), &scissor_test);
        gl->call<void>("glGetIntegerv", U(0x0be2), &blend);
        gl->call<void>("glGetIntegerv", U(0x0b71), &depth);
        gl->call<void>("glGetIntegerv", U(0x0b44), &cull);
        gl->call<void>("glGetIntegerv", U(0x8b8d), &program);
        gl->call<void>("glGetIntegerv", U(0x8069), &texture);

        return {{"framebuffer", framebuffer},
                {"viewport", Json::array({viewport[0], viewport[1], viewport[2], viewport[3]})},
                {"scissor_box", Json::array({scissor[0], scissor[1], scissor[2], scissor[3]})},
                {"scissor_test", scissor_test},
                {"blend", blend},
                {"depth_test", depth},
                {"cull_face", cull},
                {"program", program},
                {"texture_2d", texture}};
    }

    static bool gpu_opcode(U op) {
        return op != 0 && !(op >= 355 && op <= 368 && op != 358 && op != 360);
    }

    void save_record(U op, const Arguments &a, std::span<const std::byte> data, const Reply &out) {
        if (!journal.is_open() || replaying || !gpu_opcode(op)) {
            return;
        }

        std::vector<std::byte> record;
        append(record, op);
        append(record, U(data.size()));
        append(record, U(out.payload.size()));
        append(record, U(recorded_extra.size()));
        append(record, out.result);

        for (auto value : a) {
            append(record, value);
        }

        record.insert(record.end(), data.begin(), data.end());
        record.insert(record.end(), out.payload.begin(), out.payload.end());
        record.insert(record.end(), recorded_extra.begin(), recorded_extra.end());
        need(journal_bytes + record.size() + 4 <= 2ULL * 1024 * 1024 * 1024, "Journal file budget");
        U size = record.size();
        journal.write(reinterpret_cast<const char *>(&size), 4);
        journal.write(reinterpret_cast<const char *>(record.data()), record.size());
        journal.flush();
        need(bool(journal), "Journal write");
        journal_bytes += record.size() + 4;
    }

    Reply journal_dispatch(U op, const Arguments &a, std::span<const std::byte> data) {
        if (!replaying) {
            recorded_extra.clear();
        }

        auto out = dispatch(op, a, data);
        save_record(op, a, data, out);

        return out;
    }

    void replay_file(const std::filesystem::path &path) {
        const auto replay_start = std::chrono::steady_clock::now();
        need(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path) &&
                     std::filesystem::file_size(path) <= 2ULL * 1024 * 1024 * 1024,
             "Replay file ownership/extent");
        std::ifstream file(path, std::ios::binary);
        std::array<std::uint64_t, 3> header{};
        file.read(reinterpret_cast<char *>(header.data()), 24);
        need(bool(file) && header[0] == 0x3130305550475156ULL && header[1] == options.address &&
                     header[2] == options.ram_extent,
             "Replay header");
        replaying = true;

        while (file.peek() != std::char_traits<char>::eof()) {
            U size = 0;
            file.read(reinterpret_cast<char *>(&size), 4);
            need(bool(file) && size >= 152 &&
                         size <= max_blob + std::size_t{2} * rpc_payload_bytes + 152,
                 "Replay record extent");
            std::vector<std::byte> record(size);
            file.read(reinterpret_cast<char *>(record.data()), size);
            need(bool(file), "Replay truncated record");
            U op = u32(record, 0), in = u32(record, 4), out = u32(record, 8),
              extra = u32(record, 12);
            need(gpu_opcode(op) && in <= rpc_payload_bytes && out <= rpc_payload_bytes &&
                         extra <= max_blob && std::uint64_t(152) + in + out + extra == size,
                 "Replay record geometry");
            Arguments a{};
            std::memcpy(a.data(), record.data() + 24, 128);
            auto payload = std::span<const std::byte>(record).subspan(152, in);
            Reply expected{u64(record, 16),
                           std::vector<std::byte>(record.begin() + 152 + in,
                                                  record.begin() + 152 + in + out),
                           {}};
            replay_extra.assign(record.begin() + 152 + in + out, record.end());
            replay_at = 0;
            restored_handles.clear();
            restored_handle_at = 0;

            if (op == 1 || op == 7 || op == 8 || op == 62 || op == 63 || op == 70) {
                restored_handles.push_back(expected.result);
            }

            if (op == 6 || op == 60) {
                need(expected.payload.size() >= 4 && (expected.payload.size() - 4) % 8 == 0,
                     "Replay config payload");

                for (std::size_t at = 4; at < expected.payload.size(); at += 8) {
                    restored_handles.push_back(u64(expected.payload, at));
                }
            }

            auto actual = dispatch(op, a, payload);
            need(replay_at == replay_extra.size() && restored_handle_at == restored_handles.size(),
                 "Replay unused upload or handle");
            need(actual.result == expected.result && actual.payload == expected.payload,
                 "Replay GL result mismatch");
            need(++replayed_records <= 2000000, "Replay record count");
        }

        replay_extra.clear();
        replaying = false;
        logging::write(logging::Level::info, "graphics", "checkpoint.replayed",
                       {{"address", options.address},
                        {"records", replayed_records},
                        {"screen_sequence", screen_sequence},
                        {"exported", exported},
                        {"readbacks_skipped", replay_readbacks_skipped},
                        {"readback_bytes_skipped", replay_readback_bytes_skipped},
                        {"bytes", std::filesystem::file_size(path)},
                        {"elapsed_ms", std::chrono::duration<double, std::milli>(
                                               std::chrono::steady_clock::now() - replay_start)
                                               .count()}});
    }

    void open_journal() {
        need(options.record.empty() || options.replay.empty(), "Record/replay mode conflict");

        if (!options.replay.empty()) {
            auto path = options.replay / (hex_address(options.address) + ".gpu");
            need(std::filesystem::exists(path), "Missing producer replay");
            replay_file(path);
        }

        if (!options.record.empty()) {
            auto root = options.record;
            need(!std::filesystem::is_symlink(root) && std::filesystem::is_directory(root),
                 "Journal directory ownership");
            auto path = root / (hex_address(options.address) + ".gpu");
            need(!std::filesystem::exists(path) && !std::filesystem::is_symlink(path),
                 "Journal overwrite");
            journal.open(path, std::ios::binary | std::ios::out);
            need(bool(journal), "Journal open");
            std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                                       std::filesystem::perms::owner_write);
            std::array<std::uint64_t, 3> header{0x3130305550475156ULL, options.address,
                                                options.ram_extent};
            journal.write(reinterpret_cast<const char *>(header.data()), 24);
            journal.flush();
            journal_bytes = 24;
        }
    }

    std::map<std::pair<std::uint64_t, U>, U> client_buffers;
    std::map<std::uint64_t, U> client_index_buffers;
    std::map<U, std::uint64_t> op_counts;
    bool renderer_logged = false;
    std::uint64_t wheel_issued = 0, wheel_acked = 0, vehicle_issued = 0, vehicle_acked = 0,
                  theme_issued = 0, theme_acked = 0, theme_reported = 0;
    bool theme_already_selected = false;
    U theme_mode_issued = 0, theme_skin_issued = 0, theme_last_outcome = 0;
    std::map<std::string_view, Json> mcu_layouts;

    explicit Impl(Options o) : options(std::move(o)), ram(options) {
        if (options.profile_rpc) {
            profile = std::make_unique<Profile>();
        }

        need(options.address % 4096 == 0 && options.address >= ram_base &&
                     options.address - ram_base <= options.ram_extent - 4096,
             "RPC page physical geometry");
        need(!std::filesystem::is_symlink(options.session) &&
                     std::filesystem::is_directory(options.session),
             "Graphics session ownership");
        producer = options.session / ("producer-" + hex_address(options.address));
        need(!std::filesystem::is_symlink(producer), "Producer directory ownership");
        std::filesystem::create_directory(producer);
        need(std::filesystem::is_directory(producer), "Producer directory geometry");
        const auto suffix = std::string(os::shared_library_suffix);
        const auto egl_name = "libEGL" + suffix, gl_name = "libGLESv2" + suffix;
        const auto mismatch = "ANGLE " + std::string(os::platform_name) + " pin mismatch";
        // Files of the Emulator archive of this host, pinned in sdk.runtimeFilePins.
        need(file_sha256(options.angle / egl_name) ==
                             native_runtime_file_pin(os::dependency_host,
                                                     "emulator/lib64/gles_angle/" + egl_name) &&
                     file_sha256(options.angle / gl_name) ==
                             native_runtime_file_pin(os::dependency_host,
                                                     "emulator/lib64/gles_angle/" + gl_name),
             mismatch);
        const auto loader = detail::angle_loader_file(options.angle);

        if (!loader.empty()) {
            need(file_sha256(loader) ==
                         native_runtime_file_pin(os::dependency_host,
                                                 "emulator/lib64/gles_angle/libvulkan.so.1"),
                 mismatch);
            vulkan = std::make_unique<Dyn>(loader);
        }

        egl = std::make_unique<Dyn>(options.angle / egl_name);
        gl = std::make_unique<Dyn>(options.angle / gl_name);
        generated = std::make_unique<GeneratedInvoker>(GeneratedInvoker{*gl});
        open_journal();
    }

    ~Impl() {
        if (!egl) {
            return;
        }

        try {
            for (auto d : displays) {
                egl->call<U>("eglMakeCurrent", native_handle(EglKind::display, d), V{}, V{}, V{});
            }

            for (auto [d, s] : surfaces) {
                egl->call<U>("eglDestroySurface", native_handle(EglKind::display, d),
                             native_handle(EglKind::surface, s));
            }

            for (auto [d, c] : contexts) {
                egl->call<U>("eglDestroyContext", native_handle(EglKind::display, d),
                             native_handle(EglKind::context, c));
            }

            for (auto d : displays) {
                egl->call<U>("eglTerminate", native_handle(EglKind::display, d));
            }
        } catch (...) { /* Ignored: destructor cannot manufacture success or replace the original
                           failure. */
        }
    }

    std::vector<I> attributes(std::span<const std::byte> data) {
        if (data.empty()) {
            return {};
        }

        need(data.size() % 4 == 0 && data.size() <= 512, "Attribute extent");
        std::vector<I> out(data.size() / 4);
        std::memcpy(out.data(), data.data(), data.size());
        need(out.back() == 0x3038 && out.size() % 2 == 1, "Attribute terminator");

        return out;
    }

    Blob *blob(std::uint64_t key, bool texture = false) {
        if (!key) {
            return nullptr;
        }

        auto it = blobs.find(key);
        need(it != blobs.end() && it->second.position == it->second.size,
             "Incomplete or unknown bulk registration");
        need(texture || !it->second.texture_only, "Texture cache used outside pixel upload");

        return &it->second;
    }

    void write_blob(std::uint64_t key, std::span<const std::byte> bytes) {
        auto b = blob(key);
        need(b && b->direction == 1 && bytes.size() <= b->size, "Unowned output blob");

        if (replaying) {
            return;
        }

        std::size_t at = 0;

        for (auto [physical, size] : b->runs) {
            auto n = std::min<std::uint64_t>(size, bytes.size() - at);

            if (n) {
                ram.write(physical, bytes.subspan(at, n));
                at += n;
            }
        }

        need(at == bytes.size(), "Incomplete output transfer");
    }

    bool discard_replay_readback(const Arguments &a, std::size_t bytes) {
        if (!replaying || !a[6]) {
            return false;
        }

        auto output = blob(a[6]);
        need(output && output->direction == 1 && bytes <= output->size, "Unowned output blob");
        // Only discard historical client-memory reads of an actual default surface.
        // Offscreen FBO reads and inline pixel replies retain their exact driver behavior.
        const auto *version = reinterpret_cast<const char *>(
                gl->call<const unsigned char *>("glGetString", U(0x1f02)));

        if (!version || !std::string_view(version).starts_with("OpenGL ES 3")) {
            return false;
        }

        I binding = 0;
        gl->call<void>("glGetIntegerv", U(0x8caa), &binding);

        if (binding || !egl->call<V>("eglGetCurrentSurface", I(0x305a))) {
            return false;
        }

        I read_buffer = 0, component = 0;
        gl->call<void>("glGetIntegerv", U(0x0c02), &read_buffer);

        if (!read_buffer || gl->call<U>("glCheckFramebufferStatus", U(0x8ca8)) != 0x8cd5) {
            return false;
        }

        gl->call<void>("glGetFramebufferAttachmentParameteriv", U(0x8ca8), U(0x0405), U(0x8211),
                       &component);

        if (component != 0x8c17) {
            return false; // GL_UNSIGNED_NORMALIZED: RGBA/UNSIGNED_BYTE is valid.
        }

        ++replay_readbacks_skipped;
        replay_readback_bytes_skipped += bytes;

        return true;
    }

    bool controls() {
        if (!options.controls) {
            return false;
        }

        if (!control_claimed) {
            control_claimed = claim(options.session / "control-owner.json", options.address,
                                    "original-MCU-input");
        }

        return control_claimed;
    }

    void ack(const char *name, std::uint64_t acknowledged, std::uint64_t issued,
             std::uint64_t &recorded, bool canonical = true, const Json &projection = Json()) {
        need(acknowledged < 0x100000000ULL, "Guest command ACK extent");

        if (!issued || acknowledged < issued || acknowledged <= recorded) {
            return;
        }

        need(acknowledged == issued, "Guest command ACK beyond issued sequence");
        // The receiver identity lets the session reject a receipt of an earlier incarnation.
        Json value{{"schema", 1},
                   {"sequence", acknowledged},
                   {"producer_address", options.address},
                   {"generation", options.generation},
                   {"worker_nonce", options.worker_nonce},
                   {"guest_pid", options.guest_pid},
                   {"producer_incarnation", options.producer_incarnation},
                   {"guest_acknowledged", true},
                   {"host_monotonic_ns", now_ns()}};

        if (!projection.is_null()) {
            value["mcu_projection"] = projection;
        }

        replace_json((canonical ? options.session : producer) / name, value, options.address);
        recorded = acknowledged;
    }

    // The MCU layout of a qinggan11 adapter, checked against the registry and the model.
    const Json &mcu_layout(const QnxAdapterSpec &spec) {
        auto at = mcu_layouts.find(spec.name);

        if (at == mcu_layouts.end()) {
            auto definition = embedded_project_file_json(
                    "src/guest/qnx/" + std::string(spec.mcu_layout_file), 512 * 1024);
            qnx_qinggan11_mcu_layout(definition);
            at = mcu_layouts.emplace(spec.name, std::move(definition)).first;
        }

        return at->second;
    }

    Reply controls_reply(U op, const Arguments &a, std::span<const std::byte> data) {
        need(data.empty() && a[0] < 0x100000000ULL, "Control request payload/ACK");

        if (op == 355) {
            const auto theme_path = options.session / "theme-command.json";

            if (std::filesystem::exists(theme_path)) {
                const auto value = read_json(theme_path, 1024);

                // The 6.11.1 guest sets the mode and the skin; any other guest takes theme_id
                // alone.
                if (a[6] == 0x6110001) {
                    if (!value.contains("theme_skin")) {
                        return {0, {}, "owned-theme-contract-mismatch"};
                    }

                    need(value.contains("seq") && value.at("seq").is_number_unsigned() &&
                                 value.contains("theme_id") &&
                                 value.at("theme_id").is_number_integer() &&
                                 value.contains("theme_skin") &&
                                 value.at("theme_skin").is_number_integer(),
                         "Free A11 6.11.1 theme command schema");
                    const auto seq = value.at("seq").get<std::uint64_t>();
                    const auto mode_number = value.at("theme_id").get<std::int64_t>(),
                               skin_number = value.at("theme_skin").get<std::int64_t>();
                    need(mode_number >= 1 && mode_number <= 6 && skin_number >= 1 &&
                                 skin_number <= 3003,
                         "Free A11 6.11.1 theme integer extent");
                    const auto mode = U(mode_number), skin = U(skin_number);
                    need(seq && seq < 0x100000000ULL &&
                                 (skin == 1 || skin == 1001 || skin == 3001 || skin == 3002 ||
                                  skin == 3003) &&
                                 mode == (skin + 999) / 1000,
                         "Free A11 6.11.1 theme mode/skin contract");
                    need(a[0] < 0x100000000ULL && a[1] <= UINT32_MAX && a[7] <= UINT32_MAX &&
                                 a[8] < 0x100000000ULL && a[9] <= 5,
                         "Free A11 6.11.1 theme getter report extent");

                    if (theme_issued && a[0] == theme_issued && a[0] > theme_acked &&
                        a[8] == a[0] && a[9] == 4 && a[1] == theme_mode_issued &&
                        a[7] == theme_skin_issued) {
                        replace_json(producer / "theme-ack.json",
                                     {{"schema", 1},
                                      {"sequence", a[0]},
                                      {"producer_address", options.address},
                                      {"guest_acknowledged", true},
                                      {"contract", "qinggan11-611-theme-v1"},
                                      {"theme_id", theme_mode_issued},
                                      {"theme_skin", theme_skin_issued},
                                      {"observed_requested_theme", U(a[1])},
                                      {"observed_skin", U(a[7])},
                                      {"host_monotonic_ns", now_ns()}},
                                     options.address);
                        theme_acked = a[0];
                    }

                    const bool failed = a[8] == seq && a[9] && a[9] != 4;
                    const bool unchanged = !failed && (!theme_issued || a[0] >= theme_issued) &&
                                           a[1] == mode && a[7] == skin;

                    if (seq != theme_reported || unchanged != theme_already_selected ||
                        a[9] != theme_last_outcome) {
                        const char *outcomes[] = {"none",
                                                  "mode_ipc_failed",
                                                  "skin_ipc_failed",
                                                  "getter_mismatch",
                                                  "complete",
                                                  "request_entry_missing"};
                        replace_json(producer / "theme-observed.json",
                                     {{"schema", 1},
                                      {"sequence", seq},
                                      {"producer_address", options.address},
                                      {"source", "original-requested-theme-and-skin-getters"},
                                      {"contract", "qinggan11-611-theme-v1"},
                                      {"theme_id", mode},
                                      {"theme_skin", skin},
                                      {"observed_requested_theme", U(a[1])},
                                      {"observed_skin", U(a[7])},
                                      {"native_attempt_sequence", a[8]},
                                      {"native_outcome", outcomes[a[9]]},
                                      {"request_suppressed", unchanged},
                                      {"host_monotonic_ns", now_ns()}},
                                     options.address);
                        theme_reported = seq;
                        theme_already_selected = unchanged;
                        theme_last_outcome = U(a[9]);
                    }

                    if (seq <= a[0] || failed || unchanged) {
                        return {0,
                                {},
                                failed      ? "owned-theme-partial-failure"
                                : unchanged ? "owned-theme-already-selected"
                                            : "owned-theme-request"};
                    }

                    theme_issued = seq;
                    theme_mode_issued = mode;
                    theme_skin_issued = skin;
                    Reply out{seq, {}, "owned-qinggan11-611-theme-request"};
                    append(out.payload, mode);
                    append(out.payload, skin);

                    return out;
                }
            }

            ack("theme-ack.json", a[0], theme_issued, theme_acked, false);
            auto p = options.session / "theme-command.json";

            if (!std::filesystem::exists(p)) {
                return {0, {}, "owned-theme-request"};
            }

            auto value = read_json(p, 1024);
            need(value.is_object() && value.contains("seq") && value.contains("theme_id") &&
                         value["seq"].is_number_unsigned() && value["theme_id"].is_number_integer(),
                 "Theme command schema");
            auto seq = value["seq"].get<std::uint64_t>();
            auto mode = value["theme_id"].get<I>();
            need(seq >= 1 && seq < 0x100000000ULL && mode >= 1 && mode <= 6,
                 "Theme command geometry");

            if (seq <= a[0]) {
                return {0, {}, "owned-theme-request"};
            }

            const bool unchanged = (!theme_issued || a[0] >= theme_issued) &&
                                   qnx_theme_display_matches(U(mode), a[1]);

            if (seq != theme_reported || unchanged != theme_already_selected) {
                logging::write(logging::Level::info, "graphics", "theme.request",
                               {{"producer_address", options.address},
                                {"sequence", seq},
                                {"requested_theme", mode},
                                {"observed_display_theme", a[1]},
                                {"action", unchanged ? "already_selected" : "request_change"}});
                replace_json(producer / "theme-observed.json",
                             {{"schema", 1},
                              {"sequence", seq},
                              {"producer_address", options.address},
                              {"source", "original-setting-getter"},
                              {"theme_id", U(mode)},
                              {"observed_display_theme", a[1]},
                              {"request_suppressed", unchanged},
                              {"host_monotonic_ns", now_ns()}},
                             options.address);
                theme_reported = seq;
                theme_already_selected = unchanged;
            }

            if (unchanged) {
                return {0, {}, "owned-theme-already-selected"};
            }

            theme_issued = seq;

            return {seq, {std::byte(mode)}, "owned-theme-request"};
        }

        if (op == 365) {
            // Owned QNX cluster language command. Explicit opcode (353 glShaderSource and
            // 354 OEM Screen stream are already owned); never resolved by argument count.
            auto p = options.session / "language-command.json";

            if (!std::filesystem::exists(p)) {
                return {0, {}, "owned-language-request"};
            }

            auto value = read_json(p, 1024);
            need(value.is_object() && value.contains("seq") && value.contains("value") &&
                         value["seq"].is_number_unsigned() && value["value"].is_number_integer(),
                 "Language command schema");
            auto seq = value["seq"].get<std::uint64_t>();
            auto language = value["value"].get<std::uint64_t>();
            need(seq >= 1 && seq < 0x100000000ULL && language <= 1, "Language command geometry");

            if (seq <= a[0]) {
                return {0, {}, "owned-language-request"};
            }

            return {seq, {std::byte(language)}, "owned-language-request"};
        }

        if (op == 366) {
            // A submission is not the observed effect: record request_issued only. An OEM
            // getter observation would be required to claim guest_acknowledged.
            need(a[1] <= 1, "Language report flag");

            if (!a[0]) {
                return {0, {}, "owned-language-report"};
            }

            auto p = options.session / "language-command.json";

            if (!std::filesystem::exists(p)) {
                return {0, {}, "owned-language-report"};
            }

            auto value = read_json(p, 1024);
            need(value.is_object() && value.contains("seq") && value.contains("value") &&
                         value["seq"].is_number_unsigned(),
                 "Language command schema");
            auto seq = value["seq"].get<std::uint64_t>();
            auto language = value["value"].get<std::uint64_t>();

            if (a[0] != seq) {
                return {0, {}, "owned-language-report"};
            }

            replace_json(producer / "language-ack.json",
                         {{"schema", 1},
                          {"sequence", seq},
                          {"producer_address", options.address},
                          {"language", U(language)},
                          {"native_request_issued", a[1] != 0},
                          {"host_monotonic_ns", now_ns()}},
                         options.address);

            return {0, {}, "owned-language-report"};
        }

        if (!controls()) {
            return {0, {}, op == 363 ? "foreign-wheel-owner" : "foreign-MCU-owner"};
        }

        if (op == 363) {
            ack("wheel-ack.json", a[0], wheel_issued, wheel_acked);
            auto p = options.session / "wheel-command.json";

            if (!std::filesystem::exists(p)) {
                return {0, {}, "owned-wheel-phase"};
            }

            auto value = read_json(p, 1024);
            need(value.is_object() && value.size() == 3 && value.contains("sequence") &&
                         value.contains("index") && value.contains("value"),
                 "Wheel command schema");
            need(value["sequence"].is_number_unsigned() && value["index"].is_number_integer() &&
                         value["value"].is_number_integer(),
                 "Wheel command integer types");
            auto seq = value["sequence"].get<std::uint64_t>();
            auto index = value["index"].get<std::uint64_t>();
            auto phase = value["value"].get<std::uint64_t>();
            need(seq >= 1 && seq < 0x100000000ULL && index >= 322 && index <= 331 && phase <= 1,
                 "Wheel command phase geometry");

            if (seq <= a[0]) {
                return {0, {}, "owned-wheel-phase"};
            }

            need(!wheel_issued || a[0] >= wheel_issued || seq == wheel_issued,
                 "Wheel queue replaced before guest ACK");
            wheel_issued = seq;
            Reply out{seq, {}, "owned-wheel-phase"};
            append(out.payload, U(index));
            append(out.payload, U(phase));

            return out;
        }

        const auto scope = qnx_mcu_request_projection(a[1], a[2], a[3], options.mcu_adapter);
        const Json *layout = nullptr;

        if (a[1] == 650 && a[2] == 736) {
            for (const auto &spec : qnx_adapter_registry) {
                if ((options.mcu_adapter.empty() || spec.name == options.mcu_adapter) &&
                    qnx_mcu_runtime_scope(spec, a[3])) {
                    layout = &mcu_layout(spec);
                }
            }
        }

        ack("vehicle-ack.json", a[0], vehicle_issued, vehicle_acked, true, scope);
        auto path = (std::filesystem::exists(options.session / "mcu-state.json")
                             ? options.session / "mcu-state.json"
                             : options.session / "vehicle-state.json");

        if (!std::filesystem::exists(path)) {
            return {0, {}, "persistent-full-MCU-input"};
        }

        auto value = read_json(path, 128 * 1024);
        need(value.is_object() && value.size() == 6 && value.at("schema") == 1 &&
                     value.at("origin") == "manual-emulator" &&
                     value.at("sequence").is_number_unsigned() &&
                     value.at("updated_utc").is_string(),
             "Full vehicle store schema");
        // A qinggan11 guest reads the qinggan11 catalogue, every other guest the qinggan09 one.
        const auto catalog = layout ? VehicleCatalog::qinggan11 : VehicleCatalog::qinggan09;
        need(value.at("map_sha256") == catalog_map_sha256(catalog), "Vehicle catalog of the guest");
        auto seq = value.at("sequence").get<std::uint64_t>();
        need(seq >= 1 && seq < 0x100000000ULL, "Full vehicle sequence");
        const auto &raw = value.at("raw_oem");
        const auto &fields = voyemu::catalog(catalog);
        need(raw.is_object() && raw.size() == fields.size(), "Complete vehicle state required");
        // A qinggan11 guest rejects the whole revision when an unprojected field leaves the
        // initial value, so that field reaches it as the initial value.
        const auto guest = layout ? qnx_qinggan11_guest_state(*layout, raw) : raw;
        Reply out{seq, {}, "persistent-full-MCU-input"};
        append(out.payload, U(1));
        append(out.payload, U(fields.size()));

        for (const auto &f : fields) {
            need(raw.contains(f.name) && raw.at(f.name).is_number_integer(),
                 "OEM field integer required");
            const auto v = raw.at(f.name).get<std::int64_t>();
            need(v >= 0 && std::uint64_t(v) < (1ULL << f.width) &&
                         (!f.name.starts_with("swm_") || !v),
                 "OEM field width/transient state");
            append(out.payload, guest.at(f.name).get<U>());
        }

        need(concepts_consistent(catalog, raw), "Consistent speed/SOC required");

        if (seq <= a[0]) {
            return {0, {}, out.name};
        }

        vehicle_issued = seq;

        return out;
    }

    void notify_input(std::uint64_t *words) {
        std::uint64_t vehicle = 0, wheel = 0;
        try {
            auto path = (std::filesystem::exists(options.session / "mcu-state.json")
                                 ? options.session / "mcu-state.json"
                                 : options.session / "vehicle-state.json");

            if (std::filesystem::exists(path)) {
                auto value = read_json(path, vehicle_state_bytes);
                need(value.is_object() && value.size() == 6 && value.at("schema") == 1 &&
                             (value.at("map_sha256") == qinggan09_map_sha256 ||
                              value.at("map_sha256") == qinggan11_map_sha256 ||
                              value.at("map_sha256") == passion_map_sha256) &&
                             value.at("origin") == "manual-emulator" &&
                             value.at("sequence").is_number_unsigned(),
                     "Input wake vehicle schema");
                auto seq = value.at("sequence").get<std::uint64_t>();
                need(seq >= 1 && seq < 0x100000000ULL, "Input wake vehicle sequence");

                // The Passion cluster receives no vehicle state from the model.
                if (seq > vehicle_issued && value.at("map_sha256") != passion_map_sha256) {
                    vehicle = seq;
                }
            }

            path = options.session / "wheel-command.json";

            if (std::filesystem::exists(path)) {
                auto value = read_json(path, 1024);
                need(value.is_object() && value.size() == 3 &&
                             value.at("sequence").is_number_unsigned() &&
                             value.at("index").is_number_integer() &&
                             value.at("value").is_number_integer(),
                     "Input wake wheel schema");
                auto seq = value.at("sequence").get<std::uint64_t>(),
                     index = value.at("index").get<std::uint64_t>(),
                     phase = value.at("value").get<std::uint64_t>();
                need(seq >= 1 && seq < 0x100000000ULL && index >= 322 && index <= 331 && phase <= 1,
                     "Input wake wheel geometry");

                if (seq > wheel_issued) {
                    wheel = seq;
                }
            }

            input_wake_error = false;
        } catch (const std::exception &error) {
            vehicle = wheel = 0;

            if (!input_wake_error) {
                logging::write(logging::Level::error, "graphics", "input.wake_rejected",
                               {{"producer_address", options.address}, {"error", error.what()}});
            }

            input_wake_error = true;
        }

        const auto value = (vehicle << 32) | wheel;
        std::atomic_ref(words[2]).store(value, std::memory_order_release);
        std::atomic_ref(words[1]).store(input_wake_magic, std::memory_order_release);

        if (value && value != last_input_wake) {
            logging::write(logging::Level::debug, "graphics", "input.wake",
                           {{"producer_address", options.address},
                            {"vehicle_sequence", vehicle},
                            {"wheel_sequence", wheel}});
        }

        last_input_wake = value;
    }

    std::size_t query_count(const GeneratedSpec &s, const Arguments &a) {
        std::string_view name = s.name;
        U key = U(s.argc > 1 ? a[s.argc - 2] : a[0]);

        if (name == "glGetVertexAttribPointerv") {
            return 1;
        }

        if (name.starts_with("glGetUniform")) {
            I count = 0;
            gl->call<void>("glGetProgramiv", U(a[0]), U(0x8b86), &count);
            need(count >= 0 && count <= 65536, "Active uniform count");

            for (I i = 0; i < count; i++) {
                std::array<char, 1024> text{};
                I length = 0, size = 0;
                U kind = 0;
                gl->call<void>("glGetActiveUniform", U(a[0]), U(i), I(text.size()), &length, &size,
                               &kind, text.data());
                need(length >= 0 && length < 1024 && size >= 0, "Active uniform geometry");
                I base = gl->call<I>("glGetUniformLocation", U(a[0]),
                                     static_cast<const char *>(text.data()));
                I location = signed_bits(a[1]);

                if (base >= 0 && location >= base &&
                    std::int64_t(location) < std::int64_t(base) + std::max(1, size)) {
                    switch (kind) {
                    case 0x8b50:
                    case 0x8b53:
                    case 0x8b57:
                        return 2;
                    case 0x8b51:
                    case 0x8b54:
                    case 0x8b58:
                        return 3;
                    case 0x8b52:
                    case 0x8b55:
                    case 0x8b59:
                    case 0x8b5a:
                        return 4;
                    case 0x8b5b:
                        return 9;
                    case 0x8b5c:
                        return 16;
                    default:
                        return 1;
                    }
                }
            }

            return 1;
        }

        switch (key) {
        case 0xba2:
        case 0xc10:
        case 0x8005:
        case 0xc23:
        case 0x8626:
        case 0x8e46:
            return 4;
        case 0xb70:
        case 0x846d:
        case 0x846e:
        case 0xd3a:
            return 2;
        case 0x86a3:
        case 0x8df8: {
            I n = 0;
            gl->call<void>("glGetIntegerv", U(key == 0x86a3 ? 0x86a2 : 0x8df9), &n);
            need(n >= 0 && n <= 512, "GL query count");

            return std::size_t(n);
        }
        default:
            return 1;
        }
    }

    std::size_t required_input(const GeneratedSpec &s, const Arguments &a,
                               std::size_t inline_bytes = 0) {
        std::string_view n = s.name;

        if (n == "glBufferData") {
            return count_bytes(a[1], 1);
        }

        if (n == "glBufferSubData") {
            return count_bytes(a[2], 1);
        }

        if (n == "glCompressedTexImage2D") {
            return count_bytes(a[6], 1);
        }

        if (n == "glCompressedTexImage3D") {
            return count_bytes(a[7], 1);
        }

        if (n == "glCompressedTexSubImage2D") {
            return count_bytes(a[7], 1);
        }

        if (n == "glProgramBinary") {
            return count_bytes(a[3], 1);
        }

        if (n == "glUniform1fv") {
            return count_bytes(a[1], 4);
        }

        if (n == "glUniform3fv") {
            return count_bytes(a[1], 12);
        }

        if (n == "glUniform4fv") {
            return count_bytes(a[1], 16);
        }

        if (n == "glUniformMatrix3fv") {
            return count_bytes(a[1], 36);
        }

        if (n == "glUniformMatrix4fv") {
            return count_bytes(a[1], 64);
        }

        if (n.starts_with("glDelete") || n == "glDrawBuffers") {
            return count_bytes(a[0], 4);
        }

        if (n == "glInvalidateFramebuffer") {
            return count_bytes(a[1], 4);
        }

        if (n.starts_with("glClearBuffer")) {
            return a[0] == 0x1800 ? 16 : 4;
        }

        if (std::string_view(s.category) == "pixels") {
            const auto w = n == "glTexImage2D" ? a[3] : a[4], h = n == "glTexImage2D" ? a[4] : a[5];
            U format = U(a[6]), type = U(a[7]);
            I native_limit = 0;
            gl->call<void>("glGetIntegerv", U(0xd33), &native_limit);
            need(native_limit > 0, "Native texture limit");
            const auto limit = std::uint64_t(std::min(native_limit, 16384));

            if (w > limit || h > limit) {
                throw VoyemuError("GRAPHICS", "Texture dimensions: target=" + std::to_string(a[0]) +
                                                      " level=" + std::to_string(a[1]) +
                                                      " width=" + std::to_string(w) +
                                                      " height=" + std::to_string(h) +
                                                      " format=" + std::to_string(format) +
                                                      " type=" + std::to_string(type));
            }

            if (n == "glTexSubImage2D") {
                need(a[2] <= limit && a[3] <= limit && w <= limit - a[2] && h <= limit - a[3],
                     "Texture subimage bounds");
            }

            if (!w || !h) {
                return 0;
            }

            std::size_t channels = 0, unit = 0;

            switch (format) {
            case 0x1908:
            case 0x80e1:
            case 0x8d99:
                channels = 4;
                break;
            case 0x1907:
            case 0x8d98:
                channels = 3;
                break;
            case 0x8227:
            case 0x8228:
            case 0x190a:
                channels = 2;
                break;
            case 0x1903:
            case 0x8d94:
            case 0x1906:
            case 0x1909:
            case 0x1902:
            case 0x84f9:
                channels = 1;
                break;
            default:
                break;
            }

            switch (type) {
            case 0x1400:
            case 0x1401:
                unit = channels;
                break;
            case 0x1402:
            case 0x1403:
            case 0x140b:
                unit = channels * 2;
                break;
            case 0x1404:
            case 0x1405:
            case 0x1406:
                unit = channels * 4;
                break;
            case 0x8363:
            case 0x8033:
            case 0x8034:
                unit = 2;
                break;
            case 0x8368:
            case 0x8c3b:
            case 0x8c3e:
            case 0x84fa:
                unit = 4;
                break;
            case 0x8dad:
                unit = 8;
                break;
            default:
                break;
            }

            if (!channels || !unit) {
                I unpack_buffer = 0;
                gl->call<void>("glGetIntegerv", U(0x88ef), &unpack_buffer);

                // OEM's storage request carries an invalid GL format and no source pixels.
                // Native GL must report INVALID_ENUM without turning it into a transport fault.
                if (n == "glTexImage2D" && !a[6] && a[7] == 0x1401 && !inline_bytes &&
                    !a[s.pointer] && !unpack_buffer) {
                    return 0;
                }

                throw VoyemuError(
                        "GRAPHICS",
                        "Texture format/type copy extent: target=" + std::to_string(a[0]) +
                                " level=" + std::to_string(a[1]) + " width=" + std::to_string(w) +
                                " height=" + std::to_string(h) + " format=" +
                                std::to_string(format) + " type=" + std::to_string(type) +
                                " inline_bytes=" + std::to_string(inline_bytes) +
                                " blob=" + std::to_string(a[s.pointer] != 0) +
                                " unpack_buffer=" + std::to_string(unpack_buffer != 0));
            }

            I alignment = 0, row_length = 0, skip_rows = 0, skip_pixels = 0, binding = 0;
            gl->call<void>("glGetIntegerv", U(0xcf5), &alignment);
            gl->call<void>("glGetIntegerv", U(0xcf2), &row_length);
            gl->call<void>("glGetIntegerv", U(0xcf3), &skip_rows);
            gl->call<void>("glGetIntegerv", U(0xcf4), &skip_pixels);
            gl->call<void>("glGetIntegerv", U(0x88ef), &binding);
            need((alignment == 1 || alignment == 2 || alignment == 4 || alignment == 8) &&
                         row_length >= 0 && std::uint64_t(row_length) <= limit && skip_rows >= 0 &&
                         std::uint64_t(skip_rows) <= limit && skip_pixels >= 0 &&
                         std::uint64_t(skip_pixels) <= limit && !binding,
                 "Texture unpack state");
            auto row = (std::size_t(row_length ? row_length : w) * unit + alignment - 1) &
                       ~std::size_t(alignment - 1);
            auto size = std::size_t(skip_rows) * row + std::size_t(skip_pixels) * unit +
                        (h - 1) * row + w * unit;
            need(size <= max_blob, "Texture upload extent");

            return size;
        }

        return 0;
    }

    Reply generated_call(U op, const Arguments &a, std::span<const std::byte> data) {
        const auto &s = spec_for(op);
        std::string_view category = s.category, name = s.name;
        Arguments values = a;
        std::vector<std::byte> owned;
        need(category != "special" && category != "unsupported",
             "Unsupported direct generated semantics");

        // Frame census for the scene probe: which framebuffers the guest ever bound, how many draw
        // calls each one received (framebuffer 0 included, since the composite into the window
        // framebuffer is the draw whose result is missing), and the viewport last used with each of
        // them, which is the rectangle a readback of a framebuffer without a queryable attachment
        // has to use. Read-only, three names only.
        if (!replaying) {
            if (name == "glBindFramebuffer" &&
                (a[0] == 0x8d40 || a[0] == 0x8ca9 || a[0] == 0x8caa) && a[1] &&
                seen_fbs.size() < 16) {
                seen_fbs.insert(I(a[1]));
            } else if (name == "glDrawElements" || name == "glDrawArrays") {
                I target = 0;
                gl->call<void>("glGetIntegerv", U(0x8ca6), &target);
                ++draw_calls_by_fb[target];
                ++draw_calls_all[target];
                pending_draw = target == 0;
                pending_draw_target = target;

                if (live_swaps >= 1 && draw_probe_events < 24) {
                    Json entry = probe_draw_state(target);
                    entry["event"] = draw_probe_events;
                    entry["name"] = std::string(name);
                    entry["count"] = U(a[1]);
                    ++draw_probe_events;
                    logging::write(logging::Level::debug, "graphics", "screen.draw_probe", entry);
                }
            }
            // The colour the composite multiplies its texture with: a zero uniform tints every
            // layer black however good the texture is, so the first few are recorded verbatim.
            else if (name == "glUniform4fv" && data.size() >= 16 && uniform_probe_events < 8) {
                const auto *values32 = reinterpret_cast<const float *>(data.data());
                Json rgba = Json::array();

                for (std::size_t k = 0; k < 4; k++) {
                    rgba.push_back(double(values32[k]));
                }

                logging::write(
                        logging::Level::debug, "graphics", "screen.uniform_probe",
                        {{"event", uniform_probe_events}, {"location", U(a[0])}, {"value", rgba}});
                ++uniform_probe_events;
            }
            // The vertex transform of the composite: a matrix the host hands over with a wrong
            // transpose or layout collapses every layer to a point, which looks exactly like an
            // empty frame. The first values are recorded so the guest's own numbers can be compared
            // with them. The Qt shader scales every colour by a scalar uniform named opacity, so a
            // zero there makes an otherwise correct composite invisible; the scalar and the
            // blending state are recorded.
            else if ((name == "glUniform1f" || name == "glUniform1fv") &&
                     uniform1_probe_events < 16) {
                Json fields = {{"event", uniform1_probe_events},
                               {"name", std::string(name)},
                               {"location", U(a[0])}};

                if (name == "glUniform1f") {
                    fields["value"] = double(float_bits(U(a[1])));
                } else if (data.size() >= 4) {
                    fields["value"] = double(*reinterpret_cast<const float *>(data.data()));
                }

                logging::write(logging::Level::debug, "graphics", "screen.uniform1_probe", fields);
                ++uniform1_probe_events;
            }
            // The Qt text-mask shader turns a glyph's texture coordinates into atlas coordinates by
            // multiplying them with a vec2 uniform; a zero there sends every fragment of the layer
            // to the same empty texel, which is indistinguishable from an empty texture. Each value
            // is recorded so a uniform the guest never sets can be told from one it sets to zero.
            else if ((name == "glUniform2f" || name == "glUniform2fv") &&
                     uniform2_probe_events < 16) {
                Json pair = Json::array();

                if (name == "glUniform2f") {
                    pair.push_back(double(float_bits(U(a[1]))));
                    pair.push_back(double(float_bits(U(a[2]))));
                } else if (data.size() >= 8) {
                    const auto *values32 = reinterpret_cast<const float *>(data.data());
                    pair.push_back(double(values32[0]));
                    pair.push_back(double(values32[1]));
                }

                logging::write(logging::Level::debug, "graphics", "screen.uniform2_probe",
                               {{"event", uniform2_probe_events},
                                {"name", std::string(name)},
                                {"location", U(a[0])},
                                {"value", pair}});
                ++uniform2_probe_events;
            } else if ((name == "glColorMask" || name == "glBlendFunc" ||
                        name == "glBlendFuncSeparate" || name == "glDepthFunc" ||
                        name == "glDepthMask" || name == "glEnable" || name == "glDisable") &&
                       state_probe_events < 48) {
                Json arguments = Json::array();

                for (std::size_t k = 0; k < a.size() && k < 4; k++) {
                    arguments.push_back(U(a[k]));
                }

                logging::write(logging::Level::debug, "graphics", "screen.state_probe",
                               {{"event", state_probe_events},
                                {"name", std::string(name)},
                                {"arguments", arguments}});
                ++state_probe_events;
            } else if (name == "glUniformMatrix4fv" && data.size() >= 64 &&
                       matrix_probe_events < 6) {
                const auto *m = reinterpret_cast<const float *>(data.data());
                Json values = Json::array();

                for (std::size_t k = 0; k < 16; k++) {
                    values.push_back(double(m[k]));
                }

                logging::write(logging::Level::debug, "graphics", "screen.matrix_probe",
                               {{"event", matrix_probe_events},
                                {"location", U(a[0])},
                                {"count", U(a[1])},
                                {"transpose", U(a[2])},
                                {"value", values}});
                ++matrix_probe_events;
            }
        }

        if (category == "string") {
            need(!data.empty() && data.size() <= rpc_payload_bytes && data.back() == std::byte{0},
                 "String terminator/extent");
            owned.assign(data.begin(), data.end());
            values[s.pointer] = reinterpret_cast<std::uintptr_t>(owned.data());
        } else if (category == "vector" && (name.starts_with("glUniform") || clear_vector(name)) &&
                   !a[s.pointer]) {
            if (clear_vector(name)) {
                validate_inline_clear(name, a, data);
            } else {
                validate_inline_uniform(name, a, data);
            }

            owned.assign(data.begin(), data.end());
            values[s.pointer] = reinterpret_cast<std::uintptr_t>(owned.data());
        } else if (category == "pixels" && !data.empty()) {
            need(!a[s.pointer] && data.size() <= rpc_payload_bytes &&
                         data.size() == required_input(s, a, data.size()),
                 "Inline texture extent");
            owned.assign(data.begin(), data.end());
            values[s.pointer] = reinterpret_cast<std::uintptr_t>(owned.data());
        } else if (category == "vector" || category == "blob" || category == "pixels") {
            need(data.empty(), "Unexpected payload with blob");
            auto b = blob(a[s.pointer], category == "pixels");
            const auto required = required_input(s, a);

            if (name == "glTexImage2D" && a[3] && a[4] && !a[6] && a[7] == 0x1401 &&
                !a[s.pointer]) {
                // The checked no-data request carries an invalid native format. Preserve its
                // GL error for the guest and do not record successful texture storage.
                return {generated->invoke(op, values), {}, s.name};
            }

            if (!b) {
                need(category != "vector" && name != "glBufferSubData" &&
                             name != "glProgramBinary" && name != "glCompressedTexSubImage2D" &&
                             name != "glTexSubImage2D",
                     "Missing input blob");
                values[s.pointer] = 0;
            } else {
                need(b->direction == 0 && b->size >= required &&
                             (!b->texture_only || b->valid_bytes >= required),
                     "Input blob direction/typed extent");
                values[s.pointer] = reinterpret_cast<std::uintptr_t>(b->bytes.data());
            }
        } else if (category == "offset") {
            need(data.empty(), "Offset payload");
            I binding = 0;
            gl->call<void>("glGetIntegerv", U(name == "glVertexAttribPointer" ? 0x8894 : 0x8895),
                           &binding);
            need(binding || !a[s.pointer], "Client arrays require copying");
        } else {
            need(data.empty(), "Unexpected scalar/query payload");
        }

        if (category == "generate") {
            need(a[0] <= max_blob / 4, "GL object count");
            const auto bytes = std::size_t(a[0]) * 4;

            if (!a[s.pointer]) {
                need(bytes <= rpc_payload_bytes, "Inline object output extent");
                owned.resize(bytes);
                values[s.pointer] = owned.empty() ? 0
                                                  : reinterpret_cast<std::uintptr_t>(owned.data());
                generated->invoke(op, values);

                return {bytes, std::move(owned), s.name};
            }

            auto b = blob(a[s.pointer]);
            need(b && b->direction == 1 && b->size >= bytes, "GL object output extent");
            owned.resize(bytes);
            values[s.pointer] = reinterpret_cast<std::uintptr_t>(owned.data());
            generated->invoke(op, values);
            write_blob(a[s.pointer], owned);

            return {bytes, {}, s.name};
        }

        if (category == "query") {
            auto count = query_count(s, a);
            const auto unit = name == "glGetBooleanv"               ? 1
                              : name == "glGetVertexAttribPointerv" ? sizeof(V)
                                                                    : 4;
            need(count <= rpc_payload_bytes / unit, "GL query output extent");
            owned.resize(count * unit);
            values[s.pointer] = reinterpret_cast<std::uintptr_t>(owned.data());
            generated->invoke(op, values);

            return {0, std::move(owned), s.name};
        }

        // Whether the scene's own images reach the GPU with content: the compositor uploads its
        // layers with glTex(Sub)Image2D, and a black frame is also what an all-zero upload
        // produces, so the payload is counted before it is handed to GL. Bounded to the first
        // calls, read-only.
        if (!replaying && category == "pixels" && values[s.pointer] && upload_probe_events < 32 &&
            (name == "glTexSubImage2D" || name == "glTexImage2D")) {
            const bool sub = name == "glTexSubImage2D";
            const auto size = required_input(s, a);
            const auto *bytes = reinterpret_cast<const std::byte *>(values[s.pointer]);
            const std::size_t unit = size >= 4 && size % 4 == 0 ? 4 : 3;
            std::size_t nonzero_bytes = 0, nonzero_rgb = 0;

            for (std::size_t i = 0; i < size; i++) {
                if (bytes[i] != std::byte{0}) {
                    ++nonzero_bytes;
                }
            }

            for (std::size_t i = 0; i + unit <= size; i += unit) {
                bool any = false;

                for (std::size_t k = 0; k + 1 < unit; k++) {
                    if (bytes[i + k] != std::byte{0}) {
                        any = true;
                    }
                }

                if (any) {
                    ++nonzero_rgb;
                }
            }

            Json fields = {{"event", upload_probe_events},
                           {"name", std::string(name)},
                           {"width", U(a[sub ? 4 : 3])},
                           {"height", U(a[sub ? 5 : 4])},
                           {"format", U(a[6])},
                           {"type", U(a[7])},
                           {"bytes", size},
                           {"unit", unit},
                           {"nonzero_bytes", nonzero_bytes},
                           {"nonzero_rgb", nonzero_rgb}};

            if (sub) {
                fields["x"] = U(a[2]);
                fields["y"] = U(a[3]);
            }

            ++upload_probe_events;
            logging::write(logging::Level::debug, "graphics", "screen.upload_probe", fields);
        }

        if (pending_draw && !replaying) {
            const bool bound = pending_draw_target == 0;
            pending_draw = false;

            if (bound) {
                auto result = generated->invoke(op, values);

                // Whether the depth test is what stops the composite: the guest draws with the test
                // enabled and depth writes off, and a window depth buffer that was never cleared
                // rejects every fragment, which leaves the frame at exactly the colour it was
                // cleared to. The same draw is issued a second time with the test switched off and
                // the two readbacks are compared; the state is put back and the window cleared
                // again, so nothing of the probe stays in the frame.
                if (!depth_reissue_done && name == "glDrawElements") {
                    depth_reissue_done = true;
                    I depth_enabled = 0, depth_func = 0, depth_bits = 0, stencil_bits = 0,
                      samples = 0;
                    gl->call<void>("glGetIntegerv", U(0x0b71), &depth_enabled);
                    gl->call<void>("glGetIntegerv", U(0x0b74), &depth_func);
                    gl->call<void>("glGetIntegerv", U(0x0d56), &depth_bits);
                    gl->call<void>("glGetIntegerv", U(0x0d57), &stencil_bits);
                    gl->call<void>("glGetIntegerv", U(0x80a9), &samples);
                    const U state_error = gl->call<U>("glGetError");
                    const auto before = probe_framebuffer(false, true);
                    gl->call<void>("glDisable", U(0x0b71));
                    const auto second = generated->invoke(op, values);
                    gl->call<void>("glFinish");
                    const auto after = probe_framebuffer(false, true);
                    gl->call<void>(depth_enabled ? "glEnable" : "glDisable", U(0x0b71));
                    float saved_clear[4] = {0, 0, 0, 0};
                    gl->call<void>("glGetFloatv", U(0x0c22), saved_clear);
                    gl->call<void>("glClear", U(0x4000));
                    gl->call<void>("glFinish");
                    drain_gl_errors();
                    logging::write(
                            logging::Level::debug, "graphics", "screen.depth_reissue_probe",
                            {{"depth_enabled", depth_enabled},
                             {"depth_func", depth_func},
                             {"depth_bits", depth_bits},
                             {"stencil_bits", stencil_bits},
                             {"samples", samples},
                             {"state_error", state_error},
                             {"nonzero_before", before.nonzero_rgb},
                             {"nonzero_after", after.nonzero_rgb},
                             {"sampled", std::size_t(after.width) * std::size_t(after.height)},
                             {"samples_after", sample_json(after.sample)},
                             {"second_result", second},
                             {"clear_color",
                              Json::array({double(saved_clear[0]), double(saved_clear[1]),
                                           double(saved_clear[2]), double(saved_clear[3])})}});
                }

                // What a guest composite into framebuffer 0 is made of: the program it runs, the
                // matrices and sampler it holds, and the texture that sampler reads. That texture
                // is then sampled by a host program at four points of a scratch 2x2 target and read
                // back through an attachment to the texture's own level 0, so a black sample from a
                // texture that holds content separates a shading failure from missing data. Every
                // binding the probe changes is saved and put back.
                if (!composite_probe_done && name == "glDrawElements") {
                    I composite_target = 0;
                    gl->call<void>("glGetIntegerv", U(0x8ca6), &composite_target);

                    if (composite_target == 0) {
                        composite_probe_done = true;
                        I program = 0;
                        gl->call<void>("glGetIntegerv", U(0x8b8d), &program);
                        // Locations 0 and 1 carry the matrices (ANGLE takes a location index
                        // directly), and the sampler uniform is found by walking the active
                        // uniforms for the 2D sampler type.
                        float matrix0[16] = {0}, matrix1[16] = {0};
                        gl->call<void>("glGetUniformfv", U(program), I(0), matrix0);
                        gl->call<void>("glGetUniformfv", U(program), I(1), matrix1);
                        // Every active uniform is listed with its name, type and current value, not
                        // only the sampler: the composite's own colour and the smoothstep edges are
                        // plain uniforms, and a premultiplied colour at a fraction of a percent
                        // looks exactly like an empty frame.
                        I sampler_location = -1, sampler_unit = 0;
                        std::array<char, 256> uniform_name{};
                        Json uniforms = Json::array();

                        for (I i = 0; i < 32; i++) {
                            I length = 0, size = 0;
                            U type = 0;
                            gl->call<void>("glGetActiveUniform", U(program), U(i),
                                           I(uniform_name.size()), &length, &size, &type,
                                           uniform_name.data());

                            if (length <= 0) {
                                break;
                            }

                            const std::string uniform_text(uniform_name.data(),
                                                           std::size_t(length));
                            const I uniform_location = gl->call<I>(
                                    "glGetUniformLocation", U(program), uniform_text.c_str());
                            Json value = Json::array();

                            if (uniform_location >= 0) {
                                if (type == U(0x8b5e)) {
                                    I unit = 0;
                                    gl->call<void>("glGetUniformiv", U(program), uniform_location,
                                                   &unit);
                                    value.push_back(unit);
                                    sampler_location = uniform_location;
                                    sampler_unit = unit;
                                } else {
                                    const I slots = type == U(0x1406)   ? 1
                                                    : type == U(0x8b50) ? 2
                                                    : type == U(0x8b51) ? 3
                                                    : type == U(0x8b52) ? 4
                                                                        : 0;

                                    if (slots > 0) {
                                        float uniform_value[4] = {0, 0, 0, 0};
                                        gl->call<void>("glGetUniformfv", U(program),
                                                       uniform_location, uniform_value);

                                        for (I k = 0; k < slots; k++) {
                                            value.push_back(double(uniform_value[k]));
                                        }
                                    }
                                }
                            }

                            uniforms.push_back({{"name", uniform_text},
                                                {"type", type},
                                                {"size", size},
                                                {"location", uniform_location},
                                                {"value", value}});
                        }

                        I saved_active = 0;
                        gl->call<void>("glGetIntegerv", U(0x8873), &saved_active);
                        gl->call<void>("glActiveTexture", U(0x84c0 + sampler_unit));
                        I bound_texture = 0;
                        gl->call<void>("glGetIntegerv", U(0x8069), &bound_texture);
                        I min_filter = 0, mag_filter = 0, wrap_s = 0, wrap_t = 0, max_level = 0;
                        gl->call<void>("glGetTexParameteriv", U(0xde1), U(0x2801), &min_filter);
                        gl->call<void>("glGetTexParameteriv", U(0xde1), U(0x2800), &mag_filter);
                        gl->call<void>("glGetTexParameteriv", U(0xde1), U(0x2802), &wrap_s);
                        gl->call<void>("glGetTexParameteriv", U(0xde1), U(0x2803), &wrap_t);
                        gl->call<void>("glGetTexParameteriv", U(0xde1), U(0x813d), &max_level);
                        I saved_array = 0, saved_draw_fb = 0, saved_read_fb = 0, saved_enabled = 0,
                          saved_blend = 0, saved_mask[4] = {0, 0, 0, 0},
                          saved_viewport[4] = {0, 0, 0, 0};
                        gl->call<void>("glGetIntegerv", U(0x8894), &saved_array);
                        gl->call<void>("glGetIntegerv", U(0x8ca6), &saved_draw_fb);
                        gl->call<void>("glGetIntegerv", U(0x8caa), &saved_read_fb);
                        gl->call<void>("glGetVertexAttribiv", I(0), U(0x8622), &saved_enabled);
                        gl->call<void>("glGetIntegerv", U(0x0be2), &saved_blend);
                        gl->call<void>("glGetIntegerv", U(0x0c23), saved_mask);
                        gl->call<void>("glGetIntegerv", U(0x0ba2), saved_viewport);
                        // The probe draw reprograms attribute zero, and the guest keeps its own
                        // view of that state, so the whole description is saved and put back.
                        I saved_attrib_size = 0, saved_attrib_type = 0, saved_attrib_stride = 0,
                          saved_attrib_normalized = 0;
                        void *saved_attrib_pointer = nullptr;
                        gl->call<void>("glGetVertexAttribiv", I(0), U(0x8623), &saved_attrib_size);
                        gl->call<void>("glGetVertexAttribiv", I(0), U(0x8624),
                                       &saved_attrib_stride);
                        gl->call<void>("glGetVertexAttribiv", I(0), U(0x8625), &saved_attrib_type);
                        gl->call<void>("glGetVertexAttribiv", I(0), U(0x886a),
                                       &saved_attrib_normalized);
                        gl->call<void>("glGetVertexAttribPointerv", I(0), U(0x8645),
                                       &saved_attrib_pointer);
                        I unit0 = 0;
                        gl->call<void>("glActiveTexture", U(0x84c0));
                        gl->call<void>("glGetIntegerv", U(0x8069), &unit0);
                        const char *vertex_source =
                                "attribute vec2 p;void main(){gl_Position=vec4(p,0.0,1.0);}";
                        // The four fragments of the 2x2 target pick (0.15,0.15), (0.85,0.15),
                        // (0.15,0.85) and (0.85,0.85): the step turns each fragment's own corner
                        // into the mix factor.
                        const char *fragment_source =
                                "precision mediump float;uniform sampler2D t;void main(){vec2 uv=mix(vec2(0.15,0.15),vec2(0.85,0.85),step(vec2(1.0,1.0),gl_FragCoord.xy));gl_FragColor=texture2D(t,uv);}";
                        U vertex = gl->call<U>("glCreateShader", U(0x8b31));
                        U fragment = gl->call<U>("glCreateShader", U(0x8b30));
                        I vertex_size = I(std::strlen(vertex_source)),
                          fragment_size = I(std::strlen(fragment_source));
                        gl->call<void>("glShaderSource", vertex, I(1), &vertex_source,
                                       &vertex_size);
                        gl->call<void>("glCompileShader", vertex);
                        gl->call<void>("glShaderSource", fragment, I(1), &fragment_source,
                                       &fragment_size);
                        gl->call<void>("glCompileShader", fragment);
                        I vertex_status = 0, fragment_status = 0;
                        gl->call<void>("glGetShaderiv", vertex, U(0x8b81), &vertex_status);
                        gl->call<void>("glGetShaderiv", fragment, U(0x8b81), &fragment_status);
                        U host = gl->call<U>("glCreateProgram");
                        gl->call<void>("glAttachShader", host, vertex);
                        gl->call<void>("glAttachShader", host, fragment);
                        gl->call<void>("glBindAttribLocation", host, I(0), "p");
                        gl->call<void>("glLinkProgram", host);
                        I link_status = 0;
                        gl->call<void>("glGetProgramiv", host, U(0x8b82), &link_status);
                        I dst = 0;
                        gl->call<void>("glGenTextures", I(1), &dst);
                        gl->call<void>("glBindTexture", U(0xde1), dst);
                        gl->call<void>("glTexImage2D", U(0xde1), I(0), I(0x1908), I(2), I(2), I(0),
                                       U(0x1908), U(0x1401), static_cast<const void *>(nullptr));
                        I scratch = 0;
                        gl->call<void>("glGenFramebuffers", I(1), &scratch);
                        gl->call<void>("glBindFramebuffer", U(0x8d40), scratch);
                        gl->call<void>("glFramebufferTexture2D", U(0x8d40), U(0x8ce0), U(0xde1),
                                       dst, I(0));
                        const U status = gl->call<U>("glCheckFramebufferStatus", U(0x8d40));
                        I buffer = 0;
                        gl->call<void>("glGenBuffers", I(1), &buffer);
                        gl->call<void>("glBindBuffer", U(0x8892), buffer);
                        const float corners[6] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
                        gl->call<void>("glBufferData", U(0x8892), GLsizeiptr(sizeof(corners)),
                                       static_cast<const void *>(corners), U(0x88e0));
                        unsigned char grid[16] = {0};

                        if (status == U(0x8cd5)) {
                            gl->call<void>("glUseProgram", host);
                            gl->call<void>("glUniform1i",
                                           gl->call<I>("glGetUniformLocation", host, "t"), I(0));
                            gl->call<void>("glActiveTexture", U(0x84c0));
                            gl->call<void>("glBindTexture", U(0xde1), bound_texture);
                            gl->call<void>("glEnableVertexAttribArray", I(0));
                            gl->call<void>("glVertexAttribPointer", I(0), I(2), U(0x1406),
                                           GLboolean(0), I(0), static_cast<const V>(nullptr));
                            gl->call<void>("glViewport", I(0), I(0), I(2), I(2));
                            gl->call<void>("glDisable", U(0x0be2));
                            gl->call<void>("glColorMask", GLboolean(1), GLboolean(1), GLboolean(1),
                                           GLboolean(1));
                            gl->call<void>("glDrawArrays", U(0x0004), I(0), I(3));
                            gl->call<void>("glFinish");
                        }

                        const U sampled_error = gl->call<U>("glGetError");

                        if (status == U(0x8cd5)) {
                            gl->call<void>("glReadPixels", I(0), I(0), I(2), I(2), U(0x1908),
                                           U(0x1401), grid);
                        }

                        const U gl_error = gl->call<U>("glGetError");
                        std::string sampled[4];
                        Json sampled_grid = Json::array();

                        for (std::size_t k = 0; k < 4; k++) {
                            char text[32];
                            std::snprintf(text, sizeof(text), "%02x %02x %02x %02x", grid[k * 4],
                                          grid[k * 4 + 1], grid[k * 4 + 2], grid[k * 4 + 3]);
                            sampled[k] = text;
                            sampled_grid.push_back(sampled[k]);
                        }

                        // The texture's own level 0, independent of the sampler: the same scratch
                        // framebuffer is attached to the guest texture and two texels at its origin
                        // are read back, so a black sample from a texture that holds content is a
                        // shading failure, not missing data.
                        gl->call<void>("glFramebufferTexture2D", U(0x8d40), U(0x8ce0), U(0xde1),
                                       bound_texture, I(0));
                        const U attach_status = gl->call<U>("glCheckFramebufferStatus", U(0x8d40));
                        Json attached_level0;

                        if (attach_status == U(0x8cd5)) {
                            unsigned char texels[8] = {0};
                            gl->call<void>("glReadPixels", I(0), I(0), I(2), I(1), U(0x1908),
                                           U(0x1401), texels);
                            attached_level0 = Json::array();

                            for (std::size_t k = 0; k < 2; k++) {
                                char text[32];
                                std::snprintf(text, sizeof(text), "%02x %02x %02x %02x",
                                              texels[k * 4], texels[k * 4 + 1], texels[k * 4 + 2],
                                              texels[k * 4 + 3]);
                                attached_level0.push_back(text);
                            }
                        } else {
                            attached_level0 = attach_status;
                        }

                        gl->call<void>("glBindFramebuffer", U(0x8d40), saved_draw_fb);

                        if (saved_read_fb != saved_draw_fb) {
                            gl->call<void>("glBindFramebuffer", U(0x8caa), saved_read_fb);
                        }

                        gl->call<void>("glViewport", saved_viewport[0], saved_viewport[1],
                                       saved_viewport[2], saved_viewport[3]);
                        gl->call<void>(saved_blend ? "glEnable" : "glDisable", U(0x0be2));
                        gl->call<void>("glColorMask", GLboolean(saved_mask[0]),
                                       GLboolean(saved_mask[1]), GLboolean(saved_mask[2]),
                                       GLboolean(saved_mask[3]));
                        gl->call<void>("glBindBuffer", U(0x8892), saved_array);
                        gl->call<void>("glVertexAttribPointer", I(0), saved_attrib_size,
                                       U(saved_attrib_type), GLboolean(saved_attrib_normalized),
                                       saved_attrib_stride,
                                       static_cast<const V>(saved_attrib_pointer));

                        if (!saved_enabled) {
                            gl->call<void>("glDisableVertexAttribArray", I(0));
                        }

                        gl->call<void>("glActiveTexture", U(0x84c0));
                        gl->call<void>("glBindTexture", U(0xde1), unit0);
                        gl->call<void>("glActiveTexture", U(saved_active));
                        gl->call<void>("glUseProgram", U(program));
                        gl->call<void>("glDeleteShader", vertex);
                        gl->call<void>("glDeleteShader", fragment);
                        gl->call<void>("glDeleteProgram", host);
                        gl->call<void>("glDeleteTextures", I(1), &dst);
                        gl->call<void>("glDeleteFramebuffers", I(1), &scratch);
                        gl->call<void>("glDeleteBuffers", I(1), &buffer);
                        drain_gl_errors();
                        logging::write(
                                logging::Level::debug, "graphics", "screen.composite_probe",
                                {{"program", program},
                                 {"matrix0", matrix_json(matrix0)},
                                 {"matrix1", matrix_json(matrix1)},
                                 {"sampler_location", sampler_location},
                                 {"sampler_unit", sampler_unit},
                                 {"bound_texture", bound_texture},
                                 {"min_filter", min_filter},
                                 {"mag_filter", mag_filter},
                                 {"wrap_s", wrap_s},
                                 {"wrap_t", wrap_t},
                                 {"max_level", max_level},
                                 {"status", status},
                                 {"shader_status", Json::array({vertex_status, fragment_status})},
                                 {"link_status", link_status},
                                 {"sampled_hex", sampled[0]},
                                 {"sampled_grid", sampled_grid},
                                 {"sampled_error", sampled_error},
                                 {"attached_level0", attached_level0},
                                 {"gl_error", gl_error},
                                 {"uniforms", uniforms}});
                    }
                }

                // Why a draw into framebuffer 0 leaves it byte-identical to the clear: first the
                // same draw is re-issued with the guest's own program and the two uniforms its
                // fragment depends on forced open (the mask coordinate scale and the colour with
                // its smoothstep range), then, if that is still invisible, with a host program that
                // reads the guest's own position attribute through the guest's own matrix and
                // writes an opaque fragment. The element buffer the draw consumes and every active
                // attribute name are measured first, because a zero index or a misread attribute
                // turns the primitive degenerate and raises no error. State is restored and the
                // window cleared again, so nothing of the probe stays in the frame.
                if (!geometry_replay_done && geometry_replay_skips < 8 &&
                    name == "glDrawElements" && a[1] > 0 && a[1] <= U(1 << 20)) {
                    static const char *const position_names[] = {"vCoord",
                                                                 "vertexCoord",
                                                                 "vertex",
                                                                 "qt_VertexPosition",
                                                                 "vertexCoordsArray",
                                                                 "vertexPosition"};
                    I program = 0;
                    gl->call<void>("glGetIntegerv", U(0x8b8d), &program);
                    Json attributes = Json::array();
                    std::array<char, 128> attribute_name{};
                    I position_location = -1, vec4_location = -1;

                    for (I i = 0; i < 16; i++) {
                        I length = 0, size = 0;
                        U type = 0;
                        gl->call<void>("glGetActiveAttrib", U(program), U(i),
                                       I(attribute_name.size()), &length, &size, &type,
                                       attribute_name.data());

                        if (length <= 0) {
                            break;
                        }

                        const std::string text(attribute_name.data(), std::size_t(length));
                        const I location = gl->call<I>("glGetAttribLocation", U(program),
                                                       text.c_str());
                        attributes.push_back(
                                {{"name", text}, {"size", size}, {"location", location}});

                        for (const char *candidate : position_names) {
                            if (location >= 0 && text == candidate && position_location < 0) {
                                position_location = location;
                            }
                        }

                        if (vec4_location < 0 && size == 4) {
                            vec4_location = location;
                        }
                    }

                    if (position_location < 0) {
                        position_location = vec4_location;
                    }

                    // An enabled attribute with no buffer is a client array: its pointer is a guest
                    // address that means nothing here, and re-issuing the draw with it faults
                    // inside ANGLE's attribute streaming. Such a draw is recorded and left alone,
                    // and the probe waits for a draw whose attributes all live in buffers.
                    Json client_slots = Json::array();

                    for (I slot = 0; slot < 16; slot++) {
                        I enabled = 0, bound = 0;
                        gl->call<void>("glGetVertexAttribiv", slot, U(0x8622), &enabled);

                        if (!enabled) {
                            continue;
                        }

                        // Per-attribute binding (0x889F), not the current GL_ARRAY_BUFFER binding
                        // (0x8894): the latter is zero whenever no buffer happens to be bound right
                        // now, which would make every attribute look like a client array and block
                        // the replay.
                        gl->call<void>("glGetVertexAttribiv", slot, U(0x889f), &bound);

                        if (!bound) {
                            client_slots.push_back(slot);
                        }
                    }

                    const bool replay_allowed = client_slots.empty() && position_location >= 0;

                    if (!replay_allowed) {
                        ++geometry_replay_skips;
                        logging::write(logging::Level::debug, "graphics",
                                       "screen.geometry_replay_skip",
                                       {{"attempt", geometry_replay_skips},
                                        {"program", program},
                                        {"count", U(a[1])},
                                        {"attributes", attributes},
                                        {"position_location", position_location},
                                        {"client_slots", client_slots}});
                    }

                    geometry_replay_done = replay_allowed;
                    const I matrix_location = gl->call<I>("glGetUniformLocation", U(program),
                                                          "matrix");
                    float matrix_values[16] = {0};

                    if (matrix_location >= 0) {
                        gl->call<void>("glGetUniformfv", U(program), matrix_location,
                                       matrix_values);
                    }

                    const I scale_location = gl->call<I>("glGetUniformLocation", U(program),
                                                         "textureScale");
                    const I colour_location = gl->call<I>("glGetUniformLocation", U(program),
                                                          "color");
                    const I alpha_min_location = gl->call<I>("glGetUniformLocation", U(program),
                                                             "alphaMin");
                    const I alpha_max_location = gl->call<I>("glGetUniformLocation", U(program),
                                                             "alphaMax");
                    float saved_scale[2] = {0, 0}, saved_colour[4] = {0, 0, 0, 0},
                          saved_range[2] = {0, 0};

                    if (scale_location >= 0) {
                        gl->call<void>("glGetUniformfv", U(program), scale_location, saved_scale);
                    }

                    if (colour_location >= 0) {
                        gl->call<void>("glGetUniformfv", U(program), colour_location, saved_colour);
                    }

                    if (alpha_min_location >= 0) {
                        gl->call<void>("glGetUniformfv", U(program), alpha_min_location,
                                       &saved_range[0]);
                    }

                    if (alpha_max_location >= 0) {
                        gl->call<void>("glGetUniformfv", U(program), alpha_max_location,
                                       &saved_range[1]);
                    }

                    I sampler_unit = -1, sampler_location = -1;

                    for (const char *candidate :
                         {"_qt_texture", "texture", "qt_Texture", "tex", "_qt_texture0"}) {
                        const I location = gl->call<I>("glGetUniformLocation", U(program),
                                                       candidate);

                        if (location < 0) {
                            continue;
                        }

                        I unit = -1;
                        gl->call<void>("glGetUniformiv", U(program), location, &unit);

                        if (unit >= 0) {
                            sampler_unit = unit;
                            sampler_location = location;
                            break;
                        }
                    }

                    I element_binding = 0, element_size = 0;
                    gl->call<void>("glGetIntegerv", U(0x8895), &element_binding);
                    gl->call<void>("glGetBufferParameteriv", U(0x8893), U(0x8764), &element_size);
                    const U unit = U(a[2]) == U(0x1401)   ? 1
                                   : U(a[2]) == U(0x1403) ? 2
                                   : U(a[2]) == U(0x1405) ? 4
                                                          : 0;
                    const std::uint64_t length = unit ? std::uint64_t(a[1]) * unit : 0;
                    std::size_t index_bytes = 0, index_nonzero = 0;
                    U index_min = 0xffffffffu, index_max = 0;

                    if (element_binding && length &&
                        std::uint64_t(a[3]) <= std::uint64_t(element_size) &&
                        length <= std::uint64_t(element_size) - std::uint64_t(a[3])) {
                        auto pointer = gl->call<V>("glMapBufferRange", U(0x8893), GLintptr(a[3]),
                                                   GLsizeiptr(length), U(1));

                        if (pointer) {
                            const auto *raw = static_cast<const unsigned char *>(pointer);

                            for (std::size_t at = 0; at < std::size_t(length); at++) {
                                if (raw[at] != 0) {
                                    ++index_nonzero;
                                }
                            }

                            for (std::size_t at = 0; at + unit <= std::size_t(length); at += unit) {
                                U value = 0;

                                for (std::size_t k = 0; k < unit; k++) {
                                    value |= U(raw[at + k]) << (8 * k);
                                }

                                if (value < index_min) {
                                    index_min = value;
                                }

                                if (value > index_max) {
                                    index_max = value;
                                }
                            }

                            index_bytes = std::size_t(length);
                            gl->call<GLboolean>("glUnmapBuffer", U(0x8893));
                        }
                    }

                    // What the vertices actually say: the sampled position of a mask draw is its
                    // texture coordinate scaled by textureScale, and a wrong attribute translation
                    // moves the whole grid off the glyphs, which reads exactly like an empty atlas.
                    // Bounded to the vertex range the index buffer uses.
                    Json attribute_ranges = Json::array();
                    I saved_array_binding = 0;
                    gl->call<void>("glGetIntegerv", U(0x8894), &saved_array_binding);

                    if (index_min <= index_max && index_max < 1u << 20) {
                        for (I slot = 0; slot < 16; slot++) {
                            I enabled = 0, bound = 0, size = 0, type = 0, stride = 0;
                            gl->call<void>("glGetVertexAttribiv", slot, U(0x8622), &enabled);

                            if (!enabled) {
                                continue;
                            }

                            gl->call<void>("glGetVertexAttribiv", slot, U(0x889f), &bound);

                            if (!bound) {
                                continue;
                            }

                            gl->call<void>("glGetVertexAttribiv", slot, U(0x8623), &size);
                            gl->call<void>("glGetVertexAttribiv", slot, U(0x8624), &type);
                            gl->call<void>("glGetVertexAttribiv", slot, U(0x8625), &stride);
                            V offset_pointer = nullptr;
                            gl->call<void>("glGetVertexAttribPointerv", slot, U(0x8645),
                                           &offset_pointer);
                            const std::uint64_t offset =
                                    std::uint64_t(reinterpret_cast<std::uintptr_t>(offset_pointer));

                            if (type != 0x1406 || size < 1 || size > 4 || !stride) {
                                continue;
                            }

                            const std::uint64_t bytes = std::uint64_t(index_max) *
                                                                std::uint64_t(stride) +
                                                        std::uint64_t(size) * 4;

                            if (bytes > 4u << 20) {
                                continue;
                            }

                            gl->call<void>("glBindBuffer", U(0x8892), U(bound));
                            I buffer_size = 0;
                            gl->call<void>("glGetBufferParameteriv", U(0x8892), U(0x8764),
                                           &buffer_size);

                            if (std::uint64_t(buffer_size) < bytes) {
                                gl->call<void>("glBindBuffer", U(0x8892), U(saved_array_binding));
                                continue;
                            }

                            auto vertex_pointer = gl->call<V>("glMapBufferRange", U(0x8892),
                                                              GLintptr(0), GLsizeiptr(bytes), U(1));

                            if (vertex_pointer) {
                                const auto *raw =
                                        static_cast<const unsigned char *>(vertex_pointer);
                                Json low = Json::array(), high = Json::array();

                                for (I component = 0; component < size; component++) {
                                    float minimum = 0, maximum = 0;

                                    for (U index = index_min; index <= index_max; index++) {
                                        const auto *base = raw + offset +
                                                           std::uint64_t(index) *
                                                                   std::uint64_t(stride);
                                        float value = 0;
                                        std::memcpy(&value, base + std::uint64_t(component) * 4, 4);

                                        if (index == index_min || value < minimum) {
                                            minimum = value;
                                        }

                                        if (index == index_min || value > maximum) {
                                            maximum = value;
                                        }
                                    }

                                    low.push_back(double(minimum));
                                    high.push_back(double(maximum));
                                }

                                attribute_ranges.push_back({{"location", slot},
                                                            {"size", size},
                                                            {"stride", stride},
                                                            {"offset", double(offset)},
                                                            {"min", low},
                                                            {"max", high}});
                                gl->call<GLboolean>("glUnmapBuffer", U(0x8892));
                            }

                            gl->call<void>("glBindBuffer", U(0x8892), U(saved_array_binding));
                        }
                    }

                    I saved_blend = 0, saved_depth = 0, saved_mask[4] = {0, 0, 0, 0},
                      saved_enabled = 0;
                    gl->call<void>("glGetIntegerv", U(0x0be2), &saved_blend);
                    gl->call<void>("glGetIntegerv", U(0x0b71), &saved_depth);
                    gl->call<void>("glGetIntegerv", U(0x0c23), saved_mask);

                    if (position_location >= 0) {
                        gl->call<void>("glGetVertexAttribiv", position_location, U(0x8622),
                                       &saved_enabled);
                    }

                    // An empty framebuffer first: a nonzero counter cannot tell a re-issue that
                    // produced nothing from one that re-drew the very same pixels, as a depth
                    // re-issue with the depth test off can.
                    float saved_clear[4] = {0, 0, 0, 0};
                    gl->call<void>("glGetFloatv", U(0x0c22), saved_clear);
                    gl->call<void>("glClearColor", 0.0f, 0.0f, 0.0f, 0.0f);
                    gl->call<void>("glClear", U(0x4000));
                    gl->call<void>("glFinish");
                    I viewport[4] = {0, 0, 0, 0};
                    gl->call<void>("glGetIntegerv", U(0x0ba2), viewport);
                    I scissor_test = 0, scissor_box[4] = {0, 0, 0, 0};
                    gl->call<void>("glGetIntegerv", U(0x0c11), &scissor_test);
                    gl->call<void>("glGetIntegerv", U(0x0c10), scissor_box);
                    const auto before = probe_framebuffer(false, true);
                    const auto reissue = [&] {
                        if (!replay_allowed) {
                            return;
                        }

                        gl->call<void>("glDrawElements", U(a[0]), I(a[1]), U(a[2]),
                                       reinterpret_cast<V>(std::uintptr_t(a[3])));
                        gl->call<void>("glFinish");
                    };
                    // What the delivered draw itself leaves behind, with no probe state changed.
                    // 'before' is an empty-buffer baseline -- it is taken after the clear and
                    // before any re-issue -- and the variants A and B below change textureScale,
                    // the colour, the alpha range and the depth test between themselves, so
                    // neither of their numbers says what this draw writes. The two re-issues here
                    // differ only in the depth test; the colour, the alpha range and the scale
                    // stay the guest's own in both.
                    std::size_t nonzero_own_depth = 0, nonzero_own_nodepth = 0;
                    I own_depth_bbox[4] = {-1, -1, -1, -1}, own_nodepth_bbox[4] = {-1, -1, -1, -1};
                    Json samples_own_depth = sample_json(before.sample),
                         samples_own_nodepth = sample_json(before.sample);
                    gl->call<void>("glClear", U(0x4000));
                    gl->call<void>("glFinish");
                    reissue();
                    const auto own_depth = probe_framebuffer(false, true);
                    nonzero_own_depth = own_depth.nonzero_rgb;
                    own_depth_bbox[0] = own_depth.min_x;
                    own_depth_bbox[1] = own_depth.min_y;
                    own_depth_bbox[2] = own_depth.max_x;
                    own_depth_bbox[3] = own_depth.max_y;
                    samples_own_depth = sample_json(own_depth.sample);
                    gl->call<void>("glDisable", U(0x0b71));
                    gl->call<void>("glClear", U(0x4000));
                    gl->call<void>("glFinish");
                    reissue();
                    const auto own_nodepth = probe_framebuffer(false, true);
                    nonzero_own_nodepth = own_nodepth.nonzero_rgb;
                    own_nodepth_bbox[0] = own_nodepth.min_x;
                    own_nodepth_bbox[1] = own_nodepth.min_y;
                    own_nodepth_bbox[2] = own_nodepth.max_x;
                    own_nodepth_bbox[3] = own_nodepth.max_y;
                    samples_own_nodepth = sample_json(own_nodepth.sample);

                    if (scale_location >= 0) {
                        gl->call<void>("glUniform2f", scale_location, 1.0f, 1.0f);
                    }

                    gl->call<void>("glClear", U(0x4000));
                    gl->call<void>("glFinish");
                    reissue();
                    const auto scaled = probe_framebuffer(false, true);
                    Json samples_scaled = sample_json(scaled.sample);
                    std::size_t nonzero_open = 0;
                    Json samples_open = sample_json(scaled.sample);
                    std::size_t nonzero_dim = 0;
                    Json samples_dim = sample_json(scaled.sample);
                    I opened_bbox[4] = {-1, -1, -1, -1}, dim_bbox[4] = {-1, -1, -1, -1};
                    U open_error = gl->call<U>("glGetError");

                    // The scale goes back before the next variant, and that is what makes B
                    // readable: with textureScale the guest's own, an opaque colour and an opened
                    // alpha range the fragment output is the sampled alpha itself, so B renders the
                    // atlas region the draw reads. On the scale A leaves behind, (1,1), B would
                    // sample the atlas edge instead, and its zero would say nothing about the
                    // texture.
                    if (scale_location >= 0) {
                        gl->call<void>("glUniform2f", scale_location, saved_scale[0],
                                       saved_scale[1]);
                    }

                    if (scaled.nonzero_rgb == 0) {
                        if (colour_location >= 0) {
                            gl->call<void>("glUniform4f", colour_location, 1.0f, 1.0f, 1.0f, 1.0f);
                        }

                        if (alpha_min_location >= 0) {
                            gl->call<void>("glUniform1f", alpha_min_location, 0.0f);
                        }

                        if (alpha_max_location >= 0) {
                            gl->call<void>("glUniform1f", alpha_max_location, 1.0f);
                        }

                        reissue();
                        const auto opened = probe_framebuffer(false, true);
                        nonzero_open = opened.nonzero_rgb;
                        samples_open = sample_json(opened.sample);
                        open_error = gl->call<U>("glGetError");
                        opened_bbox[0] = opened.min_x;
                        opened_bbox[1] = opened.min_y;
                        opened_bbox[2] = opened.max_x;
                        opened_bbox[3] = opened.max_y;

                        // The opened range with the guest's own colour, so the colour and the range
                        // are told apart: B moves both at once and this draw's colour is 0.009,
                        // which is about one byte step of white and would fall below the first byte
                        // in a lowp fragment.
                        if (colour_location >= 0 && opened.nonzero_rgb > 0) {
                            gl->call<void>("glUniform4f", colour_location, saved_colour[0],
                                           saved_colour[1], saved_colour[2], saved_colour[3]);
                            gl->call<void>("glClear", U(0x4000));
                            gl->call<void>("glFinish");
                            reissue();
                            const auto dim = probe_framebuffer(false, true);
                            nonzero_dim = dim.nonzero_rgb;
                            dim_bbox[0] = dim.min_x;
                            dim_bbox[1] = dim.min_y;
                            dim_bbox[2] = dim.max_x;
                            dim_bbox[3] = dim.max_y;
                            samples_dim = sample_json(dim.sample);
                            gl->call<void>("glUniform4f", colour_location, 1.0f, 1.0f, 1.0f, 1.0f);
                        }
                    }

                    // Texture and blending are the two stages left between the geometry, which
                    // variant C proves lands, and a fragment that survives. D draws with opaque
                    // colour and no blending at all: content here means the blend function is what
                    // hides the layers. E draws the same and with the sampler's texture replaced by
                    // a white pixel: content here and not in D means the sampled texture is what
                    // contributes nothing.
                    std::size_t nonzero_noblend = 0, nonzero_white = 0;
                    I noblend_bbox[4] = {-1, -1, -1, -1}, white_bbox[4] = {-1, -1, -1, -1};
                    std::size_t atlas_filled = 0;
                    I atlas_bbox[4] = {-1, -1, -1, -1}, atlas_object = 0, atlas_width = 0,
                      atlas_height = 0;
                    bool atlas_fill_done = false;

                    if (scaled.nonzero_rgb == 0 && nonzero_open == 0 && replay_allowed) {
                        gl->call<void>("glClear", U(0x4000));
                        gl->call<void>("glFinish");

                        if (colour_location >= 0) {
                            gl->call<void>("glUniform4f", colour_location, 1.0f, 1.0f, 1.0f, 1.0f);
                        }

                        if (alpha_min_location >= 0) {
                            gl->call<void>("glUniform1f", alpha_min_location, 0.0f);
                        }

                        if (alpha_max_location >= 0) {
                            gl->call<void>("glUniform1f", alpha_max_location, 1.0f);
                        }

                        gl->call<void>("glDisable", U(0x0be2));
                        reissue();
                        const auto noblend = probe_framebuffer(false, true);
                        nonzero_noblend = noblend.nonzero_rgb;
                        noblend_bbox[0] = noblend.min_x;
                        noblend_bbox[1] = noblend.min_y;
                        noblend_bbox[2] = noblend.max_x;
                        noblend_bbox[3] = noblend.max_y;

                        if (nonzero_noblend == 0 && sampler_unit >= 0) {
                            I active = 0, saved_binding = 0;
                            gl->call<void>("glGetIntegerv", U(0x8873), &active);
                            gl->call<void>("glActiveTexture", U(0x84c0 + sampler_unit));
                            gl->call<void>("glGetIntegerv", U(0x8069), &saved_binding);
                            U white = 0;
                            gl->call<void>("glGenTextures", I(1), &white);
                            gl->call<void>("glBindTexture", U(0xde1), white);
                            const std::array<std::byte, 64> opaque = []() {
                                std::array<std::byte, 64> out{};

                                for (auto &b : out) {
                                    b = std::byte{0xff};
                                }

                                return out;
                            }();
                            gl->call<void>("glTexImage2D", U(0xde1), I(0), I(0x1908), I(4), I(4),
                                           I(0), U(0x1908), U(0x1401),
                                           static_cast<const void *>(opaque.data()));
                            gl->call<void>("glTexParameteri", U(0xde1), U(0x2801), I(0x2601));
                            gl->call<void>("glTexParameteri", U(0xde1), U(0x2800), I(0x2601));
                            gl->call<void>("glClear", U(0x4000));
                            gl->call<void>("glFinish");
                            reissue();
                            const auto white_probe = probe_framebuffer(false, true);
                            nonzero_white = white_probe.nonzero_rgb;
                            white_bbox[0] = white_probe.min_x;
                            white_bbox[1] = white_probe.min_y;
                            white_bbox[2] = white_probe.max_x;
                            white_bbox[3] = white_probe.max_y;
                            gl->call<void>("glBindTexture", U(0xde1), U(saved_binding));
                            gl->call<void>("glDeleteTextures", I(1), &white);
                            gl->call<void>("glActiveTexture", U(0x84c0 + active));
                        }

                        // Last variant: the sampler's own texture object is filled with opaque
                        // alpha and the draw is repeated. Content here proves the sampler reads the
                        // object the guest uploads its glyphs into, so a zero before this one is
                        // the atlas content, not the binding; still zero means the sampler reads
                        // something else entirely. The fill overwrites the guest's atlas for the
                        // rest of this diagnostic run, and nothing is restored.
                        I atlas_active = 0;
                        gl->call<void>("glGetIntegerv", U(0x8873), &atlas_active);

                        if (sampler_unit >= 0) {
                            gl->call<void>("glActiveTexture", U(0x84c0 + sampler_unit));
                            I sampler_binding = 0;
                            gl->call<void>("glGetIntegerv", U(0x8069), &sampler_binding);
                            atlas_object = sampler_binding;
                            const auto size = texture_level0_size.find(U(sampler_binding));

                            if (sampler_binding && size != texture_level0_size.end() &&
                                size->second.first > 0 && size->second.second > 0) {
                                atlas_width = size->second.first;
                                atlas_height = size->second.second;
                                const std::size_t pixels = std::size_t(atlas_width) *
                                                           std::size_t(atlas_height);

                                if (pixels <= static_cast<std::size_t>(4u * 1024) * 1024) {
                                    const std::vector<std::byte> opaque(pixels, std::byte{0xff});
                                    I saved_alignment = 0;
                                    gl->call<void>("glGetIntegerv", U(0x0cf5), &saved_alignment);
                                    gl->call<void>("glPixelStorei", U(0x0cf5), I(1));
                                    gl->call<void>("glTexSubImage2D", U(0xde1), I(0), I(0), I(0),
                                                   I(atlas_width), I(atlas_height), U(0x1906),
                                                   U(0x1401),
                                                   static_cast<const void *>(opaque.data()));
                                    gl->call<void>("glPixelStorei", U(0x0cf5), saved_alignment);
                                    const U fill_error = gl->call<U>("glGetError");
                                    gl->call<void>("glClear", U(0x4000));
                                    gl->call<void>("glFinish");
                                    reissue();
                                    const auto filled = probe_framebuffer(false, true);
                                    atlas_filled = filled.nonzero_rgb;
                                    atlas_bbox[0] = filled.min_x;
                                    atlas_bbox[1] = filled.min_y;
                                    atlas_bbox[2] = filled.max_x;
                                    atlas_bbox[3] = filled.max_y;
                                    atlas_fill_done = fill_error == 0;
                                }
                            }

                            gl->call<void>("glActiveTexture", U(0x84c0 + atlas_active));
                        }
                    }

                    if (scale_location >= 0) {
                        gl->call<void>("glUniform2f", scale_location, saved_scale[0],
                                       saved_scale[1]);
                    }

                    if (colour_location >= 0) {
                        gl->call<void>("glUniform4f", colour_location, saved_colour[0],
                                       saved_colour[1], saved_colour[2], saved_colour[3]);
                    }

                    if (alpha_min_location >= 0) {
                        gl->call<void>("glUniform1f", alpha_min_location, saved_range[0]);
                    }

                    if (alpha_max_location >= 0) {
                        gl->call<void>("glUniform1f", alpha_max_location, saved_range[1]);
                    }

                    std::size_t nonzero_host = 0;
                    Json samples_host = sample_json(before.sample);
                    I replay_link = -1;
                    I hosted_bbox[4] = {-1, -1, -1, -1};

                    if (scaled.nonzero_rgb == 0 && nonzero_open == 0 && replay_allowed) {
                        const char *replay_vertex =
                                "attribute vec4 position;uniform mat4 matrix;void main(){gl_Position=matrix*position;}";
                        const char *replay_fragment =
                                "precision mediump float;void main(){gl_FragColor=vec4(1.0,0.0,1.0,1.0);}";
                        U replay_shader = gl->call<U>("glCreateShader", U(0x8b31));
                        U replay_pixel = gl->call<U>("glCreateShader", U(0x8b30));
                        I replay_vertex_size = I(std::strlen(replay_vertex)),
                          replay_fragment_size = I(std::strlen(replay_fragment));
                        gl->call<void>("glShaderSource", replay_shader, I(1), &replay_vertex,
                                       &replay_vertex_size);
                        gl->call<void>("glCompileShader", replay_shader);
                        gl->call<void>("glShaderSource", replay_pixel, I(1), &replay_fragment,
                                       &replay_fragment_size);
                        gl->call<void>("glCompileShader", replay_pixel);
                        U replay = gl->call<U>("glCreateProgram");
                        gl->call<void>("glAttachShader", replay, replay_shader);
                        gl->call<void>("glAttachShader", replay, replay_pixel);
                        gl->call<void>("glBindAttribLocation", replay, position_location,
                                       "position");
                        gl->call<void>("glLinkProgram", replay);
                        gl->call<void>("glGetProgramiv", replay, U(0x8b82), &replay_link);

                        if (replay_link > 0) {
                            gl->call<void>("glUseProgram", replay);
                            gl->call<void>("glUniformMatrix4fv",
                                           gl->call<I>("glGetUniformLocation", replay, "matrix"),
                                           I(1), GLboolean(0), matrix_values);
                            gl->call<void>("glDisable", U(0x0be2));
                            gl->call<void>("glColorMask", GLboolean(1), GLboolean(1), GLboolean(1),
                                           GLboolean(1));
                            gl->call<void>("glEnableVertexAttribArray", position_location);
                            reissue();
                            const auto hosted = probe_framebuffer(false, true);
                            nonzero_host = hosted.nonzero_rgb;
                            samples_host = sample_json(hosted.sample);
                            hosted_bbox[0] = hosted.min_x;
                            hosted_bbox[1] = hosted.min_y;
                            hosted_bbox[2] = hosted.max_x;
                            hosted_bbox[3] = hosted.max_y;
                        }

                        const U host_error = gl->call<U>("glGetError");
                        open_error = open_error ? open_error : host_error;
                        gl->call<void>("glUseProgram", U(program));

                        if (!saved_enabled) {
                            gl->call<void>("glDisableVertexAttribArray", position_location);
                        }

                        gl->call<void>("glDeleteShader", replay_shader);
                        gl->call<void>("glDeleteShader", replay_pixel);
                        gl->call<void>("glDeleteProgram", replay);
                    }

                    gl->call<void>(saved_blend ? "glEnable" : "glDisable", U(0x0be2));
                    gl->call<void>(saved_depth ? "glEnable" : "glDisable", U(0x0b71));
                    gl->call<void>("glColorMask", GLboolean(saved_mask[0]),
                                   GLboolean(saved_mask[1]), GLboolean(saved_mask[2]),
                                   GLboolean(saved_mask[3]));
                    gl->call<void>("glClearColor", saved_clear[0], saved_clear[1], saved_clear[2],
                                   saved_clear[3]);
                    gl->call<void>("glClear", U(0x4000));
                    gl->call<void>("glFinish");
                    drain_gl_errors();
                    logging::write(
                            logging::Level::debug, "graphics", "screen.geometry_replay_probe",
                            {{"program", program},
                             {"attributes", attributes},
                             {"position_location", position_location},
                             {"matrix_location", matrix_location},
                             {"matrix", matrix_json(matrix_values)},
                             {"scale_location", scale_location},
                             {"saved_scale",
                              Json::array({double(saved_scale[0]), double(saved_scale[1])})},
                             {"colour_location", colour_location},
                             {"saved_colour",
                              Json::array({double(saved_colour[0]), double(saved_colour[1]),
                                           double(saved_colour[2]), double(saved_colour[3])})},
                             {"alpha_min_location", alpha_min_location},
                             {"alpha_max_location", alpha_max_location},
                             {"saved_range",
                              Json::array({double(saved_range[0]), double(saved_range[1])})},
                             {"element_binding", element_binding},
                             {"element_size", element_size},
                             {"count", U(a[1])},
                             {"type", U(a[2])},
                             {"offset", U(a[3])},
                             {"index_unit", unit},
                             {"index_bytes", index_bytes},
                             {"index_nonzero", index_nonzero},
                             {"index_min", index_min},
                             {"index_max", index_max},
                             {"attribute_ranges", attribute_ranges},
                             {"nonzero_own_depth", nonzero_own_depth},
                             {"samples_own_depth", samples_own_depth},
                             {"bbox_own_depth",
                              Json::array({own_depth_bbox[0], own_depth_bbox[1], own_depth_bbox[2],
                                           own_depth_bbox[3]})},
                             {"nonzero_own_nodepth", nonzero_own_nodepth},
                             {"samples_own_nodepth", samples_own_nodepth},
                             {"bbox_own_nodepth",
                              Json::array({own_nodepth_bbox[0], own_nodepth_bbox[1],
                                           own_nodepth_bbox[2], own_nodepth_bbox[3]})},
                             {"nonzero_before", before.nonzero_rgb},
                             {"client_slots", client_slots},
                             {"replay_allowed", replay_allowed},
                             {"viewport",
                              Json::array({viewport[0], viewport[1], viewport[2], viewport[3]})},
                             {"scissor_test", scissor_test},
                             {"scissor_box", Json::array({scissor_box[0], scissor_box[1],
                                                          scissor_box[2], scissor_box[3]})},
                             {"bbox_before", Json::array({before.min_x, before.min_y, before.max_x,
                                                          before.max_y})},
                             {"nonzero_scaled", scaled.nonzero_rgb},
                             {"samples_scaled", samples_scaled},
                             {"bbox_scaled", Json::array({scaled.min_x, scaled.min_y, scaled.max_x,
                                                          scaled.max_y})},
                             {"nonzero_opened", nonzero_open},
                             {"samples_opened", samples_open},
                             {"bbox_opened", Json::array({opened_bbox[0], opened_bbox[1],
                                                          opened_bbox[2], opened_bbox[3]})},
                             {"nonzero_dim", nonzero_dim},
                             {"samples_dim", samples_dim},
                             {"bbox_dim",
                              Json::array({dim_bbox[0], dim_bbox[1], dim_bbox[2], dim_bbox[3]})},
                             {"sampler_unit", sampler_unit},
                             {"sampler_location", sampler_location},
                             {"nonzero_noblend", nonzero_noblend},
                             {"bbox_noblend", Json::array({noblend_bbox[0], noblend_bbox[1],
                                                           noblend_bbox[2], noblend_bbox[3]})},
                             {"nonzero_white", nonzero_white},
                             {"bbox_white", Json::array({white_bbox[0], white_bbox[1],
                                                         white_bbox[2], white_bbox[3]})},
                             {"atlas_object", atlas_object},
                             {"atlas_width", atlas_width},
                             {"atlas_height", atlas_height},
                             {"atlas_fill_done", atlas_fill_done},
                             {"nonzero_atlas_filled", atlas_filled},
                             {"bbox_atlas_filled", Json::array({atlas_bbox[0], atlas_bbox[1],
                                                                atlas_bbox[2], atlas_bbox[3]})},
                             {"nonzero_host", nonzero_host},
                             {"samples_host", samples_host},
                             {"bbox_host", Json::array({hosted_bbox[0], hosted_bbox[1],
                                                        hosted_bbox[2], hosted_bbox[3]})},
                             {"link_status", replay_link},
                             {"gl_error", open_error}});
                }

                if (draw_state_events < 16) {
                    Json entry = probe_after_draw(0);
                    entry["event"] = draw_state_events;
                    entry["name"] = std::string(name);
                    entry["count"] = U(a[1]);
                    ++draw_state_events;
                    logging::write(logging::Level::debug, "graphics", "screen.draw_state_probe",
                                   entry);
                }

                return {result, {}, s.name};
            }
        }

        // The geometry the composite draws with: a vertex buffer that arrives empty (or is read
        // from the wrong place) collapses every layer to a point, so the first uploads are counted
        // like textures.
        if (!replaying && (name == "glBufferData" || name == "glBufferSubData") &&
            values[s.pointer] && buffer_probe_events < 24) {
            const auto size = required_input(s, a);
            const auto *bytes = reinterpret_cast<const std::byte *>(values[s.pointer]);
            std::size_t nonzero = 0;

            for (std::size_t i = 0; i < size; i++) {
                if (bytes[i] != std::byte{0}) {
                    ++nonzero;
                }
            }

            // Both calls name their target first; the binding is queried for that target, and
            // targets other than the array and element-array buffers log no binding.
            const auto target = U(a[0]);
            const U query = target == 0x8892 ? 0x8894 : target == 0x8893 ? 0x8895 : 0;
            Json binding = nullptr;

            if (query) {
                I bound = 0;
                gl->call<void>("glGetIntegerv", query, &bound);
                binding = bound;
            }

            logging::write(logging::Level::debug, "graphics", "screen.buffer_probe",
                           {{"event", buffer_probe_events},
                            {"name", std::string(name)},
                            {"target", target},
                            {"binding", binding},
                            {"bytes", size},
                            {"nonzero_bytes", nonzero}});
            ++buffer_probe_events;
        }

        // Which texture object a draw's sampler really points at: the guest binds through the same
        // entry point for every target, so the target has to be recorded to know what to look up.
        if (!replaying && name == "glBindTexture" && bind_probe_events < 64) {
            // Only pairs never seen before: the interesting part is the full set of texture objects
            // the guest touches and the target each one belongs to, not the stream of rebinds.
            if (bound_pairs.insert({U(a[0]), U(a[1])}).second) {
                logging::write(
                        logging::Level::debug, "graphics", "screen.bind_probe",
                        {{"event", bind_probe_events}, {"target", U(a[0])}, {"texture", U(a[1])}});
                ++bind_probe_events;
            }
        }

        // Which object each texture upload really lands in: an upload into a
        // created-but-never-defined object, or one into an object nobody binds afterwards, both
        // look like a missing texture later.
        if (!replaying && name == "glTexImage2D" && !a[1] && a[3] && a[4]) {
            I object = 0;
            gl->call<void>("glGetIntegerv", U(0x8069), &object);

            if (object && texture_level0_size.size() < 64) {
                texture_level0_size[U(object)] = {I(a[3]), I(a[4])};
            }
        }

        if (!replaying && (name == "glTexImage2D" || name == "glTexSubImage2D") &&
            teximage_probe_events < 400) {
            I object = 0;
            gl->call<void>("glGetIntegerv", U(0x8069), &object);
            I unpack = 0;
            gl->call<void>("glGetIntegerv", U(0x88ef), &unpack);
            const bool sub = name == "glTexSubImage2D";

            if (sub && !teximage_sub_objects.insert(object).second) {
                return {generated->invoke(op, values), {}, s.name};
            }

            // The upload is invoked here so that its own verdict is visible: the level-0 size of
            // the object right after the call tells whether the texture the compositor binds later
            // really has an image, and the error code tells whether ANGLE accepted the call at all.
            // The queue is emptied first: GL keeps errors queued, so an error raised long before
            // this call would otherwise be read as this call's own verdict.
            drain_gl_errors();
            const auto result = generated->invoke(op, values);
            // The upload's own verdict, taken before any probe call can contribute an error of its
            // own.
            const U error_after_call = gl->call<U>("glGetError");
            I bound_after = 0;
            gl->call<void>("glGetIntegerv", U(0x8069), &bound_after);
            I level_width = 0, level_height = 0;
            gl->call<void>("glGetTexLevelParameteriv", U(0xde1), I(0), U(0x1000), &level_width);
            gl->call<void>("glGetTexLevelParameteriv", U(0xde1), I(0), U(0x1001), &level_height);
            const U error_after_size = gl->call<U>("glGetError");
            drain_gl_errors();
            logging::write(logging::Level::debug, "graphics", "screen.teximage_probe",
                           {{"event", teximage_probe_events},
                            {"name", std::string(name)},
                            {"target", U(a[0])},
                            {"level", U(a[1])},
                            {"object", object},
                            {"width", U(a[sub ? 4 : 3])},
                            {"height", U(a[sub ? 5 : 4])},
                            {"inline", !a[s.pointer]},
                            {"unpack_buffer", unpack},
                            {"internalformat", U(a[2])},
                            {"border", U(a[5])},
                            {"format", U(a[6])},
                            {"type", U(a[7])},
                            {"bound_after_call", bound_after},
                            {"level0_width", level_width},
                            {"level0_height", level_height},
                            {"gl_error_after_call", error_after_call},
                            {"gl_error_after_size", error_after_size}});
            ++teximage_probe_events;

            return {result, {}, s.name};
        }

        return {generated->invoke(op, values), {}, s.name};
    }

    Reply special_call(U op, const Arguments &a, std::span<const std::byte> input) {
        need(input.empty(), "Special call payload");
        Reply out{};

        if (op == 300 || op == 301) {
            auto cap = a[2];
            need(cap <= 3827, "Active-name extent");
            std::vector<char> text(std::max<std::uint64_t>(1, cap));
            I length = 0, size = 0;
            U kind = 0;
            const auto name = op == 300 ? "glGetActiveAttrib" : "glGetActiveUniform";
            gl->call<void>(name, U(a[0]), U(a[1]), I(cap), &length, &size, &kind, text.data());
            need(length >= 0 && std::uint64_t(length) < std::max<std::uint64_t>(1, cap),
                 "Active-name reply extent");
            append(out.payload, length);
            append(out.payload, size);
            append(out.payload, kind);

            if (cap) {
                auto at = out.payload.size();
                out.payload.resize(at + std::min<std::uint64_t>(cap, std::uint64_t(length) + 1));
                std::memcpy(out.payload.data() + at, text.data(), out.payload.size() - at);
            }

            out.name = name;

            return out;
        }

        if (op == 302 || op == 303) {
            auto cap = a[1];
            auto b = blob(a[2]);
            need(cap <= max_blob - 4 && b && b->direction == 1 && b->size >= cap + 4,
                 "Info-log output extent");
            std::vector<char> text(std::max<std::uint64_t>(1, cap));
            I length = 0;
            const auto name = op == 302 ? "glGetProgramInfoLog" : "glGetShaderInfoLog";
            gl->call<void>(name, U(a[0]), I(cap), &length, text.data());
            need(length >= 0 && std::uint64_t(length) < std::max<std::uint64_t>(1, cap),
                 "Info-log reply extent");
            append(out.payload, length);

            if (cap) {
                auto at = out.payload.size();
                out.payload.resize(at + std::min<std::uint64_t>(cap, std::uint64_t(length) + 1));
                std::memcpy(out.payload.data() + at, text.data(), out.payload.size() - at);
            }

            write_blob(a[2], out.payload);

            return {out.payload.size(), {}, name};
        }

        if (op == 304) {
            auto cap = a[1];
            auto b = blob(a[2]);
            need(cap <= max_blob - 8 && b && b->direction == 1 && b->size >= cap + 8,
                 "Program-binary output extent");
            std::vector<std::byte> binary(std::max<std::uint64_t>(1, cap));
            I length = 0;
            U kind = 0;
            gl->call<void>("glGetProgramBinary", U(a[0]), I(cap), &length, &kind, binary.data());
            need(length >= 0 && std::uint64_t(length) <= cap, "Program-binary reply extent");
            append(out.payload, length);
            append(out.payload, kind);
            out.payload.insert(out.payload.end(), binary.begin(), binary.begin() + length);
            write_blob(a[2], out.payload);

            return {out.payload.size(), {}, "glGetProgramBinary"};
        }

        need(op == 305, "Special opcode");
        auto objects = blob(a[1]), code = blob(a[3]);
        need(objects && code && objects->direction == 0 && code->direction == 0 &&
                     a[0] <= max_blob / 4 && objects->size >= a[0] * 4 && a[4] <= code->size,
             "Shader-binary extent");
        gl->call<void>("glShaderBinary", I(a[0]),
                       reinterpret_cast<const U *>(objects->bytes.data()), U(a[2]),
                       static_cast<const V>(code->bytes.data()), I(a[4]));

        return {0, {}, "glShaderBinary"};
    }

    Reply client_call(U op, const Arguments &a, std::span<const std::byte> data) {
        auto context = egl->call<V>("eglGetCurrentContext");
        need(context != nullptr, "Client array context ownership");
        auto context_id = reinterpret_cast<std::uintptr_t>(context);

        if (op == 350) {
            Blob temporary{data.size(),
                           0,
                           data.size(),
                           {},
                           std::vector<std::byte>(data.begin(), data.end())};
            Blob *b = nullptr;

            if (!data.empty()) {
                validate_inline_client(a, data);
                b = &temporary;
            } else {
                b = blob(a[5]);
            }

            const std::set<std::uint64_t> types{0x1400, 0x1401, 0x1402, 0x1403, 0x1404,
                                                0x1405, 0x1406, 0x140b, 0x140c};
            need(a[0] < 64 && a[1] >= 1 && a[1] <= 4 && types.contains(a[2]) && a[3] <= 255 &&
                         a[4] <= max_blob && b && b->direction == 0,
                 "Client attribute extent");
            I binding = 0;
            gl->call<void>("glGetIntegerv", U(0x8894), &binding);
            auto &buffer = client_buffers[{context_id, U(a[0])}];

            if (!buffer) {
                gl->call<void>("glGenBuffers", I(1), &buffer);
            }

            gl->call<void>("glBindBuffer", U(0x8892), buffer);
            gl->call<void>("glBufferData", U(0x8892), GLsizeiptr(b->size),
                           static_cast<const V>(b->bytes.data()), U(0x88e0));
            gl->call<void>("glVertexAttribPointer", U(a[0]), I(a[1]), U(a[2]), GLboolean(a[3]),
                           I(a[4]), static_cast<const V>(nullptr));
            gl->call<void>("glBindBuffer", U(0x8892), U(binding));

            return {1, {}, data.empty() ? "client-attribute-upload" : "client-attribute-inline"};
        }

        need(data.empty(), "Client index payload");
        const auto kind = op == 351 ? a[2] : a[1];
        std::size_t unit = kind == 0x1401 ? 1 : kind == 0x1403 ? 2 : kind == 0x1405 ? 4 : 0;
        need(unit, "Client index type");

        if (op == 351) {
            auto b = blob(a[3]);
            need(a[1] <= max_blob / unit && b && b->direction == 0 && a[1] * unit <= b->size,
                 "Client index extent");
            I binding = 0;
            gl->call<void>("glGetIntegerv", U(0x8895), &binding);
            auto &buffer = client_index_buffers[context_id];

            if (!buffer) {
                gl->call<void>("glGenBuffers", I(1), &buffer);
            }

            gl->call<void>("glBindBuffer", U(0x8893), buffer);
            gl->call<void>("glBufferData", U(0x8893), GLsizeiptr(b->size),
                           static_cast<const V>(b->bytes.data()), U(0x88e0));
            gl->call<void>("glDrawElements", U(a[0]), I(a[1]), U(kind),
                           static_cast<const V>(nullptr));
            gl->call<void>("glBindBuffer", U(0x8893), U(binding));

            return {1, {}, "client-index-draw"};
        }

        auto length = count_bytes(a[0], unit);
        I size = 0;
        gl->call<void>("glGetBufferParameteriv", U(0x8893), U(0x8764), &size);

        if (!length || size < 0 || a[2] > std::uint64_t(size) ||
            length > std::uint64_t(size) - a[2]) {
            throw VoyemuError("GRAPHICS",
                              "Element-buffer draw extent: indices=" + std::to_string(a[0]) +
                                      " type=" + std::to_string(kind) +
                                      " offset=" + std::to_string(a[2]) +
                                      " buffer_bytes=" + std::to_string(size));
        }

        auto pointer = gl->call<V>("glMapBufferRange", U(0x8893), GLintptr(a[2]),
                                   GLsizeiptr(length), U(1));
        need(pointer != nullptr, "Element-buffer map failed");
        std::uint64_t maximum = 0;
        const auto *raw = static_cast<const unsigned char *>(pointer);

        for (std::size_t at = 0; at < length; at += unit) {
            U value = 0;

            for (std::size_t byte = 0; byte < unit; byte++) {
                value |= U(raw[at + byte]) << (8 * byte);
            }

            maximum = std::max<std::uint64_t>(maximum, value);
        }

        need(gl->call<GLboolean>("glUnmapBuffer", U(0x8893)) != 0, "Element-buffer unmap failed");

        return {maximum + 1, {}, "client-index-range"};
    }

    Reply observers(U op, const Arguments &a, std::span<const std::byte> data) {
        Json record{{"opcode", op},
                    {"producer_address", options.address},
                    {"host_monotonic_ns", now_ns()}};

        if (op == 356) {
            validate_mcd_batch(a[0], a[1], data);
            record["count"] = a[0];
            record["dropped"] = a[1];
        }

        if (op == 357) {
            need(a[0] < a[1] && a[1] <= 32 * 1024 * 1024 && a[1] - a[0] == data.size() &&
                         data.size() <= 3072,
                 "DLT tail geometry");
            record["begin"] = a[0];
            record["end"] = a[1];
        }

        if (op == 359) {
            need(data.empty() && a[0] <= 6 && a[1] < 0x100000000ULL && a[2] > 0 &&
                         a[2] < 0x100000000ULL && a[3] < 3000,
                 "Manager metadata geometry");
            record["state"] = a[0];
            record["data_ready_mask"] = a[1];
            record["pid"] = a[2];
            record["poll"] = a[3];
        }

        if (!data.empty()) {
            constexpr char hex[] = "0123456789abcdef";
            std::string encoded;

            for (auto b : data) {
                auto v = std::to_integer<unsigned>(b);
                encoded += hex[v >> 4];
                encoded += hex[v & 15];
            }

            record["payload_hex"] = encoded;
        }

        auto path = producer / "oem-observations.jsonl";
        need(!std::filesystem::is_symlink(path), "Observer file ownership");

        if (!std::filesystem::exists(path) || std::filesystem::file_size(path) < 8 * 1024 * 1024) {
            std::ofstream output(path, std::ios::app);
            need(bool(output), "Observer append");
            output << record.dump() << '\n';
            need(bool(output), "Observer write");
        }

        return {1,
                {},
                op == 356   ? "oem-Mcd-trace"
                : op == 357 ? "oem-DLT-tail"
                            : "oem-manager-state"};
    }

    Reply owner_liveness(U op, const Arguments &a, std::span<const std::byte> data) {
        need(!replaying && canonical_screen && live_exported && options.generation &&
                     options.worker_nonce && !options.session_id.empty(),
             "Owner liveness observer identity");
        const auto claim = read_json(options.session / "frame-owner.json", 512);
        need(claim.at("schema") == 1 && claim.at("producer_address") == options.address &&
                     claim.at("source") == "original-OEM-Screen-BMP",
             "Owner liveness Screen claim");
        need(data.empty(), "Owner liveness payload");
        const auto path = options.session / "active-owners.json";

        if (!std::filesystem::exists(path)) {
            return {0, {}, "owner-liveness-not-ready"};
        }

        const auto census = read_json(path, rpc_payload_bytes);
        need(census.is_object() && census.at("schema") == 1 &&
                     census.at("session_id") == options.session_id &&
                     census.at("generation") == options.generation,
             "Owner liveness session");
        auto positive = [](const Json &value) {
            need(value.is_number_unsigned() && value.get<std::uint64_t>() > 0,
                 "Owner liveness unsigned identity");

            return value.get<std::uint64_t>();
        };
        const auto challenge = positive(census.at("challenge"));
        const auto &owners = census.at("owners");
        need(owners.is_array() && !owners.empty() && owners.size() <= 8,
             "Owner liveness census extent");
        const auto worker_pid = os::current_process();
        bool observer = false;
        std::set<std::uint64_t> nonces, addresses;
        Json candidates = Json::array();

        for (const auto &owner : owners) {
            for (const char *field : {"nonce", "producer_address", "worker_pid"}) {
                positive(owner.at(field));
            }

            need(owner.at("guest_pid").is_number_unsigned() &&
                         owner.at("producer_incarnation").is_number_unsigned(),
                 "Owner liveness guest identity type");
            const auto pid = owner.at("guest_pid").get<std::uint64_t>(),
                       incarnation = owner.at("producer_incarnation").get<std::uint64_t>();
            need(bool(pid) == bool(incarnation), "Owner liveness partial guest identity");

            if (pid && incarnation) {
                candidates.push_back(owner);
            }

            need(nonces.insert(owner.at("nonce").get<std::uint64_t>()).second &&
                         addresses.insert(owner.at("producer_address").get<std::uint64_t>()).second,
                 "Owner liveness duplicate identity");

            if (owner.at("nonce") == options.worker_nonce &&
                owner.at("producer_address") == options.address &&
                owner.at("worker_pid") == worker_pid &&
                owner.at("guest_pid") == options.guest_pid &&
                owner.at("producer_incarnation") == options.producer_incarnation) {
                observer = true;
            }
        }

        need(observer, "Owner liveness unregistered observer");

        if (op == owner_liveness_request_opcode) {
            need(std::all_of(a.begin(), a.end(),
                             [](auto value) {
                                 return !value;
                             }),
                 "Owner liveness request arguments");

            if (challenge == consumed_owner_challenge || candidates.empty()) {
                return {0, {}, "owner-liveness-consumed"};
            }

            Reply out{1, {}, "owner-liveness-census"};
            append(out.payload, std::uint64_t(1));
            append(out.payload, options.generation);
            append(out.payload, challenge);
            append(out.payload, std::uint64_t(candidates.size()));

            for (const auto &owner : candidates) {
                for (const char *field : {"nonce", "producer_address", "worker_pid", "guest_pid",
                                          "producer_incarnation"}) {
                    append(out.payload, owner.at(field).get<std::uint64_t>());
                }
            }

            offered_owner_challenge = challenge;
            offered_owners = owners;

            return out;
        }

        need(a[0] == options.generation && a[7] == UINT64_MAX && a[8] == 3 &&
                     std::all_of(a.begin() + 9, a.end(),
                                 [](auto value) {
                                     return !value;
                                 }),
             "Owner liveness terminal result");

        if (a[1] != challenge) {
            return {0, {}, "owner-liveness-stale"};
        }

        need(a[1] == offered_owner_challenge && a[1] != consumed_owner_challenge &&
                     offered_owners == owners,
             "Owner liveness unissued or replayed challenge");
        const Json target{{"nonce", a[2]},
                          {"producer_address", a[3]},
                          {"worker_pid", a[4]},
                          {"guest_pid", a[5]},
                          {"producer_incarnation", a[6]}};
        need(a[5] && a[6] &&
                     std::find(candidates.begin(), candidates.end(), target) != candidates.end() &&
                     a[2] != options.worker_nonce && a[5] != options.guest_pid,
             "Owner liveness target identity");
        const auto receipt_path = options.session /
                                  ("owner-death-" + std::to_string(a[2]) + ".json");

        if (std::filesystem::exists(receipt_path)) {
            const auto previous = read_json(receipt_path, 2048);

            if (previous.at("challenge") != challenge) {
                return {0, {}, "owner-liveness-stale-receipt"};
            }

            throw VoyemuError("GRAPHICS", "Owner liveness duplicate receipt");
        }

        Json receipt = target;
        receipt.update({{"schema", 1},
                        {"session_id", options.session_id},
                        {"generation", options.generation},
                        {"challenge", challenge},
                        {"result", -1},
                        {"errno", 3},
                        {"observer_producer_address", options.address},
                        {"observer_worker_pid", worker_pid},
                        {"observer_guest_pid", options.guest_pid},
                        {"observer_producer_incarnation", options.producer_incarnation},
                        {"observer_nonce", options.worker_nonce}});
        replace_json(receipt_path, receipt, options.address);
        consumed_owner_challenge = challenge;

        return {1, {}, "owner-liveness-terminal"};
    }

    Reply dispatch(U op, const Arguments &a, std::span<const std::byte> data) {
        need(data.size() <= rpc_payload_bytes, "RPC payload extent");
        op_counts[op]++;

        if (op == owner_liveness_request_opcode || op == owner_liveness_report_opcode) {
            return owner_liveness(op, a, data);
        }

        if (op >= 362 && op <= 364) {
            if (op == 362) { // Compatibility projection; never steals the Gauge owner's claim.
                if (!control_claimed) {
                    return {0, {}, "legacy-MCU-unclaimed"};
                }

                auto full = controls_reply(364, a, data);

                if (!full.result) {
                    return full;
                }

                auto state = read_json((std::filesystem::exists(options.session / "mcu-state.json")
                                                ? options.session / "mcu-state.json"
                                                : options.session / "vehicle-state.json"),
                                       128 * 1024);
                const auto &raw = state.at("raw_oem");
                full.payload.clear();

                for (const char *name : {"speed", "soc", "door_fl", "turn_left", "turn_right"}) {
                    append(full.payload, raw.at(name).get<U>());
                }

                full.name = "legacy-MCU-projection";

                return full;
            }

            return controls_reply(op, a, data);
        }

        if (op == 355) {
            return controls_reply(op, a, data);
        }

        if (op == 365 || op == 366) {
            return controls_reply(op, a, data);
        }

        if (op == 356 || op == 357 || op == 359) {
            return observers(op, a, data);
        }

        if (op == 360) {
            auto records = validate_batch(a[0], data);

            for (const auto &r : records) {
                auto reply = dispatch(r.opcode, r.arguments, r.payload);
                need(reply.payload.empty(), "GL batch unexpected output");
            }

            return {records.size(), {}, "gl-void-batch"};
        }

        if (op == 358) {
            need(data.empty() && a[1] == uniform_schema && a[0] < 0x100000000ULL,
                 "Uniform prefetch schema/arguments");
            I linked = 0;
            gl->call<void>("glGetProgramiv", U(a[0]), U(0x8b82), &linked);
            need(linked == 0 || linked == 1, "Original link status value");
            Reply out{0, {}, "linked-uniform-prefetch"};
            append(out.payload, linked);
            append(out.payload, U(linked ? std::size(uniform_names) : 0));
            append(out.payload, uniform_schema);

            if (linked) {
                for (auto name : uniform_names) {
                    append(out.payload, gl->call<I>("glGetUniformLocation", U(a[0]), name));
                }
            }

            need(out.payload.size() <= rpc_payload_bytes, "Uniform prefetch output extent");

            return out;
        }

        if (op == 40) {
            need(data.empty() && a[0] > 0 && a[0] <= max_blob && a[1] <= 1 && blobs.size() < 256 &&
                         allocated + a[0] <= 256 * 1024 * 1024,
                 "Bulk geometry/producer budget");
            auto key = next_blob++;
            Blob b{a[0], a[1], 0, {}, std::vector<std::byte>(a[0])};
            allocated += a[0];
            blobs.emplace(key, std::move(b));

            return {key, {}, "bulk-begin"};
        }

        if (op == 41) {
            auto it = blobs.find(a[0]);
            need(it != blobs.end(), "Unknown bulk handle");
            auto &b = it->second;
            need(a[1] > 0 && a[1] <= 240 && data.size() == a[1] * 16 &&
                         b.runs.size() + a[1] <= 65536,
                 "Bulk descriptors");
            std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
            auto at = b.position;

            for (std::size_t i = 0; i < a[1]; i++) {
                auto physical = u64(data, i * 16), size = u64(data, i * 16 + 8);
                validate_ram_extent(physical, size, options.ram_extent);
                need(size <= b.size - at, "Bulk physical extent");
                at += size;
                runs.emplace_back(physical, size);
            }

            for (auto [physical, size] : runs) {
                if (b.direction == 0) {
                    read_input(b.bytes.data() + b.position, physical, size);
                }

                b.runs.emplace_back(physical, size);
                b.position += size;
            }

            return {1, {}, "bulk-runs"};
        }

        if (op == 42) {
            need(data.empty(), "Bulk release payload");

            if (auto it = blobs.find(a[0]); it != blobs.end()) {
                allocated -= it->second.size;
                blobs.erase(it);
            }

            return {1, {}, "bulk-release"};
        }

        if (op == 43) {
            need(data.empty(), "Texture refresh payload");
            auto b = blob(a[0], true);
            need(b && b->direction == 0 && a[1] > 0 && a[1] <= b->size,
                 "Texture refresh extent/direction");
            std::vector<std::span<std::byte>> sources;
            std::size_t remaining = a[1];

            for (auto [physical, size] : b->runs) {
                if (!remaining) {
                    break;
                }

                auto count = std::min<std::uint64_t>(size, remaining);
                sources.push_back(ram.extent(physical, count));
                remaining -= count;
            }

            need(!remaining, "Texture refresh registration extent");
            std::size_t at = 0;

            for (std::size_t i = 0; i < sources.size(); ++i) {
                auto src = sources[i];
                read_input(b->bytes.data() + at, b->runs[i].first, src.size());
                at += src.size();
            }

            b->valid_bytes = a[1];
            b->texture_only = true;

            return {1, {}, "texture-refresh"};
        }

        if (op == 354) {
            need(data.empty(), "Screen artifact payload");
            auto b = blob(a[0]);
            need(b && b->direction == 0 && a[1] == b->size && b->size >= 54 &&
                         b->size <= frames::pixel_bytes + 4096,
                 "Screen artifact ownership");

            if (replaying) {
                if (a[2]) {
                    need(a[2] == screen_sequence + 1, "Replay Screen sequence");
                    screen_sequence = a[2];
                }

                ++exported;

                return {1, {}, "original-OEM-Screen-frame"};
            }

            if (a[2]) {
                need(a[2] == screen_sequence + 1 && a[2] < 0x100000000ULL,
                     "Screen stream sequence");
                std::size_t at = 0;

                for (auto [physical, size] : b->runs) {
                    auto src = ram.extent(physical, size);
                    std::memcpy(b->bytes.data() + at, src.data(), size);
                    at += size;
                }

                need(at == b->size, "Screen stream extent");
            }

            auto pixels = frames::decode_screen_bmp(b->bytes);

            // A published frame was measured with correct geometry, a growing sequence and
            // all-zero pixels, while the guest kept returning result=1 with no fault. The
            // sample tells apart a guest that draws black from a host that publishes an
            // unread or misparsed buffer.
            if (live_exported < 3 || live_exported % 500 == 0) {
                std::size_t raw_nz = 0, pixel_nz = 0;

                for (std::size_t i = 0; i < b->bytes.size(); i += 997) {
                    if (b->bytes[i] != std::byte{0}) {
                        ++raw_nz;
                    }
                }

                for (std::size_t i = 0; i < pixels.size(); i += 997) {
                    if (pixels[i] != std::byte{0}) {
                        ++pixel_nz;
                    }
                }

                logging::write(logging::Level::debug, "graphics", "screen.sample",
                               {{"stream_sequence", a[2]},
                                {"blob_bytes", b->size},
                                {"runs", b->runs.size()},
                                {"raw_nonzero_samples", raw_nz},
                                {"pixel_nonzero_samples", pixel_nz},
                                {"exported", exported}});
            }

            if (!frame_writer) {
                canonical_screen = claim(options.session / "frame-owner.json", options.address,
                                         "original-OEM-Screen-BMP");
                auto path = canonical_screen
                                    ? (options.frame.empty() ? options.session / "frame.bgra"
                                                             : options.frame)
                                    : producer / "frame.bgra";
                frame_writer = std::make_unique<frames::Writer>(path);
            }

            frame_writer->publish(pixels, ++exported, now_ns());
            ++live_exported;

            if (a[2]) {
                screen_sequence = a[2];
            }

            return {1, {}, "original-OEM-Screen-frame"};
        }

        if (op == 353) {
            need(data.empty(), "Shader bulk payload");
            auto b = blob(a[1]);
            need(b && b->direction == 0 && a[2] == b->size, "Shader bulk extent");
            auto text = reinterpret_cast<const char *>(b->bytes.data());
            I size = I(b->size);
            gl->call<void>("glShaderSource", U(a[0]), I(1), &text, &size);

            return {0, {}, "glShaderSource-bulk"};
        }

        if (op >= 350 && op <= 352) {
            return client_call(op, a, data);
        }

        if (op >= 300 && op <= 305) {
            return special_call(op, a, data);
        }

        if (op >= 100) {
            return generated_call(op, a, data);
        }

        Reply out{};
        auto attrs = attributes((op == 6 || op == 7 || op == 8) ? data
                                                                : std::span<const std::byte>{});
        auto pointer = [&](unsigned i) {
            auto kind = i == 0 ? EglKind::display
                        : (op == 6 || op == 7 || op == 8 || op == 61)
                                ? (i == 2 ? EglKind::context : EglKind::config)
                                : (op == 9 ? (i == 3 ? EglKind::context : EglKind::surface)
                                           : (op == 10 || op == 65 ? EglKind::context
                                                                   : EglKind::surface));

            return native_handle(kind, a[i]);
        };

        if (op != 6 && op != 7 && op != 8 && op != 26) {
            need(data.empty(), "Core scalar payload");
        }

        switch (op) {
        case 1:
            need(a[0] == 0, "EGL native display input");
            out = {expose_handle(EglKind::display, egl->call<V>("eglGetDisplay", V{})),
                   {},
                   "eglGetDisplay"};

            if (out.result) {
                displays.insert(out.result);
            }

            break;
        case 2: {
            I major = 0, minor = 0;
            out = {egl->call<U>("eglInitialize", pointer(0), &major, &minor), {}, "eglInitialize"};
            append(out.payload, major);
            append(out.payload, minor);
            break;
        }
        case 3:
            out = {encode_result(egl->call<I>("eglGetError")), {}, "eglGetError"};
            break;
        case 4:
            out = {egl->call<U>("eglTerminate", pointer(0)), {}, "eglTerminate"};

            if (out.result) {
                displays.erase(a[0]);
            }

            break;
        case 5:
            out = {egl->call<U>("eglBindAPI", U(a[0])), {}, "eglBindAPI"};
            break;
        case 6:
        case 60: {
            need(a[1] <= 256, "EGL config capacity");
            std::array<V, 256> configs{};
            I count = 0;
            U result = op == 6 ? egl->call<U>("eglChooseConfig", pointer(0),
                                              attrs.empty() ? static_cast<I *>(nullptr)
                                                            : attrs.data(),
                                              a[1] ? configs.data() : static_cast<V *>(nullptr),
                                              I(a[1]), &count)
                               : egl->call<U>("eglGetConfigs", pointer(0),
                                              a[1] ? configs.data() : static_cast<V *>(nullptr),
                                              I(a[1]), &count);
            need(count >= 0, "EGL config count");
            out = {result, {}, op == 6 ? "eglChooseConfig" : "eglGetConfigs"};
            append(out.payload, count);

            for (std::uint64_t i = 0; i < std::min<std::uint64_t>(a[1], count); i++) {
                append(out.payload, expose_handle(EglKind::config, configs[i]));
            }

            break;
        }
        case 7:
            out = {expose_handle(
                           EglKind::context,
                           egl->call<V>("eglCreateContext", pointer(0), pointer(1), pointer(2),
                                        attrs.empty() ? static_cast<I *>(nullptr) : attrs.data())),
                   {},
                   "eglCreateContext"};

            if (out.result) {
                contexts.emplace(a[0], out.result);
            }

            break;
        case 8:
            out = {expose_handle(
                           EglKind::surface,
                           egl->call<V>("eglCreatePbufferSurface", pointer(0), pointer(1),
                                        attrs.empty() ? static_cast<I *>(nullptr) : attrs.data())),
                   {},
                   "eglCreatePbufferSurface"};

            if (out.result) {
                surfaces.emplace(a[0], out.result);
            }

            break;
        case 9: {
            out = {egl->call<U>("eglMakeCurrent", pointer(0), pointer(1), pointer(2), pointer(3)),
                   {},
                   "eglMakeCurrent"};

            if (out.result && a[3] && !renderer_logged) {
                auto text = [&](U key) {
                    const auto *value = gl->call<const unsigned char *>("glGetString", key);

                    if (!value) {
                        return std::string{};
                    }

                    std::size_t size = 0;

                    while (size < 512 && value[size]) {
                        ++size;
                    }

                    return std::string(reinterpret_cast<const char *>(value), size);
                };
                logging::write(logging::Level::info, "graphics", "renderer.selected",
                               {{"producer_address", options.address},
                                {"vendor", text(0x1f00)},
                                {"renderer", text(0x1f01)},
                                {"version", text(0x1f02)}});
                renderer_logged = true;
            }

            break;
        }
        case 10:
        case 11:
            out = {egl->call<U>(op == 10 ? "eglDestroyContext" : "eglDestroySurface", pointer(0),
                                pointer(1)),
                   {},
                   op == 10 ? "eglDestroyContext" : "eglDestroySurface"};

            if (out.result) {
                (op == 10 ? contexts : surfaces).erase({a[0], a[1]});
                egl_handles.release(op == 10 ? EglKind::context : EglKind::surface, a[1]);
            }

            break;
        case 12:
        case 21: {
            const char *text =
                    op == 12 ? egl->call<const char *>("eglQueryString", pointer(0), I(a[1]))
                             : reinterpret_cast<const char *>(
                                       gl->call<const unsigned char *>("glGetString", U(a[0])));
            out = {text ? 1ULL : 0ULL, {}, op == 12 ? "eglQueryString" : "glGetString"};

            if (text) {
                const auto target = a[op == 12 ? 2 : 1];
                const auto limit = target ? std::size_t(65536) : std::size_t(rpc_payload_bytes);
                std::size_t n = 0;

                while (n < limit && text[n]) {
                    n++;
                }

                need(n < limit, "GL/EGL string output extent");

                if (target) {
                    write_blob(target, {reinterpret_cast<const std::byte *>(text), n + 1});
                    out.result = n + 1;
                } else {
                    out.payload = bytes_of(std::string_view(text, n));
                }
            }

            break;
        }
        case 20:
            out = {gl->call<U>("glGetError"), {}, "glGetError"};
            break;
        case 22:
            clear_bits[0] = U(a[0]);
            clear_bits[1] = U(a[1]);
            clear_bits[2] = U(a[2]);
            clear_bits[3] = U(a[3]);
            gl->call<void>("glClearColor", float_bits(a[0]), float_bits(a[1]), float_bits(a[2]),
                           float_bits(a[3]));
            out.name = "glClearColor";
            break;
        case 23: {
            gl->call<void>("glClear", U(a[0]));
            out.name = "glClear";

            // Which framebuffer the compositor's clear targets, and with which colour.
            if (!replaying) {
                auto p = probe_framebuffer(false, false);
                logging::write(logging::Level::debug, "graphics", "screen.clear_probe",
                               {{"event", clear_events},
                                {"mask", U(a[0])},
                                {"draw_fb", p.draw_fb},
                                {"read_fb", p.read_fb},
                                {"draw_buffer", p.draw_buffer},
                                {"read_buffer", p.read_buffer},
                                {"clear_rgba_bits", Json::array({clear_bits[0], clear_bits[1],
                                                                 clear_bits[2], clear_bits[3]})}});
                ++clear_events;
            }

            break;
        }
        case 24: {
            need(a[2] > 0 && a[2] <= 8192 && a[3] > 0 && a[3] <= 8192 &&
                         a[2] * a[3] <= max_blob / 4 && a[4] == 0x1908 && a[5] == 0x1401,
                 "Readback geometry");
            I pack_buffer = 0, alignment = 0, row_length = 0, skip_rows = 0, skip_pixels = 0;

            for (auto [key, target] :
                 std::initializer_list<std::pair<U, I *>>{{0x88ed, &pack_buffer},
                                                          {0xd05, &alignment},
                                                          {0xd02, &row_length},
                                                          {0xd03, &skip_rows},
                                                          {0xd04, &skip_pixels}}) {
                gl->call<void>("glGetIntegerv", key, target);
            }

            need(!pack_buffer && !row_length && !skip_rows && !skip_pixels &&
                         (alignment == 1 || alignment == 2 || alignment == 4 || alignment == 8) &&
                         ((a[2] * 4) % alignment == 0),
                 "Readback pack extent");
            const auto bytes = std::size_t(a[2] * a[3] * 4);

            if (discard_replay_readback(a, bytes)) {
                out = {bytes, {}, "ANGLE-diagnostic-readback"};
                break;
            }

            std::vector<std::byte> pixels(bytes);
            gl->call<void>("glReadPixels", signed_bits(a[0]), signed_bits(a[1]), I(a[2]), I(a[3]),
                           U(a[4]), U(a[5]), pixels.data());

            // The guest publishes exactly these pixels, so a black frame is decided here: a nonzero
            // count means the guest loses the pixels on its way to the Screen buffer, a zero count
            // means the compositor's own readback is black and the screen framebuffer is the
            // suspect.
            if (!replaying) {
                I draw_fb = 0, read_fb = 0;
                gl->call<void>("glGetIntegerv", U(0x8ca6), &draw_fb);
                gl->call<void>("glGetIntegerv", U(0x8caa), &read_fb);
                std::size_t nonzero_rgb = 0;

                for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
                    if (pixels[i] != std::byte{0} || pixels[i + 1] != std::byte{0} ||
                        pixels[i + 2] != std::byte{0}) {
                        ++nonzero_rgb;
                    }
                }

                Json extra;

                if (!nonzero_rgb) {
                    auto screen = probe_framebuffer(true, true);
                    extra = {{"screen_nonzero_rgb", screen.nonzero_rgb},
                             {"screen_draw_buffer", screen.draw_buffer},
                             {"screen_probe_width", screen.width},
                             {"screen_probe_height", screen.height}};
                }

                logging::write(logging::Level::debug, "graphics", "screen.readback_probe",
                               {{"width", a[2]},
                                {"height", a[3]},
                                {"x", a[0]},
                                {"y", a[1]},
                                {"target_blob", a[6]},
                                {"draw_fb", draw_fb},
                                {"read_fb", read_fb},
                                {"nonzero_rgb", nonzero_rgb},
                                {"sampled_pixels", pixels.size() / 4},
                                {"clear_rgba_bits", Json::array({clear_bits[0], clear_bits[1],
                                                                 clear_bits[2], clear_bits[3]})},
                                {"screen_probe", extra}});
            }

            if (a[6]) {
                write_blob(a[6], pixels);
                out = {pixels.size(), {}, "ANGLE-diagnostic-readback"};
            } else {
                out = {0, std::move(pixels), "ANGLE-diagnostic-readback"};
            }

            break;
        }
        case 25:
            out = {gl->call<U>("glCreateShader", U(a[0])), {}, "glCreateShader"};
            break;
        case 26: {
            need(std::find(data.begin(), data.end(), std::byte{0}) == data.end(),
                 "Shader source terminator");
            auto text = reinterpret_cast<const char *>(data.data());
            I size = I(data.size());

            // The composite draws five times per frame into the window with three programs and
            // leaves it empty, so the sources themselves settle what those draws are supposed to
            // produce: the blend of the sampler with the colour uniform, a discard, or a coverage
            // test.
            if (!replaying && shader_probe_events < 12) {
                const auto limit = std::min<std::size_t>(data.size(), 2048);
                logging::write(logging::Level::debug, "graphics", "screen.shader_probe",
                               {{"event", shader_probe_events},
                                {"shader", U(a[0])},
                                {"bytes", data.size()},
                                {"text", std::string(text, limit)}});
                ++shader_probe_events;
            }

            gl->call<void>("glShaderSource", U(a[0]), I(1), &text, &size);
            out.name = "glShaderSource";
            break;
        }
        case 27:
            gl->call<void>("glCompileShader", U(a[0]));
            out.name = "glCompileShader";
            break;
        case 28:
        case 32: {
            I value = 0;
            gl->call<void>(op == 28 ? "glGetShaderiv" : "glGetProgramiv", U(a[0]), U(a[1]), &value);
            append(out.payload, value);
            out.name = op == 28 ? "glGetShaderiv" : "glGetProgramiv";
            break;
        }
        case 29:
            out = {gl->call<U>("glCreateProgram"), {}, "glCreateProgram"};
            break;
        case 30:
            gl->call<void>("glAttachShader", U(a[0]), U(a[1]));
            out.name = "glAttachShader";
            break;
        case 31:
            gl->call<void>("glLinkProgram", U(a[0]));
            out.name = "glLinkProgram";
            break;
        case 33:
            gl->call<void>("glUseProgram", U(a[0]));
            out.name = "glUseProgram";
            break;
        case 34:
            gl->call<void>("glViewport", signed_bits(a[0]), signed_bits(a[1]), signed_bits(a[2]),
                           signed_bits(a[3]));
            out.name = "glViewport";

            if (!replaying) {
                I target = 0;
                gl->call<void>("glGetIntegerv", U(0x8ca6), &target);
                viewport_by_fb[target] = {signed_bits(a[0]), signed_bits(a[1]), signed_bits(a[2]),
                                          signed_bits(a[3])};
            }

            break;
        case 35:
            gl->call<void>("glDrawArrays", U(a[0]), signed_bits(a[1]), signed_bits(a[2]));
            out.name = "glDrawArrays";

            // glDrawArrays has its own dispatch case, so the census in generated_call never sees
            // it; counting it here keeps the per-framebuffer draw map complete.
            if (!replaying) {
                I target = 0;
                gl->call<void>("glGetIntegerv", U(0x8ca6), &target);
                ++draw_calls_by_fb[target];
                ++draw_calls_all[target];

                if (live_swaps >= 1 && draw_probe_events < 24) {
                    Json entry = probe_draw_state(target);
                    entry["event"] = draw_probe_events;
                    entry["name"] = "glDrawArrays";
                    entry["count"] = signed_bits(a[2]);
                    ++draw_probe_events;
                    logging::write(logging::Level::debug, "graphics", "screen.draw_probe", entry);
                }

                if (target == 0 && draw_state_events < 16) {
                    Json entry = probe_after_draw(0);
                    entry["event"] = draw_state_events;
                    entry["name"] = "glDrawArrays";
                    entry["count"] = signed_bits(a[2]);
                    ++draw_state_events;
                    logging::write(logging::Level::debug, "graphics", "screen.draw_state_probe",
                                   entry);
                }
            }

            break;
        case 36:
        case 37:
            gl->call<void>(op == 36 ? "glDeleteShader" : "glDeleteProgram", U(a[0]));
            out.name = op == 36 ? "glDeleteShader" : "glDeleteProgram";
            break;
        case 61:
        case 65:
        case 66: {
            I value = 0;
            const char *name = op == 61   ? "eglGetConfigAttrib"
                               : op == 65 ? "eglQueryContext"
                                          : "eglQuerySurface";
            out = {egl->call<U>(name, pointer(0), pointer(1), I(a[2]), &value), {}, name};
            append(out.payload, value);
            break;
        }
        case 62:
        case 63:
            out = {expose_handle(op == 62 ? EglKind::context : EglKind::display,
                                 egl->call<V>(op == 62 ? "eglGetCurrentContext"
                                                       : "eglGetCurrentDisplay")),
                   {},
                   op == 62 ? "eglGetCurrentContext" : "eglGetCurrentDisplay"};
            break;
        case 70:
            out = {expose_handle(EglKind::surface, egl->call<V>("eglGetCurrentSurface", I(a[0]))),
                   {},
                   "eglGetCurrentSurface"};
            break;
        case 64:
            out = {egl->call<U>("eglQueryAPI"), {}, "eglQueryAPI"};
            break;
        case 67:
            out = {egl->call<U>("eglSurfaceAttrib", pointer(0), pointer(1), I(a[2]), I(a[3])),
                   {},
                   "eglSurfaceAttrib"};
            break;
        case 68:
            out = {egl->call<U>("eglSwapInterval", pointer(0), I(a[1])), {}, "eglSwapInterval"};
            break;
        case 69: {
            // Swapped frames can be published black, so the swap names the surface it swapped and
            // compares the pixels in the bound framebuffer with those in the screen (default)
            // framebuffer. Both probes only read, and the bound pair is restored. The record is
            // written after the swap so it can also carry the swap's own result and arguments: a
            // guest that never prints its present line returned early because the swap it called
            // reported failure. Capped, because each probe costs a full-frame readback.
            const bool probe = !replaying && live_swaps < 8;
            BufferProbe bound{}, screen{};
            Json frames = Json::array();

            if (probe) {
                bound = probe_framebuffer(false, true);
                screen = probe_framebuffer(true, true);

                if (live_swaps < 4 && !seen_fbs.empty()) {
                    frames = probe_scene_framebuffers();
                }
            }

            out = {egl->call<U>("eglSwapBuffers", pointer(0), pointer(1)), {}, "eglSwapBuffers"};

            if (probe) {
                Json draws = Json::array();

                for (const auto &[framebuffer, count] : draw_calls_all) {
                    draws.push_back({{"framebuffer", framebuffer}, {"draws", count}});
                }

                logging::write(logging::Level::debug, "graphics", "screen.present_probe",
                               {{"swap", live_swaps},
                                {"probe_width", bound.width},
                                {"probe_height", bound.height},
                                {"bound_draw_fb", bound.draw_fb},
                                {"bound_read_fb", bound.read_fb},
                                {"bound_draw_buffer", bound.draw_buffer},
                                {"bound_nonzero_rgb", bound.nonzero_rgb},
                                {"bound_samples", sample_json(bound.sample)},
                                {"screen_nonzero_rgb", screen.nonzero_rgb},
                                {"screen_samples", sample_json(screen.sample)},
                                {"screen_draw_buffer", screen.draw_buffer},
                                {"current_draw_surface", current_surface(I(0x3059))},
                                {"current_read_surface", current_surface(I(0x305a))},
                                {"current_context", current_context()},
                                {"exported", exported},
                                {"clear_events", clear_events},
                                {"swap_result", out.result},
                                {"display_arg", a[0]},
                                {"surface_arg", a[1]},
                                {"draws_by_framebuffer", draws},
                                {"scene_framebuffers", frames}});
            }

            // Whether the window framebuffer accepts writes at all: the guest's own composite
            // leaves it untouched, and the difference between "the draw wrote nothing" and "nothing
            // can be written here" is one clear of a known colour followed by a read of a single
            // pixel. The colour is put back afterwards and the probe's own errors are drained.
            if (!replaying && live_swaps == 1) {
                float saved[4] = {0, 0, 0, 0};
                gl->call<void>("glGetFloatv", U(0x0c22), saved);
                gl->call<void>("glClearColor", 0.25f, 0.5f, 0.75f, 1.0f);
                gl->call<void>("glClear", U(0x4000));
                gl->call<void>("glFinish");
                unsigned char pixel[4] = {0, 0, 0, 0};
                gl->call<void>("glReadPixels", I(960), I(360), I(1), I(1), U(0x1908), U(0x1401),
                               pixel);
                const U error = gl->call<U>("glGetError");
                char text[32];
                std::snprintf(text, sizeof(text), "%02x %02x %02x %02x", pixel[0], pixel[1],
                              pixel[2], pixel[3]);
                const bool matches = pixel[0] == 64 && pixel[1] == 128 && pixel[2] == 191 &&
                                     pixel[3] == 255;
                gl->call<void>("glClearColor", saved[0], saved[1], saved[2], saved[3]);
                drain_gl_errors();
                logging::write(
                        logging::Level::debug, "graphics", "screen.fb_write_probe",
                        {{"pixel", text},
                         {"expected_hex", "40 80 bf ff"},
                         {"matches", matches},
                         {"gl_error", error},
                         {"clear_color_saved", Json::array({double(saved[0]), double(saved[1]),
                                                            double(saved[2]), double(saved[3])})}});
                // Whether the window's depth buffer is what hides the guest's composite: the guest
                // clears colour only and keeps the depth test enabled, so a surface whose depth
                // starts anywhere but 1.0 rejects every fragment without an error. One clear of the
                // depth buffer here, before the next guest frame, separates the two cases, and the
                // count of fragments in the frames that follow answers which case it is.
                GLboolean depth_mask = 0;
                gl->call<void>("glGetBooleanv", U(0x0b72), &depth_mask);
                I depth_bits = 0;
                gl->call<void>("glGetIntegerv", U(0x0d56), &depth_bits);
                gl->call<void>("glDepthMask", GLboolean(1));
                gl->call<void>("glClearDepthf", 1.0f);
                gl->call<void>("glClear", U(0x0100));
                const U depth_clear_error = gl->call<U>("glGetError");
                gl->call<void>("glDepthMask", depth_mask);
                drain_gl_errors();
                logging::write(logging::Level::debug, "graphics", "screen.fb_depth_clear_probe",
                               {{"depth_bits", depth_bits},
                                {"depth_mask_before", (bool)depth_mask},
                                {"clear_error", depth_clear_error}});
                // Whether a texture level can be defined on this connection at all: the guest's own
                // uploads all leave an undefined level 0, and the same call made here answers
                // whether the rejection belongs to the call's arguments or to the environment.
                I self = 0, previous = 0;
                gl->call<void>("glGenTextures", I(1), &self);
                gl->call<void>("glGetIntegerv", U(0x8069), &previous);
                gl->call<void>("glBindTexture", U(0xde1), self);
                gl->call<void>("glTexImage2D", U(0xde1), I(0), I(0x1908), I(64), I(64), I(0),
                               U(0x1908), U(0x1401), static_cast<const void *>(nullptr));
                const U self_error = gl->call<U>("glGetError");
                I self_width = 0, self_height = 0;
                gl->call<void>("glGetTexLevelParameteriv", U(0xde1), I(0), U(0x1000), &self_width);
                gl->call<void>("glGetTexLevelParameteriv", U(0xde1), I(0), U(0x1001), &self_height);
                const U self_size_error = gl->call<U>("glGetError");
                // The same question asked functionally, since the size query itself may be
                // unsupported on this connection: the texture is filled with a known colour,
                // attached to a scratch framebuffer and read back. A complete attachment and the
                // expected pixel mean the upload works and the level exists, which would make every
                // earlier 0x0 a statement about the query rather than about the texture.
                std::vector<unsigned char> fill(std::size_t(64) * std::size_t(64) * 4, 0);

                for (std::size_t i = 0; i + 3 < fill.size(); i += 4) {
                    fill[i] = 255;
                    fill[i + 3] = 255;
                }

                gl->call<void>("glTexSubImage2D", U(0xde1), I(0), I(0), I(0), I(64), I(64),
                               U(0x1908), U(0x1401), static_cast<const void *>(fill.data()));
                const U self_sub_error = gl->call<U>("glGetError");
                I saved_fb = 0;
                gl->call<void>("glGetIntegerv", U(0x8ca6), &saved_fb);
                I scratch = 0;
                gl->call<void>("glGenFramebuffers", I(1), &scratch);
                gl->call<void>("glBindFramebuffer", U(0x8d40), scratch);
                gl->call<void>("glFramebufferTexture2D", U(0x8d40), U(0x8ce0), U(0xde1), self,
                               I(0));
                const U self_status = gl->call<U>("glCheckFramebufferStatus", U(0x8d40));
                unsigned char own[4] = {0, 0, 0, 0};

                if (self_status == U(0x8cd5)) {
                    gl->call<void>("glReadPixels", I(0), I(0), I(1), I(1), U(0x1908), U(0x1401),
                                   own);
                }

                char own_text[32];
                std::snprintf(own_text, sizeof(own_text), "%02x %02x %02x %02x", own[0], own[1],
                              own[2], own[3]);
                gl->call<void>("glBindFramebuffer", U(0x8d40), saved_fb);
                gl->call<void>("glDeleteFramebuffers", I(1), &scratch);
                drain_gl_errors();
                logging::write(logging::Level::debug, "graphics", "screen.texture_self_read_probe",
                               {{"sub_error", self_sub_error},
                                {"status", self_status},
                                {"pixel", own_text},
                                {"expected_hex", "ff 00 00 ff"}});
                gl->call<void>("glBindTexture", U(0xde1), previous);
                gl->call<void>("glDeleteTextures", I(1), &self);
                drain_gl_errors();
                logging::write(logging::Level::debug, "graphics", "screen.texture_self_probe",
                               {{"previous", previous},
                                {"error", self_error},
                                {"width", self_width},
                                {"height", self_height},
                                {"size_error", self_size_error}});
                // Whether the client can run the ES3 entry point used above at all: the version
                // string and the major version decide if the "level 0 is undefined" verdict of
                // every size probe in this run is a statement about the texture or about the query
                // itself.
                const char *version = reinterpret_cast<const char *>(
                        gl->call<const void *>("glGetString", U(0x1f02)));
                const char *renderer = reinterpret_cast<const char *>(
                        gl->call<const void *>("glGetString", U(0x1f01)));
                I major = -1;
                gl->call<void>("glGetIntegerv", U(0x821b), &major);
                const U version_error = gl->call<U>("glGetError");
                drain_gl_errors();
                logging::write(logging::Level::debug, "graphics", "screen.gl_version_probe",
                               {{"version", version ? version : ""},
                                {"renderer", renderer ? renderer : ""},
                                {"major", major},
                                {"version_error", version_error}});
                // Whether the window framebuffer accepts a draw of this worker's own making, with
                // its own program and its own buffer: the guest's composite leaves the window
                // untouched while its offscreen targets do receive fragments, so "the drawable
                // refuses every draw" and "this particular draw is rejected" have to be told apart
                // before the guest's state is blamed.
                {
                    I program_before = 0;
                    gl->call<void>("glGetIntegerv", U(0x8b8d), &program_before);
                    I array_before = 0;
                    gl->call<void>("glGetIntegerv", U(0x8894), &array_before);
                    I enabled_before = 0;
                    gl->call<void>("glGetVertexAttribiv", I(0), U(0x8622), &enabled_before);
                    // Attribute zero is reprogrammed for the probe draw, so its whole description
                    // is saved: the guest keeps its own view of this state and does not re-specify
                    // an attribute it believes is already set, so a probe that leaves its own
                    // pointer behind would change every later draw of the guest.
                    I attrib_size_before = 0, attrib_type_before = 0, attrib_stride_before = 0,
                      attrib_normalized_before = 0;
                    void *attrib_pointer_before = nullptr;
                    gl->call<void>("glGetVertexAttribiv", I(0), U(0x8623), &attrib_size_before);
                    gl->call<void>("glGetVertexAttribiv", I(0), U(0x8624), &attrib_stride_before);
                    gl->call<void>("glGetVertexAttribiv", I(0), U(0x8625), &attrib_type_before);
                    gl->call<void>("glGetVertexAttribiv", I(0), U(0x886a),
                                   &attrib_normalized_before);
                    gl->call<void>("glGetVertexAttribPointerv", I(0), U(0x8645),
                                   &attrib_pointer_before);
                    const char *vertex_source =
                            "attribute vec2 p;void main(){gl_Position=vec4(p,0.0,1.0);}";
                    const char *fragment_source =
                            "precision mediump float;void main(){gl_FragColor=vec4(1.0,0.0,1.0,1.0);}";
                    U vertex = gl->call<U>("glCreateShader", U(0x8b31));
                    U fragment = gl->call<U>("glCreateShader", U(0x8b30));
                    I vertex_size = I(std::strlen(vertex_source)),
                      fragment_size = I(std::strlen(fragment_source));
                    gl->call<void>("glShaderSource", vertex, I(1), &vertex_source, &vertex_size);
                    gl->call<void>("glCompileShader", vertex);
                    gl->call<void>("glShaderSource", fragment, I(1), &fragment_source,
                                   &fragment_size);
                    gl->call<void>("glCompileShader", fragment);
                    I vertex_status = 0, fragment_status = 0;
                    gl->call<void>("glGetShaderiv", vertex, U(0x8b81), &vertex_status);
                    gl->call<void>("glGetShaderiv", fragment, U(0x8b81), &fragment_status);
                    const U shader_error = gl->call<U>("glGetError");
                    U program = gl->call<U>("glCreateProgram");
                    gl->call<void>("glAttachShader", program, vertex);
                    gl->call<void>("glAttachShader", program, fragment);
                    gl->call<void>("glBindAttribLocation", program, I(0), "p");
                    gl->call<void>("glLinkProgram", program);
                    I link_status = 0;
                    gl->call<void>("glGetProgramiv", program, U(0x8b82), &link_status);
                    const U link_error = gl->call<U>("glGetError");
                    I buffer = 0;
                    gl->call<void>("glGenBuffers", I(1), &buffer);
                    gl->call<void>("glBindBuffer", U(0x8892), buffer);
                    const float corners[6] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
                    gl->call<void>("glBufferData", U(0x8892), GLsizeiptr(sizeof(corners)),
                                   static_cast<const void *>(corners), U(0x88e0));
                    gl->call<void>("glUseProgram", program);
                    gl->call<void>("glEnableVertexAttribArray", I(0));
                    gl->call<void>("glVertexAttribPointer", I(0), I(2), U(0x1406), GLboolean(0),
                                   I(0), static_cast<const V>(nullptr));
                    gl->call<void>("glColorMask", GLboolean(1), GLboolean(1), GLboolean(1),
                                   GLboolean(1));
                    gl->call<void>("glDrawArrays", U(0x0004), I(0), I(3));
                    gl->call<void>("glFinish");
                    unsigned char own[4] = {0, 0, 0, 0};
                    gl->call<void>("glReadPixels", I(960), I(360), I(1), I(1), U(0x1908), U(0x1401),
                                   own);
                    const bool own_matches = own[0] == 255 && own[1] == 0 && own[2] == 255 &&
                                             own[3] == 255;
                    const U draw_error = gl->call<U>("glGetError");
                    char own_text[32];
                    std::snprintf(own_text, sizeof(own_text), "%02x %02x %02x %02x", own[0], own[1],
                                  own[2], own[3]);
                    gl->call<void>("glBindBuffer", U(0x8892), I(array_before));
                    gl->call<void>("glVertexAttribPointer", I(0), attrib_size_before,
                                   U(attrib_type_before), GLboolean(attrib_normalized_before),
                                   attrib_stride_before,
                                   static_cast<const V>(attrib_pointer_before));
                    gl->call<void>("glDeleteBuffers", I(1), &buffer);

                    if (!enabled_before) {
                        gl->call<void>("glDisableVertexAttribArray", I(0));
                    }

                    gl->call<void>("glUseProgram", U(program_before));
                    gl->call<void>("glDeleteShader", vertex);
                    gl->call<void>("glDeleteShader", fragment);
                    gl->call<void>("glDeleteProgram", program);
                    gl->call<void>("glClear", U(0x4000));
                    drain_gl_errors();
                    logging::write(
                            logging::Level::debug, "graphics", "screen.fb_draw_probe",
                            {{"pixel", own_text},
                             {"expected_hex", "ff 00 ff ff"},
                             {"matches", own_matches},
                             {"gl_error", draw_error},
                             {"shader_status", Json::array({vertex_status, fragment_status})},
                             {"link_status", link_status},
                             {"shader_error", shader_error},
                             {"link_error", link_error},
                             {"program_before", program_before}});
                }
            }

            ++live_swaps;
            break;
        }
        default:
            throw VoyemuError("GRAPHICS", "Unsupported opcode");
        }

        need(out.payload.size() <= rpc_payload_bytes, "RPC output extent");

        return out;
    }
};

void write_invalidated_channel_disposition(const Options &options) {
    need(options.generation && options.worker_nonce && !options.session_id.empty(),
         "Worker disposition identity");
    const auto worker_pid = os::current_process();
    write_private_json(options.session / ("worker-disposition-" +
                                          std::to_string(options.worker_nonce) + ".json"),
                       {{"schema", 1},
                        {"session_id", options.session_id},
                        {"reason", "rpc_channel_invalidated"},
                        {"producer_address", options.address},
                        {"worker_pid", worker_pid},
                        {"generation", options.generation},
                        {"nonce", options.worker_nonce},
                        {"guest_pid", options.guest_pid},
                        {"producer_incarnation", options.producer_incarnation}});
}

Backend::Backend(Options o) : impl_(std::make_unique<Impl>(std::move(o))) {}

Backend::~Backend() = default;

Reply Backend::dispatch(U opcode, const Arguments &a, std::span<const std::byte> data) {
    auto out = impl_->journal_dispatch(opcode, a, data);

    need(out.payload.size() <= rpc_payload_bytes, "RPC output extent");

    return out;
}

bool Backend::finished() const {
    return impl_->done;
}

ServiceStats Backend::service_stats() const {
    return impl_->service_stats;
}

unsigned Backend::idle_wait_us() const {
    const auto &p = *impl_;

    return p.idle_poll.microseconds(p.control_claimed && p.input_request_seen &&
                                    !p.other_request_seen);
}

RpcProfileSnapshot Backend::rpc_profile() const {
    const auto &p = *impl_;
    RpcProfileSnapshot result;

    if (!p.profile) {
        return result;
    }

    result.enabled = true;
    result.first_received_ns = p.profile->first_received_ns;
    result.last_acknowledged_ns = p.profile->last_acknowledged_ns;

    for (std::size_t i = 0; i < p.profile->wire.size(); i++) {
        if (p.profile->wire[i].calls) {
            auto entry = p.profile->wire[i];
            entry.opcode = i < 512 ? U(i) : std::numeric_limits<U>::max();
            result.wire.push_back(entry);
        }
    }

    // Existing API counts include calls unpacked from a wire batch; keep them separate from wire
    // requests.
    for (const auto &[opcode, count] : p.op_counts) {
        if (opcode < 512) {
            result.api.emplace_back(opcode, count);
        } else {
            result.unknown_api_calls += count;
        }
    }

    return result;
}

bool Backend::service() {
    auto &p = *impl_;

    ++p.service_stats.polls;
    const auto now = now_ns();
    // Legacy producers check idle backing at 10 Hz; a negotiated input controller checks at 50 Hz.
    // Pending RPC requests and input notifications always validate backing identity.
    bool checked = false;

    if (now >= p.next_idle_identity_check_ns) {
        p.ram.check_identity();
        ++p.service_stats.identity_checks;
        p.next_idle_identity_check_ns = now + 100000000;
        checked = true;
    }

    auto page = p.ram.extent(p.options.address, 4096);
    auto *header = reinterpret_cast<U *>(page.data());
    const U magic = header[0], version = header[1];

    if (magic != rpc_magic || version != rpc_version) {
        // Capture only eight protocol words; this best-effort snapshot never reads a payload.
        std::array<U, 8> words{magic, version};

        for (unsigned i = 2; i < words.size(); ++i) {
            words[i] = std::atomic_ref(header[i]).load(std::memory_order_relaxed);
        }

        logging::write(
                logging::Level::error, "graphics", "rpc.header_failed",
                {{"producer_address", p.options.address},
                 {"expected_magic", rpc_magic},
                 {"expected_version", rpc_version},
                 {"actual_magic", magic},
                 {"actual_version", version},
                 {"header_words", words},
                 {"last_opcode", p.last_opcode ? Json(*p.last_opcode) : Json(nullptr)},
                 {"last_sequence",
                  p.last_successful_sequence ? Json(*p.last_successful_sequence) : Json(nullptr)},
                 {"last_state", p.last_state ? Json(*p.last_state) : Json(nullptr)}});
        throw RpcChannelInvalidated();
    }

    if (p.options.guest_pid || p.options.producer_incarnation) {
        const auto *owner = reinterpret_cast<const std::uint64_t *>(page.data() + 240);
        const auto guest_pid = owner[0], incarnation = owner[1];

        if (guest_pid != p.options.guest_pid || incarnation != p.options.producer_incarnation) {
            logging::write(logging::Level::error, "graphics", "rpc.owner_failed",
                           {{"producer_address", p.options.address},
                            {"expected_guest_pid", p.options.guest_pid},
                            {"actual_guest_pid", guest_pid},
                            {"expected_producer_incarnation", p.options.producer_incarnation},
                            {"actual_producer_incarnation", incarnation},
                            {"last_opcode", p.last_opcode ? Json(*p.last_opcode) : Json(nullptr)},
                            {"last_sequence", p.last_successful_sequence
                                                      ? Json(*p.last_successful_sequence)
                                                      : Json(nullptr)}});
            throw RpcChannelInvalidated("Published RPC page owner");
        }
    }

    auto state = std::atomic_ref(header[2]).load(std::memory_order_acquire);
    p.last_state = state;

    if (state == 3) {
        auto *reserved = reinterpret_cast<std::uint64_t *>(page.data() + 176);

        if (reserved[0] == 0x3145534f4c434751ULL) {
            const auto worker_pid = os::current_process();
            write_private_json(p.options.session /
                                       ("retired-" + std::to_string(p.options.address) + ".json"),
                               {{"schema", 1},
                                {"producer_address", p.options.address},
                                {"worker_pid", worker_pid},
                                {"acknowledged", true}});
            std::atomic_ref(header[2]).store(4, std::memory_order_release);
        }

        p.done = true;

        return false;
    }

    p.idle_poll.observe(state == 1, now);
    auto *wake = reinterpret_cast<std::uint64_t *>(page.data() + 176);

    if (p.control_claimed &&
        std::atomic_ref(wake[0]).load(std::memory_order_acquire) == input_wake_magic &&
        now >= p.next_input_wake_ns) {
        if (!checked) {
            p.ram.check_identity();
            ++p.service_stats.identity_checks;
            checked = true;
        }

        p.next_input_wake_ns = now + 20000000;
        p.notify_input(wake);
    }

    if (state != 1) {
        return false;
    }

    if (!checked) {
        p.ram.check_identity();
        ++p.service_stats.identity_checks;
    }

    U opcode = header[3], sequence = header[4], size = header[5];
    need(sequence > p.last_seq && size <= rpc_payload_bytes, "RPC replay/input extent");
    Arguments arguments{};
    std::memcpy(arguments.data(), page.data() + 32, sizeof(arguments));
    std::vector<std::byte> input(page.begin() + 256, page.begin() + 256 + size);
    Reply result{};
    U fault = 0;

    // Non-MCU requests retain the fast burst cadence; long idle periods may back off.
    if (opcode != 363 && opcode != 364) {
        p.other_request_seen = true;
    }

    const auto begin = now_ns();
    try {
        result = dispatch(opcode, arguments, input);
    } catch (const std::exception &e) {
        fault = 1;
        result.name = "error-op-" + std::to_string(opcode);
        std::string text = e.what();

        if (text.size() > rpc_payload_bytes) {
            text.resize(rpc_payload_bytes);
        }

        result.payload = bytes_of(text);

        if (p.options.profile_rpc) {
            const std::string_view message = e.what();
            const bool control = message == "Control file ownership" ||
                                 message == "Control file extent" ||
                                 message == "Control file open" || message == "Control file race" ||
                                 message == "Control file read";
            logging::write(logging::Level::debug, "graphics", "rpc.dispatch_fault",
                           {{"producer_address", p.options.address},
                            {"opcode", opcode},
                            {"category", control ? std::string(message) : "other"}});
        }
    }

    if (!fault && (opcode == 363 || opcode == 364)) {
        p.input_request_seen = true;
    }

    std::memcpy(page.data() + 256, result.payload.data(), result.payload.size());
    std::memcpy(page.data() + 160, &result.result, 8);
    header[6] = U(result.payload.size());
    header[7] = fault;
    std::atomic_ref(header[2]).store(2, std::memory_order_release);

    if (p.profile) {
        const auto acknowledged = now_ns();

        if (!p.profile->first_received_ns) {
            p.profile->first_received_ns = now;
        }

        p.profile->last_acknowledged_ns = acknowledged;
        auto &entry = p.profile->wire[opcode < 512 ? opcode : 512];
        const auto duration = acknowledged - begin;
        entry.calls++;
        entry.faults += fault != 0;
        entry.dispatch_ns += duration;
        entry.max_dispatch_ns = std::max(entry.max_dispatch_ns, duration);
    }

    p.last_seq = sequence;

    if (!fault) {
        p.last_opcode = opcode;
        p.last_successful_sequence = sequence;
    }

    if (fault) {
        logging::write(logging::Level::error, "graphics", "rpc.failed",
                       {{"opcode", opcode},
                        {"sequence", sequence},
                        {"error", std::string(reinterpret_cast<const char *>(result.payload.data()),
                                              result.payload.size())}});
    } else if (opcode == 354 && p.live_exported == 1) {
        logging::write(logging::Level::info, "graphics", "screen.first_frame",
                       {{"sequence", p.exported},
                        {"width", frames::width},
                        {"height", frames::height},
                        {"dispatch_us", (now_ns() - begin) / 1000}});
    }

    if (fault || opcode >= 354) {
        Json evidence{{"opcode", opcode},
                      {"sequence", sequence},
                      {"name", result.name},
                      {"fault", fault},
                      {"result", result.result},
                      {"input_bytes", size},
                      {"output_bytes", result.payload.size()},
                      {"dispatch_us", (now_ns() - begin) / 1000},
                      {"host_monotonic_ns", now_ns()}};

        if (fault) {
            evidence["error"] = std::string(reinterpret_cast<const char *>(result.payload.data()),
                                            result.payload.size());
        }

        auto path = p.producer / "native-transport.jsonl";
        need(!std::filesystem::is_symlink(path), "Native trace ownership");

        if (!std::filesystem::exists(path) || std::filesystem::file_size(path) < 8 * 1024 * 1024) {
            std::ofstream output(path, std::ios::app);
            need(bool(output), "Native trace append");
            output << evidence.dump() << '\n';
        }
    }

    return true;
}
} // namespace voyemu::graphics
