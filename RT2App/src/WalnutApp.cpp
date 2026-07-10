#include "Walnut/Application.h"
#include "Walnut/EntryPoint.h"

#include "Walnut/Image.h"
#include "Walnut/Timer.h"
#include "Renderer.h"
#include "RendererGPU.h"
#include "RenderSettings.h"
#include "SceneManager.h"
#include "Mesh.h"
#include "FileDialog.h"
#include "Scene.h"
#include "SceneLoader.h"
#include "GPUSceneData.h"
#include "ECSScene.h"
#include "SceneGraph.h"
#include "SceneEditorUI.h"
#include "PrimitiveGeometry.h"
#include "CLIArgs.h"
#include "RTLog.h"
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <tinyexr.h>
#include "NRD.h"

#include <cstdio>
#include <thread>
#include <chrono>

using namespace Walnut;

// Global CLI args (parsed in CreateApplication, consumed by RT2Layer)
static CLIArgs g_CLI;

class RT2Layer : public Walnut::Layer
{
public:

		RT2Layer()
		{
			m_Cam = Camera(45.0f, 0.1f, 10000.0f,0.005f,2.5f);
			m_Renderer = Renderer();
			m_RenderOnUpdate = true;
			m_Cam.m_Aperture = 0.0f;  // NRD/raster-first requires no DOF

			m_MeshMaterial = make_shared<LambertianMaterial>(vec3(0.7f, 0.7f, 0.7f));

			// Wire scene editor UI
			m_EditorUI.SetSceneMgr(&m_SceneMgr);
			m_EditorUI.SetOnSceneChanged([this]() {
				if (m_UseGPU && m_RendererGPU.IsAvailable())
				{
					m_SceneMgr.CompactMeshRegistry();
					if (m_PendingFullSync)
					{
						// Import brought new textures — need full re-upload
						m_SceneMgr.SetSyncCallback([this](const GPUSceneData& gpuData) {
							m_RendererGPU.SetScene(gpuData);
						});
						m_SceneMgr.SyncToGPU();
						m_PendingFullSync = false;
					}
					else
					{
						m_SceneMgr.SetSyncKeepTexturesCallback([this](const GPUSceneData& gpuData) {
							m_RendererGPU.SetSceneKeepTextures(gpuData);
						});
						m_SceneMgr.SyncToGPUKeepTextures();
					}
				}
				m_RendererGPU.ResetAccumulation();
			});
			m_EditorUI.SetOnTransformChanged([this]() {
				if (m_UseGPU && m_RendererGPU.IsAvailable())
				{
					m_SceneMgr.SetInstanceSyncCallback([this](const GPUSceneData& gpuData) {
						m_RendererGPU.UpdateSceneInstances(gpuData);
					});
					m_SceneMgr.SyncTransformsToGPU();
				}
				m_RendererGPU.ResetAccumulation();
			});
			m_EditorUI.SetOnLoadMeshFile([this](const std::string& path) -> SceneManager::EntityId {
				return LoadMeshFileAsEntity(path);
			});
			m_EditorUI.SetOnImportGltf([this](const std::string& path) -> SceneManager::EntityId {
				auto id = m_SceneMgr.ImportGltf(path);
				if (id.IsValid())
				{
					// Don't sync here — NotifySceneChanged (called by the Add
					// popup after this callback) will trigger SyncToGPUKeepTextures.
					// But imported glTFs bring new textures, so we need a FULL
					// sync (SetScene with texture upload), not keep-textures.
					// Flag the scene changed callback to use full sync this once.
					m_PendingFullSync = true;
				}
				return id;
			});
			m_EditorUI.SetOnDumpGPUTransforms([this]() {
				if (m_UseGPU && m_RendererGPU.IsAvailable())
					m_RendererGPU.DumpInstanceTransforms();
			});
			m_EditorUI.SetOnDumpNEEBuffers([this]() {
				if (m_UseGPU && m_RendererGPU.IsAvailable())
					m_RendererGPU.DumpNEEBuffers();
			});
		}

