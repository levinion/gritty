#include "core/kitty_graphics.hpp"
#include <png.h>
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
        request.src_x = pending.src_x;
        request.src_y = pending.src_y;
        request.src_w = pending.src_w;
        request.src_h = pending.src_h;
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
                case 'i': request.image_id = static_cast<uint32_t>(int_val); request.has_image_id = true; break;
                case 'p': request.placement_id = static_cast<uint32_t>(int_val); break;
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
                case 'd': if (!val.empty()) request.delete_target = val[0]; break;
                case 'x': request.src_x = int_val; break;
                case 'y': request.src_y = int_val; break;
                case 'w': request.src_w = int_val; break;
                case 'h': request.src_h = int_val; break;
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
    if (request.delete_target == 'i' || request.delete_target == 'I') {
        if (request.has_image_id) delete_image(request.image_id);
    } else if (request.delete_target == 'p' || request.delete_target == 'P') {
        if (request.has_image_id) {
            if (request.delete_target == 'P') delete_image(request.image_id);
            else delete_placements(KittyPlacement{.image_id = request.image_id,
                                                  .placement_id = request.placement_id});
        }
    } else if (request.delete_target == 'c' || request.delete_target == 'C') {
        std::vector<uint32_t> affected;
        for (const auto& p : placements_) {
            if (cursor_col >= p.col && cursor_col < p.col + p.cols &&
                cursor_row >= p.row && cursor_row < p.row + p.rows)
                affected.push_back(p.image_id);
        }
        delete_placements_at(cursor_col, cursor_row);
        if (request.delete_target == 'C') for (const auto id : affected) delete_image(id);
    } else if (request.has_image_id && request.delete_target == 'a') {
        // a=d,i=N uses the default d=a but explicitly names an image.
        delete_image(request.image_id);
    } else if (request.delete_target == 'z' || request.delete_target == 'Z') {
        std::vector<uint32_t> affected;
        for (const auto& p : placements_) if (p.z_index == request.z_index) affected.push_back(p.image_id);
        delete_placements_by_z(request.z_index);
        if (request.delete_target == 'Z') for (const auto id : affected) delete_image(id);
    } else { // a/A: all visible placements.
        std::vector<uint32_t> affected;
        for (const auto& p : placements_) affected.push_back(p.image_id);
        clear_placements();
        if (request.delete_target == 'A') for (const auto id : affected) delete_image(id);
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
        static_cast<size_t>(st.st_size) <= max_memory_) {
        mapping = util::MappedBuffer(fd.get(), static_cast<size_t>(st.st_size), 0);
        if (mapping.valid()) {
            source = {static_cast<const uint8_t*>(mapping.data()), mapping.size()};
            loaded = true;
        }
    }
    if (request.medium == KittyMedium::TempFile) ::unlink(canonical.c_str());
    return loaded && !source.empty();
}

bool KittyGraphicsEngine::store_image(const KittyRequest& request, std::span<const uint8_t> source) {
    auto img_opt = decode_image(source, request.format, request.width, request.height,
                                request.compressed);
    if (!img_opt) return false;

    const size_t bytes = img_opt->height() * img_opt->stride();
    while (current_memory_ + bytes > max_memory_ && !images_.empty()) {
        auto first_it = images_.begin();
        current_memory_ -= first_it->second.byte_size;
        images_.erase(first_it);
    }
    if (auto existing = images_.find(request.image_id); existing != images_.end())
        current_memory_ -= existing->second.byte_size;
    images_[request.image_id] = KittyImage{
        .id = request.image_id,
        .image = std::move(*img_opt),
        .byte_size = bytes
    };
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

    if (request.more_data == 1) {
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
            .src_x = request.src_x,
            .src_y = request.src_y,
            .src_w = request.src_w,
            .src_h = request.src_h,
            .buffer = std::move(accumulated),
            .in_progress = true,
        };
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

    bool image_ready = images_.find(request.image_id) != images_.end();
    if (!image_ready) {
        std::vector<uint8_t> decoded;
        util::MappedBuffer mapping;
        std::span<const uint8_t> source;
        image_ready = load_source(request, full_payload, decoded, mapping, source) &&
                      store_image(request, source);
    }

    if (!image_ready) return reply("ENOENT:failed to load image");

    if (request.unicode_placeholder == 1) {
        auto existing = std::find_if(virtual_placements_.begin(), virtual_placements_.end(),
            [&](const KittyVirtualPlacement& vp) {
                return vp.image_id == request.image_id && vp.placement_id == request.placement_id;
            });
        if (existing != virtual_placements_.end()) {
            existing->cols = request.cols;
            existing->rows = request.rows;
            existing->z_index = request.z_index;
        } else {
            virtual_placements_.push_back(KittyVirtualPlacement{
                request.image_id, request.placement_id, request.cols, request.rows, request.z_index});
        }
        return reply("OK");
    }

    if (request.action == KittyAction::TransmitAndDisplay || request.action == KittyAction::Put) {
        KittyPlacement placement{
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
            .cursor_policy = request.cursor_policy,
        };
        if (placement_handler_) placement_handler_(placement, images_.at(request.image_id).image.get());
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
    if (images_.empty()) release_temporary_buffers();
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
    if (images_.empty() && placements_.empty()) release_temporary_buffers();
}

void KittyGraphicsEngine::clear() {
    virtual_placements_.clear();
    images_.clear();
    placements_.clear();
    current_memory_ = 0;
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
