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

   includedirs {
      "../RT2AudioBackend/src",
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
