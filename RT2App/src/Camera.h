#pragma once
#ifndef CAMERA_H
#define CAMERA_H


class Camera
{
public:
	Camera() {

		aspect_ratio = 16.0 / 9.0;
		viewport_height = 2.0;
		viewport_width = viewport_height * aspect_ratio;
		focal_length = 1.0;
		

		origin = point3(0.0, 0.0, 0.0);
		horizontal = vec3(viewport_width, 0.0, 0.0);
		vertical = vec3(0.0, viewport_height, 0.0);
		
	};

	~Camera();

	void SetWidthAndHeight(double width, double height) {
		aspect_ratio = width / height;
		viewport_width = viewport_height * aspect_ratio;
		horizontal = vec3(viewport_width, 0.0, 0.0);
		vertical = vec3(0.0, viewport_height, 0.0);
	}

	Ray get_ray(double u, double v) {
		//lower_left_corner = origin - horizontal / 2.0 - vertical / 2.0 - vec3(0.0, 0.0, focal_length);
		//return Ray(origin, lower_left_corner + u * horizontal + v * vertical - origin);
		top_left_corner = origin - horizontal / 2.0 + vertical / 2.0 - vec3(0.0, 0.0, focal_length);
		return Ray(origin, top_left_corner + u * horizontal - v * vertical - origin);

	}

	double viewport_height;
	double viewport_width;
	double focal_length;
	double aspect_ratio;

	 
private:

	point3 origin;
	vec3 horizontal;
	vec3 vertical;
	point3 top_left_corner;

};

Camera::~Camera()
{
}

#endif // !CAMERA_H
