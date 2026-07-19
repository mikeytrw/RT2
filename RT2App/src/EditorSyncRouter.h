#pragma once

#ifndef RT2_EDITOR_SYNC_ROUTER_H
#define RT2_EDITOR_SYNC_ROUTER_H

#include "SceneMutation.h"
#include "SceneManager.h"

#include <functional>

// ============================================================================
// EditorSyncRouter — CPU-only extraction of the sync-impact routing that
// lives in WalnutApp's m_OnMutation lambda. It takes an EditorMutationResult
// plus injected callables and reproduces the host mapping:
//
//   None       -> nothing
//   Transform  -> transformSync + resetAccum
//   Material   -> materialSync (only when renderer available AND no texture
//                 upload is pending) + resetAccum
//   Structural -> fullSync + resetAccum, UNLESS the SceneManager's
//                 ResourceGeneration has not changed since the last sync this
//                 router dispatched — in that case downgrade to materialSync
//                 (a structural mutation that did not actually invalidate
//                 resources, e.g. a pure visibility toggle, does not require a
//                 full AS+texture rebuild).
//
// The router never calls the render bridge directly; it calls the injected
// callables. This makes the mapping testable from CPU-only tests.
//
// The router has no dependency on Walnut/ImGui/Vulkan and links cleanly into
// RT2Tests.
//
// ============================================================================

class EditorSyncRouter
{
public:
	using TransformSyncFn       = std::function<void()>;
	using MaterialSyncFn        = std::function<void()>;
	using FullSyncFn            = std::function<void()>;
	using ResetAccumFn          = std::function<void()>;
	using RendererAvailableFn   = std::function<bool()>;
	using TextureUploadPendingFn = std::function<bool()>;

	EditorSyncRouter() = default;

	void SetTransformSync(TransformSyncFn cb)        { m_TransformSync = std::move(cb); }
	void SetMaterialSync(MaterialSyncFn cb)          { m_MaterialSync  = std::move(cb); }
	void SetFullSync(FullSyncFn cb)                  { m_FullSync      = std::move(cb); }
	void SetResetAccum(ResetAccumFn cb)              { m_ResetAccum    = std::move(cb); }
	void SetRendererAvailable(RendererAvailableFn cb){ m_RendererAvailable = std::move(cb); }
	void SetTextureUploadPending(TextureUploadPendingFn cb)
	{ m_TexUploadPending = std::move(cb); }

	// Route the mutation result through the configured callables. Updates the
	// router's tracked last-synced resource generation when a full or material
	// sync is dispatched.
	void Route(const EditorMutationResult& result, SceneManager& scene);

	// Test inspection.
	uint64_t LastSyncedResourceGeneration() const { return m_LastSyncedResourceGeneration; }
	void ResetSyncedGeneration(uint64_t gen) { m_LastSyncedResourceGeneration = gen; }

private:
	TransformSyncFn        m_TransformSync;
	MaterialSyncFn         m_MaterialSync;
	FullSyncFn             m_FullSync;
	ResetAccumFn           m_ResetAccum;
	RendererAvailableFn    m_RendererAvailable;
	TextureUploadPendingFn m_TexUploadPending;

	uint64_t m_LastSyncedResourceGeneration = 0;
};

#endif // RT2_EDITOR_SYNC_ROUTER_H