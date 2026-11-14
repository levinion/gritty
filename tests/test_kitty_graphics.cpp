#include <doctest/doctest.h>
#include "core/kitty_graphics.hpp"

using namespace gritty::core;

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
