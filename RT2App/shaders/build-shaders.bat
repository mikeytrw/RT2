@echo off
setlocal

set GLSLC="%VULKAN_SDK%\Bin\glslc.exe"
set SHADER_DIR=%~dp0

echo Compiling shaders...

%GLSLC% -fshader-stage=comp "%SHADER_DIR%pathtracer.comp" -o "%SHADER_DIR%pathtracer.spv"

if %ERRORLEVEL% neq 0 (
    echo Shader compilation failed!
    exit /b 1
)

echo Shaders compiled successfully.