#include "core/sixel_decoder.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace gritty::core {
namespace {

constexpr int kMaxDimension = 4096;

int read_number(std::string_view stream, size_t& i) {
    const size_t start = i;
    while (i < stream.size() && stream[i] >= '0' && stream[i] <= '9') ++i;
    int value = 0;
    if (start == i) return -1;
    std::from_chars(stream.data() + start, stream.data() + i, value);
    return value;
}

int cvt(double c, double m) noexcept {
    return static_cast<int>(std::lround((c + m) * 255.0)) & 0xff;
}

// HLS -> RGB888, using the same 0..360 / 0..100 / 0..100 ranges as DEC Sixel.
uint32_t hls_to_rgb(int hue, int lightness, int saturation) noexcept {
    const double h = std::clamp(hue, 0, 360) % 360;
    const double l = std::clamp(lightness, 0, 100) / 100.0;
    const double s = std::clamp(saturation, 0, 100) / 100.0;
    const double c = (1.0 - std::abs(2.0 * l - 1.0)) * s;
    const double x = c * (1.0 - std::abs(std::fmod(h / 60.0, 2.0) - 1.0));
    const double m = l - c / 2.0;
    int r = 0, g = 0, b = 0;
    if (h < 60)       { r = cvt(c, m); g = cvt(x, m); b = cvt(0, m); }
    else if (h < 120) { r = cvt(x, m); g = cvt(c, m); b = cvt(0, m); }
    else if (h < 180) { r = cvt(0, m); g = cvt(c, m); b = cvt(x, m); }
    else if (h < 240) { r = cvt(0, m); g = cvt(x, m); b = cvt(c, m); }
    else if (h < 300) { r = cvt(x, m); g = cvt(0, m); b = cvt(c, m); }
    else              { r = cvt(c, m); g = cvt(0, m); b = cvt(x, m); }
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

} // namespace

std::optional<util::PixmanImage> SixelDecoder::decode(
    std::string_view stream, bool background_transparent) {
    if (stream.empty()) return std::nullopt;

    std::array<uint32_t, 256> palette{};
    static constexpr uint32_t kDefaultSixelColors[16] = {
        0x000000, 0x3333cc, 0xcc2121, 0x33cc33,
        0xcc33cc, 0x33cccc, 0xcccc33, 0x878787,
        0x424242, 0x5454ff, 0xff5454, 0x54ff54,
        0xff54ff, 0x54ffff, 0xffff54, 0xffffff
    };
    for (size_t i = 0; i < 16; ++i) palette[i] = 0xFF000000 | kDefaultSixelColors[i];
    const uint32_t background = background_transparent ? 0u : palette[0];

    int width = 0;
    int max_width = 0;
    int current_band_y = 0;
    int active_color_idx = 0;

    int grid_width = 256;
    int grid_height = 256;
    std::vector<uint32_t> pixel_grid(static_cast<size_t>(grid_width) * grid_height, background);

    // Returns false for hostile repeat counts instead of growing without bound.
    auto ensure_size = [&](int req_w, int req_h) {
        if (req_w <= grid_width && req_h <= grid_height) return true;
        if (req_w > kMaxDimension || req_h > kMaxDimension ||
            req_w < 1 || req_h < 1) return false;
        const int new_w = std::max(grid_width, req_w);
        const int new_h = std::max(grid_height, req_h);
        if (static_cast<int64_t>(new_w) * new_h > kMaxDimension * kMaxDimension) return false;
        std::vector<uint32_t> new_grid(static_cast<size_t>(new_w) * new_h, background);
        for (int y = 0; y < grid_height; ++y)
            for (int x = 0; x < grid_width; ++x)
                new_grid[static_cast<size_t>(y) * new_w + x] =
                    pixel_grid[static_cast<size_t>(y) * grid_width + x];
        pixel_grid = std::move(new_grid);
        grid_width = new_w;
        grid_height = new_h;
        return true;
    };

    size_t i = 0;
    if (stream[0] == 'q') ++i;

    while (i < stream.size()) {
        const char c = stream[i];
        if (c == '#') {
            ++i;
            const int col_idx = read_number(stream, i);
            if (col_idx < 0 || col_idx >= 256) continue;
            if (i < stream.size() && stream[i] == ';') {
                ++i;
                const int type = read_number(stream, i);
                if (type == 2 || type == 1) {
                    int a = 0, b = 0, d = 0;
                    if (i < stream.size() && stream[i] == ';') {
                        ++i; a = read_number(stream, i);
                    }
                    if (i < stream.size() && stream[i] == ';') {
                        ++i; b = read_number(stream, i);
                    }
                    if (i < stream.size() && stream[i] == ';') {
                        ++i; d = read_number(stream, i);
                    }
                    const uint32_t rgb = type == 2
                        ? (static_cast<uint32_t>(std::clamp(a, 0, 100) * 255 / 100) << 16) |
                          (static_cast<uint32_t>(std::clamp(b, 0, 100) * 255 / 100) << 8) |
                           static_cast<uint32_t>(std::clamp(d, 0, 100) * 255 / 100)
                        : hls_to_rgb(a, b, d);
                    palette[col_idx] = 0xFF000000 | rgb;
                }
            } else {
                active_color_idx = col_idx;
            }
            continue;
        }

        if (c == '$') {
            width = 0;
            ++i;
            continue;
        }
        if (c == '-') {
            width = 0;
            current_band_y += 6;
            ++i;
            continue;
        }
        if (c == '!') {
            ++i;
            const int repeat = read_number(stream, i);
            if (repeat < 1 || i >= stream.size() || stream[i] < '?' || stream[i] > '~') {
                ++i;
                continue;
            }
            if (!ensure_size(width + repeat, current_band_y + 6)) {
                ++i;
                continue;
            }
            const uint8_t bits = static_cast<uint8_t>(stream[i] - '?');
            const uint32_t color = palette[active_color_idx];
            for (int r = 0; r < repeat; ++r)
                for (int bit = 0; bit < 6; ++bit)
                    if (bits & (1 << bit))
                        pixel_grid[static_cast<size_t>(current_band_y + bit) * grid_width + width + r] = color;
            width += repeat;
            max_width = std::max(max_width, width);
            ++i;
            continue;
        }
        if (c >= '?' && c <= '~') {
            if (!ensure_size(width + 1, current_band_y + 6)) {
                ++i;
                continue;
            }
            const uint8_t bits = static_cast<uint8_t>(c - '?');
            const uint32_t color = palette[active_color_idx];
            for (int bit = 0; bit < 6; ++bit)
                if (bits & (1 << bit))
                    pixel_grid[static_cast<size_t>(current_band_y + bit) * grid_width + width] = color;
            ++width;
            max_width = std::max(max_width, width);
            ++i;
            continue;
        }
        ++i;
    }

    const int total_height = current_band_y + 6;
    if (max_width <= 0 || total_height <= 0 || max_width > kMaxDimension ||
        total_height > kMaxDimension) return std::nullopt;

    util::PixmanImage result(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, max_width, total_height, nullptr, max_width * 4));
    if (!result.valid()) return std::nullopt;
    uint32_t* dst = result.data();
    for (int y = 0; y < total_height; ++y)
        for (int x = 0; x < max_width; ++x)
            dst[static_cast<size_t>(y) * max_width + x] =
                pixel_grid[static_cast<size_t>(y) * grid_width + x];
    return result;
}

} // namespace gritty::core
