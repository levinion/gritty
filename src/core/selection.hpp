#pragma once

#include <cstdint>
#include <string>
#include <algorithm>
#include "core/screen_grid.hpp"

namespace gritty::core {

enum class SelectionMode : uint8_t {
    Normal,   // Character stream
    Block,    // Rectangular block
    Line,     // Entire lines
};

class SelectionController final {
public:
    SelectionController() = default;

    void start(int col, int row, SelectionMode mode = SelectionMode::Normal) noexcept {
        start_col_ = col;
        start_row_ = row;
        end_col_ = col;
        end_row_ = row;
        mode_ = mode;
        active_ = true;
    }

    void update(int col, int row) noexcept {
        if (!active_) return;
        end_col_ = col;
        end_row_ = row;
    }

    void clear() noexcept {
        active_ = false;
    }

    [[nodiscard]] bool is_active() const noexcept { return active_; }
    [[nodiscard]] SelectionMode mode() const noexcept { return mode_; }

    [[nodiscard]] bool is_selected(int col, int row) const noexcept {
        if (!active_) return false;

        int r1 = std::min(start_row_, end_row_);
        int r2 = std::max(start_row_, end_row_);
        int c1 = std::min(start_col_, end_col_);
        int c2 = std::max(start_col_, end_col_);

        if (row < r1 || row > r2) return false;

        if (mode_ == SelectionMode::Block) {
            return col >= c1 && col <= c2;
        }

        if (mode_ == SelectionMode::Line) {
            return true;
        }

        // Normal stream selection
        if (start_row_ == end_row_) {
            return col >= c1 && col <= c2;
        }

        // Directional
        int start_c = (start_row_ < end_row_) ? start_col_ : end_col_;
        int end_c   = (start_row_ < end_row_) ? end_col_ : start_col_;

        if (row == r1) return col >= start_c;
        if (row == r2) return col <= end_c;
        return true;
    }

    std::string extract_text(const ScreenGrid& grid) const;

private:
    bool active_{false};
    SelectionMode mode_{SelectionMode::Normal};
    int start_col_{0};
    int start_row_{0};
    int end_col_{0};
    int end_row_{0};
};

} // namespace gritty::core
