#include "core/selection.hpp"
#include <utf8proc.h>

namespace squirt::core {

namespace {

void append_utf8(std::string& out, char32_t cp) {
    if (cp == 0 || cp == ' ') {
        out.push_back(' ');
        return;
    }
    uint8_t buf[4];
    utf8proc_ssize_t len = utf8proc_encode_char(static_cast<utf8proc_int32_t>(cp), buf);
    if (len > 0) {
        out.append(reinterpret_cast<char*>(buf), static_cast<size_t>(len));
    }
}

} // namespace

std::string SelectionController::extract_text(const ScreenGrid& grid) const {
    if (!active_) return "";

    std::string result;
    int r1 = std::min(start_row_, end_row_);
    int r2 = std::max(start_row_, end_row_);

    r1 = std::clamp(r1, 0, grid.rows() - 1);
    r2 = std::clamp(r2, 0, grid.rows() - 1);

    for (int y = r1; y <= r2; ++y) {
        int line_end = grid.cols() - 1;
        // Trim trailing spaces for cleaner copy
        while (line_end >= 0 && grid.cell(line_end, y).codepoint == ' ') {
            line_end--;
        }

        for (int x = 0; x <= line_end; ++x) {
            if (is_selected(x, y)) {
                const auto& c = grid.cell(x, y);
                if (c.attrs.width != 0) { // skip continuation cells
                    append_utf8(result, c.codepoint);
                }
            }
        }

        if (y < r2 && !grid.row(y).wrapped) {
            result.push_back('\n');
        }
    }

    return result;
}

} // namespace squirt::core
