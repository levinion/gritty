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

void TerminalEngine::configure_initial_colors(uint32_t foreground, uint32_t background,
                                              uint32_t cursor_color) noexcept {
    configured_fg_ = foreground;
    configured_bg_ = background;
    configured_cursor_ = cursor_color;
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

void TerminalEngine::reset_palette_color(uint8_t index) noexcept {
    set_palette_color(index, configured_palette_[index]);
}

void TerminalEngine::reset_palette() noexcept {
    palette_ = configured_palette_;
    primary_grid_.mark_all_dirty();
    alt_grid_.mark_all_dirty();
}

void TerminalEngine::resize(int cols, int rows) {
    if (cols <= 0 || rows <= 0) return;
    reset_grapheme_state();
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
    kitty_graphics_.set_alt_screen(enable);
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
    reset_grapheme_state();
    if (!enable && mode == 1049) restore_cursor();
    active_grid_->mark_all_dirty();
}

void TerminalEngine::set_cursor_pos(int col, int row) {
    wrap_next_ = false;
    reset_grapheme_state();
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
    reset_grapheme_state();
    const int min_row = origin_mode_ ? margin_top_ : 0;
    cursor_.row = std::max(min_row, cursor_.row - std::min(count, rows_));
}

void TerminalEngine::move_cursor_down(int count) {
    wrap_next_ = false;
    reset_grapheme_state();
    const int max_row = origin_mode_ ? margin_bottom_ : rows_ - 1;
    cursor_.row = std::min(max_row, cursor_.row + std::min(count, rows_));
}

void TerminalEngine::move_cursor_forward(int count) {
    wrap_next_ = false;
    reset_grapheme_state();
    cursor_.col = std::min(cols_ - 1, cursor_.col + std::min(count, cols_));
}

void TerminalEngine::move_cursor_backward(int count) {
    wrap_next_ = false;
    reset_grapheme_state();
    cursor_.col = std::max(0, cursor_.col - std::min(count, cols_));
}

void TerminalEngine::set_cursor_col(int col) {
    wrap_next_ = false;
    reset_grapheme_state();
    cursor_.col = std::clamp(col, 0, cols_ - 1);
}

void TerminalEngine::set_cursor_row(int row) {
    wrap_next_ = false;
    reset_grapheme_state();
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
    reset_grapheme_state();
}

void TerminalEngine::line_feed() {
    wrap_next_ = false;
    reset_grapheme_state();
    if (cursor_.row == margin_bottom_) {
        scroll_up(1);
    } else if (cursor_.row < rows_ - 1) {
        cursor_.row++;
    }
}

void TerminalEngine::reverse_index() {
    wrap_next_ = false;
    reset_grapheme_state();
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
    reset_grapheme_state();
}

void TerminalEngine::print_char(char32_t cp) {
    // DEC special graphics mapping, also used by tmux's ACS pane borders.
    if (graphic_charsets_[selected_charset_] && cp >= 0x60 && cp <= 0x7e) {
        constexpr std::u32string_view graphics = U"◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";
        cp = graphics[cp - 0x60];
    }
    if (grapheme_clustering_ && last_grapheme_valid_ && (wrap_next_ || cursor_.col > 0)) {
        if (!utf8proc_grapheme_break_stateful(
                static_cast<utf8proc_int32_t>(last_grapheme_cp_),
                static_cast<utf8proc_int32_t>(cp),
                &grapheme_state_)) {
            int target_col = wrap_next_ ? cursor_.col : (cursor_.col - 1);
            if (active_grid_->cell(target_col, cursor_.row).attrs.width == 0 && target_col > 0) {
                --target_col;
            }
            auto& prev_cell = active_grid_->cell(target_col, cursor_.row);
            CellExtra extra = active_grid_->get_cell_extra(prev_cell.extra_idx);
            extra.combining_idx = active_grid_->append_combining_codepoint(extra.combining_idx, cp);
            prev_cell.extra_idx = active_grid_->intern_cell_extra(extra);
            auto& row = active_grid_->row(cursor_.row);
            if (prev_cell.attrs.width == 1 &&
                (cp == 0xFE0F || (cp >= 0x1F1E6 && cp <= 0x1F1FF)) &&
                target_col + 1 < cols_) {
                prev_cell.attrs.width = 2;
                active_grid_->repair_wide_span(target_col + 1, target_col + 1, cursor_.row);
                Cell cont;
                cont.codepoint = 0;
                cont.fg = prev_cell.fg;
                cont.bg = prev_cell.bg;
                cont.attrs = prev_cell.attrs;
                cont.attrs.width = 0;
                cont.extra_idx = 0;
                row.cells[target_col + 1] = cont;
                if (target_col + 2 >= cols_) {
                    cursor_.col = cols_ - 1;
                    if (auto_wrap_) wrap_next_ = true;
                } else {
                    cursor_.col = target_col + 2;
                    wrap_next_ = false;
                }
                row.mark_dirty_range(target_col, target_col + 1);
                row.note_written(target_col + 1);
            } else {
                row.mark_dirty(target_col);
                row.note_written(target_col);
            }
            last_grapheme_cp_ = cp;
            return;
        }
        grapheme_state_ = 0;
    }
    const int width = (cp >= 0x20 && cp <= 0x7e) ? 1 : (common_wide_char(cp) ? 2 : utf8proc_charwidth(cp));
    if (width <= 0) {
        // Combining character: append to previous cell (or final cell when wrap_next_ is pending).
        if (width == 0 && (wrap_next_ || cursor_.col > 0)) {
            int target_col = wrap_next_ ? cursor_.col : (cursor_.col - 1);
            if (active_grid_->cell(target_col, cursor_.row).attrs.width == 0 && target_col > 0) {
                target_col--; // If this was continuation cell, find leading cell
            }
            auto& prev_cell = active_grid_->cell(target_col, cursor_.row);
            CellExtra extra = active_grid_->get_cell_extra(prev_cell.extra_idx);
            extra.combining_idx = active_grid_->append_combining_codepoint(extra.combining_idx, cp);
            prev_cell.extra_idx = active_grid_->intern_cell_extra(extra);
            auto& row = active_grid_->row(cursor_.row);
            row.mark_dirty(target_col);
            row.note_written(target_col);
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
    if (current_underline_color_.is_default() && current_hyperlink_id_ == 0) {
        c.extra_idx = 0;
        active_grid_->set_cell(cursor_.col, cursor_.row, c);
    } else {
        const CellExtra extra{
            .combining_idx = 0,
            .underline_color = current_underline_color_,
            .hyperlink_id = current_hyperlink_id_,
        };
        active_grid_->set_cell_with_extra(cursor_.col, cursor_.row, c, extra);
    }

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
    if (grapheme_clustering_) {
        last_grapheme_cp_ = cp;
        grapheme_state_ = 0;
        last_grapheme_valid_ = true;
    }
}

void TerminalEngine::print_ascii_char(uint8_t ch) {
    if (graphic_charsets_[selected_charset_] || insert_mode_) {
        print_char(static_cast<char32_t>(ch));
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

    const int col = cursor_.col;
    auto& r = active_grid_->row(cursor_.row);
    auto& target = r.cells[col];
    if (target.attrs.width != 1) {
        active_grid_->repair_wide_span(col, col, cursor_.row);
    }

    Cell c;
    c.codepoint = static_cast<char32_t>(ch);
    c.fg = current_fg_;
    c.bg = current_bg_;
    c.attrs = current_attrs_;
    c.attrs.width = 1;
    c.extra_idx = (current_underline_color_.is_default() && current_hyperlink_id_ == 0)
        ? 0
        : active_grid_->intern_cell_extra(CellExtra{
            .combining_idx = 0,
            .underline_color = current_underline_color_,
            .hyperlink_id = current_hyperlink_id_,
        });
    if (target != c) {
        target = c;
        r.mark_dirty(col);
        r.note_written(col);
    }
    last_printed_ = c.codepoint;
    if (col + 1 >= cols_) {
        cursor_.col = cols_ - 1;
        if (auto_wrap_) wrap_next_ = true;
    } else {
        cursor_.col = col + 1;
    }
    if (grapheme_clustering_) {
        last_grapheme_cp_ = last_printed_;
        grapheme_state_ = 0;
        last_grapheme_valid_ = true;
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
        auto& row = active_grid_->row(cursor_.row);
        if (start < row.clean_from) {
            active_grid_->repair_wide_span(start, start + static_cast<int>(chunk) - 1, cursor_.row);
        }
        const uint16_t extra_idx = (current_underline_color_.is_default() && current_hyperlink_id_ == 0)
            ? 0
            : active_grid_->intern_cell_extra(CellExtra{
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
        if (dirty_max >= dirty_min) {
            row.mark_dirty_range(dirty_min, dirty_max);
            row.note_written(dirty_max);
        }
        offset += chunk;
        last_printed_ = static_cast<char32_t>(static_cast<unsigned char>(ascii[offset - 1]));
        cursor_.col += static_cast<int>(chunk);
        if (cursor_.col >= cols_) {
            cursor_.col = cols_ - 1;
            if (auto_wrap_) wrap_next_ = true;
        }
    }
    if (grapheme_clustering_ && last_printed_ != 0) {
        last_grapheme_cp_ = last_printed_;
        grapheme_state_ = 0;
        last_grapheme_valid_ = true;
    }
}

void TerminalEngine::print_wide_run(std::span<const char32_t> cps) {
    if (cps.empty()) return;
    if (graphic_charsets_[selected_charset_] || insert_mode_) {
        for (const char32_t cp : cps) print_char(cp);
        return;
    }

    const uint16_t extra_idx = (current_underline_color_.is_default() && current_hyperlink_id_ == 0)
        ? 0
        : active_grid_->intern_cell_extra(CellExtra{
            .combining_idx = 0,
            .underline_color = current_underline_color_,
            .hyperlink_id = current_hyperlink_id_,
        });

    CellAttributes leader_attrs = current_attrs_;
    leader_attrs.width = 2;
    Cell leader;
    leader.fg = current_fg_;
    leader.bg = current_bg_;
    leader.attrs = leader_attrs;
    leader.extra_idx = extra_idx;

    CellAttributes cont_attrs = current_attrs_;
    cont_attrs.width = 0;
    Cell cont;
    cont.codepoint = 0;
    cont.fg = current_fg_;
    cont.bg = current_bg_;
    cont.attrs = cont_attrs;

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
        const int slots = remaining / 2;
        const size_t pair_count = std::min(static_cast<size_t>(slots), cps.size() - i);
        size_t written = 0;
        if (pair_count > 0 && cursor_.col >= row.clean_from) {
            // Fast path on blank tails: skip wide repair and per-cell equality checks.
            const int start_col = cursor_.col;
            for (; written < pair_count; ++written, ++i) {
                const int col = start_col + static_cast<int>(written) * 2;
                leader.codepoint = cps[i];
                cells[col] = leader;
                cells[col + 1] = cont;
            }
            last_printed_ = cps[i - 1];
            const int end_col = start_col + static_cast<int>(written) * 2 - 1;
            row.mark_dirty_range(start_col, end_col);
            row.note_written(end_col);
        } else {
            int dirty_min = cols_, dirty_max = -1;
            if (pair_count > 0) {
                active_grid_->repair_wide_span(
                    cursor_.col, cursor_.col + static_cast<int>(pair_count) * 2 - 1, cursor_.row);
            }
            for (; written < pair_count; ++written, ++i) {
                const int col = cursor_.col + static_cast<int>(written) * 2;
                leader.codepoint = cps[i];
                if (cells[col] != leader) {
                    cells[col] = leader;
                    dirty_min = std::min(dirty_min, col);
                    dirty_max = std::max(dirty_max, col);
                }
                if (cells[col + 1] != cont) {
                    cells[col + 1] = cont;
                    dirty_min = std::min(dirty_min, col + 1);
                    dirty_max = std::max(dirty_max, col + 1);
                }
            }
            if (written > 0) last_printed_ = cps[i - 1];
            if (dirty_max >= dirty_min) {
                row.mark_dirty_range(dirty_min, dirty_max);
                row.note_written(dirty_max);
            }
        }

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
            active_grid_->repair_wide_span(col, col, cursor_.row);
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
                narrow_row.note_written(0);
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
    if (grapheme_clustering_ && last_printed_ != 0) {
        last_grapheme_cp_ = last_printed_;
        grapheme_state_ = 0;
        last_grapheme_valid_ = true;
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
    if (mode != 3) {
        wrap_next_ = false;
        reset_grapheme_state();
    }
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->erase_in_display(cursor_.col, cursor_.row, mode, empty_cell);
    // Per Kitty graphics protocol, ED 2 clears placements on the active screen;
    // partial clears (ED 0/1) leave placements intact for split-pane TUIs.
    if (mode == 2) {
        kitty_graphics_.clear_placements_for_screen(is_alt_screen_);
        const bool had = std::any_of(images_.begin(), images_.end(),
            [this](const PlacedImage& image) { return image.alt_screen == is_alt_screen_; });
        if (had) active_grid_->mark_all_dirty();
        images_.erase(std::remove_if(images_.begin(), images_.end(),
            [this](const PlacedImage& image) { return image.alt_screen == is_alt_screen_; }),
            images_.end());
    }
}

void TerminalEngine::erase_in_line(int mode) {
    wrap_next_ = false;
    reset_grapheme_state();
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->erase_in_line(cursor_.col, cursor_.row, mode, empty_cell);
}

void TerminalEngine::insert_lines(int count) {
    if (cursor_.row < margin_top_ || cursor_.row > margin_bottom_) return;
    reset_grapheme_state();
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->insert_lines(cursor_.row, margin_bottom_, count, empty_cell);
    scroll_images(cursor_.row, margin_bottom_, -count);
}

void TerminalEngine::delete_lines(int count) {
    if (cursor_.row < margin_top_ || cursor_.row > margin_bottom_) return;
    reset_grapheme_state();
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->delete_lines(cursor_.row, margin_bottom_, count, empty_cell);
    scroll_images(cursor_.row, margin_bottom_, count);
}

void TerminalEngine::insert_chars(int count) {
    reset_grapheme_state();
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->insert_cells(cursor_.col, cursor_.row, count, empty_cell);
}

void TerminalEngine::delete_chars(int count) {
    reset_grapheme_state();
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->delete_cells(cursor_.col, cursor_.row, count, empty_cell);
}

void TerminalEngine::erase_chars(int count) {
    reset_grapheme_state();
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

    // Bitmap memory is bounded by KittyGraphicsEngine's LRU before a
    // placement reaches this list; placements only share those refcounted
    // bitmaps, so no second budget is needed here.
    damage_image_rows(placement.row, placement.rows);
    images_.push_back(std::move(placement));
}

void TerminalEngine::clear_images() noexcept {
    if (!images_.empty()) active_grid_->mark_all_dirty();
    images_.clear();
}

void TerminalEngine::remove_image_by_uid(uint64_t uid) noexcept {
    if (uid == 0) return;
    const bool found = std::any_of(images_.begin(), images_.end(),
        [uid](const PlacedImage& image) { return image.uid == uid; });
    if (found) active_grid_->mark_all_dirty();
    std::erase_if(images_, [uid](const PlacedImage& image) { return image.uid == uid; });
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

void TerminalEngine::scroll_images(int top, int bottom, int rows) noexcept {
    if (images_.empty() || rows == 0) return;
    // Images overlapping the scrolled region move with it and disappear once
    // fully outside; images in other panes stay put. Partially scrolled images
    // stay clipped inside [top, bottom] so they never overlap headers/footers.
    bool scrolled_any = false;
    std::vector<PlacedImage> kept;
    kept.reserve(images_.size());
    for (auto& image : images_) {
        if (image.alt_screen != is_alt_screen_ || image.row + image.rows <= top || image.row > bottom) {
            kept.push_back(std::move(image));
            continue;
        }
        scrolled_any = true;
        damage_image_rows(image.row, image.rows); // Old position needs a repaint.
        image.row -= rows;
        image.clip_top_row = std::max(image.clip_top_row, top);
        image.clip_bottom_row = image.clip_bottom_row < 0
            ? bottom : std::min(image.clip_bottom_row, bottom);
        if (image.row + image.rows > top && image.row <= bottom) {
            damage_image_rows(image.row, image.rows); // New position too.
            kept.push_back(std::move(image));
        }
    }
    images_ = std::move(kept);
    if (scrolled_any) kitty_graphics_.scroll_placements(top, bottom, rows, is_alt_screen_);
}

} // namespace gritty::core
