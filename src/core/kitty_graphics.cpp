#include "core/kitty_graphics.hpp"
#include <png.h>
#include "core/kitty_placeholder.hpp"
#include <zlib.h>
#include <algorithm>
#include <charconv>
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include "util/mapped_buffer.hpp"

namespace gritty::core {

namespace {

struct PngMemReader {
    const uint8_t* data;
    size_t size;
    size_t offset;
};

void png_mem_read_fn(png_structp png_ptr, png_bytep outBytes, png_size_t byteCountToRead) {
    auto* reader = static_cast<PngMemReader*>(png_get_io_ptr(png_ptr));
    if (reader->offset + byteCountToRead > reader->size) {
        png_error(png_ptr, "PNG read EOF");
        return;
    }
    std::memcpy(outBytes, reader->data + reader->offset, byteCountToRead);
    reader->offset += byteCountToRead;
}

// Pixman's PIXMAN_a8r8g8b8 destination is premultiplied. Libpng and Kitty's f=32
// payloads carry straight alpha, so convert once at decode time.
void premultiply_argb(uint32_t* pixels, int count) {
    int i = 0;
#if defined(__aarch64__) || defined(_M_ARM64)
    const uint16x8_t round_bias = vdupq_n_u16(128);
    for (; i + 8 <= count; i += 8) {
        // PIXMAN_a8r8g8b8 memory is BGRA on little-endian ARM64.
        uint8x8x4_t bgra = vld4_u8(reinterpret_cast<const uint8_t*>(pixels + i));
        const uint8x8_t a = bgra.val[3];
        for (int channel = 0; channel < 3; ++channel) {
            const uint16x8_t product = vmull_u8(bgra.val[channel], a);
            const uint16x8_t biased = vaddq_u16(product, round_bias);
            const uint16x8_t divided = vaddq_u16(biased, vshrq_n_u16(biased, 8));
            bgra.val[channel] = vshrn_n_u16(divided, 8);
        }
        vst4_u8(reinterpret_cast<uint8_t*>(pixels + i), bgra);
    }
#endif
    for (; i < count; ++i) {
        const uint32_t value = pixels[i];
        const uint32_t a = value >> 24;
        const uint32_t r = (value >> 16) & 255;
        const uint32_t g = (value >> 8) & 255;
        const uint32_t b = value & 255;
        pixels[i] = (a << 24) |
            (static_cast<uint32_t>((r * a + 127) / 255) << 16) |
            (static_cast<uint32_t>((g * a + 127) / 255) << 8) |
            static_cast<uint32_t>((b * a + 127) / 255);
    }
}

} // namespace

namespace {
bool path_is_under(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto p = path.lexically_normal().string();
    const auto r = root.lexically_normal().string();
    return p == r || (p.size() > r.size() && p.starts_with(r) &&
        (r.empty() || r.back() == '/' || p[r.size()] == '/'));
}
} // namespace

KittyGraphicsEngine::KittyGraphicsEngine(size_t max_memory_bytes)
    : max_memory_(max_memory_bytes) {}

std::vector<uint8_t> KittyGraphicsEngine::decode_base64(std::string_view input) {
    static const int8_t b64_table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    std::vector<uint8_t> out;
    out.reserve((input.size() * 3) / 4);

    uint32_t val = 0;
    int valb = -8;
    for (char c : input) {
        if (c == '\r' || c == '\n' || c == ' ') continue;
        if (c == '=') break;
        int d = b64_table[static_cast<uint8_t>(c)];
        if (d < 0) continue;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::optional<util::PixmanImage> KittyGraphicsEngine::decode_image(
    std::span<const uint8_t> raw_data,
    KittyFormat format,
    int width,
    int height,
    bool compressed
) {
    std::vector<uint8_t> decompressed;
    const uint8_t* data_ptr = raw_data.data();
    size_t data_len = raw_data.size();

    if (compressed) {
        uLongf dest_len = width > 0 && height > 0 ? (width * height * 4) : (raw_data.size() * 4);
        decompressed.resize(dest_len);
        int res = uncompress(decompressed.data(), &dest_len, raw_data.data(), raw_data.size());
        if (res == Z_OK) {
            decompressed.resize(dest_len);
            data_ptr = decompressed.data();
            data_len = dest_len;
        } else {
            return std::nullopt;
        }
    }

    if (format == KittyFormat::Png) {
        PngMemReader reader{
            .data = data_ptr,
            .size = data_len,
            .offset = 0
        };

        png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png) return std::nullopt;

        png_infop info = png_create_info_struct(png);
        if (!info) {
            png_destroy_read_struct(&png, nullptr, nullptr);
            return std::nullopt;
        }

        if (setjmp(png_jmpbuf(png))) {
            png_destroy_read_struct(&png, &info, nullptr);
            return std::nullopt;
        }

        png_set_read_fn(png, &reader, png_mem_read_fn);
        png_read_info(png, info);

        int img_w = png_get_image_width(png, info);
        int img_h = png_get_image_height(png, info);
        png_byte color_type = png_get_color_type(png, info);
        png_byte bit_depth = png_get_bit_depth(png, info);

        if (bit_depth == 16) png_set_strip_16(png);
        if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
        if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
        if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
        if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY) {
            png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
        }

        png_set_bgr(png); // Convert to BGRA host order for PIXMAN_a8r8g8b8
        png_read_update_info(png, info);

        int stride = img_w * 4;
        pixman_image_t* raw_img = pixman_image_create_bits(
            PIXMAN_a8r8g8b8, img_w, img_h, nullptr, stride);
        if (!raw_img) {
            png_destroy_read_struct(&png, &info, nullptr);
            return std::nullopt;
        }

        util::PixmanImage img(raw_img);
        uint32_t* dst_data = img.data();

        std::vector<png_bytep> rows(img_h);
        for (int y = 0; y < img_h; ++y) {
            rows[y] = reinterpret_cast<png_bytep>(dst_data + y * (stride / 4));
        }

        png_read_image(png, rows.data());
        png_read_end(png, nullptr);
        premultiply_argb(dst_data, img_w * img_h);
        png_destroy_read_struct(&png, &info, nullptr);

        return img;
    }

    if (width <= 0 || height <= 0) return std::nullopt;

    int stride = width * 4;
    pixman_image_t* raw_img = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, width, height, nullptr, stride);
    if (!raw_img) return std::nullopt;

