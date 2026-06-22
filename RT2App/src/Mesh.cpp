#include "Mesh.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tinyobjloader/tiny_obj_loader.h"

#include <cmath>

static vec3 TransformVertex(const vec3& v, const vec3& position, const vec3& rotation, float scale)
{
	vec3 result = v * scale;

	vec3 radians = glm::radians(rotation);

	// Rotate X
	float cosX = cos(radians.x), sinX = sin(radians.x);
	float y1 = result.y * cosX - result.z * sinX;
	float z1 = result.y * sinX + result.z * cosX;
	result.y = y1; result.z = z1;

	// Rotate Y
	float cosY = cos(radians.y), sinY = sin(radians.y);
	float x2 = result.x * cosY + result.z * sinY;
	float z2 = -result.x * sinY + result.z * cosY;
	result.x = x2; result.z = z2;

	// Rotate Z
	float cosZ = cos(radians.z), sinZ = sin(radians.z);
	float x3 = result.x * cosZ - result.y * sinZ;
	float y3 = result.x * sinZ + result.y * cosZ;
	result.x = x3; result.y = y3;

	result += position;
	return result;
}

bool Mesh::Load(const std::string& filepath, const vec3& position, const vec3& rotation, float scale,
                const shared_ptr<Material>& material)
{
	m_Triangles.clear();

	tinyobj::ObjReader reader;
	tinyobj::ObjReaderConfig reader_config;
	reader_config.mtl_search_path = "";

	if (!reader.ParseFromFile(filepath, reader_config))
	{
		return false;
	}

	const auto& attrib = reader.GetAttrib();
	const auto& shapes = reader.GetShapes();

	for (const auto& shape : shapes)
	{
		size_t index_offset = 0;
		for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++)
		{
			int fv = shape.mesh.num_face_vertices[f];
			if (fv != 3)
				continue; // only triangles supported; quads/ngons need triangulation

			tinyobj::index_t idx0 = shape.mesh.indices[index_offset + 0];
			tinyobj::index_t idx1 = shape.mesh.indices[index_offset + 1];
			tinyobj::index_t idx2 = shape.mesh.indices[index_offset + 2];
			index_offset += fv;

			vec3 v0 = TransformVertex(
				vec3(attrib.vertices[3 * idx0.vertex_index + 0],
				     attrib.vertices[3 * idx0.vertex_index + 1],
				     attrib.vertices[3 * idx0.vertex_index + 2]),
				position, rotation, scale);
			vec3 v1 = TransformVertex(
				vec3(attrib.vertices[3 * idx1.vertex_index + 0],
				     attrib.vertices[3 * idx1.vertex_index + 1],
				     attrib.vertices[3 * idx1.vertex_index + 2]),
				position, rotation, scale);
			vec3 v2 = TransformVertex(
				vec3(attrib.vertices[3 * idx2.vertex_index + 0],
				     attrib.vertices[3 * idx2.vertex_index + 1],
				     attrib.vertices[3 * idx2.vertex_index + 2]),
				position, rotation, scale);

			m_Triangles.add(make_shared<Triangle>(v0, v1, v2, material));
		}
	}

	if (IsLoaded())
	{
		std::vector<shared_ptr<Hittable>> objects = m_Triangles.objects;
		m_Bvh = make_shared<BVHNode>(objects, 0, objects.size());
	}

	return IsLoaded();
}