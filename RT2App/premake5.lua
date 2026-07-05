project "RT2App"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   targetdir "bin/%{cfg.buildcfg}"
   staticruntime "off"

    files { "src/**.h", "src/**.cpp", "vendor/**.h", "vendor/**.cpp" }

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