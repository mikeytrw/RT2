@echo off
setlocal

rem Try newest Vulkan SDK first, fall back to older
if exist "C:\VulkanSDK\1.4.350.0\Bin\glslc.exe" (
    set GLSLC="C:\VulkanSDK\1.4.350.0\Bin\glslc.exe"
) else (
    set GLSLC="%VULKAN_SDK%\Bin\glslc.exe"
)
set SHADER_DIR=%~dp0
set TARGET=--target-env=vulkan1.2

echo Compiling RT shaders...

%GLSLC% %TARGET% -fshader-stage=rgen  "%SHADER_DIR%raygen.rgen"       -o "%SHADER_DIR%raygen.spv"       -I "%SHADER_DIR%"
if %ERRORLEVEL% neq 0 goto :fail
%GLSLC% %TARGET% -fshader-stage=rmiss "%SHADER_DIR%miss.rmiss"        -o "%SHADER_DIR%miss.spv"          -I "%SHADER_DIR%"
if %ERRORLEVEL% neq 0 goto :fail
%GLSLC% %TARGET% -fshader-stage=rmiss "%SHADER_DIR%shadow.rmiss"     -o "%SHADER_DIR%shadow.spv"        -I "%SHADER_DIR%"
if %ERRORLEVEL% neq 0 goto :fail
%GLSLC% %TARGET% -fshader-stage=rchit "%SHADER_DIR%closesthit.rchit"  -o "%SHADER_DIR%closesthit.spv"    -I "%SHADER_DIR%"
if %ERRORLEVEL% neq 0 goto :fail
%GLSLC% %TARGET% -fshader-stage=rahit "%SHADER_DIR%anyhit.rahit"     -o "%SHADER_DIR%anyhit.spv"        -I "%SHADER_DIR%"
if %ERRORLEVEL% neq 0 goto :fail
%GLSLC% %TARGET% -fshader-stage=rahit "%SHADER_DIR%shadow.rahit"     -o "%SHADER_DIR%shadowhit.spv"     -I "%SHADER_DIR%"
if %ERRORLEVEL% neq 0 goto :fail

echo RT shaders compiled successfully.
exit /b 0

:fail
echo Shader compilation failed!
exit /b 1