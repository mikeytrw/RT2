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
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <tinyexr.h>
#include "NRD.h"

#include <cstdio>
#include <thread>
#include <chrono>

using namespace Walnut;

static CLIArgs g_CLI;

class RT2Layer : public Walnut::Layer
{
public:

	RT2Layer()
	{
		m_Cam = Camera(45.0f, 0.1f, 10000.0f, 0.005f, 2.5f);
		m_RenderOnUpdate = true;
		m_Cam.m_Aperture = 0.0f;

		m_EditorUI.SetSceneMgr(&m_SceneMgr);
		m_EditorUI.SetOnSceneChanged([this]() {
			if (m_RendererGPU.IsAvailable())
			{
				m_SceneMgr.CompactMeshRegistry();
				if (m_PendingFullSync)
				{
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
			if (m_RendererGPU.IsAvailable())
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
			std::string ext = path.substr(path.find_last_of('.') + 1);
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			if (ext == "obj")
			{
				LoadScene(path);
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
			static const char* timingNames[] = { "GPU Frame", "Raster", "ReSTIR Temporal", "ReSTIR Spatial", "RT Shading", "NRD", "Compose" };
			ImGui::Text("GPU timings (frame %llu)", static_cast<unsigned long long>(timings.frameIndex));
			for (uint32_t i = 0; i < static_cast<uint32_t>(GpuTimestampProfiler::Region::Count); i++)
				if (timings.validMask & (1u << i))
					ImGui::Text("  %s: %.3f ms", timingNames[i], timings.milliseconds[i]);
		}
	}

	if (m_RendererGPU.HasOutput())
		ImGui::Text("Render Res: %d x %d", m_RendererGPU.GetWidth(), m_RendererGPU.GetHeight());
	ImGui::Separator();

	if (ImGui::Button("Render")) {
		Render();
	};

	ImGui::Checkbox("Render on Update", &m_RenderOnUpdate);

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
	if (ImGui::DragInt("SPP", &m_Settings.spp, 1.0f, 1, 1500))
		m_RendererGPU.ApplySettings(m_Settings);
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
		ImGui::BeginDisabled(m_Settings.restirEnabled);
		if (ImGui::Checkbox("Jitter", &m_Settings.nrdJitterEnabled))
			m_RendererGPU.ApplySettings(m_Settings);
		ImGui::EndDisabled();
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
		m_RendererGPU.ApplySettings(m_Settings);
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
	ImGui::Text("G-buffer Debug");
	const char* gbufferModes[] = {
		"Off", "Shading Normal", "Roughness", "ViewZ (depth)",
		"Motion Vectors", "Albedo", "F0", "Direct Emission",
		"World Position", "Geo Normal", "UV", "Material Index",
		"ReSTIR Reservoir"
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
		std::string path = FileDialog::OpenFile("HDR Files (*.hdr;*.exr)\0*.hdr;*.exr\0HDR (*.hdr)\0*.hdr\0All Files (*.*)\0*.*\0");
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
	ImGui::Separator();
	if (ImGui::Button("Load Scene..."))
	{
		std::string path = FileDialog::OpenFile("glTF Binary (*.glb)\0*.glb\0glTF JSON (*.gltf)\0*.gltf\0OBJ Files (*.obj)\0*.obj\0All Files (*.*)\0*.*\0");
		if (!path.empty())
			LoadScene(path);
	}
	ImGui::SameLine();
	if (ImGui::Button("Save Scene..."))
	{
		std::string path = FileDialog::SaveFile("glTF Binary (*.glb)\0*.glb\0glTF JSON (*.gltf)\0*.gltf\0All Files (*.*)\0*.*\0");
		if (!path.empty())
			SceneLoader::Save(m_SceneMgr.GetECS(), path);
	}
	ImGui::End();

	m_EditorUI.RenderPanels();

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("Viewport");

	m_ViewportWidth = (uint32_t)ImGui::GetContentRegionAvail().x;
	m_ViewportHeight = (uint32_t)ImGui::GetContentRegionAvail().y;

	if (m_RendererGPU.IsAvailable())
	{
		m_RendererGPU.OnResize(m_ViewportWidth, m_ViewportHeight);
		m_Cam.OnResize(m_ViewportWidth, m_ViewportHeight);
	}

	if (m_RendererGPU.HasOutput())
		ImGui::Image((ImTextureID)m_RendererGPU.GetOutputDescriptorSet(),
		             { (float)m_RendererGPU.GetWidth(), (float)m_RendererGPU.GetHeight() });

	ImGui::End();
	ImGui::PopStyleVar();
	}

	virtual void OnUpdate(float ts) override
	{
		m_Cam.OnUpdate(ts);

		if (!m_CLIProcessed) return;

		if (m_RenderOnUpdate) {
			Render();
		}
	}

private:

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
		if (g_CLI.rasterFirst)
		{
			m_Settings.rasterFirst = true;
			m_Cam.m_Aperture = 0.0f;
		}
		if (g_CLI.gbufferDebug >= 0)
			m_Settings.gbufferDebugMode = g_CLI.gbufferDebug;

		if (Walnut::Application::IsRayTracingSupported() && !m_RendererGPU.IsAvailable())
		{
			if (m_RendererGPU.Init())
			{
				m_Settings = m_RendererGPU.GetSettings();
				if (g_CLI.spp > 0) m_Settings.spp = g_CLI.spp;
				if (g_CLI.bounces > 0) m_Settings.maxBounces = g_CLI.bounces;
				if (g_CLI.nrd) m_Settings.nrdEnabled = true;
				if (g_CLI.rasterFirst) m_Settings.rasterFirst = true;
				if (g_CLI.ris || g_CLI.restir) { m_Settings.restirEnabled = true; m_Settings.rasterFirst = true; }
				if (g_CLI.restirCandidates > 0) m_Settings.restirFreshCandidates = (uint32_t)g_CLI.restirCandidates;
				if (g_CLI.gbufferDebug >= 0) m_Settings.gbufferDebugMode = g_CLI.gbufferDebug;
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
			LoadScene(g_CLI.scenePath);

		if (g_CLI.rasterFirst)
			m_Cam.m_Aperture = 0.0f;

		if (g_CLI.headless)
			RunHeadless();
	}

	void RunHeadless()
	{
		printf("[Headless] starting: %d frames at %dx%d\n", g_CLI.frames, g_CLI.width, g_CLI.height);

		m_ViewportWidth = (uint32_t)g_CLI.width;
		m_ViewportHeight = (uint32_t)g_CLI.height;
		if (m_RendererGPU.IsAvailable())
		{
			m_RendererGPU.OnResize(m_ViewportWidth, m_ViewportHeight);
			m_Cam.OnResize(m_ViewportWidth, m_ViewportHeight);
		}

		if (m_RendererGPU.IsAvailable())
		{
			while (m_RendererGPU.IsTextureUploadPending())
			{
				m_RendererGPU.PollTextureUpload();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		for (int i = 0; i < g_CLI.frames; i++)
		{
			Timer timer;
			if (m_RendererGPU.IsAvailable())
				m_RendererGPU.Render(m_Cam);
			float ms = timer.ElapsedMillis();
			if (g_CLI.verbose || i == g_CLI.frames - 1)
				printf("[Headless] frame %d/%d: %.1fms\n", i + 1, g_CLI.frames, ms);
		}

		if (g_CLI.hasOutput() && m_RendererGPU.IsAvailable())
		{
			std::vector<uint8_t> pixels;
			uint32_t w, h;
			if (m_RendererGPU.ReadbackOutput(pixels, w, h))
			{
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

		printf("[Headless] done, exiting\n");
		Walnut::Application::Get().Close();
	}

	void Render() {
		Timer timer;

		if (m_RendererGPU.IsAvailable())
			m_RendererGPU.Render(m_Cam);

		m_LastRenderTime = timer.ElapsedMillis();

		float alpha = m_LastRenderTime / (m_LastRenderTime + 500.0f);
		m_SmoothedFrameTime = m_SmoothedFrameTime * (1.0f - alpha) + m_LastRenderTime * alpha;
		m_SmoothedFPS = m_SmoothedFrameTime > 0.0f ? 1000.0f / m_SmoothedFrameTime : 0.0f;
	}

	void UploadMeshToGPU()
	{
		if (m_SceneMgr.GetECS().meshRegistry.GetCount() == 0) return;

		m_SceneMgr.SetSyncCallback([this](const GPUSceneData& gpuData) {
			m_RendererGPU.SetScene(gpuData);
		});
		m_SceneMgr.SyncToGPU();
	}

	void LoadScene(const std::string& filepath)
	{
		if (!m_SceneMgr.LoadScene(filepath))
		{
			ImGui::OpenPopup("Scene Load Failed");
			return;
		}

		std::string ext = filepath.substr(filepath.find_last_of('.') + 1);
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		if (ext == "obj")
		{
			m_SceneMgr.SetSyncCallback([this](const GPUSceneData& gpuData) {
				m_RendererGPU.SetScene(gpuData);
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
	}

	SceneManager::EntityId LoadMeshFileAsEntity(const std::string& filepath)
	{
		std::string ext = filepath.substr(filepath.find_last_of('.') + 1);
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == "obj")
		{
			LoadScene(filepath);
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
	uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;
	float m_LastRenderTime = 0.0f;
	float m_SmoothedFrameTime = 0.0f;
	float m_SmoothedFPS = 0.0f;
	bool m_RenderOnUpdate = true;
	bool m_PendingFullSync = false;
	Camera m_Cam;
	bool m_CLIProcessed = false;

	void LoadEnvMap(const std::string& filepath)
	{
		if (!m_SceneMgr.LoadEnvMap(filepath))
			return;
		if (m_RendererGPU.IsAvailable() && m_SceneMgr.GetECS().meshRegistry.GetCount() > 0)
			m_SceneMgr.SyncToGPU();
		m_RendererGPU.ResetAccumulation();
	}
};

Walnut::Application* Walnut::CreateApplication(int argc, char** argv)
{
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