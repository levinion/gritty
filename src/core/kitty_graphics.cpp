#include "core/kitty_graphics.hpp"
#include <png.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <charconv>
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include "util/base64.hpp"
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
constexpr int kMaxKittyImageDim = 16384;

pixman_image_t* decode_png_raw(std::span<const uint8_t> data, size_t max_memory) {
    PngMemReader reader{.data = data.data(), .size = data.size(), .offset = 0};
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) return nullptr;

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return nullptr;
    }

    pixman_image_t* volatile raw_img = nullptr;
    png_bytep* volatile rows = nullptr;
    if (setjmp(png_jmpbuf(png))) {
        std::free(rows);
        if (raw_img) pixman_image_unref(raw_img);
        png_destroy_read_struct(&png, &info, nullptr);
        return nullptr;
    }

    png_set_read_fn(png, &reader, png_mem_read_fn);
    png_read_info(png, info);

    const int img_w = static_cast<int>(png_get_image_width(png, info));
    const int img_h = static_cast<int>(png_get_image_height(png, info));
    if (img_w <= 0 || img_h <= 0 || img_w > kMaxKittyImageDim || img_h > kMaxKittyImageDim ||
        static_cast<uint64_t>(img_w) * static_cast<uint64_t>(img_h) * 4 > max_memory) {
        png_destroy_read_struct(&png, &info, nullptr);
        return nullptr;
    }

    const png_byte color_type = png_get_color_type(png, info);
    const png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if ((color_type & PNG_COLOR_MASK_ALPHA) == 0 && !png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    }

    png_set_bgr(png); // Convert to BGRA host order for PIXMAN_a8r8g8b8
    png_read_update_info(png, info);

    const int stride = img_w * 4;
    if (png_get_rowbytes(png, info) != static_cast<size_t>(stride)) {
        png_destroy_read_struct(&png, &info, nullptr);
        return nullptr;
    }
    raw_img = pixman_image_create_bits(PIXMAN_a8r8g8b8, img_w, img_h, nullptr, stride);
    if (!raw_img) {
        png_destroy_read_struct(&png, &info, nullptr);
        return nullptr;
    }

    uint32_t* dst_data = pixman_image_get_data(raw_img);
    rows = static_cast<png_bytep*>(std::malloc(static_cast<size_t>(img_h) * sizeof(png_bytep)));
    if (!rows) {
        pixman_image_unref(raw_img);
        raw_img = nullptr;
        png_destroy_read_struct(&png, &info, nullptr);
        return nullptr;
    }
    for (int y = 0; y < img_h; ++y) {
        rows[y] = reinterpret_cast<png_bytep>(dst_data + static_cast<size_t>(y) * img_w);
    }

    png_read_image(png, rows);
    png_read_end(png, nullptr);
    std::free(rows);
    rows = nullptr;
    png_destroy_read_struct(&png, &info, nullptr);
    return raw_img;
}

bool path_is_under(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto p = path.lexically_normal().string();
    const auto r = root.lexically_normal().string();
    return p == r || (p.size() > r.size() && p.starts_with(r) &&
        (r.empty() || r.back() == '/' || p[r.size()] == '/'));
}
} // namespace

