#pragma once

#ifndef MESH_H
#define MESH_H

#include "Utility.h"
#include "Triangle.h"
#include "BVHNode.h"

#include <string>
#include <utility>
#include <vector>

class Mesh
{
public:
	Mesh() = default;

	bool Load(const std::string& filepath, const vec3& position, const vec3& rotation, float scale,
	          const shared_ptr<Material>& material);

	bool LoadFromGeometry(const std::vector<float>& vertices, const std::vector<float>& normals,
	                      const std::vector<uint32_t>& indices, const vec3& position,
	                      const vec3& rotation, float scale, const shared_ptr<Material>& material);

	void Clear() { m_Triangles.clear(); m_Bvh.reset(); }

	bool IsLoaded() const { return !m_Triangles.objects.empty(); }

	shared_ptr<Hittable> GetBvhRoot() const { return m_Bvh; }
	size_t GetTriangleCount() const { return m_Triangles.objects.size(); }
	shared_ptr<BVHNode> GetBvhNode() const { return std::dynamic_pointer_cast<BVHNode>(m_Bvh); }

	std::pair<std::vector<float>, std::vector<uint32_t>> GetRawVertexData() const { return { m_RawVertices, m_RawIndices }; }

private:
	HittableList m_Triangles;
	shared_ptr<Hittable> m_Bvh;
	std::vector<float> m_RawVertices;
	std::vector<uint32_t> m_RawIndices;
};

#endif // !MESH_H