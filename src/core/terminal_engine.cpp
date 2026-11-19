#include "core/terminal_engine.hpp"
#include <utf8proc.h>
#include <algorithm>
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace gritty::core {
namespace {

// Common wide ranges used by CJK/CJK-compat text. Avoiding the utf8proc table
// lookup here matters for bulk CJK output; everything else keeps the Unicode
// database result.
inline bool common_wide_char(char32_t cp) noexcept {
    if (cp == 0x3099 || cp == 0x309a) return false; // zero-width combining marks
    return (cp >= 0x4e00 && cp <= 0x9fff) ||
           (cp >= 0x3040 && cp <= 0x30ff) ||
           (cp >= 0xac00 && cp <= 0xd7af) ||
           (cp >= 0xff01 && cp <= 0xff60);
}

} // namespace

TerminalEngine::TerminalEngine(int cols, int rows)
    : cols_(cols),
      rows_(rows),
      primary_grid_(cols, rows),
      alt_grid_(cols, rows),
      active_grid_(&primary_grid_),
      margin_top_(0),
      margin_bottom_(rows - 1) {
    reset_palette();
    init_tabs();
    for (auto& stack : kitty_keyboard_stacks_) stack.reserve(64);
}

void TerminalEngine::configure_palette(const std::array<uint32_t, 256>& colors) noexcept {
    configured_palette_ = colors;
    for (auto& color : configured_palette_) color &= 0xffffff;
    reset_palette();
}

void TerminalEngine::set_palette_color(uint8_t index, uint32_t rgb24) noexcept {
    rgb24 &= 0xffffff;
    if (palette_[index] == rgb24) return;
    palette_[index] = rgb24;
    primary_grid_.mark_all_dirty();
    alt_grid_.mark_all_dirty();
}

void TerminalEngine::reset_palette() noexcept {
    palette_ = configured_palette_;
    primary_grid_.mark_all_dirty();
    alt_grid_.mark_all_dirty();
}

void TerminalEngine::resize(int cols, int rows) {
    if (cols <= 0 || rows <= 0) return;
    cols_ = cols;
    rows_ = rows;

    primary_grid_.resize(cols, rows);
    alt_grid_.resize(cols, rows);

    margin_top_ = 0;
    margin_bottom_ = rows - 1;

    cursor_.col = std::clamp(cursor_.col, 0, cols_ - 1);
    cursor_.row = std::clamp(cursor_.row, 0, rows_ - 1);
    wrap_next_ = false;

    init_tabs();
}

void TerminalEngine::set_alt_screen(bool enable, int mode) {
    if (enable == is_alt_screen_) return;
    // foot csi.c: 47/1047 switch grids; only 1049 saves/restores the cursor.
    if (enable && mode == 1049) save_cursor();
    is_alt_screen_ = enable;

    if (is_alt_screen_) {
        active_grid_ = &alt_grid_;
        alt_grid_.erase_in_display(0, 0, 2);
    } else {
        active_grid_ = &primary_grid_;
    }
    wrap_next_ = false;
    if (!enable && mode == 1049) restore_cursor();
    active_grid_->mark_all_dirty();
}

void TerminalEngine::set_cursor_pos(int col, int row) {
    wrap_next_ = false;
    if (origin_mode_) {
        row = margin_top_ + row;
        cursor_.row = std::clamp(row, margin_top_, margin_bottom_);
    } else {
        cursor_.row = std::clamp(row, 0, rows_ - 1);
    }
    cursor_.col = std::clamp(col, 0, cols_ - 1);
}

void TerminalEngine::move_cursor_up(int count) {
    wrap_next_ = false;
    const int min_row = origin_mode_ ? margin_top_ : 0;
    cursor_.row = std::max(min_row, cursor_.row - std::min(count, rows_));
}

void TerminalEngine::move_cursor_down(int count) {
    wrap_next_ = false;
    const int max_row = origin_mode_ ? margin_bottom_ : rows_ - 1;
    cursor_.row = std::min(max_row, cursor_.row + std::min(count, rows_));
}

void TerminalEngine::move_cursor_forward(int count) {
    wrap_next_ = false;
    cursor_.col = std::min(cols_ - 1, cursor_.col + std::min(count, cols_));
}

void TerminalEngine::move_cursor_backward(int count) {
    wrap_next_ = false;
    cursor_.col = std::max(0, cursor_.col - std::min(count, cols_));
}

void TerminalEngine::set_cursor_col(int col) {
    wrap_next_ = false;
    cursor_.col = std::clamp(col, 0, cols_ - 1);
}

void TerminalEngine::set_cursor_row(int row) {
    wrap_next_ = false;
    if (origin_mode_) {
        row = margin_top_ + row;
        cursor_.row = std::clamp(row, margin_top_, margin_bottom_);
    } else {
        cursor_.row = std::clamp(row, 0, rows_ - 1);
    }
}

void TerminalEngine::carriage_return() {
    cursor_.col = 0;
    wrap_next_ = false;
}

void TerminalEngine::line_feed() {
    wrap_next_ = false;
    if (cursor_.row == margin_bottom_) {
        scroll_up(1);
    } else if (cursor_.row < rows_ - 1) {
        cursor_.row++;
    }
}

void TerminalEngine::reverse_index() {
    wrap_next_ = false;
    if (cursor_.row == margin_top_) {
        scroll_down(1);
    } else if (cursor_.row > 0) {
        cursor_.row--;
    }
}

void TerminalEngine::save_cursor() {
    auto& target = is_alt_screen_ ? saved_cursor_alt_ : saved_cursor_primary_;
    target = cursor_;
    target.fg = current_fg_;
    target.bg = current_bg_;
    target.underline_color = current_underline_color_;
    target.attrs = current_attrs_;
    target.origin_mode = origin_mode_;
    target.auto_wrap = auto_wrap_;
    target.pending_wrap = wrap_next_;
    target.graphic_charsets = graphic_charsets_;
    target.selected_charset = selected_charset_;
}

void TerminalEngine::restore_cursor() {
    const auto& src = is_alt_screen_ ? saved_cursor_alt_ : saved_cursor_primary_;
    // Cursor visibility/shape are not part of DECSC's saved rendition state.
    cursor_.col = src.col;
    cursor_.row = src.row;
    current_fg_ = src.fg;
    current_bg_ = src.bg;
    current_underline_color_ = src.underline_color;
    current_attrs_ = src.attrs;
    origin_mode_ = src.origin_mode;
    auto_wrap_ = src.auto_wrap;
    wrap_next_ = src.pending_wrap;
    graphic_charsets_ = src.graphic_charsets;
    selected_charset_ = src.selected_charset;
    cursor_.col = std::clamp(cursor_.col, 0, cols_ - 1);
    cursor_.row = std::clamp(cursor_.row, 0, rows_ - 1);
}

void TerminalEngine::print_char(char32_t cp) {
    // DEC special graphics mapping, also used by tmux's ACS pane borders.
    if (graphic_charsets_[selected_charset_] && cp >= 0x60 && cp <= 0x7e) {
        constexpr std::u32string_view graphics = U"◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";
        cp = graphics[cp - 0x60];
    }
    int width = common_wide_char(cp) ? 2 : utf8proc_charwidth(cp);
    if (width <= 0) {
        // Combining character: append to previous cell
        if (width == 0 && cursor_.col > 0) {
            int target_col = cursor_.col - 1;
            if (active_grid_->cell(target_col, cursor_.row).attrs.width == 0 && target_col > 0) {
                target_col--; // If this was continuation cell, find leading cell
            }
            auto& prev_cell = active_grid_->cell(target_col, cursor_.row);
            CellExtra extra = active_grid_->get_cell_extra(prev_cell.extra_idx);
            extra.combining_idx = active_grid_->append_combining_codepoint(extra.combining_idx, cp);
            prev_cell.extra_idx = active_grid_->intern_cell_extra(extra);
            active_grid_->row(cursor_.row).mark_dirty(target_col);
        }
        return;
    }

    if (wrap_next_) {
        if (auto_wrap_) {
            active_grid_->row(cursor_.row).wrapped = true;
            cursor_.col = 0;
            line_feed();
        } else {
            cursor_.col = cols_ - 1;
        }
        wrap_next_ = false;
    }

    if (width == 2 && cursor_.col == cols_ - 1) {
        // Wide char cannot fit on final col: wrap to next line
        if (auto_wrap_) {
            active_grid_->row(cursor_.row).wrapped = true;
            cursor_.col = 0;
            line_feed();
        }
    }

    if (insert_mode_) {
        Cell empty;
        empty.bg = current_bg_;
        active_grid_->insert_cells(cursor_.col, cursor_.row, width, empty);
    }
    last_printed_ = cp;
    Cell c;
    c.codepoint = cp;
    c.fg = current_fg_;
    c.bg = current_bg_;
    c.attrs = current_attrs_;
    c.attrs.width = static_cast<uint16_t>(width);
    const CellExtra extra{
        .combining_idx = 0,
        .underline_color = current_underline_color_,
        .hyperlink_id = current_hyperlink_id_,
    };
    active_grid_->set_cell_with_extra(cursor_.col, cursor_.row, c, extra);

    if (width == 2 && cursor_.col + 1 < cols_) {
        Cell cont;
        cont.codepoint = 0;
        cont.fg = current_fg_;
        cont.bg = current_bg_;
        cont.attrs = current_attrs_;
        cont.attrs.width = 0;
        active_grid_->set_cell(cursor_.col + 1, cursor_.row, cont);
    }

    cursor_.col += width;
    if (cursor_.col >= cols_) {
        cursor_.col = cols_ - 1;
        if (auto_wrap_) {
            wrap_next_ = true;
        }
    }
}

void TerminalEngine::print_ascii_run(std::string_view ascii) {
    if (ascii.empty()) return;
    if (graphic_charsets_[selected_charset_] || insert_mode_) {
        for (const char c : ascii) print_char(static_cast<char32_t>(static_cast<unsigned char>(c)));
        return;
    }

    size_t offset = 0;
    while (offset < ascii.size()) {
        if (wrap_next_) {
            if (auto_wrap_) {
                active_grid_->row(cursor_.row).wrapped = true;
                cursor_.col = 0;
                line_feed();
            } else {
                cursor_.col = cols_ - 1;
            }
            wrap_next_ = false;
        }

        const int start = cursor_.col;
        int avail = cols_ - start;
        if (avail <= 0) {
            if (!auto_wrap_) { cursor_.col = cols_ - 1; avail = 1; }
            else {
                active_grid_->row(cursor_.row).wrapped = true;
                cursor_.col = 0;
                line_feed();
                continue;
            }
        }

        const size_t chunk = std::min(avail, static_cast<int>(ascii.size() - offset));
        const uint16_t extra_idx = active_grid_->intern_cell_extra(CellExtra{
            .combining_idx = 0,
            .underline_color = current_underline_color_,
            .hyperlink_id = current_hyperlink_id_,
        });
        Cell base;
        base.fg = current_fg_;
        base.bg = current_bg_;
        base.attrs = current_attrs_;
        base.attrs.width = 1;
        base.extra_idx = extra_idx;
        auto& row = active_grid_->row(cursor_.row);
        auto* cells = row.cells.data();
        int dirty_min = cols_, dirty_max = -1;
        size_t i = 0;
#if defined(__aarch64__) || defined(_M_ARM64)
        const uint32x4_t v_fg = vdupq_n_u32(base.fg.value);
        const uint32x4_t v_bg = vdupq_n_u32(base.bg.value);
        const uint32x4_t v_tail = vdupq_n_u32(cell_tail_word(base));
        for (; i + 4 <= chunk; i += 4) {
            const uint32_t cps[4] = {
                static_cast<unsigned char>(ascii[offset + i]),
                static_cast<unsigned char>(ascii[offset + i + 1]),
                static_cast<unsigned char>(ascii[offset + i + 2]),
                static_cast<unsigned char>(ascii[offset + i + 3]),
            };
            const uint32x4_t v_cp = vld1q_u32(cps);
            auto* raw = reinterpret_cast<uint32_t*>(cells + start + i);
            const uint32x4x4_t existing = vld4q_u32(raw);
            uint32x4_t equal = vceqq_u32(v_cp, existing.val[0]);
            equal = vandq_u32(equal, vceqq_u32(v_fg, existing.val[1]));
            equal = vandq_u32(equal, vceqq_u32(v_bg, existing.val[2]));
            equal = vandq_u32(equal, vceqq_u32(v_tail, existing.val[3]));
            if (vminvq_u32(equal) != 0) continue;
            const uint32x4x4_t write{v_cp, v_fg, v_bg, v_tail};
            vst4q_u32(raw, write);
            dirty_min = std::min(dirty_min, start + static_cast<int>(i));
            dirty_max = std::max(dirty_max, start + static_cast<int>(i + 3));
        }
#endif
        for (; i < chunk; ++i) {
            base.codepoint = static_cast<unsigned char>(ascii[offset + i]);
            const int col = start + static_cast<int>(i);
            if (cells[col] == base) continue;
            cells[col] = base;
            dirty_min = std::min(dirty_min, col);
            dirty_max = std::max(dirty_max, col);
        }
        if (dirty_max >= dirty_min) row.mark_dirty_range(dirty_min, dirty_max);
        offset += chunk;
        last_printed_ = static_cast<char32_t>(static_cast<unsigned char>(ascii[offset - 1]));
        cursor_.col += static_cast<int>(chunk);
        if (cursor_.col >= cols_) {
            cursor_.col = cols_ - 1;
            if (auto_wrap_) wrap_next_ = true;
        }
    }
}

void TerminalEngine::print_wide_run(std::span<const char32_t> cps) {
    if (cps.empty()) return;
    if (graphic_charsets_[selected_charset_] || insert_mode_) {
        for (const char32_t cp : cps) print_char(cp);
        return;
    }

    const uint16_t extra_idx = active_grid_->intern_cell_extra(CellExtra{
        .combining_idx = 0,
        .underline_color = current_underline_color_,
        .hyperlink_id = current_hyperlink_id_,
    });

    size_t i = 0;
    while (i < cps.size()) {
        if (wrap_next_) {
            if (auto_wrap_) {
                active_grid_->row(cursor_.row).wrapped = true;
                cursor_.col = 0;
                line_feed();
            } else {
                cursor_.col = cols_ - 1;
            }
            wrap_next_ = false;
        }

        const int remaining = cols_ - cursor_.col;
        if (remaining <= 0) {
            if (auto_wrap_) {
                active_grid_->row(cursor_.row).wrapped = true;
                cursor_.col = 0;
                line_feed();
            } else {
                cursor_.col = cols_ - 1;
            }
            continue;
        }

        auto& row = active_grid_->row(cursor_.row);
        auto* cells = row.cells.data();
        int dirty_min = cols_, dirty_max = -1;
        const int slots = remaining / 2;
        size_t written = 0;
        for (; written < static_cast<size_t>(slots) && i < cps.size(); ++written, ++i) {
            const int col = cursor_.col + static_cast<int>(written) * 2;
            CellAttributes leader_attrs = current_attrs_;
            leader_attrs.width = 2;
            Cell leader;
            leader.codepoint = cps[i];
            leader.fg = current_fg_;
            leader.bg = current_bg_;
            leader.attrs = leader_attrs;
            leader.extra_idx = extra_idx;
            if (cells[col] != leader) {
                cells[col] = leader;
                dirty_min = std::min(dirty_min, col);
                dirty_max = std::max(dirty_max, col);
            }

            CellAttributes cont_attrs = current_attrs_;
            cont_attrs.width = 0;
            Cell cont;
            cont.codepoint = 0;
            cont.fg = current_fg_;
            cont.bg = current_bg_;
            cont.attrs = cont_attrs;
            if (cells[col + 1] != cont) {
                cells[col + 1] = cont;
                dirty_min = std::min(dirty_min, col + 1);
                dirty_max = std::max(dirty_max, col + 1);
            }
            last_printed_ = cps[i];
        }
        if (dirty_max >= dirty_min) row.mark_dirty_range(dirty_min, dirty_max);

        cursor_.col += static_cast<int>(written) * 2;
        if (cursor_.col >= cols_) {
            cursor_.col = cols_ - 1;
            if (auto_wrap_) wrap_next_ = true;
        }
        if (i >= cps.size()) break;
        if (written != 0) continue;

        // Only the final odd column is left. print_char() writes a lone wide
        // leader there when wrapping is disabled; with auto-wrap it moves to
        // the next row instead (two columns never fit in one cell).
        const int col = cols_ - 1;
        if (!auto_wrap_) {
            CellAttributes attrs = current_attrs_;
            attrs.width = 2;
            Cell leader;
            leader.codepoint = cps[i];
            leader.fg = current_fg_;
            leader.bg = current_bg_;
            leader.attrs = attrs;
            leader.extra_idx = extra_idx;
            if (cells[col] != leader) {
                cells[col] = leader;
                row.mark_dirty(col);
            }
            last_printed_ = cps[i++];
            cursor_.col = cols_ - 1;
            continue;
        }
        if (cols_ == 1) {
            // print_char() also wraps a double-width codepoint that starts on
            // the final column, even when that column is the entire row.
            row.wrapped = true;
            cursor_.col = 0;
            line_feed();
            auto& narrow_row = active_grid_->row(cursor_.row);
            CellAttributes attrs = current_attrs_;
            attrs.width = 2;
            Cell leader;
            leader.codepoint = cps[i];
            leader.fg = current_fg_;
            leader.bg = current_bg_;
            leader.attrs = attrs;
            leader.extra_idx = extra_idx;
            if (narrow_row.cells[0] != leader) {
                narrow_row.cells[0] = leader;
                narrow_row.mark_dirty(0);
            }
            last_printed_ = cps[i++];
            cursor_.col = 0;
            wrap_next_ = true;
            continue;
        }
        row.wrapped = true;
        cursor_.col = 0;
        line_feed();
    }
}

void TerminalEngine::print_utf8(std::string_view utf8_str) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(utf8_str.data());
    size_t len = utf8_str.size();

    while (len > 0) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t res = utf8proc_iterate(ptr, len, &cp);
        if (res < 0) {
            // Invalid UTF-8 sequence, skip 1 byte
            ptr++;
            len--;
            continue;
        }
        print_char(static_cast<char32_t>(cp));
        ptr += res;
        len -= res;
    }
}

