#pragma once
#ifndef MATERIAL_H
#define MATERIAL_H

#include "Hittable.h"
#include "glm/glm.hpp"

class Material
{
public: 
	virtual bool scatter(const Ray& rayIn, const HitRecord& rec, colour& attenuation, Ray& rayOut) const = 0;
	virtual ~Material() = default;

public:
	Material(){};

};



class LambertianMaterial : public Material {
public:

	LambertianMaterial(const vec3& albedo) : mAlbedo(albedo) {};
	virtual bool scatter(const Ray& rayIn, const HitRecord& rec, colour& attenuation, Ray& rayOut) const override {

		auto scatter_direction = rec.m_Normal + unit_vector(random_in_unit_sphere());
		rayOut = Ray(rec.m_P, scatter_direction);
		attenuation = mAlbedo;
		return true;

		
/*
		//my ported over code cause i thought this looked weird
		vec3 unit_sphere_point = rec.m_P + rec.m_Normal + unit_vector(random_in_unit_sphere());
		vec3 diffuseDirection = unit_sphere_point - rec.m_P;

		if (nearZero(diffuseDirection)) {
			diffuseDirection = rec.m_Normal;
		}

		rayOut = Ray(rec.m_P, diffuseDirection);
		attenuation = mAlbedo;
		return true;

*/
	}
public:
	vec3 mAlbedo;
	
};


class MetalMaterial : public Material {

public:
	MetalMaterial(const vec3& albedo, float fuzz) : mAlbedo(albedo), mFuzz(fuzz < 1 ? fuzz : 1) {};

	virtual bool scatter(const Ray& rayIn, const HitRecord& rec, colour& attenuation, Ray& rayOut) const override {

		vec3 reflectionVector = glm::reflect(glm::normalize(rayIn.dir), rec.m_Normal);

		rayOut = Ray(rec.m_P, reflectionVector + mFuzz*random_in_unit_sphere());
		attenuation = mAlbedo;

		return (glm::dot(reflectionVector, rec.m_Normal) > 0);		//not sure what this is for

	}
		

public:
	vec3 mAlbedo;
	float mFuzz;
};


class DieletricMaterial : public Material {

public:
	DieletricMaterial(float refractionIndex) : mRefractionIndex(refractionIndex) {};

	virtual bool scatter(const Ray& rayIn, const HitRecord& rec, colour& attenuation, Ray& rayOut) const override {

		attenuation = vec3(1.0f);
		float refractionRatio = rec.m_FrontFace ? (1.0f / mRefractionIndex) : mRefractionIndex;

		vec3 unitDirection = glm::normalize(rayIn.dir);
		float cosTheta = glm::min(glm::dot(-unitDirection, rec.m_Normal), 1.0f);
		float sinTheta = glm::sqrt(1.0f - cosTheta * cosTheta);

		bool cannotRefract = refractionRatio * sinTheta > 1.0f;
		vec3 direction;

		if (cannotRefract || reflectance(cosTheta, refractionRatio) > randomDouble()) {
			direction = glm::reflect(unitDirection, rec.m_Normal);
		}
		else {
			direction = glm::refract(unitDirection, rec.m_Normal, refractionRatio);
		}

		rayOut = Ray(rec.m_P, direction);
		return true;
	}

private:
	float mRefractionIndex;

	static float reflectance(float cosine, float refIdx) {
		// Schlick's approximation
		float r0 = (1 - refIdx) / (1 + refIdx);
		r0 = r0 * r0;
		return r0 + (1 - r0) * glm::pow((1 - cosine), 5);
	}
};


#endif // !MATERIAL_H

