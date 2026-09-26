-- RT2AudioProbe/premake5.lua
--
-- A1: real no-device probe shell. ConsoleApp that links RT2AudioBackend and
-- exercises the pinned miniaudio engine in fixed float32 stereo 48 kHz
-- no-device mode with exact frame-count checks. Full decoder/mixer/sample
-- oracles belong to A4; this target proves the pin, the static link, and
-- the production render path. CPU-only targets (RT2Tests, RT2SliceRunner)
-- must never reference this project.

project "RT2AudioProbe"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"
   -- Pinned identity (same rationale as RT2AudioBackend).
   uuid "67AB1F1E-D36B-9B54-9CA6-E4D10826E030"

    files { "src/**.h", "src/**.cpp" }

    -- A5 fixup: S20 drives AudioWorld's split phases against the
    -- production no-device engine. These CPU-only engine sources (plus
    -- their core TU dependencies) carry no miniaudio/device types; the
    -- adapter and miniaudio.c still compile exactly once in
    -- RT2AudioBackend (A1 gate).
    files {
        "../RT2App/src/AudioWorld.cpp",
        "../RT2App/src/core/UUID.cpp",
        "../RT2App/src/core/Error.cpp",
    }

    -- A8 fixup (P1): S21 resolves the shipped acceptance scene's real
    -- clip references through the production AssetResolver (path+sidecar
    -- verification, no database). These three TUs are std-only plus
    -- core/neutral headers; they carry no miniaudio/device types and add
    -- no backend TU (A1 gate). AudioClipAssetProvider.cpp itself stays
    -- out: its FNV lives in the physics TU, which this probe must never
    -- link; the provider's raw-byte cache on these same shipped files is
    -- proven by the RT2Tests A8 case instead.
    files {
        "../RT2App/src/AssetResolver.cpp",
        "../RT2App/src/AssetIdentity.cpp",
        "../RT2App/src/AssetDatabase.cpp",
    }

    includedirs {
      "../RT2AudioBackend/src",
      -- A4: the production backend header implements the CPU-only
      -- IAudioBackend / IAudioClipProvider interfaces from RT2App/src
      -- (header-only; this target compiles no RT2App translation unit).
      "../RT2App/src",
      -- A8 fixup (P1): header-only nlohmann json.hpp for parsing the
      -- shipped scene file; entt/glm for the neutral asset headers above.
      "../RT2App/vendor/tinygltf",
      "../RT2App/vendor/entt/src",
      "../Walnut/vendor/glm",
   }

   links {
      "RT2AudioBackend",
   }

   targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
   objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

   filter "system:windows"
      systemversion "latest"

   filter "configurations:Debug"
      defines { "WL_DEBUG" }
      -- Same /MD compromise as RT2Tests/RT2SliceRunner: the shared
      -- RT2AudioBackend Debug static library builds /MD, so this consumer
      -- stays /MD too (symbols on, optimization off).
      runtime "Release"
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