std::string TerminalEngine::format_paste_payload(std::string_view text) const {
    if (!bracketed_paste_) return std::string(text);

    constexpr std::string_view begin_marker = "\x1b[200~";
    constexpr std::string_view end_marker = "\x1b[201~";
    std::string payload;
    payload.reserve(begin_marker.size() + text.size() + end_marker.size());
    payload.append(begin_marker);

    size_t pos = 0;
    while (pos < text.size()) {
        const size_t next = text.find(end_marker, pos);
        if (next == std::string_view::npos) {
            payload.append(text.substr(pos));
            break;
        }
        payload.append(text.substr(pos, next - pos));
        pos = next + end_marker.size();
    }
    // Removing a marker can join an earlier "\x1b[201" with a following '~'
    // into a new terminator, so repeat until the whole payload is inert.
    for (size_t at = begin_marker.size();
         (at = payload.find(end_marker, at)) != std::string::npos;) {
        payload.erase(at, end_marker.size());
    }
    payload.append(end_marker);
    return payload;
}

void TerminalEngine::erase_in_display(int mode) {
    if (mode != 3) wrap_next_ = false;
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->erase_in_display(cursor_.col, cursor_.row, mode, empty_cell);
    switch (mode) {
        case 2:
            images_.erase(std::remove_if(images_.begin(), images_.end(),
                [this](const PlacedImage& image) { return image.alt_screen == is_alt_screen_; }),
                images_.end());
            break;
        case 0: erase_images(cursor_.col, cursor_.row, cols_ - 1, rows_ - 1); break;
        case 1: erase_images(0, 0, cursor_.col, cursor_.row); break;
        case 3: break; // DECSED 3 is a no-op with no local history; keep images.
        default: break;
    }
}

