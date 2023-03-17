#pragma once

#ifndef SPHERE_H
#define SPHERE_H



class Sphere : public Hittable {
public:
	Sphere() {}
	Sphere(point3 cen, double rad,colour albedo,int index) : centre(cen), radius(rad), m_Albedo(albedo), m_Index(index) {};
	virtual bool hit(const Ray& r, double t_min, double t_max, HitRecord& rec) const override;

public:
	point3 centre;
	double radius;
	colour m_Albedo;
	int m_Index = 0;


};


bool Sphere::hit(const Ray& r, double t_min, double t_max, HitRecord& rec) const {

	vec3 oc = r.origin() - centre;
	auto a = dot(r.direction(), r.direction());
	auto h = dot(oc, r.direction());
	auto c = dot(oc, oc) - radius * radius;

	auto discriminant = h * h - a * c;

	if (discriminant < 0) {
		return false;
	}

	auto sqrtd = glm::sqrt(discriminant);

	auto root = (-h - sqrtd) / a;   //set root to be the -ve root

	if (root > t_max || root < t_min) {
		root = (-h + sqrtd) / a;   //set root to be the +ve root
		if (root > t_max || root < t_min) //if we're still outside the bounds of t_min and t_max
			return false;
	}

	rec.m_T = root;
	rec.m_P = r.at(rec.m_T);
	rec.m_hitColour = m_Albedo;
	rec.m_Index = m_Index;

	vec3 outward_normal = (rec.m_P - centre) / radius;
	rec.SetFaceNormal(r, outward_normal);

	return true;
}



#endif
