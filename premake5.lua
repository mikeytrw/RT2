-- premake5.lua
workspace "RT2App"
   architecture "x64"
   configurations { "Debug", "Release", "Dist" }
   startproject "RT2App"

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
include "Walnut/WalnutExternal.lua"

include "RT2App"