	virtual void OnUIRender() override
	{
		// First-frame: process CLI args (scene/env auto-load, headless mode)
		if (!m_CLIProcessed)
		{
			ProcessCLIArgs();
			m_CLIProcessed = true;

			// Apply UI defaults for interactive (non-headless) mode.
			// CLI flags have already overridden what they specify; the
			// remaining fields get sensible UI defaults.
			if (!g_CLI.headless && m_UseGPU && m_RendererGPU.IsAvailable())
			{
				m_Settings.showBackground = true;
				m_Settings.rasterFirst = true;
				m_Settings.nrdEnabled = true;
				m_Cam.m_Aperture = 0.0f;  // NRD/raster-first requires no DOF
				m_RendererGPU.ApplySettings(m_Settings);
			}
		}

	ImGui::Begin("Info");
	ImGui::Text("Last Render: %.3fms", m_SmoothedFrameTime);
	ImGui::Text("Rays Cast: %d", m_Renderer.m_NumRaysCast);
	ImGui::Text("FPS: %.1f", m_SmoothedFPS);

	float raysPerSec = m_SmoothedFPS > 0.0f ? m_Renderer.m_NumRaysCast * m_SmoothedFPS : 0.0f;

	ImGui::Text("Rays/Sec: %.1f", raysPerSec);
		
		if (auto image = m_Renderer.GetFinalImage())
			ImGui::Text("Render Res: %d x %d", image->GetWidth(), image->GetHeight());
		ImGui::Separator();
		ImGui::Text("Triangles: %u", m_Renderer.m_TriangleCount);
		ImGui::Text("BVH Nodes: %u", m_Renderer.m_BvhNodeCount);
		ImGui::Text("BVH Max Depth: %d", m_Renderer.m_BvhMaxDepth);
		ImGui::Text("Accum Frames: %u", m_Renderer.mFrameIndex);
		ImGui::Separator();
		
		if (ImGui::Button("Render")) {
			Render();
		};

		ImGui::Checkbox("Render on Update", &m_RenderOnUpdate);

		ImGui::Separator();
		ImGui::Text("Renderer");
		bool rtSupported = Walnut::Application::IsRayTracingSupported();
		ImGui::Text("RT Supported: %s", rtSupported ? "yes" : "no");

		// Auto-init GPU renderer if RT is supported and default is GPU
		if (rtSupported && m_UseGPU == 1 && !m_RendererGPU.IsAvailable())
		{
			if (m_RendererGPU.Init())
			{
				m_Settings = m_RendererGPU.GetSettings();
				m_Settings.spp = m_Renderer.m_SamplesPerPixel;
				m_Settings.maxBounces = m_Renderer.m_MaxBounceDepth;
				m_RendererGPU.ApplySettings(m_Settings);
			}
			else
				m_UseGPU = 0; // fall back to CPU if init fails
		}
		if (rtSupported)
		{
			if (ImGui::RadioButton("CPU", m_UseGPU == 0)) { m_UseGPU = 0; }
			ImGui::SameLine();
			if (ImGui::RadioButton("GPU (Vulkan RT)", m_UseGPU == 1))
			{
				if (!m_RendererGPU.IsAvailable())
				{
					if (m_RendererGPU.Init())
					{
						m_UseGPU = 1;
						m_Settings = m_RendererGPU.GetSettings();
						m_Settings.spp = m_Renderer.m_SamplesPerPixel;
						m_Settings.maxBounces = m_Renderer.m_MaxBounceDepth;
						m_RendererGPU.ApplySettings(m_Settings);
						if (m_SceneMgr.GetECS().meshRegistry.GetCount() > 0 || m_Mesh.IsLoaded())
							UploadMeshToGPU();
					}
					else
					{
						ImGui::OpenPopup("GPU Init Failed");
					}
				}
				else
				{
					m_UseGPU = 1;
					m_Settings = m_RendererGPU.GetSettings();
					m_Settings.spp = m_Renderer.m_SamplesPerPixel;
					m_Settings.maxBounces = m_Renderer.m_MaxBounceDepth;
					m_RendererGPU.ApplySettings(m_Settings);
					if (m_SceneMgr.GetECS().meshRegistry.GetCount() > 0 || m_Mesh.IsLoaded())
						UploadMeshToGPU();
				}
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
	if (ImGui::DragInt("SPP", &m_Renderer.m_SamplesPerPixel, 1.0f, 1, 1500))
	{
		m_Settings.spp = m_Renderer.m_SamplesPerPixel;
		m_RendererGPU.ApplySettings(m_Settings);
	}
	ImGui::Text("Sample Depth");
	if (ImGui::DragInt("Bounces", &m_Renderer.m_MaxBounceDepth, 1.0f, 2, 100))
	{
		m_Settings.maxBounces = m_Renderer.m_MaxBounceDepth;
		m_RendererGPU.ApplySettings(m_Settings);
	}
	if (ImGui::Button("Reset")) {
		m_Renderer.m_SamplesPerPixel = 1;
		m_Renderer.m_MaxBounceDepth = 2;
		m_Settings.spp = 1;
		m_Settings.maxBounces = 2;
		m_RendererGPU.ApplySettings(m_Settings);
	};

	ImGui::Separator();
	ImGui::Text("Camera");
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
	if (m_Settings.rasterFirst)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(aperture=0, no DOF)");
	}
	if (ImGui::Checkbox("Show Background", &m_Settings.showBackground))
		m_RendererGPU.ApplySettings(m_Settings);
	if (ImGui::SliderFloat("Emissive Boost", &m_Settings.emissiveBoost, 0.0f, 50.0f, "%.1f"))
		m_RendererGPU.ApplySettings(m_Settings);

	// NRD only works with raster-first (RT-primary is accumulation-only after B4)
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
		if (ImGui::Checkbox("Jitter", &m_Settings.nrdJitterEnabled))
			m_RendererGPU.ApplySettings(m_Settings);
		ImGui::SameLine();
		ImGui::PushID("JitterScale");
		ImGui::SetNextItemWidth(80.0f);
		if (ImGui::SliderFloat("Scale", &m_Settings.nrdJitterScale, 0.0f, 1.0f, "%.2f"))
			m_RendererGPU.ApplySettings(m_Settings);
		ImGui::PopID();
		ImGui::SliderFloat("Blur Radius", &m_Settings.nrdMaxBlurRadius, 1.0f, 50.0f, "%.1f");
		ImGui::SliderInt("Accum Frames", &m_Settings.nrdMaxAccumFrames, 1, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
		ImGui::Checkbox("Anti-Firefly", &m_Settings.nrdAntiFirefly);
		const char* ditherOptions[] = { "Off (white noise)", "Bayer 4x4", "Interleaved Gradient" };
		int& dither = m_Settings.nrdLobeDither;
		ImGui::Combo("Lobe Dither", &dither, ditherOptions, 3);
		ImGui::SliderFloat("Split Screen", &m_Settings.nrdSplitScreen, 0.0f, 1.0f, "%.2f");
		ImGui::Unindent();
		// Apply any NRD slider changes
		m_RendererGPU.ApplySettings(m_Settings);
	}
	ImGui::EndDisabled();
	ImGui::Separator();

	// RIS (Resampled Importance Sampling) — raster-first only
	bool risAvailable = m_Settings.rasterFirst;
	if (!risAvailable && m_Settings.risEnabled)
	{
		m_Settings.risEnabled = false;
		m_RendererGPU.ApplySettings(m_Settings);
	}
	ImGui::BeginDisabled(!risAvailable);
	if (ImGui::Checkbox("RIS (Resampled Importance Sampling)", &m_Settings.risEnabled))
		m_RendererGPU.ApplySettings(m_Settings);
	if (m_Settings.risEnabled)
	{
		ImGui::Indent();
		int m = (int)m_Settings.risCandidateCount;
		if (ImGui::SliderInt("Candidates (M)", &m, 1, 32))
		{
			m_Settings.risCandidateCount = (uint32_t)m;
			m_RendererGPU.ApplySettings(m_Settings);
		}
		ImGui::Unindent();
	}
	ImGui::EndDisabled();
	ImGui::Separator();
	ImGui::Text("G-buffer Debug");
	const char* gbufferModes[] = {
		"Off", "Shading Normal", "Roughness", "ViewZ (depth)",
		"Motion Vectors", "Albedo", "F0", "Direct Emission",
		"World Position", "Geo Normal", "UV", "Material Index",
		"RIS Reservoir"
	};
	int debugCombo = m_Settings.gbufferDebugMode + 1; // -1→0 (Off), 0→1, etc.
	if (ImGui::Combo("G-buffer View", &debugCombo, gbufferModes, IM_ARRAYSIZE(gbufferModes)))
	{
		m_Settings.gbufferDebugMode = debugCombo - 1;
		m_RendererGPU.ApplySettings(m_Settings);
	}
	ImGui::Separator();
	ImGui::Text("Environment Map");
	if (ImGui::Button("Load HDR..."))
	{
		std::string path = FileDialog::OpenFile("HDR Files (*.hdr;*.exr)\0*.hdr;*.exr\0HDR (*.hdr)\0*.hdr\0All Files (*.*)\0*.*\0");
		if (!path.empty())
			LoadEnvMap(path);
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear HDR"))
	{
		m_SceneMgr.ClearEnvMap();
		if (m_UseGPU && m_RendererGPU.IsAvailable() && (m_SceneMgr.GetECS().meshRegistry.GetCount() > 0 || m_Mesh.IsLoaded()))
			UploadMeshToGPU();
		m_RendererGPU.ResetAccumulation();
	}
	if (m_SceneMgr.HasEnvMap())
		ImGui::Text("Loaded: %s (%dx%d)", m_SceneMgr.GetEnvMapPath().c_str(), m_SceneMgr.GetEnvMapWidth(), m_SceneMgr.GetEnvMapHeight());
	if (ImGui::SliderFloat("Env Intensity", &m_Settings.envIntensity, 0.0f, 10.0f, "%.2f"))
		m_RendererGPU.ApplySettings(m_Settings);
	ImGui::End();

		ImGui::Begin("Mesh");
		ImGui::Text("Loaded: %s", m_Mesh.IsLoaded() ? "yes" : "no");
		if (ImGui::Button("Load Mesh..."))
		{
			std::string path = FileDialog::OpenFile("OBJ Files (*.obj)\0*.obj\0All Files (*.*)\0*.*\0");
			if (!path.empty())
			{
				m_MeshPath = path;
				ReloadMesh();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear Mesh"))
		{
			m_Mesh.Clear();
			m_Renderer.ClearHittables();
			m_Renderer.m_TriangleCount = 0;
			m_Renderer.m_BvhNodeCount = 0;
			m_Renderer.m_BvhMaxDepth = 0;
		}

		bool transformChanged = false;
		transformChanged |= ImGui::DragFloat3("Position", &m_MeshPosition[0], 0.1f);
		transformChanged |= ImGui::DragFloat3("Rotation", &m_MeshRotation[0], 1.0f);
		transformChanged |= ImGui::DragFloat("Scale", &m_MeshScale, 0.1f, 0.01f, 100.0f);

		ImGui::Separator();
		ImGui::Text("Material");
		if (ImGui::Combo("Type", &m_MaterialType, "Lambertian\0Metal\0Dielectric\0"))
		{
			RebuildMaterial();
			ReloadMesh();
		}
		if (m_MaterialType == 0 || m_MaterialType == 1)
		{
			if (ImGui::ColorEdit3("Albedo", &m_MeshAlbedo[0]))
			{
				RebuildMaterial();
				ReloadMesh();
			}
		}
		if (m_MaterialType == 1)
		{
			if (ImGui::DragFloat("Fuzz", &m_MeshFuzz, 0.01f, 0.0f, 1.0f))
			{
				RebuildMaterial();
				ReloadMesh();
			}
		}
		if (m_MaterialType == 2)
		{
			if (ImGui::DragFloat("IOR", &m_MeshIOR, 0.01f, 1.0f, 3.0f))
			{
				RebuildMaterial();
				ReloadMesh();
			}
		}

		if (m_Mesh.IsLoaded() && transformChanged)
		{
			ReloadMesh();
		}

		ImGui::End();

		ImGui::Begin("Scene");
		ImGui::Text("Meshes: %d", (int)m_SceneMgr.GetScene().GetMeshes().size());
		ImGui::Text("Materials: %d", (int)m_SceneMgr.GetScene().GetMaterials().size());
		ImGui::Text("Lights: %d", (int)m_SceneMgr.GetScene().GetLights().size());
		ImGui::Text("Textures: %d", (int)m_SceneMgr.GetScene().GetTextures().size());
		ImGui::Separator();
		if (ImGui::Button("Load Scene..."))
		{
			printf("[Scene] Open file dialog...\n");
			std::string path = FileDialog::OpenFile("glTF Binary (*.glb)\0*.glb\0glTF JSON (*.gltf)\0*.gltf\0All Files (*.*)\0*.*\0");
			printf("[Scene] Dialog returned: '%s'\n", path.c_str());
			if (!path.empty())
				LoadScene(path);
		}
		ImGui::SameLine();
		if (ImGui::Button("Save Scene..."))
		{
			std::string path = FileDialog::SaveFile("glTF Binary (*.glb)\0*.glb\0glTF JSON (*.gltf)\0*.gltf\0All Files (*.*)\0*.*\0");
			if (!path.empty())
			{
				BuildSceneFromCurrentState();
				SceneLoader::Save(m_SceneMgr.GetScene(), path);
			}
		}
		ImGui::Separator();
		if (!m_SceneMgr.GetScene().GetMeshes().empty())
		{
			ImGui::Text("Loaded meshes:");
			for (int i = 0; i < (int)m_SceneMgr.GetScene().GetMeshes().size(); i++)
			{
				const auto& m = m_SceneMgr.GetScene().GetMesh(i);
				if (m.HasGeometry())
					ImGui::Text("  [%d] geometry (%d verts, %d tris)",
					            i, (int)(m.vertices.size() / 3), (int)(m.indices.size() / 3));
				else
					ImGui::Text("  [%d] %s", i, m.filepath.c_str());
			}
		}
		ImGui::End();

		m_EditorUI.RenderPanels();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Viewport");

		m_ViewportWidth = ImGui::GetContentRegionAvail().x;
		m_ViewportHeight = ImGui::GetContentRegionAvail().y;

		// Resize before ImGui::Image so the descriptor set is valid when
		// ImGui records its draw. Calling after would free the descriptor
		// that ImGui just used, causing device loss on submit.
		if (m_UseGPU && m_RendererGPU.IsAvailable())
		{
			m_RendererGPU.OnResize(m_ViewportWidth, m_ViewportHeight);
			m_Cam.OnResize(m_ViewportWidth, m_ViewportHeight);
		}

		if (m_UseGPU && m_RendererGPU.HasOutput())
			ImGui::Image((ImTextureID)m_RendererGPU.GetOutputDescriptorSet(),
			             { (float)m_RendererGPU.GetWidth(), (float)m_RendererGPU.GetHeight() });
		else if (auto image = m_Renderer.GetFinalImage())
			ImGui::Image(image->GetDescriptorSet(), { (float)image->GetWidth(),(float)image->GetHeight() });

		ImGui::End();
		ImGui::PopStyleVar();

		if (m_RenderOnUpdate) {
			if (m_UseGPU)
			{
				Render();
			}
			else
			{
				m_Renderer.setTemporalAccumulation(true);
				Render();
			}
		}
		else {
			m_Renderer.setTemporalAccumulation(false);
		}
	}

	virtual void OnUpdate(float ts) override
	{
		m_Cam.OnUpdate(ts);
	}

private:

	void ProcessCLIArgs()
	{
		if (g_CLI.verbose)
			g_CLI.Print();

		// --list: just print and exit
		if (g_CLI.listScenes)
		{
			printf("[CLI] --list mode: would load scene='%s' env='%s'\n",
			       g_CLI.scenePath.c_str(), g_CLI.envMapPath.c_str());
			Walnut::Application::Get().Close();
			return;
		}

		// Apply renderer selection
		if (g_CLI.renderer == "cpu")
			m_UseGPU = 0;
		else
			m_UseGPU = 1;

		// Apply SPP/bounces overrides
		if (g_CLI.spp > 0)
		{
			m_Renderer.m_SamplesPerPixel = g_CLI.spp;
			m_Settings.spp = g_CLI.spp;
		}
		if (g_CLI.bounces > 0)
		{
			m_Renderer.m_MaxBounceDepth = g_CLI.bounces;
			m_Settings.maxBounces = g_CLI.bounces;
		}
		if (g_CLI.nrd)
			m_Settings.nrdEnabled = true;
		if (g_CLI.rasterFirst)
		{
			m_Settings.rasterFirst = true;
			m_Cam.m_Aperture = 0.0f;  // raster-first can't do DOF
		}
		if (g_CLI.gbufferDebug >= 0)
			m_Settings.gbufferDebugMode = g_CLI.gbufferDebug;

		// Auto-init GPU renderer if needed
		if (m_UseGPU == 1 && Walnut::Application::IsRayTracingSupported() && !m_RendererGPU.IsAvailable())
		{
			if (m_RendererGPU.Init())
			{
				m_Settings = m_RendererGPU.GetSettings();
				m_Settings.spp = m_Renderer.m_SamplesPerPixel;
				m_Settings.maxBounces = m_Renderer.m_MaxBounceDepth;
				// Re-apply CLI overrides on top of defaults
			if (g_CLI.nrd) m_Settings.nrdEnabled = true;
			if (g_CLI.rasterFirst) m_Settings.rasterFirst = true;
			if (g_CLI.ris) { m_Settings.risEnabled = true; m_Settings.rasterFirst = true; }
			if (g_CLI.gbufferDebug >= 0) m_Settings.gbufferDebugMode = g_CLI.gbufferDebug;
				m_RendererGPU.ApplySettings(m_Settings);
			}
			else
			{
				RT_LOG("[CLI] GPU renderer init failed, falling back to CPU");
				m_UseGPU = 0;
			}
		}

		// Auto-load env map
		if (g_CLI.hasEnvMap())
			LoadEnvMap(g_CLI.envMapPath);

		// Auto-load scene
		if (g_CLI.hasScene())
			LoadScene(g_CLI.scenePath);

		// Raster-first: force aperture=0 (DOF fallback) AFTER scene load,
		// because the scene file may store a non-zero aperture in camera extras.
		if (g_CLI.rasterFirst)
			m_Cam.m_Aperture = 0.0f;

		// Headless mode: render N frames, screenshot, exit
		if (g_CLI.headless)
		{
			RunHeadless();
		}
	}

	void RunHeadless()
	{
		printf("[Headless] starting: %d frames at %dx%d\n", g_CLI.frames, g_CLI.width, g_CLI.height);

		// Resize to requested dimensions
		m_ViewportWidth = (uint32_t)g_CLI.width;
		m_ViewportHeight = (uint32_t)g_CLI.height;
		if (m_UseGPU && m_RendererGPU.IsAvailable())
		{
			m_RendererGPU.OnResize(m_ViewportWidth, m_ViewportHeight);
			m_Cam.OnResize(m_ViewportWidth, m_ViewportHeight);
		}
		else
		{
			m_Renderer.OnResize(m_ViewportWidth, m_ViewportHeight);
			m_Cam.OnResize(m_ViewportWidth, m_ViewportHeight);
		}

		// Wait for async texture upload to complete before rendering headless
		// frames (ensures the screenshot captures the final textured scene).
		if (m_UseGPU && m_RendererGPU.IsAvailable())
		{
			while (m_RendererGPU.IsTextureUploadPending())
			{
				m_RendererGPU.PollTextureUpload();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		// Render N frames
		for (int i = 0; i < g_CLI.frames; i++)
		{
			Timer timer;
			if (m_UseGPU && m_RendererGPU.IsAvailable())
			{
				m_RendererGPU.Render(m_Cam);
			}
			else
				m_Renderer.Render(m_Cam);
			float ms = timer.ElapsedMillis();
			if (g_CLI.verbose || i == g_CLI.frames - 1)
				printf("[Headless] frame %d/%d: %.1fms\n", i + 1, g_CLI.frames, ms);
		}

		// Screenshot
		if (g_CLI.hasOutput() && m_UseGPU && m_RendererGPU.IsAvailable())
		{
			std::vector<uint8_t> pixels;
			uint32_t w, h;
			if (m_RendererGPU.ReadbackOutput(pixels, w, h))
			{
				// stbi_write_png expects bottom-up rows; Vulkan image is top-down
				// Flip vertically by writing rows in reverse
				std::vector<uint8_t> flipped(pixels.size());
				for (uint32_t y = 0; y < h; y++)
				{
					memcpy(&flipped[(size_t)(h - 1 - y) * w * 4],
					       &pixels[(size_t)y * w * 4],
					       (size_t)w * 4);
				}

				if (stbi_write_png(g_CLI.outputPath.c_str(), w, h, 4, flipped.data(), w * 4))
					printf("[Headless] saved screenshot: %s (%ux%u)\n", g_CLI.outputPath.c_str(), w, h);
				else
					RT_LOG("[Headless] stbi_write_png failed for %s", g_CLI.outputPath.c_str());
			}
			else
			{
				RT_LOG("[Headless] ReadbackOutput failed");
			}
		}
		else if (g_CLI.hasOutput() && !m_UseGPU)
		{
			// CPU renderer: get final image and save
			if (auto image = m_Renderer.GetFinalImage())
			{
				uint32_t w = image->GetWidth();
				uint32_t h = image->GetHeight();
				const uint32_t* data = m_Renderer.GetImageData();
				if (data)
				{
					// CPU renderer packs as ABGR (BGRA in memory), convert to RGBA for PNG
					std::vector<uint8_t> pixels(w * h * 4);
					for (size_t i = 0; i < (size_t)w * h; i++)
					{
						uint32_t c = data[i];
						pixels[i * 4 + 0] = (uint8_t)(c & 0xFF);         // R
						pixels[i * 4 + 1] = (uint8_t)((c >> 8) & 0xFF);  // G
						pixels[i * 4 + 2] = (uint8_t)((c >> 16) & 0xFF); // B
						pixels[i * 4 + 3] = (uint8_t)((c >> 24) & 0xFF); // A
					}
					// Flip vertically for PNG
					std::vector<uint8_t> flipped(pixels.size());
					for (uint32_t y = 0; y < h; y++)
					{
						memcpy(&flipped[(size_t)(h - 1 - y) * w * 4],
						       &pixels[(size_t)y * w * 4],
						       (size_t)w * 4);
					}
					if (stbi_write_png(g_CLI.outputPath.c_str(), w, h, 4, flipped.data(), w * 4))
						printf("[Headless] saved CPU screenshot: %s (%ux%u)\n", g_CLI.outputPath.c_str(), w, h);
				}
			}
		}

		printf("[Headless] done, exiting\n");
		Walnut::Application::Get().Close();
	}

	void Render() {
		Timer timer;

		if (m_UseGPU && m_RendererGPU.IsAvailable())
		{
			m_RendererGPU.Render(m_Cam);
		}
		else
		{
			m_Renderer.OnResize(m_ViewportWidth, m_ViewportHeight);
			m_Cam.OnResize(m_ViewportWidth, m_ViewportHeight);
			m_Renderer.Render(m_Cam);
		}
		m_LastRenderTime = timer.ElapsedMillis();

		// EMA over ~0.5s: alpha = dt / (dt + timeConstant)
		// At ~60fps, dt~16.7ms, 0.5s constant → alpha~0.032 (~15 frame avg)
		float alpha = m_LastRenderTime / (m_LastRenderTime + 500.0f);
		m_SmoothedFrameTime = m_SmoothedFrameTime * (1.0f - alpha) + m_LastRenderTime * alpha;
		m_SmoothedFPS = m_SmoothedFrameTime > 0.0f ? 1000.0f / m_SmoothedFrameTime : 0.0f;
	}

	void RebuildMaterial()
	{
		switch (m_MaterialType)
		{
		case 0: m_MeshMaterial = make_shared<LambertianMaterial>(m_MeshAlbedo); break;
		case 1: m_MeshMaterial = make_shared<MetalMaterial>(m_MeshAlbedo, m_MeshFuzz); break;
		case 2: m_MeshMaterial = make_shared<DieletricMaterial>(m_MeshIOR); break;
		}
	}

	void ReloadMesh()
	{
		if (m_MeshPath.empty()) return;
		RebuildMaterial();
		if (m_Mesh.Load(m_MeshPath, m_MeshPosition, m_MeshRotation, m_MeshScale, m_MeshMaterial))
		{
			m_Renderer.ClearHittables();
			m_Renderer.AddHittable(m_Mesh.GetBvhRoot());
			m_Renderer.m_TriangleCount = static_cast<uint32_t>(m_Mesh.GetTriangleCount());

			m_Renderer.m_BvhNodeCount = 0;
			m_Renderer.m_BvhMaxDepth = 0;
			if (auto bvh = m_Mesh.GetBvhNode())
				bvh->GetStats(m_Renderer.m_BvhNodeCount, m_Renderer.m_BvhMaxDepth);

			if (m_UseGPU && m_RendererGPU.IsAvailable())
				UploadMeshToGPU();
		}
	}

	void UploadMeshToGPU()
	{
		if (m_SceneMgr.GetECS().meshRegistry.GetCount() == 0 && !m_Mesh.IsLoaded()) return;

		// Set the sync callback to push to the renderer, then call SyncToGPU.
		// The callback is set once but is safe to set repeatedly.
		m_SceneMgr.SetSyncCallback([this](const GPUSceneData& gpuData) {
			m_RendererGPU.SetScene(gpuData);
		});

		// If no ECS meshes but we have a CPU OBJ mesh, inject it as a
		// single-mesh scene via the legacy path.
		if (m_SceneMgr.GetECS().meshRegistry.GetCount() == 0 && m_Mesh.IsLoaded())
		{
			// Build a minimal GPUSceneData from the OBJ mesh
			GPUSceneData gpuData;
			auto [verts, indices] = m_Mesh.GetRawVertexData();
			GPUMeshGeometry geo;
			geo.vertices = verts;
			geo.indices = indices;
			geo.materialIndex = 0;
			gpuData.meshes.push_back(std::move(geo));

			SceneMaterial sm;
			sm.baseColor = m_MeshAlbedo;
			sm.metallic = (m_MaterialType == 1) ? 1.0f : 0.0f;
			sm.roughness = m_MeshFuzz;
			sm.ior = m_MeshIOR;
			gpuData.materials.push_back(GPUMaterial::fromSceneMaterial(sm));

			GPUInstance inst;
			inst.meshIndex = 0;
			inst.materialIndex = 0;
			inst.worldMatrix = glm::mat4(1.0f);
			inst.prevWorldMatrix = glm::mat4(1.0f);
			gpuData.instances.push_back(inst);

			m_RendererGPU.SetScene(gpuData);
			return;
		}

		m_SceneMgr.SyncToGPU();
	}

	void LoadScene(const std::string& filepath)
	{
		if (!m_SceneMgr.LoadScene(filepath))
		{
			ImGui::OpenPopup("Scene Load Failed");
			return;
		}

		// Wire CPU renderer to the loaded meshes
		m_Renderer.ClearHittables();
		m_Renderer.m_TriangleCount = 0;
		m_Renderer.m_BvhNodeCount = 0;
		m_Renderer.m_BvhMaxDepth = 0;

		bool loadedAny = false;
		for (const auto& sceneMesh : m_SceneMgr.GetScene().GetMeshes())
		{
			Mesh mesh;
			bool meshLoaded = false;

			if (sceneMesh.HasGeometry())
			{
				auto material = make_shared<LambertianMaterial>(vec3(0.7f, 0.7f, 0.7f));
				meshLoaded = mesh.LoadFromGeometry(sceneMesh.vertices, sceneMesh.normals,
				                                   sceneMesh.indices, sceneMesh.position,
				                                   sceneMesh.rotation, sceneMesh.scale, material);
			}
			else if (!sceneMesh.filepath.empty())
			{
				auto material = make_shared<LambertianMaterial>(vec3(0.7f, 0.7f, 0.7f));
				meshLoaded = mesh.Load(sceneMesh.filepath, sceneMesh.position, sceneMesh.rotation,
				                       sceneMesh.scale, material);
			}

			if (meshLoaded)
			{
				m_Renderer.AddHittable(mesh.GetBvhRoot());
				m_Renderer.m_TriangleCount += static_cast<uint32_t>(mesh.GetTriangleCount());
				if (auto bvh = mesh.GetBvhNode())
					bvh->GetStats(m_Renderer.m_BvhNodeCount, m_Renderer.m_BvhMaxDepth);
				loadedAny = true;
			}
		}

		if (loadedAny)
		{
			if (m_RendererGPU.IsAvailable())
				UploadMeshToGPU();

			const auto& cam = m_SceneMgr.GetScene().GetCamera();
			m_Cam.SetPosition(cam.position);
			m_Cam.SetForwardDirection(cam.forwardDirection);
			m_Renderer.mFrameIndex = 1;
		}
		else
		{
			printf("[Scene] No meshes loaded!\n");
		}
	}

	SceneManager::EntityId LoadMeshFileAsEntity(const std::string& filepath)
	{
		// Load OBJ file as geometry, then add to ECS via SceneManager
		Mesh mesh;
		auto material = make_shared<LambertianMaterial>(vec3(0.7f, 0.7f, 0.7f));
		if (!mesh.Load(filepath, {0, 0, 0}, {0, 0, 0}, 1.0f, material))
		{
			printf("[SceneEditor] Failed to load mesh: %s\n", filepath.c_str());
			return SceneManager::EntityId{};
		}

		auto [verts, indices] = mesh.GetRawVertexData();

		MeshData meshData;
		meshData.vertices = verts;
		meshData.indices = indices;
		meshData.name = filepath;

		// Extract a name from the filepath
		std::string name = filepath;
		size_t lastSlash = name.find_last_of("/\\");
		if (lastSlash != std::string::npos)
			name = name.substr(lastSlash + 1);
		size_t lastDot = name.find_last_of('.');
		if (lastDot != std::string::npos)
			name = name.substr(0, lastDot);

		return m_SceneMgr.AddObjectWithGeometry(name, std::move(meshData), {0, 0.5f, 0});
	}

	void BuildSceneFromCurrentState()
	{
		auto& scene = m_SceneMgr.GetScene();
		scene.Clear();

		SceneMesh mesh;
		if (m_Mesh.IsLoaded())
		{
			auto meshData = m_Mesh.GetRawVertexData();
			if (!m_MeshPath.empty())
			{
				mesh.filepath = m_MeshPath;
			}
			else
			{
				mesh.hasGeometry = true;
				mesh.vertices = meshData.first;
				mesh.indices = meshData.second;
			}
			mesh.position = m_MeshPosition;
			mesh.rotation = m_MeshRotation;
			mesh.scale = m_MeshScale;
		}
		scene.AddMesh(mesh);

		SceneMaterial mat;
		mat.type = static_cast<MaterialType>(m_MaterialType);
		mat.baseColor = m_MeshAlbedo;
		mat.ior = m_MeshIOR;
		scene.AddMaterial(mat);

		SceneCamera cam;
		cam.position = m_Cam.GetPosition();
		cam.forwardDirection = m_Cam.GetDirection();
		cam.verticalFOV = 45.0f;
		cam.aperture = m_Cam.m_Aperture;
		cam.focusDistance = m_Cam.m_FocusDistance;
		scene.GetCamera() = cam;
	}
	Renderer m_Renderer;
	RendererGPU m_RendererGPU;
	RenderSettings m_Settings; // local copy — mutated by UI, synced via ApplySettings
	SceneManager m_SceneMgr;
	SceneEditorUI m_EditorUI;
	int m_UseGPU = 1; // 0=CPU, 1=GPU
	uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;
	uint32_t* m_ImageData = nullptr;
	float m_LastRenderTime = 0.0f;
	float m_SmoothedFrameTime = 0.0f;
	float m_SmoothedFPS = 0.0f;
	bool m_RenderOnUpdate;
	bool m_PendingFullSync = false;  // true when next SceneChanged should use SetScene (full) not KeepTextures
	Camera m_Cam;

	// Mesh UI state (OBJ path — kept in RT2Layer for UI binding)
	Mesh m_Mesh;
	std::string m_MeshPath;
	vec3 m_MeshPosition = vec3(0.0f, 0.0f, 0.0f);
	vec3 m_MeshRotation = vec3(0.0f, 0.0f, 0.0f);
	float m_MeshScale = 1.0f;
	shared_ptr<Material> m_MeshMaterial;
	int m_MaterialType = 0;
	vec3 m_MeshAlbedo = vec3(0.7f, 0.7f, 0.7f);
	float m_MeshFuzz = 0.0f;
	float m_MeshIOR = 1.5f;

	bool m_CLIProcessed = false;

	void LoadEnvMap(const std::string& filepath)
	{
		if (!m_SceneMgr.LoadEnvMap(filepath))
			return;
		if (m_UseGPU && m_RendererGPU.IsAvailable() && m_SceneMgr.GetECS().meshRegistry.GetCount() > 0)
			m_SceneMgr.SyncToGPU();
		m_RendererGPU.ResetAccumulation();
	}
};

Walnut::Application* Walnut::CreateApplication(int argc, char** argv)
{
	// Parse CLI args (stored in global g_CLI for RT2Layer to consume)
	g_CLI = CLIArgs::Parse(argc, argv);

	Walnut::ApplicationSpecification spec;
	spec.Name = "RT2";
	spec.EnableValidation = g_CLI.validate;
	spec.EnableSyncValidation = g_CLI.syncValidate;

	Walnut::Application* app = new Walnut::Application(spec);
	app->PushLayer<RT2Layer>();
	app->SetMenubarCallback([app]()
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("Exit"))
			{
				app->Close();
			}
			ImGui::EndMenu();
		}
	});
	return app;
}