-- RT2AudioBackend/premake5.lua
--
-- A1: isolated miniaudio backend static library (review finding 7 closure).
-- The concrete adapters (src/MiniaudioNoDeviceAdapter.cpp, A4
-- src/ProductionAudioBackend.cpp) and vendored vendor/miniaudio/miniaudio.c
-- live in this top-level project, outside both RT2App/src and RT2App/vendor, so the broad RT2App src/**.cpp and
-- vendor/**.c globs cannot compile either translation unit a second time.
-- RT2App and RT2AudioProbe link this library; RT2Tests and RT2SliceRunner
-- must never compile or link it (enforced by run_audio_a1_gates.ps1).
--
-- Pinned upstream: miniaudio 0.11.25 @ 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d
-- (hashes, URLs, license: see VENDORING.md). Never edit vendored bytes.
--
-- Policy: C++17, staticruntime off, same per-config runtime as RT2App and
-- the Bullet libs (Debug -> runtime "Release" + symbols; Release ->
-- runtime "Release" + optimize) so one static lib links cleanly into every
-- consumer in both configurations. miniaudio.c compiles as C (VS default
-- per-extension CompileAs); its third-party warnings stay off.

project "RT2AudioBackend"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"
   -- Pinned identity: premake otherwise derives the project GUID from the
   -- generator version, so a bare regen with a different premake would
   -- silently rewrite every ProjectReference to this library. The GUID
   -- below is the value the current generator derives; pinning it keeps
   -- tracked references stable.
   uuid "D7F38CC6-437F-FA8A-4C90-7D7FB89A568B"

    files {
      "src/AudioBackendPin.h",
      "src/MiniaudioNoDeviceAdapter.h",
      "src/MiniaudioNoDeviceAdapter.cpp",
      "src/ProductionAudioBackend.h",
      "src/ProductionAudioBackend.cpp",
      "vendor/miniaudio/miniaudio.h",
      "vendor/miniaudio/miniaudio.c",
   }

   includedirs {
      "src",
      "vendor/miniaudio",
      -- A4: the production backend implements the CPU-only IAudioBackend /
      -- IAudioClipProvider interfaces declared in RT2App/src. This is a
      -- header-only dependency (no RT2App translation unit is compiled or
      -- linked here); the A1 gate still proves CPU targets neither compile
      -- nor link this library.
      "../RT2App/src",
   }

   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
   objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   -- Vendored third-party TU: silence warnings, keep the build log clean.
   -- This must not mask warnings in RT2-owned adapter sources.
   filter { "files:vendor/miniaudio/miniaudio.c" }
      warnings "Off"

   filter "system:windows"
      systemversion "latest"

   filter "configurations:Debug"
      runtime "Release"
      symbols "On"

   filter "configurations:Release"
      runtime "Release"
      optimize "On"
      symbols "On"

   filter "configurations:Dist"
      runtime "Release"
      optimize "On"
      symbols "Off"

   filter {}
