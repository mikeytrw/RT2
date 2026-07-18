project "RT2SliceRunner"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"

    files { "src/**.h", "src/**.cpp" }

    -- Link only CPU-only RT2SceneCore source files. This target must NOT
    -- link Walnut, ImGui, GLFW, Vulkan, NRD, or NRI. If it fails to link,
    -- a transitive dependency has leaked into scene core code.
    files {
        "../RT2App/src/SceneDocument.cpp",
        "../RT2App/src/SceneSerializer.cpp",
        "../RT2App/src/SceneAssetResolver.cpp",
        "../RT2App/src/RuntimeSceneController.cpp",
        "../RT2App/src/SceneManager.cpp",
        "../RT2App/src/SceneGraph.cpp",
        "../RT2App/src/SceneLoader.cpp",
        "../RT2App/src/GPUSceneData.cpp",
        "../RT2App/src/PrimitiveGeometry.cpp",
        "../RT2App/src/TinyEXRLoader.cpp",
        "../RT2App/src/EditorSettings.cpp",
        "../RT2App/src/SceneRecoveryService.cpp",
        "../RT2App/src/UnsavedChangesCoordinator.cpp",
        "../RT2App/src/core/UUID.cpp",
        "../RT2App/src/core/Error.cpp",
    }

    includedirs
    {
        "../RT2App/src",
        "../RT2App/vendor",
        "../RT2App/vendor/tinygltf",
        "../RT2App/vendor/entt/src",
        "../Walnut/vendor/glm",
        "../Walnut/vendor/stb_image",
    }

    targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

    filter "system:windows"
       systemversion "latest"
       defines { "GLM_FORCE_DEPTH_ZERO_TO_ONE" }

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