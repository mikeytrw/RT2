#pragma once

#ifndef SPHERE_H
#define SPHERE_H



class Sphere : public Hittable {
public:
	Sphere() {}
	Sphere(vec3 cen, float rad,int index) : centre(cen), radius(rad), m_Index(index) {};
	virtual bool hit(const Ray& r, float t_min, float t_max, HitRecord& rec) const override;

public:
	vec3 centre = vec3(0.0f);
	float radius = 0.0f;
	int m_Index = 0;
	


};


bool Sphere::hit(const Ray& r, float t_min, float t_max, HitRecord& rec) const {

	vec3 oc = r.origin() - centre;
	auto a = dot(r.direction(), r.direction());
	auto h = dot(oc, r.direction());
	auto c = dot(oc, oc) - radius * radius;

	auto discriminant = h * h - a * c;

	if (discriminant < 0) {
		return false;
	}

	float sqrtd = glm::sqrt(discriminant);

	float root = (-h - sqrtd) / a;   //set root to be the -ve root

	if (root > t_max || root < t_min) {
		root = (-h + sqrtd) / a;   //set root to be the +ve root
		if (root > t_max || root < t_min) //if we're still outside the bounds of t_min and t_max
			return false;
	}

	rec.m_T = root;
	rec.m_P = r.at(rec.m_T);
	rec.hitMaterial = mat;
	rec.m_Index = m_Index;

	vec3 outward_normal = (rec.m_P - centre) / radius;
	rec.SetFaceNormal(r, outward_normal);

	return true;
}



#endif
