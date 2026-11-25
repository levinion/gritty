#include <doctest/doctest.h>
#include "core/kitty_graphics.hpp"
#include "core/vt_parser.hpp"
#include "core/terminal_session.hpp"
#include <png.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace gritty::core;
namespace {
std::string base64_encode(std::string_view in) {
    static constexpr const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : in) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 6) { bits -= 6; out.push_back(table[(acc >> bits) & 63]); }
    }
    if (bits) out.push_back(table[(acc << (6 - bits)) & 63]);
    return out;
}
}

TEST_CASE("KittyGraphicsEngine query and delete actions") {
    KittyGraphicsEngine engine;

    // Query action: a=q,i=42;
    std::string resp = engine.process_apc_sequence("Ga=q,i=42;", 0, 0);
    CHECK(resp == "\x1b_Gi=42;OK\x1b\\");

    // Delete action: a=d,i=42;
    std::string del_resp = engine.process_apc_sequence("Ga=d,i=42;", 0, 0);
    CHECK(del_resp == "\x1b_Gi=42;OK\x1b\\");
}

TEST_CASE("KittyGraphicsEngine RGBA image transmission and placement") {
    KittyGraphicsEngine engine;

    // 1x1 RGBA pixel: Red (255, 0, 0, 255) in base64:
    // Bytes: 0xFF, 0x00, 0x00, 0xFF -> Base64: "/wAA/w=="
    std::string seq = "Ga=T,f=32,s=1,v=1,i=10;/wAA/w==";
    std::string resp = engine.process_apc_sequence(seq, 5, 10);
    CHECK(resp == "\x1b_Gi=10;OK\x1b\\");

    const KittyImage* img = engine.get_image(10);
    REQUIRE(img != nullptr);
    CHECK(img->id == 10);
    CHECK(img->image.width() == 1);
    CHECK(img->image.height() == 1);

    uint32_t* pixel = img->image.data();
    CHECK(*pixel == 0xFFFF0000); // Host ARGB Little Endian

    // Relaxed Kitty base64 ignores whitespace in transmitted payloads.
    KittyGraphicsEngine spaced_engine;
    resp = spaced_engine.process_apc_sequence(
        "Ga=T,f=32,s=1,v=1,i=11;/w AA/w==", 0, 0);
    CHECK(resp == "\x1b_Gi=11;OK\x1b\\");
    const KittyImage* spaced = spaced_engine.get_image(11);
    REQUIRE(spaced != nullptr);
    CHECK(*spaced->image.data() == 0xFFFF0000);

    // Verify placement
    const auto& placements = engine.placements();
    REQUIRE(placements.size() == 1);
    CHECK(placements[0].image_id == 10);
    CHECK(placements[0].col == 5);
    CHECK(placements[0].row == 10);

    // Delete image 10
    engine.delete_image(10);
    CHECK(engine.get_image(10) == nullptr);
    CHECK(engine.placements().empty());
}

TEST_CASE("KittyGraphicsEngine chunked transmission") {
    KittyGraphicsEngine engine;

    // Chunk 1 (m=1): more data coming
    std::string chunk1 = "Ga=T,f=32,s=1,v=1,i=20,m=1;/w";
    std::string resp1 = engine.process_apc_sequence(chunk1, 0, 0);
    CHECK(resp1.empty()); // No response until complete

    // Chunk 2 (m=0): final chunk
    std::string chunk2 = "Gm=0;AA/w==";
    std::string resp2 = engine.process_apc_sequence(chunk2, 2, 4);
    CHECK(resp2 == "\x1b_Gi=20;OK\x1b\\");

    const KittyImage* img = engine.get_image(20);
    REQUIRE(img != nullptr);
    CHECK(img->image.width() == 1);
}

