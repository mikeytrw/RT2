project "RT2App"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   targetdir "bin/%{cfg.buildcfg}"
   staticruntime "off"

    files { "src/**.h", "src/**.cpp", "vendor/**.h", "vendor/**.cpp", "vendor/**.c" }

    -- Exclude entt entirely from the file glob — it's header-only and
    -- included via #include <entt/entt.hpp>. The include dir is sufficient.
    -- Exclude sol2's tests/examples (they need Catch2 which we don't vendor).
    -- Exclude Lua's standalone interpreter, compiler, test suite, and docs.
    removefiles {
        "vendor/entt/**",
        "vendor/sol2/tests/**",
        "vendor/sol2/examples/**",
        "vendor/sol2/scripts/**",
        "vendor/sol2/single/**",
        "vendor/lua/lua.c",
        "vendor/lua/luac.c",
        "vendor/lua/onelua.c",
        "vendor/lua/testes/**",
        "vendor/lua/manual/**",
    }

    -- Shader source files (compiled via custom build rules below)
    files {
        "shaders/raygen.rgen",
        "shaders/secondary_raygen.rgen",
        "shaders/miss.rmiss",
        "shaders/shadow.rmiss",
        "shaders/closesthit.rchit",
        "shaders/anyhit.rahit",
        "shaders/shadow.rahit",
        "shaders/compose.comp",
        "shaders/tonemap.comp",
        "shaders/restir_temporal.comp",
        "shaders/restir_spatial.comp",
        "shaders/restir_gi_temporal.comp",
        "shaders/restir_gi_history.comp",
        "shaders/raster.vert",
        "shaders/raster.frag",
        "shaders/gbuffer_debug.comp",
		"shaders/picking.comp",
        "shaders/pathtracer_shared.glsl",
        "shaders/scatter_shared.glsl",
        "shaders/restir_gi_shared.glsl",
        "shaders/restir_gi_bindings.glsl",
        "shaders/surface_history_shared.glsl",
        "shaders/material_resolve.glsl",
        "shaders/ray_query_scene.glsl",
        "shaders/shader_interface.h",
    }

    -- glslc path: prefer VULKAN_SDK env var, fall back to known SDK install
    local vulkanSdk = os.getenv("VULKAN_SDK")
    local glslc
    if vulkanSdk and os.isfile(vulkanSdk .. "/Bin/glslc.exe") then
        glslc = vulkanSdk .. "/Bin/glslc.exe"
    else
        glslc = "C:\\VulkanSDK\\1.4.350.0\\Bin\\glslc.exe"
    end
    -- Shader directory: use $(ProjectDir) so paths resolve correctly whether
    -- building from the .sln or directly from the .vcxproj. The .vcxproj lives
    -- in RT2App/, so $(ProjectDir)shaders = RT2App/shaders.
    local shaderDir = "$(ProjectDir)shaders"
    local shaderTarget = "--target-env=vulkan1.2"
    local shaderOpt = ""
    local shaderInclude = "-I " .. shaderDir
    -- Shared dependencies (relative paths for MSBuild buildinputs — premake
    -- tokens like %{wks.location} don't expand in buildinputs, so use
    -- paths relative to the .vcxproj which lives in the RT2App project dir).
    local depShared  = { "shaders/pathtracer_shared.glsl", "shaders/scatter_shared.glsl", "shaders/restir_shared.glsl", "shaders/restir_bindings.glsl", "shaders/shader_interface.h" }
    local depReSTIR  = { "shaders/restir_shared.glsl", "shaders/restir_bindings.glsl", "shaders/shader_interface.h" }
    local depGI      = { "shaders/restir_gi_shared.glsl", "shaders/restir_gi_bindings.glsl", "shaders/surface_history_shared.glsl", "shaders/material_resolve.glsl", "shaders/ray_query_scene.glsl", "shaders/shader_interface.h" }
    local depBasic   = { "shaders/shader_interface.h" }

    -- Per-stage custom build rules (dependency tracking via premake)
    filter {"files:shaders/raygen.rgen"}
        buildmessage "Compiling raygen.rgen"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=rgen " .. shaderInclude .. " " .. shaderDir .. "/raygen.rgen -o " .. shaderDir .. "/raygen.spv" }
        buildoutputs { shaderDir .. "/raygen.spv" }
        buildinputs { depShared }

    filter {"files:shaders/secondary_raygen.rgen"}
        buildmessage "Compiling secondary_raygen.rgen"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=rgen " .. shaderInclude .. " " .. shaderDir .. "/secondary_raygen.rgen -o " .. shaderDir .. "/secondary_raygen.spv" }
        buildoutputs { shaderDir .. "/secondary_raygen.spv" }
        buildinputs { depShared }

    filter {"files:shaders/miss.rmiss"}
        buildmessage "Compiling miss.rmiss"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=rmiss " .. shaderInclude .. " " .. shaderDir .. "/miss.rmiss -o " .. shaderDir .. "/miss.spv" }
        buildoutputs { shaderDir .. "/miss.spv" }
        buildinputs { depShared }

    filter {"files:shaders/shadow.rmiss"}
        buildmessage "Compiling shadow.rmiss"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=rmiss " .. shaderInclude .. " " .. shaderDir .. "/shadow.rmiss -o " .. shaderDir .. "/shadow.spv" }
        buildoutputs { shaderDir .. "/shadow.spv" }
        buildinputs { depShared }

    filter {"files:shaders/closesthit.rchit"}
        buildmessage "Compiling closesthit.rchit"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=rchit " .. shaderInclude .. " " .. shaderDir .. "/closesthit.rchit -o " .. shaderDir .. "/closesthit.spv" }
        buildoutputs { shaderDir .. "/closesthit.spv" }
        buildinputs { depShared }

    filter {"files:shaders/anyhit.rahit"}
        buildmessage "Compiling anyhit.rahit"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=rahit " .. shaderInclude .. " " .. shaderDir .. "/anyhit.rahit -o " .. shaderDir .. "/anyhit.spv" }
        buildoutputs { shaderDir .. "/anyhit.spv" }
        buildinputs { depShared }

    filter {"files:shaders/shadow.rahit"}
        buildmessage "Compiling shadow.rahit"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=rahit " .. shaderInclude .. " " .. shaderDir .. "/shadow.rahit -o " .. shaderDir .. "/shadowhit.spv" }
        buildoutputs { shaderDir .. "/shadowhit.spv" }
        buildinputs { depShared }

    filter {"files:shaders/compose.comp"}
        buildmessage "Compiling compose.comp"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=comp " .. shaderInclude .. " " .. shaderDir .. "/compose.comp -o " .. shaderDir .. "/compose.spv" }
        buildoutputs { shaderDir .. "/compose.spv" }
        buildinputs { depBasic }

    filter {"files:shaders/tonemap.comp"}
        buildmessage "Compiling tonemap.comp"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=comp " .. shaderInclude .. " " .. shaderDir .. "/tonemap.comp -o " .. shaderDir .. "/tonemap.spv" }
        buildoutputs { shaderDir .. "/tonemap.spv" }

    filter {"files:shaders/restir_temporal.comp"}
        buildmessage "Compiling restir_temporal.comp"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=comp " .. shaderInclude .. " " .. shaderDir .. "/restir_temporal.comp -o " .. shaderDir .. "/restir_temporal.spv" }
        buildoutputs { shaderDir .. "/restir_temporal.spv" }
        buildinputs { depReSTIR }

    filter {"files:shaders/restir_spatial.comp"}
        buildmessage "Compiling restir_spatial.comp"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=comp " .. shaderInclude .. " " .. shaderDir .. "/restir_spatial.comp -o " .. shaderDir .. "/restir_spatial.spv" }
        buildoutputs { shaderDir .. "/restir_spatial.spv" }
        buildinputs { depReSTIR }

    filter {"files:shaders/restir_gi_temporal.comp"}
        buildmessage "Compiling restir_gi_temporal.comp"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=comp " .. shaderInclude .. " " .. shaderDir .. "/restir_gi_temporal.comp -o " .. shaderDir .. "/restir_gi_temporal.spv" }
        buildoutputs { shaderDir .. "/restir_gi_temporal.spv" }
        buildinputs { depGI }

    filter {"files:shaders/restir_gi_history.comp"}
        buildmessage "Compiling restir_gi_history.comp"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=comp " .. shaderInclude .. " " .. shaderDir .. "/restir_gi_history.comp -o " .. shaderDir .. "/restir_gi_history.spv" }
        buildoutputs { shaderDir .. "/restir_gi_history.spv" }
        buildinputs { depGI }

    filter {"files:shaders/raster.vert"}
        buildmessage "Compiling raster.vert"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=vert " .. shaderInclude .. " " .. shaderDir .. "/raster.vert -o " .. shaderDir .. "/raster.spv" }
        buildoutputs { shaderDir .. "/raster.spv" }
        buildinputs { depBasic }

    filter {"files:shaders/raster.frag"}
        buildmessage "Compiling raster.frag"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=frag " .. shaderInclude .. " " .. shaderDir .. "/raster.frag -o " .. shaderDir .. "/rasterfrag.spv" }
        buildoutputs { shaderDir .. "/rasterfrag.spv" }
        buildinputs { depBasic }

    filter {"files:shaders/gbuffer_debug.comp"}
        buildmessage "Compiling gbuffer_debug.comp"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=comp " .. shaderInclude .. " " .. shaderDir .. "/gbuffer_debug.comp -o " .. shaderDir .. "/gbufferdebug.spv" }
        buildoutputs { shaderDir .. "/gbufferdebug.spv" }
        buildinputs { depBasic }

	filter {"files:shaders/picking.comp"}
		buildmessage "Compiling picking.comp"
		buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=comp " .. shaderInclude .. " " .. shaderDir .. "/picking.comp -o " .. shaderDir .. "/picking.spv" }
		buildoutputs { shaderDir .. "/picking.spv" }

    -- Reset filter for the rest
    filter {}

    includedirs
    {
       "vendor",
       "vendor/tinygltf",
       "vendor/stb",
       "vendor/entt/src",
       "vendor/sol2/include",          -- Phase 6: sol2 header-only bindings
       "vendor/lua",                   -- Phase 6: Lua 5.4 public headers
       "vendor/NRD/Include",
       "vendor/NRD/Integration",
       "vendor/NRI/Include",
       "shaders",
       "../Walnut/vendor/imgui",
       "../Walnut/vendor/glfw/include",
       "../Walnut/vendor/glm",
       "../Walnut/vendor/stb_image",

       "../Walnut/Walnut/src",

       "%{IncludeDir.VulkanSDK}",
    }

   libdirs
   {
       "vendor/NRD/Lib",
       "vendor/NRI/Lib",
   }

   links
   {
       "Walnut",
       "vendor/NRD/Lib/NRD.lib",
       "vendor/NRD/Lib/ShaderMakeBlob.lib",
       "vendor/NRI/Lib/NRI.lib",
       "vendor/NRI/Lib/NRI_VK.lib",
       "vendor/NRI/Lib/NRI_Shared.lib",
       "vendor/NRI/Lib/NRI_Validation.lib",
   }

   targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
   objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

    filter "system:windows"
       systemversion "latest"
       defines { "WL_PLATFORM_WINDOWS", "GLM_FORCE_DEPTH_ZERO_TO_ONE" }
        postbuildcommands {
            "copy /Y \"$(ProjectDir)shaders\\raygen.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\secondary_raygen.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\miss.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\shadow.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\closesthit.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\anyhit.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\shadowhit.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\compose.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\tonemap.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\restir_temporal.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\restir_spatial.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\restir_gi_temporal.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\restir_gi_history.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\raster.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"$(ProjectDir)shaders\\rasterfrag.spv\" \"%{cfg.targetdir}\"",
			"copy /Y \"$(ProjectDir)shaders\\gbufferdebug.spv\" \"%{cfg.targetdir}\"",
			"copy /Y \"$(ProjectDir)shaders\\picking.spv\" \"%{cfg.targetdir}\""
        }

   filter "configurations:Debug"
      defines { "WL_DEBUG" }
      runtime "Debug"
      symbols "On"

   filter "configurations:Release"
      defines { "WL_RELEASE" }
      runtime "Release"
      optimize "On"
      symbols "On"

   filter "configurations:Dist"
      kind "WindowedApp"
      defines { "WL_DIST" }
      runtime "Release"
      optimize "On"
      symbols "Off"
