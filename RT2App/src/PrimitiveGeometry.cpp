#include "PrimitiveGeometry.h"
#include <cmath>

MeshData PrimitiveGeometry::CreateCube(float size)
{
	float s = size * 0.5f;

	// 24 vertices (4 per face) so each face has its own flat normals
	// 36 indices (12 triangles)
	MeshData mesh;
	mesh.name = "Cube";
	mesh.vertices = {
		// +X face
		s, -s, -s,  s, -s,  s,  s,  s,  s,  s,  s, -s,
		// -X face
		-s, -s,  s, -s, -s, -s, -s,  s, -s, -s,  s,  s,
		// +Y face (top)
		-s,  s, -s,  s,  s, -s,  s,  s,  s, -s,  s,  s,
		// -Y face (bottom)
		-s, -s,  s,  s, -s,  s,  s, -s, -s, -s, -s, -s,
		// +Z face
		-s, -s,  s,  s, -s,  s,  s,  s,  s, -s,  s,  s,
		// -Z face
		-s, -s, -s, -s,  s, -s,  s,  s, -s,  s, -s, -s,
	};
	mesh.indices = {
		0, 1, 2,  0, 2, 3,
		4, 5, 6,  4, 6, 7,
		8, 9, 10, 8, 10, 11,
		12, 13, 14, 12, 14, 15,
		16, 17, 18, 16, 18, 19,
		20, 21, 22, 20, 22, 23,
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

	MeshData mesh;
	mesh.name = "Plane";
	mesh.vertices = {
		-s, 0, -s,  s, 0, -s,  s, 0,  s, -s, 0,  s,
	};
	mesh.indices = {
		0, 1, 2, 0, 2, 3,
	};
	mesh.normals = {
		0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0,
	};
	return mesh;
}