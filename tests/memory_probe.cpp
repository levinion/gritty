// Explicit opt-in workload probe, not a machine-dependent RSS unit-test gate.
#include <cstdio>
#include <chrono>
#include <string>
#include <sys/resource.h>
#if defined(PLATFORM_MACOS)
#include <mach/mach.h>
#endif
#include "core/terminal_engine.hpp"
#include "render/font_engine.hpp"

namespace {
uint64_t footprint() {
#if defined(PLATFORM_MACOS)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.phys_footprint;
#endif
    return 0;
}

template<class Grid> void combining_stats(const Grid& grid) {
    if constexpr (requires { grid.combining_entry_count(); grid.combining_storage_bytes(); })
        std::printf(",\"entries\":%zu,\"storage_bytes\":%zu", grid.combining_entry_count(), grid.combining_storage_bytes());
}
template<class Fonts> void glyph_stats(const Fonts& fonts) {
    if constexpr (requires { fonts.cache_stats(); }) {
        const auto stats = fonts.cache_stats();
        std::printf(",\"entries\":%zu,\"bitmap_bytes\":%zu,\"evictions\":%zu", stats.entries, stats.bitmap_bytes, stats.evictions);
    }
}
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    const auto start = std::chrono::steady_clock::now();
    if (mode == "combining") {
        gritty::core::TerminalEngine terminal(80, 24);
        const auto initial = footprint();
        for (int i = 0; i < 500000; ++i) {
            terminal.set_cursor_pos(0, 0);
            terminal.print_char(U'e');
            terminal.print_char(U'\u0301');
        }
        const auto marks = terminal.grid().get_combining_codepoints(terminal.grid().get_cell_extra(terminal.grid().cell(0, 0).extra_idx).combining_idx);
        if (marks.size() != 1 || marks[0] != U'\u0301') return 3;
        std::printf("{\"mode\":\"combining\",\"initial_footprint\":%llu,\"final_footprint\":%llu",
            static_cast<unsigned long long>(initial), static_cast<unsigned long long>(footprint()));
        combining_stats(terminal.grid());
    } else if (mode == "glyphs" && argc >= 3) {
        gritty::render::FontEngine fonts;
        if (!fonts.load_font(argv[2], 14.0, 192)) return 4;
        gritty::util::FtLibrary library;
        FT_Face raw = nullptr;
        if (FT_New_Face(library.get(), argv[2], 0, &raw) != 0) return 5;
        gritty::util::FtFace face(raw);
        const auto initial = footprint();
        size_t requests = 0;
        uint64_t checksum = 0;
        FT_UInt index = 0;
        for (FT_ULong cp = FT_Get_First_Char(raw, &index); index != 0; cp = FT_Get_Next_Char(raw, cp, &index)) {
            for (int style = 0; style < 4; ++style) {
                const auto* glyph = fonts.get_glyph(static_cast<char32_t>(cp), style & 1, style & 2);
                if (!glyph) continue;
                ++requests;
                if (glyph->mask.valid()) checksum += glyph->mask.data()[0] + glyph->width + glyph->height;
            }
        }
        std::printf("{\"mode\":\"glyphs\",\"initial_footprint\":%llu,\"final_footprint\":%llu,\"requests\":%zu,\"checksum\":%llu",
            static_cast<unsigned long long>(initial), static_cast<unsigned long long>(footprint()), requests,
            static_cast<unsigned long long>(checksum));
        glyph_stats(fonts);
    } else return 2;
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf(",\"seconds\":%.6f}\n", seconds);
}
