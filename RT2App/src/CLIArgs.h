#pragma once

#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstdlib>

struct CLIArgs
{
	std::string scenePath;
	std::string projectPath;
	std::string envMapPath;
	std::string outputPath;       // screenshot PNG path
	std::string outputHDRPath;    // linear HDR output (.exr or .pfm)
	std::string rrGuideReport;    // checked native RR-neutral guide readback JSON
	int frames = 0;               // number of frames to render before screenshot (0 = no auto-screenshot)
	int width = 1280;             // viewport width for headless mode
	int height = 720;             // viewport height for headless mode
	int spp = 0;                  // SPP override (0 = use default)
	int bounces = 0;              // bounces override (0 = use default)
	bool nrd = false;             // enable NRD
	int nrdMaxAccumFrames = 0;    // NRD history override (0 = default)
	float nrdResponsiveRoughness = -1.0f; // responsive threshold override (<0 = default)
	int nrdResponsiveMinFrames = -1;      // responsive minimum history override (<0 = default)
	bool noAccumulate = false;    // disable non-NRD temporal accumulation
	bool headless = false;        // render N frames, save screenshot, exit
	bool listScenes = false;      // just print what would be loaded
	bool verbose = false;
	bool validate = false;        // enable Vulkan validation layers
	bool syncValidate = false;    // enable synchronization validation
	bool ngxReport = false;       // print the read-only NGX support snapshot
	std::string ngxProjectId;     // optional override; empty is a deliberate invalid-ID test
	std::string ngxFeaturePath;   // optional isolated NGX runtime search path
	bool benchmarkTimings = false; // emit one JSON timing record per completed GPU frame
	bool rasterFirst = false;      // raster-first hybrid path
	bool ris = false;              // enable ReSTIR DI (backward compat alias)
	bool restir = false;           // enable ReSTIR DI
	int restirCandidates = 0;     // ReSTIR fresh candidate count override (0 = use default)
	bool restirNoTemporal = false;
	bool restirNoSpatial = false;
	bool restirGI = false;         // enable ReSTIR GI independently of DI
	int restirGICandidates = 0;   // GI fresh candidate count override (0 = use default)
	int gbufferDebug = -1;        // G-buffer debug view mode (-1 = off)
	bool hasCameraPosition = false;
	bool hasCameraForward = false;
	float cameraPosition[3] = {};
	float cameraForward[3] = {};
	float cameraSweepAmplitude = 0.0f; // world-space left/right amplitude
	int cameraSweepMode = 0;           // 0=lateral, 1=forward, 2=yaw (amplitude=radians)
	int cameraSweepWarmup = 0;         // stationary frames before motion
	int cameraSweepPeriod = 32;        // frames per complete left/right cycle
	int captureEvery = 0;              // save sequence frame every N frames
	int cameraSweepCycles = 0;         // complete cycles before holding still (0 = repeat)
	uint32_t sceneSeed = 0;            // deterministic sampling seed

	bool hasScene() const { return !scenePath.empty(); }
	bool hasProject() const { return !projectPath.empty(); }
	bool hasEnvMap() const { return !envMapPath.empty(); }
	bool hasOutput() const { return !outputPath.empty() || !outputHDRPath.empty(); }

