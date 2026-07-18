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
#include "SceneRenderBridge.h"
#include "ECSComponents.h"
#include "EditorSettings.h"
#include "SceneRecoveryService.h"
#include "UnsavedChangesCoordinator.h"
#include "ViewportCoordinates.h"
#include "EditorTransformGizmo.h"
#include "core/UUID.h"
#include "core/Error.h"
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <tinyexr.h>
#include "NRD.h"

#include <cstdio>
#include <cmath>
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>

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

		m_EditorUI.SetSceneMgr(&m_SceneMgr);
		m_EditorUI.SetDialogInitialDirectoryProvider([this]() {
			return DialogInitialDirectory();
		});
		m_EditorUI.SetOnSceneChanged([this]() {
			if (m_RendererGPU.IsAvailable())
			{
				bool didCompact = m_SceneMgr.CompactMeshRegistry();
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
		m_EditorUI.SetOnLoadMeshFile([this](const std::string& path) -> SceneManager::EntityId {
			return LoadMeshFileAsEntity(path);
		});
		m_EditorUI.SetOnImportGltf([this](const std::string& path) -> SceneManager::EntityId {
			std::string ext = path.substr(path.find_last_of('.') + 1);
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			if (ext == "obj")
			{
				RequestOpenScene(path);
				return SceneManager::EntityId{};
			}
			auto id = m_SceneMgr.ImportGltf(path);
			if (id.IsValid())
			{
				m_PendingFullSync = true;
			}
			return id;
		});
		m_EditorUI.SetOnDumpGPUTransforms([this]() {
			if (m_RendererGPU.IsAvailable())
				m_RendererGPU.DumpInstanceTransforms();
		});
		m_EditorUI.SetOnDumpNEEBuffers([this]() {
			if (m_RendererGPU.IsAvailable())
				m_RendererGPU.DumpNEEBuffers();
		});
	}

	virtual void OnUIRender() override
	{
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

	ImGui::Begin("Info");
	ImGui::Text("Last Render: %.3fms", m_SmoothedFrameTime);
	ImGui::Text("FPS: %.1f", m_SmoothedFPS);
	if (m_RendererGPU.HasGpuTimings())
	{
		const auto& timings = m_RendererGPU.GetGpuTimings();
		if (timings.validMask != 0)
		{
			ImGui::Text("GPU timings (frame %llu)", static_cast<unsigned long long>(timings.frameIndex));
			for (uint32_t i = 0; i < static_cast<uint32_t>(GpuTimestampProfiler::Region::Count); i++)
				if (timings.validMask & (1u << i))
					ImGui::Text("  %s: %.3f ms",
					            GpuTimestampProfiler::RegionName(static_cast<GpuTimestampProfiler::Region>(i)),
					            timings.milliseconds[i]);
		}
	}

	if (m_RendererGPU.HasOutput())
		ImGui::Text("Render Res: %d x %d", m_RendererGPU.GetWidth(), m_RendererGPU.GetHeight());
	ImGui::Separator();

	ImGui::Separator();
	ImGui::Text("Renderer");
	bool rtSupported = Walnut::Application::IsRayTracingSupported();
	ImGui::Text("RT Supported: %s", rtSupported ? "yes" : "no");

	if (rtSupported && !m_RendererGPU.IsAvailable())
	{
		if (m_RendererGPU.Init())
		{
			m_Settings = m_RendererGPU.GetSettings();
			m_RendererGPU.ApplySettings(m_Settings);
		}
	}
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
	ImGui::Separator();
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
	ImGui::Text("Camera");
	glm::vec3 cameraPosition = m_Cam.GetPosition();
	glm::vec3 cameraForward = m_Cam.GetDirection();
	bool cameraPoseChanged = false;
	if (ImGui::DragFloat3("Position", &cameraPosition.x, 0.01f, 0.0f, 0.0f, "%.6f"))
	{
		m_Cam.SetPosition(cameraPosition);
		cameraPoseChanged = true;
	}
	if (ImGui::DragFloat3("Forward", &cameraForward.x, 0.001f, -1.0f, 1.0f, "%.6f"))
	{
		if (glm::dot(cameraForward, cameraForward) > 1e-8f)
		{
			m_Cam.SetForwardDirection(glm::normalize(cameraForward));
			cameraPoseChanged = true;
		}
	}
	if (cameraPoseChanged && m_RendererGPU.IsAvailable())
	{
		m_RendererGPU.ResetAccumulation();
		m_RendererGPU.InvalidateReSTIRHistory();
		m_RendererGPU.InvalidateGIHistory();
	}
	if (ImGui::Button("Copy Camera Pose"))
	{
		const glm::vec3& p = m_Cam.GetPosition();
		const glm::vec3& f = m_Cam.GetDirection();
		char pose[256];
		std::snprintf(pose, sizeof(pose),
		              "position=(%.6f, %.6f, %.6f) forward=(%.6f, %.6f, %.6f)",
		              p.x, p.y, p.z, f.x, f.y, f.z);
		ImGui::SetClipboardText(pose);
	}
	ImGui::SliderFloat("Move Speed", &m_Cam.m_Speed, 0.5f, 50.0f, "%.1f");
	if (ImGui::SliderFloat("Far Clip", &m_Cam.m_FarClip, 100.0f, 100000.0f, "%.0f"))
	{
		m_Cam.SetFarClip(m_Cam.m_FarClip);
		m_RendererGPU.ResetAccumulation();
	}
	bool rasterFirst = m_Settings.rasterFirst;
	ImGui::BeginDisabled(rasterFirst);
	if (ImGui::DragFloat("Aperture", &m_Cam.m_Aperture, 0.001f, 0.0f, 5.0f))
		m_RendererGPU.ResetAccumulation();
	if (ImGui::DragFloat("Focus Distance", &m_Cam.m_FocusDistance, 0.1f, 0.1f, 50.0f))
		m_RendererGPU.ResetAccumulation();
	ImGui::EndDisabled();
	if (rasterFirst && m_Cam.m_Aperture > 0.0f)
	{
		m_Cam.m_Aperture = 0.0f;
		m_RendererGPU.ResetAccumulation();
	}
	ImGui::Separator();
	if (ImGui::Checkbox("Raster-First Path", &m_Settings.rasterFirst))
	{
		if (m_Settings.rasterFirst)
			m_Cam.m_Aperture = 0.0f;
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

	ImGui::End();

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

	m_EditorUI.RenderPanels();

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
			m_Runtime.GetState() == rt2::core::SceneRunState::Edit,
			m_EditorUI.GetTransformSpace(), m_EditorUI.GetTransformPivot(),
			m_EditorUI.GetTransformSnapSettings());
		if (gizmo.changed)
			SyncAuthoringTransforms();
		if (!gizmo.error.empty())
			m_LastStatusMsg = gizmo.error;

		const bool ordinaryPickClick = imageHovered && !gizmo.consumesMouse &&
			ImGui::IsMouseClicked(ImGuiMouseButton_Left);
		const bool canPick = m_Runtime.GetState() == rt2::core::SceneRunState::Edit &&
			(ordinaryPickClick || gizmo.pickThrough) &&
			!ImGui::IsMouseDown(ImGuiMouseButton_Right);
		if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			RT_LOG("[ViewportPick] click edit=%d gizmoConsumes=%d gizmoActive=%d rightDown=%d",
				m_Runtime.GetState() == rt2::core::SceneRunState::Edit ? 1 : 0,
				gizmo.consumesMouse ? 1 : 0, gizmo.active ? 1 : 0,
				ImGui::IsMouseDown(ImGuiMouseButton_Right) ? 1 : 0);
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
	}

	ImGui::End();
	ImGui::PopStyleVar();

	// ---- Phase 1B: Session / Recovery UI ----
	DrawSessionPanel();
	DrawRecoveryPrompt();
	DrawUnsavedChangesPrompt();
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
				rt2::core::SceneDocument restored;
				restored.SetUuidProvider(m_SceneMgr.AuthoringDoc().GetUuidProvider());
				bool ok = m_Recovery->Restore(r, restored, diags, err);
				if (ok)
				{
					// Commit the already validated document without clearing it.
					m_SceneMgr.ReplaceAuthoringDocument(
						std::move(restored), std::max<uint64_t>(1, r.revision));
					m_Recovery->ResetSchedule();
					m_LastStatusMsg = "Restored recovery";
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

	virtual void OnUpdate(float ts) override
	{
		if (m_RuntimeCamActive)
			m_RuntimeCam.OnUpdate(ts);
		else
			m_Cam.OnUpdate(ts);

		if (!m_CLIProcessed) return;

		// Drive the runtime controller when Playing.
		if (m_Runtime.GetState() == rt2::core::SceneRunState::Playing && m_RenderBridge)
		{
			m_Runtime.Update(ts, *m_RenderBridge);
		}

		// ---- Phase 1B: autosave (authoring only, never runtime) ----
		// Only snapshot the authoring document; the runtime Play clone is
		// never captured. Skips work entirely on clean frames or when the
		// revision has not advanced since the last snapshot.
		if (m_Recovery && m_Runtime.GetState() == rt2::core::SceneRunState::Edit)
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

		if (!m_SceneMgr.LoadScene(filepath))
		{
			ImGui::OpenPopup("Scene Load Failed");
			return;
		}

		if (ext == "obj")
		{
			m_SceneMgr.SetSyncCallback([this](GPUSceneData& gpuData, const RenderInstanceMap& instanceMap) {
				m_RendererGPU.SetScene(gpuData, instanceMap);
			});
			m_SceneMgr.SyncToGPU();
			m_RendererGPU.ResetAccumulation();
		}
		else
		{
			if (m_RendererGPU.IsAvailable())
				UploadMeshToGPU();
		}

		const auto& cam = m_SceneMgr.GetECS().camera;
		m_Cam.SetPosition(cam.position);
		m_Cam.SetForwardDirection(cam.forwardDirection);

		// Imported interchange files become an untitled native authoring
		// document. They must be explicitly saved as .rt2scene.
		m_SceneMgr.AuthoringDoc().metadata.sourcePath.clear();
		m_SceneMgr.MarkDirty();
		m_UntitledRecoveryId = rt2::core::OsUuidProvider{}.CreateV4().ToString();
		m_Recovery->ResetSchedule();
		m_LastStatusMsg = "Imported scene (unsaved)";
	}

	SceneManager::EntityId LoadMeshFileAsEntity(const std::string& filepath)
	{
		std::string ext = filepath.substr(filepath.find_last_of('.') + 1);
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == "obj")
		{
			RequestOpenScene(filepath);
			return SceneManager::EntityId{};
		}

		if (!SceneLoader::LoadIntoECS(m_SceneMgr.GetECS(), filepath))
		{
			printf("[SceneEditor] Failed to load mesh: %s\n", filepath.c_str());
			return SceneManager::EntityId{};
		}

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
	std::unique_ptr<rt2::core::EditorSettingsStore>      m_Settings2;
	std::unique_ptr<rt2::core::SceneRecoveryService>      m_Recovery;
	rt2::core::UnsavedChangesCoordinator                  m_Unsaved;
	std::vector<rt2::core::SceneRecoveryService::RecoveryRecord> m_PendingRecovery;
	size_t                                                m_RecoveryPromptIndex = 0;
	bool                                                  m_RecoveryPromptOpen = false;
	std::string                                           m_UntitledRecoveryId; // stable per session
	std::string                                           m_LastStatusMsg;

	// ---- Runtime lifecycle ----

	void EnterPlay()
	{
		if (!m_RenderBridge)
			m_RenderBridge = new SceneRenderBridge(m_RendererGPU);

		rt2::core::Error err;
		if (!m_Runtime.Play(m_SceneMgr.AuthoringDoc(), *m_RenderBridge, err))
		{
			printf("[Play] Failed to enter Play: %s\n", err.Format().c_str());
			return;
		}

		// Snapshot the editor camera and switch to the runtime camera.
		m_EditorCamSnapshot = m_Cam;
		m_RuntimeCam = m_Cam;
		m_RuntimeCamActive = true;

		// Find the runtime scene's camera entity and adopt its pose.
		const rt2::core::SceneDocument* rt = m_Runtime.TryGetRuntimeScene();
		if (rt)
		{
			auto& reg = rt->ecs.registry;
			auto view = reg.view<CameraComponent>();
			for (auto e : view)
			{
				auto& cc = view.get<CameraComponent>(e);
				auto* tf = reg.try_get<Transform>(e);
				if (tf)
				{
					m_RuntimeCam.SetPosition(tf->translation);
					m_RuntimeCam.SetForwardDirection(cc.forwardDirection);
				}
				break; // first camera entity
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

	void NewScene()
	{
		m_Unsaved.Request({rt2::core::UnsavedChangesCoordinator::ActionKind::New, {}});
	}

	void NewSceneInternal()
	{
		m_SceneMgr.Clear();
		m_SceneMgr.ClearDirty();
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
		rt2::core::Error err;
		// Load + resolve into a temporary document first. Only on success do
		// we swap it into the live authoring document and GPU-sync. This
		// preserves the transactional guarantee: a parse, schema, or hard
		// resolution failure cannot partially replace the currently open
		// authoring scene.
		rt2::core::SceneDocument tempDoc;
		tempDoc.SetUuidProvider(m_SceneMgr.AuthoringDoc().GetUuidProvider());

		if (!rt2::core::SceneSerializer::Load(tempDoc, filepath, err))
		{
			printf("[Scene] Failed to load .rt2scene: %s\n", err.Format().c_str());
			ImGui::OpenPopup("Scene Load Failed");
			m_LastStatusMsg = std::string("Open failed: ") + err.Format();
			return;
		}

		// Resolve durable asset references into the temporary document. The
		// serializer persists durable refs only; the resolver rebuilds
		// transient mesh/texture/material/environment state from the source
		// files. Missing assets produce diagnostics, not crashes.
		std::filesystem::path sceneRoot = std::filesystem::path(filepath).parent_path();
		std::vector<rt2::core::AssetDiagnostic> diagnostics;
		rt2::core::Error resolveErr;
		bool resolveOk = rt2::core::SceneAssetResolver::ResolveAll(
		        tempDoc, sceneRoot, diagnostics, resolveErr);

		for (const auto& d : diagnostics)
		{
			const char* sev = (d.severity == rt2::core::AssetDiagnostic::Missing)
			                  ? "Missing" : (d.severity == rt2::core::AssetDiagnostic::Malformed)
			                  ? "Malformed" : "Unresolved";
			printf("[Scene] Asset %s: kind=%d ref='%s' resolved='%s' entity=%s%s%s"
			       " sourceKey='%s' detail=%s\n",
			       sev, (int)d.kind, d.refPath.c_str(), d.resolvedPath.c_str(),
			       d.entityUuid.IsNull() ? "(env)" : d.entityUuid.ToString().c_str(),
			       d.entityName.empty() ? "" : " (",
			       d.entityName.empty() ? "" : d.entityName.c_str(),
			       d.sourceKey.c_str(), d.detail.c_str());
		}

		if (!resolveOk)
		{
			// Hard resolution failure: every imported entity was unresolvable.
			// Preserve the currently open authoring document — do not swap in
			// a document that cannot render its imported content.
			printf("[Scene] Asset resolution failed, keeping current scene: %s\n",
			       resolveErr.Format().c_str());
			ImGui::OpenPopup("Scene Load Failed");
			m_LastStatusMsg = std::string("Open failed (resolution): ") + resolveErr.Format();
			return;
		}

		// Adopt the resolved document without an intermediate cleared live state.
		m_SceneMgr.ReplaceAuthoringDocument(std::move(tempDoc));
		m_SceneMgr.ClearDirty();
		m_Recovery->ResetSchedule();
		m_UntitledRecoveryId = rt2::core::OsUuidProvider{}.CreateV4().ToString();

		// Upload to GPU
		if (m_RendererGPU.IsAvailable() && m_SceneMgr.GetECS().meshRegistry.GetCount() > 0)
			UploadMeshToGPU();

		m_RendererGPU.ResetAccumulation();

		// Adopt the scene camera
		const auto& cam = m_SceneMgr.GetECS().camera;
		m_Cam.SetPosition(cam.position);
		m_Cam.SetForwardDirection(cam.forwardDirection);

		// Update recents.
		if (m_Settings2)
		{
			m_Settings2->AddRecentScene(filepath);
			PersistEditorSettings("recent scenes");
		}
		printf("[Scene] Loaded .rt2scene: %s\n", filepath.c_str());
		m_LastStatusMsg = "Opened";
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
		if (!m_SceneMgr.LoadEnvMap(filepath))
			return;
		if (m_RendererGPU.IsAvailable() && m_SceneMgr.GetECS().meshRegistry.GetCount() > 0)
			m_SceneMgr.SyncToGPU();
		m_RendererGPU.ResetAccumulation();
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
	});
	return app;
}
