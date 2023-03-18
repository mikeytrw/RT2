#pragma once
#ifndef MATERIAL_H
#define MATERIAL_H

struct Material
{
	double roughness = 1.0;
	vec3 albedo = vec3(0.0,0.0,0.0);
	double metallic = 0.0;
};


#endif // !MATERIAL_H

