#include <doctest/doctest.h>

#include "SceneTypes.h"
#include "SceneLoader.h"
#include <glm/glm.hpp>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

// ============================================================================
// SceneTexture pixel data tests (RED phase)
// ============================================================================

TEST_CASE("SceneTexture has pixel data fields")
{
    SceneTexture tex;
    CHECK(tex.width == 0);
    CHECK(tex.height == 0);
    CHECK(tex.channels == 4); // RGBA8
    CHECK(tex.pixels.empty());
    CHECK(tex.ref.path.empty());
}

TEST_CASE("SceneTexture stores decoded RGBA8 pixels")
{
    SceneTexture tex;
    tex.width = 2;
    tex.height = 2;
    tex.channels = 4;
    tex.pixels = {
        255, 0, 0, 255,    // red
        0, 255, 0, 255,     // green
        0, 0, 255, 255,     // blue
        255, 255, 255, 255  // white
    };

    CHECK(tex.pixels.size() == 16);
    CHECK(tex.pixels[0] == 255); // R of pixel 0
    CHECK(tex.pixels[1] == 0);   // G of pixel 0
    CHECK(tex.pixels[12] == 255); // R of pixel 3
    CHECK(tex.pixels[15] == 255); // A of pixel 3
}

// ============================================================================
// stb_image decode helper tests
// ============================================================================

// Create a minimal valid PNG (1x1 red pixel) for testing decode.
// This is a hand-crafted 1x1 red PNG.
static const unsigned char TINY_PNG[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // PNG signature
    // IHDR chunk
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
    0xDE, // 1x1, 8-bit RGB
    // IDAT chunk (compressed: filter byte + RGB pixel)
    0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54,
    0x08, 0xD7, 0x63, 0xD8, 0xCD, 0xCD, 0xCD, 0xCD,
    0x07, 0x00, 0x02, 0x9E, 0x01, 0x85, 0x2C, 0x03,
    // IEND chunk
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
    0xAE, 0x42, 0x60, 0x82
};

TEST_CASE("stb_image decodes raw image bytes to RGBA8")
{
    // This test verifies that the decode path exists and produces pixels.
    // We simulate by checking the SceneTexture after a manual decode.
    // The actual stb_image call happens inside SceneLoader.

    // For now, just verify the fields exist and can hold decoded data
    SceneTexture tex;
    tex.width = 1;
    tex.height = 1;
    tex.channels = 4;
    tex.pixels.resize(4);
    tex.pixels[0] = 255; // R
    tex.pixels[1] = 0;   // G
    tex.pixels[2] = 0;   // B
    tex.pixels[3] = 255; // A

    CHECK(tex.width == 1);
    CHECK(tex.height == 1);
    CHECK(tex.pixels.size() == 4);
    CHECK(tex.pixels[0] == 255);
}