TEST_CASE("Kitty clear releases chunk buffers without breaking later transfers") {
    KittyGraphicsEngine engine;
    CHECK(engine.process_apc_sequence("Ga=T,f=32,s=1,v=1,i=31,m=1;/w", 0, 0).empty());
    CHECK(engine.process_apc_sequence("Gm=0;AA/w==", 0, 0) == "\x1b_Gi=31;OK\x1b\\");
    REQUIRE(engine.get_image(31) != nullptr);

    engine.clear();
    CHECK(engine.get_image(31) == nullptr);
    CHECK(engine.memory_bytes() == 0);

    CHECK(engine.process_apc_sequence("Ga=T,f=32,s=1,v=1,i=32,m=1;/w", 0, 0).empty());
    CHECK(engine.process_apc_sequence("Gm=0;AA/w==", 1, 1) == "\x1b_Gi=32;OK\x1b\\");
    CHECK(engine.get_image(32) != nullptr);
}

TEST_CASE("Kitty Unicode placeholder virtual placements and feature query") {
    KittyGraphicsEngine engine;
    int placements = 0;
    engine.set_placement_handler([&](const KittyPlacement&, pixman_image_t*) { ++placements; });

    // a=T with U=1 transmits and creates only a virtual placement.
    const std::string transmit = "Ga=T,U=1,i=7,c=2,r=1,f=32,s=1,v=1;/wAA/w==";
    CHECK(engine.process_apc_sequence(transmit, 4, 5) == "\x1b_Gi=7;OK\x1b\\");
    CHECK(placements == 0);
    CHECK(engine.placements().empty());
    REQUIRE(engine.virtual_placements().size() == 1);
    CHECK(engine.virtual_placements()[0].image_id == 7);
    CHECK(engine.virtual_placements()[0].cols == 2);
    CHECK(engine.virtual_placements()[0].rows == 1);
    CHECK(engine.find_virtual_placement(7, 0) != nullptr);
    CHECK(engine.find_virtual_placement(7, 99) == nullptr);

    // Feature query reports the supported placeholder and cursor-policy keys.
    const std::string reply = engine.process_apc_sequence("Ga=q,i=9,e=T,U=1,C=1", 0, 0);
    CHECK(reply.find("e=T") != std::string::npos);
    CHECK(reply.find("U=1") != std::string::npos);
    CHECK(reply.find("C=1") != std::string::npos);

    // Quiet mode suppresses responses (host applications must stay calm).
    CHECK(engine.process_apc_sequence("Ga=T,U=1,i=8,c=1,r=1,q=2,f=32,s=1,v=1;/wAA/w==", 0, 0).empty());

    // Deleting the image also drops its virtual placement.
    engine.delete_image(7);
    CHECK(engine.find_virtual_placement(7, 0) == nullptr);
}

TEST_CASE("tmux APC passthrough dispatches Kitty graphics APC") {
    gritty::core::TerminalEngine engine(80, 24);
    gritty::core::VtParser parser(engine);
    std::string received;
    parser.set_apc_handler([&](std::string_view payload) { received.assign(payload); });
    parser.feed("\x1bPtmu" "x;\x1b\x1b_Ga=q,i=42\x1b\x1b\\\x1b\\");
    CHECK(received == "Ga=q,i=42");
}

TEST_CASE("Kitty file and temp-file transmission use mmap and delete temp after read") {
    KittyGraphicsEngine engine;
    const auto dir = std::filesystem::temp_directory_path() / "gritty-kitty-file-test";
    std::filesystem::create_directories(dir);
    const auto image_path = dir / "image.rgba";
    { std::ofstream out(image_path, std::ios::binary); out.write("\xff\x00\x00\xff", 4); }
    const auto path_string = image_path.string();
    CHECK(engine.process_apc_sequence("Ga=T,t=f,f=32,s=1,v=1,i=60;" + base64_encode(path_string), 0, 0) ==
        "\x1b_Gi=60;OK\x1b\\");
    REQUIRE(engine.get_image(60) != nullptr);
    CHECK(engine.get_image(60)->image.data()[0] == 0xFFFF0000);

    const auto temp_path = dir / "temp.rgba";
    { std::ofstream out(temp_path, std::ios::binary); out.write("\x00\xff\x00\xff", 4); }
    CHECK(engine.process_apc_sequence("Ga=T,t=t,f=32,s=1,v=1,i=61;" + base64_encode(temp_path.string()), 0, 0) ==
        "\x1b_Gi=61;OK\x1b\\");
    CHECK_FALSE(std::filesystem::exists(temp_path)); // Consumed temp file is removed.
    REQUIRE(engine.get_image(61) != nullptr);
    CHECK(engine.get_image(61)->image.data()[0] == 0xFF00FF00);

    CHECK(engine.process_apc_sequence("Ga=T,t=f,f=32,s=1,v=1,i=62;" + base64_encode("/no/such/image"), 0, 0) ==
        "\x1b_Gi=62;ENOENT:failed to load image\x1b\\");
}

