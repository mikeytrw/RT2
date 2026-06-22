#pragma once

#ifndef BVH_NODE_H
#define BVH_NODE_H

#include "Hittable.h"
#include "AABB.h"

#include <algorithm>
#include <vector>

class BVHNode : public Hittable {
public:
	BVHNode() = default;

	BVHNode(std::vector<shared_ptr<Hittable>>& objects, size_t start, size_t end) {
		size_t count = end - start;
		if (count == 1) {
			m_Left = m_Right = objects[start];
		}
		else if (count == 2) {
			m_Left = objects[start];
			m_Right = objects[start + 1];
		}
		else {
			AABB centroidBox = AABB(objects[start]->boundingBox().minimum,
			                        objects[start]->boundingBox().maximum);
			for (size_t i = start + 1; i < end; i++) {
				AABB b = objects[i]->boundingBox();
				centroidBox.minimum = glm::min(centroidBox.minimum, b.minimum);
				centroidBox.maximum = glm::max(centroidBox.maximum, b.maximum);
			}

			int axis = longestAxis(centroidBox);

			auto comparator = [axis](const shared_ptr<Hittable>& a, const shared_ptr<Hittable>& b) {
				return centroidAxisValue(a, axis) < centroidAxisValue(b, axis);
			};

			std::sort(objects.begin() + start, objects.begin() + end, comparator);

			size_t mid = start + count / 2;
			m_Left = make_shared<BVHNode>(objects, start, mid);
			m_Right = make_shared<BVHNode>(objects, mid, end);
		}

		AABB boxLeft = m_Left->boundingBox();
		AABB boxRight = m_Right->boundingBox();
		m_Box = AABB::surroundingBox(boxLeft, boxRight);
	}

	virtual bool hit(const Ray& r, float t_min, float t_max, HitRecord& rec) const override {
		if (!m_Box.hit(r, t_min, t_max))
			return false;

		bool hitLeft = m_Left->hit(r, t_min, t_max, rec);
		bool hitRight = m_Right->hit(r, t_min, hitLeft ? rec.m_T : t_max, rec);

		return hitLeft || hitRight;
	}

	virtual AABB boundingBox() const override { return m_Box; }

	void GetStats(uint32_t& nodeCount, int& maxDepth, int depth = 1) const {
		nodeCount++;

		bool leftIsBvh = dynamic_cast<BVHNode*>(m_Left.get()) != nullptr;
		bool rightIsBvh = dynamic_cast<BVHNode*>(m_Right.get()) != nullptr;

		int leftDepth = depth, rightDepth = depth;
		if (leftIsBvh) static_cast<BVHNode*>(m_Left.get())->GetStats(nodeCount, leftDepth, depth + 1);
		if (rightIsBvh) static_cast<BVHNode*>(m_Right.get())->GetStats(nodeCount, rightDepth, depth + 1);

		maxDepth = (leftDepth > rightDepth) ? leftDepth : rightDepth;
	}

private:
	shared_ptr<Hittable> m_Left;
	shared_ptr<Hittable> m_Right;
	AABB m_Box;

	static int longestAxis(const AABB& box) {
		vec3 extent = box.maximum - box.minimum;
		if (extent.x > extent.y && extent.x > extent.z) return 0;
		if (extent.y > extent.z) return 1;
		return 2;
	}

	static float centroidAxisValue(const shared_ptr<Hittable>& obj, int axis) {
		AABB b = obj->boundingBox();
		return (b.minimum[axis] + b.maximum[axis]) * 0.5f;
	}
};

#endif // !BVH_NODE_H