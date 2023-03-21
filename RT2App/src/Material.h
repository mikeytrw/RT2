#pragma once
#ifndef MATERIAL_H
#define MATERIAL_H

struct Material
{
	float roughness = 1.0f;
	vec3 albedo = vec3(0.0,0.0,0.0);
	float metallic = 0.0f;
};


#endif // !MATERIAL_H