    util::PixmanImage img(raw_img);
    uint32_t* dst = img.data();

    if (format == KittyFormat::Rgba) {
        if (data_len < static_cast<size_t>(width * height * 4)) return std::nullopt;
        for (int i = 0; i < width * height; ++i) {
            uint8_t r = data_ptr[i * 4 + 0];
            uint8_t g = data_ptr[i * 4 + 1];
            uint8_t b = data_ptr[i * 4 + 2];
            uint8_t a = data_ptr[i * 4 + 3];
            dst[i] = (static_cast<uint32_t>(a) << 24) |
                     (static_cast<uint32_t>(r) << 16) |
                     (static_cast<uint32_t>(g) << 8) | b;
        }
        premultiply_argb(dst, width * height);
        return img;
    } else if (format == KittyFormat::Rgb) {
        if (data_len < static_cast<size_t>(width * height * 3)) return std::nullopt;
        for (int i = 0; i < width * height; ++i) {
            uint8_t r = data_ptr[i * 3 + 0];
            uint8_t g = data_ptr[i * 3 + 1];
            uint8_t b = data_ptr[i * 3 + 2];
            dst[i] = 0xFF000000 |
                     (static_cast<uint32_t>(r) << 16) |
                     (static_cast<uint32_t>(g) << 8) | b;
        }
        return img;
    }

    return std::nullopt;
}

