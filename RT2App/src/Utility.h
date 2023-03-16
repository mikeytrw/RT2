#pragma once

#ifndef UTILITY_H
#define UTILITY_H

#include <cmath>
#include <limits>
#include <memory>
#include <cstdlib>
#include "glm/glm.hpp"

//Namespaces 

using std::shared_ptr;
using std::make_shared;
using std::sqrt;


using point3 = glm::dvec3;
using vec3 = glm::dvec3;
using colour = glm::dvec3;

//constants

const double infinity = std::numeric_limits<double>::infinity();
const double pi = 3.1415926535897932385;

//Utility functions

inline double randomDouble() {
    // Returns a random real in [0,1).
    return rand() / (RAND_MAX + 1.0);
}

inline double randomDouble(double min, double max) {
    // Returns a random real in [min,max).
    return min + (max - min) * randomDouble();
}

inline double clamp(double x, double min, double max) {
    if (x < min) return min;
    if (x > max) return max;
    return x;
}

inline vec3 unit_vector(vec3 v) {
    return v / static_cast<double>(v.length());
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



//common headers
#include "Ray.h"
#include "Colour.h"
#include "Hittable.h"
#include "HittableList.h"
#include "Sphere.h"
#include "Camera.h"



#endif // !UTILITY_H