void TerminalEngine::erase_in_line(int mode) {
    wrap_next_ = false;
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->erase_in_line(cursor_.col, cursor_.row, mode, empty_cell);
    switch (mode) {
        case 0: erase_images(cursor_.col, cursor_.row, cols_ - 1, cursor_.row); break;
        case 1: erase_images(0, cursor_.row, cursor_.col, cursor_.row); break;
        case 2: erase_images(0, cursor_.row, cols_ - 1, cursor_.row); break;
        default: break;
    }
}

void TerminalEngine::insert_lines(int count) {
    if (cursor_.row < margin_top_ || cursor_.row > margin_bottom_) return;
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->insert_lines(cursor_.row, margin_bottom_, count, empty_cell);
    scroll_images(cursor_.row, margin_bottom_, -count);
}

void TerminalEngine::delete_lines(int count) {
    if (cursor_.row < margin_top_ || cursor_.row > margin_bottom_) return;
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->delete_lines(cursor_.row, margin_bottom_, count, empty_cell);
    scroll_images(cursor_.row, margin_bottom_, count);
}

void TerminalEngine::insert_chars(int count) {
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->insert_cells(cursor_.col, cursor_.row, count, empty_cell);
}

void TerminalEngine::delete_chars(int count) {
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->delete_cells(cursor_.col, cursor_.row, count, empty_cell);
}

