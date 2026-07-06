project "RT2Tests"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"

    files { "src/**.h", "src/**.cpp", "vendor/**.h", "vendor/**.cpp" }

    -- Include source files from RT2App for testing
    files { "../RT2App/src/SceneLoader.cpp", "../RT2App/src/GPUSceneData.cpp", "../RT2App/src/SceneGraph.cpp" }

    includedirs
    {
       "vendor",
       "vendor/doctest",
       "../Walnut/vendor/glm",
       "../Walnut/vendor/stb_image",
       "../RT2App/vendor",
       "../RT2App/vendor/tinygltf",
       "../RT2App/vendor/entt/src",
       "../RT2App/src",
    }

    targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

    filter "system:windows"
       systemversion "latest"

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
       defines { "WL_DIST" }
       runtime "Release"
       optimize "On"
       symbols "Off"