#include "core/kitty_graphics.hpp"
#include <png.h>
#include <zlib.h>
#include <algorithm>
#include <cstring>
#include <charconv>

namespace squirt::core {

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
    const std::vector<uint8_t>& raw_data,
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
    KittyFormat format = pending_chunk_.in_progress ? pending_chunk_.format : KittyFormat::Rgba;
    bool compressed = pending_chunk_.in_progress ? pending_chunk_.compressed : false;
    uint32_t image_id = pending_chunk_.in_progress ? pending_chunk_.image_id : 0;
    uint32_t placement_id = pending_chunk_.in_progress ? pending_chunk_.placement_id : 0;
    int width = pending_chunk_.in_progress ? pending_chunk_.width : 0;
    int height = pending_chunk_.in_progress ? pending_chunk_.height : 0;
    int cols = pending_chunk_.in_progress ? pending_chunk_.cols : 0;
    int rows = pending_chunk_.in_progress ? pending_chunk_.rows : 0;
    int z_index = pending_chunk_.in_progress ? pending_chunk_.z_index : 0;
    int more_data = 0;

    // Parse comma-separated key=val
    size_t pos = 0;
    while (pos < control_part.size()) {
        size_t next_comma = control_part.find(',', pos);
        std::string_view kv = (next_comma != std::string::npos)
            ? control_part.substr(pos, next_comma - pos)
            : control_part.substr(pos);

        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            char key = kv[0];
            std::string_view val = kv.substr(eq + 1);
            int int_val = 0;
            std::from_chars(val.data(), val.data() + val.size(), int_val);

            switch (key) {
                case 'a': action = static_cast<KittyAction>(val[0]); break;
                case 'f': format = static_cast<KittyFormat>(int_val); break;
                case 'o': compressed = (val == "z"); break;
                case 'i': image_id = static_cast<uint32_t>(int_val); break;
                case 'p': placement_id = static_cast<uint32_t>(int_val); break;
                case 's': width = int_val; break;
                case 'v': height = int_val; break;
                case 'c': cols = int_val; break;
                case 'r': rows = int_val; break;
                case 'z': z_index = int_val; break;
                case 'm': more_data = int_val; break;
                default: break;
            }
        }

        if (next_comma == std::string::npos) break;
        pos = next_comma + 1;
    }

    if (image_id == 0) {
        image_id = next_auto_id_++;
    }

    if (action == KittyAction::Query) {
        return "\x1b_Gi=" + std::to_string(image_id) + ";OK\x1b\\";
    }

    if (action == KittyAction::Delete) {
        delete_image(image_id);
        return "\x1b_Gi=" + std::to_string(image_id) + ";OK\x1b\\";
    }

    if (more_data == 1) {
        // Start or continue chunked transmission
        pending_chunk_.in_progress = true;
        pending_chunk_.image_id = image_id;
        pending_chunk_.placement_id = placement_id;
        pending_chunk_.action = action;
        pending_chunk_.format = format;
        pending_chunk_.compressed = compressed;
        pending_chunk_.width = width;
        pending_chunk_.height = height;
        pending_chunk_.cols = cols;
        pending_chunk_.rows = rows;
        pending_chunk_.z_index = z_index;
        pending_chunk_.buffer.append(data_part);
        return ""; // Wait for next chunk
    }

    // Final chunk (m=0) or single transmission
    std::string full_payload;
    if (pending_chunk_.in_progress) {
        pending_chunk_.buffer.append(data_part);
        full_payload = std::move(pending_chunk_.buffer);
        pending_chunk_ = PendingChunkTransmission{};
    } else {
        full_payload = std::string(data_part);
    }

    std::vector<uint8_t> decoded_data = decode_base64(full_payload);

    if (!decoded_data.empty()) {
        auto img_opt = decode_image(decoded_data, format, width, height, compressed);
        if (img_opt) {
            size_t bytes = img_opt->height() * img_opt->stride();
            while (current_memory_ + bytes > max_memory_ && !images_.empty()) {
                auto first_it = images_.begin();
                current_memory_ -= first_it->second.byte_size;
                images_.erase(first_it);
            }
            images_[image_id] = KittyImage{
                .id = image_id,
                .image = std::move(*img_opt),
                .byte_size = bytes
            };
            current_memory_ += bytes;
        }
    }

    // Display placement
    if (action == KittyAction::TransmitAndDisplay || action == KittyAction::Put) {
        placements_.push_back(KittyPlacement{
            .image_id = image_id,
            .placement_id = placement_id,
            .col = cursor_col,
            .row = cursor_row,
            .cols = cols > 0 ? cols : 1,
            .rows = rows > 0 ? rows : 1,
            .z_index = z_index
        });
    }

    return "\x1b_Gi=" + std::to_string(image_id) + ";OK\x1b\\";
}

const KittyImage* KittyGraphicsEngine::get_image(uint32_t image_id) const noexcept {
    auto it = images_.find(image_id);
    return (it != images_.end()) ? &it->second : nullptr;
}

void KittyGraphicsEngine::delete_placements_at(int col, int row) {
    placements_.erase(
        std::remove_if(placements_.begin(), placements_.end(),
            [col, row](const KittyPlacement& p) {
                return p.col == col && p.row == row;
            }),
        placements_.end());
}

void KittyGraphicsEngine::delete_image(uint32_t image_id) {
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
    images_.clear();
    placements_.clear();
    current_memory_ = 0;
}

} // namespace squirt::core