void TerminalEngine::erase_chars(int count) {
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    for (int i = 0; i < count && cursor_.col + i < cols_; ++i) {
        active_grid_->set_cell(cursor_.col + i, cursor_.row, empty_cell);
    }
}

void TerminalEngine::set_margins(int top, int bottom) {
    top = std::max(0, top);
    bottom = std::min(rows_ - 1, bottom);
    if (top < bottom) {
        margin_top_ = top;
        margin_bottom_ = bottom;
        set_cursor_pos(0, 0);
    }
    // Invalid margins are ignored, including their implied cursor movement.
}

void TerminalEngine::set_origin_mode(bool enable) noexcept {
    origin_mode_ = enable;
    set_cursor_pos(0, 0);
}

void TerminalEngine::damage_image_rows(int row, int rows) noexcept {
    const int last = std::min(rows_, row + rows);
    for (int y = std::max(0, row); y < last; ++y)
        active_grid_->row(y).mark_all_dirty(cols_);
}

void TerminalEngine::place_image(PlacedImage placement) {
    if (!placement.image.valid()) return;
    placement.alt_screen = is_alt_screen_;
    placement.cols = std::max(1, placement.cols);
    placement.rows = std::max(1, placement.rows);

    // Sixel images are not capped by the Kitty image store; bound the total
    // render-side bitmap memory the same way (oldest image is evicted first).
    if (placement.image_id == 0) {
        constexpr size_t max_sixel_bytes = size_t{32} << 20;
        const size_t incoming = static_cast<size_t>(placement.image.stride()) * placement.image.height();
        auto bytes_of = [](const PlacedImage& image) {
            return static_cast<size_t>(image.image.stride()) * image.image.height();
        };
        while (true) {
            size_t total = 0;
            for (const auto& image : images_)
                if (image.image_id == 0) total += bytes_of(image);
            if (total + incoming <= max_sixel_bytes) break;
            auto oldest = std::find_if(images_.begin(), images_.end(),
                [](const PlacedImage& image) { return image.image_id == 0; });
            if (oldest == images_.end()) break;
            damage_image_rows(oldest->row, oldest->rows);
            images_.erase(oldest);
        }
    }

    damage_image_rows(placement.row, placement.rows);
    images_.push_back(std::move(placement));
}

