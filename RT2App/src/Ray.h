#pragma once
#ifndef RAY_H
#define RAY_H

#include "glm/glm.hpp"
#include "Utility.h"

class Ray {

public:
	Ray() {}
	Ray(const vec3& origin, const vec3& direction) : orig(origin), dir(direction) {}

	vec3 origin() const { return orig; }
	vec3 direction() const { return dir; }

	vec3 at(float t) const {
		return orig + t * dir;
	}

public:
	vec3 orig = vec3(0.0, 0.0, 0.0);
	vec3 dir = vec3(0.0, 0.0, 0.0);


};


#endif // !RAY_H
