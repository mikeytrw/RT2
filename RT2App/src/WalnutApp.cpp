#include "Walnut/Application.h"
#include "Walnut/EntryPoint.h"

#include "Walnut/Image.h"
#include "Walnut/Timer.h"
#include "RendererGPU.h"
#include "RenderSettings.h"
#include "SceneManager.h"
#include "FileDialog.h"
#include "SceneTypes.h"
#include "SceneLoader.h"
#include "GPUSceneData.h"
#include "ECSScene.h"
#include "SceneGraph.h"
#include "SceneEditorUI.h"
#include "PrimitiveGeometry.h"
#include "CLIArgs.h"
#include "RTLog.h"
#include "SceneSerializer.h"
#include "SceneDocument.h"
#include "SceneAssetResolver.h"
#include "RuntimeSceneController.h"
#include "ScriptSystem.h"
#include "ScriptFieldRegistry.h"
#include "ScriptFieldResolver.h"
#include "ScriptAssetPath.h"
#include "ScriptFieldChangePolicy.h"
#include "ScriptFileWatchPolicy.h"
#include "SceneRenderBridge.h"
#include "ECSComponents.h"
#include "EditorSettings.h"
#include "SceneRecoveryService.h"
#include "UnsavedChangesCoordinator.h"
#include "ViewportCoordinates.h"
#include "EditorTransformGizmo.h"
#include "EditorCommandHistory.h"
#include "EditorSyncRouter.h"
#include "EditorCommands.h"
#include "EditorPropertyCommands.h"
#include "InputService.h"
#include "BackgroundWork.h"
#include "core/UUID.h"
#include "core/Error.h"
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <tinyexr.h>
#include "NRD.h"
#include "efsw/efsw.hpp"

#include <cstdio>
#include <cmath>
#include <thread>
#include <chrono>
#include <fstream>
#include <cassert>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

using namespace Walnut;

static CLIArgs g_CLI;

class RT2Layer : public Walnut::Layer
{
public:

	RT2Layer()
	{
		m_Cam = Camera(45.0f, 0.1f, 10000.0f, 0.005f, 2.5f);
		m_Cam.m_Aperture = 0.0f;

		// ---- Phase 1B: settings, recovery, unsaved-changes coordinator ----
		auto appData = AppDataRoot();
		m_Settings2 = std::make_unique<rt2::core::EditorSettingsStore>(appData);
		m_Recovery  = std::make_unique<rt2::core::SceneRecoveryService>(
		    appData / "Recovery",
		    nullptr,                              // real wall clock
		    rt2::core::SceneRecoveryService::kDefaultMaxRecords,
		    rt2::core::SceneRecoveryService::kDefaultIntervalSeconds);
		m_UntitledRecoveryId = rt2::core::OsUuidProvider{}.CreateV4().ToString();

		rt2::core::Error settingsErr;
		if (!m_Settings2->Load(settingsErr))
			printf("[Settings] Failed to load: %s\n", settingsErr.Format().c_str());

		// Load saved window visibility + performance detail level.
		LoadViewConfig();

		// Discover pending recovery records from a previous unclean exit.
		// We surface these as a startup Restore/Discard modal below.
		{
			rt2::core::Error rErr;
			m_PendingRecovery = m_Recovery->Discover(rErr);
			if (!rErr.IsOk())
				printf("[Recovery] Discover failed: %s\n", rErr.Format().c_str());
			if (!m_PendingRecovery.empty())
				m_RecoveryPromptOpen = true;
		}

		// Wire the unsaved-changes coordinator.
		m_Unsaved.SetIsDirtyQuery([this]() { return m_SceneMgr.IsDirty(); });
		m_Unsaved.SetSaveGate([this]() -> bool {
			// One transactional host path decides Save versus Save As from the
			// current source path. Cancellation/failure retains the pending action.
			return SaveCurrentScene(false);
		});
		m_Unsaved.SetExecuteGate([this](const rt2::core::UnsavedChangesCoordinator::PendingAction& a) {
			switch (a.kind)
			{
			case rt2::core::UnsavedChangesCoordinator::ActionKind::New:
				NewSceneInternal();
				break;
			case rt2::core::UnsavedChangesCoordinator::ActionKind::Open:
				LoadSceneInternal(a.path.string());
				break;
			case rt2::core::UnsavedChangesCoordinator::ActionKind::Recent:
				LoadSceneInternal(a.path.string());
				break;
			case rt2::core::UnsavedChangesCoordinator::ActionKind::Exit:
				SaveViewConfig();
				Walnut::Application::Get().Close();
				break;
			case rt2::core::UnsavedChangesCoordinator::ActionKind::None:
			default: break;
			}
		});
		m_Unsaved.SetDiscardRecoveryGate([this]() {
			std::string docId = rt2::core::SceneRecoveryService::DocIdFor(
			    m_SceneMgr.AuthoringDoc(), m_UntitledRecoveryId);
			m_Recovery->DiscardForDoc(docId);
		});

		// Wire the Walnut close-request callback so OS close (title-bar X /
		// Alt+F4) routes through the unsaved-changes coordinator.
		Walnut::Application::Get().SetCloseRequestCallback([this]() -> bool {
			// If a recovery prompt is open, refuse to close.
			if (m_RecoveryPromptOpen) return false;
			// If a coordinator prompt is already pending, do not clobber it.
			if (m_Unsaved.NeedsPrompt()) return false;
			bool executed = m_Unsaved.Request({rt2::core::UnsavedChangesCoordinator::ActionKind::Exit, {}});
			return !executed ? false : true;
			// When the doc is clean, Request executes immediately and returns
			// true — we return true so the app proceeds to close.
			// When dirty, Request queues the prompt and returns false — we
			// return false so the app stays open until the user resolves.
		});

		m_InspectorFieldRegistry = std::make_unique<rt2::core::ScriptFieldRegistry>();

		// Phase 6C/W2: file watcher for hot reload.
		m_FileWatchListener = std::make_unique<ScriptFileWatchListener>();
		m_FileWatcher = std::make_unique<efsw::FileWatcher>();
		m_FileWatcher->watch();

		m_EditorUI.SetSceneMgr(&m_SceneMgr);
		m_EditorUI.SetCommandHistory(&m_History);
		m_EditorUI.SetFieldRegistry(m_InspectorFieldRegistry.get());
		m_EditorUI.SetDialogInitialDirectoryProvider([this]() {
			return DialogInitialDirectory();
		});
		m_EditorUI.SetOnSceneChanged([this]() {
			if (m_RendererGPU.IsAvailable())
			{
				// Phase 3B1 invariant: compaction cannot run while any Undo
				// or Redo entry references resource slots. A snapshot's stored
				// MeshRef::meshIndex would point to a different resource or
				// nothing after compaction. Defer compaction until the history
				// is cleared (history.Clear() calls CompactMeshRegistryNow()).
				const bool historyLive = m_History.CanUndo() || m_History.CanRedo();
				bool didCompact = false;
				if (!historyLive)
					didCompact = m_SceneMgr.CompactMeshRegistry();
				if (m_PendingFullSync || didCompact)
				{
					m_SceneMgr.SetSyncCallback([this](GPUSceneData& gpuData, const RenderInstanceMap& instanceMap) {
						m_RendererGPU.SetScene(gpuData, instanceMap);
					});
					m_SceneMgr.SyncToGPU();
					m_PendingFullSync = false;
				}
				else if (!m_RendererGPU.IsTextureUploadPending())
				{
					m_SceneMgr.SetSyncKeepTexturesCallback([this](GPUSceneData& gpuData, const RenderInstanceMap& instanceMap) {
						m_RendererGPU.SetSceneKeepTextures(gpuData, instanceMap);
					});
					m_SceneMgr.SyncToGPUKeepTextures();
				}
			}
			m_RendererGPU.ResetAccumulation();
		});
		m_EditorUI.SetOnTransformChanged([this]() {
			SyncAuthoringTransforms();
		});
		// Phase 3A: configure the CPU-only sync router with the same
		// callables the m_OnMutation lambda used inline. The lambda body
		// then delegates to the router, making the impact->sync mapping
		// testable from CPU-only tests.
		m_SyncRouter.SetTransformSync([this]() { SyncAuthoringTransforms(); });
		m_SyncRouter.SetFullSync([this]() {
			if (!m_RendererGPU.IsAvailable()) return;
			m_SceneMgr.SetSyncCallback([this](GPUSceneData& gpuData,
				const RenderInstanceMap& instanceMap) {
				m_RendererGPU.SetScene(gpuData, instanceMap);
			});
			m_SceneMgr.SyncToGPU();
		});
		m_SyncRouter.SetMaterialSync([this]() {
			if (!m_RendererGPU.IsAvailable()) return;
			m_SceneMgr.SetSyncKeepTexturesCallback([this](GPUSceneData& gpuData,
				const RenderInstanceMap& instanceMap) {
				m_RendererGPU.SetSceneKeepTextures(gpuData, instanceMap);
			});
			m_SceneMgr.SyncToGPUKeepTextures();
		});
		m_SyncRouter.SetResetAccum([this]() { m_RendererGPU.ResetAccumulation(); });
		m_SyncRouter.SetRendererAvailable([this]() { return m_RendererGPU.IsAvailable(); });
		m_SyncRouter.SetTextureUploadPending([this]() { return m_RendererGPU.IsTextureUploadPending(); });
		m_EditorUI.SetOnMutation([this](rt2::core::SyncImpact impact) {
			EditorMutationResult routed;
			routed.success = true;
			routed.syncImpact = impact;
			m_SyncRouter.Route(routed, m_SceneMgr);
		});
		m_EditorUI.SetOnLoadMeshFile([this](const std::string& path) -> SceneManager::EntityId {
			return LoadMeshFileAsEntity(path);
		});
		m_EditorUI.SetOnImportGltf([this](const std::string& path) -> SceneManager::EntityId {
			std::vector<rt2::core::AssetDiagnostic> diagnostics;
			auto id = m_SceneMgr.ImportGltf(path, &diagnostics);
			LogAssetDiagnostics(diagnostics, 0, "Import");
			if (id.IsValid())
			{
				m_PendingFullSync = true;
			}
			return id;
		});
		m_EditorUI.SetOnImportWithOptions(
			[this](const std::string& path,
			       const ImportSettings& settings) -> SceneManager::EntityId
		{
			if (IsBackgroundBusy()) return SceneManager::EntityId{};

			std::string ext = path.substr(path.find_last_of('.') + 1);
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			const bool isObj = (ext == "obj");
			const std::string pathCopy = path;
			const ImportSettings settingsCopy = settings;

			// The worker parses + decodes into a temp ECSScene. The
			// completion callback (main thread) merges it into the live
			// scene + uploads to GPU.
			struct ImportResult
			{
				ECSScene ecs;
				entt::entity root = entt::null;
				bool isObj = false;
				std::vector<rt2::core::AssetDiagnostic> diagnostics;
			};
			auto result = std::make_shared<ImportResult>();
			result->isObj = isObj;
			auto textureContext = MakeExplicitTextureContext(pathCopy);

			StartBackgroundWork(isObj ? "Importing OBJ..." : "Importing glTF...",
				[result, pathCopy, settingsCopy, isObj,
				 textureContext](BackgroundWork& self) mutable -> bool
			{
				self.SetStatus("Parsing file...");
				if (isObj)
					result->root = SceneLoader::ImportObjIntoECS(
						result->ecs, pathCopy, settingsCopy,
						textureContext, result->diagnostics);
				else
					result->root = SceneLoader::ImportIntoECS(
						result->ecs, pathCopy, textureContext,
						result->diagnostics);
				return result->root != entt::null;
			},
				[this, result, pathCopy](bool success)
			{
				LogAssetDiagnostics(result->diagnostics, 0, "Import");
				if (!success)
				{
					printf("[Scene] Import failed: %s\n", pathCopy.c_str());
					m_LastStatusMsg = "Import failed";
					return;
				}

				auto id = m_SceneMgr.MergeImportedECS(std::move(result->ecs),
				                                      result->root, pathCopy);
				if (id.IsValid())
				{
					m_PendingFullSync = true;
					m_GpuSyncPending = true;
					m_EditorUI.OnImportComplete(id);
					m_SceneMgr.MarkDirty();
					m_UntitledRecoveryId = rt2::core::OsUuidProvider{}.CreateV4().ToString();
					m_Recovery->ResetSchedule();
					m_LastStatusMsg = "Imported (unsaved)";
				}
			});

			// Return invalid — the completion callback handles selection.
			return SceneManager::EntityId{};
		});
		m_EditorUI.SetOnDumpGPUTransforms([this]() {
			if (m_RendererGPU.IsAvailable())
				m_RendererGPU.DumpInstanceTransforms();
		});
		m_EditorUI.SetOnDumpNEEBuffers([this]() {
			if (m_RendererGPU.IsAvailable())
				m_RendererGPU.DumpNEEBuffers();
		});
		m_EditorUI.SetOnViewThroughCamera([this](const rt2::core::UUID& camera) {
			ViewThroughCamera(camera);
		});
		m_EditorUI.SetOnAlignCameraToView([this](const rt2::core::UUID& camera) {
			AlignCameraToView(camera);
		});
	}

