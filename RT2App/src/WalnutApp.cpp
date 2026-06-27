#include "Walnut/Application.h"
#include "Walnut/EntryPoint.h"

#include "Walnut/Image.h"
#include "Walnut/Timer.h"
#include "Renderer.h"
#include "RendererGPU.h"
#include "Mesh.h"
#include "FileDialog.h"
#include "Scene.h"
#include "SceneLoader.h"
#include "GPUSceneData.h"

#include <cstdio>

using namespace Walnut;
 
class ExampleLayer : public Walnut::Layer
{
public:

		ExampleLayer() 
		{

			m_Cam = Camera(45.0f, 0.1f, 100.0f,0.005f,2.5f);
			m_Renderer = Renderer();
			m_RenderOnUpdate = false; 

			m_MeshMaterial = make_shared<LambertianMaterial>(vec3(0.7f, 0.7f, 0.7f));

		}

	virtual void OnUIRender() override
	{
		ImGui::Begin("Info");
		ImGui::Text("Last Render: %.3fms", m_LastRenderTime);
		ImGui::Text("Rays Cast: %d", m_Renderer.m_NumRaysCast);
		ImGui::Text("FPS: %.1f",1000/m_LastRenderTime);

		float raysPerSec = m_Renderer.m_NumRaysCast / (m_LastRenderTime / 1000);

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
						m_RendererGPU.m_SPP = m_Renderer.m_SamplesPerPixel;
						m_RendererGPU.m_MaxBounces = m_Renderer.m_MaxBounceDepth;
						if (!m_SceneMeshes.empty() || m_Mesh.IsLoaded())
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
					m_RendererGPU.m_SPP = m_Renderer.m_SamplesPerPixel;
					m_RendererGPU.m_MaxBounces = m_Renderer.m_MaxBounceDepth;
					if (!m_SceneMeshes.empty() || m_Mesh.IsLoaded())
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
		m_RendererGPU.m_SPP = m_Renderer.m_SamplesPerPixel;
		m_RendererGPU.ResetAccumulation();
	}
	ImGui::Text("Sample Depth");
	if (ImGui::DragInt("Bounces", &m_Renderer.m_MaxBounceDepth, 1.0f, 2, 100))
	{
		m_RendererGPU.m_MaxBounces = m_Renderer.m_MaxBounceDepth;
		m_RendererGPU.ResetAccumulation();
	}
	if (ImGui::Button("Reset")) {
		m_Renderer.m_SamplesPerPixel = 1;
		m_Renderer.m_MaxBounceDepth = 2;
		m_RendererGPU.m_SPP = 1;
		m_RendererGPU.m_MaxBounces = 2;
		m_RendererGPU.ResetAccumulation();
	};

