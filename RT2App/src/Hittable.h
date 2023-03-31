#pragma once

#ifndef HITTABLE_H
#define HITTABLE_H

class Material;

struct HitRecord {
	vec3 m_P = vec3(0.0, 0.0, 0.0);
	vec3 m_Normal = vec3(0.0, 0.0, 0.0);
	float m_T=0.0f;
	bool m_FrontFace = false;
	shared_ptr<Material> matPtr;
	int m_Index = 0;

	inline void SetFaceNormal(const Ray& r, const vec3& outwardNormal) {
		m_FrontFace = glm::dot(r.direction(), outwardNormal) < 0;
		m_Normal = m_FrontFace ? outwardNormal : -outwardNormal;
	}
};



class Hittable {
public:
	Hittable() {};
	Hittable(shared_ptr<Material> matPtr) : mMatPtr(matPtr) {};

	virtual bool hit(const Ray& r, float t_min, float t_max, HitRecord& rec) const = 0;
	shared_ptr<Material> mMatPtr;
};


#endif