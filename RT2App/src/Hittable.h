#pragma once

#ifndef HITTABLE_H
#define HITTABLE_H

struct HitRecord {
	vec3 m_P = vec3(0.0, 0.0, 0.0);
	vec3 m_Normal = vec3(0.0, 0.0, 0.0);
	float m_T=0.0f;
	bool m_FrontFace = false;
	Material hitMaterial;
	int m_Index = 0;

	/*
	inline void set_front_face(const ray& r, const vec3& outward_normal) {
		front_face = dot(r.dir, outward_normal) > 0;
		normal = front_face ? outward_normal : -outward_normal;
	}*/

	inline void SetFaceNormal(const Ray& r, const vec3& outwardNormal) {
		m_FrontFace = glm::dot(r.direction(), outwardNormal) < 0;
		m_Normal = m_FrontFace ? outwardNormal : -outwardNormal;
	}
};



class Hittable {
public:

	virtual bool hit(const Ray& r, float t_min, float t_max, HitRecord& rec) const = 0;
	Material mat;
};


#endif