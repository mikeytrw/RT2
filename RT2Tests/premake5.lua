project "RT2Tests"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"

    files { "src/**.h", "src/**.cpp", "vendor/**.h", "vendor/**.cpp" }

    -- Phase1A fixture generator header (header-only, included by tests).
    files { "../RT2App/src/Phase1AFixtureGenerator.h" }

    -- Include source files from RT2App for testing
    files { "../RT2App/src/SceneLoader.cpp", "../RT2App/src/GPUSceneData.cpp", "../RT2App/src/SceneGraph.cpp", "../RT2App/src/SceneHierarchy.cpp", "../RT2App/src/SceneVisibility.cpp", "../RT2App/src/SceneManager.cpp", "../RT2App/src/PrimitiveGeometry.cpp", "../RT2App/src/TinyEXRLoader.cpp", "../RT2App/src/SceneDocument.cpp", "../RT2App/src/SceneSerializer.cpp", "../RT2App/src/SceneAssetResolver.cpp", "../RT2App/src/RuntimeSceneController.cpp", "../RT2App/src/EditorSettings.cpp", "../RT2App/src/SceneRecoveryService.cpp", "../RT2App/src/UnsavedChangesCoordinator.cpp", "../RT2App/src/EditorSelection.cpp", "../RT2App/src/EditorSceneState.cpp", "../RT2App/src/ViewportCoordinates.cpp", "../RT2App/src/TransformEditing.cpp", "../RT2App/src/core/UUID.cpp", "../RT2App/src/core/Error.cpp" }

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
