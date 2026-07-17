#include <doctest/doctest.h>

#include "core/UUID.h"
#include "core/Error.h"

using namespace rt2::core;

// ============================================================================
// UUID value tests
// ============================================================================

TEST_CASE("UUID: nil is null and formats as all zeros")
{
    UUID n = UUID::Nil();
    CHECK(n.IsNull());
    CHECK(n.ToString() == "00000000-0000-0000-0000-000000000000");
    CHECK(n.Version() == 0);
    CHECK(n.Variant() == 0);
}

TEST_CASE("UUID: parse/format round trip")
{
    const char* canonical = "550e8400-e29b-41d4-a716-446655440000";
    UUID parsed = UUID::Parse(canonical);
    CHECK_FALSE(parsed.IsNull());
    CHECK(parsed.ToString() == canonical);
    CHECK(parsed.Version() == 4);
    CHECK(parsed.Variant() == 2); // 10xx -> top 2 bits = 10 = 2
}

TEST_CASE("UUID: parse accepts uppercase")
{
    UUID parsed = UUID::Parse("550E8400-E29B-41D4-A716-446655440000");
    CHECK(parsed.ToString() == "550e8400-e29b-41d4-a716-446655440000");
}

TEST_CASE("UUID: parse accepts bare 32-char hex")
{
    UUID parsed = UUID::Parse("550e8400e29b41d4a716446655440000");
    CHECK_FALSE(parsed.IsNull());
    CHECK(parsed.ToString() == "550e8400-e29b-41d4-a716-446655440000");
}

TEST_CASE("UUID: parse rejects malformed input")
{
    CHECK(UUID::Parse("").IsNull());
    CHECK(UUID::Parse("not-a-uuid").IsNull());
    CHECK(UUID::Parse("550e8400-e29b-41d4-a716").IsNull());            // too short
    CHECK(UUID::Parse("550e8400-e29b-41d4-a716-446655440000-extra").IsNull()); // too long
    CHECK(UUID::Parse("550e8400-e29b-41d4-a716-44665544000g").IsNull()); // bad hex char
}

TEST_CASE("UUID: equality and ordering")
{
    UUID a = UUID::Parse("550e8400-e29b-41d4-a716-446655440000");
    UUID b = UUID::Parse("550e8400-e29b-41d4-a716-446655440000");
    UUID c = UUID::Parse("660e8400-e29b-41d4-a716-446655440000");
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a < c);
    CHECK(c > a);
}

// ============================================================================
// OsUuidProvider tests
// ============================================================================

TEST_CASE("OsUuidProvider: produces unique valid v4 UUIDs")
{
    OsUuidProvider provider;
    std::unordered_map<UUID, int> seen;
    constexpr int N = 10000;
    for (int i = 0; i < N; ++i)
    {
        UUID u = provider.CreateV4();
        CHECK_FALSE(u.IsNull());
        CHECK(u.Version() == 4);
        CHECK(u.Variant() == 2);
        ++seen[u];
        CHECK(seen[u] == 1);
    }
    CHECK(seen.size() == N);
}

// ============================================================================
// DeterministicUuidProvider tests
// ============================================================================

TEST_CASE("DeterministicUuidProvider: produces stable sequence")
{
    DeterministicUuidProvider a;
    DeterministicUuidProvider b;
    UUID first = a.CreateV4();
    UUID second = a.CreateV4();
    CHECK(first == b.CreateV4());   // two providers with same seed yield same seq
    CHECK(second == b.CreateV4());
    CHECK(first != second);
    CHECK(first.Version() == 4);
    CHECK(first.Variant() == 2);
}

TEST_CASE("DeterministicUuidProvider: different seeds yield different sequences")
{
    UUID seedA = UUID::Parse("00000000-0000-4000-8000-000000000000");
    UUID seedB = UUID::Parse("11111111-1111-4111-8111-111111111111");
    DeterministicUuidProvider a(seedA);
    DeterministicUuidProvider b(seedB);
    CHECK(a.CreateV4() != b.CreateV4());
}

TEST_CASE("DeterministicUuidProvider: uniqueness within a large sample")
{
    DeterministicUuidProvider provider;
    std::unordered_map<UUID, int> seen;
    constexpr int N = 100000;
    for (int i = 0; i < N; ++i)
    {
        UUID u = provider.CreateV4();
        ++seen[u];
        CHECK(seen[u] == 1);
    }
    CHECK(seen.size() == N);
}

// ============================================================================
// Error tests
// ============================================================================

TEST_CASE("Error: default is ok")
{
    Error err;
    CHECK(err.IsOk());
    CHECK(err.code == Error::None);
    CHECK(err.Format() == "code=none");
}

TEST_CASE("Error: format with path and detail")
{
    Error err;
    err.code = Error::DuplicateUuid;
    err.path = "vertical-slice.rt2scene";
    err.detail = "two entities share 550e8400-e29b-41d4-a716-446655440000";
    std::string f = err.Format();
    CHECK(f.find("code=duplicate_uuid") != std::string::npos);
    CHECK(f.find("path=vertical-slice.rt2scene") != std::string::npos);
    CHECK(f.find("detail=two entities share") != std::string::npos);
}

TEST_CASE("Error: all code names resolve")
{
    CHECK(std::string(Error::CodeName(Error::None)) == "none");
    CHECK(std::string(Error::CodeName(Error::Io)) == "io");
    CHECK(std::string(Error::CodeName(Error::Parse)) == "parse");
    CHECK(std::string(Error::CodeName(Error::SchemaVersion)) == "schema_version");
    CHECK(std::string(Error::CodeName(Error::DuplicateUuid)) == "duplicate_uuid");
    CHECK(std::string(Error::CodeName(Error::MissingParent)) == "missing_parent");
    CHECK(std::string(Error::CodeName(Error::UnknownPrimitive)) == "unknown_primitive");
    CHECK(std::string(Error::CodeName(Error::MissingAsset)) == "missing_asset");
    CHECK(std::string(Error::CodeName(Error::InvalidRuntimeState)) == "invalid_runtime_state");
}