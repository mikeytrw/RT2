#include "Walnut/Application.h"
#include "Walnut/EntryPoint.h"

#include "Walnut/Image.h"
#include "Walnut/Timer.h"
#include "Renderer.h"
#include "RendererGPU.h"
#include "Mesh.h"
#include "FileDialog.h"

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
		auto image = m_Renderer.GetFinalImage();

		ImGui::Begin("Info");
		ImGui::Text("Last Render: %.3fms", m_LastRenderTime);
		ImGui::Text("Rays Cast: %d", m_Renderer.m_NumRaysCast);
		ImGui::Text("FPS: %.1f",1000/m_LastRenderTime);

		float raysPerSec = m_Renderer.m_NumRaysCast / (m_LastRenderTime / 1000);

		ImGui::Text("Rays/Sec: %.1f", raysPerSec);
		
		if(image)
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
						if (m_Mesh.IsLoaded())
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
					if (m_Mesh.IsLoaded())
						UploadMeshToGPU();
				}
			}
		}
		if (ImGui::BeginPopupModal("GPU Init Failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Failed to initialize GPU renderer.\nCheck that pathtracer.spv exists next to the executable.\nSee console for details.");
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

		//ImGui::ShowDemoWindow();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Viewport");

		m_ViewportWidth = ImGui::GetContentRegionAvail().x;
		m_ViewportHeight = ImGui::GetContentRegionAvail().y;

		if (m_UseGPU && m_RendererGPU.HasOutput())
			ImGui::Image((ImTextureID)m_RendererGPU.GetOutputDescriptorSet(),
			             { (float)m_RendererGPU.GetWidth(), (float)m_RendererGPU.GetHeight() });
		else if (image)
			ImGui::Image(image->GetDescriptorSet(), { (float)image->GetWidth(),(float)image->GetHeight() });

		ImGui::End();
		ImGui::PopStyleVar();

		if (m_RenderOnUpdate) {
			if (m_UseGPU)
			{
				m_RendererGPU.OnResize(m_ViewportWidth, m_ViewportHeight);
				m_Cam.OnResize(m_ViewportWidth, m_ViewportHeight);
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
			m_RendererGPU.OnResize(m_ViewportWidth, m_ViewportHeight);
			m_Cam.OnResize(m_ViewportWidth, m_ViewportHeight);
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
		if (!m_Mesh.IsLoaded()) return;

		GPUMeshData gpuData;
		gpuData.materialType = m_MaterialType;
		gpuData.albedo = m_MeshAlbedo;
		gpuData.fuzz = m_MeshFuzz;
		gpuData.ior = m_MeshIOR;
		gpuData.position = m_MeshPosition;
		gpuData.rotation = m_MeshRotation;
		gpuData.scale = m_MeshScale;

		// Extract vertex/index data from the loaded mesh's triangles
		const auto& triangles = m_Mesh.GetBvhRoot();
		// We need the raw triangle data - let's get it from the Mesh's HittableList
		// Since we don't have direct access, we'll extract from the tinyobj data via Mesh
		// For now, use a simpler approach: extract from Triangle objects
		// We need to flatten the BVH leaves - but that's complex.
		// Simpler: add a method to Mesh to get raw vertex/index data

		// Let's get the data from Mesh directly
		auto meshData = m_Mesh.GetRawVertexData();
		gpuData.vertices = meshData.first;
		gpuData.indices = meshData.second;

		m_RendererGPU.SetMesh(gpuData);
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
	std::string m_MeshPath;
	vec3 m_MeshPosition = vec3(0.0f, 0.0f, 0.0f);
	vec3 m_MeshRotation = vec3(0.0f, 0.0f, 0.0f);
	float m_MeshScale = 1.0f;
	shared_ptr<Material> m_MeshMaterial;
	int m_MaterialType = 0;
	vec3 m_MeshAlbedo = vec3(0.7f, 0.7f, 0.7f);
	float m_MeshFuzz = 0.0f;
	float m_MeshIOR = 1.5f;
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