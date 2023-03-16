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
		m_MaxBounceDepth = 2;

		//world
		
		world.add(make_shared<Sphere>(point3(0.0, 0.0, -1.0), 0.5));
		world.add(make_shared<Sphere>(point3(0.0, -100.5, -1.0), 100.0));
		//world.add(make_shared<sphere>(point3(0.0, 0.0, -1.01), 0.7));

		//Camera
		m_Cam = Camera();

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

	inline void Render() {

	//Render every pixel

		int width = m_FinalImage->GetWidth();
		int height = m_FinalImage->GetHeight();

		m_Cam.SetWidthAndHeight(double(width), double(height));
		
		for (int j = 0; j <= height - 1; ++j) {    //for (int j = height - 1; j >= 0; --j) {  //upside down
			for (int i = 0; i < width; ++i) {
				colour pixel_colour(0.0, 0.0, 0.0); // start with black then we add each sample and finally divide.
				for (int s = 0; s < m_SamplesPerPixel; ++s) {
					int depth = m_MaxBounceDepth;
					auto u = (double(i) + randomDouble()) / (width - 1);
					auto v = (double(j) + randomDouble()) / (height - 1);
					Ray r = m_Cam.get_ray(u, v);
					pixel_colour += RayColor(r, world, depth);
				}
				//write_colour(charbuf, pixel_colour, buffer_position, samples_per_pixel);

				double scale = 1.0 / m_SamplesPerPixel;

				/*
				double r = sqrt(pixel_colour.x * scale);
				double g = sqrt(pixel_colour.y * scale);
				double b = sqrt(pixel_colour.z * scale);
				*/

				double r = pixel_colour.x * scale;
				double g = pixel_colour.y * scale;
				double b = pixel_colour.z * scale;


				uint32_t ir = static_cast<uint32_t>(256 * clamp(r, 0.0, 0.999));
				uint32_t ig = static_cast<uint32_t>(256 * clamp(g, 0.0, 0.999));
				uint32_t ib = static_cast<uint32_t>(256 * clamp(b, 0.0, 0.999));

				uint32_t color = (static_cast<uint32_t>(255) << 24) | (ib << 16) | (ig << 8) | ir;

				m_ImageData[j * width + i] = color;
			}
		}


		/*
		for (uint32_t i = 0; i < m_FinalImage->GetWidth() * m_FinalImage->GetHeight(); i++) {
			m_ImageData[i] = 0xffff00ff; // ABGR
		}
		*/
		m_FinalImage->SetData(m_ImageData);


	
	}

	std::shared_ptr<Walnut::Image> GetFinalImage() const {
		return m_FinalImage;
	}

	colour RayColor(const Ray& r, const Hittable& world, int depth) {

		HitRecord rec;

		if (depth <= 0) {
			return colour(0.0, 0.0, 0.0);
		}

		if (world.hit(r, 0.001, infinity, rec)) {

			//return rec.m_Normal;

			//if (rec.m_FrontFace) return colour(1.0, 0.0, 0.0);
			//return colour(0.1, 0.8, 0.1);
			//point in unit sphere:
			point3 unit_sphere_point = rec.m_P + rec.m_Normal + unit_vector(random_in_unit_sphere());
			return 0.5 * RayColor(Ray(rec.m_P, unit_sphere_point - rec.m_P), world, --depth);
			//return 0.5 * ray_color(ray((rec.p+(rec.normal*0.0000001)), unit_sphere_point - rec.p), world, --depth);

		}


		//sky colour
		vec3 unit_direction = unit_vector(r.direction());
		auto t = 0.5 * (unit_direction.y + 1.0);
		return (1.0 - t) * colour(1.0, 1.0, 1.0) + t * colour(0.5, 0.7, 1.0);
	}

private:
	std::shared_ptr<Walnut::Image> m_FinalImage;
	uint32_t* m_ImageData = nullptr;
	int m_SamplesPerPixel;
	int m_MaxBounceDepth;
	Camera m_Cam;
	HittableList world;

};



#endif
