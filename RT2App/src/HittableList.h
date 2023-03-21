#pragma once
#ifndef HITTABLE_LIST_H
#define HITTABLE_LIST_H

#include "hittable.h"

#include <memory>
#include <vector>

using std::shared_ptr;
using std::make_shared;


class HittableList : public Hittable
{
public:
	HittableList() {};
	HittableList(shared_ptr<Hittable> object) { add(object); };
	~HittableList() { clear(); };
	
	void clear() { objects.clear(); };
	void add(shared_ptr<Hittable> object) { objects.push_back(object); };

	virtual bool hit(const Ray& r, float t_min, float t_max, HitRecord& rec) const override;
	std::vector<shared_ptr<Hittable>> objects;
private:
	
};

bool HittableList::hit(const Ray& r, float t_min, float t_max, HitRecord& rec) const {

	HitRecord temp_hit_record;
	bool hit_anything = false;
	auto closest_so_far = t_max;

	for (const auto& object : objects) {
		if (object->hit(r, t_min, closest_so_far, temp_hit_record)) {
			hit_anything = true;
			closest_so_far = temp_hit_record.m_T;
			rec = temp_hit_record;
		}
	}

	return hit_anything;
}


#endif // !HITTABLE_LIST_H