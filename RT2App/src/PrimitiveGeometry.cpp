#include "PrimitiveGeometry.h"
#include <cmath>

MeshData PrimitiveGeometry::CreateCube(float size)
{
	float s = size * 0.5f;

	// 24 vertices (4 per face) so each face has its own flat normals
	// 36 indices (12 triangles)
	// Winding: indices ordered so cross(v1-v0, v2-v0) points outward
	// (matches declared normals), ensuring NEE light sampling works.
	MeshData mesh;
	mesh.name = "Cube";
	mesh.vertices = {
		// +X face (looking from +X towards origin)
		s, -s, -s,  s, -s,  s,  s,  s,  s,  s,  s, -s,
		// -X face (looking from -X towards origin)
		-s, -s,  s, -s, -s, -s, -s,  s, -s, -s,  s,  s,
		// +Y face (top, looking from +Y down)
		-s,  s, -s,  s,  s, -s,  s,  s,  s, -s,  s,  s,
		// -Y face (bottom, looking from -Y up)
		-s, -s,  s,  s, -s,  s,  s, -s, -s, -s, -s, -s,
		// +Z face (looking from +Z towards origin)
		-s, -s,  s,  s, -s,  s,  s,  s,  s, -s,  s,  s,
		// -Z face (looking from -Z towards origin)
		-s, -s, -s, -s,  s, -s,  s,  s, -s,  s, -s, -s,
	};
	mesh.indices = {
		// +X: cross(v1-v0, v2-v0) = cross((0,0,2s),(0,2s,2s)) = (-4s²,0,0) → inward
		// Fix: swap to (0,2,1),(0,3,2) → cross(v2-v0, v1-v0) = (4s²,0,0) → outward
		0, 2, 1,  0, 3, 2,
		// -X: cross(v1-v0, v2-v0) = cross((0,0,-2s),(0,2s,-2s)) = (4s²,0,0) → inward
		// Fix: swap to (4,6,5),(4,7,6)
		4, 6, 5,  4, 7, 6,
		// +Y: cross(v1-v0, v2-v0) = cross((2s,0,0),(2s,0,2s)) = (0,-4s²,0) → inward
		// Fix: swap to (8,10,9),(8,11,10)
		8, 10, 9,  8, 11, 10,
		// -Y: cross(v1-v0, v2-v0) = cross((2s,0,0),(2s,0,-2s)) = (0,4s²,0) → inward
		// Fix: swap to (12,14,13),(12,15,14)
		12, 14, 13,  12, 15, 14,
		// +Z: cross(v1-v0, v2-v0) = cross((2s,0,0),(2s,2s,0)) = (0,0,4s²) → outward ✓
		16, 17, 18,  16, 18, 19,
		// -Z: cross(v1-v0, v2-v0) = cross((0,2s,0),(2s,2s,0)) = (0,0,-4s²) → outward ✓
		20, 21, 22,  20, 22, 23,
	};
	mesh.normals = {
		1, 0, 0,  1, 0, 0,  1, 0, 0,  1, 0, 0,
		-1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0,
		0, 1, 0,  0, 1, 0,  0, 1, 0,  0, 1, 0,
		0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0,
		0, 0, 1,  0, 0, 1,  0, 0, 1,  0, 0, 1,
		0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1,
	};
	return mesh;
}

MeshData PrimitiveGeometry::CreateSphere(float radius, int segments, int rings)
{
	MeshData mesh;
	mesh.name = "Sphere";

	for (int ring = 0; ring <= rings; ++ring)
	{
		float phi = 3.14159265359f * float(ring) / float(rings);
		float y = radius * cosf(phi);
		float r = radius * sinf(phi);

		for (int seg = 0; seg <= segments; ++seg)
		{
			float theta = 2.0f * 3.14159265359f * float(seg) / float(segments);
			float x = r * cosf(theta);
			float z = r * sinf(theta);

			mesh.vertices.push_back(x);
			mesh.vertices.push_back(y);
			mesh.vertices.push_back(z);

			float nx = x / radius;
			float ny = y / radius;
			float nz = z / radius;
			mesh.normals.push_back(nx);
			mesh.normals.push_back(ny);
			mesh.normals.push_back(nz);
		}
	}

	for (int ring = 0; ring < rings; ++ring)
	{
		for (int seg = 0; seg < segments; ++seg)
		{
			uint32_t a = ring * (segments + 1) + seg;
			uint32_t b = a + 1;
			uint32_t c = a + (segments + 1);
			uint32_t d = c + 1;

			mesh.indices.push_back(a);
			mesh.indices.push_back(c);
			mesh.indices.push_back(b);

			mesh.indices.push_back(b);
			mesh.indices.push_back(c);
			mesh.indices.push_back(d);
		}
	}

	return mesh;
}

MeshData PrimitiveGeometry::CreatePlane(float size)
{
	float s = size * 0.5f;

	// Winding: cross(v1-v0, v2-v0) must point +Y (up, matching normals)
	// Vertices: (-s,0,-s), (s,0,-s), (s,0,s), (-s,0,s)
	// cross(v1-v0, v2-v0) = cross((2s,0,0),(2s,0,2s)) = (0,-4s²,0) → inward
	// Fix: swap indices to (0,2,1),(0,3,2) → cross(v2-v0, v1-v0) = (0,4s²,0) → outward
	MeshData mesh;
	mesh.name = "Plane";
	mesh.vertices = {
		-s, 0, -s,  s, 0, -s,  s, 0,  s, -s, 0,  s,
	};
	mesh.indices = {
		0, 2, 1,  0, 3, 2,
	};
	mesh.normals = {
		0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0,
	};
	return mesh;
}