std::string KittyGraphicsEngine::process_apc_sequence(
    std::string_view payload,
    int cursor_col,
    int cursor_row
) {
    if (!payload.starts_with('G')) return "";
    payload.remove_prefix(1);

    size_t semicolon = payload.find(';');
    std::string_view control_part = (semicolon != std::string::npos) ? payload.substr(0, semicolon) : payload;
    std::string_view data_part = (semicolon != std::string::npos) ? payload.substr(semicolon + 1) : "";

    KittyAction action = pending_chunk_.in_progress ? pending_chunk_.action : KittyAction::TransmitAndDisplay;
    KittyMedium medium = pending_chunk_.in_progress ? pending_chunk_.medium : KittyMedium::Direct;
    KittyFormat format = pending_chunk_.in_progress ? pending_chunk_.format : KittyFormat::Rgba;
    bool compressed = pending_chunk_.in_progress ? pending_chunk_.compressed : false;
    uint32_t image_id = pending_chunk_.in_progress ? pending_chunk_.image_id : 0;
    bool has_image_id = pending_chunk_.in_progress;
    uint32_t placement_id = pending_chunk_.in_progress ? pending_chunk_.placement_id : 0;
    int width = pending_chunk_.in_progress ? pending_chunk_.width : 0;
    int height = pending_chunk_.in_progress ? pending_chunk_.height : 0;
    int cols = pending_chunk_.in_progress ? pending_chunk_.cols : 0;
    int rows = pending_chunk_.in_progress ? pending_chunk_.rows : 0;
    int z_index = pending_chunk_.in_progress ? pending_chunk_.z_index : 0;
    int unicode_placeholder = pending_chunk_.in_progress ? pending_chunk_.unicode_placeholder : 0;
    int quiet = pending_chunk_.in_progress ? pending_chunk_.quiet : 0;
    int cursor_policy = pending_chunk_.in_progress ? pending_chunk_.cursor_policy : 1;
    char delete_target = pending_chunk_.in_progress ? pending_chunk_.delete_target : 'a';
    int src_x = pending_chunk_.in_progress ? pending_chunk_.src_x : 0;
    int src_y = pending_chunk_.in_progress ? pending_chunk_.src_y : 0;
    int src_w = pending_chunk_.in_progress ? pending_chunk_.src_w : 0;
    int src_h = pending_chunk_.in_progress ? pending_chunk_.src_h : 0;
    char feature = 0;
    int more_data = 0;

    size_t pos = 0;
    while (pos < control_part.size()) {
        size_t next_comma = control_part.find(',', pos);
        std::string_view kv = (next_comma != std::string::npos)
            ? control_part.substr(pos, next_comma - pos)
            : control_part.substr(pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && eq > 0) {
            const char key = kv[0];
            const std::string_view val = kv.substr(eq + 1);
            int int_val = 0;
            std::from_chars(val.data(), val.data() + val.size(), int_val);
            switch (key) {
                case 'a': if (!val.empty()) action = static_cast<KittyAction>(val[0]); break;
                case 'f': format = static_cast<KittyFormat>(int_val); break;
                case 'o': compressed = (val == "z"); break;
                case 'i': image_id = static_cast<uint32_t>(int_val); has_image_id = true; break;
                case 'p': placement_id = static_cast<uint32_t>(int_val); break;
                case 's': width = int_val; break;
                case 'v': height = int_val; break;
                case 'c': cols = int_val; break;
                case 'r': rows = int_val; break;
                case 'z': z_index = int_val; break;
                case 'm': more_data = int_val; break;
                case 'U': unicode_placeholder = int_val; break;
                case 'q': quiet = int_val; break;
                case 'e': feature = val.empty() ? 0 : val[0]; break;
                case 'C': cursor_policy = int_val; break;
                case 't': if (!val.empty()) medium = static_cast<KittyMedium>(val[0]); break;
                case 'd': if (!val.empty()) delete_target = val[0]; break;
                case 'x': src_x = int_val; break;
                case 'y': src_y = int_val; break;
                case 'w': src_w = int_val; break;
                case 'h': src_h = int_val; break;
                default: break;
            }
        }
        if (next_comma == std::string::npos) break;
        pos = next_comma + 1;
    }

    auto reply = [&](std::string_view status) {
        if (quiet >= 2) return std::string{};
        if (quiet >= 1 && status == "OK") return std::string{};
        return "\x1b_Gi=" + std::to_string(image_id) + ";" + std::string(status) + "\x1b\\";
    };

    // Delete must happen before auto-id allocation: a=d without i= targets all
    // visible placements (or the requested d= class), not a newly minted image.
    if (action == KittyAction::Delete) {
        if (delete_target == 'i' || delete_target == 'I') {
            if (has_image_id) delete_image(image_id);
        } else if (delete_target == 'p' || delete_target == 'P') {
            if (has_image_id) {
                if (delete_target == 'P') delete_image(image_id);
                else delete_placements(KittyPlacement{.image_id = image_id, .placement_id = placement_id});
            }
        } else if (delete_target == 'c' || delete_target == 'C') {
            std::vector<uint32_t> affected;
            for (const auto& p : placements_)
                if (cursor_col >= p.col && cursor_col < p.col + p.cols &&
                    cursor_row >= p.row && cursor_row < p.row + p.rows)
                    affected.push_back(p.image_id);
            delete_placements_at(cursor_col, cursor_row);
            if (delete_target == 'C') for (const auto id : affected) delete_image(id);
        } else if (has_image_id && delete_target == 'a') {
            // a=d,i=N uses the default d=a but explicitly names an image.
            delete_image(image_id);
        } else if (delete_target == 'z' || delete_target == 'Z') {
            std::vector<uint32_t> affected;
            for (const auto& p : placements_) if (p.z_index == z_index) affected.push_back(p.image_id);
            delete_placements_by_z(z_index);
            if (delete_target == 'Z') for (const auto id : affected) delete_image(id);
        } else { // a/A: all visible placements.
            std::vector<uint32_t> affected;
            for (const auto& p : placements_) affected.push_back(p.image_id);
            clear_placements();
            if (delete_target == 'A') for (const auto id : affected) delete_image(id);
        }
        return reply("OK");
    }

    if (image_id == 0) image_id = next_auto_id_++;

    if (action == KittyAction::Query) {
        if (medium == KittyMedium::SharedMem) return reply("ENOTSUP:shared memory not supported");
        if (quiet >= 2) return "";
        std::string response = "\x1b_Gi=" + std::to_string(image_id);
        if (feature == 'T') {
            response += ",e=T";
            if (unicode_placeholder == 1) response += ",U=1";
            if (cursor_policy == 1) response += ",C=1";
        }
        return response + ";OK\x1b\\";
    }

    if (more_data == 1) {
        pending_chunk_.in_progress = true;
        pending_chunk_.image_id = image_id;
        pending_chunk_.placement_id = placement_id;
        pending_chunk_.action = action;
        pending_chunk_.medium = medium;
        pending_chunk_.format = format;
        pending_chunk_.compressed = compressed;
        pending_chunk_.width = width;
        pending_chunk_.height = height;
        pending_chunk_.cols = cols;
        pending_chunk_.rows = rows;
        pending_chunk_.z_index = z_index;
        pending_chunk_.unicode_placeholder = unicode_placeholder;
        pending_chunk_.quiet = quiet;
        pending_chunk_.cursor_policy = cursor_policy;
        pending_chunk_.delete_target = delete_target;
        pending_chunk_.src_x = src_x;
        pending_chunk_.src_y = src_y;
        pending_chunk_.src_w = src_w;
        pending_chunk_.src_h = src_h;
        pending_chunk_.buffer.append(data_part);
        return "";
    }

    std::string full_payload;
    if (pending_chunk_.in_progress) {
        pending_chunk_.buffer.append(data_part);
        full_payload = std::move(pending_chunk_.buffer);
        pending_chunk_ = PendingChunkTransmission{};
    } else {
        full_payload = std::string(data_part);
    }

    bool load_failed = false;
    bool image_ready = images_.find(image_id) != images_.end();
    if (!image_ready) {
        std::span<const uint8_t> source;
        std::vector<uint8_t> decoded;
        util::MappedBuffer mapping;
        if (medium == KittyMedium::File || medium == KittyMedium::TempFile) {
            decoded = decode_base64(full_payload);
            const std::string path(reinterpret_cast<const char*>(decoded.data()), decoded.size());
            if (path.empty() || path.find('\0') != std::string::npos) {
                load_failed = true;
            } else {
                std::error_code ec;
                auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
                if (ec || canonical.empty() ||
                    !std::filesystem::is_regular_file(canonical, ec) || ec) {
                    load_failed = true;
                } else if (medium == KittyMedium::TempFile) {
                    bool allowed = false;
                    auto check_root = [&](std::string_view value) {
                        if (allowed || value.empty()) return;
                        std::error_code root_ec;
                        const auto root = std::filesystem::weakly_canonical(
                            std::filesystem::path(value), root_ec);
                        if (!root_ec && path_is_under(canonical, root)) allowed = true;
                    };
                    check_root("/tmp");
                    if (const char* env = std::getenv("TMPDIR")) check_root(env);
                    if (!allowed) load_failed = true;
                }
                if (!load_failed) {
                    const util::UniqueFd fd(::open(canonical.c_str(), O_RDONLY | O_CLOEXEC));
                    struct stat st{};
                    if (!fd.valid() || ::fstat(fd.get(), &st) != 0 || st.st_size <= 0 ||
                        static_cast<size_t>(st.st_size) > max_memory_) {
                        load_failed = true;
                    } else {
                        mapping = util::MappedBuffer(fd.get(), static_cast<size_t>(st.st_size), 0);
                        if (!mapping.valid()) load_failed = true;
                        else source = {static_cast<const uint8_t*>(mapping.data()), mapping.size()};
                    }
                }
                if (medium == KittyMedium::TempFile && !canonical.empty())
                    ::unlink(canonical.c_str());
            }
        } else {
            decoded = decode_base64(full_payload);
            source = decoded;
        }

        if (!load_failed && !source.empty()) {
            auto img_opt = decode_image(source, format, width, height, compressed);
            if (img_opt) {
                const size_t bytes = img_opt->height() * img_opt->stride();
                while (current_memory_ + bytes > max_memory_ && !images_.empty()) {
                    auto first_it = images_.begin();
                    current_memory_ -= first_it->second.byte_size;
                    images_.erase(first_it);
                }
                if (auto existing = images_.find(image_id); existing != images_.end())
                    current_memory_ -= existing->second.byte_size;
                images_[image_id] = KittyImage{
                    .id = image_id,
                    .image = std::move(*img_opt),
                    .byte_size = bytes
                };
                current_memory_ += bytes;
                image_ready = true;
            } else {
                load_failed = true;
            }
        } else if (source.empty()) {
            load_failed = true;
        }
    }

    if (!image_ready) return reply("ENOENT:failed to load image");

    if (unicode_placeholder == 1) {
        auto existing = std::find_if(virtual_placements_.begin(), virtual_placements_.end(),
            [&](const KittyVirtualPlacement& vp) {
                return vp.image_id == image_id && vp.placement_id == placement_id;
            });
        if (existing != virtual_placements_.end()) {
            existing->cols = cols;
            existing->rows = rows;
            existing->z_index = z_index;
        } else {
            virtual_placements_.push_back(KittyVirtualPlacement{image_id, placement_id, cols, rows, z_index});
        }
        return reply("OK");
    }

    if (action == KittyAction::TransmitAndDisplay || action == KittyAction::Put) {
        KittyPlacement placement{
            .image_id = image_id,
            .placement_id = placement_id,
            .col = cursor_col,
            .row = cursor_row,
            .cols = cols > 0 ? cols : 0,
            .rows = rows > 0 ? rows : 0,
            .z_index = z_index,
            .src_x = src_x,
            .src_y = src_y,
            .src_w = src_w,
            .src_h = src_h,
            .cursor_policy = cursor_policy,
        };
        if (placement_handler_) placement_handler_(placement, images_.at(image_id).image.get());
        placements_.push_back(placement);
    }
    return reply("OK");
}

