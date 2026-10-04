// NormalMatrix.h — RT4-T2 Part A host-side normal-matrix classification/pack.
//
// Pure glm math, no Vulkan includes: used by SceneResources upload and
// unit-tested in RT2Tests. Computes transpose(inverse(mat3(world))) with
// the EXACT scale-invariant support rule from the ticket:
//
//   1. Scan the 9 linear elements for finiteness FIRST (NaN handling must
//      never depend on max() behaviour).
//   2. s = max|Mij|; reject s == 0 (s is finite here by step 1).
//   3. Reject abs(det(M/s)) < 1e-12 or non-finite determinant.
//   4. Reject any non-finite element of the computed (single-precision)
//      inverse — the same direct inverse() expression the legacy shader
//      evaluates, so host/uploaded values stay comparable to it.
//   5. Otherwise pack transpose(inverse) column-major (w = 0).
//
// Out-of-support instances get an explicit identity matrix + supported=false;
// the caller (SceneResources) emits the loud `[NormalMatrix] out-of-support
// instance <id>` diagnostic. Identity+diagnostic is NEW behaviour, not
// parity: the current shader just calls inverse() with no classification.
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <cmath>

struct NormalMatrixPack
{
    glm::vec4 columns[3];  // column-major normal matrix, w forced to 0
    bool supported = true; // false = identity fallback, diagnostic required
};

inline NormalMatrixPack ComputeNormalMatrixPack(const glm::mat4& world)
{
    NormalMatrixPack out;
    const glm::mat3 m(world);

    // Step 1: finite scan before any max/normalize-style reduction.
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            if (!std::isfinite(m[c][r]))
            {
                out.columns[0] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                out.columns[1] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
                out.columns[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
                out.supported = false;
                return out;
            }

    // Step 2: scale estimate (all elements finite after step 1).
    float s = 0.0f;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            s = std::fabs(m[c][r]) > s ? std::fabs(m[c][r]) : s;
    if (s == 0.0f)
    {
        out.columns[0] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        out.columns[1] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        out.columns[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
        out.supported = false;
        return out;
    }

    // Step 3: scale-normalized determinant.
    const glm::mat3 ms = m / s;
    const float d = glm::determinant(ms);
    if (!std::isfinite(d) || std::fabs(d) < 1e-12f)
    {
        out.columns[0] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        out.columns[1] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        out.columns[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
        out.supported = false;
        return out;
    }

    // Step 4: direct single-precision inverse (same expression as the shader).
    const glm::mat3 inv = glm::inverse(m);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            if (!std::isfinite(inv[c][r]))
            {
                out.columns[0] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                out.columns[1] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
                out.columns[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
                out.supported = false;
                return out;
            }

    // Step 5: transpose + column-major pack, w forced to zero.
    const glm::mat3 n = glm::transpose(inv);
    out.columns[0] = glm::vec4(n[0], 0.0f);
    out.columns[1] = glm::vec4(n[1], 0.0f);
    out.columns[2] = glm::vec4(n[2], 0.0f);
    out.supported = true;
    return out;
}
