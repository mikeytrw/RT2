// material_resolve.glsl — canonical per-triangle material resolution.
//
// Resolves the material index for a hit (instanceID, primitiveID) using the
// closest-hit path: instanceMatOffsets[instanceID] + primitiveID →
// instanceMaterialIndices[...]. Used by closesthit.rchit, anyhit.rahit,
// shadow.rahit, and the ray-query path (restir_gi) so all paths agree.
//
// Previously any-hit/shadow-any-hit used gl_InstanceCustomIndexEXT directly,
// which is a different (per-instance) semantic. This helper unifies them on
// the per-triangle path. Include this AFTER the buffers it references
// (instanceMatOffsets, instanceMaterialIndices) are declared.

// Returns the per-triangle material index for (instanceID, primitiveID).
// Returns 0xFFFFFFFFu if the index is out of range (caller should treat as
// default/opaque).
uint resolveMaterialIndex(uint instanceID, uint primitiveID)
{
    uint offset = instanceMatOffsets[instanceID] + primitiveID;
    uint matIdx  = instanceMaterialIndices[offset];
    return matIdx;
}