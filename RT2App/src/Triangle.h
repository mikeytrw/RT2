#pragma once

#include "Hittable.h"
#include "AABB.h"

class Triangle : public Hittable {
public:
    Triangle(const vec3& v0, const vec3& v1, const vec3& v2, shared_ptr<Material> mat)
        : m_V0(v0), m_V1(v1), m_V2(v2), Hittable(mat) {}

    virtual bool hit(const Ray& r, float t_min, float t_max, HitRecord& rec) const override {
        vec3 edge1 = m_V1 - m_V0;
        vec3 edge2 = m_V2 - m_V0;
        vec3 h = glm::cross(r.direction(), edge2);
        float a = glm::dot(edge1, h);

        if (a > -1e-6f && a < 1e-6f) return false; // Ray is parallel to triangle

        float f = 1.0f / a;
        vec3 s = r.origin() - m_V0;
        float u = f * glm::dot(s, h);

        if (u < 0.0f || u > 1.0f) return false;

        vec3 q = glm::cross(s, edge1);
        float v = f * glm::dot(r.direction(), q);

        if (v < 0.0f || u + v > 1.0f) return false;

        float t = f * glm::dot(edge2, q);

        if (t > t_min && t < t_max) {
            rec.m_T = t;
            rec.m_P = r.at(t);
            vec3 normal = glm::normalize(glm::cross(edge1, edge2));
            rec.SetFaceNormal(r, normal);
            rec.matPtr = mMatPtr;
            return true;
        }

        return false;
    }

    AABB boundingBox() const override {
        vec3 min = glm::min(m_V0, glm::min(m_V1, m_V2));
        vec3 max = glm::max(m_V0, glm::max(m_V1, m_V2));
        return AABB(min, max);
    }

private:
    vec3 m_V0, m_V1, m_V2;
};
