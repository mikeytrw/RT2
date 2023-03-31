#pragma once

#ifndef UTILITY_H
#define UTILITY_H

#include <cmath>
#include <limits>
#include <memory>
#include <cstdlib>
#include "glm/glm.hpp"
#include "Walnut/Random.h"


//Namespaces 

using std::shared_ptr;
using std::make_shared;
using std::sqrt;

using glm::vec3;


//using vec3 = glm::dvec3;
using colour = vec3;

//constants

const float infinity = std::numeric_limits<float>::infinity();
const float pi = 3.1415926535897932385f;

//Utility functions

inline float randomDouble() {
    // Returns a random real in [0,1).
    
    //return rand() / (RAND_MAX + 1.0f);

    return Walnut::Random::Float();
}

inline float randomDouble(float min, float max) {
    // Returns a random real in [min,max).
   return min + (max - min) * randomDouble();

    
}

inline float clamp(float x, float min, float max) {
    if (x < min) return min;
    if (x > max) return max;
    return x;
}

inline vec3 unit_vector(vec3 v) {
    return glm::normalize(v);
}

double length_squared(vec3 e){

    return e.x * e.x + e.y * e.y + e.z * e.z;

}

inline vec3 random_in_unit_sphere() {
    while (true) {
        vec3 p = vec3(randomDouble(-1.0, 1.0), randomDouble(-1.0, 1.0), randomDouble(-1.0, 1.0));
        if (length_squared(p) >= 1) continue;
        return p;
    }

}

inline vec3 randomInUnitdisk() {

    while (true) {
        vec3 p = vec3(randomDouble(-1.0, 1.0), randomDouble(-1.0, 1.0), 0.0f);
        if (length_squared(p) >= 1) continue;
        return p;
    }
}


bool nearZero(const vec3& vector) {
    const auto s = 1e-8;
    return (vector.x < s) && (vector.y < s) && (vector.z < s);
}
/*
vec3 VCosineSampleHemisphere(float alpha)
{
    float cosTheta = pow(RandomUNorm(), 1 / (alpha + 1));
    float sinTheta = sqrt(1 - cosTheta * cosTheta);
    float phi = 2 * pi * RandomUNorm();
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
   
}
*/
float VCosineSampleHemispherePDF(vec3 v, float alpha)
{
    float cosTheta = v.z;
    return (cosTheta + alpha) * pow(cosTheta, alpha) / pi;
}

//common headers
#include "Ray.h"
#include "Material.h"
#include "Colour.h"
#include "Hittable.h"
#include "HittableList.h"
#include "Sphere.h"
#include "Camera.h"




#endif // !UTILITY_H
