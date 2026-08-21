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
        "../RT2App/src/core/PathTransaction.cpp",
        "../RT2App/src/SceneDocument.cpp",
        "../RT2App/src/SceneSerializer.cpp",
        "../RT2App/src/SceneAssetReferenceVisitor.cpp",
        "../RT2App/src/SceneAssetMigration.cpp",
        "../RT2App/src/ContentBrowserOperations.cpp",
        "../RT2App/src/AssetResolver.cpp",
        "../RT2App/src/SceneAssetResolver.cpp",
        "../RT2App/src/RuntimeSceneController.cpp",
        "../RT2App/src/RuntimeSceneMutator.cpp",
        "../RT2App/src/SceneManager.cpp",
        "../RT2App/src/EntityReferenceRemapper.cpp",
        "../RT2App/src/EditorCameraWorkflow.cpp",
        "../RT2App/src/SceneGraph.cpp",
        "../RT2App/src/SceneHierarchy.cpp",
        "../RT2App/src/SceneVisibility.cpp",
        "../RT2App/src/TransformEditing.cpp",
        "../RT2App/src/SceneLoader.cpp",
        "../RT2App/src/TextureAssetPipeline.cpp",
        "../RT2App/src/GPUSceneData.cpp",
        "../RT2App/src/PrimitiveGeometry.cpp",
        "../RT2App/src/TinyEXRLoader.cpp",
        "../RT2App/src/EditorSettings.cpp",
        "../RT2App/src/InputConfig.cpp",
        "../RT2App/src/InputBindingEditor.cpp",
        "../RT2App/src/Project.cpp",
        "../RT2App/src/ProjectAssetScanner.cpp",
        "../RT2App/src/ProjectContext.cpp",
        "../RT2App/src/SceneRecoveryService.cpp",
        "../RT2App/src/UnsavedChangesCoordinator.cpp",
        "../RT2App/src/core/UUID.cpp",
        "../RT2App/src/core/Error.cpp",
        "../RT2App/src/AssetIdentity.cpp",
        "../RT2App/src/AssetDatabase.cpp",
        "../RT2App/src/PrefabSerializer.cpp",
        "../RT2App/src/PrefabPropagationContracts.cpp",
        "../RT2App/src/PrefabPropagationDiscovery.cpp",
        "../RT2App/src/PrefabPropagationCommand.cpp",
        "../RT2App/src/PrefabPropagationService.cpp",
        -- Phase 6C/W7: scripting sources for --script-scenario mode.
        "../RT2App/src/ScriptSystem.cpp",
        "../RT2App/src/ScriptAssetPath.cpp",
        "../RT2App/src/ScriptFieldReconcile.cpp",
        "../RT2App/src/ScriptFieldRegistry.cpp",
        "../RT2App/src/ScriptFieldResolver.cpp",
    }

    -- Phase 6C/W7: Lua 5.4 C sources compiled directly into the target
    -- (same source set as RT2Tests; no lua.c/luac.c/onelua.c/testes).
    files {
        "../RT2App/vendor/lua/lapi.c", "../RT2App/vendor/lua/lauxlib.c",
        "../RT2App/vendor/lua/lbaselib.c", "../RT2App/vendor/lua/lcode.c",
        "../RT2App/vendor/lua/lcorolib.c", "../RT2App/vendor/lua/lctype.c",
        "../RT2App/vendor/lua/ldblib.c", "../RT2App/vendor/lua/ldebug.c",
        "../RT2App/vendor/lua/ldo.c", "../RT2App/vendor/lua/ldump.c",
        "../RT2App/vendor/lua/lfunc.c", "../RT2App/vendor/lua/lgc.c",
        "../RT2App/vendor/lua/linit.c", "../RT2App/vendor/lua/liolib.c",
        "../RT2App/vendor/lua/ljumptab.h", "../RT2App/vendor/lua/llex.c",
        "../RT2App/vendor/lua/lmathlib.c", "../RT2App/vendor/lua/lmem.c",
        "../RT2App/vendor/lua/loadlib.c", "../RT2App/vendor/lua/lobject.c",
        "../RT2App/vendor/lua/lopcodes.c", "../RT2App/vendor/lua/loslib.c",
        "../RT2App/vendor/lua/lparser.c", "../RT2App/vendor/lua/lstate.c",
        "../RT2App/vendor/lua/lstring.c", "../RT2App/vendor/lua/lstrlib.c",
        "../RT2App/vendor/lua/ltable.c", "../RT2App/vendor/lua/ltablib.c",
        "../RT2App/vendor/lua/ltests.c", "../RT2App/vendor/lua/ltm.c",
        "../RT2App/vendor/lua/lundump.c", "../RT2App/vendor/lua/lutf8lib.c",
        "../RT2App/vendor/lua/lvm.c", "../RT2App/vendor/lua/lzio.c",
    }

    includedirs
    {
        "../RT2App/src",
        "../RT2App/vendor",
        "../RT2App/vendor/tinygltf",
        "../RT2App/vendor/entt/src",
        "../RT2App/vendor/sol2/include",   -- Phase 6C/W7: sol2 header-only bindings
        "../RT2App/vendor/lua",            -- Phase 6C/W7: Lua 5.4 public headers
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

    filter { "configurations:Debug", "files:../RT2App/src/SceneLoader.cpp" }
       buildoptions { "/bigobj" }

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