const KittyVirtualPlacement* KittyGraphicsEngine::find_virtual_placement(
    uint32_t image_id, uint32_t placement_id) const noexcept {
    const KittyVirtualPlacement* fallback = nullptr;
    for (const auto& vp : virtual_placements_) {
        if (vp.image_id != image_id) continue;
        if (placement_id != 0 && vp.placement_id == placement_id) return &vp;
        if (!fallback) fallback = &vp;
    }
    return placement_id == 0 ? fallback : nullptr;
}

const KittyImage* KittyGraphicsEngine::get_image(uint32_t image_id) const noexcept {
    auto it = images_.find(image_id);
    return (it != images_.end()) ? &it->second : nullptr;
}

void KittyGraphicsEngine::delete_placements(const KittyPlacement& target) {
    for (const auto& p : placements_) {
        if (p.image_id != target.image_id) continue;
        if (target.placement_id != 0 && p.placement_id != target.placement_id) continue;
        if (placement_delete_handler_) placement_delete_handler_(p);
    }
    std::erase_if(placements_, [&](const KittyPlacement& p) {
        if (p.image_id != target.image_id) return false;
        return target.placement_id == 0 || p.placement_id == target.placement_id;
    });
}

void KittyGraphicsEngine::delete_placements_at(int col, int row) {
    for (const auto& p : placements_) {
        if (col >= p.col && col < p.col + p.cols && row >= p.row && row < p.row + p.rows &&
            placement_delete_handler_) placement_delete_handler_(p);
    }
    std::erase_if(placements_, [col, row](const KittyPlacement& p) {
        return col >= p.col && col < p.col + p.cols && row >= p.row && row < p.row + p.rows;
    });
}