TEST_CASE("Kitty delete without i, source crop and C=0 cursor advance") {
    KittyGraphicsEngine graphics;
    int placements = 0;
    graphics.set_placement_handler([&](const KittyPlacement&, pixman_image_t*) { ++placements; });
    graphics.process_apc_sequence("Ga=T,i=70,f=32,s=1,v=1;/wAA/w==", 2, 3);
    REQUIRE(graphics.placements().size() == 1);
    CHECK(placements == 1);
    CHECK(graphics.process_apc_sequence("Ga=d,d=a", 0, 0) == "\x1b_Gi=0;OK\x1b\\");
    CHECK(graphics.placements().empty());
    CHECK(graphics.get_image(70) != nullptr); // Lowercase a keeps bitmap cache.
    CHECK(graphics.process_apc_sequence("Ga=d,i=70", 0, 0) == "\x1b_Gi=70;OK\x1b\\");
    CHECK(graphics.get_image(70) == nullptr); // Explicit i= deletes the bitmap.

    graphics.process_apc_sequence("Ga=T,i=71,f=32,s=1,v=1,x=1,y=2,w=3,h=4,C=0;/wAA/w==", 1, 1);
    REQUIRE(graphics.placements().size() == 1);
    CHECK(graphics.placements()[0].src_x == 1);
    CHECK(graphics.placements()[0].src_y == 2);
    CHECK(graphics.placements()[0].src_w == 3);
    CHECK(graphics.placements()[0].src_h == 4);
    CHECK(graphics.placements()[0].cursor_policy == 0);

    gritty::core::SessionConfig config;
    gritty::core::TerminalSession session(config, nullptr, nullptr, nullptr);
    const auto before_col = session.engine().cursor_col();
    const auto before_row = session.engine().cursor_row();
    session.engine().kitty_graphics().process_apc_sequence(
        "Ga=T,i=72,f=32,s=1,v=1,c=3,r=2,C=0;/wAA/w==", before_col, before_row);
    CHECK(session.engine().cursor_col() == before_col + 3);
    CHECK(session.engine().cursor_row() == before_row + 1); // C=0 ends on the last image row.
}

TEST_CASE("Kitty shared-memory query, temp roots and same-id replacement accounting") {
    KittyGraphicsEngine graphics;
    CHECK(graphics.process_apc_sequence("Ga=q,t=s,i=80", 0, 0) ==
        "\x1b_Gi=80;ENOTSUP:shared memory not supported\x1b\\");

    // macOS normally points TMPDIR at /var/folders; /tmp must remain valid too.
    const char* previous_tmpdir = std::getenv("TMPDIR");
    const std::string saved = previous_tmpdir ? previous_tmpdir : "";
    ::setenv("TMPDIR", "/definitely/not/a/temp/root", 1);
    const auto path = std::filesystem::path("/tmp") / "gritty-temp-root-test.rgba";
    { std::ofstream out(path, std::ios::binary); out.write("\xff\x00\x00\xff", 4); }
    CHECK(graphics.process_apc_sequence("Ga=T,t=t,f=32,s=1,v=1,i=81;" + base64_encode(path.string()), 0, 0) ==
        "\x1b_Gi=81;OK\x1b\\");
    if (previous_tmpdir) ::setenv("TMPDIR", saved.c_str(), 1);
    else ::unsetenv("TMPDIR");

    KittyGraphicsEngine bounded(8); // Two 4-byte frames would exceed it if miscounted.
    const std::string pixel = "/wAA/w==";
    CHECK(bounded.process_apc_sequence("Ga=T,i=82,f=32,s=1,v=1;" + pixel, 0, 0).find("OK") != std::string::npos);
    CHECK(bounded.memory_bytes() == 4);
    CHECK(bounded.process_apc_sequence("Ga=T,i=82,f=32,s=1,v=1;" + pixel, 0, 0).find("OK") != std::string::npos);
    CHECK(bounded.memory_bytes() == 4); // Overwrite must release the old bitmap first.
}

