#pragma once

#ifndef RENDERER_H
#define RENDERER_H

#include "Utility.h"


class Renderer {
public:

	Renderer() {

		//do stuff here 

		//Image
		m_SamplesPerPixel = 1;
		m_MaxBounceDepth = 4;
		m_NumRaysCast = 0;

		//world
		
		m_World.add(make_shared<Sphere>(vec3(0.0, 0.0, -1.0), 0.5,0));
		m_World.objects[0]->mat.albedo = vec3(1.0, 0.0, 0.0);

		//ground
		m_World.add(make_shared<Sphere>(vec3(0.0, -100.5, -1.0), 100.0,1));
		m_World.objects[1]->mat.albedo = vec3(0.0, 0.0, 0.0);
		m_World.objects[1]->mat.roughness = 0.15f;
		//blue light
		m_World.add(make_shared<Sphere>(vec3(1.0,2.0, -1.0), 0.5,2));   //blue light
		m_World.objects[2]->mat.albedo = vec3(0.0, 0.0, 40.0);

		//m_World.add(make_shared<Sphere>(point3(1.0, 2.0, -1.0), 0.5, colour(40.0, 0.0, 0.0),3));



		//lightSphere

		//Camera

	}

	inline void OnResize(uint32_t width, uint32_t height) {

		if (m_FinalImage) {
			if (m_FinalImage->GetWidth() == width && m_FinalImage->GetHeight() == height) {
				return;
			}
			m_FinalImage->Resize(width, height); 
		}
		else {
			m_FinalImage = std::make_shared<Walnut::Image>(width, height, Walnut::ImageFormat::RGBA);
		}

		delete[] m_ImageData;
		m_ImageData = new uint32_t[width * height];


	}

	inline void Render(Camera cam) {

		m_NumRaysCast = 0;

	//Render every pixel

		int width = m_FinalImage->GetWidth();
		int height = m_FinalImage->GetHeight();

		//cam.SetWidthAndHeight(float(width), float(height));
		
		for (int x = 0; x < height; ++x) {
			for (int y = 0; y < width; ++y) {
				colour pixel_colour(0.0, 0.0, 0.0); // start with black then we add each sample and finally divide.
				for (int s = 0; s < m_SamplesPerPixel; ++s) {
					int depth = m_MaxBounceDepth;
					auto u = float(y) / (width - 1); //(float(i) + randomDouble()) / (width - 1);
					auto v = float(x) / (height - 1); //(float(j) + randomDouble()) / (height - 1);
					Ray r = Ray(cam.GetPosition(),cam.getRayDirection(u, v));
					pixel_colour += RayColor(r, m_World, depth);
				}
				//write_colour(charbuf, pixel_colour, buffer_position, samples_per_pixel);

				float scale = 1.0f / m_SamplesPerPixel;

				float r = sqrt(pixel_colour.x * scale);
				float g = sqrt(pixel_colour.y * scale);
				float b = sqrt(pixel_colour.z * scale);
				

				uint32_t ir = static_cast<uint32_t>(256 * clamp(r, 0.0f, 0.999f));
				uint32_t ig = static_cast<uint32_t>(256 * clamp(g, 0.0f, 0.999f));
				uint32_t ib = static_cast<uint32_t>(256 * clamp(b, 0.0f, 0.999f));

				uint32_t color = (static_cast<uint32_t>(255) << 24) | (ib << 16) | (ig << 8) | ir;

				m_ImageData[y + x * width] = color;
				
			}
		}


		m_FinalImage->SetData(m_ImageData);


	
	}

	std::shared_ptr<Walnut::Image> GetFinalImage() const {
		return m_FinalImage;
	}

	colour RayColor(const Ray& r, const Hittable& world, int depth) {

		
		auto dirLight = vec3(1.0, 1.0, 1.0);
		auto dirLightColour = vec3(0.5, 0.0, 1.0);

		m_NumRaysCast++;

		HitRecord rec;

		if (depth <= 0) {
			return colour(0.0, 0.0, 0.0);
		}

		if (world.hit(r, 0.001f, infinity, rec)) {

			//return rec.m_Normal;

			//point in unit sphere:
			vec3 unit_sphere_point = rec.m_P + rec.m_Normal + unit_vector(random_in_unit_sphere());
			//return 0.5 * RayColor(Ray(rec.m_P, unit_sphere_point - rec.m_P), world, --depth) + rec.hitMaterial.albedo;
			
			vec3 diffuseDirection = unit_sphere_point - rec.m_P;
			vec3 reflectionVector = glm::reflect(r.dir, rec.m_Normal);
			vec3 reflectionRayDir = (1.0f - rec.hitMaterial.roughness) * reflectionVector + rec.hitMaterial.roughness* diffuseDirection;
			return 0.5f * RayColor(Ray(rec.m_P, reflectionRayDir), world, --depth) + rec.hitMaterial.albedo;

		}

		return Miss(r, world, depth);
	}


	colour Miss(const Ray& r, const Hittable& world, int depth) {
		//Sky colour

		//return vec3(0.0, 0.0, 0.0);
		vec3 unit_direction = glm::normalize(r.direction());
		auto t = 0.5f * (unit_direction.y + 1.0f);
		return (1.0f - t) * colour(1.0, 1.0, 1.0) + t * colour(0.5, 0.7, 1.0);
	}

	uint32_t m_NumRaysCast;
	int m_SamplesPerPixel;
	int m_MaxBounceDepth;

private:
	std::shared_ptr<Walnut::Image> m_FinalImage;
	uint32_t* m_ImageData = nullptr;
	uint32_t* m_PrevImageData = nullptr;

	HittableList m_World;
	

};



#endif
