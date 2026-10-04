// ============================================================================
// RT4-T2 Part A: host normal-matrix classification + packing.
//
// CPU-side contract for ComputeNormalMatrixPack (RT2App/src/NormalMatrix.h):
// the exact s-rule support classification, identity fallback, w=0 padding
// and column-major packing. Host-vs-shader numeric equality is NOT proven
// here — that is the GPU oracle's job (actual-shader dispatch). These tests
// pin the classification boundaries and the fallback contract so a
// regression fails loudly on the CPU first.
// ============================================================================

#include <doctest/doctest.h>
#include "NormalMatrix.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <limits>

TEST_CASE("NormalMatrix identity is supported and exact")
{
    const NormalMatrixPack p = ComputeNormalMatrixPack(glm::mat4(1.0f));
    CHECK(p.supported);
    CHECK(p.columns[0] == glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
    CHECK(p.columns[1] == glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    CHECK(p.columns[2] == glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
}

TEST_CASE("NormalMatrix uniform scale inverts")
{
    glm::mat4 w(1.0f);
    w[0][0] = 2.0f; w[1][1] = 2.0f; w[2][2] = 2.0f;
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(p.supported);
    CHECK(p.columns[0].x == doctest::Approx(0.5f));
    CHECK(p.columns[1].y == doctest::Approx(0.5f));
    CHECK(p.columns[2].z == doctest::Approx(0.5f));
    CHECK(p.columns[0].w == 0.0f);
    CHECK(p.columns[1].w == 0.0f);
    CHECK(p.columns[2].w == 0.0f);
}

TEST_CASE("NormalMatrix non-uniform scale inverts per axis")
{
    glm::mat4 w(1.0f);
    w[0][0] = 2.0f; w[1][1] = 0.5f; w[2][2] = 4.0f;
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(p.supported);
    CHECK(p.columns[0].x == doctest::Approx(0.5f));
    CHECK(p.columns[1].y == doctest::Approx(2.0f));
    CHECK(p.columns[2].z == doctest::Approx(0.25f));
}

TEST_CASE("NormalMatrix rotation stays orthonormal")
{
    const glm::mat4 w = glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0.0f, 1.0f, 0.0f));
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(p.supported);
    // For a pure rotation the normal matrix equals the rotation itself.
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            CHECK(p.columns[c][r] == doctest::Approx(w[c][r]).epsilon(1e-6));
}

TEST_CASE("NormalMatrix mirror (negative determinant) is supported")
{
    glm::mat4 w(1.0f);
    w[0][0] = -1.0f;
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(p.supported);
    CHECK(p.columns[0].x == doctest::Approx(-1.0f));
}

TEST_CASE("NormalMatrix shear is supported")
{
    glm::mat4 w(1.0f);
    w[1][0] = 0.5f; // shear x into y
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(p.supported);
    // transpose(inverse) of [[1,0,0],[0.5,1,0],[0,0,1]] is [[1,-0.5,0],[0,1,0],[0,0,1]].
    CHECK(p.columns[0].x == doctest::Approx(1.0f));
    CHECK(p.columns[0].y == doctest::Approx(-0.5f));
    CHECK(p.columns[1].x == doctest::Approx(0.0f));
    CHECK(p.columns[1].y == doctest::Approx(1.0f));
}

TEST_CASE("NormalMatrix zero matrix is out of support with identity fallback")
{
    const NormalMatrixPack p = ComputeNormalMatrixPack(glm::mat4(0.0f));
    CHECK(!p.supported);
    CHECK(p.columns[0] == glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
    CHECK(p.columns[1] == glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    CHECK(p.columns[2] == glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
}

TEST_CASE("NormalMatrix singular scale is out of support")
{
    glm::mat4 w(1.0f);
    w[2][2] = 0.0f; // flatten z: det == 0
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(!p.supported);
    CHECK(p.columns[0] == glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
}

TEST_CASE("NormalMatrix near-singular scale below 1e-12 is out of support")
{
    glm::mat4 w(1.0f);
    w[2][2] = 1e-13f; // |det(M/s)| = 1e-13 < 1e-12
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(!p.supported);
}

TEST_CASE("NormalMatrix NaN element is out of support before any max")
{
    glm::mat4 w(1.0f);
    w[1][1] = std::numeric_limits<float>::quiet_NaN();
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(!p.supported);
    CHECK(p.columns[2] == glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
}

TEST_CASE("NormalMatrix infinite element is out of support")
{
    glm::mat4 w(1.0f);
    w[0][0] = std::numeric_limits<float>::infinity();
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(!p.supported);
}

TEST_CASE("NormalMatrix large uniform scale stays supported")
{
    glm::mat4 w(1.0f);
    w[0][0] = 1e10f; w[1][1] = 1e10f; w[2][2] = 1e10f;
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(p.supported);
    CHECK(p.columns[0].x == doctest::Approx(1e-10f));
}

TEST_CASE("NormalMatrix overflowing scale is out of support")
{
    // det(1e20 * I) = 1e60 overflows float32, so the direct single-precision
    // inverse the shader would evaluate is non-finite. The rule correctly
    // rejects this (identity fallback), even though the scale is uniform.
    glm::mat4 w(1.0f);
    w[0][0] = 1e20f; w[1][1] = 1e20f; w[2][2] = 1e20f;
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(!p.supported);
    CHECK(p.columns[0] == glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
}

TEST_CASE("NormalMatrix translation never affects the result")
{
    glm::mat4 w = glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, -50.0f, 3.0f));
    w[0][0] = 2.0f; w[1][1] = 3.0f; w[2][2] = 5.0f;
    const NormalMatrixPack p = ComputeNormalMatrixPack(w);
    CHECK(p.supported);
    CHECK(p.columns[0].x == doctest::Approx(0.5f));
    CHECK(p.columns[1].y == doctest::Approx(1.0f / 3.0f));
    CHECK(p.columns[2].z == doctest::Approx(0.2f));
    CHECK(p.columns[0].w == 0.0f);
    CHECK(p.columns[1].w == 0.0f);
    CHECK(p.columns[2].w == 0.0f);
}