KittyGraphicsEngine::KittyGraphicsEngine(size_t max_memory_bytes)
    : max_memory_(max_memory_bytes) {}

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
        if (raw_data.size() > std::numeric_limits<uInt>::max()) return std::nullopt;
        z_stream zs{};
        if (inflateInit(&zs) != Z_OK) return std::nullopt;
        zs.next_in = const_cast<Bytef*>(raw_data.data());
        zs.avail_in = static_cast<uInt>(raw_data.size());
        std::array<uint8_t, 65536> chunk{};
        int res = Z_OK;
        do {
            zs.next_out = chunk.data();
            zs.avail_out = static_cast<uInt>(chunk.size());
            res = inflate(&zs, Z_NO_FLUSH);
            if (res != Z_OK && res != Z_STREAM_END) {
                inflateEnd(&zs);
                return std::nullopt;
            }
            const size_t have = chunk.size() - zs.avail_out;
            if (decompressed.size() + have > max_memory_) {
                inflateEnd(&zs);
                return std::nullopt;
            }
            decompressed.insert(decompressed.end(), chunk.data(), chunk.data() + have);
        } while (res != Z_STREAM_END);
        inflateEnd(&zs);
        data_ptr = decompressed.data();
        data_len = decompressed.size();
    }

    if (format == KittyFormat::Png) {
        pixman_image_t* raw_img = decode_png_raw(
            std::span<const uint8_t>(data_ptr, data_len), max_memory_);
        if (!raw_img) return std::nullopt;
        util::PixmanImage img(raw_img);
        premultiply_argb(img.data(), img.width() * img.height());
        return img;
    }

    if (width <= 0 || height <= 0 || width > kMaxKittyImageDim || height > kMaxKittyImageDim ||
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4 > max_memory_) {
        return std::nullopt;
    }

    const int stride = width * 4;
    pixman_image_t* raw_img = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, width, height, nullptr, stride);
    if (!raw_img) return std::nullopt;

    util::PixmanImage img(raw_img);
    uint32_t* dst = img.data();

    if (format == KittyFormat::Rgba) {
        if (data_len < static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4) return std::nullopt;
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
        if (data_len < static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 3) return std::nullopt;
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

KittyGraphicsEngine::KittyRequest KittyGraphicsEngine::parse_control(
    std::string_view control_part,
    const PendingChunkTransmission& pending
) {
    KittyRequest request;
    if (pending.in_progress) {
        request.action = pending.action;
        request.medium = pending.medium;
        request.format = pending.format;
        request.compressed = pending.compressed;
        request.image_id = pending.image_id;
        request.has_image_id = true;
        request.placement_id = pending.placement_id;
        request.width = pending.width;
        request.height = pending.height;
        request.cols = pending.cols;
        request.rows = pending.rows;
        request.z_index = pending.z_index;
        request.unicode_placeholder = pending.unicode_placeholder;
        request.quiet = pending.quiet;
        request.cursor_policy = pending.cursor_policy;
        request.delete_target = pending.delete_target;
        request.has_delete_target = pending.has_delete_target;
        request.src_x = pending.src_x;
        request.src_y = pending.src_y;
        request.src_w = pending.src_w;
        request.src_h = pending.src_h;
        request.x_offset = pending.x_offset;
        request.y_offset = pending.y_offset;
        request.file_size = pending.file_size;
        request.file_offset = pending.file_offset;
    }

    size_t pos = 0;
    while (pos < control_part.size()) {
        const size_t next_comma = control_part.find(',', pos);
        const std::string_view kv = (next_comma != std::string::npos)
            ? control_part.substr(pos, next_comma - pos)
            : control_part.substr(pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && eq > 0) {
            const char key = kv[0];
            const std::string_view val = kv.substr(eq + 1);
            int int_val = 0;
            std::from_chars(val.data(), val.data() + val.size(), int_val);
            switch (key) {
                case 'a': if (!val.empty()) request.action = static_cast<KittyAction>(val[0]); break;
                case 'f': request.format = static_cast<KittyFormat>(int_val); break;
                case 'o': request.compressed = (val == "z"); break;
                case 'i': {
                    uint32_t u32 = 0;
                    const auto [end, ec] = std::from_chars(val.data(), val.data() + val.size(), u32);
                    if (ec == std::errc{} && end == val.data() + val.size()) {
                        request.image_id = u32;
                        request.has_image_id = true;
                    }
                    break;
                }
                case 'p': {
                    uint32_t u32 = 0;
                    const auto [end, ec] = std::from_chars(val.data(), val.data() + val.size(), u32);
                    if (ec == std::errc{} && end == val.data() + val.size())
                        request.placement_id = u32;
                    break;
                }
                case 's': request.width = int_val; break;
                case 'v': request.height = int_val; break;
                case 'c': request.cols = int_val; break;
                case 'r': request.rows = int_val; break;
                case 'z': request.z_index = int_val; break;
                case 'm': request.more_data = int_val; break;
                case 'U': request.unicode_placeholder = int_val; break;
                case 'q': request.quiet = int_val; break;
                case 'e': request.feature = val.empty() ? 0 : val[0]; break;
                case 'C': request.cursor_policy = int_val; break;
                case 't': if (!val.empty()) request.medium = static_cast<KittyMedium>(val[0]); break;
                case 'd':
                    if (!val.empty()) {
                        request.delete_target = val[0];
                        request.has_delete_target = true;
                    }
                    break;
                case 'x': request.src_x = int_val; break;
                case 'y': request.src_y = int_val; break;
                case 'w': request.src_w = int_val; break;
                case 'h': request.src_h = int_val; break;
                case 'X': request.x_offset = int_val; break;
                case 'Y': request.y_offset = int_val; break;
                case 'S': {
                    int64_t i64 = 0;
                    const auto [end, ec] = std::from_chars(val.data(), val.data() + val.size(), i64);
                    if (ec == std::errc{} && end == val.data() + val.size())
                        request.file_size = i64;
                    break;
                }
                case 'O': {
                    int64_t i64 = 0;
                    const auto [end, ec] = std::from_chars(val.data(), val.data() + val.size(), i64);
                    if (ec == std::errc{} && end == val.data() + val.size())
                        request.file_offset = i64;
                    break;
                }
                default: break;
            }
        }
        if (next_comma == std::string::npos) break;
        pos = next_comma + 1;
    }
    return request;
}

bool KittyGraphicsEngine::handle_delete(
    const KittyRequest& request, int cursor_col, int cursor_row
) {
    if (request.action != KittyAction::Delete) return false;

    // Delete must happen before auto-id allocation: a=d without i= targets all
    // visible placements (or the requested d= class), not a newly minted image.
    // Bare `a=d,i=N` without `d=` deletes the image (compatible with direct callers).
    if (!request.has_delete_target && request.has_image_id) {
        delete_image(request.image_id);
        return true;
    }

    auto notify_virtual = [&]() {
        if (virtual_placement_change_handler_) virtual_placement_change_handler_();
    };
    auto maybe_delete_unreferenced_image = [&](uint32_t image_id) {
        const bool has_p = std::any_of(placements_.begin(), placements_.end(),
            [&](const KittyPlacement& p) { return p.image_id == image_id; });
        const bool has_vp = std::any_of(virtual_placements_.begin(), virtual_placements_.end(),
            [&](const KittyVirtualPlacement& vp) { return vp.image_id == image_id; });
        if (!has_p && !has_vp) delete_image(image_id);
    };
    auto delete_placement_and_maybe_image = [&](uint32_t image_id, uint32_t placement_id) {
        if (placement_id == 0) {
            delete_image(image_id);
            return;
        }
        delete_placements(KittyPlacement{.image_id = image_id, .placement_id = placement_id});
        const size_t erased_vp = std::erase_if(virtual_placements_, [&](const KittyVirtualPlacement& vp) {
            return vp.image_id == image_id && vp.placement_id == placement_id;
        });
        if (erased_vp > 0) notify_virtual();
        maybe_delete_unreferenced_image(image_id);
    };
    auto delete_by_predicate = [&](auto pred, bool delete_img) {
        std::vector<uint32_t> affected;
        for (const auto& p : placements_) {
            if (pred(p)) {
                if (placement_delete_handler_) placement_delete_handler_(p);
                if (delete_img) affected.push_back(p.image_id);
            }
        }
        std::erase_if(placements_, pred);
        if (delete_img) {
            for (const auto id : affected) maybe_delete_unreferenced_image(id);
        } else if (images_.empty() && placements_.empty() && virtual_placements_.empty()) {
            release_temporary_buffers();
        }
    };

    if (request.delete_target == 'i') {
        // Lowercase d=i deletes placements for i= (optionally narrowed by p=)
        // while keeping cached image data in memory for subsequent a=p calls.
        if (request.has_image_id) {
            delete_placements(KittyPlacement{.image_id = request.image_id,
                                              .placement_id = request.placement_id});
            const size_t erased = std::erase_if(virtual_placements_, [&](const KittyVirtualPlacement& vp) {
                if (vp.image_id != request.image_id) return false;
                return request.placement_id == 0 || vp.placement_id == request.placement_id;
            });
            if (erased > 0) notify_virtual();
        }
    } else if (request.delete_target == 'I') {
        if (request.has_image_id) delete_placement_and_maybe_image(request.image_id, request.placement_id);
    } else if (request.delete_target == 'p' || request.delete_target == 'P') {
        const bool drop_img = request.delete_target == 'P';
        if (request.has_image_id) {
            if (drop_img) delete_placement_and_maybe_image(request.image_id, request.placement_id);
            else {
                delete_placements(KittyPlacement{.image_id = request.image_id,
                                                  .placement_id = request.placement_id});
                const size_t erased = std::erase_if(virtual_placements_, [&](const KittyVirtualPlacement& vp) {
                    if (vp.image_id != request.image_id) return false;
                    return request.placement_id == 0 || vp.placement_id == request.placement_id;
                });
                if (erased > 0) notify_virtual();
            }
        } else {
            // Point cell target (1-based X, Y in spec, defaulting to 1).
            const int col = std::max(0, request.src_x - 1);
            const int row = std::max(0, request.src_y - 1);
            delete_by_predicate([this, col, row](const KittyPlacement& p) {
                const int w = std::max(1, p.cols);
                const int h = std::max(1, p.rows);
                return p.alt_screen == active_alt_screen_ &&
                       col >= p.col && col < p.col + w && row >= p.row && row < p.row + h;
            }, drop_img);
        }
    } else if (request.delete_target == 'q' || request.delete_target == 'Q') {
        const bool drop_img = request.delete_target == 'Q';
        const int col = std::max(0, request.src_x - 1);
        const int row = std::max(0, request.src_y - 1);
        const int z = request.z_index;
        delete_by_predicate([this, col, row, z](const KittyPlacement& p) {
            const int w = std::max(1, p.cols);
            const int h = std::max(1, p.rows);
            return p.alt_screen == active_alt_screen_ && p.z_index == z &&
                   col >= p.col && col < p.col + w && row >= p.row && row < p.row + h;
        }, drop_img);
    } else if (request.delete_target == 'x' || request.delete_target == 'X') {
        const bool drop_img = request.delete_target == 'X';
        const int col = std::max(0, request.src_x - 1);
        delete_by_predicate([this, col](const KittyPlacement& p) {
            return p.alt_screen == active_alt_screen_ &&
                   col >= p.col && col < p.col + std::max(1, p.cols);
        }, drop_img);
    } else if (request.delete_target == 'y' || request.delete_target == 'Y') {
        const bool drop_img = request.delete_target == 'Y';
        const int row = std::max(0, request.src_y - 1);
        delete_by_predicate([this, row](const KittyPlacement& p) {
            return p.alt_screen == active_alt_screen_ &&
                   row >= p.row && row < p.row + std::max(1, p.rows);
        }, drop_img);
    } else if (request.delete_target == 'r' || request.delete_target == 'R') {
        const bool drop_img = request.delete_target == 'R';
        const uint32_t id_lo = static_cast<uint32_t>(std::max(0, request.src_x));
        const uint32_t id_hi = static_cast<uint32_t>(std::max(0, request.src_y));
        if (id_lo <= id_hi) {
            const size_t erased_vp = std::erase_if(virtual_placements_, [&](const KittyVirtualPlacement& vp) {
                return vp.image_id >= id_lo && vp.image_id <= id_hi;
            });
            if (erased_vp > 0) notify_virtual();
            std::vector<uint32_t> img_ids;
            if (drop_img) {
                for (const auto& [id, _] : images_) {
                    if (id >= id_lo && id <= id_hi) img_ids.push_back(id);
                }
            }
            delete_by_predicate([id_lo, id_hi](const KittyPlacement& p) {
                return p.image_id >= id_lo && p.image_id <= id_hi;
            }, false);
            for (const auto id : img_ids) delete_image(id);
        }
    } else if (request.delete_target == 'c' || request.delete_target == 'C') {
        const bool drop_img = request.delete_target == 'C';
        delete_by_predicate([this, cursor_col, cursor_row](const KittyPlacement& p) {
            return p.alt_screen == active_alt_screen_ &&
                   cursor_col >= p.col && cursor_col < p.col + std::max(1, p.cols) &&
                   cursor_row >= p.row && cursor_row < p.row + std::max(1, p.rows);
        }, drop_img);
    } else if (request.delete_target == 'z' || request.delete_target == 'Z') {
        const bool drop_img = request.delete_target == 'Z';
        const int z = request.z_index;
        std::vector<uint32_t> vp_affected;
        const size_t erased_vp = std::erase_if(virtual_placements_, [&](const KittyVirtualPlacement& vp) {
            if (vp.z_index != z) return false;
            if (drop_img) vp_affected.push_back(vp.image_id);
            return true;
        });
        if (erased_vp > 0) notify_virtual();
        delete_by_predicate([z](const KittyPlacement& p) { return p.z_index == z; }, drop_img);
        if (drop_img) {
            for (const auto id : vp_affected) maybe_delete_unreferenced_image(id);
        }
    } else if (request.delete_target == 'A') {
        std::vector<uint32_t> ids;
        ids.reserve(images_.size());
        for (const auto& [id, _] : images_) ids.push_back(id);
        clear_placements();
        for (const auto id : ids) delete_image(id);
    } else { // d=a: all visible placements, keep image cache.
        clear_placements();
    }
    return true;
}

bool KittyGraphicsEngine::load_source(
    const KittyRequest& request,
    std::string_view payload,
    std::vector<uint8_t>& decoded,
    util::MappedBuffer& mapping,
    std::span<const uint8_t>& source
) const {
    auto decoded_opt = util::decode_base64(payload, util::Base64Mode::Relaxed, max_memory_);
    if (!decoded_opt) return false;
    decoded = std::move(*decoded_opt);

    if (request.medium != KittyMedium::File && request.medium != KittyMedium::TempFile) {
        source = decoded;
        return !source.empty();
    }

    const std::string path(reinterpret_cast<const char*>(decoded.data()), decoded.size());
    if (path.empty() || path.find('\0') != std::string::npos) return false;

    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    if (ec || canonical.empty() || !std::filesystem::is_regular_file(canonical, ec) || ec) return false;

    if (request.medium == KittyMedium::TempFile) {
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
        if (!allowed) return false;
    }

    const util::UniqueFd fd(::open(canonical.c_str(), O_RDONLY | O_CLOEXEC));
    struct stat st{};
    bool loaded = false;
    if (fd.valid() && ::fstat(fd.get(), &st) == 0 && st.st_size > 0 &&
        request.file_offset >= 0 && request.file_size >= 0 &&
        request.file_offset < st.st_size) {
        const size_t avail = static_cast<size_t>(st.st_size - request.file_offset);
        const size_t slice_len = request.file_size > 0
            ? std::min(avail, static_cast<size_t>(request.file_size))
            : avail;
        if (slice_len > 0 && slice_len <= max_memory_) {
            const long raw_page = ::sysconf(_SC_PAGESIZE);
            const off_t page_sz = raw_page > 0 ? static_cast<off_t>(raw_page) : 4096;
            const off_t aligned = (static_cast<off_t>(request.file_offset) / page_sz) * page_sz;
            const size_t delta = static_cast<size_t>(static_cast<off_t>(request.file_offset) - aligned);
            mapping = util::MappedBuffer(fd.get(), delta + slice_len, aligned);
            if (mapping.valid()) {
                source = {static_cast<const uint8_t*>(mapping.data()) + delta, slice_len};
                loaded = true;
            }
        }
    }
    if (request.medium == KittyMedium::TempFile) ::unlink(canonical.c_str());
    return loaded && !source.empty();
}

bool KittyGraphicsEngine::store_image(const KittyRequest& request, std::span<const uint8_t> source) {
    auto img_opt = decode_image(source, request.format, request.width, request.height,
                                request.compressed);
    if (!img_opt) return false;

    const size_t bytes = static_cast<size_t>(img_opt->height()) * static_cast<size_t>(img_opt->stride());
    if (bytes > max_memory_) return false;
    if (auto existing = images_.find(request.image_id); existing != images_.end()) {
        image_lru_.erase(existing->second.lru_it);
        current_memory_ -= existing->second.byte_size;
        images_.erase(existing);
    }
    while (current_memory_ + bytes > max_memory_ && !image_lru_.empty()) {
        delete_image(image_lru_.front());
    }
    image_lru_.push_back(request.image_id);
    images_.emplace(request.image_id, KittyImage{
        .id = request.image_id,
        .image = std::move(*img_opt),
        .byte_size = bytes,
        .lru_it = std::prev(image_lru_.end()),
    });
    current_memory_ += bytes;
    return true;
}

std::string KittyGraphicsEngine::process_apc_sequence(
    std::string_view payload,
    int cursor_col,
    int cursor_row
) {
    if (!payload.starts_with('G')) return "";
    payload.remove_prefix(1);

    const size_t semicolon = payload.find(';');
    const std::string_view control_part = (semicolon != std::string::npos)
        ? payload.substr(0, semicolon) : payload;
    const std::string_view data_part = (semicolon != std::string::npos)
        ? payload.substr(semicolon + 1) : "";

    KittyRequest request = parse_control(control_part, pending_chunk_);
    auto reply = [&](std::string_view status) {
        if (request.quiet >= 2) return std::string{};
        if (request.quiet >= 1 && status == "OK") return std::string{};
        return "\x1b_Gi=" + std::to_string(request.image_id) + ";" + std::string(status) + "\x1b\\";
    };

    if (handle_delete(request, cursor_col, cursor_row)) return reply("OK");
    if (request.image_id == 0) request.image_id = next_auto_id_++;

    if (request.action == KittyAction::Query) {
        if (request.medium == KittyMedium::SharedMem)
            return reply("ENOTSUP:shared memory not supported");
        if (request.quiet >= 2) return "";
        std::string response = "\x1b_Gi=" + std::to_string(request.image_id);
        if (request.feature == 'T') {
            response += ",e=T";
            if (request.unicode_placeholder == 1) response += ",U=1";
            if (request.cursor_policy == 1) response += ",C=1";
        }
        return response + ";OK\x1b\\";
    }

    const size_t max_b64 = ((max_memory_ + 2) / 3) * 4 + 4096;
    if (request.more_data == 1) {
        const size_t current_len = pending_chunk_.in_progress ? pending_chunk_.buffer.size() : 0;
        if (current_len + data_part.size() > max_b64) {
            pending_chunk_ = PendingChunkTransmission{};
            return reply("EFBIG:chunk payload too large");
        }
        // Later chunks usually omit metadata. Keep the first chunk's parsed
        // values and append payload bytes instead of replacing the buffer.
        std::string accumulated;
        if (pending_chunk_.in_progress) accumulated = std::move(pending_chunk_.buffer);
        accumulated.append(data_part);
        pending_chunk_ = PendingChunkTransmission{
            .image_id = request.image_id,
            .placement_id = request.placement_id,
            .action = request.action,
            .medium = request.medium,
            .format = request.format,
            .compressed = request.compressed,
            .width = request.width,
            .height = request.height,
            .cols = request.cols,
            .rows = request.rows,
            .z_index = request.z_index,
            .unicode_placeholder = request.unicode_placeholder,
            .quiet = request.quiet,
            .cursor_policy = request.cursor_policy,
            .delete_target = request.delete_target,
            .has_delete_target = request.has_delete_target,
            .src_x = request.src_x,
            .src_y = request.src_y,
            .src_w = request.src_w,
            .src_h = request.src_h,
            .x_offset = request.x_offset,
            .y_offset = request.y_offset,
            .file_size = request.file_size,
            .file_offset = request.file_offset,
            .buffer = std::move(accumulated),
            .in_progress = true,
        };
        return "";
    }

    std::string full_payload;
    if (pending_chunk_.in_progress) {
        if (pending_chunk_.buffer.size() + data_part.size() > max_b64) {
            pending_chunk_ = PendingChunkTransmission{};
            return reply("EFBIG:chunk payload too large");
        }
        pending_chunk_.buffer.append(data_part);
        full_payload = std::move(pending_chunk_.buffer);
        pending_chunk_ = PendingChunkTransmission{};
    } else {
        full_payload = std::string(data_part);
    }

    const bool has_transmission =
        (request.action == KittyAction::Transmit || request.action == KittyAction::TransmitAndDisplay) &&
        !full_payload.empty();
    auto existing_it = images_.find(request.image_id);
    bool image_ready = existing_it != images_.end();
    if (has_transmission || !image_ready) {
        std::vector<uint8_t> decoded;
        util::MappedBuffer mapping;
        std::span<const uint8_t> source;
        image_ready = load_source(request, full_payload, decoded, mapping, source) &&
                      store_image(request, source);
        existing_it = images_.find(request.image_id);
    } else if (existing_it != images_.end()) {
        image_lru_.splice(image_lru_.end(), image_lru_, existing_it->second.lru_it);
    }

    if (!image_ready || existing_it == images_.end()) return reply("ENOENT:failed to load image");

    if (request.unicode_placeholder == 1) {
        auto existing = std::find_if(virtual_placements_.begin(), virtual_placements_.end(),
            [&](const KittyVirtualPlacement& vp) {
                return vp.image_id == request.image_id && vp.placement_id == request.placement_id;
            });
        if (existing != virtual_placements_.end()) {
            existing->cols = request.cols;
            existing->rows = request.rows;
            existing->z_index = request.z_index;
            existing->src_x = request.src_x;
            existing->src_y = request.src_y;
            existing->src_w = request.src_w;
            existing->src_h = request.src_h;
        } else {
            virtual_placements_.push_back(KittyVirtualPlacement{
                request.image_id, request.placement_id, request.cols, request.rows, request.z_index,
                request.src_x, request.src_y, request.src_w, request.src_h});
        }
        if (virtual_placement_change_handler_) virtual_placement_change_handler_();
        return reply("OK");
    }

    if (request.action == KittyAction::TransmitAndDisplay || request.action == KittyAction::Put) {
        if (request.placement_id != 0) {
            delete_placements(KittyPlacement{.image_id = request.image_id,
                                              .placement_id = request.placement_id});
        }
        KittyPlacement placement{
            .uid = next_placement_uid_++,
            .image_id = request.image_id,
            .placement_id = request.placement_id,
            .col = cursor_col,
            .row = cursor_row,
            .cols = request.cols > 0 ? request.cols : 0,
            .rows = request.rows > 0 ? request.rows : 0,
            .z_index = request.z_index,
            .src_x = request.src_x,
            .src_y = request.src_y,
            .src_w = request.src_w,
            .src_h = request.src_h,
            .x_offset = request.x_offset,
            .y_offset = request.y_offset,
            .cursor_policy = request.cursor_policy,
            .alt_screen = active_alt_screen_,
        };
        if (placement_handler_) placement_handler_(placement, existing_it->second.image.get());
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

void KittyGraphicsEngine::scroll_placements(int top, int bottom, int rows, bool alt_screen) noexcept {
    if (placements_.empty() || rows == 0) return;
    std::vector<KittyPlacement> kept;
    kept.reserve(placements_.size());
    for (auto& p : placements_) {
        const int eff_rows = std::max(1, p.rows);
        if (p.alt_screen != alt_screen || p.row + eff_rows <= top || p.row > bottom) {
            kept.push_back(p);
            continue;
        }
        p.row -= rows;
        if (p.row + eff_rows > top && p.row <= bottom) {
            kept.push_back(p);
        }
    }
    placements_ = std::move(kept);
}

void KittyGraphicsEngine::clear_placements_for_screen(bool alt_screen) {
    for (const auto& p : placements_) {
        if (p.alt_screen == alt_screen && placement_delete_handler_) {
            placement_delete_handler_(p);
        }
    }
    std::erase_if(placements_, [alt_screen](const KittyPlacement& p) {
        return p.alt_screen == alt_screen;
    });
    if (images_.empty() && placements_.empty() && virtual_placements_.empty()) {
        release_temporary_buffers();
    }
}

void KittyGraphicsEngine::clear_placements() {
    if (placement_delete_handler_) for (const auto& p : placements_) placement_delete_handler_(p);
    const bool had_virtual = !virtual_placements_.empty();
    placements_.clear();
    virtual_placements_.clear();
    if (had_virtual && virtual_placement_change_handler_) virtual_placement_change_handler_();
    if (images_.empty()) release_temporary_buffers();
}

void KittyGraphicsEngine::delete_image(uint32_t image_id) {
    if (delete_handler_) delete_handler_(image_id);
    const size_t erased_vp = std::erase_if(virtual_placements_,
        [image_id](const KittyVirtualPlacement& vp) { return vp.image_id == image_id; });
    if (erased_vp > 0 && virtual_placement_change_handler_) virtual_placement_change_handler_();
    auto it = images_.find(image_id);
    if (it != images_.end()) {
        image_lru_.erase(it->second.lru_it);
        current_memory_ -= it->second.byte_size;
        images_.erase(it);
    }
    placements_.erase(
        std::remove_if(placements_.begin(), placements_.end(),
            [image_id](const KittyPlacement& p) {
                return p.image_id == image_id;
            }),
        placements_.end());
    if (images_.empty() && placements_.empty()) release_temporary_buffers();
}

void KittyGraphicsEngine::clear() {
    const bool had_virtual = !virtual_placements_.empty();
    virtual_placements_.clear();
    image_lru_.clear();
    images_.clear();
    placements_.clear();
    current_memory_ = 0;
    if (had_virtual && virtual_placement_change_handler_) virtual_placement_change_handler_();
    release_temporary_buffers();
}

void KittyGraphicsEngine::release_temporary_buffers() {
    std::string empty;
    pending_chunk_.buffer.swap(empty);
    if (!images_.empty() || !placements_.empty() || !virtual_placements_.empty()) return;
    placements_.shrink_to_fit();
    virtual_placements_.shrink_to_fit();
    decltype(images_) fresh;
    images_.swap(fresh);
}

} // namespace gritty::core