void KittyGraphicsEngine::delete_placements_by_z(int z_index) {
    for (const auto& p : placements_)
        if (p.z_index == z_index && placement_delete_handler_) placement_delete_handler_(p);
    std::erase_if(placements_, [z_index](const KittyPlacement& p) { return p.z_index == z_index; });
}

void KittyGraphicsEngine::clear_placements() {
    if (placement_delete_handler_) for (const auto& p : placements_) placement_delete_handler_(p);
    placements_.clear();
    virtual_placements_.clear();
}

void KittyGraphicsEngine::delete_image(uint32_t image_id) {
    if (delete_handler_) delete_handler_(image_id);
    std::erase_if(virtual_placements_,
        [image_id](const KittyVirtualPlacement& vp) { return vp.image_id == image_id; });
    auto it = images_.find(image_id);
    if (it != images_.end()) {
        current_memory_ -= it->second.byte_size;
        images_.erase(it);
    }
    placements_.erase(
        std::remove_if(placements_.begin(), placements_.end(),
            [image_id](const KittyPlacement& p) {
                return p.image_id == image_id;
            }),
        placements_.end());
}

void KittyGraphicsEngine::clear() {
    virtual_placements_.clear();
    images_.clear();
    placements_.clear();
    current_memory_ = 0;
}

} // namespace gritty::core
