project "RT2App"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   targetdir "bin/%{cfg.buildcfg}"
   staticruntime "off"

    files { "src/**.h", "src/**.cpp", "vendor/**.h", "vendor/**.cpp" }

    -- Exclude entt entirely from the file glob — it's header-only and
    -- included via #include <entt/entt.hpp>. The include dir is sufficient.
    removefiles {
        "vendor/entt/**",
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
        "shaders/ris.comp",
        "shaders/restir_temporal.comp",
        "shaders/restir_spatial.comp",
        "shaders/raster.vert",
        "shaders/raster.frag",
        "shaders/gbuffer_debug.comp",
        "shaders/pathtracer_shared.glsl",
        "shaders/scatter_shared.glsl",
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
    local shaderDir = "%{wks.location}/RT2App/shaders"
    local shaderTarget = "--target-env=vulkan1.2"
    local shaderOpt = ""
    local shaderInclude = "-I " .. shaderDir
    -- Shared dependencies (relative paths for MSBuild buildinputs — premake
    -- tokens like %{wks.location} don't expand in buildinputs, so use
    -- paths relative to the .vcxproj which lives in the RT2App project dir).
    local depShared  = { "shaders/pathtracer_shared.glsl", "shaders/scatter_shared.glsl", "shaders/restir_shared.glsl", "shaders/restir_bindings.glsl", "shaders/shader_interface.h" }
    local depReSTIR  = { "shaders/restir_shared.glsl", "shaders/restir_bindings.glsl", "shaders/shader_interface.h" }
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

    filter {"files:shaders/ris.comp"}
        buildmessage "Compiling ris.comp"
        buildcommands { glslc .. " " .. shaderTarget .. " " .. shaderOpt .. " -fshader-stage=comp " .. shaderInclude .. " " .. shaderDir .. "/ris.comp -o " .. shaderDir .. "/ris.spv" }
        buildoutputs { shaderDir .. "/ris.spv" }
        buildinputs { depReSTIR }

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

    -- Reset filter for the rest
    filter {}

    includedirs
    {
       "vendor",
       "vendor/tinygltf",
       "vendor/stb",
       "vendor/entt/src",
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
            "copy /Y \"%{wks.location}RT2App\\shaders\\raygen.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\secondary_raygen.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\miss.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\shadow.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\closesthit.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\anyhit.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\shadowhit.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\compose.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\ris.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\restir_temporal.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\restir_spatial.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\raster.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\rasterfrag.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\gbufferdebug.spv\" \"%{cfg.targetdir}\""
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