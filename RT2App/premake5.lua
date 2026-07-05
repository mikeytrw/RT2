project "RT2App"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   targetdir "bin/%{cfg.buildcfg}"
   staticruntime "off"

    files { "src/**.h", "src/**.cpp", "vendor/**.h", "vendor/**.cpp" }

    -- Shader source files (compiled via custom build rules below)
    files {
        "shaders/raygen.rgen",
        "shaders/miss.rmiss",
        "shaders/shadow.rmiss",
        "shaders/closesthit.rchit",
        "shaders/anyhit.rahit",
        "shaders/shadow.rahit",
        "shaders/compose.comp",
        "shaders/pathtracer_shared.glsl",
        "shaders/shader_interface.h",
    }

    -- glslc path: try SDK 1.4.350.0 first, then VULKAN_SDK env var
    local glslc = "C:\\VulkanSDK\\1.4.350.0\\Bin\\glslc.exe"
    local shaderDir = "%{wks.location}/RT2App/shaders"
    local shaderTarget = "--target-env=vulkan1.2"
    local shaderInclude = "-I " .. shaderDir

    -- Per-stage custom build rules (dependency tracking via premake)
    filter {"files:shaders/raygen.rgen"}
        buildmessage "Compiling raygen.rgen"
        buildcommands { glslc .. " " .. shaderTarget .. " -fshader-stage=rgen " .. shaderInclude .. " " .. shaderDir .. "/raygen.rgen -o " .. shaderDir .. "/raygen.spv" }
        buildoutputs { shaderDir .. "/raygen.spv" }

    filter {"files:shaders/miss.rmiss"}
        buildmessage "Compiling miss.rmiss"
        buildcommands { glslc .. " " .. shaderTarget .. " -fshader-stage=rmiss " .. shaderInclude .. " " .. shaderDir .. "/miss.rmiss -o " .. shaderDir .. "/miss.spv" }
        buildoutputs { shaderDir .. "/miss.spv" }

    filter {"files:shaders/shadow.rmiss"}
        buildmessage "Compiling shadow.rmiss"
        buildcommands { glslc .. " " .. shaderTarget .. " -fshader-stage=rmiss " .. shaderInclude .. " " .. shaderDir .. "/shadow.rmiss -o " .. shaderDir .. "/shadow.spv" }
        buildoutputs { shaderDir .. "/shadow.spv" }

    filter {"files:shaders/closesthit.rchit"}
        buildmessage "Compiling closesthit.rchit"
        buildcommands { glslc .. " " .. shaderTarget .. " -fshader-stage=rchit " .. shaderInclude .. " " .. shaderDir .. "/closesthit.rchit -o " .. shaderDir .. "/closesthit.spv" }
        buildoutputs { shaderDir .. "/closesthit.spv" }

    filter {"files:shaders/anyhit.rahit"}
        buildmessage "Compiling anyhit.rahit"
        buildcommands { glslc .. " " .. shaderTarget .. " -fshader-stage=rahit " .. shaderInclude .. " " .. shaderDir .. "/anyhit.rahit -o " .. shaderDir .. "/anyhit.spv" }
        buildoutputs { shaderDir .. "/anyhit.spv" }

    filter {"files:shaders/shadow.rahit"}
        buildmessage "Compiling shadow.rahit"
        buildcommands { glslc .. " " .. shaderTarget .. " -fshader-stage=rahit " .. shaderInclude .. " " .. shaderDir .. "/shadow.rahit -o " .. shaderDir .. "/shadowhit.spv" }
        buildoutputs { shaderDir .. "/shadowhit.spv" }

    filter {"files:shaders/compose.comp"}
        buildmessage "Compiling compose.comp"
        buildcommands { glslc .. " " .. shaderTarget .. " -fshader-stage=comp " .. shaderInclude .. " " .. shaderDir .. "/compose.comp -o " .. shaderDir .. "/compose.spv" }
        buildoutputs { shaderDir .. "/compose.spv" }

    -- Reset filter for the rest
    filter {}

    includedirs
    {
       "vendor",
       "vendor/tinygltf",
       "vendor/stb",
       "vendor/NRD/Include",
       "vendor/NRD/Integration",
       "vendor/NRI/Include",
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
       defines { "WL_PLATFORM_WINDOWS" }
        postbuildcommands {
            "copy /Y \"%{wks.location}RT2App\\shaders\\raygen.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\miss.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\shadow.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\closesthit.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\anyhit.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\shadowhit.spv\" \"%{cfg.targetdir}\"",
            "copy /Y \"%{wks.location}RT2App\\shaders\\compose.spv\" \"%{cfg.targetdir}\""
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