	static CLIArgs Parse(int argc, char** argv)
	{
		CLIArgs args;
		args.ngxProjectId = "41f7cbd8-e97e-49f0-b41d-14a4f5b547f8";
		for (int i = 1; i < argc; i++)
		{
			const char* a = argv[i];
			auto next = [&]() -> const char* {
				if (i + 1 < argc) return argv[++i];
				return nullptr;
			};

			if (strcmp(a, "--scene") == 0 || strcmp(a, "-s") == 0)
			{
				if (const char* v = next()) args.scenePath = v;
			}
			else if (strcmp(a, "--project") == 0 || strcmp(a, "-p") == 0)
			{
				if (const char* v = next()) args.projectPath = v;
			}
			else if (strcmp(a, "--env") == 0 || strcmp(a, "-e") == 0)
			{
				if (const char* v = next()) args.envMapPath = v;
			}
			else if (strcmp(a, "--output") == 0 || strcmp(a, "-o") == 0)
			{
				if (const char* v = next()) args.outputPath = v;
			}
			else if (strcmp(a, "--output-hdr") == 0)
			{
				if (const char* v = next()) args.outputHDRPath = v;
			}
			else if (strcmp(a, "--rr-guide-report") == 0)
			{
				if (const char* v = next()) args.rrGuideReport = v;
			}
			else if (strcmp(a, "--frames") == 0 || strcmp(a, "-f") == 0)
			{
				if (const char* v = next()) args.frames = std::max(1, std::atoi(v));
			}
			else if (strcmp(a, "--width") == 0 || strcmp(a, "-w") == 0)
			{
				if (const char* v = next()) args.width = std::max(1, std::atoi(v));
			}
			else if (strcmp(a, "--height") == 0 || strcmp(a, "-h") == 0)
			{
				if (const char* v = next()) args.height = std::max(1, std::atoi(v));
			}
			else if (strcmp(a, "--spp") == 0)
			{
				if (const char* v = next()) args.spp = std::max(1, std::atoi(v));
			}
			else if (strcmp(a, "--bounces") == 0)
			{
				if (const char* v = next()) args.bounces = std::max(1, std::atoi(v));
			}
			else if (strcmp(a, "--nrd") == 0)
			{
				args.nrd = true;
			}
			else if (strcmp(a, "--nrd-accum-frames") == 0)
			{
				if (const char* v = next()) args.nrdMaxAccumFrames = std::max(1, std::atoi(v));
			}
			else if (strcmp(a, "--nrd-responsive-roughness") == 0)
			{
				if (const char* v = next()) args.nrdResponsiveRoughness = (float)std::atof(v);
			}
			else if (strcmp(a, "--nrd-responsive-min-frames") == 0)
			{
				if (const char* v = next()) args.nrdResponsiveMinFrames = std::max(0, std::atoi(v));
			}
			else if (strcmp(a, "--no-accumulate") == 0)
			{
				args.noAccumulate = true;
			}
		else if (strcmp(a, "--gbuffer-debug") == 0)
		{
			if (const char* v = next()) args.gbufferDebug = std::atoi(v);
		}
		else if (strcmp(a, "--camera-pos") == 0)
		{
			for (int c = 0; c < 3; c++)
				if (const char* v = next()) args.cameraPosition[c] = (float)std::atof(v);
			args.hasCameraPosition = true;
		}
		else if (strcmp(a, "--camera-forward") == 0)
		{
			for (int c = 0; c < 3; c++)
				if (const char* v = next()) args.cameraForward[c] = (float)std::atof(v);
			args.hasCameraForward = true;
		}
		else if (strcmp(a, "--camera-sweep") == 0)
		{
			if (const char* v = next()) args.cameraSweepAmplitude = (float)std::atof(v);
			if (const char* v = next()) args.cameraSweepWarmup = std::max(0, std::atoi(v));
			if (const char* v = next()) args.cameraSweepPeriod = std::max(4, std::atoi(v));
		}
		else if (strcmp(a, "--camera-sweep-mode") == 0)
		{
			if (const char* v = next())
			{
				if (strcmp(v, "forward") == 0) args.cameraSweepMode = 1;
				else if (strcmp(v, "yaw") == 0) args.cameraSweepMode = 2;
				else args.cameraSweepMode = 0;
			}
		}
		else if (strcmp(a, "--capture-every") == 0)
		{
			if (const char* v = next()) args.captureEvery = std::max(1, std::atoi(v));
		}
		else if (strcmp(a, "--camera-sweep-cycles") == 0)
		{
			if (const char* v = next()) args.cameraSweepCycles = std::max(0, std::atoi(v));
		}
		else if (strcmp(a, "--seed") == 0)
		{
			if (const char* v = next()) args.sceneSeed = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
		}
		else if (strcmp(a, "--raster-first") == 0)
		{
			args.rasterFirst = true;
		}
		else if (strcmp(a, "--ris") == 0)
		{
			args.ris = true;  // backward compat
			args.restir = true;
		}
		else if (strcmp(a, "--restir") == 0)
		{
			args.restir = true;
		}
		else if (strcmp(a, "--restir-candidates") == 0)
		{
			if (const char* v = next()) args.restirCandidates = std::max(1, std::atoi(v));
		}
		else if (strcmp(a, "--restir-no-temporal") == 0)
		{
			args.restirNoTemporal = true;
		}
		else if (strcmp(a, "--restir-no-spatial") == 0)
		{
			args.restirNoSpatial = true;
		}
		else if (strcmp(a, "--restir-gi") == 0)
		{
			args.restirGI = true;
		}
		else if (strcmp(a, "--restir-gi-candidates") == 0)
		{
			if (const char* v = next()) args.restirGICandidates = std::max(1, std::atoi(v));
		}
			else if (strcmp(a, "--headless") == 0)
			{
				args.headless = true;
			}
			else if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0)
			{
				args.verbose = true;
			}
			else if (strcmp(a, "--benchmark-timings") == 0)
			{
				args.benchmarkTimings = true;
			}
			else if (strcmp(a, "--validate") == 0)
			{
				args.validate = true;
			}
			else if (strcmp(a, "--sync-validate") == 0)
			{
				args.validate = true;
				args.syncValidate = true;
			}
			else if (strcmp(a, "--ngx-report") == 0)
			{
				args.ngxReport = true;
			}
			else if (strcmp(a, "--ngx-project-id") == 0)
			{
				if (const char* v = next()) args.ngxProjectId = v;
			}
			else if (strcmp(a, "--ngx-feature-path") == 0)
			{
				if (const char* v = next()) args.ngxFeaturePath = v;
			}
			else if (strcmp(a, "--list") == 0 || strcmp(a, "--dry-run") == 0)
			{
				args.listScenes = true;
			}
			else if (strcmp(a, "--help") == 0 || strcmp(a, "-?") == 0)
			{
				printf("RT2 — path traced renderer\n");
				printf("Usage: RT2App [options]\n");
				printf("Options:\n");
				printf("  --scene <path>       Load scene (.glb/.gltf/.obj) on startup\n");
				printf("  --project <path>     Load a portable .rt2proj project\n");
				printf("  --env <path>         Load HDR env map (.hdr/.exr) on startup\n");
			printf("  --output <path>      Save tonemapped PNG after rendering\n");
			printf("  --output-hdr <path>  Save linear HDR output (.exr or .pfm)\n");
			printf("  --rr-guide-report <path>  Save checked RR-neutral guide readback JSON\n");
			printf("  --frames <N>         Render N frames before screenshot (default 1)\n");
			printf("  --width <W>          Viewport width (default 1280)\n");
				printf("  --height <H>         Viewport height (default 720)\n");
				printf("  --spp <N>            Samples per pixel override\n");
				printf("  --bounces <N>        Max bounces override\n");
				printf("  --nrd                Enable NRD denoiser\n");
				printf("  --nrd-accum-frames <N>  Override REBLUR maximum history\n");
				printf("  --nrd-responsive-roughness <R>  Override responsive-history roughness threshold\n");
				printf("  --nrd-responsive-min-frames <N>  Override responsive minimum history\n");
				printf("  --no-accumulate      Disable non-NRD temporal accumulation\n");
				printf("  --raster-first       Enable raster-first hybrid path\n");
				printf("  --ris                Enable ReSTIR DI (alias for --restir, requires raster-first)\n");
			printf("  --restir             Enable ReSTIR DI\n");
		printf("  --restir-candidates <N>  ReSTIR fresh candidate count\n");
		printf("  --restir-no-temporal  Disable ReSTIR DI temporal reuse\n");
		printf("  --restir-no-spatial   Disable ReSTIR DI spatial reuse\n");
		printf("  --restir-gi          Enable ReSTIR GI independently of DI\n");
		printf("  --restir-gi-candidates <N>  ReSTIR GI fresh candidate count\n");
		printf("  --gbuffer-debug <N>  G-buffer debug view mode\n");
		printf("  --camera-pos <x> <y> <z>      Override loaded camera position\n");
		printf("  --camera-forward <x> <y> <z>  Override loaded camera direction\n");
		printf("  --camera-sweep <amplitude> <warmup> <period>  Headless left/right motion\n");
		printf("  --camera-sweep-mode <lateral|forward|yaw>  Sweep direction (yaw amplitude is radians)\n");
		printf("  --camera-sweep-cycles <N>  Sweep N cycles, then return to the base pose and hold\n");
		printf("  --capture-every <N>  Save periodic headless sequence frames\n");
		printf("  --seed <N>           Deterministic sampling seed (decimal or 0x-prefixed)\n");
		printf("  --headless           Render N frames, save screenshot, exit\n");
				printf("  --list               Print what would be loaded, then exit\n");
			printf("  --verbose            Verbose logging\n");
			printf("  --benchmark-timings  Emit per-frame GPU timing records as JSON lines\n");
			printf("  --validate           Enable Vulkan validation layers\n");
			printf("  --sync-validate     Enable synchronization validation (implies --validate)\n");
			printf("  --ngx-report         Print the read-only NGX support snapshot\n");
			printf("  --ngx-project-id <id>  Override NGX Project/Application ID\n");
			printf("  --ngx-feature-path <path>  Override NGX runtime search path\n");
			printf("  --help               Show this help\n");
				exit(0);
			}
			else
			{
				fprintf(stderr, "[CLI] Unknown argument: %s (use --help)\n", a);
			}
		}

