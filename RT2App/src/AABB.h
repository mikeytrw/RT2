#pragma once

#include "glm/glm.hpp"
#include "Ray.h"
#include <utility>

struct AABB {
    glm::vec3 minimum;
    glm::vec3 maximum;

    AABB() : minimum(0.0f), maximum(0.0f) {}
    AABB(const glm::vec3& min, const glm::vec3& max) : minimum(min), maximum(max) {}

    bool hit(const Ray& r, float t_min, float t_max) const {
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

    static AABB surroundingBox(const AABB& box0, const AABB& box1) {
        glm::vec3 small = glm::min(box0.minimum, box1.minimum);
        glm::vec3 big = glm::max(box0.maximum, box1.maximum);
        return AABB(small, big);
    }
};
