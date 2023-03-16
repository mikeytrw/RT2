#pragma once
#ifndef RAY_H
#define RAY_H

#include "glm/glm.hpp"

class Ray {

public:
	Ray() {}
	Ray(const point3& origin, const vec3& direction) : orig(origin), dir(direction) {}

	point3 origin() const { return orig; }
	vec3 direction() const { return dir; }

	point3 at(double t) const {
		return orig + t * dir;
	}

public:
	point3 orig = point3(0.0, 0.0, 0.0);
	vec3 dir = vec3(0.0, 0.0, 0.0);


};


#endif // !RAY_H
