#include "EditorSyncRouter.h"

void EditorSyncRouter::Route(const EditorMutationResult& result, SceneManager& scene)
{
	if (result.syncImpact == rt2::core::SyncImpact::None)
		return;

	if (result.syncImpact == rt2::core::SyncImpact::Transform)
	{
		if (m_TransformSync) m_TransformSync();
		if (m_ResetAccum)    m_ResetAccum();
		return;
	}

	const bool rendererAvailable = m_RendererAvailable ? m_RendererAvailable() : true;
	if (!rendererAvailable)
	{
		// No renderer: still reset accumulation so a later-available renderer
		// starts fresh. Matches the existing WalnutApp behavior where the
		// accumulation reset is unconditional when impact != None/Transform.
		if (m_ResetAccum) m_ResetAccum();
		return;
	}

	if (result.syncImpact == rt2::core::SyncImpact::Structural)
	{
		// Structural downgrade check: if the SceneManager's resource
		// generation has not changed since the last full/material sync this
		// router dispatched, a structural mutation did not actually
		// invalidate GPU resources (e.g. a pure visibility change). Downgrade
		// to a material sync to avoid an AS+texture rebuild.
		const uint64_t current = scene.ResourceGeneration();
		if (m_LastSyncedResourceGeneration != 0 &&
			m_LastSyncedResourceGeneration == current)
		{
			const bool texPending = m_TexUploadPending ? m_TexUploadPending() : false;
			if (!texPending)
			{
				if (m_MaterialSync) m_MaterialSync();
				m_LastSyncedResourceGeneration = current;
			}
		}
		else
		{
			if (m_FullSync) m_FullSync();
			m_LastSyncedResourceGeneration = current;
		}
		if (m_ResetAccum) m_ResetAccum();
		return;
	}

	// Material
	{
		const bool texPending = m_TexUploadPending ? m_TexUploadPending() : false;
		if (!texPending)
		{
			if (m_MaterialSync) m_MaterialSync();
			m_LastSyncedResourceGeneration = scene.ResourceGeneration();
		}
		if (m_ResetAccum) m_ResetAccum();
	}
}