	virtual void OnUIRender() override
	{
		// Phase 5: ResolveUI applies ImGui suppression and viewport
		// sub-context push/pop. The viewport hover / gizmo-consumes-mouse
		// state from the PREVIOUS frame is used here (we don't know
		// this frame's viewport hover until the viewport panel is drawn,
		// which happens later in OnUIRender). One-frame latency is
		// acceptable for context switching — the camera reads actions
		// in OnUpdate which already used the correct raw-down state.
		//
		// We do the actual viewport context push/pop AFTER the viewport
		// panel is drawn (see the viewport block), using this frame's
		// state. ResolveUI here only applies ImGui suppression.
		m_Input.ResolveUI(m_ViewportHoveredThisFrame, m_GizmoConsumesMouseThisFrame);

		if (!m_CLIProcessed)
		{
			ProcessCLIArgs();
			m_CLIProcessed = true;

			if (!g_CLI.headless && m_RendererGPU.IsAvailable())
			{
				m_Settings.showBackground = true;
				m_Settings.rasterFirst = true;
				m_Settings.nrdEnabled = true;
				m_Cam.m_Aperture = 0.0f;
				m_RendererGPU.ApplySettings(m_Settings);
			}
		}

	// Modal popups are always rendered (not gated by window visibility).
	if (ImGui::BeginPopupModal("GPU Init Failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Failed to initialize GPU renderer.\nCheck that raygen.spv / miss.spv / closesthit.spv exist next to the executable.\nSee console for details.");
		if (ImGui::Button("OK"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopupModal("Scene Load Failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Failed to load scene file.\nCheck the file path and format.\nSee console for details.");
		if (ImGui::Button("OK"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// RT init: happens once when the Info window would first show the
	// renderer status. Keep it here so it runs even if Info is hidden.
	{
		bool rtSupported = Walnut::Application::IsRayTracingSupported();
		if (rtSupported && !m_RendererGPU.IsAvailable())
		{
			if (m_RendererGPU.Init())
			{
				m_Settings = m_RendererGPU.GetSettings();
				m_RendererGPU.ApplySettings(m_Settings);
			}
		}
	}

	if (m_ShowInfoWindow)
	{
		ImGui::Begin("Camera");
		EditorCameraPose editorPose = m_Cam.GetEditorPose();
		bool cameraPoseChanged = false;
		const bool editCamera = m_Runtime.GetState() == rt2::core::SceneRunState::Edit;
		ImGui::BeginDisabled(!editCamera);
		cameraPoseChanged |= ImGui::DragFloat3("Position", &editorPose.position.x,
			0.01f, 0.0f, 0.0f, "%.6f");
		cameraPoseChanged |= ImGui::DragFloat3("Forward", &editorPose.forward.x,
			0.001f, -1.0f, 1.0f, "%.6f");
		cameraPoseChanged |= ImGui::DragFloat("Vertical FOV", &editorPose.verticalFOV,
			0.25f, 10.0f, 170.0f, "%.1f");
		if (ImGui::Button("Copy Camera Pose"))
		{
			const glm::vec3& p = editorPose.position;
			const glm::vec3& f = editorPose.forward;
			char pose[256];
			std::snprintf(pose, sizeof(pose),
			              "position=(%.6f, %.6f, %.6f) forward=(%.6f, %.6f, %.6f)",
			              p.x, p.y, p.z, f.x, f.y, f.z);
			ImGui::SetClipboardText(pose);
		}
		ImGui::SliderFloat("Move Speed", &m_Cam.m_Speed, 0.5f, 50.0f, "%.1f");
		cameraPoseChanged |= ImGui::SliderFloat("Far Clip", &editorPose.farClip,
			100.0f, 100000.0f, "%.0f");
		bool rasterFirst = m_Settings.rasterFirst;
		ImGui::BeginDisabled(rasterFirst);
		cameraPoseChanged |= ImGui::DragFloat("Aperture", &editorPose.aperture,
			0.001f, 0.0f, 5.0f);
		cameraPoseChanged |= ImGui::DragFloat("Focus Distance", &editorPose.focusDistance,
			0.1f, 0.1f, 50.0f);
		ImGui::EndDisabled();
		if (rasterFirst && editorPose.aperture > 0.0f)
		{
			editorPose.aperture = 0.0f;
			cameraPoseChanged = true;
		}
		if (cameraPoseChanged)
			ApplyEditorCameraPose(editorPose);
		if (ImGui::Button("Frame Selected")) FrameEditorSelection(true);
		ImGui::SameLine();
		if (ImGui::Button("Focus Selected")) FrameEditorSelection(false);

		ImGui::Text("Camera Bookmarks");
		for (size_t slot = 0; slot < EditorSceneState::kCameraBookmarkCount; ++slot)
		{
			ImGui::PushID(static_cast<int>(slot));
			ImGui::Text("%d", static_cast<int>(slot + 1));
			ImGui::SameLine();
			if (ImGui::SmallButton("Store"))
				m_EditorUI.CaptureCameraBookmark(slot, m_Cam.GetEditorPose());
			ImGui::SameLine();
			const EditorCameraPose* bookmark = m_EditorUI.CameraBookmark(slot);
			ImGui::BeginDisabled(bookmark == nullptr);
			if (ImGui::SmallButton("Recall") && bookmark)
				ApplyEditorCameraPose(*bookmark);
			ImGui::SameLine();
			if (ImGui::SmallButton("Clear"))
				m_EditorUI.ClearCameraBookmark(slot);
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::EndDisabled();
		ImGui::End();
	}

	// ---- Performance window ----
	// Level 1: FPS + frametime.
	// Level 2: + raster / ReSTIR GPU timings.
	// Level 3: + BLAS / TLAS / BVH build times.
	if (m_ShowPerfWindow)
	{
	ImGui::Begin("Performance");
	{
		const char* levels[] = { "1 - FPS & Frametime",
		                         "2 - Raster & ReSTIR Timings",
		                         "3 - Everything (incl. BLAS/TLAS/BVH)" };
		ImGui::Combo("Detail Level", &m_PerfDetailLevel, levels, IM_ARRAYSIZE(levels));
		ImGui::Separator();
	}
	ImGui::Text("Last Render: %.3fms", m_SmoothedFrameTime);
	ImGui::Text("FPS: %.1f", m_SmoothedFPS);
	if (m_PerfDetailLevel >= kPerfLevelPasses && m_RendererGPU.HasGpuTimings())
	{
		const auto& timings = m_RendererGPU.GetGpuTimings();
		if (timings.validMask != 0)
		{
			ImGui::Separator();
			if (m_PerfDetailLevel >= kPerfLevelEverything)
				ImGui::Text("GPU timings (frame %llu)", static_cast<unsigned long long>(timings.frameIndex));
			else
				ImGui::Text("GPU timings");
			for (uint32_t i = 0; i < static_cast<uint32_t>(GpuTimestampProfiler::Region::Count); i++)
			{
				if (!(timings.validMask & (1u << i))) continue;
				const auto region = static_cast<GpuTimestampProfiler::Region>(i);
				// Level 2 shows only the raster + ReSTIR pass regions.
				// Level 3 shows every captured region, so regions added to
				// GpuTimestampProfiler::Region later appear here with no edit.
				if (m_PerfDetailLevel < kPerfLevelEverything && !IsPassLevelRegion(region))
					continue;
				ImGui::Text("  %s: %.3f ms",
				            GpuTimestampProfiler::RegionName(region),
				            timings.milliseconds[i]);
			}
		}
	}
	if (m_PerfDetailLevel >= kPerfLevelEverything && m_RendererGPU.HasOutput())
	{
		ImGui::Separator();
		ImGui::Text("Render Res: %d x %d", m_RendererGPU.GetWidth(), m_RendererGPU.GetHeight());
	}
	if (m_PerfDetailLevel >= kPerfLevelEverything && m_RendererGPU.IsAvailable())
	{
		ImGui::Separator();
		ImGui::Text("BLAS/TLAS Build (last rebuild)");
		if (m_RendererGPU.GetLastAsTotalMs() < 0.0f)
		{
			ImGui::TextDisabled("(no build yet)");
		}
		else
		{
			ImGui::Text("  BLAS build:  %.3f ms", m_RendererGPU.GetLastBlasBuildMs());
			ImGui::Text("  TLAS build:  %.3f ms", m_RendererGPU.GetLastTlasBuildMs());
			ImGui::Text("  Total AS:    %.3f ms", m_RendererGPU.GetLastAsTotalMs());
			ImGui::Text("  BLAS count:   %u", m_RendererGPU.GetBlasCount());
		}
	}
	ImGui::End();
	}

	// ---- Render Settings window ----
	if (m_ShowRenderSettingsWin)
	{
	ImGui::Begin("Render Settings");
	ImGui::Text("Samples Per Pixel");
	ImGui::BeginDisabled(m_Settings.rasterFirst);
	if (ImGui::DragInt("SPP", &m_Settings.spp, 1.0f, 1, 1500))
		m_RendererGPU.ApplySettings(m_Settings);
	ImGui::EndDisabled();
	if (m_Settings.rasterFirst)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(unused by raster-first)");
	}
	ImGui::Text("Sample Depth");
	if (ImGui::DragInt("Bounces", &m_Settings.maxBounces, 1.0f, 2, 100))
		m_RendererGPU.ApplySettings(m_Settings);
	if (ImGui::Button("Reset")) {
		m_Settings.spp = 1;
		m_Settings.maxBounces = 2;
		m_RendererGPU.ApplySettings(m_Settings);
	};

	ImGui::Separator();
	if (ImGui::Checkbox("Raster-First Path", &m_Settings.rasterFirst))
	{
		if (m_Settings.rasterFirst)
		{
			EditorCameraPose pinhole = m_Cam.GetEditorPose();
			pinhole.aperture = 0.0f;
			ApplyEditorCameraPose(pinhole);
		}
		m_RendererGPU.ApplySettings(m_Settings);
	}
	ImGui::BeginDisabled(m_Settings.nrdEnabled);
	if (ImGui::Checkbox("Accumulate", &m_Settings.accumulate))
	{
		if (!m_Settings.accumulate)
			m_RendererGPU.ResetAccumulation();
		m_RendererGPU.ApplySettings(m_Settings);
	}
	ImGui::EndDisabled();
	if (m_Settings.rasterFirst)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(aperture=0, no DOF)");
	}
	if (ImGui::Checkbox("Show Background", &m_Settings.showBackground))
		m_RendererGPU.ApplySettings(m_Settings);
	if (ImGui::SliderFloat("Emissive Boost", &m_Settings.emissiveBoost, 0.0f, 50.0f, "%.1f"))
		m_RendererGPU.ApplySettings(m_Settings);

	bool nrdAvailable = m_Settings.rasterFirst;
	if (!nrdAvailable && m_Settings.nrdEnabled)
	{
		m_Settings.nrdEnabled = false;
		m_RendererGPU.ApplySettings(m_Settings);
	}
	ImGui::BeginDisabled(!nrdAvailable);
	if (ImGui::Checkbox("NRD Denoiser", &m_Settings.nrdEnabled))
		m_RendererGPU.ApplySettings(m_Settings);
	if (m_Settings.nrdEnabled)
	{
		ImGui::Indent();
		bool restirActive = m_Settings.restirEnabled || m_Settings.restirGIEnabled;
		ImGui::BeginDisabled(restirActive);
		if (ImGui::Checkbox("Jitter", &m_Settings.nrdJitterEnabled))
			m_RendererGPU.ApplySettings(m_Settings);
		ImGui::SameLine();
		ImGui::PushID("JitterScale");
		ImGui::SetNextItemWidth(80.0f);
		if (ImGui::SliderFloat("Scale", &m_Settings.nrdJitterScale, 0.0f, 1.0f, "%.2f"))
			m_RendererGPU.ApplySettings(m_Settings);
		ImGui::PopID();
		ImGui::EndDisabled();
		ImGui::SliderFloat("Blur Radius", &m_Settings.nrdMaxBlurRadius, 1.0f, 50.0f, "%.1f");
		ImGui::SliderInt("Accum Frames", &m_Settings.nrdMaxAccumFrames, 1, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
		ImGui::SliderFloat("Responsive Roughness", &m_Settings.nrdResponsiveRoughnessThreshold, 0.0f, 1.0f, "%.3f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Below this roughness, REBLUR shortens history. Set to 0 to disable.");
		ImGui::SliderInt("Responsive Min Frames", &m_Settings.nrdResponsiveMinAccumFrames, 0, 3);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Minimum glossy-history length when responsive accumulation is active.");
		ImGui::Checkbox("Anti-Firefly", &m_Settings.nrdAntiFirefly);
		const char* ditherOptions[] = { "Off (white noise)", "Bayer 4x4", "Interleaved Gradient" };
		int& dither = m_Settings.nrdLobeDither;
		ImGui::Combo("Lobe Dither", &dither, ditherOptions, 3);
		ImGui::SliderFloat("Split Screen", &m_Settings.nrdSplitScreen, 0.0f, 1.0f, "%.2f");
		ImGui::Unindent();
		m_RendererGPU.ApplySettings(m_Settings);
	}
	ImGui::EndDisabled();
	ImGui::Separator();

	bool restirAvailable = m_Settings.rasterFirst;
	if (!restirAvailable && m_Settings.restirEnabled)
	{
		m_Settings.restirEnabled = false;
		m_RendererGPU.ApplySettings(m_Settings);
	}
	ImGui::BeginDisabled(!restirAvailable);
	if (ImGui::Checkbox("ReSTIR DI", &m_Settings.restirEnabled))
	{
		if (m_Settings.restirEnabled)
			m_Settings.nrdJitterEnabled = false;
		m_RendererGPU.ApplySettings(m_Settings);
	}
	if (m_Settings.restirEnabled)
	{
		ImGui::Indent();
		int m = (int)m_Settings.restirFreshCandidates;
		if (ImGui::SliderInt("Fresh Candidates (M)", &m, 1, 32))
		{ m_Settings.restirFreshCandidates = (uint32_t)m; m_RendererGPU.ApplySettings(m_Settings); }
		if (ImGui::Checkbox("Temporal Reuse", &m_Settings.restirTemporalReuse))
			m_RendererGPU.ApplySettings(m_Settings);
		if (ImGui::Checkbox("Spatial Reuse", &m_Settings.restirSpatialReuse))
			m_RendererGPU.ApplySettings(m_Settings);
		int sn = (int)m_Settings.restirSpatialNeighbors;
		if (ImGui::SliderInt("Spatial Neighbors", &sn, 1, 32))
		{ m_Settings.restirSpatialNeighbors = (uint32_t)sn; m_RendererGPU.ApplySettings(m_Settings); }
		int sr = (int)m_Settings.restirSpatialRadius;
		if (ImGui::SliderInt("Spatial Radius", &sr, 5, 60))
		{ m_Settings.restirSpatialRadius = (uint32_t)sr; m_RendererGPU.ApplySettings(m_Settings); }
		if (ImGui::SliderFloat("Depth Threshold", &m_Settings.restirDepthThreshold, 0.01f, 0.5f, "%.3f"))
			m_RendererGPU.ApplySettings(m_Settings);
		if (ImGui::SliderFloat("Normal Threshold", &m_Settings.restirNormalThreshold, 0.8f, 1.0f, "%.3f"))
			m_RendererGPU.ApplySettings(m_Settings);
		ImGui::Unindent();
	}
	ImGui::EndDisabled();
	ImGui::Separator();

	// ReSTIR GI — one-bounce diffuse GI, temporal reuse, raster-first only.
	// Independent of DI; requires raster-first G-buffer but not DI enabled.
	bool giAvailable = m_RendererGPU.IsAvailable() && m_Settings.rasterFirst;
	if (!giAvailable && m_Settings.restirGIEnabled)
	{
		m_Settings.restirGIEnabled = false;
		m_RendererGPU.ApplySettings(m_Settings);
	}
	ImGui::BeginDisabled(!giAvailable);
	if (ImGui::Checkbox("ReSTIR GI", &m_Settings.restirGIEnabled))
	{
		if (m_Settings.restirGIEnabled)
			m_Settings.nrdJitterEnabled = false;
		m_RendererGPU.ApplySettings(m_Settings);
	}
	if (m_Settings.restirGIEnabled)
	{
		ImGui::Indent();
		if (ImGui::Checkbox("Temporal Reuse", &m_Settings.restirGITemporalEnabled))
			m_RendererGPU.ApplySettings(m_Settings);
		int m = (int)m_Settings.restirGIFreshCandidates;
		if (ImGui::SliderInt("GI Fresh Candidates (M)", &m, 1, 8))
		{ m_Settings.restirGIFreshCandidates = (uint32_t)m; m_RendererGPU.ApplySettings(m_Settings); }
		int tmc = (int)m_Settings.restirGITemporalMCap;
		if (ImGui::SliderInt("GI Temporal M Cap", &tmc, 1, 100))
		{ m_Settings.restirGITemporalMCap = (uint32_t)tmc; m_RendererGPU.ApplySettings(m_Settings); }
		int age = (int)m_Settings.restirGIMaxTemporalAge;
		if (ImGui::SliderInt("GI Max Temporal Age", &age, 0, 16))
		{ m_Settings.restirGIMaxTemporalAge = (uint32_t)age; m_RendererGPU.ApplySettings(m_Settings); }
		if (ImGui::SliderFloat("GI Depth Threshold", &m_Settings.restirGIDepthThreshold, 0.01f, 0.5f, "%.3f"))
			m_RendererGPU.ApplySettings(m_Settings);
		if (ImGui::SliderFloat("GI Normal Threshold", &m_Settings.restirGINormalThreshold, 0.5f, 1.0f, "%.3f"))
			m_RendererGPU.ApplySettings(m_Settings);
		if (ImGui::SliderFloat("GI WorldPos Threshold", &m_Settings.restirGIWorldPosThreshold, 0.01f, 0.5f, "%.3f"))
			m_RendererGPU.ApplySettings(m_Settings);
		ImGui::Unindent();
	}
	ImGui::EndDisabled();
	ImGui::Separator();
	ImGui::Text("G-buffer Debug");
	const char* gbufferModes[] = {
		"Off", "Shading Normal", "Roughness", "ViewZ (depth)",
		"Motion Vectors", "Albedo", "F0", "Direct Emission",
		"World Position", "Geo Normal", "UV", "Material Index",
		"ReSTIR Reservoir",
		"GI Direction", "GI Lo", "GI HitT", "GI M/Age",
		"GI Fresh vs History", "GI Rejection Reason", "GI Fallback",
		"NRD Diffuse Input", "NRD Specular Input", "ReSTIR DI Weight"
	};
	int debugCombo = m_Settings.gbufferDebugMode + 1;
	if (ImGui::Combo("G-buffer View", &debugCombo, gbufferModes, IM_ARRAYSIZE(gbufferModes)))
	{
		m_Settings.gbufferDebugMode = debugCombo - 1;
		m_RendererGPU.ApplySettings(m_Settings);
	}
	ImGui::End();
	}

	if (m_ShowSceneWindow)
	{
	ImGui::Begin("Scene");
	ImGui::Text("Meshes: %d", (int)m_SceneMgr.GetECS().meshRegistry.GetCount());
	ImGui::Text("Materials: %d", (int)m_SceneMgr.GetMaterials().size());
	ImGui::Text("Lights: %d", (int)m_SceneMgr.GetECS().lights.size());
	ImGui::Text("Textures: %d", (int)m_SceneMgr.GetECS().textures.size());
	if (m_SceneMgr.IsDirty())
		ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "Unsaved changes");
	ImGui::Separator();

	// ---- File operations ----
	if (ImGui::Button("New"))
		NewScene();
	ImGui::SameLine();
	if (ImGui::Button("Open..."))
	{
		std::string path = FileDialog::OpenFile(
			L"RT2 Scene (*.rt2scene)\0*.rt2scene\0glTF Binary (*.glb)\0*.glb\0glTF JSON (*.gltf)\0*.gltf\0OBJ Files (*.obj)\0*.obj\0All Files (*.*)\0*.*\0",
			DialogInitialDirectory());
		if (!path.empty())
			RequestOpenScene(path);
	}
	ImGui::SameLine();
	if (ImGui::Button("Save"))
		SaveRt2Scene();
	ImGui::SameLine();
	if (ImGui::Button("Save As..."))
		SaveRt2SceneAs();

	ImGui::Separator();

	// ---- Play/Pause/Step/Stop controls ----
	{
		auto state = m_Runtime.GetState();
		bool playing = (state == rt2::core::SceneRunState::Playing);
		bool paused  = (state == rt2::core::SceneRunState::Paused);
		bool editing = (state == rt2::core::SceneRunState::Edit);

		ImGui::BeginDisabled(playing);
		if (ImGui::Button("Play"))
		{
			if (paused)
				m_Runtime.Resume();
			else
				EnterPlay();
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::BeginDisabled(!playing);
		if (ImGui::Button("Pause"))
			EnterPause();
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::BeginDisabled(!paused);
		if (ImGui::Button("Step"))
			EnterStep();
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::BeginDisabled(editing);
		if (ImGui::Button("Stop"))
			EnterStop();
		ImGui::EndDisabled();

		// State indicator
		const char* stateStr = editing ? "Edit" : (paused ? "Paused" : "Playing");
		ImGui::SameLine();
		ImGui::TextDisabled("(%s)", stateStr);
	}

	ImGui::Separator();
	ImGui::Text("Environment Map");
	if (ImGui::Button("Load HDR..."))
	{
		std::string path = FileDialog::OpenFile(
			L"HDR Files (*.hdr;*.exr)\0*.hdr;*.exr\0HDR (*.hdr)\0*.hdr\0All Files (*.*)\0*.*\0",
			DialogInitialDirectory());
		if (!path.empty())
			LoadEnvMap(path);
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear HDR"))
	{
		m_SceneMgr.ClearEnvMap();
		if (m_RendererGPU.IsAvailable() && m_SceneMgr.GetECS().meshRegistry.GetCount() > 0)
			UploadMeshToGPU();
		m_RendererGPU.ResetAccumulation();
	}
	if (m_SceneMgr.HasEnvMap())
		ImGui::Text("Loaded: %s (%dx%d)", m_SceneMgr.GetEnvMapPath().c_str(), m_SceneMgr.GetEnvMapWidth(), m_SceneMgr.GetEnvMapHeight());
	if (ImGui::SliderFloat("Env Intensity", &m_Settings.envIntensity, 0.0f, 10.0f, "%.2f"))
		m_RendererGPU.ApplySettings(m_Settings);

	ImGui::End();
	}

	if (auto pick = m_RendererGPU.ConsumePickResult())
	{
		if (m_Runtime.GetState() == rt2::core::SceneRunState::Edit &&
			pick->hit && m_SceneMgr.FindEntityByUuid(pick->entityUuid) != entt::null)
		{
			if (pick->serial == m_ViewportPickSerial && m_ViewportPickToggle)
				m_EditorUI.Selection().Toggle(pick->entityUuid);
			else
				m_EditorUI.SelectUuid(pick->entityUuid);
		}
		else if (m_Runtime.GetState() == rt2::core::SceneRunState::Edit &&
			!(pick->serial == m_ViewportPickSerial && m_ViewportPickToggle))
			m_EditorUI.ClearSelection();
	}

	m_EditorUI.SetOutlinerVisible(m_ShowHierarchyWindow);
	m_EditorUI.SetInspectorVisible(m_ShowInspectorWindow);
 	m_EditorUI.RenderPanels();
	HandleEditorCameraShortcuts();
	HandleUndoRedoShortcuts();

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("Viewport");

	m_ViewportWidth = (uint32_t)ImGui::GetContentRegionAvail().x;
	m_ViewportHeight = (uint32_t)ImGui::GetContentRegionAvail().y;

	if (m_RendererGPU.IsAvailable())
	{
		m_RendererGPU.OnResize(m_ViewportWidth, m_ViewportHeight);
		Camera& activeCam = m_RuntimeCamActive ? m_RuntimeCam : m_Cam;
		activeCam.OnResize(m_ViewportWidth, m_ViewportHeight);
	}

	if (m_RendererGPU.HasOutput())
	{
		const ImVec2 imageMin = ImGui::GetCursorScreenPos();
		const ImVec2 imageSize((float)m_RendererGPU.GetWidth(),
		                       (float)m_RendererGPU.GetHeight());
		ImGui::Image((ImTextureID)m_RendererGPU.GetOutputDescriptorSet(),
		             imageSize);
		const bool imageHovered = ImGui::IsItemHovered();
		const TransformGizmoResult gizmo = m_TransformGizmo.Draw(
			m_SceneMgr, m_EditorUI.Selection(), m_Cam,
			{ imageMin.x, imageMin.y }, { imageSize.x, imageSize.y }, imageHovered,
			m_Runtime.GetState() == rt2::core::SceneRunState::Edit &&
				!m_EditorUI.SelectionHasDirectLock(),
			m_EditorUI.GetTransformSpace(), m_EditorUI.GetTransformPivot(),
			m_EditorUI.GetTransformSnapSettings(), m_EditorUI.GetUniformScale());
		if (gizmo.changed)
			SyncAuthoringTransforms();
		if (!gizmo.error.empty())
			m_LastStatusMsg = gizmo.error;

		// Phase 3B1: on gizmo drag end, build a multi-entity TransformCommand
		// and record it via RecordApplied. The gizmo's per-frame
		// TrySetWorldTransforms calls already applied the mutation; we only
		// record the command here. The before-local TRS was captured at drag
		// start; the after-local TRS is read live now.
		if (gizmo.dragJustEnded && !gizmo.draggedUuids.empty())
		{
			std::vector<TransformTriple> triples;
			triples.reserve(gizmo.draggedUuids.size());
			for (std::size_t i = 0; i < gizmo.draggedUuids.size(); ++i)
			{
				const auto& uuid = gizmo.draggedUuids[i];
				const auto entity = m_SceneMgr.FindEntityByUuid(uuid);
				if (entity == entt::null) continue;
				EditableTRS afterLocal;
				if (!m_SceneMgr.GetLocalTransform(SceneManager::EntityId{ entity }, afterLocal))
					continue;
				triples.push_back({ uuid, gizmo.dragStartLocal[i], afterLocal });
			}
			auto cmd = MakeTransformCommandIfEffective(std::move(triples));
			if (cmd)
			{
				EditorMutationResult applied;
				applied.success = true;
				applied.syncImpact = rt2::core::SyncImpact::Transform;
				for (std::size_t i = 0; i < gizmo.draggedUuids.size(); ++i)
					applied.affectedEntities.push_back(gizmo.draggedUuids[i]);
				m_History.RecordApplied(std::move(cmd), m_SceneMgr, applied);
			}
		}

		const bool ordinaryPickClick = imageHovered && !gizmo.consumesMouse &&
			m_Input.IsPressed("viewport_pick");
		const bool canPick = m_Runtime.GetState() == rt2::core::SceneRunState::Edit &&
			(ordinaryPickClick || gizmo.pickThrough) &&
			!m_Input.IsDown("look");
		if (imageHovered && m_Input.IsPressed("viewport_pick"))
		{
			RT_LOG("[ViewportPick] click edit=%d gizmoConsumes=%d gizmoActive=%d rightDown=%d",
				m_Runtime.GetState() == rt2::core::SceneRunState::Edit ? 1 : 0,
				gizmo.consumesMouse ? 1 : 0, gizmo.active ? 1 : 0,
				m_Input.IsDown("look") ? 1 : 0);
		}
		if (canPick)
		{
			const ImVec2 mouse = ImGui::GetMousePos();
			ViewportImageRect rect;
			rect.screenMin = { imageMin.x, imageMin.y };
			rect.displaySize = { imageSize.x, imageSize.y };
			rect.renderExtent = { m_RendererGPU.GetWidth(), m_RendererGPU.GetHeight() };
			if (const auto pixel = ScreenToRenderPixel({ mouse.x, mouse.y }, rect))
			{
				const glm::vec2 uv = (glm::vec2(*pixel) + 0.5f) /
					glm::vec2(rect.renderExtent);
				const uint64_t serial = m_RendererGPU.RequestPick(
					m_Cam.GetPickingRay(uv.x, uv.y), m_Cam.m_FarClip);
				m_ViewportPickSerial = serial;
				m_ViewportPickToggle = ImGui::GetIO().KeyCtrl;
				RT_LOG("[ViewportPick] requested serial=%llu pixel=(%u,%u) uv=(%.6f,%.6f)",
					static_cast<unsigned long long>(serial), pixel->x, pixel->y, uv.x, uv.y);
			}
		}

		// Phase 5: capture viewport hover + gizmo-consumes-mouse for next
		// frame's ResolveUI, and push/pop the viewport sub-contexts based
		// on this frame's state. Context transitions are applied here so
		// they take effect for the next frame's OnUpdate (which reads
		// actions for the camera).
		m_ViewportHoveredThisFrame = imageHovered;
		m_GizmoConsumesMouseThisFrame = gizmo.consumesMouse;
	}

	ImGui::End();
	ImGui::PopStyleVar();

	// ---- Phase 1B: Session / Recovery UI ----
	if (m_ShowSessionWindow)
		DrawSessionPanel();
	DrawRecoveryPrompt();
	DrawUnsavedChangesPrompt();
	DrawLoadingModal();

	// Phase 6C/W2: drain file-watcher changes with debounce. Atomic save
	// (temp + rename) yields Modified + Added + Deleted for one Ctrl+S.
	// Coalesce by path over a ~100ms quiet window, then reload.
	if (m_FileWatchListener)
	{
		{
			std::lock_guard<std::mutex> lock(m_FileWatchListener->mutex);
			if (!m_FileWatchListener->pendingChanges.empty())
			{
				for (const auto& p : m_FileWatchListener->pendingChanges)
				{
					// Dedupe: don't add a path already in the debounce buffer.
					if (std::find(m_DebouncedChanges.begin(),
					              m_DebouncedChanges.end(), p) ==
					    m_DebouncedChanges.end())
						m_DebouncedChanges.push_back(p);
				}
				m_FileWatchListener->pendingChanges.clear();
				m_LastFileChangeTime = std::chrono::steady_clock::now();
			}
		}

		// If no new changes for 100ms and we have pending, drain.
		if (!m_DebouncedChanges.empty())
		{
			const auto now = std::chrono::steady_clock::now();
			const auto elapsed = std::chrono::duration_cast<
				std::chrono::milliseconds>(now - m_LastFileChangeTime);
			if (elapsed.count() >= 100)
			{
				auto changes = std::move(m_DebouncedChanges);
				m_DebouncedChanges.clear();
				const auto action = rt2::core::DecideScriptFileChange(
					m_Runtime.GetState(),
					m_ScriptSystem != nullptr,
					m_InspectorFieldRegistry != nullptr);
				for (const auto& path : changes)
				{
					if (action.reloadScript)
						m_ScriptSystem->ReloadScript(path);
				}
				// Once per drain, not per path: the inspector cache is
				// whole-registry, so clearing it per file would be
				// redundant work.
				if (action.invalidateFieldRegistry)
					m_InspectorFieldRegistry->Clear();
			}
		}
	}

	// Phase 5: EndFrame commits current → previous state, clears
	// per-frame deltas, and applies cursor capture. Called at the end
	// of OnUIRender so all UI consumers have run.
	m_Input.EndFrame();
	} // end OnUIRender

	// ---- Phase 1B: Session / Recovery / Unsaved-changes UI ----

	void DrawSessionPanel()
	{
		ImGui::Begin("Session");
		ImGui::Text("Dirty: %s", m_SceneMgr.IsDirty() ? "yes" : "no");
		ImGui::Text("Revision: %llu", static_cast<unsigned long long>(m_SceneMgr.AuthoringRevision()));
		ImGui::Text("Status: %s", m_LastStatusMsg.c_str());
		ImGui::Separator();
		if (m_Settings2)
		{
			ImGui::Text("Project Root");
			auto pr = m_Settings2->GetProjectRoot();
			char buf[512];
			std::snprintf(buf, sizeof(buf), "%s", pr.empty() ? "(none)" : pr.string().c_str());
			ImGui::InputText("##ProjectRoot", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
			ImGui::SameLine();
			if (ImGui::Button("Browse..."))
			{
				std::string folder = FileDialog::OpenFolder(DialogInitialDirectory());
				if (!folder.empty())
				{
					m_Settings2->SetProjectRoot(std::filesystem::path(folder));
					PersistEditorSettings("project root");
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Clear"))
			{
				m_Settings2->ClearProjectRoot();
				PersistEditorSettings("project root");
			}
		}
		ImGui::Separator();
		if (m_Settings2 && !m_Settings2->GetRecentScenes().empty())
		{
			ImGui::Text("Recent Scenes");
			auto recents = m_Settings2->GetRecentScenes();
			for (size_t i = 0; i < recents.size(); ++i)
			{
				const auto& p = recents[i];
				bool exists = std::filesystem::exists(p);
				std::string label = p.string();
				if (!exists) label = "(missing) " + label;
				if (ImGui::MenuItem(label.c_str()))
				{
					if (exists)
					{
						rt2::core::UnsavedChangesCoordinator::PendingAction a;
						a.kind = rt2::core::UnsavedChangesCoordinator::ActionKind::Recent;
						a.path = p;
						m_Unsaved.Request(a);
					}
				}
				if (!exists)
				{
					ImGui::SameLine();
					std::string removeId = "Remove##" + std::to_string(i);
					if (ImGui::SmallButton(removeId.c_str()))
					{
						m_Settings2->RemoveRecentScene(p);
						PersistEditorSettings("recent scenes");
					}
				}
			}
		}
		ImGui::End();
	}

	void DrawRecoveryPrompt()
	{
		if (!m_RecoveryPromptOpen || m_PendingRecovery.empty()) return;
		ImGui::OpenPopup("Recovery Available");
		m_RecoveryPromptOpen = false; // we opened it; let the modal drive state
		if (ImGui::BeginPopupModal("Recovery Available", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Unsaved work from a previous session is available.");
			size_t idx = m_RecoveryPromptIndex < m_PendingRecovery.size() ? m_RecoveryPromptIndex : 0;
			const auto& r = m_PendingRecovery[idx];
			ImGui::Text("Doc: %s", r.docId.c_str());
			ImGui::Text("Created: %lld", static_cast<long long>(r.createdAtUnix));
			ImGui::Text("Original: %s", r.originalSourcePath.empty() ? "(untitled)" : r.originalSourcePath.string().c_str());
			if (!r.valid)
				ImGui::TextColored({1,0.6f,0.4f,1}, "Malformed: %s", r.diagnostic.c_str());
			if (ImGui::Button("Restore"))
			{
				rt2::core::Error err;
				std::vector<rt2::core::AssetDiagnostic> diags;
				rt2::core::SceneLoadReport loadReport;
				std::vector<rt2::core::FieldDiagnostic> fieldDiags;
				rt2::core::ScriptFieldResolutionResult fieldResolution;
				rt2::core::ScriptFieldChangeClassification fieldChanges;
				rt2::core::SceneDocument restored;
				restored.SetUuidProvider(m_SceneMgr.AuthoringDoc().GetUuidProvider());
				bool ok = m_Recovery->Restore(
					r, restored, diags, loadReport, err);
				if (ok)
				{
					fieldDiags = loadReport.fieldDiagnostics;
					rt2::core::ScriptFieldRegistry fieldRegistry;
					rt2::core::AssetResolutionContext assetContext{
						r.assetRoot, nullptr};
					fieldResolution = rt2::core::ScriptFieldResolver::ResolveDocument(
						restored, fieldRegistry, assetContext, diags,
						fieldDiags);
					fieldChanges = rt2::core::ClassifyScriptFieldChanges(
						loadReport, fieldResolution, fieldDiags);
					for (const auto& diagnostic : fieldDiags)
						printf("[Recovery] Script field: %s\n", diagnostic.message.c_str());
				}
			if (ok)
			{
				// Commit the already validated document without clearing it.
				m_SceneMgr.ReplaceAuthoringDocument(
					std::move(restored), std::max<uint64_t>(1, r.revision));
			m_EditorUI.ResetForDocument();
			if (m_InspectorFieldRegistry) m_InspectorFieldRegistry->Clear();
			m_History.Clear();
				CompactMeshRegistryNowAsserted();
				m_Recovery->ResetSchedule();
				m_ScriptRepairGate.Adopt(fieldChanges.destructive);
				m_LastStatusMsg = fieldChanges.destructive
					? "Restored recovery with discarded script field data; Save once to acknowledge"
					: (fieldChanges.requiresSave
						? "Restored recovery; script fields changed and need saving"
						: "Restored recovery");
					// Upload to GPU
					if (m_RendererGPU.IsAvailable() && m_SceneMgr.GetECS().meshRegistry.GetCount() > 0)
						UploadMeshToGPU();
					m_RendererGPU.ResetAccumulation();
					// Adopt the scene camera
					const auto& cam = m_SceneMgr.GetECS().camera;
					m_Cam.SetPosition(cam.position);
					m_Cam.SetForwardDirection(cam.forwardDirection);

					// Stop offering this record during this process, but deliberately
					// keep it on disk until explicit Save or Discard.
					m_PendingRecovery.erase(m_PendingRecovery.begin() + idx);
					if (!m_PendingRecovery.empty()) m_RecoveryPromptOpen = true;
					ImGui::CloseCurrentPopup();
				}
				else
				{
					m_LastStatusMsg = std::string("Restore failed: ") + err.Format();
					printf("[Recovery] Restore failed: %s\n", err.Format().c_str());
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Discard"))
			{
				rt2::core::Error err;
				if (m_Recovery->Discard(r, err))
				{
					m_PendingRecovery.erase(m_PendingRecovery.begin() + idx);
					if (m_PendingRecovery.empty()) m_RecoveryPromptIndex = 0;
					else
					{
						if (m_RecoveryPromptIndex >= m_PendingRecovery.size()) m_RecoveryPromptIndex = 0;
						m_RecoveryPromptOpen = true;
					}
					m_LastStatusMsg = "Discarded recovery";
					ImGui::CloseCurrentPopup();
				}
				else
				{
					m_LastStatusMsg = std::string("Discard failed: ") + err.Format();
					printf("[Recovery] %s\n", m_LastStatusMsg.c_str());
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Skip"))
			{
				// Leave the record intact, advance to the next or close.
				if (m_PendingRecovery.size() > 1)
				{
					m_RecoveryPromptIndex = (m_RecoveryPromptIndex + 1) % m_PendingRecovery.size();
					m_RecoveryPromptOpen = true;
				}
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	void DrawUnsavedChangesPrompt()
	{
		if (!m_Unsaved.NeedsPrompt()) return;
		ImGui::OpenPopup("Unsaved Changes");
		if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			const auto& a = m_Unsaved.Pending();
			const char* actionName = "action";
			switch (a.kind)
			{
			case rt2::core::UnsavedChangesCoordinator::ActionKind::New:    actionName = "New Scene"; break;
			case rt2::core::UnsavedChangesCoordinator::ActionKind::Open:  actionName = "Open Scene"; break;
			case rt2::core::UnsavedChangesCoordinator::ActionKind::Recent: actionName = "Open Recent"; break;
			case rt2::core::UnsavedChangesCoordinator::ActionKind::Exit:  actionName = "Exit"; break;
			default: break;
			}
			ImGui::Text("You have unsaved changes.");
			ImGui::Text("Pending: %s", actionName);
			if (!a.path.empty())
				ImGui::Text("To: %s", a.path.string().c_str());
			if (ImGui::Button("Save"))
			{
				m_Unsaved.ResolveSave();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Discard"))
			{
				m_Unsaved.ResolveDiscard();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				m_Unsaved.ResolveCancel();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	// ---- Loading modal ----
	// Shown while a BackgroundWork is running. The modal is modal (no other
	// UI interactions possible), but the main loop keeps running so the
	// window doesn't freeze and the status text updates each frame. When
	// the work completes, the thread is joined and the completion callback
	// runs on the main thread (safe for Vulkan/GPU calls).
	void DrawLoadingModal()
	{
		// The modal stays open while either background work is running OR
		// the GPU sync phase (texture upload + AS rebuild) is pending. It is
		// also submitted for one final frame after both finish, because
		// ImGui::CloseCurrentPopup() is only valid between BeginPopupModal()
		// and EndPopup() — closing from outside that scope is a no-op.
		if (!m_BackgroundWork && !m_GpuSyncPending && !m_LoadingModalOpen) return;

		// Open the modal on the first frame.
		if (!m_LoadingModalOpen)
		{
			m_LoadingModalOpen = true;
			ImGui::OpenPopup("Loading...");
			printf("[LoadingModal] opened (bgWork=%d gpuSync=%d)\n",
			       m_BackgroundWork ? 1 : 0, m_GpuSyncPending ? 1 : 0);
			fflush(stdout);
		}

		if (ImGui::BeginPopupModal("Loading...", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize))
		{
			const char* spinner[] = { "|", "/", "-", "\\" };
			const double time = ImGui::GetTime();
			const int frame = static_cast<int>(time * 4.0) & 3;

			const char* status = "Working...";
			if (m_GpuSyncPending)
				status = m_GpuSyncStatus.c_str();
			else if (m_BackgroundWork)
				status = m_BackgroundWork->GetStatus().c_str();

			ImGui::Text("%s %s", spinner[frame], status);

			const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(time) * 3.0f);
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
			ImGui::ProgressBar(pulse, ImVec2(200.0f, 6.0f), "");
			ImGui::PopStyleColor();

			// All work finished on a previous frame — close from inside the
			// popup scope, the only place CloseCurrentPopup() is valid.
			if (!m_BackgroundWork && !m_GpuSyncPending)
			{
				m_LoadingModalOpen = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		// Phase 1: wait for the background worker thread to finish.
		if (m_BackgroundWork && m_BackgroundWork->JoinIfDone())
		{
			printf("[LoadingModal] Phase 1: worker done, running completion callback\n");
			fflush(stdout);
			const bool success = m_BackgroundWork->GetResult();
			m_BackgroundWork.reset();
			// Don't close the modal yet — the completion callback may set
			// m_GpuSyncPending to keep it open for the GPU sync phase.
			if (m_OnBackgroundComplete)
			{
				auto cb = std::move(m_OnBackgroundComplete);
				m_OnBackgroundComplete = nullptr;
				// Seed a status for the GPU phase so its first frame shows
				// something meaningful rather than a stale/empty string.
				m_GpuSyncStatus = "Preparing GPU upload...";
				cb(success);
			}
			printf("[LoadingModal] Phase 1 done: gpuSyncPending=%d\n", m_GpuSyncPending ? 1 : 0);
			fflush(stdout);
			// If the callback didn't set m_GpuSyncPending there is nothing
			// left to do; the popup block above closes the modal next frame.
			return;
		}

		// Phase 2: GPU sync (texture upload + AS rebuild). This runs on the
		// main thread (Vulkan queue ownership). The modal stays visible so
		// the user sees feedback instead of a frozen window.
		if (m_GpuSyncPending)
		{
			if (m_RendererGPU.IsAvailable())
			{
				// Poll async texture upload — Adopt() blocks on
				// vkWaitForFences when the upload is complete.
				if (m_RendererGPU.IsTextureUploadPending())
				{
					// Announce BEFORE blocking: the modal above already drew
					// this frame, so doing the work now would leave the user
					// staring at the previous status for the whole stall.
					// Set the status, render one frame, then block next frame.
					static const char* kTextureStatus = "Uploading textures to GPU...";
					if (m_GpuSyncStatus != kTextureStatus)
					{
						m_GpuSyncStatus = kTextureStatus;
						return;
					}
					printf("[LoadingModal] Phase 2: polling texture upload (pending=%d)\n",
					       m_RendererGPU.IsTextureUploadPending() ? 1 : 0);
					fflush(stdout);
					m_RendererGPU.PollTextureUpload();
					printf("[LoadingModal] Phase 2: PollTextureUpload returned (pending=%d)\n",
					       m_RendererGPU.IsTextureUploadPending() ? 1 : 0);
					fflush(stdout);
					// PollTextureUpload returns true when adopted; if it
					// did, fall through to AS rebuild check. If not yet
					// complete, return and try again next frame.
					if (m_RendererGPU.IsTextureUploadPending())
						return;
				}

				// Synchronous AS rebuild. The BLAS/TLAS build blocks the main
				// thread (hundreds of ImmediateSubmit/vkQueueWaitIdle calls for
				// staging uploads + the build itself). The loading modal shows
				// "Building acceleration structures..." before the freeze, so
				// the user sees feedback instead of a blank window. Running
				// this on a worker thread crashes because Vulkan queues are
				// not thread-safe (the render loop also submits to the queue).
				if (m_RendererGPU.NeedsASRebuild() || m_RendererGPU.IsASRebuildPending())
				{
					// Announce before the submit (the CPU-side geometry/
					// attribute upload still costs time), then drive the
					// GPU build through the ASYNC path: submit once with a
					// fence and poll it each frame. The synchronous
					// RebuildAccelerationStructures() blocks the main thread
					// for the whole BLAS/TLAS build — with ~9k meshes that
					// froze the UI for the entire build, which is exactly
					// what this modal exists to avoid.
					static const char* kASStatus = "Building acceleration structures...";
					if (m_GpuSyncStatus != kASStatus)
					{
						m_GpuSyncStatus = kASStatus;
						return;
					}

					if (!m_RendererGPU.IsASRebuildPending())
					{
						printf("[LoadingModal] Phase 2: beginning async AS rebuild\n");
						fflush(stdout);
						if (!m_RendererGPU.BeginRebuildAccelerationStructures())
						{
							// Could not submit the async build — fall back to
							// the blocking path so the scene still becomes
							// renderable (it updates descriptors itself).
							printf("[LoadingModal] Phase 2: async submit failed, falling back to sync\n");
							fflush(stdout);
							m_RendererGPU.RebuildAccelerationStructures();
						}
					}

					if (m_RendererGPU.IsASRebuildPending())
					{
						// Non-blocking fence check; keeps the modal animating.
						if (!m_RendererGPU.PollASRebuild())
							return; // still building — poll again next frame
						printf("[LoadingModal] Phase 2: async AS rebuild complete\n");
						fflush(stdout);
						m_RendererGPU.UpdateDescriptorSetAfterAS();
					}
				}
			}

			// GPU sync complete. The popup block above closes the modal on the
			// next frame, from inside the BeginPopupModal/EndPopup scope.
			printf("[LoadingModal] GPU sync complete, closing modal\n");
			fflush(stdout);
			m_GpuSyncPending = false;
		}
	}

	// Start a background work operation. Only one may be active at a time.
	// The completion callback runs on the main thread after the worker
	// joins, so it's safe to do Vulkan/GPU calls there. If work is already
	// active, this is a no-op (the caller should check IsBackgroundBusy).
	bool IsBackgroundBusy() const { return m_BackgroundWork != nullptr; }

	// Block until any background work completes (used by headless mode
	// where the UI loop doesn't run to poll completion). Returns the
	// success result and runs the completion callback. Also runs the
	// GPU sync phase (texture upload + AS rebuild) synchronously.
	void WaitForBackgroundWork()
	{
		if (!m_BackgroundWork) return;
		m_BackgroundWork->Join();
		const bool success = m_BackgroundWork->GetResult();
		m_BackgroundWork.reset();
		m_LoadingModalOpen = false;
		if (m_OnBackgroundComplete)
		{
			auto cb = std::move(m_OnBackgroundComplete);
			m_OnBackgroundComplete = nullptr;
			cb(success);
		}

		// Run the GPU sync phase synchronously (headless has no UI loop).
		if (m_GpuSyncPending && m_RendererGPU.IsAvailable())
		{
			while (m_RendererGPU.IsTextureUploadPending())
			{
				m_RendererGPU.PollTextureUpload();
				if (m_RendererGPU.IsTextureUploadPending())
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			// Synchronous AS rebuild (headless — no UI loop).
			if (m_RendererGPU.NeedsASRebuild())
				m_RendererGPU.RebuildAccelerationStructures();
			m_GpuSyncPending = false;
		}
	}

	void StartBackgroundWork(const std::string& initialStatus,
		BackgroundWork::WorkFn workFn,
		std::function<void(bool)> onComplete)
	{
		if (m_BackgroundWork) return;
		m_BackgroundWork = std::make_unique<BackgroundWork>();
		m_OnBackgroundComplete = std::move(onComplete);
		m_BackgroundWork->Run(initialStatus, std::move(workFn));
	}

	virtual void OnUpdate(float ts) override
	{
		// Phase 5: sample raw input at the top of OnUpdate (before camera).
		// ResolveUI runs later in OnUIRender after ImGui::NewFrame.
		if (!m_InputDefaultsLoaded)
		{
			m_Input.LoadDefaults();
			m_Input.PushContext(&m_Input.EditorContext());
			m_InputDefaultsLoaded = true;
		}
		m_Input.SampleRaw();

		// Phase 5: viewport sub-context management. Context transitions
		// use the previous frame's viewport-hover state (captured at the
		// end of the viewport panel draw). One-frame latency is
		// acceptable — the camera reads actions here, and the viewport
		// sub-contexts only affect W/E claim resolution.
		//
		// Stack base: editor (Edit) or runtime (Play).
		// Pushed on top: "viewport" when viewport hovered (Edit only).
		// Pushed on top of that: "viewport.look" when right-mouse held.
		const bool inEdit = m_Runtime.GetState() == rt2::core::SceneRunState::Edit;
		const bool lookDown = m_Input.IsDown("look");

		// Sync the base context: editor (Edit) or runtime (Play/Paused).
		// We track this with m_RuntimeCamActive — when it's true, the
		// runtime context should be the base. This is set in EnterPlay
		// and cleared in EnterStop.
		auto& stack = m_Input.StateMachine().ContextStack();
		// Determine the desired base context.
		rt2::core::InputContext* desiredBase =
			m_RuntimeCamActive ? &m_Input.RuntimeContext() : &m_Input.EditorContext();
		// Build the desired context stack for this frame: base, then the
		// viewport sub-contexts (Edit only), then viewport.look while the
		// right mouse is held.
		std::vector<rt2::core::InputContext*> desiredStack;
		desiredStack.push_back(desiredBase);
		if (inEdit && m_ViewportHoveredThisFrame)
		{
			desiredStack.push_back(&m_Input.ViewportContext());
			if (lookDown)
				desiredStack.push_back(&m_Input.ViewportLookContext());
		}
		// Only rebuild the stack when it actually changes. PopContext /
		// ClearContextStack reset the mouse-delta history; doing that every
		// frame (as the old unconditional teardown/re-push did) zeroes the
		// editor camera's look delta, so pan/tilt drops out intermittently.
		// Steady-state frames must leave the stack — and the mouse history —
		// untouched.
		bool stackDiffers = stack.size() != desiredStack.size();
		if (!stackDiffers)
		{
			for (size_t i = 0; i < desiredStack.size(); ++i)
				if (stack[i] != desiredStack[i]) { stackDiffers = true; break; }
		}
		if (stackDiffers)
		{
			m_Input.ClearContextStack();
			for (rt2::core::InputContext* ctx : desiredStack)
				m_Input.PushContext(ctx);
		}

		if (m_RuntimeCamActive)
			m_RuntimeCam.OnUpdate(ts, m_Input);
		else
			m_Cam.OnUpdate(ts, m_Input);

		if (!m_CLIProcessed) return;

		// Skip rendering + autosave while background work is active or the
		// GPU sync phase is running — the worker thread may be parsing files
		// or the GPU may be uploading textures / building AS. The loading
		// modal (drawn in OnUIRender) drives the GPU sync and keeps the UI
		// responsive.
		if (IsBackgroundBusy() || m_GpuSyncPending)
		{
			static int skipCount = 0;
			if (skipCount++ % 60 == 0) // print once per second
			{
				printf("[OnUpdate] skipping Render (bgWork=%d gpuSync=%d) frame=%d\n",
				       IsBackgroundBusy() ? 1 : 0, m_GpuSyncPending ? 1 : 0, skipCount);
				fflush(stdout);
			}
			return;
		}

		// Drive the runtime controller when Playing.
		if (m_Runtime.GetState() == rt2::core::SceneRunState::Playing && m_RenderBridge)
		{
			m_Runtime.Update(ts, *m_RenderBridge);
		}

		// ---- Phase 1B: autosave (authoring only, never runtime) ----
		// Only snapshot the authoring document; the runtime Play clone is
		// never captured. Skips work entirely on clean frames or when the
		// revision has not advanced since the last snapshot.
		if (m_Recovery && m_Runtime.GetState() == rt2::core::SceneRunState::Edit &&
			!m_ScriptRepairGate.SuppressAutosave())
		{
			rt2::core::Error ae;
			const auto started = std::chrono::steady_clock::now();
			const bool wrote = m_Recovery->MaybeSnapshot(
				m_SceneMgr.AuthoringDoc(), m_SceneMgr.AuthoringRevision(),
				m_UntitledRecoveryId, UntitledAssetRoot(), ae);
			const double elapsedMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - started).count();
			if (wrote)
			{
				char status[128];
				std::snprintf(status, sizeof(status), "Autosaved in %.2f ms", elapsedMs);
				m_LastStatusMsg = status;
				printf("[Recovery] %s\n", status);
				if (elapsedMs > 10.0)
					printf("[Recovery] Warning: main-thread autosave exceeded 10 ms guardrail\n");
			}
			if (!ae.IsOk())
			{
				m_LastStatusMsg = std::string("Autosave failed: ") + ae.Format();
				printf("[Recovery] Autosave failed: %s\n", ae.Format().c_str());
			}
		}

		Render();
	}

private:
	rt2::core::TextureAssetLoadContext MakeExplicitTextureContext(
		const std::string& filepath) const
	{
		rt2::core::TextureAssetLoadContext context;
		context.resolvedOwnerPath =
			std::filesystem::absolute(std::filesystem::u8path(filepath)).
				lexically_normal();
		context.resolution.assetRoot =
			context.resolvedOwnerPath.parent_path();
		context.ownerModel.kind = AssetKind::Model;
		context.ownerModel.path =
			context.resolvedOwnerPath.filename().generic_string();
		context.identityMode =
			rt2::core::TextureIdentityMode::ExplicitImport;
		context.uuidProvider =
			m_SceneMgr.AuthoringDoc().GetUuidProvider();
		return context;
	}

	void LogAssetDiagnostics(
		const std::vector<rt2::core::AssetDiagnostic>& diagnostics,
		size_t base,
		const char* context) const
	{
		for (size_t i = base; i < diagnostics.size(); ++i)
		{
			const auto& diagnostic = diagnostics[i];
			const char* severity = "Unknown";
			switch (diagnostic.severity)
			{
			case rt2::core::AssetDiagnostic::Missing:
				severity = "Missing"; break;
			case rt2::core::AssetDiagnostic::Malformed:
				severity = "Malformed"; break;
			case rt2::core::AssetDiagnostic::Unresolved:
				severity = "Unresolved"; break;
			case rt2::core::AssetDiagnostic::Conflict:
				severity = "Conflict"; break;
			case rt2::core::AssetDiagnostic::Stale:
				severity = "Stale"; break;
			default:
				break;
			}
			printf("[%s] Asset %s: ref=\"%s\" entity=%s "
			       "sourceKey=\"%s\" detail=%s\n",
			       context, severity, diagnostic.refPath.c_str(),
			       diagnostic.entityUuid.ToString().c_str(),
			       diagnostic.sourceKey.c_str(),
			       diagnostic.detail.c_str());
		}
	}

	void LogScriptAssetDiagnostics(size_t base, const char* context) const
	{
		LogAssetDiagnostics(m_ScriptAssetDiagnostics, base, context);
	}

	void EnsureRenderBridge()
	{
		if (!m_RenderBridge)
			m_RenderBridge = new SceneRenderBridge(m_RendererGPU);
	}

	// Phase 6B/W0: install the script system into the runtime controller.
	//
	// Before this, ScriptSystem was never instantiated by the app at all —
	// 6A shipped test-only, so ScriptComponent-bearing entities were inert
	// in Play. The controller side was already fully wired (Play fires
	// OnSceneStart, the tick fires SyncScriptEnvironments/OnFixedUpdate/
	// OnUpdate); only the owner was missing.
	//
	// Lazy, idempotent, and mirrors EnsureRenderBridge. ScriptSystem takes
	// its UUID provider by reference, so construction is deferred until the
	// authoring document actually has one — hence unique_ptr rather than a
	// plain member. If the provider is still null we simply stay unwired and
	// retry on the next Play; scripts are inert, nothing else is affected.
	void EnsureScriptRuntimeWired()
	{
		if (m_ScriptSystem) return;

		// Reuse the authoring document's provider, for the same reason
		// EnterPlay does: it is stateless and the UUID spaces are disjoint.
		rt2::core::IUuidProvider* provider =
			m_SceneMgr.AuthoringDoc().GetUuidProvider();
		if (!provider) return;

		m_ScriptSystem = std::make_unique<rt2::core::ScriptSystem>(
			*provider, m_ScriptAssetContext, m_ScriptAssetDiagnostics);
		m_ScriptSink   = std::make_unique<rt2::core::RuntimeCommandSink>(m_Runtime);

		m_Runtime.SetLifecycleObserver(m_ScriptSystem.get());
		m_Runtime.SetScriptDispatch(m_ScriptSystem.get());
		m_Runtime.SetRuntimeCommandSink(m_ScriptSink.get());
		m_Runtime.SetInputService(&m_Input);
	}

	bool ApplyEditorCameraPose(const EditorCameraPose& pose)
	{
		if (m_Runtime.GetState() != rt2::core::SceneRunState::Edit)
			return false;
		bool applied = false;
		if (m_RendererGPU.IsAvailable())
		{
			EnsureRenderBridge();
			applied = ApplyEditorCameraCut(pose, *m_RenderBridge,
				[this](const EditorCameraPose& normalized) {
					return m_Cam.SetEditorPose(normalized);
				});
		}
		else
		{
			applied = m_Cam.SetEditorPose(pose);
		}
		if (!applied)
		{
			m_LastStatusMsg = "Camera pose is invalid";
			return false;
		}
		return true;
	}

	bool FrameEditorSelection(bool moveToFit)
	{
		if (m_EditorUI.Selection().Empty())
		{
			m_LastStatusMsg = "Select an entity to frame or focus";
			return false;
		}
		EditorSelectionBounds bounds;
		if (!ComputeEditorSelectionBounds(m_SceneMgr.AuthoringDoc(),
			m_EditorUI.Selection().Ordered(), bounds))
		{
			m_LastStatusMsg = "Selection has no valid world-space bounds";
			return false;
		}
		EditorCameraPose result;
		const EditorCameraPose current = m_Cam.GetEditorPose();
		bool valid = false;
		if (moveToFit)
		{
			EditorFrameSettings settings;
			settings.viewportAspect = m_Cam.GetViewportAspect();
			settings.nearClip = m_Cam.GetNearClip();
			valid = TryFrameEditorCamera(current, bounds, settings, result);
		}
		else
		{
			valid = TryFocusEditorCamera(current, bounds, m_Cam.GetNearClip(), result);
		}
		if (!valid || !ApplyEditorCameraPose(result))
		{
			m_LastStatusMsg = moveToFit ? "Unable to frame selection" :
				"Unable to focus selection";
			return false;
		}
		m_LastStatusMsg = moveToFit ? "Framed selection" : "Focused selection";
		return true;
	}

	void HandleEditorCameraShortcuts()
	{
		// Phase 5: actions are read from m_Input. The state machine
		// already applied ImGui suppression (WantTextInput / AnyItemActive)
		// via ResolveUI, so suppressed keyboard actions read None here.
		if (m_Runtime.GetState() != rt2::core::SceneRunState::Edit)
			return;
		if (m_Input.IsPressed("focus"))
		{
			// Shift+F → focus_fit (frame); F → focus (focus point).
			// We distinguish via the focus_fit action which is mapped to
			// Shift+F. Check focus_fit first since both would fire on
			// Shift+F (focus is F without modifiers, but the state machine
			// checks modifier subsets — focus requires no modifiers, so
			// Shift+F does NOT fire focus; only focus_fit fires).
			if (m_Input.IsPressed("focus_fit"))
				FrameEditorSelection(false);
			else
				FrameEditorSelection(true);
			return;
		}
		// Camera bookmarks 1..9 (Ctrl+number).
		for (int slot = 0;
			slot < static_cast<int>(EditorSceneState::kCameraBookmarkCount); ++slot)
		{
			char name[32];
			std::snprintf(name, sizeof(name), "camera_bookmark_slot%d_shift", slot);
			if (m_Input.IsPressed(name))
			{
				m_EditorUI.CaptureCameraBookmark(slot, m_Cam.GetEditorPose());
				m_LastStatusMsg = "Stored camera bookmark " + std::to_string(slot + 1);
				return;
			}
			std::snprintf(name, sizeof(name), "camera_bookmark_slot%d", slot);
			if (m_Input.IsPressed(name))
			{
				if (const EditorCameraPose* bookmark = m_EditorUI.CameraBookmark(slot))
				{
					ApplyEditorCameraPose(*bookmark);
					m_LastStatusMsg = "Recalled camera bookmark " + std::to_string(slot + 1);
				}
				return;
			}
		}
	}

	// Phase 3A: Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y. Suppressed during text entry,
	// active widget editing, and Play (same suppression pattern as the
	// Phase 2D camera shortcuts). Routes through the editor UI's public
	// Undo()/Redo(), which run history and route results through the existing
	// private ApplyMutation() sync path.
	void HandleUndoRedoShortcuts()
	{
		if (m_Runtime.GetState() != rt2::core::SceneRunState::Edit) return;
		// Phase 5: read from m_Input. redo_shift_z is Ctrl+Shift+Z;
		// undo is Ctrl+Z; redo is Ctrl+Y. The state machine's modifier
		// matching ensures undo (Ctrl only) does not fire on Ctrl+Shift+Z.
		if (m_Input.IsPressed("redo_shift_z"))
			m_EditorUI.Redo();
		else if (m_Input.IsPressed("undo"))
			m_EditorUI.Undo();
		else if (m_Input.IsPressed("redo"))
			m_EditorUI.Redo();
	}

	void ViewThroughCamera(const rt2::core::UUID& camera)
	{
		EditorCameraPose pose;
		if (!TryGetCameraEntityPose(m_SceneMgr.AuthoringDoc(), camera,
			m_Cam.GetEditorPose(), pose) || !ApplyEditorCameraPose(pose))
		{
			m_LastStatusMsg = "Unable to view through selected camera";
			return;
		}
		m_LastStatusMsg = "Editor view aligned to camera entity";
	}

	void AlignCameraToView(const rt2::core::UUID& camera)
	{
		// Phase 3B2: capture the before-state, apply the alignment, capture
		// the after-state, and record via RecordApplied. Redo re-applies the
		// stored after-state (NOT re-align to current view). Undo restores
		// the before-localTRS + before-cameraProps via the atomic
		// SetCameraPoseState API (one revision bump, one authoritative
		// Transform impact). Routing goes through ApplyMutation/router so
		// the accumulation reset fires exactly once.
		const auto entity = m_SceneMgr.FindEntityByUuid(camera);
		if (entity == entt::null) return;
		EditableTRS beforeLocal;
		if (!m_SceneMgr.GetLocalTransform(SceneManager::EntityId{ entity }, beforeLocal))
		{
			m_LastStatusMsg = "Camera entity has no transform";
			return;
		}
		auto& reg = m_SceneMgr.GetECS().registry;
		auto* beforeCam = reg.try_get<CameraComponent>(entity);
		if (!beforeCam)
		{
			m_LastStatusMsg = "Selected entity is not a camera";
			return;
		}
		const CameraComponent beforeCamera = *beforeCam;

		const auto result = m_SceneMgr.AlignCameraEntityToView(camera,
			m_Cam.GetEditorPose());
		if (!result.success)
		{
			m_LastStatusMsg = result.error.Format();
			return;
		}

		// Capture the composite after-state.
		EditableTRS afterLocal;
		if (!m_SceneMgr.GetLocalTransform(SceneManager::EntityId{ entity }, afterLocal))
		{
			m_LastStatusMsg = "Camera alignment produced an unreadable transform";
			return;
		}
		const CameraComponent afterCamera = *reg.try_get<CameraComponent>(entity);

		auto cmd = MakeAlignCameraCommandIfEffective(camera, beforeLocal, afterLocal,
			beforeCamera, afterCamera);
		if (cmd)
		{
			EditorMutationResult applied;
			applied.success = true;
			applied.syncImpact = rt2::core::SyncImpact::Transform;
			applied.affectedEntities.push_back(camera);
			m_History.RecordApplied(std::move(cmd), m_SceneMgr, applied);
		}
		// Route through the router so accumulation reset fires once.
		m_SyncRouter.Route(result, m_SceneMgr);
		m_LastStatusMsg = "Camera entity aligned to editor view";
	}

	void SyncAuthoringTransforms()
	{
		m_RendererGPU.CancelPicks();
		if (m_RendererGPU.IsAvailable())
		{
			m_SceneMgr.SetInstanceSyncCallback(
				[this](GPUSceneData& gpuData, const RenderInstanceMap& instanceMap) {
					m_RendererGPU.UpdateSceneInstances(gpuData, instanceMap);
				});
			m_SceneMgr.SyncTransformsToGPU();
		}
		m_RendererGPU.ResetAccumulation();
	}

	// Phase 3B1 invariant: compaction cannot run while any Undo or Redo
	// entry references resource slots. This wrapper asserts the history is
	// empty in debug builds (catches host-contract violations) and forwards
	// to SceneManager::CompactMeshRegistryNow(). Call only at
	// history.Clear(), document adoption, or save/reload.
	void CompactMeshRegistryNowAsserted()
	{
		assert((!m_History.CanUndo() && !m_History.CanRedo()) &&
		       "CompactMeshRegistryNow must not run while history is non-empty");
		m_SceneMgr.CompactMeshRegistryNow();
	}


	void ProcessCLIArgs()
	{
		if (g_CLI.verbose)
			g_CLI.Print();

		if (g_CLI.listScenes)
		{
			printf("[CLI] --list mode: would load scene='%s' env='%s'\n",
			       g_CLI.scenePath.c_str(), g_CLI.envMapPath.c_str());
			Walnut::Application::Get().Close();
			return;
		}

		if (g_CLI.spp > 0)
			m_Settings.spp = g_CLI.spp;
		if (g_CLI.bounces > 0)
			m_Settings.maxBounces = g_CLI.bounces;
		if (g_CLI.nrd)
			m_Settings.nrdEnabled = true;
		if (g_CLI.nrdMaxAccumFrames > 0)
			m_Settings.nrdMaxAccumFrames = g_CLI.nrdMaxAccumFrames;
		if (g_CLI.nrdResponsiveRoughness >= 0.0f)
			m_Settings.nrdResponsiveRoughnessThreshold = g_CLI.nrdResponsiveRoughness;
		if (g_CLI.nrdResponsiveMinFrames >= 0)
			m_Settings.nrdResponsiveMinAccumFrames = g_CLI.nrdResponsiveMinFrames;
		if (g_CLI.rasterFirst)
		{
			m_Settings.rasterFirst = true;
			m_Cam.m_Aperture = 0.0f;
		}
		if (g_CLI.gbufferDebug >= 0)
			m_Settings.gbufferDebugMode = g_CLI.gbufferDebug;
		m_Settings.sceneSeed = g_CLI.sceneSeed;

		if (Walnut::Application::IsRayTracingSupported() && !m_RendererGPU.IsAvailable())
		{
			if (m_RendererGPU.Init())
			{
				m_Settings = m_RendererGPU.GetSettings();
				if (g_CLI.spp > 0) m_Settings.spp = g_CLI.spp;
				if (g_CLI.bounces > 0) m_Settings.maxBounces = g_CLI.bounces;
				if (g_CLI.nrd) m_Settings.nrdEnabled = true;
				if (g_CLI.nrdMaxAccumFrames > 0) m_Settings.nrdMaxAccumFrames = g_CLI.nrdMaxAccumFrames;
				if (g_CLI.nrdResponsiveRoughness >= 0.0f) m_Settings.nrdResponsiveRoughnessThreshold = g_CLI.nrdResponsiveRoughness;
				if (g_CLI.nrdResponsiveMinFrames >= 0) m_Settings.nrdResponsiveMinAccumFrames = g_CLI.nrdResponsiveMinFrames;
				if (g_CLI.noAccumulate) m_Settings.accumulate = false;
				if (g_CLI.rasterFirst) m_Settings.rasterFirst = true;
				if (g_CLI.ris || g_CLI.restir) { m_Settings.restirEnabled = true; m_Settings.rasterFirst = true; }
				if (g_CLI.restirCandidates > 0) m_Settings.restirFreshCandidates = (uint32_t)g_CLI.restirCandidates;
				if (g_CLI.restirNoTemporal) m_Settings.restirTemporalReuse = false;
				if (g_CLI.restirNoSpatial) m_Settings.restirSpatialReuse = false;
				if (g_CLI.restirGI) { m_Settings.restirGIEnabled = true; m_Settings.rasterFirst = true; }
				if (g_CLI.restirGICandidates > 0) m_Settings.restirGIFreshCandidates = (uint32_t)g_CLI.restirGICandidates;
				if (g_CLI.gbufferDebug >= 0) m_Settings.gbufferDebugMode = g_CLI.gbufferDebug;
				m_Settings.sceneSeed = g_CLI.sceneSeed;
				m_RendererGPU.ApplySettings(m_Settings);
			}
			else
			{
				RT_LOG("[CLI] GPU renderer init failed");
			}
		}

		if (g_CLI.hasEnvMap())
			LoadEnvMap(g_CLI.envMapPath);

		if (g_CLI.hasScene())
			LoadSceneInternal(g_CLI.scenePath);

		// Headless: wait for any async loads to complete before proceeding.
		if (g_CLI.headless)
			WaitForBackgroundWork();

		if (g_CLI.hasCameraPosition)
			m_Cam.SetPosition(glm::vec3(g_CLI.cameraPosition[0], g_CLI.cameraPosition[1], g_CLI.cameraPosition[2]));
		if (g_CLI.hasCameraForward)
		{
			glm::vec3 forward(g_CLI.cameraForward[0], g_CLI.cameraForward[1], g_CLI.cameraForward[2]);
			if (glm::dot(forward, forward) > 1e-8f)
				m_Cam.SetForwardDirection(glm::normalize(forward));
		}

		if (g_CLI.rasterFirst)
			m_Cam.m_Aperture = 0.0f;

		printf("[CLI] LoadScene done, headless=%d\n", g_CLI.headless ? 1 : 0);
		fflush(stdout);

		if (g_CLI.headless)
			RunHeadless();
	}

	void RunHeadless()
	{
		printf("[Headless] starting: %d frames at %dx%d\n", g_CLI.frames, g_CLI.width, g_CLI.height);
		fflush(stdout);

		m_ViewportWidth = (uint32_t)g_CLI.width;
		m_ViewportHeight = (uint32_t)g_CLI.height;
		if (m_RendererGPU.IsAvailable())
		{
			printf("[Headless] OnResize %dx%d...\n", m_ViewportWidth, m_ViewportHeight);
			fflush(stdout);
			m_RendererGPU.OnResize(m_ViewportWidth, m_ViewportHeight);
			m_Cam.OnResize(m_ViewportWidth, m_ViewportHeight);
		}

		if (m_RendererGPU.IsAvailable())
		{
			printf("[Headless] waiting for texture upload...\n");
			fflush(stdout);
			while (m_RendererGPU.IsTextureUploadPending())
			{
				m_RendererGPU.PollTextureUpload();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			printf("[Headless] texture upload done\n");
			fflush(stdout);
		}

		printf("[Headless] rendering %d frames...\n", g_CLI.frames);
		fflush(stdout);

		auto saveOutput = [&](const std::string& path) -> bool {
			std::error_code directoryError;
			const std::filesystem::path outputPath(path);
			if (outputPath.has_parent_path())
				std::filesystem::create_directories(outputPath.parent_path(), directoryError);
			if (directoryError)
			{
				RT_LOG("[Headless] failed to create output directory for %s: %s", path.c_str(), directoryError.message().c_str());
				return false;
			}
			std::vector<uint8_t> pixels;
			uint32_t w, h;
			if (!m_RendererGPU.ReadbackOutput(pixels, w, h))
			{
				RT_LOG("[Headless] ReadbackOutput failed");
				return false;
			}

			std::vector<uint8_t> flipped(pixels.size());
			for (uint32_t y = 0; y < h; y++)
			{
				memcpy(&flipped[(size_t)(h - 1 - y) * w * 4],
				       &pixels[(size_t)y * w * 4],
				       (size_t)w * 4);
			}

			if (!stbi_write_png(path.c_str(), w, h, 4, flipped.data(), w * 4))
			{
				RT_LOG("[Headless] stbi_write_png failed for %s", path.c_str());
				return false;
			}

			printf("[Headless] saved screenshot: %s (%ux%u)\n", path.c_str(), w, h);
			return true;
		};

		auto saveHDROutput = [&](const std::string& path) -> bool {
			std::error_code directoryError;
			const std::filesystem::path outputPath(path);
			if (outputPath.has_parent_path())
				std::filesystem::create_directories(outputPath.parent_path(), directoryError);
			if (directoryError)
			{
				RT_LOG("[Headless] failed to create output directory for %s: %s", path.c_str(), directoryError.message().c_str());
				return false;
			}
			std::vector<float> pixels;
			uint32_t w = 0, h = 0;
			if (!m_RendererGPU.ReadbackOutputLinear(pixels, w, h))
			{
				RT_LOG("[Headless] ReadbackOutputLinear failed");
				return false;
			}

			// Vulkan readback is bottom-up for the application's presentation convention.
			// Store conventional top-down scanlines while dropping the unused alpha channel.
			std::vector<float> rgb((size_t)w * h * 3);
			for (uint32_t y = 0; y < h; y++)
			{
				const uint32_t sourceY = h - 1 - y;
				for (uint32_t x = 0; x < w; x++)
				{
					const size_t src = ((size_t)sourceY * w + x) * 4;
					const size_t dst = ((size_t)y * w + x) * 3;
					rgb[dst + 0] = pixels[src + 0];
					rgb[dst + 1] = pixels[src + 1];
					rgb[dst + 2] = pixels[src + 2];
				}
			}

			std::string extension;
			size_t dot = path.find_last_of('.');
			if (dot != std::string::npos)
				extension = path.substr(dot);
			std::transform(extension.begin(), extension.end(), extension.begin(),
			               [](unsigned char c) { return (char)std::tolower(c); });

			if (extension == ".exr")
			{
				const char* error = nullptr;
				int result = SaveEXR(rgb.data(), (int)w, (int)h, 3, 0, path.c_str(), &error);
				if (result != TINYEXR_SUCCESS)
				{
					RT_LOG("[Headless] SaveEXR failed for %s: %s", path.c_str(), error ? error : "unknown error");
					if (error) FreeEXRErrorMessage(error);
					return false;
				}
			}
			else if (extension == ".pfm")
			{
				std::ofstream output(path, std::ios::binary);
				if (!output)
				{
					RT_LOG("[Headless] failed to open PFM output %s", path.c_str());
					return false;
				}
				output << "PF\n" << w << " " << h << "\n-1.0\n";
				// PFM stores scanlines bottom-to-top. rgb is currently top-to-bottom.
				for (uint32_t y = 0; y < h; y++)
				{
					const uint32_t sourceY = h - 1 - y;
					output.write(reinterpret_cast<const char*>(&rgb[(size_t)sourceY * w * 3]),
					             (std::streamsize)((size_t)w * 3 * sizeof(float)));
				}
				if (!output)
				{
					RT_LOG("[Headless] failed while writing PFM output %s", path.c_str());
					return false;
				}
			}
			else
			{
				RT_LOG("[Headless] unsupported HDR output extension for %s (use .exr or .pfm)", path.c_str());
				return false;
			}

			printf("[Headless] saved linear HDR: %s (%ux%u)\n", path.c_str(), w, h);
			return true;
		};

		const glm::vec3 sweepBasePosition = m_Cam.GetPosition();
		const glm::vec3 sweepBaseForward = m_Cam.GetDirection();
		glm::vec3 sweepRight = glm::cross(m_Cam.GetDirection(), glm::vec3(0.0f, 1.0f, 0.0f));
		if (glm::dot(sweepRight, sweepRight) > 1e-8f)
			sweepRight = glm::normalize(sweepRight);
		else
			sweepRight = glm::vec3(1.0f, 0.0f, 0.0f);

		auto sequencePath = [&](const std::string& basePath, const char* tag, int frame) {
			std::string path = basePath;
			size_t dot = path.find_last_of('.');
			if (dot == std::string::npos)
				dot = path.size();
			char suffix[64];
			if (frame >= 0)
				std::snprintf(suffix, sizeof(suffix), "_%s_%04d", tag, frame);
			else
				std::snprintf(suffix, sizeof(suffix), "_%s", tag);
			path.insert(dot, suffix);
			return path;
		};

		std::vector<GpuTimestampProfiler::Timings> benchmarkTimings;
		uint64_t lastBenchmarkTimingFrame = UINT64_MAX;
		auto collectBenchmarkTiming = [&]() {
			if (!g_CLI.benchmarkTimings || !m_RendererGPU.IsAvailable() || !m_RendererGPU.HasGpuTimings())
				return;
			const auto& timings = m_RendererGPU.GetGpuTimings();
			if (timings.validMask == 0 || timings.frameIndex == lastBenchmarkTimingFrame)
				return;
			benchmarkTimings.push_back(timings);
			lastBenchmarkTimingFrame = timings.frameIndex;
		};

		for (int i = 0; i < g_CLI.frames; i++)
		{
			if (g_CLI.cameraSweepAmplitude > 0.0f && i >= g_CLI.cameraSweepWarmup)
			{
				const int motionFrame = i - g_CLI.cameraSweepWarmup;
				const bool sweepEnded = g_CLI.cameraSweepCycles > 0 &&
				                        motionFrame >= g_CLI.cameraSweepCycles * g_CLI.cameraSweepPeriod;
				if (sweepEnded)
				{
					m_Cam.SetPosition(sweepBasePosition);
					m_Cam.SetForwardDirection(sweepBaseForward);
				}
				else
				{
				float phase = float(motionFrame) / float(g_CLI.cameraSweepPeriod);
				float offset = -g_CLI.cameraSweepAmplitude * std::sin(phase * 6.28318530718f);
				if (g_CLI.cameraSweepMode == 1)
				{
					m_Cam.SetPosition(sweepBasePosition + sweepBaseForward * offset);
				}
				else if (g_CLI.cameraSweepMode == 2)
				{
					float c = std::cos(offset);
					float s = std::sin(offset);
					glm::vec3 yawed(c * sweepBaseForward.x + s * sweepBaseForward.z,
					                  sweepBaseForward.y,
					                 -s * sweepBaseForward.x + c * sweepBaseForward.z);
					m_Cam.SetForwardDirection(glm::normalize(yawed));
				}
				else
				{
					m_Cam.SetPosition(sweepBasePosition + sweepRight * offset);
				}
				}
			}
			Timer timer;
			if (m_RendererGPU.IsAvailable())
				m_RendererGPU.Render(m_Cam);
			collectBenchmarkTiming();
			float ms = timer.ElapsedMillis();
			if (g_CLI.verbose || i == g_CLI.frames - 1)
				printf("[Headless] frame %d/%d: %.1fms\n", i + 1, g_CLI.frames, ms);
			fflush(stdout);

			if (g_CLI.captureEvery > 0 && m_RendererGPU.IsAvailable())
			{
				bool stillFrame = g_CLI.cameraSweepWarmup > 0 && i == g_CLI.cameraSweepWarmup - 1;
				bool periodicFrame = i >= g_CLI.cameraSweepWarmup &&
				                     ((i - g_CLI.cameraSweepWarmup) % g_CLI.captureEvery == 0);
				const bool holdFrame = g_CLI.cameraSweepCycles > 0 &&
				                       i - g_CLI.cameraSweepWarmup >= g_CLI.cameraSweepCycles * g_CLI.cameraSweepPeriod;
				if (stillFrame)
				{
					if (!g_CLI.outputPath.empty()) saveOutput(sequencePath(g_CLI.outputPath, "still", -1));
					if (!g_CLI.outputHDRPath.empty()) saveHDROutput(sequencePath(g_CLI.outputHDRPath, "still", -1));
				}
				else if (periodicFrame)
				{
					const char* tag = holdFrame ? "hold" : "move";
					if (!g_CLI.outputPath.empty()) saveOutput(sequencePath(g_CLI.outputPath, tag, i + 1));
					if (!g_CLI.outputHDRPath.empty()) saveHDROutput(sequencePath(g_CLI.outputHDRPath, tag, i + 1));
				}
			}
		}

		if (m_RendererGPU.IsAvailable() && m_RendererGPU.HasGpuTimings())
		{
			m_RendererGPU.FlushGpuTimings();
			collectBenchmarkTiming();
			const auto& timings = m_RendererGPU.GetGpuTimings();
			printf("[Headless] GPU timings (frame %llu)\n",
			       static_cast<unsigned long long>(timings.frameIndex));
			for (uint32_t i = 0; i < static_cast<uint32_t>(GpuTimestampProfiler::Region::Count); i++)
			{
				if ((timings.validMask & (1u << i)) == 0)
					continue;
				printf("[Headless]   %s: %.3f ms\n",
				       GpuTimestampProfiler::RegionName(static_cast<GpuTimestampProfiler::Region>(i)),
				       timings.milliseconds[i]);
			}
			fflush(stdout);
		}

		if (g_CLI.benchmarkTimings)
		{
			for (const auto& timings : benchmarkTimings)
			{
				printf("[HeadlessTiming] {\"frame\":%llu,\"regions_ms\":{",
				       static_cast<unsigned long long>(timings.frameIndex));
				bool first = true;
				for (uint32_t i = 0; i < static_cast<uint32_t>(GpuTimestampProfiler::Region::Count); i++)
				{
					if ((timings.validMask & (1u << i)) == 0)
						continue;
					if (!first) printf(",");
					first = false;
					printf("\"%s\":%.6f",
					       GpuTimestampProfiler::RegionName(static_cast<GpuTimestampProfiler::Region>(i)),
					       timings.milliseconds[i]);
				}
				printf("}}\n");
			}
			fflush(stdout);
		}

		if (!g_CLI.outputPath.empty() && m_RendererGPU.IsAvailable())
			saveOutput(g_CLI.outputPath);
		if (!g_CLI.outputHDRPath.empty() && m_RendererGPU.IsAvailable())
			saveHDROutput(g_CLI.outputHDRPath);

		printf("[Headless] done, exiting\n");
		Walnut::Application::Get().Close();
	}

	void Render() {
		Timer timer;

		if (m_RendererGPU.IsAvailable())
			m_RendererGPU.Render(m_RuntimeCamActive ? m_RuntimeCam : m_Cam);

		m_LastRenderTime = timer.ElapsedMillis();

		float alpha = m_LastRenderTime / (m_LastRenderTime + 500.0f);
		m_SmoothedFrameTime = m_SmoothedFrameTime * (1.0f - alpha) + m_LastRenderTime * alpha;
		m_SmoothedFPS = m_SmoothedFrameTime > 0.0f ? 1000.0f / m_SmoothedFrameTime : 0.0f;
	}

	void UploadMeshToGPU()
	{
		if (m_SceneMgr.GetECS().meshRegistry.GetCount() == 0) return;

		m_SceneMgr.SetSyncCallback([this](GPUSceneData& gpuData, const RenderInstanceMap& instanceMap) {
			m_RendererGPU.SetScene(gpuData, instanceMap);
		});
		m_SceneMgr.SyncToGPU();
	}

	void RequestOpenScene(const std::string& filepath)
	{
		rt2::core::UnsavedChangesCoordinator::PendingAction action;
		action.kind = rt2::core::UnsavedChangesCoordinator::ActionKind::Open;
		action.path = std::filesystem::u8path(filepath);
		m_Unsaved.Request(action);
	}

	void LoadSceneInternal(const std::string& filepath)
	{
		// Dispatch .rt2scene files to the native serializer.
		std::string ext = filepath.substr(filepath.find_last_of('.') + 1);
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == "rt2scene")
		{
			OpenRt2SceneInternal(filepath);
			return;
		}

		if (IsBackgroundBusy()) return;

		const std::string pathCopy = filepath;
		const bool isObj = (ext == "obj");

		struct LoadResult
		{
			ECSScene ecs;
			bool ok = false;
			std::vector<rt2::core::AssetDiagnostic> diagnostics;
		};
		auto result = std::make_shared<LoadResult>();
		auto textureContext = MakeExplicitTextureContext(pathCopy);

		StartBackgroundWork(isObj ? "Loading OBJ scene..." : "Loading glTF scene...",
			[result, pathCopy, isObj,
			 textureContext](BackgroundWork& self) mutable -> bool
		{
			self.SetStatus("Parsing file...");
			if (isObj)
				result->ok = SceneLoader::LoadObjIntoECS(
					result->ecs, pathCopy, textureContext,
					result->diagnostics);
			else
				result->ok = SceneLoader::LoadIntoECS(
					result->ecs, pathCopy, textureContext,
					result->diagnostics);
			return result->ok;
		},
			[this, result, pathCopy, isObj, ext](bool success)
		{
			LogAssetDiagnostics(result->diagnostics, 0, "LoadScene");
			if (!success)
			{
				ImGui::OpenPopup("Scene Load Failed");
				m_LastStatusMsg = "Scene load failed";
				return;
			}

			// Clear the live scene and adopt the loaded ECS.
			m_SceneMgr.Clear();
			// Move the loaded ECS resources into the live scene.
			auto& live = m_SceneMgr.GetECS();
			live.meshRegistry = std::move(result->ecs.meshRegistry);
			live.materials = std::move(result->ecs.materials);
			live.textures = std::move(result->ecs.textures);
			live.lights = std::move(result->ecs.lights);
			live.camera = std::move(result->ecs.camera);
			// Move the registry: entt registries are movable.
			live.registry = std::move(result->ecs.registry);

			// Assign UUIDs to all entities.
			auto& reg = live.registry;
			auto view = reg.view<Transform>();
			for (auto entity : view)
			{
				if (!reg.all_of<EntityIdComponent>(entity))
					m_SceneMgr.AuthoringDoc().AssignNewUuid(entity);
			}

			// Record source paths on imported entities.
			{
				auto mv = reg.view<ImportedMeshSourceComponent>();
				for (auto e : mv)
				{
					auto& src = mv.get<ImportedMeshSourceComponent>(e);
					if (src.model.path.empty())
						src.model.path = pathCopy;
				}
			}

			m_EditorUI.ResetForDocument();
			if (m_InspectorFieldRegistry) m_InspectorFieldRegistry->Clear();
			m_History.Clear();
			CompactMeshRegistryNowAsserted();

			if (isObj)
			{
				m_SceneMgr.SetSyncCallback([this](GPUSceneData& gpuData, const RenderInstanceMap& instanceMap) {
					m_RendererGPU.SetScene(gpuData, instanceMap);
				});
				m_SceneMgr.SyncToGPU();
				m_RendererGPU.ResetAccumulation();
				m_GpuSyncPending = true;
			}
			else
			{
				if (m_RendererGPU.IsAvailable())
				{
					UploadMeshToGPU();
					m_GpuSyncPending = true;
				}
			}

			const auto& cam = m_SceneMgr.GetECS().camera;
			m_Cam.SetPosition(cam.position);
			m_Cam.SetForwardDirection(cam.forwardDirection);

			// Imported interchange files become an untitled native authoring
			// document. They must be explicitly saved as .rt2scene.
			m_SceneMgr.AuthoringDoc().metadata.sourcePath.clear();
			m_SceneMgr.MarkDirty();
			m_ScriptRepairGate.OnPersistedOrReset();
			m_UntitledRecoveryId = rt2::core::OsUuidProvider{}.CreateV4().ToString();
			m_Recovery->ResetSchedule();
			m_LastStatusMsg = "Imported scene (unsaved)";
		});
	}

	SceneManager::EntityId LoadMeshFileAsEntity(const std::string& filepath)
	{
		std::string ext = filepath.substr(filepath.find_last_of('.') + 1);
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == "obj")
		{
			// OBJ now imports into the current scene (consistent with glTF).
			ImportSettings settings;
			settings.mergeMegaMesh = true;
			std::vector<rt2::core::AssetDiagnostic> diagnostics;
			auto id = m_SceneMgr.ImportObj(
				filepath, settings, &diagnostics);
			LogAssetDiagnostics(diagnostics, 0, "Import");
			if (id.IsValid())
				m_PendingFullSync = true;
			return id;
		}

		std::vector<rt2::core::AssetDiagnostic> diagnostics;
		auto textureContext = MakeExplicitTextureContext(filepath);
		if (!SceneLoader::LoadIntoECS(
			    m_SceneMgr.GetECS(), filepath, textureContext,
			    diagnostics))
		{
			LogAssetDiagnostics(diagnostics, 0, "LoadMesh");
			printf("[SceneEditor] Failed to load mesh: %s\n", filepath.c_str());
			return SceneManager::EntityId{};
		}
		LogAssetDiagnostics(diagnostics, 0, "LoadMesh");

		std::string name = filepath;
		size_t lastSlash = name.find_last_of("/\\");
		if (lastSlash != std::string::npos)
			name = name.substr(lastSlash + 1);
		size_t lastDot = name.find_last_of('.');
		if (lastDot != std::string::npos)
			name = name.substr(0, lastDot);

		return m_SceneMgr.AddObject(name, {0, 0.5f, 0});
	}

	RendererGPU m_RendererGPU;
	RenderSettings m_Settings;
	SceneManager m_SceneMgr;
	SceneEditorUI m_EditorUI;
	EditorCommandHistory m_History;
	EditorSyncRouter m_SyncRouter;
	EditorTransformGizmo m_TransformGizmo;
	uint64_t m_ViewportPickSerial = 0;
	bool m_ViewportPickToggle = false;
	uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;
	float m_LastRenderTime = 0.0f;
	float m_SmoothedFrameTime = 0.0f;
	float m_SmoothedFPS = 0.0f;
	bool m_PendingFullSync = false;
	Camera m_Cam;
	bool m_CLIProcessed = false;

	// Runtime lifecycle
	SceneRenderBridge* m_RenderBridge = nullptr;
	rt2::core::RuntimeSceneController m_Runtime;
	Camera m_RuntimeCam;           // separate camera for Play mode
	Camera m_EditorCamSnapshot;    // saved on Play, restored on Stop
	bool m_RuntimeCamActive = false;

	// Phase 6B/W0: the app's script system + runtime command sink. Declared
	// after m_Runtime because the sink binds to the controller by reference.
	// Installed lazily by EnsureScriptRuntimeWired(); null until the first
	// Play with a valid UUID provider.
	rt2::core::AssetResolutionContext               m_ScriptAssetContext;
	std::vector<rt2::core::AssetDiagnostic>          m_ScriptAssetDiagnostics;
	std::unique_ptr<rt2::core::ScriptSystem>         m_ScriptSystem;
	std::unique_ptr<rt2::core::RuntimeCommandSink>   m_ScriptSink;

	// Phase 6B/W5: inspector-side field registry. Created at startup so the
	// inspector can query declared fields while the editor is STOPPED (the
	// ScriptSystem's registry is lazy-created at Play). Cleared on scene
	// load/close alongside ResetForDocument.
	std::unique_ptr<rt2::core::ScriptFieldRegistry> m_InspectorFieldRegistry;

	// Phase 6C/W2: file watcher for hot reload. efsw watches directories
	// containing referenced script assets. On .lua file change, the path
	// is posted to m_PendingFileChanges (thread-safe). The drain in
	// OnUIRender debounces (~100ms) and calls ScriptSystem::ReloadScript.
	class ScriptFileWatchListener : public efsw::FileWatchListener
	{
	public:
		std::mutex mutex;
		std::vector<std::string> pendingChanges;

		void handleFileAction(efsw::WatchID watchid, const std::string& dir,
		                      const std::string& filename, efsw::Action action,
		                      const std::string& oldFilename) override
		{
			(void)watchid; (void)oldFilename;
			// Only react to .lua file modifications and adds (atomic save =
			// delete + add, so catch both). Delete alone means the file is
			// gone — no reload needed.
			if (action == efsw::Actions::Delete) return;

			// Only .lua files (case-insensitive on Windows).
			if (filename.size() < 5) return;
			auto ext = filename.substr(filename.size() - 4);
			std::transform(ext.begin(), ext.end(), ext.begin(),
				[](unsigned char c) { return std::tolower(c); });
			if (ext != ".lua") return;

			// Build the full path. efsw gives dir + filename separately;
			// normalize to a single path.
			std::filesystem::path full = std::filesystem::path(dir) / filename;
			std::lock_guard<std::mutex> lock(mutex);
			pendingChanges.push_back(full.string());
		}
	};

	// Declaration order matters: m_FileWatchListener must outlive
	// m_FileWatcher so that efsw's watch thread (stopped by
	// ~FileWatcher) never calls handleFileAction on a destroyed
	// listener. Members destroy in reverse declaration order, so
	// the watcher is declared second and dies first.
	std::unique_ptr<ScriptFileWatchListener>    m_FileWatchListener;
	std::unique_ptr<efsw::FileWatcher>          m_FileWatcher;
	std::vector<efsw::WatchID>                  m_ActiveWatchIds;
	std::vector<std::string>                    m_DebouncedChanges;
	std::chrono::steady_clock::time_point       m_LastFileChangeTime;

	// Phase 5 input service — owns the context stack and frame phasing.
	rt2::core::InputService m_Input;
	bool m_InputDefaultsLoaded = false;
	bool m_ViewportHoveredThisFrame = false;
	bool m_GizmoConsumesMouseThisFrame = false;

	// ---- Background async work (scene load / import / env map decode) ----
	// Only one BackgroundWork may be active at a time. The loading modal
	// is modal — it blocks all other UI interactions until the work done.
	// The completion callback runs on the main thread after the worker
	// thread joins, so Vulkan/GPU calls are safe there.
	std::unique_ptr<BackgroundWork> m_BackgroundWork;
	std::function<void(bool)> m_OnBackgroundComplete;
	bool m_LoadingModalOpen = false;
	// GPU sync phase: after the async worker completes, the completion
	// callback may set this to keep the modal open while the GPU uploads
	// textures + builds acceleration structures. The modal polls each
	// frame and runs the GPU sync, then clears the flag.
	bool m_GpuSyncPending = false;
	std::string m_GpuSyncStatus;

	// ---- Phase 1B: editor settings, recovery, unsaved-changes coordinator ----
	std::filesystem::path DialogInitialDirectory() const
	{
		const auto& source = m_SceneMgr.AuthoringDoc().metadata.sourcePath;
		if (!source.empty()) return source.parent_path();
		if (m_Settings2 && !m_Settings2->GetProjectRoot().empty())
			return m_Settings2->GetProjectRoot();
		return {};
	}

	std::filesystem::path UntitledAssetRoot() const
	{
		if (m_Settings2 && !m_Settings2->GetProjectRoot().empty())
			return m_Settings2->GetProjectRoot();
		std::error_code ec;
		auto cwd = std::filesystem::current_path(ec);
		return ec ? std::filesystem::path{} : cwd;
	}

	bool PersistEditorSettings(const char* context)
	{
		if (!m_Settings2) return true;
		rt2::core::Error err;
		if (m_Settings2->Save(err)) return true;
		m_LastStatusMsg = std::string("Settings save failed (") + context + "): " + err.Format();
		printf("[Settings] %s\n", m_LastStatusMsg.c_str());
		return false;
	}

	std::filesystem::path AppDataRoot() const
	{
		// Production: %LOCALAPPDATA%\RT2\Editor. Tests would override this
		// by constructing the services with a temp dir, but WalnutApp uses
		// the production path.
	#ifdef _WIN32
		const wchar_t* la = _wgetenv(L"LOCALAPPDATA");
		if (la) return std::filesystem::path(la) / L"RT2" / L"Editor";
	#else
		const char* la = std::getenv("LOCALAPPDATA");
		if (la) return std::filesystem::path(la) / "RT2" / "Editor";
	#endif
		return std::filesystem::current_path() / "RT2Editor";
	}

	// ---- View config persistence (window visibility + perf detail) ----
	// Saved to <AppDataRoot>/view_config.txt as plain key=value lines.
	// Loaded once at construction; saved when a flag changes (dirty bit).
	std::filesystem::path ViewConfigPath() const
	{
		return AppDataRoot() / "view_config.txt";
	}

	void LoadViewConfig()
	{
		std::ifstream in(ViewConfigPath());
		if (!in) return;
		std::string line;
		while (std::getline(in, line))
		{
			auto eq = line.find('=');
			if (eq == std::string::npos) continue;
			std::string key = line.substr(0, eq);
			std::string val = line.substr(eq + 1);
			auto getBool = [&](bool& out) {
				if (val == "1" || val == "true") out = true;
				else if (val == "0" || val == "false") out = false;
			};
			if (key == "perfDetail")      m_PerfDetailLevel      = std::atoi(val.c_str());
			else if (key == "showCamera")        getBool(m_ShowInfoWindow);
			else if (key == "showPerformance")   getBool(m_ShowPerfWindow);
			else if (key == "showRenderSettings")getBool(m_ShowRenderSettingsWin);
			else if (key == "showScene")         getBool(m_ShowSceneWindow);
			else if (key == "showSession")       getBool(m_ShowSessionWindow);
			else if (key == "showInspector")     getBool(m_ShowInspectorWindow);
			else if (key == "showOutliner")      getBool(m_ShowHierarchyWindow);
		}
	}

	void SaveViewConfig()
	{
		auto path = ViewConfigPath();
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		std::ofstream out(path, std::ios::trunc);
		if (!out) return;
		out << "perfDetail=" << m_PerfDetailLevel << "\n";
		out << "showCamera=" << (m_ShowInfoWindow ? 1 : 0) << "\n";
		out << "showPerformance=" << (m_ShowPerfWindow ? 1 : 0) << "\n";
		out << "showRenderSettings=" << (m_ShowRenderSettingsWin ? 1 : 0) << "\n";
		out << "showScene=" << (m_ShowSceneWindow ? 1 : 0) << "\n";
		out << "showSession=" << (m_ShowSessionWindow ? 1 : 0) << "\n";
		out << "showInspector=" << (m_ShowInspectorWindow ? 1 : 0) << "\n";
		out << "showOutliner=" << (m_ShowHierarchyWindow ? 1 : 0) << "\n";
	}

	std::unique_ptr<rt2::core::EditorSettingsStore>      m_Settings2;
	std::unique_ptr<rt2::core::SceneRecoveryService>      m_Recovery;
	rt2::core::UnsavedChangesCoordinator                  m_Unsaved;
	std::vector<rt2::core::SceneRecoveryService::RecoveryRecord> m_PendingRecovery;
	size_t                                                m_RecoveryPromptIndex = 0;
	bool                                                  m_RecoveryPromptOpen = false;
	std::string                                           m_UntitledRecoveryId; // stable per session
	std::string                                           m_LastStatusMsg;
	rt2::core::ScriptRepairPersistenceGate                m_ScriptRepairGate;

	// ---- Runtime lifecycle ----

	void EnterPlay()
	{
		EnsureRenderBridge();
		EnsureScriptRuntimeWired();
		m_ScriptAssetDiagnostics.clear();
		const auto& sourcePath =
			m_SceneMgr.AuthoringDoc().metadata.sourcePath;
		m_ScriptAssetContext.assetRoot = sourcePath.empty()
			? UntitledAssetRoot()
			: sourcePath.parent_path();
		m_ScriptAssetContext.database = nullptr;

		// Phase 4: inject the production UUID provider so the runtime document
		// can generate fresh UUIDs for deferred-create operations. The
		// provider is stateless and the UUID spaces are disjoint (the runtime
		// document's uuidIndex is a fresh copy at Play), so we reuse the
		// authoring document's provider.
		m_Runtime.SetRuntimeUuidProvider(
			m_SceneMgr.AuthoringDoc().GetUuidProvider());

		rt2::core::Error err;
		if (!m_Runtime.Play(m_SceneMgr.AuthoringDoc(), *m_RenderBridge, err))
		{
			LogScriptAssetDiagnostics(0, "Play");
			printf("[Play] Failed to enter Play: %s\n", err.Format().c_str());
			return;
		}
		LogScriptAssetDiagnostics(0, "Play");

		// Snapshot the editor camera and switch to the runtime camera.
		m_EditorCamSnapshot = m_Cam;
		m_RuntimeCam = m_Cam;
		m_RuntimeCamActive = true;

		// Find the runtime scene's camera entity and adopt its pose.
		rt2::core::SceneDocument* rt = const_cast<rt2::core::SceneDocument*>(
			m_Runtime.TryGetRuntimeScene());
		if (rt)
		{
			const auto cameraEntity = FindDeterministicCameraEntity(*rt);
			if (!cameraEntity.IsNull())
			{
				EditorCameraPose runtimePose;
				if (TryGetCameraEntityPose(*rt, cameraEntity,
					m_RuntimeCam.GetEditorPose(), runtimePose))
					m_RuntimeCam.SetEditorPose(runtimePose);
			}
		}

		m_EditorUI.SetEditable(false);
		printf("[Play] Entered Play mode\n");
	}

	void EnterPause()
	{
		m_Runtime.Pause();
		printf("[Play] Paused\n");
	}

	void EnterStep()
	{
		if (!m_RenderBridge) return;
		m_Runtime.Step(*m_RenderBridge);
	}

	void EnterStop()
	{
		if (!m_RenderBridge) return;
		m_Runtime.Stop(m_SceneMgr.AuthoringDoc(), *m_RenderBridge);

		// Restore the editor camera.
		m_Cam = m_EditorCamSnapshot;
		m_RuntimeCamActive = false;

		m_EditorUI.SetEditable(true);
		printf("[Play] Stopped, editor scene restored\n");
	}

	// ---- Native .rt2scene file operations ----
	//
	// Public wrappers route through the unsaved-changes coordinator. The
	// *Internal functions perform the actual mutation and are called by
	// the coordinator's ExecuteGate once any pending prompt is resolved.

public:
	std::filesystem::path GetDialogInitialDirectory() const
	{
		return DialogInitialDirectory();
	}

	// Phase 3A: public undo/redo entry points for the Edit menu and Ctrl+Z/
	// Ctrl+Shift+Z/Ctrl+Y shortcuts. Each delegates to the editor UI, which
	// runs history and routes results through the existing ApplyMutation().
	bool CanUndo() const { return m_EditorUI.CanUndo(); }
	bool CanRedo() const { return m_EditorUI.CanRedo(); }
	std::string UndoDescription() const { return m_EditorUI.UndoDescription(); }
	std::string RedoDescription() const { return m_EditorUI.RedoDescription(); }
	void Undo() { m_EditorUI.Undo(); }
	void Redo() { m_EditorUI.Redo(); }

	// Performance window detail levels. These are ImGui::Combo indices, so
	// they are 0-based even though the labels read "1".."3".
	static constexpr int kPerfLevelBasic      = 0; // FPS + frame time only
	static constexpr int kPerfLevelPasses     = 1; // + raster/ReSTIR GPU regions
	static constexpr int kPerfLevelEverything = 2; // + all regions, res, AS builds

	// Regions shown at kPerfLevelPasses. Everything else is level-3 only, so
	// a region added to GpuTimestampProfiler::Region shows up at level 3
	// automatically without touching this list.
	static bool IsPassLevelRegion(GpuTimestampProfiler::Region region)
	{
		switch (region)
		{
		case GpuTimestampProfiler::Region::Raster:
		case GpuTimestampProfiler::Region::ReSTIRDITemporal:
		case GpuTimestampProfiler::Region::ReSTIRDISpatial:
		case GpuTimestampProfiler::Region::ReSTIRGITemporal:
		case GpuTimestampProfiler::Region::ReSTIRGIHistory:
			return true;
		default:
			return false;
		}
	}

	// View-menu window visibility + Performance detail level. Public so
	// the menubar callback (a free function holding a RT2Layer*) can toggle
	// them. Viewport is always shown and is not toggleable.
	int  m_PerfDetailLevel       = kPerfLevelBasic;
	bool m_ShowInfoWindow        = true;
	bool m_ShowPerfWindow        = true;
	bool m_ShowRenderSettingsWin  = true;
	bool m_ShowSceneWindow       = true;
	bool m_ShowSessionWindow     = true;
	bool m_ShowInspectorWindow   = true; // SceneEditorUI Inspector panel
	bool m_ShowHierarchyWindow   = true; // SceneEditorUI Outliner panel

	void NewScene()
	{
		m_Unsaved.Request({rt2::core::UnsavedChangesCoordinator::ActionKind::New, {}});
	}

	void NewSceneInternal()
	{
		m_SceneMgr.Clear();
			m_EditorUI.ResetForDocument();
			if (m_InspectorFieldRegistry) m_InspectorFieldRegistry->Clear();
			m_History.Clear();
		m_SceneMgr.CompactMeshRegistryNow();
		m_SceneMgr.ClearDirty();
		m_ScriptRepairGate.OnPersistedOrReset();
		m_Recovery->ResetSchedule();
		// New untitled doc gets a fresh recovery id for this session.
		m_UntitledRecoveryId = rt2::core::OsUuidProvider{}.CreateV4().ToString();

		// Push the now-empty scene to the GPU so the renderer drops all
		// geometry and instances from the previous scene.
		if (m_RendererGPU.IsAvailable())
		{
			m_SceneMgr.SetSyncCallback([this](GPUSceneData& gpuData, const RenderInstanceMap& instanceMap) {
				m_RendererGPU.SetScene(gpuData, instanceMap);
			});
			m_SceneMgr.SyncToGPU();
			m_RendererGPU.ResetAccumulation();
		}
		m_LastStatusMsg = "New scene";
	}

	void OpenRt2Scene(const std::string& filepath)
	{
		RequestOpenScene(filepath);
	}

	void OpenRt2SceneInternal(const std::string& filepath)
	{
		if (IsBackgroundBusy()) return;

		// The CPU-heavy work (JSON parse + asset re-import + texture decode)
		// runs on a worker thread. The result SceneDocument is captured in
		// a shared_ptr so the worker can fill it and the completion callback
		// (on the main thread) can adopt it + upload to GPU.
		auto resultDoc = std::make_shared<rt2::core::SceneDocument>();
		auto errorStr = std::make_shared<std::string>();
		auto diagStr = std::make_shared<std::string>();
		auto loadReport = std::make_shared<rt2::core::SceneLoadReport>();
		auto fieldDiagnostics =
			std::make_shared<std::vector<rt2::core::FieldDiagnostic>>();
		auto fieldResolution =
			std::make_shared<rt2::core::ScriptFieldResolutionResult>();
		auto fieldChanges =
			std::make_shared<rt2::core::ScriptFieldChangeClassification>();
		const std::string filepathCopy = filepath;
		auto uuidProvider = m_SceneMgr.AuthoringDoc().GetUuidProvider();

		StartBackgroundWork("Loading scene...",
			[resultDoc, errorStr, diagStr, loadReport, fieldDiagnostics,
			 fieldResolution, fieldChanges, filepathCopy, uuidProvider](BackgroundWork& self) -> bool
		{
			self.SetStatus("Parsing scene file...");
			resultDoc->SetUuidProvider(uuidProvider);

			rt2::core::Error err;
			if (!rt2::core::SceneSerializer::Load(
					*resultDoc, filepathCopy, *loadReport, err))
			{
				*errorStr = "Failed to load .rt2scene: " + err.Format();
				return false;
			}

			self.SetStatus("Resolving assets (models, textures, env)...");
			std::filesystem::path sceneRoot = std::filesystem::path(filepathCopy).parent_path();
			std::vector<rt2::core::AssetDiagnostic> diagnostics;
			const rt2::core::AssetResolutionContext assetContext{
				sceneRoot, nullptr};
			rt2::core::Error resolveErr;
			bool resolveOk = rt2::core::SceneAssetResolver::ResolveAll(
			        *resultDoc, sceneRoot, diagnostics, resolveErr);

			auto formatAssetDiagnostics = [&]() {
				for (const auto& d : diagnostics)
				{
					const char* sev = nullptr;
					switch (d.severity)
					{
					case rt2::core::AssetDiagnostic::Missing:
						sev = "Missing"; break;
					case rt2::core::AssetDiagnostic::Malformed:
						sev = "Malformed"; break;
					case rt2::core::AssetDiagnostic::Unresolved:
						sev = "Unresolved"; break;
					case rt2::core::AssetDiagnostic::Conflict:
						sev = "Conflict"; break;
					case rt2::core::AssetDiagnostic::Stale:
						sev = "Stale"; break;
					default:
						sev = "Unknown"; break;
					}
					*diagStr += std::string("[Scene] Asset ") + sev +
						": kind=" + std::to_string((int)d.kind) +
						" ref='" + d.refPath + "'" +
						" sourceKey='" + d.sourceKey + "'" +
						" detail=" + d.detail + "\n";
				}
			};

			if (!resolveOk)
			{
				formatAssetDiagnostics();
				*errorStr = "Asset resolution failed, keeping current scene: " + resolveErr.Format();
				return false;
			}

			self.SetStatus("Reconciling script fields...");
			*fieldDiagnostics = loadReport->fieldDiagnostics;
			rt2::core::ScriptFieldRegistry fieldRegistry;
			*fieldResolution = rt2::core::ScriptFieldResolver::ResolveDocument(
				*resultDoc, fieldRegistry, assetContext, diagnostics,
				*fieldDiagnostics);
			formatAssetDiagnostics();
			*fieldChanges = rt2::core::ClassifyScriptFieldChanges(
				*loadReport, *fieldResolution, *fieldDiagnostics);
			for (const auto& diagnostic : *fieldDiagnostics)
			{
				*diagStr += "[Scene] Script field: " + diagnostic.message + "\n";
			}

			return true;
		},
			[this, resultDoc, errorStr, diagStr, fieldChanges,
			 filepathCopy](bool success)
		{
			// Main thread: log diagnostics.
			if (!diagStr->empty())
				printf("%s", diagStr->c_str());

			if (!success)
			{
				printf("[Scene] %s\n", errorStr->c_str());
				ImGui::OpenPopup("Scene Load Failed");
				m_LastStatusMsg = *errorStr;
				return;
			}

			// Adopt the resolved document.
			printf("[OpenRt2Scene] adopting document...\n"); fflush(stdout);
			m_SceneMgr.ReplaceAuthoringDocument(std::move(*resultDoc));
			m_EditorUI.ResetForDocument();
			if (m_InspectorFieldRegistry) m_InspectorFieldRegistry->Clear();

			// Phase 6C/W2: watch directories containing referenced
			// script assets for .lua file changes. The scene's parent
			// directory covers scene-relative scripts; we also walk
			// every ScriptComponent to catch absolute paths or shared
			// script folders outside the scene tree. All watches are
			// recursive and deduped by directory path.
			if (m_FileWatcher && m_FileWatchListener)
			{
				for (efsw::WatchID wid : m_ActiveWatchIds)
					m_FileWatcher->removeWatch(wid);
				m_ActiveWatchIds.clear();

				std::set<std::string> dirs;
				auto sceneDir = std::filesystem::path(filepathCopy).
					parent_path().lexically_normal().string();
				if (!sceneDir.empty())
					dirs.insert(sceneDir);

				auto& doc = m_SceneMgr.AuthoringDoc();
				m_ScriptAssetContext.assetRoot =
					std::filesystem::path(filepathCopy).parent_path();
				m_ScriptAssetContext.database = nullptr;
				m_ScriptAssetDiagnostics.clear();
				auto view = doc.ecs.registry.view<ScriptComponent>();
				for (auto e : view)
				{
					const auto& sc = view.get<ScriptComponent>(e);
					const auto* id =
						doc.ecs.registry.try_get<EntityIdComponent>(e);
					const auto* name =
						doc.ecs.registry.try_get<NameComponent>(e);
					const size_t diagnosticBase =
						m_ScriptAssetDiagnostics.size();
					auto resolved = rt2::core::ResolveScriptAssetPath(
						sc, m_ScriptAssetContext,
						id ? id->id : rt2::core::UUID::Nil(),
						name ? name->name : std::string{},
						m_ScriptAssetDiagnostics);
					std::filesystem::path watchPath =
						resolved.resolvedPath;
					if (watchPath.empty() &&
						m_ScriptAssetDiagnostics.size() > diagnosticBase)
					{
						watchPath = m_ScriptAssetDiagnostics.back().
							resolvedPath;
					}
					auto parent = watchPath.parent_path().
						lexically_normal().string();
					if (!parent.empty())
						dirs.insert(parent);
				}
				LogScriptAssetDiagnostics(0, "FileWatcher");

				for (const auto& dir : dirs)
				{
					efsw::WatchID wid =
						m_FileWatcher->addWatch(dir,
							m_FileWatchListener.get(), true);
					if (wid < 0)
						printf("[FileWatcher] addWatch failed for "
							"\"%s\" (id=%d)\n", dir.c_str(),
							static_cast<int>(wid));
					else
						m_ActiveWatchIds.push_back(wid);
				}
			}

			m_History.Clear();
			CompactMeshRegistryNowAsserted();
			m_SceneMgr.ClearDirty();
			if (fieldChanges->requiresSave)
				m_SceneMgr.MarkDirty();
			m_ScriptRepairGate.Adopt(fieldChanges->destructive);
			m_Recovery->ResetSchedule();
			m_UntitledRecoveryId = rt2::core::OsUuidProvider{}.CreateV4().ToString();

			// Upload to GPU.
			if (m_RendererGPU.IsAvailable() && m_SceneMgr.GetECS().meshRegistry.GetCount() > 0)
			{
				printf("[OpenRt2Scene] UploadMeshToGPU...\n"); fflush(stdout);
				UploadMeshToGPU();
				printf("[OpenRt2Scene] UploadMeshToGPU done, setting gpuSyncPending\n"); fflush(stdout);
				m_GpuSyncPending = true;
			}
			else
			{
				printf("[OpenRt2Scene] no GPU upload needed (meshes=%d available=%d)\n",
				       (int)m_SceneMgr.GetECS().meshRegistry.GetCount(),
				       m_RendererGPU.IsAvailable() ? 1 : 0);
				fflush(stdout);
			}

			m_RendererGPU.ResetAccumulation();

			// Adopt the scene camera.
			const auto& cam = m_SceneMgr.GetECS().camera;
			m_Cam.SetPosition(cam.position);
			m_Cam.SetForwardDirection(cam.forwardDirection);

			// Update recents.
			if (m_Settings2)
			{
				m_Settings2->AddRecentScene(filepathCopy);
				PersistEditorSettings("recent scenes");
			}
			printf("[Scene] Loaded .rt2scene: %s\n", filepathCopy.c_str());
			if (fieldChanges->destructive)
				m_LastStatusMsg = "Opened with discarded script field data; Save once to acknowledge";
			else if (fieldChanges->requiresSave)
				m_LastStatusMsg = "Opened; script fields changed and the scene needs saving";
			else
				m_LastStatusMsg = "Opened";
		});
	}

	void SaveRt2Scene()
	{
		SaveCurrentScene(false);
	}

	void SaveRt2SceneAs()
	{
		SaveCurrentScene(true);
	}

	bool SaveCurrentScene(bool forceSaveAs)
	{
		if (m_ScriptRepairGate.ConsumeSaveAcknowledgement())
		{
			m_LastStatusMsg =
				"Script field data was discarded during load; repeat Save to confirm persistence";
			printf("[Scene] %s\n", m_LastStatusMsg.c_str());
			return false;
		}

		auto& doc = m_SceneMgr.AuthoringDoc();
		const std::filesystem::path oldSourcePath = doc.metadata.sourcePath;
		const std::string oldDocId = rt2::core::SceneRecoveryService::DocIdFor(
			doc, m_UntitledRecoveryId);

		std::filesystem::path target = oldSourcePath;
		if (forceSaveAs || target.empty())
		{
			const std::string selected = FileDialog::SaveFile(
				L"RT2 Scene (*.rt2scene)\0*.rt2scene\0", DialogInitialDirectory());
			if (selected.empty()) return false;
			target = std::filesystem::u8path(selected);
		}

		std::string extension = target.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
		if (extension != ".rt2scene") target.replace_extension(".rt2scene");

		rt2::core::Error err;
		if (!rt2::core::SceneSerializer::Save(doc, target, err))
		{
			// The live source path is committed only after the file is safe.
			m_LastStatusMsg = std::string("Save failed: ") + err.Format();
			printf("[Scene] %s\n", m_LastStatusMsg.c_str());
			return false;
		}

		doc.metadata.sourcePath = target;
		m_SceneMgr.ClearDirty();
		m_ScriptRepairGate.OnPersistedOrReset();
		const std::string newDocId = rt2::core::SceneRecoveryService::DocIdFor(
			doc, m_UntitledRecoveryId);
		if (oldDocId == newDocId) m_Recovery->DiscardForDoc(newDocId);
		else m_Recovery->OnSaveAs(oldDocId, newDocId);
		m_Recovery->ResetSchedule();

		if (m_Settings2) m_Settings2->AddRecentScene(target);
		const bool settingsSaved = PersistEditorSettings("recent scenes");
		if (settingsSaved)
			m_LastStatusMsg = (forceSaveAs || oldSourcePath.empty()) ? "Saved As" : "Saved";
		printf("[Scene] Saved .rt2scene: %s\n", target.u8string().c_str());
		return true;
	}

	void LoadEnvMap(const std::string& filepath)
	{
		if (IsBackgroundBusy()) return;

		const std::string pathCopy = filepath;
		struct EnvResult
		{
			int w = 0, h = 0;
			std::vector<float> pixels;
			std::string error;
			rt2::core::Error envImportErr; // sidecar read/write diagnostic
		};
		auto result = std::make_shared<EnvResult>();

		StartBackgroundWork("Loading environment map...",
			[result, pathCopy](BackgroundWork& self) -> bool
		{
			self.SetStatus("Decoding HDR/EXR...");
			bool isEXR = pathCopy.size() >= 4 &&
			             (pathCopy.compare(pathCopy.size() - 4, 4, ".exr") == 0 ||
			              pathCopy.compare(pathCopy.size() - 4, 4, ".EXR") == 0);

			if (isEXR)
			{
				float* outRGBA = nullptr;
				const char* err = nullptr;
				int ret = LoadEXR(&outRGBA, &result->w, &result->h, pathCopy.c_str(), &err);
				if (ret != TINYEXR_SUCCESS || !outRGBA)
				{
					result->error = err ? err : "unknown EXR decode error";
					if (err) free((void*)err);
					return false;
				}
				result->pixels.assign(outRGBA, outRGBA + (size_t)result->w * result->h * 4);
				free(outRGBA);
				if (err) free((void*)err);
			}
			else
			{
				int channels;
				float* data = stbi_loadf(pathCopy.c_str(), &result->w, &result->h, &channels, 4);
				if (!data)
				{
					result->error = "stbi_loadf failed";
					return false;
				}
				result->pixels.assign(data, data + (size_t)result->w * result->h * 4);
				stbi_image_free(data);
			}
			return true;
		},
			[this, result, pathCopy](bool success)
		{
			if (!success)
			{
				printf("[EnvMap] Failed: %s\n", result->error.c_str());
				m_LastStatusMsg = "Env map load failed";
				return;
			}

			m_SceneMgr.SetEnvMapData(pathCopy, result->w, result->h,
			                         std::move(result->pixels),
			                         /*envImportErr=*/&result->envImportErr);
			printf("[EnvMap] Loaded %dx%d\n", result->w, result->h);
			// Surface sidecar write/read errors through the status bar so the
			// user sees them instead of relying on console output (item 4).
			if (!result->envImportErr.IsOk())
			{
				m_LastStatusMsg = "Env loaded; identity sidecar issue: " +
				                  result->envImportErr.Format();
				printf("[Asset] env sidecar diagnostic: %s\n",
				       result->envImportErr.Format().c_str());
			}

			if (m_RendererGPU.IsAvailable() && m_SceneMgr.GetECS().meshRegistry.GetCount() > 0)
			{
				// SyncToGPU() is a silent no-op unless a sync callback has
				// been installed (SceneManager guards on `if (m_SyncCallback)`).
				// Calling it bare meant the env map never reached the GPU
				// unless some earlier operation happened to install one.
				UploadMeshToGPU();
				m_GpuSyncPending = true;
			}
			m_RendererGPU.ResetAccumulation();
			m_LastStatusMsg = "Env map loaded";
		});
	}

private:
};

Walnut::Application* Walnut::CreateApplication(int argc, char** argv)
{
	g_CLI = CLIArgs::Parse(argc, argv);

	Walnut::ApplicationSpecification spec;
	spec.Name = "RT2";
	spec.EnableValidation = g_CLI.validate;
	spec.EnableSyncValidation = g_CLI.syncValidate;

	Walnut::Application* app = new Walnut::Application(spec);
	auto layer = std::make_shared<RT2Layer>();
	app->PushLayer(layer);
	// Keep a raw pointer for the menubar callback. The Application owns the
	// layer (shared_ptr in the layer stack), so this pointer is valid for
	// the app's lifetime.
	RT2Layer* layerPtr = layer.get();
	app->SetMenubarCallback([app, layerPtr]()
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New"))
			{
				layerPtr->NewScene();
			}
			if (ImGui::MenuItem("Open..."))
			{
				std::string filepath = FileDialog::OpenFile(
					L"RT2 Scene (*.rt2scene)\0*.rt2scene\0glTF Binary (*.glb)\0*.glb\0glTF JSON (*.gltf)\0*.gltf\0OBJ Files (*.obj)\0*.obj\0",
					layerPtr->GetDialogInitialDirectory());
				if (!filepath.empty())
					layerPtr->OpenRt2Scene(filepath);
			}
			if (ImGui::MenuItem("Save"))
			{
				layerPtr->SaveRt2Scene();
			}
			if (ImGui::MenuItem("Save As..."))
			{
				layerPtr->SaveRt2SceneAs();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Exit"))
			{
				app->RequestClose();
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit"))
		{
			const std::string undoLabel = layerPtr->CanUndo()
				? ("Undo " + layerPtr->UndoDescription()) : "Undo";
			if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, layerPtr->CanUndo()))
				layerPtr->Undo();
			const std::string redoLabel = layerPtr->CanRedo()
				? ("Redo " + layerPtr->RedoDescription()) : "Redo";
			if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Shift+Z", false, layerPtr->CanRedo()))
				layerPtr->Redo();
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View"))
		{
			ImGui::TextDisabled("Windows");
			ImGui::Separator();
			ImGui::MenuItem("Camera", nullptr, &layerPtr->m_ShowInfoWindow);
			ImGui::MenuItem("Performance", nullptr, &layerPtr->m_ShowPerfWindow);
			ImGui::MenuItem("Render Settings", nullptr, &layerPtr->m_ShowRenderSettingsWin);
			ImGui::MenuItem("Scene", nullptr, &layerPtr->m_ShowSceneWindow);
			ImGui::MenuItem("Session", nullptr, &layerPtr->m_ShowSessionWindow);
			ImGui::MenuItem("Outliner", nullptr, &layerPtr->m_ShowHierarchyWindow);
			ImGui::MenuItem("Inspector", nullptr, &layerPtr->m_ShowInspectorWindow);
			ImGui::Separator();
			ImGui::TextDisabled("(Viewport is always shown)");
			ImGui::EndMenu();
		}
	});
	return app;
}
