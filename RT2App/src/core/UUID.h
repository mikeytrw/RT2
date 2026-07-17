#pragma once

#ifndef RT2_CORE_UUID_H
#define RT2_CORE_UUID_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <functional>

// ============================================================================
// UUID — 128-bit RFC 9562 identifier.
//
// The engine uses UUID v4 (random) for authored entity identity. Generation is
// injectable through IUuidProvider so tests and deterministic fixtures can use
// a deterministic provider while production uses the OS cryptographic RNG.
//
// The render sampling seed must never feed UUID generation. UUID generation is
// a pure identity concern, not a rendering or simulation concern.
//
// Policy (see docs/game-engine-development-plan.md):
//   - Authored entities:        v4 from OS cryptographic RNG.
//   - Asset IDs:                separate stable IDs in the asset database.
//   - Linked imported nodes:    v5 from asset ID + canonical node key (future).
//   - Ordinary import entities: fresh v4, retained in the native scene.
//   - Runtime cloning:          preserve UUIDs (same logical entities).
//   - Duplication:              fresh v4 for the duplicated subtree, remap refs.
//   - Undo/delete restoration:  restore the original IDs.
//
// ============================================================================

namespace rt2::core {

class UUID
{
public:
    // 128 bits stored as 16 octets (RFC 9562 layout: time-low/time-mid/
    // time-high-and-version/clock-seq-and-reserved/node).
    std::array<uint8_t, 16> bytes{};

    UUID() = default;
    explicit UUID(std::array<uint8_t, 16> b) : bytes(b) {}

    // Nil UUID (all zeros) per RFC 9562 §5.1.
    static UUID Nil() { return UUID{}; }

    // True if all bytes are zero.
    bool IsNull() const;

    // Canonical lowercase hyphenated form, e.g.
    //   "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
    std::string ToString() const;

    // Parse a canonical hyphenated string. Returns Nil() on malformed input;
    // the caller should check IsNull() on the result. Accepts uppercase or
    // lowercase hex. Hyphens are required at the canonical positions but a
    // 32-char hex string without hyphens is also accepted for robustness.
    static UUID Parse(std::string_view s);

    // RFC 9562 version field (4 for v4, 0 for nil).
    uint8_t Version() const;

    // RFC 9562 variant field (top bits of byte 8: 10xx = RFC 9562).
    uint8_t Variant() const;

    bool operator==(const UUID& other) const { return bytes == other.bytes; }
    bool operator!=(const UUID& other) const { return bytes != other.bytes; }
    bool operator<(const UUID& other) const { return bytes < other.bytes; }
    bool operator>(const UUID& other) const { return bytes > other.bytes; }
    bool operator<=(const UUID& other) const { return bytes <= other.bytes; }
    bool operator>=(const UUID& other) const { return bytes >= other.bytes; }
};

static_assert(sizeof(UUID) == 16, "UUID must be 128 bits");

// Injectable generation interface. Production uses OsUuidProvider (defined in
// UUID.cpp); tests use DeterministicUuidProvider.
class IUuidProvider
{
public:
    virtual ~IUuidProvider() = default;
    virtual UUID CreateV4() = 0;
};

// OS-backed cryptographic v4 generator. On Windows uses CoCreateGuid/
// UuidCreate. Keeps Windows headers out of UUID.h.
class OsUuidProvider final : public IUuidProvider
{
public:
    UUID CreateV4() override;
};

// Deterministic v4 generator for tests and fixtures. Produces a stable
// sequence of v4 UUIDs from a 16-byte seed by treating the seed as a 128-bit
// counter prefix and incrementing. The output is valid RFC 9562 v4 (version
// 4, RFC variant) even though it is not random.
class DeterministicUuidProvider final : public IUuidProvider
{
public:
    // Default seed = all zeros. Each call increments the low 64 bits and
    // re-applies the v4/version/variant bits.
    explicit DeterministicUuidProvider(UUID seed = UUID::Nil());

    UUID CreateV4() override;

private:
    UUID m_Seed{};
    uint64_t m_Counter = 0;
};

} // namespace rt2::core

namespace std
{
template<>
struct hash<rt2::core::UUID>
{
    size_t operator()(const rt2::core::UUID& u) const noexcept
    {
        // Treat the 16 bytes as two uint64_t and combine with a cheap mixer.
        const uint64_t* p = reinterpret_cast<const uint64_t*>(u.bytes.data());
        uint64_t a = p[0];
        uint64_t b = p[1];
        a ^= b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2);
        return std::hash<uint64_t>{}(a);
    }
};
} // namespace std

#endif // RT2_CORE_UUID_H