	ImGui::Separator();
	ImGui::Text("Camera");
	if (ImGui::DragFloat("Aperture", &m_Cam.m_Aperture, 0.001f, 0.0f, 5.0f))
		m_RendererGPU.ResetAccumulation();
	if (ImGui::DragFloat("Focus Distance", &m_Cam.m_FocusDistance, 0.1f, 0.1f, 50.0f))
		m_RendererGPU.ResetAccumulation();
	ImGui::Separator();
	if (ImGui::Checkbox("Show Background", &m_RendererGPU.m_ShowBackground))
		m_RendererGPU.ResetAccumulation();
	if (ImGui::SliderFloat("Emissive Boost", &m_RendererGPU.m_EmissiveBoost, 0.0f, 50.0f, "%.1f"))
		m_RendererGPU.ResetAccumulation();
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
		ImGui::Text("Meshes: %d", (int)m_Scene.GetMeshes().size());
		ImGui::Text("Materials: %d", (int)m_Scene.GetMaterials().size());
		ImGui::Text("Lights: %d", (int)m_Scene.GetLights().size());
		ImGui::Text("Textures: %d", (int)m_Scene.GetTextures().size());
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
				SceneLoader::Save(m_Scene, path);
			}
		}
		ImGui::Separator();
		if (!m_Scene.GetMeshes().empty())
		{
			ImGui::Text("Loaded meshes:");
			for (int i = 0; i < (int)m_Scene.GetMeshes().size(); i++)
			{
				const auto& m = m_Scene.GetMesh(i);
				if (m.HasGeometry())
					ImGui::Text("  [%d] geometry (%d verts, %d tris)",
					            i, (int)(m.vertices.size() / 3), (int)(m.indices.size() / 3));
				else
					ImGui::Text("  [%d] %s", i, m.filepath.c_str());
			}
		}
		ImGui::End();

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
		if (m_SceneMeshes.empty() && !m_Mesh.IsLoaded()) return;

		// Build GPUSceneData from the Scene (per-mesh materials from glTF)
		GPUSceneData gpuData = BuildGPUSceneData(m_Scene);

		// Fallback: if no scene meshes from glTF, use the single OBJ mesh
		if (gpuData.meshes.empty() && m_Mesh.IsLoaded())
		{
			auto [verts, indices] = m_Mesh.GetRawVertexData();
			GPUMeshGeometry geo;
			geo.vertices = verts;
			geo.indices = indices;
			geo.materialIndex = 0;
			gpuData.meshes.push_back(std::move(geo));

			// Single default material from UI controls
			SceneMaterial sm;
			sm.baseColor = m_MeshAlbedo;
			sm.metallic = (m_MaterialType == 1) ? 1.0f : 0.0f;
			sm.roughness = m_MeshFuzz;
			sm.ior = m_MeshIOR;
			gpuData.materials.push_back(GPUMaterial::fromSceneMaterial(sm));
		}

		printf("[Scene] GPU upload: %d meshes, %d materials\n",
		       (int)gpuData.meshes.size(), (int)gpuData.materials.size());

		m_RendererGPU.SetScene(gpuData);
	}

	void LoadScene(const std::string& filepath)
	{
		printf("[Scene] LoadScene: '%s'\n", filepath.c_str());
		if (!SceneLoader::Load(m_Scene, filepath))
		{
			printf("[Scene] SceneLoader::Load failed!\n");
			ImGui::OpenPopup("Scene Load Failed");
			return;
		}
		printf("[Scene] SceneLoader::Load succeeded\n");

		printf("[Scene] Loaded %d meshes, %d materials, %d lights, %d textures\n",
		       (int)m_Scene.GetMeshes().size(), (int)m_Scene.GetMaterials().size(),
		       (int)m_Scene.GetLights().size(), (int)m_Scene.GetTextures().size());

		m_Renderer.ClearHittables();
		m_Renderer.m_TriangleCount = 0;
		m_Renderer.m_BvhNodeCount = 0;
		m_Renderer.m_BvhMaxDepth = 0;
		m_Mesh.Clear();
		for (auto& m : m_SceneMeshes) m.Clear();
		m_SceneMeshes.clear();

		bool loadedAny = false;
		int meshIdx = 0;

		for (const auto& sceneMesh : m_Scene.GetMeshes())
		{
			// printf("[Scene] Mesh %d: hasGeometry=%d, filepath='%s', pos=(%.1f,%.1f,%.1f), scale=%.3f, verts=%d, indices=%d\n",
			//        meshIdx, sceneMesh.HasGeometry(), sceneMesh.filepath.c_str(),
			//        sceneMesh.position.x, sceneMesh.position.y, sceneMesh.position.z, sceneMesh.scale,
			//        (int)(sceneMesh.vertices.size() / 3), (int)sceneMesh.indices.size());

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
				m_SceneMeshes.push_back(std::move(mesh));
				// printf("[Scene]   Loaded: %d triangles\n", (int)m_SceneMeshes.back().GetTriangleCount());
			}
			else
			{
				printf("[Scene]   Failed to load\n");
			}
			meshIdx++;
		}

		// printf("[Scene] Total triangles: %d, BVH nodes: %d, BVH depth: %d\n",
		//        m_Renderer.m_TriangleCount, m_Renderer.m_BvhNodeCount, m_Renderer.m_BvhMaxDepth);

		if (loadedAny)
		{
			if (m_RendererGPU.IsAvailable())
				UploadMeshToGPU();

			const auto& cam = m_Scene.GetCamera();
			printf("[Scene] Camera: pos=(%.1f,%.1f,%.1f), forward=(%.1f,%.1f,%.1f), fov=%.1f\n",
			       cam.position.x, cam.position.y, cam.position.z,
			       cam.forwardDirection.x, cam.forwardDirection.y, cam.forwardDirection.z,
			       cam.verticalFOV);
			m_Cam.SetPosition(cam.position);
			m_Cam.SetForwardDirection(cam.forwardDirection);

			m_Renderer.mFrameIndex = 1;
		}
		else
		{
			printf("[Scene] No meshes loaded!\n");
		}
	}

	void BuildSceneFromCurrentState()
	{
		m_Scene.Clear();

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
		m_Scene.AddMesh(mesh);

		SceneMaterial mat;
		mat.type = static_cast<MaterialType>(m_MaterialType);
		mat.baseColor = m_MeshAlbedo;
		mat.ior = m_MeshIOR;
		m_Scene.AddMaterial(mat);

		SceneCamera cam;
		cam.position = m_Cam.GetPosition();
		cam.forwardDirection = m_Cam.GetDirection();
		cam.verticalFOV = 45.0f;
		cam.aperture = m_Cam.m_Aperture;
		cam.focusDistance = m_Cam.m_FocusDistance;
		m_Scene.GetCamera() = cam;
	}
	Renderer m_Renderer;
	RendererGPU m_RendererGPU;
	int m_UseGPU = 0; // 0=CPU, 1=GPU
	uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;
	uint32_t* m_ImageData = nullptr;
	float m_LastRenderTime = 0.0f;
	bool m_RenderOnUpdate;
	Camera m_Cam;

	Mesh m_Mesh;
	std::vector<Mesh> m_SceneMeshes;  // one per scene mesh when loaded via Scene panel
	std::string m_MeshPath;
	vec3 m_MeshPosition = vec3(0.0f, 0.0f, 0.0f);
	vec3 m_MeshRotation = vec3(0.0f, 0.0f, 0.0f);
	float m_MeshScale = 1.0f;
	shared_ptr<Material> m_MeshMaterial;
	int m_MaterialType = 0;
	vec3 m_MeshAlbedo = vec3(0.7f, 0.7f, 0.7f);
	float m_MeshFuzz = 0.0f;
	float m_MeshIOR = 1.5f;

	Scene m_Scene;
};

Walnut::Application* Walnut::CreateApplication(int argc, char** argv)
{
	Walnut::ApplicationSpecification spec;
	spec.Name = "RT2";

	Walnut::Application* app = new Walnut::Application(spec);
	app->PushLayer<ExampleLayer>();
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