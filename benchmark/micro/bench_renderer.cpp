#include "bench.hpp"
#include "core/terminal_engine.hpp"
#include "core/vt_parser.hpp"
#include "render/cpu_renderer.hpp"
#include "render/font_engine.hpp"
#include <cstdint>
#include <string>

namespace {

using namespace gritty;
using namespace gritty::bench;

std::string highlight_stream() {
    static const char* tokens[] = {"const", "auto", "int", "return", "if", "else", "void", "struct", "class", "for"};
    std::string out;
    out.reserve(220 * 1000);
    uint32_t state = 7;
    for (int row = 0; row < 48; ++row) {
        for (int col = 0; col < 40; ++col) {
            state = state * 1103515245 + 12345;
            const char* token = tokens[(state >> 8) % 10];
            out += "\x1b[38;2;80;180;255m";
            out += token;
            out += "\x1b[38;2;221;221;221m ";
        }
        out.push_back('\n');
    }
    return out;
}

static void render_full_highlight_1000() {
    const auto font_path = find_monospace_font();
    if (font_path.empty()) { std::cerr << "No monospace font; skipping renderer benchmark\n"; return; }
    render::FontEngine fonts;
    if (!fonts.load_font(font_path, 11.0, 96)) { std::cerr << "Font load failed\n"; return; }
    core::TerminalEngine engine(160, 48);
    engine.set_cursor_visible(false);
    core::VtParser parser(engine);
    parser.feed(highlight_stream());
    engine.grid().reset_damage();

    const int width = 1920, height = 1080;
    util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    if (!target.valid()) return;
    render::CpuRenderer renderer(fonts);
    Runner runner(5);
    runner.report("render/full_highlight_1000", static_cast<uint64_t>(width) * height * 4, 1000,
        [&](size_t) { renderer.render(engine, target.get(), nullptr, true); });
}
static Registrar render_full_highlight_1000_reg("render/full_highlight_1000", &render_full_highlight_1000);

static void render_scroll_adaptive_1000() {
    const auto font_path = find_monospace_font();
    if (font_path.empty()) return;
    render::FontEngine fonts;
    if (!fonts.load_font(font_path, 11.0, 96)) return;
    core::TerminalEngine engine(160, 48);
    engine.set_cursor_visible(false);
    core::VtParser parser(engine);
    parser.feed(highlight_stream());
    engine.grid().reset_damage();

    const int width = 1920, height = 1080;
    util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    if (!target.valid()) return;
    render::CpuRenderer renderer(fonts);
    renderer.render(engine, target.get(), nullptr, true);
    const std::string line = "\x1b[38;2;100;200;255mnew row with syntax colors 1234567890\r\n";
    Runner runner(5);
    runner.report("render/scroll_adaptive_1000", static_cast<uint64_t>(width) * height * 4 / 100, 1000,
        [&](size_t) {
            parser.feed(line);
            renderer.render(engine, target.get(), nullptr, false);
        });
}
static Registrar render_scroll_adaptive_1000_reg("render/scroll_adaptive_1000", &render_scroll_adaptive_1000);

} // namespace

namespace gritty::bench {
void register_renderer() {}
} // namespace gritty::bench
