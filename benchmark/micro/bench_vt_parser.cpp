#include "bench.hpp"
#include "core/terminal_engine.hpp"
#include "core/vt_parser.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace {

using namespace gritty;
using namespace gritty::bench;

uint32_t next_random(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

std::string ascii_payload(size_t bytes) {
    std::string out;
    out.reserve(bytes);
    uint32_t state = 42;
    size_t column = 0;
    while (out.size() < bytes) {
        if (column == 120) { out.push_back('\n'); column = 0; }
        else {
            const uint32_t r = next_random(state);
            out.push_back(static_cast<char>(0x20 + (r % (0x7e - 0x20 + 1))));
            ++column;
        }
    }
    return out;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

std::string truecolor_payload(size_t bytes) {
    static const std::array<char32_t, 8> marks{U'A', U'B', U'C', U'D', U'中', U'文', U'─', U'│'};
    std::string out;
    out.reserve(bytes);
    uint32_t state = 0x12345678;
    size_t column = 0;
    while (out.size() < bytes) {
        const uint32_t r = next_random(state);
        out += "\x1b[38;2;";
        out += std::to_string(r & 255) + ";" + std::to_string((r >> 8) & 255) + ";" + std::to_string((r >> 16) & 255) + "m";
        const uint32_t g = next_random(state);
        out += "\x1b[48;2;";
        out += std::to_string(g & 255) + ";" + std::to_string((g >> 8) & 255) + ";" + std::to_string((g >> 16) & 255) + "m";
        if ((r & 3) == 0) out += "\x1b[1m";
        if ((r & 7) == 0) out += "\x1b[3m";
        if ((g & 3) == 0) out += "\x1b[4m";
        for (int i = 0; i < 2 + static_cast<int>(r % 3); ++i) {
            const char32_t cp = marks[(r >> (i * 3)) & 7];
            append_utf8(out, cp);
            column += cp > 0x7f ? 2 : 1;
        }
        if (column >= 120) { out.push_back('\n'); column = 0; }
    }
    return out;
}

std::string unicode_payload(size_t bytes) {
    static const std::array<char32_t, 10> glyphs = {
        U'中', U'文', U'界', U'あ', U'┌', U'─', U'┬', U'┐', U'│', U'e',
    };
    std::string out;
    out.reserve(bytes);
    uint32_t state = 0x5eed;
    size_t column = 0;
    while (out.size() < bytes) {
        const uint32_t r = next_random(state);
        const char32_t cp = glyphs[r % glyphs.size()];
        append_utf8(out, cp);
        if (cp == U'e' && (r & 1)) append_utf8(out, 0x301);
        column += cp > 0x7f ? 2 : 1;
        if (column >= 120) { out.push_back('\n'); column = 0; }
    }
    return out;
}

void feed(core::VtParser& parser, const std::string& payload) {
    constexpr size_t chunk = 64 * 1024;
    for (size_t i = 0; i < payload.size(); i += chunk)
        parser.feed(std::string_view(payload).substr(i, std::min(chunk, payload.size() - i)));
}

static const std::string ascii_data = ascii_payload(16u << 20);
static const std::string truecolor_data = truecolor_payload(8u << 20);
static const std::string unicode_data = unicode_payload(4u << 20);
static const std::string scroll_data = std::string(100000, '\n');
static const std::string region_data = "\x1b[1;24r";

static void vt_ascii_16m() {
    Runner runner(1);
    core::TerminalEngine engine(160, 48);
    core::VtParser parser(engine);
    runner.report("vt/ascii_16m", ascii_data.size(), 3, [&](size_t) {
        engine.reset();
        parser.reset();
        feed(parser, ascii_data);
    });
}
static Registrar vt_ascii_16m_reg("vt/ascii_16m", &vt_ascii_16m);

static void vt_truecolor_8m() {
    Runner runner(1);
    core::TerminalEngine engine(160, 48);
    core::VtParser parser(engine);
    runner.report("vt/truecolor_8m", truecolor_data.size(), 3, [&](size_t) {
        engine.reset();
        parser.reset();
        feed(parser, truecolor_data);
    });
}
static Registrar vt_truecolor_8m_reg("vt/truecolor_8m", &vt_truecolor_8m);

static void vt_unicode_cjk_4m() {
    Runner runner(1);
    core::TerminalEngine engine(160, 48);
    core::VtParser parser(engine);
    runner.report("vt/unicode_cjk_4m", unicode_data.size(), 3, [&](size_t) {
        engine.reset();
        parser.reset();
        feed(parser, unicode_data);
    });
}
static Registrar vt_unicode_cjk_4m_reg("vt/unicode_cjk_4m", &vt_unicode_cjk_4m);

static void vt_scroll_100k() {
    Runner runner(1);
    core::TerminalEngine engine(160, 48);
    core::VtParser parser(engine);
    runner.report("vt/scroll_100k", scroll_data.size() * 2, 3, [&](size_t round) {
        engine.reset();
        parser.reset();
        if (round % 2 == 0) feed(parser, scroll_data);
        else { feed(parser, region_data); feed(parser, scroll_data); engine.set_margins(0, 47); }
    });
}
static Registrar vt_scroll_100k_reg("vt/scroll_100k", &vt_scroll_100k);

} // namespace

namespace gritty::bench {
void register_vt() {}
} // namespace gritty::bench
