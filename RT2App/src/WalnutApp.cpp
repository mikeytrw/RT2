#include "Walnut/Application.h"
#include "Walnut/EntryPoint.h"

#include "Walnut/Image.h"
#include "Walnut/Timer.h"
#include "Renderer.h"

using namespace Walnut;
 
class ExampleLayer : public Walnut::Layer
{
public:

	ExampleLayer() 
	{

		m_Cam = Camera(45.0f, 0.1f, 100.0f,0.5f,3.0f);
		m_Renderer = Renderer();
		m_RenderOnUpdate = true;


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
		
		if (ImGui::Button("Render")) {
			Render();
		};

		ImGui::Checkbox("Render on Update", &m_RenderOnUpdate);

		
		ImGui::Separator();
		ImGui::Text("Samples Per Pixel");
		ImGui::DragInt("SPP", & m_Renderer.m_SamplesPerPixel, 1.0f, 1, 1500);
		ImGui::Text("Sample Depth");
		ImGui::DragInt("Bounces", &m_Renderer.m_MaxBounceDepth, 1.0f, 2, 100);
		if (ImGui::Button("Reset")) {
			m_Renderer.m_SamplesPerPixel = 1;
			m_Renderer.m_MaxBounceDepth = 2;
		};

		ImGui::End();

		//ImGui::ShowDemoWindow();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Viewport");

		m_ViewportWidth = ImGui::GetContentRegionAvail().x;
		m_ViewportHeight = ImGui::GetContentRegionAvail().y;

		if(image)
			ImGui::Image(image->GetDescriptorSet(), { (float)image->GetWidth(),(float)image->GetHeight() });

		ImGui::End();
		ImGui::PopStyleVar();

		if (m_RenderOnUpdate) {
			m_Renderer.setTemporalAccumulation(true);
			Render();
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

		m_Renderer.OnResize(m_ViewportWidth, m_ViewportHeight);
		m_Cam.OnResize(m_ViewportWidth, m_ViewportHeight);
		m_Renderer.Render(m_Cam);
		m_LastRenderTime = timer.ElapsedMillis();

		return;
	}
	
	Renderer m_Renderer;
	uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;
	uint32_t* m_ImageData = nullptr;
	float m_LastRenderTime = 0.0f;
	bool m_RenderOnUpdate;
	Camera m_Cam;
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