		if (!args.rrGuideReport.empty())
		{
			args.headless = true;
			args.rasterFirst = true;
			// RR-neutral validation must exercise the producer without relying on
			// NRD's branch to populate any guide pixel.
		}
		if (args.headless && !args.hasOutput())
			args.outputPath = "screenshot.png";
		if (args.headless && args.frames == 0)
			args.frames = 1;

		return args;
	}

	void Print() const
	{
		printf("[CLI] scene     = %s\n", scenePath.empty() ? "(none)" : scenePath.c_str());
		printf("[CLI] project   = %s\n", projectPath.empty() ? "(none)" : projectPath.c_str());
		printf("[CLI] env       = %s\n", envMapPath.empty() ? "(none)" : envMapPath.c_str());
		printf("[CLI] output    = %s\n", outputPath.empty() ? "(none)" : outputPath.c_str());
		printf("[CLI] outputHDR = %s\n", outputHDRPath.empty() ? "(none)" : outputHDRPath.c_str());
		printf("[CLI] rrGuideReport = %s\n", rrGuideReport.empty() ? "(none)" : rrGuideReport.c_str());
		printf("[CLI] seed      = %u\n", sceneSeed);
		printf("[CLI] frames    = %d\n", frames);
		printf("[CLI] %dx%d  spp=%d  bounces=%d  nrd=%d  headless=%d\n",
		       width, height, spp, bounces, nrd, headless);
	}
};
