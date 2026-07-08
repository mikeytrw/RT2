#pragma once

#ifndef PRIMITIVE_GEOMETRY_H
#define PRIMITIVE_GEOMETRY_H

#include "MeshRegistry.h"

// ============================================================================
// PrimitiveGeometry — procedural mesh generators for the "Add" menu.
//
// Generates MeshData (vertices, indices, normals) for common primitives.
// UVs are left empty (procedural primitives don't need them by default).
//
// ============================================================================

namespace PrimitiveGeometry
{
	MeshData CreateCube(float size = 1.0f);
	MeshData CreateSphere(float radius = 0.5f, int segments = 24, int rings = 16);
	MeshData CreatePlane(float size = 5.0f);
}

#endif // PRIMITIVE_GEOMETRY_H