TEST_CASE("Kitty RGBA image is premultiplied for Pixman") {
    KittyGraphicsEngine engine;
    // RGBA(255, 0, 0, 128): /wAAgA== (f=32). Pixman expects ARGB 0x80800000.
    engine.process_apc_sequence("Ga=T,i=73,f=32,s=1,v=1;/wAAgA==", 0, 0);
    REQUIRE(engine.get_image(73) != nullptr);
    CHECK(engine.get_image(73)->image.data()[0] == 0x80800000);
}

TEST_CASE("Kitty graphics accumulates chunked transmissions") {
    KittyGraphicsEngine graphics;
    graphics.set_placement_handler([](const KittyPlacement&, pixman_image_t*) {});
    CHECK(graphics.process_apc_sequence(
        "Ga=T,q=2,i=90,f=32,s=1,v=1,c=4,r=2,U=1,m=1;/w", 0, 0).empty());
    CHECK(graphics.process_apc_sequence("Ga=T,q=2,m=1;AA/", 0, 0).empty());
    CHECK(graphics.process_apc_sequence("Ga=T,q=2;w==", 0, 0).empty());

    const KittyImage* image = graphics.get_image(90);
    REQUIRE(image != nullptr);
    REQUIRE(image->image.width() == 1);
    CHECK(image->image.data()[0] == 0xFFFF0000u);
    REQUIRE(graphics.virtual_placements().size() == 1);
    CHECK(graphics.virtual_placements()[0].image_id == 90);
    CHECK(graphics.virtual_placements()[0].cols == 4);
    CHECK(graphics.virtual_placements()[0].rows == 2);
}

TEST_CASE("Kitty graphics bounds oversized dimensions/chunks, replaces same id and notifies LRU eviction") {
    KittyGraphicsEngine small(64);
    // Oversized dimensions rejected before allocation.
    CHECK(small.process_apc_sequence("Ga=t,i=1,f=32,s=100000,v=100000;/wAA/w==", 0, 0)
              .find("ENOENT") != std::string::npos);

    // Oversized chunked transfer rejected cleanly.
    const std::string big_chunk(8192, 'A');
    CHECK(small.process_apc_sequence("Ga=t,i=2,f=32,s=1,v=1,m=1;" + big_chunk, 0, 0)
              .find("EFBIG") != std::string::npos);

    // Same-ID replacement updates pixels instead of keeping stale image.
    KittyGraphicsEngine engine(8);
    std::vector<uint32_t> deleted_ids;
    engine.set_delete_handler([&](uint32_t id) { deleted_ids.push_back(id); });
    REQUIRE(engine.process_apc_sequence("Ga=T,i=10,f=32,s=1,v=1;/wAA/w==", 0, 0).find("OK") != std::string::npos);
    REQUIRE(engine.get_image(10) != nullptr);
    CHECK(engine.get_image(10)->image.data()[0] == 0xFFFF0000u);
    REQUIRE(engine.process_apc_sequence("Ga=T,i=10,f=32,s=1,v=1;AP8A/w==", 0, 0).find("OK") != std::string::npos);
    REQUIRE(engine.get_image(10) != nullptr);
    CHECK(engine.get_image(10)->image.data()[0] == 0xFF00FF00u);

    // LRU eviction deletes oldest image and notifies delete_handler_.
    REQUIRE(engine.process_apc_sequence("Ga=T,i=11,f=32,s=1,v=1;/wAA/w==", 0, 0).find("OK") != std::string::npos);
    REQUIRE(engine.process_apc_sequence("Ga=T,i=12,f=32,s=1,v=1;/wAA/w==", 0, 0).find("OK") != std::string::npos);
    CHECK(engine.get_image(10) == nullptr);
    CHECK(engine.get_image(11) != nullptr);
    CHECK(engine.get_image(12) != nullptr);
    REQUIRE(deleted_ids.size() == 1);
    CHECK(deleted_ids[0] == 10);
}

