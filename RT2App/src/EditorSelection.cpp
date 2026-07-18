#include "EditorSelection.h"
#include "SceneDocument.h"

#include <algorithm>

bool EditorSelection::Contains(const UUID& uuid) const
{
	return !uuid.IsNull() &&
		std::find(m_Ordered.begin(), m_Ordered.end(), uuid) != m_Ordered.end();
}

void EditorSelection::Clear()
{
	m_Ordered.clear();
}

void EditorSelection::SelectOnly(const UUID& uuid)
{
	m_Ordered.clear();
	if (!uuid.IsNull())
		m_Ordered.push_back(uuid);
}

void EditorSelection::Add(const UUID& uuid)
{
	if (uuid.IsNull() || Contains(uuid))
		return;
	m_Ordered.push_back(uuid);
}

void EditorSelection::Toggle(const UUID& uuid)
{
	if (Contains(uuid))
		Remove(uuid);
	else
		Add(uuid);
}

void EditorSelection::Remove(const UUID& uuid)
{
	m_Ordered.erase(std::remove(m_Ordered.begin(), m_Ordered.end(), uuid),
	                m_Ordered.end());
}

bool EditorSelection::Prune(const rt2::core::SceneDocument& document)
{
	const auto oldSize = m_Ordered.size();
	m_Ordered.erase(
		std::remove_if(m_Ordered.begin(), m_Ordered.end(),
			[&document](const UUID& uuid) {
				return document.FindByUuid(uuid) == entt::null;
			}),
		m_Ordered.end());
	return oldSize != m_Ordered.size();
}
