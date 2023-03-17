#pragma once

#ifndef HITTABLE_H
#define HITTABLE_H

struct HitRecord {
	point3 m_P = point3(0.0, 0.0, 0.0);
	vec3 m_Normal = vec3(0.0, 0.0, 0.0);
	double m_T=0.0;
	bool m_FrontFace = false;
	colour m_hitColour = colour(0.0, 0.0, 0.0); 
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

	virtual bool hit(const Ray& r, double t_min, double t_max, HitRecord& rec) const = 0;
	
};


#endif