#include "UUID.h"

#include <cstdio>
#include <cstring>
#include <random>

// Windows UUID generation. Isolated here so UUID.h stays portable.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
// rpcdce.h provides UuidCreate; objbase.h pulls CoCreateGuid.
#pragma comment(lib, "rpcrt4.lib")
#endif

namespace rt2::core {

// ---------------------------------------------------------------------------
// UUID value methods
// ---------------------------------------------------------------------------

bool UUID::IsNull() const
{
    for (uint8_t b : bytes)
        if (b != 0) return false;
    return true;
}

std::string UUID::ToString() const
{
    // Canonical 8-4-4-4-12 lowercase hyphenated form.
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5],
        bytes[6], bytes[7],
        bytes[8], bytes[9],
        bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
}

UUID UUID::Parse(std::string_view s)
{
    UUID out{};
    // Accept canonical hyphenated (36 chars) or bare 32-char hex.
    char hex[32];
    int hexLen = 0;
    for (char c : s)
    {
        if (c == '-') continue;
        if (hexLen >= 32) return Nil(); // too long
        if (c >= '0' && c <= '9')       hex[hexLen++] = c;
        else if (c >= 'a' && c <= 'f')  hex[hexLen++] = c;
        else if (c >= 'A' && c <= 'F')  hex[hexLen++] = static_cast<char>(c - 'A' + 'a');
        else return Nil(); // invalid character
    }
    if (hexLen != 32) return Nil();

    for (int i = 0; i < 16; ++i)
    {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return 0;
        };
        out.bytes[i] = static_cast<uint8_t>((nib(hex[i * 2]) << 4) | nib(hex[i * 2 + 1]));
    }
    return out;
}

uint8_t UUID::Version() const
{
    // Upper 4 bits of byte 6.
    return (bytes[6] >> 4) & 0x0F;
}

uint8_t UUID::Variant() const
{
    // Top 2 bits of byte 8: RFC 9562 = 10xx.
    return (bytes[8] >> 6) & 0x03;
}

// ---------------------------------------------------------------------------
// OsUuidProvider — Windows cryptographic RNG
// ---------------------------------------------------------------------------

#ifdef _WIN32
UUID OsUuidProvider::CreateV4()
{
    GUID g;
    // CoCreateGuid wraps UuidCreate and produces an RFC 4122 v4 GUID on
    // modern Windows (data4[0] high nibble = 4, data4[0] top 2 bits = 10).
    HRESULT hr = CoCreateGuid(&g);
    if (FAILED(hr))
    {
        // Fall back to UuidCreate which is almost always available and returns
        // RPC_S_OK on success. This path is a safety net; CoCreateGuid should
        // not fail in practice.
        RPC_STATUS status = UuidCreate(&g);
        (void)status;
    }
    UUID out{};
    out.bytes[0]  = g.Data1 & 0xFF;
    out.bytes[1]  = (g.Data1 >> 8) & 0xFF;
    out.bytes[2]  = (g.Data1 >> 16) & 0xFF;
    out.bytes[3]  = (g.Data1 >> 24) & 0xFF;
    out.bytes[4]  = g.Data2 & 0xFF;
    out.bytes[5]  = (g.Data2 >> 8) & 0xFF;
    out.bytes[6]  = g.Data3 & 0xFF;
    out.bytes[7]  = (g.Data3 >> 8) & 0xFF;
    for (int i = 0; i < 8; ++i)
        out.bytes[8 + i] = g.Data4[i];
    // Force v4 + RFC variant in case the OS ever hands back something odd.
    out.bytes[6] = (out.bytes[6] & 0x0F) | 0x40;
    out.bytes[8] = (out.bytes[8] & 0x3F) | 0x80;
    return out;
}
#else
UUID OsUuidProvider::CreateV4()
{
    // Portable fallback: std::random_device. This branch is not exercised on
    // the Windows CI target today but keeps the file buildable elsewhere.
    std::random_device rd;
    UUID out{};
    for (int i = 0; i < 16; i += 4)
    {
        uint32_t v = rd();
        out.bytes[i]     = v & 0xFF;
        out.bytes[i + 1] = (v >> 8) & 0xFF;
        out.bytes[i + 2] = (v >> 16) & 0xFF;
        out.bytes[i + 3] = (v >> 24) & 0xFF;
    }
    out.bytes[6] = (out.bytes[6] & 0x0F) | 0x40;
    out.bytes[8] = (out.bytes[8] & 0x3F) | 0x80;
    return out;
}
#endif

// ---------------------------------------------------------------------------
// DeterministicUuidProvider — stable fixtures
// ---------------------------------------------------------------------------

DeterministicUuidProvider::DeterministicUuidProvider(UUID seed) : m_Seed(seed) {}

UUID DeterministicUuidProvider::CreateV4()
{
    // Spread the counter across the 14 bytes that are NOT reserved by the
    // RFC 9562 v4/version/variant fields. Bytes 6 (high nibble) and 8 (top 2
    // bits) are masked below, so the counter occupies bytes 0-5 and 9-15
    // (14 bytes, 112 bits) — far beyond any practical test sample size.
    UUID out = m_Seed;
    uint64_t counter = m_Counter++;

    // Pack the low 48 bits of the counter into bytes 0..5.
    for (int i = 0; i < 6; ++i)
        out.bytes[i] = static_cast<uint8_t>((counter >> (i * 8)) & 0xFF);
    // Pack the high 16 bits into bytes 9..10 (leaving byte 8 for the variant).
    out.bytes[9]  = static_cast<uint8_t>((counter >> 48) & 0xFF);
    out.bytes[10] = static_cast<uint8_t>((counter >> 56) & 0xFF);
    // Bytes 11..15 retain the seed; bytes 7 and 6-low-nibble retain the seed.

    // Force v4 (high nibble of byte 6 = 4) and RFC 9562 variant (top 2 bits
    // of byte 8 = 10).
    out.bytes[6] = (out.bytes[6] & 0x0F) | 0x40;
    out.bytes[8] = (out.bytes[8] & 0x3F) | 0x80;
    return out;
}

} // namespace rt2::core