void TerminalEngine::clear_images() noexcept {
    if (!images_.empty()) active_grid_->mark_all_dirty();
    images_.clear();
}

void TerminalEngine::remove_images_by_id(uint32_t image_id) noexcept {
    const bool found = std::any_of(images_.begin(), images_.end(),
        [image_id](const PlacedImage& image) { return image.image_id == image_id; });
    if (found) active_grid_->mark_all_dirty();
    std::erase_if(images_, [image_id](const PlacedImage& image) { return image.image_id == image_id; });
}

void TerminalEngine::remove_images_by_placement(uint32_t placement_id) noexcept {
    if (placement_id == 0) return;
    const bool found = std::any_of(images_.begin(), images_.end(),
        [placement_id](const PlacedImage& image) { return image.placement_id == placement_id; });
    if (found) active_grid_->mark_all_dirty();
    std::erase_if(images_, [placement_id](const PlacedImage& image) { return image.placement_id == placement_id; });
}

void TerminalEngine::remove_images_by_placement(uint32_t image_id, uint32_t placement_id) noexcept {
    const bool found = std::any_of(images_.begin(), images_.end(), [&](const PlacedImage& image) {
        return image.image_id == image_id && image.placement_id == placement_id;
    });
    if (found) active_grid_->mark_all_dirty();
    std::erase_if(images_, [&](const PlacedImage& image) {
        return image.image_id == image_id && image.placement_id == placement_id;
    });
}

