project "RT2Tests"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"

    files { "src/**.h", "src/**.cpp", "vendor/**.h", "vendor/**.cpp" }

    -- Phase1A fixture generator header (header-only, included by tests).
    files { "../RT2App/src/Phase1AFixtureGenerator.h" }

    -- Include source files from RT2App for testing
    files { "../RT2App/src/SceneLoader.cpp", "../RT2App/src/GPUSceneData.cpp", "../RT2App/src/SceneGraph.cpp", "../RT2App/src/SceneHierarchy.cpp", "../RT2App/src/SceneVisibility.cpp", "../RT2App/src/SceneManager.cpp", "../RT2App/src/PrimitiveGeometry.cpp", "../RT2App/src/TinyEXRLoader.cpp", "../RT2App/src/SceneDocument.cpp", "../RT2App/src/SceneSerializer.cpp", "../RT2App/src/SceneAssetResolver.cpp", "../RT2App/src/RuntimeSceneController.cpp", "../RT2App/src/RuntimeSceneMutator.cpp", "../RT2App/src/InputStateMachine.cpp", "../RT2App/src/EditorSettings.cpp", "../RT2App/src/SceneRecoveryService.cpp", "../RT2App/src/UnsavedChangesCoordinator.cpp", "../RT2App/src/EditorSelection.cpp", "../RT2App/src/EditorSceneState.cpp", "../RT2App/src/EditorCameraWorkflow.cpp", "../RT2App/src/EditorCommandHistory.cpp", "../RT2App/src/EditorCommands.cpp", "../RT2App/src/EditorStructuralCommands.cpp", "../RT2App/src/EditorPropertyCommands.cpp", "../RT2App/src/EditorSyncRouter.cpp", "../RT2App/src/ViewportCoordinates.cpp", "../RT2App/src/TransformEditing.cpp", "../RT2App/src/ScriptAssetPath.cpp", "../RT2App/src/ScriptFieldReconcile.cpp", "../RT2App/src/ScriptFieldRegistry.cpp", "../RT2App/src/ScriptFieldResolver.cpp", "../RT2App/src/ScriptSystem.cpp", "../RT2App/src/AssetIdentity.cpp", "../RT2App/src/core/UUID.cpp", "../RT2App/src/core/Error.cpp" }

    -- Phase 6: Lua 5.4 C sources compiled into the test target so the
    -- CPU-only ScriptSystem tests can execute Lua without an external
    -- .lib. Same source set as RT2App (no lua.c/luac.c/onelua.c/testes).
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
       "vendor",
       "vendor/doctest",
       "../Walnut/vendor/glm",
       "../Walnut/vendor/stb_image",
       "../RT2App/vendor",
       "../RT2App/vendor/tinygltf",
       "../RT2App/vendor/entt/src",
       "../RT2App/vendor/sol2/include",   -- Phase 6: sol2 header-only bindings
       "../RT2App/vendor/lua",            -- Phase 6: Lua 5.4 public headers
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
