#include "core/sixel_decoder.hpp"
#include <algorithm>
#include <charconv>

namespace gritty::core {

std::optional<util::PixmanImage> SixelDecoder::decode(std::string_view stream) {
    if (stream.empty()) return std::nullopt;

    // Default 16-color Sixel palette
    std::array<uint32_t, 256> palette{};
    static constexpr uint32_t kDefaultSixelColors[16] = {
        0x000000, 0x3333cc, 0xcc2121, 0x33cc33,
        0xcc33cc, 0x33cccc, 0xcccc33, 0x878787,
        0x424242, 0x5454ff, 0xff5454, 0x54ff54,
        0xff54ff, 0x54ffff, 0xffff54, 0xffffff
    };
    for (size_t i = 0; i < 16; ++i) {
        palette[i] = 0xFF000000 | kDefaultSixelColors[i];
    }

    int width = 0;
    int max_width = 0;
    int current_band_y = 0;
    int active_color_idx = 0;

    std::vector<uint32_t> pixel_grid; // Linear grid of width * height
    int grid_width = 256;
    int grid_height = 256;
    pixel_grid.resize(grid_width * grid_height, 0); // 0 = transparent

    auto ensure_size = [&](int req_w, int req_h) {
        if (req_w > grid_width || req_h > grid_height) {
            int new_w = std::max(grid_width, req_w);
            int new_h = std::max(grid_height, req_h);
            std::vector<uint32_t> new_grid(new_w * new_h, 0);
            for (int y = 0; y < grid_height; ++y) {
                for (int x = 0; x < grid_width; ++x) {
                    new_grid[y * new_w + x] = pixel_grid[y * grid_width + x];
                }
            }
            pixel_grid = std::move(new_grid);
            grid_width = new_w;
            grid_height = new_h;
        }
    };

    size_t i = 0;
    size_t len = stream.size();

    // Skip optional leading DCS 'q'
    if (stream[0] == 'q') i++;

    while (i < len) {
        char c = stream[i];

        if (c == '#') { // Color introducer
            i++;
            int col_idx = 0;
            size_t num_start = i;
            while (i < len && stream[i] >= '0' && stream[i] <= '9') i++;
            std::from_chars(stream.data() + num_start, stream.data() + i, col_idx);

            if (i < len && stream[i] == ';') { // Defining palette color: #n;2;r;g;b
                i++; // skip ';'
                int type = 0;
                num_start = i;
                while (i < len && stream[i] >= '0' && stream[i] <= '9') i++;
                std::from_chars(stream.data() + num_start, stream.data() + i, type);

                if (type == 2) { // RGB in percentage 0..100
                    int r = 0, g = 0, b = 0;
                    if (i < len && stream[i] == ';') { i++; num_start = i; while (i < len && stream[i] >= '0' && stream[i] <= '9') i++; std::from_chars(stream.data() + num_start, stream.data() + i, r); }
                    if (i < len && stream[i] == ';') { i++; num_start = i; while (i < len && stream[i] >= '0' && stream[i] <= '9') i++; std::from_chars(stream.data() + num_start, stream.data() + i, g); }
                    if (i < len && stream[i] == ';') { i++; num_start = i; while (i < len && stream[i] >= '0' && stream[i] <= '9') i++; std::from_chars(stream.data() + num_start, stream.data() + i, b); }

                    uint8_t r8 = static_cast<uint8_t>((r * 255) / 100);
                    uint8_t g8 = static_cast<uint8_t>((g * 255) / 100);
                    uint8_t b8 = static_cast<uint8_t>((b * 255) / 100);
                    if (col_idx >= 0 && col_idx < 256) {
                        palette[col_idx] = 0xFF000000 | (static_cast<uint32_t>(r8) << 16) | (static_cast<uint32_t>(g8) << 8) | b8;
                    }
                }
            } else {
                active_color_idx = col_idx & 0xFF;
            }
            continue;
        }

        if (c == '$') { // CR: return to start of line
            width = 0;
            i++;
            continue;
        }

        if (c == '-') { // NL: advance to next band of 6 pixels
            width = 0;
            current_band_y += 6;
            i++;
            continue;
        }

        if (c == '!') { // Repeat count: !<count><char>
            i++;
            int repeat = 1;
            size_t num_start = i;
            while (i < len && stream[i] >= '0' && stream[i] <= '9') i++;
            std::from_chars(stream.data() + num_start, stream.data() + i, repeat);

            if (i < len && stream[i] >= '?' && stream[i] <= '~') {
                uint8_t bits = static_cast<uint8_t>(stream[i] - '?');
                ensure_size(width + repeat, current_band_y + 6);
                uint32_t color = palette[active_color_idx];

                for (int r = 0; r < repeat; ++r) {
                    for (int bit = 0; bit < 6; ++bit) {
                        if (bits & (1 << bit)) {
                            pixel_grid[(current_band_y + bit) * grid_width + (width + r)] = color;
                        }
                    }
                }
                width += repeat;
                max_width = std::max(max_width, width);
                i++;
            }
            continue;
        }

        if (c >= '?' && c <= '~') { // Sixel character
            uint8_t bits = static_cast<uint8_t>(c - '?');
            ensure_size(width + 1, current_band_y + 6);
            uint32_t color = palette[active_color_idx];

            for (int bit = 0; bit < 6; ++bit) {
                if (bits & (1 << bit)) {
                    pixel_grid[(current_band_y + bit) * grid_width + width] = color;
                }
            }
            width++;
            max_width = std::max(max_width, width);
            i++;
            continue;
        }

        i++; // Unknown / ignored
    }

    int total_height = current_band_y + 6;
    if (max_width <= 0 || total_height <= 0) return std::nullopt;

    int stride = max_width * 4;
    pixman_image_t* raw_img = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, max_width, total_height, nullptr, stride);
    if (!raw_img) return std::nullopt;

    util::PixmanImage result(raw_img);
    uint32_t* dst = result.data();

    for (int y = 0; y < total_height; ++y) {
        for (int x = 0; x < max_width; ++x) {
            dst[y * max_width + x] = pixel_grid[y * grid_width + x];
        }
    }

    return result;
}

} // namespace gritty::core