void TerminalEngine::remove_images_at(int col, int row) noexcept {
    const bool found = std::any_of(images_.begin(), images_.end(), [&](const PlacedImage& image) {
        return image.alt_screen == is_alt_screen_ && col >= image.col &&
            col < image.col + image.cols && row >= image.row && row < image.row + image.rows;
    });
    if (found) active_grid_->mark_all_dirty();
    std::erase_if(images_, [&](const PlacedImage& image) {
        return image.alt_screen == is_alt_screen_ && col >= image.col &&
            col < image.col + image.cols && row >= image.row && row < image.row + image.rows;
    });
}

void TerminalEngine::remove_images_by_z(int z_index) noexcept {
    const bool found = std::any_of(images_.begin(), images_.end(),
        [z_index](const PlacedImage& image) { return image.z_index == z_index; });
    if (found) active_grid_->mark_all_dirty();
    std::erase_if(images_, [z_index](const PlacedImage& image) { return image.z_index == z_index; });
}

void TerminalEngine::erase_images(int col1, int row1, int col2, int row2) noexcept {
    if (col1 > col2) std::swap(col1, col2);
    if (row1 > row2) std::swap(row1, row2);
    const bool found = std::any_of(images_.begin(), images_.end(), [&](const PlacedImage& image) {
        if (image.alt_screen != is_alt_screen_) return false;
        const int c1 = image.col, c2 = image.col + image.cols - 1;
        const int r1 = image.row, r2 = image.row + image.rows - 1;
        return c1 <= col2 && c2 >= col1 && r1 <= row2 && r2 >= row1;
    });
    if (found) active_grid_->mark_all_dirty();
    std::erase_if(images_, [&](const PlacedImage& image) {
        if (image.alt_screen != is_alt_screen_) return false;
        const int c1 = image.col, c2 = image.col + image.cols - 1;
        const int r1 = image.row, r2 = image.row + image.rows - 1;
        return c1 <= col2 && c2 >= col1 && r1 <= row2 && r2 >= row1;
    });
}

void TerminalEngine::scroll_images(int top, int bottom, int rows) noexcept {
    if (rows == 0) return;
    // Images overlapping the scrolled region move with it and disappear once
    // fully outside; images in other panes stay put.
    std::vector<PlacedImage> kept;
    kept.reserve(images_.size());
    for (auto& image : images_) {
        if (image.alt_screen != is_alt_screen_ || image.row + image.rows <= top || image.row > bottom) {
            kept.push_back(std::move(image));
            continue;
        }
        damage_image_rows(image.row, image.rows); // Old position needs a repaint.
        image.row -= rows;
        if (image.row + image.rows > top && image.row <= bottom) {
            damage_image_rows(image.row, image.rows); // New position too.
            kept.push_back(std::move(image));
        }
    }
    images_ = std::move(kept);
}

} // namespace gritty::core
