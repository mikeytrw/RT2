#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require

#include "pathtracer_shared.glsl"

// Shadow ray payload (location 2): visibility flag.
// Initialized to 0.0 before trace; this miss shader sets it to 1.0
// when the ray reaches the sky (unoccluded). Any-hit/closest-hit
// leaves it at 0.0 (ray hit something = occluded).
layout(location = 2) rayPayloadInEXT float shadowVisible;

void main()
{
    shadowVisible = 1.0;
}