// Drawing half of the editor icon overlay. Separated from the geometry so the
// arithmetic in EditorViewportIcons.cpp stays testable in a project that does
// not link ImGui.
#include "EditorViewportIcons.h"

#include <imgui.h>

#include <cmath>

namespace {

// Glyph radius. Smaller than the hit radius so a near-miss still selects.
constexpr float kGlyphRadius = 8.0f;

ImU32 IconColour(EditorIconKind kind, bool selected)
{
	if (selected) return IM_COL32(255, 190, 60, 255);
	switch (kind)
	{
	case EditorIconKind::PointLight:
	case EditorIconKind::SpotLight:
	case EditorIconKind::DirectionalLight:
		return IM_COL32(250, 235, 140, 220);
	case EditorIconKind::Camera:
		return IM_COL32(150, 205, 255, 220);
	}
	return IM_COL32(255, 255, 255, 220);
}

ImVec2 ToImVec(const glm::vec2& v) { return ImVec2(v.x, v.y); }

// A light: a filled core with rays. Spot and directional add an aim
// indicator, which is the only on-screen cue that either one points somewhere
// other than where the user assumed.
void DrawLightGlyph(ImDrawList* draw, const EditorIconPlacement& icon, ImU32 colour)
{
	const glm::vec2 c = icon.screenPos;
	const float core = kGlyphRadius * 0.45f;
	draw->AddCircleFilled(ToImVec(c), core, colour, 12);

	for (int i = 0; i < 8; ++i)
	{
		const float a = (float)i * 3.14159265f * 0.25f;
		const glm::vec2 dir(std::cos(a), std::sin(a));
		draw->AddLine(ToImVec(c + dir * (core * 1.6f)),
			ToImVec(c + dir * kGlyphRadius), colour, 1.5f);
	}

	const float aimLen = glm::length(icon.screenAim);
	if (icon.kind == EditorIconKind::PointLight || aimLen < 1e-4f) return;

	const glm::vec2 dir = icon.screenAim / aimLen;
	const glm::vec2 perp(-dir.y, dir.x);
	const glm::vec2 tip = c + dir * (kGlyphRadius * 2.4f);
	if (icon.kind == EditorIconKind::SpotLight)
	{
		// Cone edges opening toward the aim direction.
		const glm::vec2 base = c + dir * kGlyphRadius;
		draw->AddLine(ToImVec(base), ToImVec(tip + perp * 6.0f), colour, 1.5f);
		draw->AddLine(ToImVec(base), ToImVec(tip - perp * 6.0f), colour, 1.5f);
	}
	else
	{
		// Directional: parallel rays, matching the parallel-ray model.
		for (int i = -1; i <= 1; ++i)
		{
			const glm::vec2 offset = perp * ((float)i * 4.5f);
			draw->AddLine(ToImVec(c + offset + dir * kGlyphRadius),
				ToImVec(c + offset + dir * (kGlyphRadius * 2.2f)), colour, 1.5f);
		}
	}
}

// A camera: a body plus the lens cone, aimed along its screen-space forward.
void DrawCameraGlyph(ImDrawList* draw, const EditorIconPlacement& icon, ImU32 colour)
{
	const glm::vec2 c = icon.screenPos;
	const float aimLen = glm::length(icon.screenAim);
	const glm::vec2 dir = aimLen > 1e-4f ? icon.screenAim / aimLen : glm::vec2(1.0f, 0.0f);
	const glm::vec2 perp(-dir.y, dir.x);

	const float half = kGlyphRadius * 0.62f;
	const glm::vec2 back = c - dir * half;
	const glm::vec2 front = c + dir * half;
	const ImVec2 body[4] = {
		ToImVec(back + perp * half),
		ToImVec(front + perp * half),
		ToImVec(front - perp * half),
		ToImVec(back - perp * half),
	};
	draw->AddPolyline(body, 4, colour, ImDrawFlags_Closed, 1.5f);

	const glm::vec2 tip = c + dir * (kGlyphRadius * 1.9f);
	draw->AddLine(ToImVec(front), ToImVec(tip + perp * 5.5f), colour, 1.5f);
	draw->AddLine(ToImVec(front), ToImVec(tip - perp * 5.5f), colour, 1.5f);
	draw->AddLine(ToImVec(tip + perp * 5.5f), ToImVec(tip - perp * 5.5f), colour, 1.5f);
}

} // namespace

EditorIconOverlayResult DrawEditorViewportIcons(
	const std::vector<EditorIconPlacement>& icons,
	const glm::vec2& mouse, bool imageHovered, bool clickPressed)
{
	EditorIconOverlayResult result;
	if (icons.empty()) return result;

	const EditorIconHit hovered = imageHovered
		? HitTestEditorIcons(icons, mouse) : EditorIconHit{};

	ImDrawList* draw = ImGui::GetWindowDrawList();
	for (const auto& icon : icons)
	{
		const bool isHovered = hovered.hit && hovered.entity == icon.entity;
		const ImU32 colour = isHovered
			? IM_COL32(255, 255, 255, 255) : IconColour(icon.kind, icon.selected);

		if (icon.selected || isHovered)
			draw->AddCircle(ToImVec(icon.screenPos), kEditorIconRadius, colour, 16, 1.0f);

		if (icon.kind == EditorIconKind::Camera)
			DrawCameraGlyph(draw, icon, colour);
		else
			DrawLightGlyph(draw, icon, colour);
	}

	if (hovered.hit)
	{
		result.consumesMouse = true;
		if (clickPressed)
		{
			result.clicked = true;
			result.clickedEntity = hovered.entity;
		}
	}
	return result;
}
