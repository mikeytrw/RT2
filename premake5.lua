-- premake5.lua
workspace "RT2App"
   architecture "x64"
   configurations { "Debug", "Release", "Dist" }
   startproject "RT2App"

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
include "Walnut/WalnutExternal.lua"

-- Pinned Bullet core libraries (T1). Included before the consumers so that
-- RT2App, RT2Tests and RT2SliceRunner can link the three StaticLib projects.
-- The Bullet top-level project is never added: only LinearMath,
-- BulletCollision and BulletDynamics (see RT2App/vendor/bullet/premake5.lua).
include "RT2App/vendor/bullet"
include "RT2App"
include "RT2Tests"
include "RT2SliceRunner"
include "RT2ImGuiProbe"