TEST_CASE("Kitty image.nvim workflow: d=i keeps image cache, d=I drops, S/O slicing, X/Y offsets, >INT32 ID") {
    KittyGraphicsEngine graphics;
    // Omitted action defaults to Transmit ('t'), not TransmitAndDisplay.
    const uint32_t high_id = 3000000000u;
    CHECK(graphics.process_apc_sequence(
              "Gi=" + std::to_string(high_id) + ",f=32,s=1,v=1;/wAA/w==", 0, 0) ==
          "\x1b_Gi=" + std::to_string(high_id) + ";OK\x1b\\");
    REQUIRE(graphics.get_image(high_id) != nullptr);
    CHECK(graphics.placements().empty());

    // Place with p=1 and X/Y pixel offsets.
    CHECK(graphics.process_apc_sequence(
              "Ga=p,i=" + std::to_string(high_id) + ",p=1,X=3,Y=4,c=2,r=2;", 1, 2) ==
          "\x1b_Gi=" + std::to_string(high_id) + ";OK\x1b\\");
    REQUIRE(graphics.placements().size() == 1);
    CHECK(graphics.placements()[0].x_offset == 3);
    CHECK(graphics.placements()[0].y_offset == 4);

    // Re-placing same (image_id, placement_id) replaces instead of duplicating.
    CHECK(graphics.process_apc_sequence(
              "Ga=p,i=" + std::to_string(high_id) + ",p=1,X=5,Y=6,c=2,r=2;", 2, 3) ==
          "\x1b_Gi=" + std::to_string(high_id) + ";OK\x1b\\");
    REQUIRE(graphics.placements().size() == 1);
    CHECK(graphics.placements()[0].col == 2);
    CHECK(graphics.placements()[0].x_offset == 5);

    // Lowercase d=i removes placement(s) while retaining cached image for a=p reuse.
    CHECK(graphics.process_apc_sequence(
              "Ga=d,d=i,i=" + std::to_string(high_id) + ",q=2;", 0, 0).empty());
    CHECK(graphics.placements().empty());
    REQUIRE(graphics.get_image(high_id) != nullptr);

    // Subsequent a=p succeeds immediately without retransmission.
    CHECK(graphics.process_apc_sequence(
              "Ga=p,i=" + std::to_string(high_id) + ",p=2;", 0, 0) ==
          "\x1b_Gi=" + std::to_string(high_id) + ";OK\x1b\\");
    REQUIRE(graphics.placements().size() == 1);

    // Uppercase d=I deletes placement AND image data once no placements remain.
    CHECK(graphics.process_apc_sequence(
              "Ga=d,d=I,i=" + std::to_string(high_id) + ",p=2;", 0, 0) ==
          "\x1b_Gi=" + std::to_string(high_id) + ";OK\x1b\\");
    CHECK(graphics.placements().empty());
    CHECK(graphics.get_image(high_id) == nullptr);

    // File transmission with O= offset and S= size reads the requested byte slice.
    const auto dir = std::filesystem::temp_directory_path() / "gritty-kitty-slice-test";
    std::filesystem::create_directories(dir);
    const auto slice_path = dir / "slice.bin";
    {
        std::ofstream out(slice_path, std::ios::binary);
        out.write("HDR!", 4);
        out.write("\x00\x00\xff\xff", 4); // RGBA(0, 0, 255, 255) -> 0xFF0000FF
        out.write("TAIL", 4);
    }
    CHECK(graphics.process_apc_sequence(
              "Ga=t,t=f,f=32,s=1,v=1,O=4,S=4,i=95;" + base64_encode(slice_path.string()), 0, 0) ==
          "\x1b_Gi=95;OK\x1b\\");
    REQUIRE(graphics.get_image(95) != nullptr);
    CHECK(graphics.get_image(95)->image.data()[0] == 0xFF0000FFu);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Kitty virtual_placement_change_handler fires and targeted delete selectors work") {
    KittyGraphicsEngine graphics;
    int virtual_changes = 0;
    graphics.set_virtual_placement_change_handler([&]() { ++virtual_changes; });

    CHECK(graphics.process_apc_sequence(
              "Ga=T,U=1,i=101,c=2,r=2,x=1,y=2,w=3,h=4,f=32,s=4,v=4;" +
              base64_encode(std::string(64, '\xff')), 0, 0) == "\x1b_Gi=101;OK\x1b\\");
    CHECK(virtual_changes == 1);
    const auto* vp = graphics.find_virtual_placement(101, 0);
    REQUIRE(vp != nullptr);
    CHECK(vp->src_x == 1);
    CHECK(vp->src_y == 2);
    CHECK(vp->src_w == 3);
    CHECK(vp->src_h == 4);

    // Deleting the virtual placement triggers change handler too.
    CHECK(graphics.process_apc_sequence("Ga=d,d=i,i=101;", 0, 0) == "\x1b_Gi=101;OK\x1b\\");
    CHECK(virtual_changes == 2);

    // Direct placements: test d=p (by cell), d=x (by col), d=y (by row), d=r (by id range).
    graphics.process_apc_sequence("Ga=T,i=201,c=2,r=2,f=32,s=1,v=1;/wAA/w==", 1, 1);
    graphics.process_apc_sequence("Ga=T,i=202,c=2,r=2,f=32,s=1,v=1;/wAA/w==", 5, 1);
    graphics.process_apc_sequence("Ga=T,i=203,c=2,r=2,f=32,s=1,v=1;/wAA/w==", 9, 5);
    REQUIRE(graphics.placements().size() == 3);

    // d=p at 1-based cell (2,2) hits only placement at (1,1) spanning [1..2,1..2].
    graphics.process_apc_sequence("Ga=d,d=p,x=2,y=2;", 0, 0);
    REQUIRE(graphics.placements().size() == 2);
    CHECK(graphics.placements()[0].image_id == 202);

    // d=x at 1-based col 6 hits only placement at (5,1) spanning cols [5..6].
    graphics.process_apc_sequence("Ga=d,d=x,x=6;", 0, 0);
    REQUIRE(graphics.placements().size() == 1);
    CHECK(graphics.placements()[0].image_id == 203);

    // d=r range [203..205] deletes placement and leaves other cached images intact.
    graphics.process_apc_sequence("Ga=d,d=r,x=203,y=205;", 0, 0);
    CHECK(graphics.placements().empty());
    CHECK(graphics.get_image(201) != nullptr);
    CHECK(graphics.get_image(203) != nullptr);
}

