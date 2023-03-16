#pragma once

#ifndef COLOUR_H
#define COLOUR_H


#include <iostream>
#include <string>


void write_colour(char* out, colour pixel_colour, int& buffer_position, int samples_per_pixel) {

	double scale = 1.0 / samples_per_pixel;

	double r = sqrt(pixel_colour.x * scale);
	double g = sqrt(pixel_colour.y * scale);
	double b = sqrt(pixel_colour.z * scale);

	uint32_t ir = static_cast<uint32_t>(256 * clamp(r, 0.0, 0.999));
	uint32_t ig = static_cast<uint32_t>(256 * clamp(g, 0.0, 0.999));
	uint32_t ib = static_cast<uint32_t>(256 * clamp(b, 0.0, 0.999));

	//out << ir << ' ' << ig << ' ' << ib << '\n';


	//out.append(std::to_string(ir) + " " + std::to_string(ig) + " " + std::to_string(ib) + "\n");

	char* tempbuf = new char[30];
		
	std::snprintf(tempbuf, 30, "%d %d %d\n", ir, ig, ib);

	std::snprintf(out + buffer_position, 30000000, "%d %d %d\n", ir, ig, ib);

	buffer_position += (int) strlen(tempbuf);

	delete[] tempbuf;

}


#endif // !COLOUR_H

