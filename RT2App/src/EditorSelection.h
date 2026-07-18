#pragma once

#include "core/UUID.h"

#include <cstddef>
#include <vector>

namespace rt2::core { class SceneDocument; }

// Editor selection is authoring state, identified by stable UUID rather than
// transient entt entity handles. The vector preserves selection order and the
// final entry is the primary selection used by the Inspector and gizmo.
class EditorSelection
{
public:
	using UUID = rt2::core::UUID;

	bool Empty() const { return m_Ordered.empty(); }
	std::size_t Size() const { return m_Ordered.size(); }
	const std::vector<UUID>& Ordered() const { return m_Ordered; }
	UUID Primary() const { return m_Ordered.empty() ? UUID::Nil() : m_Ordered.back(); }

	bool Contains(const UUID& uuid) const;
	void Clear();
	void SelectOnly(const UUID& uuid);
	void Add(const UUID& uuid);
	void Toggle(const UUID& uuid);
	void Remove(const UUID& uuid);

	// Remove UUIDs that no longer resolve in the supplied authoring document.
	// Returns true when the selection changed.
	bool Prune(const rt2::core::SceneDocument& document);

private:
	std::vector<UUID> m_Ordered;
};
