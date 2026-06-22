#pragma once

#include "glm/glm.hpp"
#include <utility>

class Ray;

struct AABB {
    glm::vec3 minimum;
    glm::vec3 maximum;

    AABB() : minimum(0.0f), maximum(0.0f) {}
    AABB(const glm::vec3& min, const glm::vec3& max) : minimum(min), maximum(max) {}

    bool hit(const Ray& r, float t_min, float t_max) const;

    static AABB surroundingBox(const AABB& box0, const AABB& box1) {
        glm::vec3 small = glm::min(box0.minimum, box1.minimum);
        glm::vec3 big = glm::max(box0.maximum, box1.maximum);
        return AABB(small, big);
    }
};