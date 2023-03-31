#pragma once

#ifndef RENDERER_H
#define RENDERER_H

#include "Utility.h"
#include <execution>

class Renderer {
public:

	Renderer() {

		//do stuff here 

		//Image
		m_SamplesPerPixel = 1;
		m_MaxBounceDepth = 40;
		m_NumRaysCast = 0;

		//world
		
		m_World.add(make_shared<Sphere>(vec3(0.0, 0.0, -1.0), 0.5,make_shared<LambertianMaterial>(vec3(0.0f,0.0f,1.0f))));
		m_World.add(make_shared<Sphere>(vec3(1.0, 0.0, -1.0), 0.5, make_shared<MetalMaterial>(vec3(0.8f, 0.6f, 0.2f),2.1f)));
		m_World.add(make_shared<Sphere>(vec3(-1.0, 0.0, -1.0), 0.5, make_shared<MetalMaterial>(vec3(0.8f, 0.8f, 0.8f),0.01f)));

		//ground
		m_World.add(make_shared<Sphere>(vec3(0.0, -500.5, -1.0), 500.0, make_shared<LambertianMaterial>(vec3(0.1f,0.3f,0.1f))));

		//blue light
		m_World.add(make_shared<Sphere>(vec3(-0.5,-0.25, -0.25), 0.25, make_shared<DieletricMaterial>(1.5f)));   //blue light






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
		
		mImageVerticalIter.resize(height);
		for (uint32_t i = 0; i < height; i++) {
			mImageVerticalIter[i] = i;
		}

		if (mTemporalBuffer) {
			delete[] mTemporalBuffer;
		}
			
		mTemporalBuffer = new glm::vec3[width * height];
			
		mFrameIndex = 1;
		


	}

	inline void Render(Camera cam) {
		mCam = cam;
		m_NumRaysCast = 0;

		if (cam.checkHasMoved()) {
			mFrameIndex = 1;
		}

	//Render every pixel
		if (mFrameIndex == 1) {

			memset(mTemporalBuffer, 0, m_FinalImage->GetWidth() * m_FinalImage->GetHeight() * sizeof(glm::vec3));
		}


		//cam.SetWidthAndHeight(float(width), float(height));

		std::for_each(std::execution::par, mImageVerticalIter.begin(), mImageVerticalIter.end(),
			[this](uint32_t y)
			{
					int width = m_FinalImage->GetWidth();
					int height = m_FinalImage->GetHeight();
				for (int x = 0; x < width; ++x) {
					colour pixel_colour(0.0, 0.0, 0.0); // start with black then we add each sample and finally divide.
					for (int s = 0; s < m_SamplesPerPixel; ++s) {
						int depth = m_MaxBounceDepth;
						auto u = (float(x) + randomDouble()) / (width - 1);
						auto v = (float(y) + randomDouble()) / (height - 1);

						std::pair<glm::vec3, glm::vec3> ray_origin_and_direction = mCam.GetRayOriginAndDirection(u, v);
						Ray r = Ray(ray_origin_and_direction.first, ray_origin_and_direction.second);
						pixel_colour += PerPixel(r, m_World, depth);
					}

					float scale = 1.0f / m_SamplesPerPixel;

					float r = sqrt(pixel_colour.x * scale);
					float g = sqrt(pixel_colour.y * scale);
					float b = sqrt(pixel_colour.z * scale);

					if (mTemporalAccumulation) {
						mTemporalBuffer[x + y * width] += vec3(r,g,b);

						glm::vec3 accumulatedColour = mTemporalBuffer[x + y * width];
						accumulatedColour /= (float)mFrameIndex;
						accumulatedColour = glm::clamp(accumulatedColour, vec3(0.0f), vec3(1.0f));
						r = accumulatedColour.r;
						g = accumulatedColour.g;
						b = accumulatedColour.b;

					}


					uint32_t ir = static_cast<uint32_t>(256 * clamp(r, 0.0f, 0.999f));
					uint32_t ig = static_cast<uint32_t>(256 * clamp(g, 0.0f, 0.999f));
					uint32_t ib = static_cast<uint32_t>(256 * clamp(b, 0.0f, 0.999f));

					uint32_t color = (static_cast<uint32_t>(255) << 24) | (ib << 16) | (ig << 8) | ir;

					m_ImageData[x + y * width] = color;

				}
			});

		m_FinalImage->SetData(m_ImageData);

		if (mTemporalAccumulation) {
			mFrameIndex++;
		}
		else
			mFrameIndex = 1;
	
	}

	std::shared_ptr<Walnut::Image> GetFinalImage() const {
		return m_FinalImage;
	}

	colour PerPixel(const Ray& r, const Hittable& world, int depth) {

		//m_NumRaysCast++;  //commented out for performance reasons

		HitRecord rec;

		if (depth <= 0) {
			return colour(0.0, 0.0, 0.0);
		}

		if (world.hit(r, 0.001f, infinity, rec)) {
			/*    //The old code that looked sick and ran fast af
			vec3 unit_sphere_point = rec.m_P + rec.m_Normal + unit_vector(random_in_unit_sphere());
			vec3 diffuseDirection = unit_sphere_point - rec.m_P;
			vec3 reflectionVector = glm::reflect(r.dir, rec.m_Normal);
			vec3 reflectionRayDir = (1.0f - rec.hitMaterial.roughness) * reflectionVector + rec.hitMaterial.roughness* diffuseDirection;
			return 0.5f * RayColor(Ray(rec.m_P, reflectionRayDir), world, --depth) + rec.hitMaterial.albedo;
			*/


			 //hacked at old code to work with new Material system
/*	
			vec3 unit_sphere_point = rec.m_P + rec.m_Normal + unit_vector(random_in_unit_sphere());
			vec3 diffuseDirection = unit_sphere_point - rec.m_P;
			vec3 reflectionVector = glm::reflect(r.dir, rec.m_Normal);
			//vec3 reflectionRayDir = (1.0f - rec.hitMaterial.roughness) * reflectionVector + rec.hitMaterial.roughness* diffuseDirection;
			vec3 reflectionRayDir = diffuseDirection;
			//return 0.5f * RayColor(Ray(rec.m_P, reflectionRayDir), world, --depth) + rec.matPtr->mAlbedo;
			return 0.5f * RayColor(Ray(rec.m_P, reflectionRayDir), world, --depth);
*/			

			//new code
			Ray scatterRay;
			vec3 attenuation;

			if (rec.matPtr->scatter(r, rec, attenuation, scatterRay)) {
				//return 0.5f * PerPixel(scatterRay, world, depth - 1) + attenuation;
				return attenuation * PerPixel(scatterRay, world, depth - 1);
			}
			return vec3(0.0f);
			


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

	void setTemporalAccumulation(bool enabled) {
		if (!enabled) mFrameIndex = 1;
		mTemporalAccumulation = enabled;
	}

	uint32_t m_NumRaysCast;
	int m_SamplesPerPixel;
	int m_MaxBounceDepth;

private:
	std::shared_ptr<Walnut::Image> m_FinalImage;
	uint32_t* m_ImageData = nullptr;
	uint32_t* m_PrevImageData = nullptr;

	uint32_t mFrameIndex = 1;

	HittableList m_World;
	std::vector<uint32_t> mImageVerticalIter;

	glm::vec3* mTemporalBuffer = nullptr;

	bool mTemporalAccumulation = false;

	Camera mCam;

};



#endif
