#pragma once

#include <cstdint>
#include <optional>

class OutputExtent;

// Render and output dimensions are intentionally non-interchangeable. Render
// sizes internal raster/compute resources; output sizes display/readback.
class RenderExtent
{
public:
    constexpr RenderExtent() = default;
    static std::optional<RenderExtent> TryCreate(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0) return std::nullopt;
        return RenderExtent(width, height);
    }
    constexpr bool IsValid() const { return m_Width != 0 && m_Height != 0; }
    constexpr uint32_t Width() const { return m_Width; }
    constexpr uint32_t Height() const { return m_Height; }
    constexpr bool operator==(const RenderExtent& other) const
        { return m_Width == other.m_Width && m_Height == other.m_Height; }
    constexpr bool operator!=(const RenderExtent& other) const { return !(*this == other); }
    static RenderExtent FromOutputNative(const OutputExtent& output);
private:
    constexpr RenderExtent(uint32_t width, uint32_t height) : m_Width(width), m_Height(height) {}
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
};

class OutputExtent
{
public:
    constexpr OutputExtent() = default;
    static std::optional<OutputExtent> TryCreate(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0) return std::nullopt;
        return OutputExtent(width, height);
    }
    constexpr bool IsValid() const { return m_Width != 0 && m_Height != 0; }
    constexpr uint32_t Width() const { return m_Width; }
    constexpr uint32_t Height() const { return m_Height; }
    constexpr bool operator==(const OutputExtent& other) const
        { return m_Width == other.m_Width && m_Height == other.m_Height; }
    constexpr bool operator!=(const OutputExtent& other) const { return !(*this == other); }
    RenderExtent ToRenderNative() const { return RenderExtent::FromOutputNative(*this); }
private:
    friend class RenderExtent;
    constexpr OutputExtent(uint32_t width, uint32_t height) : m_Width(width), m_Height(height) {}
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
};

inline RenderExtent RenderExtent::FromOutputNative(const OutputExtent& output)
{
    return RenderExtent(output.m_Width, output.m_Height);
}

struct NativeExtentPlan { OutputExtent output; RenderExtent render; };

inline std::optional<NativeExtentPlan> PlanNativeExtents(uint32_t width, uint32_t height)
{
    const auto output = OutputExtent::TryCreate(width, height);
    if (!output) return std::nullopt;
    return NativeExtentPlan{ *output, output->ToRenderNative() };
}
