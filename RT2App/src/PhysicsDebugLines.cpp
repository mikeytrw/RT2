// CPU-only PhysicsDebugLines ordering + deterministic headless dump.
// No Bullet, no Vulkan, no ImGui, no Walnut — safe for RT2Tests/SliceRunner.
#include "PhysicsDebugLines.h"

#include <algorithm>
#include <cstdio>

namespace rt2::core {

void PhysicsDebugLines::SortStable()
{
    std::stable_sort(segments.begin(), segments.end(),
        [](const PhysicsDebugSegment& a, const PhysicsDebugSegment& b) {
            const std::string as = a.owner.ToString();
            const std::string bs = b.owner.ToString();
            if (as != bs) return as < bs;
            return static_cast<uint8_t>(a.kind) < static_cast<uint8_t>(b.kind);
        });
}

std::string PhysicsDebugLines::Dump() const
{
    std::string out;
    char buf[256];
    for (const auto& s : segments)
    {
        std::snprintf(buf, sizeof(buf), "%s %s %.6f %.6f %.6f %.6f %.6f %.6f\n",
            s.owner.ToString().c_str(), PhysicsDebugLineKindName(s.kind),
            (double)s.a.x, (double)s.a.y, (double)s.a.z,
            (double)s.b.x, (double)s.b.y, (double)s.b.z);
        out += buf;
    }
    return out;
}

size_t PhysicsDebugLines::CountByKind(PhysicsDebugLineKind kind) const
{
    size_t n = 0;
    for (const auto& s : segments)
        if (s.kind == kind) ++n;
    return n;
}

} // namespace rt2::core