TEST_CASE("Kitty PNG decoder handles Grayscale, Gray+Alpha, and opaque Palette PNGs") {
    auto encode_png = [](int color_type, const std::vector<uint8_t>& row,
                         const std::vector<png_color>& palette = {}) -> std::string {
        std::string out;
        png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        png_infop info = png_create_info_struct(png);
        REQUIRE(png != nullptr);
        REQUIRE(info != nullptr);
        png_set_write_fn(png, &out,
            [](png_structp p, png_bytep data, png_size_t len) {
                static_cast<std::string*>(png_get_io_ptr(p))->append(
                    reinterpret_cast<const char*>(data), len);
            },
            [](png_structp) {});
        png_set_IHDR(png, info, 1, 1, 8, color_type, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        if (!palette.empty()) {
            png_set_PLTE(png, info, palette.data(), static_cast<int>(palette.size()));
        }
        png_write_info(png, info);
        png_bytep row_ptr = const_cast<png_bytep>(row.data());
        png_write_image(png, &row_ptr);
        png_write_end(png, nullptr);
        png_destroy_write_struct(&png, &info);
        return out;
    };

    KittyGraphicsEngine graphics;
    // Grayscale (color_type=0): pixel 0x40 -> 0xFF404040
    const auto gray_png = encode_png(PNG_COLOR_TYPE_GRAY, {0x40});
    CHECK(graphics.process_apc_sequence("Ga=t,f=100,i=301;" + base64_encode(gray_png), 0, 0) ==
          "\x1b_Gi=301;OK\x1b\\");
    REQUIRE(graphics.get_image(301) != nullptr);
    CHECK(graphics.get_image(301)->image.data()[0] == 0xFF404040u);

    // Gray+Alpha (color_type=4): pixel (0x80, 0xFF) -> 0xFF808080
    const auto ga_png = encode_png(PNG_COLOR_TYPE_GRAY_ALPHA, {0x80, 0xFF});
    CHECK(graphics.process_apc_sequence("Ga=t,f=100,i=302;" + base64_encode(ga_png), 0, 0) ==
          "\x1b_Gi=302;OK\x1b\\");
    REQUIRE(graphics.get_image(302) != nullptr);
    CHECK(graphics.get_image(302)->image.data()[0] == 0xFF808080u);

    // Opaque Palette without tRNS (color_type=3): RGB(0x12, 0x34, 0x56) -> 0xFF123456
    const auto pal_png = encode_png(PNG_COLOR_TYPE_PALETTE, {0x00}, {{0x12, 0x34, 0x56}});
    CHECK(graphics.process_apc_sequence("Ga=t,f=100,i=303;" + base64_encode(pal_png), 0, 0) ==
          "\x1b_Gi=303;OK\x1b\\");
    REQUIRE(graphics.get_image(303) != nullptr);
    CHECK(graphics.get_image(303)->image.data()[0] == 0xFF123456u);
}

TEST_CASE("Kitty d=P/d=Z preserve still-referenced images and distinguish anonymous placements") {
    TerminalEngine engine(20, 10);
    auto& gfx = engine.kitty_graphics();
    uint64_t next_uid = 1;
    (void)next_uid;
    gfx.set_placement_handler([&](KittyPlacement& placement, pixman_image_t* image) {
        if (!image) return;
        placement.cols = placement.cols > 0 ? placement.cols : 1;
        placement.rows = placement.rows > 0 ? placement.rows : 1;
        PlacedImage placed;
        placed.image.reset(pixman_image_ref(image));
        placed.uid = placement.uid;
        placed.image_id = placement.image_id;
        placed.placement_id = placement.placement_id;
        placed.col = placement.col;
        placed.row = placement.row;
        placed.cols = placement.cols;
        placed.rows = placement.rows;
        placed.z_index = placement.z_index;
        engine.place_image(std::move(placed));
    });
    gfx.set_delete_handler([&](uint32_t image_id) { engine.remove_images_by_id(image_id); });
    gfx.set_placement_delete_handler([&](const KittyPlacement& placement) {
        engine.remove_image_by_uid(placement.uid);
    });

    // Two anonymous (p=0) placements of image 401 at different cells.
    CHECK(gfx.process_apc_sequence("Ga=T,i=401,c=2,r=2,f=32,s=1,v=1;/wAA/w==", 0, 0) ==
          "\x1b_Gi=401;OK\x1b\\");
    CHECK(gfx.process_apc_sequence("Ga=p,i=401,c=2,r=2;", 6, 0) == "\x1b_Gi=401;OK\x1b\\");
    REQUIRE(gfx.placements().size() == 2);
    REQUIRE(engine.images().size() == 2);

    // d=P at (1,1) deletes only the first placement and keeps image 401 alive because (6,0) remains!
    CHECK(gfx.process_apc_sequence("Ga=d,d=P,x=1,y=1;", 0, 0) == "\x1b_Gi=0;OK\x1b\\");
    REQUIRE(gfx.placements().size() == 1);
    REQUIRE(engine.images().size() == 1);
    CHECK(engine.images()[0].col == 6);
    CHECK(gfx.get_image(401) != nullptr);

    // Virtual placement on z=-5 freed by d=Z even without direct placements.
    CHECK(gfx.process_apc_sequence("Ga=T,U=1,i=402,c=2,r=2,z=-5,f=32,s=1,v=1;/wAA/w==", 0, 0) ==
          "\x1b_Gi=402;OK\x1b\\");
    REQUIRE(gfx.get_image(402) != nullptr);
    gfx.process_apc_sequence("Ga=d,d=Z,z=-5;", 0, 0);
    CHECK(gfx.virtual_placements().empty());
    CHECK(gfx.get_image(402) == nullptr);
}
