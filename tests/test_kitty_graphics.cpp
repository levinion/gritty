#include <doctest/doctest.h>
#include "core/kitty_graphics.hpp"
#include "core/vt_parser.hpp"
#include "core/terminal_session.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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
