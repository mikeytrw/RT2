#pragma once
#ifndef RAY_H
#define RAY_H

#include "glm/glm.hpp"

using glm::vec3;

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

#include "AABB.h"

inline bool AABB::hit(const Ray& r, float t_min, float t_max) const {
    for (int a = 0; a < 3; a++) {
        auto invD = 1.0f / r.direction()[a];
        auto t0 = (minimum[a] - r.origin()[a]) * invD;
        auto t1 = (maximum[a] - r.origin()[a]) * invD;
        if (invD < 0.0f) std::swap(t0, t1);

        t_min = t0 > t_min ? t0 : t_min;
        t_max = t1 < t_max ? t1 : t_max;

        if (t_max <= t_min) return false;
    }
    return true;
}
