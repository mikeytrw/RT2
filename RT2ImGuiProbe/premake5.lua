project "RT2ImGuiProbe"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
    staticruntime "off"

    -- Separate ImGui event-probe target (R1 boundary): the real Combo/reset
    -- gesture probe compiles the vendored Dear ImGui core and replays mouse
    -- event frames through the production inspector-commit policy. RT2Tests
    -- and RT2SliceRunner must never link ImGui/Vulkan/Walnut, so this probe
    -- lives here, not there. The VS build authority is
    -- RT2ImGuiProbe.vcxproj (+ RT2Tests/RT2AppCpuSources.props, shared with
    -- RT2Tests so both link the identical CPU translation-unit closure);
    -- this file keeps premake regeneration from dropping the target.
    files { "src/**.h", "src/**.cpp" }

    -- Phase1A fixture generator header (header-only, included by tests).
    files { "../RT2App/src/Phase1AFixtureGenerator.h" }

    -- stb_image implementation TU shared with RT2Tests (Walnut not linked).
    files { "../RT2Tests/src/StbImageImpl.cpp" }

    -- Dear ImGui core only (no backends needed for event-logic simulation).
    files {
        "../Walnut/vendor/imgui/imgui.cpp",
        "../Walnut/vendor/imgui/imgui_draw.cpp",
        "../Walnut/vendor/imgui/imgui_tables.cpp",
        "../Walnut/vendor/imgui/imgui_widgets.cpp",
    }

    -- Identical CPU translation-unit closure as RT2Tests (keep in lockstep
    -- with RT2Tests/premake5.lua and RT2Tests/RT2AppCpuSources.props).
    files { "../RT2App/src/core/PathTransaction.cpp", "../RT2App/src/SceneLoader.cpp", "../RT2App/src/TextureAssetPipeline.cpp", "../RT2App/src/GPUSceneData.cpp", "../RT2App/src/SceneGraph.cpp", "../RT2App/src/SceneHierarchy.cpp", "../RT2App/src/SceneVisibility.cpp", "../RT2App/src/SceneManager.cpp", "../RT2App/src/EntityReferenceRemapper.cpp", "../RT2App/src/PrimitiveGeometry.cpp", "../RT2App/src/TinyEXRLoader.cpp", "../RT2App/src/SceneDocument.cpp", "../RT2App/src/SceneSerializer.cpp", "../RT2App/src/PrefabSerializer.cpp", "../RT2App/src/PrefabEditorActions.cpp", "../RT2App/src/PrefabEditorPresentation.cpp", "../RT2App/src/PrefabPropagationContracts.cpp", "../RT2App/src/PrefabPropagationDiscovery.cpp", "../RT2App/src/PrefabPropagationCommand.cpp", "../RT2App/src/PrefabPropagationService.cpp", "../RT2App/src/SceneAssetReferenceVisitor.cpp", "../RT2App/src/SceneAssetMigration.cpp", "../RT2App/src/ContentBrowserOperations.cpp", "../RT2App/src/ContentBrowserDispatch.cpp", "../RT2App/src/AssetResolver.cpp", "../RT2App/src/SceneAssetResolver.cpp", "../RT2App/src/RuntimeSceneController.cpp", "../RT2App/src/PhysicsWorld.cpp", "../RT2App/src/RuntimeSceneMutator.cpp", "../RT2App/src/InputStateMachine.cpp", "../RT2App/src/InputConfig.cpp", "../RT2App/src/InputBindingEditor.cpp", "../RT2App/src/EditorSettings.cpp", "../RT2App/src/Project.cpp", "../RT2App/src/ProjectAssetScanner.cpp", "../RT2App/src/ProjectContext.cpp", "../RT2App/src/SceneRecoveryService.cpp", "../RT2App/src/UnsavedChangesCoordinator.cpp", "../RT2App/src/EditorSelection.cpp", "../RT2App/src/EditorSceneState.cpp", "../RT2App/src/EditorCameraWorkflow.cpp", "../RT2App/src/EditorCommandHistory.cpp", "../RT2App/src/EditorCommands.cpp", "../RT2App/src/EditorStructuralCommands.cpp", "../RT2App/src/EditorPropertyCommands.cpp", "../RT2App/src/EditorSyncRouter.cpp", "../RT2App/src/ViewportCoordinates.cpp", "../RT2App/src/EditorViewportIcons.cpp", "../RT2App/src/TransformEditing.cpp", "../RT2App/src/ScriptAssetPath.cpp", "../RT2App/src/ScriptFieldReconcile.cpp", "../RT2App/src/ScriptFieldRegistry.cpp", "../RT2App/src/ScriptFieldResolver.cpp",         "../RT2App/src/ScriptSystem.cpp", "../RT2App/src/AssetIdentity.cpp", "../RT2App/src/AssetDatabase.cpp", "../RT2App/src/AssetWatchPolicy.cpp", "../RT2App/src/core/UUID.cpp", "../RT2App/src/core/Error.cpp" }

    files {
        "../RT2App/src/PrefabPropagationLive.cpp",
        "../RT2App/src/PrefabCommandTransaction.cpp",
        "../RT2App/src/CompositePreviewSession.cpp",
        "../RT2App/src/PreviewSessionClose.cpp",
        "../RT2App/src/NgxSupport.cpp",
        "../RT2App/src/NgxLifecycle.cpp",
        "../RT2App/src/RRGuideContract.cpp"
        ,"../RT2App/src/RRFeatureLifecycle.cpp"
    }

    -- Phase 6: Lua 5.4 C sources (same set as RT2App/RT2Tests).
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
       "../RT2Tests/vendor",
       "../RT2Tests/vendor/doctest",
       "../Walnut/vendor/glm",
       "../Walnut/vendor/imgui",
       "../Walnut/vendor/stb_image",
       "../RT2App/vendor",
       "../RT2App/vendor/bullet/src",   -- T3: pinned Bullet core (src include root only; PhysicsWorld.h pulls it)
       "../RT2App/vendor/tinygltf",
       "../RT2App/vendor/entt/src",
       "../RT2App/vendor/sol2/include",
       "../RT2App/vendor/lua",
       "../RT2App/src",
    }

    targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

    -- T3: pinned Bullet core (same explicit three-library link as RT2App,
    -- RT2Tests and RT2SliceRunner; the shared CPU closure now compiles
    -- PhysicsWorld.cpp, which needs the Bullet symbols. No Vulkan/ImGui/
    -- Walnut includes are added with it).
    links
    {
        "BulletDynamics",
        "BulletCollision",
        "LinearMath",
    }

    filter "system:windows"
       systemversion "latest"

    filter "configurations:Debug"
       defines { "WL_DEBUG" }
       -- T3 (T1 Amendment F applied here): runtime "Release" (/MD), not
       -- "Debug" (/MDd). The single shared Bullet Debug static libraries
       -- build /MD (matching RT2App, RT2Tests and RT2SliceRunner); one
       -- static lib per config cannot serve a /MD and a /MDd consumer
       -- simultaneously (LNK2038/LNK1319). Symbols stay on, optimization
       -- off. The vendored ImGui core compiles into this target, so the CRT
       -- stays consistent throughout it.
       runtime "Release"
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
