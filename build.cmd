@echo off
rem ngxGym -- build. One compiler invocation, no build system, on purpose: the
rem thing under test is built with a single cl.exe line and this should not be
rem harder to build than what it tests.
rem
rem Paths, all overridable from the environment:
rem   VCVARS      vcvars64.bat of a Visual Studio or Build Tools install
rem   NGX_SDK     NVIDIA's DLSS SDK (github.com/NVIDIA/DLSS): include\ and lib\
rem   VULKAN_SDK  the Vulkan SDK, for glslc and the Khronos validation layer.
rem               Optional: without it only the D3D11 host is built.

setlocal
if "%VCVARS%"=="" set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo build.cmd: no vcvars64.bat at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul 2>&1

rem NVIDIA's DLSS SDK. This builds /MT, so the static-CRT NGX library is the
rem right one: nvsdk_ngx_s.lib. nvsdk_ngx_d.lib is the /MD build and gives
rem duplicate-CRT errors here.
set NGX=%NGX_SDK%
if "%NGX%"=="" set NGX=%~dp0dlss-sdk
if not exist "%NGX%\include\nvsdk_ngx.h" (
  echo build.cmd: no DLSS SDK at "%NGX%" -- set NGX_SDK, or clone github.com/NVIDIA/DLSS to dlss-sdk\ beside this file
  exit /b 1
)

if not exist "%~dp0bin" mkdir "%~dp0bin"

cl /nologo /W4 /EHsc /O2 /MT /std:c++17 ^
   /I"%NGX%\include" ^
   /Fe:"%~dp0bin\ngxGym.exe" /Fo:"%~dp0bin\\" ^
   "%~dp0src\d3d11.cpp" ^
   /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib advapi32.lib shlwapi.lib ^
         "%NGX%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib"
if errorlevel 1 exit /b 1

echo built: %~dp0bin\ngxGym.exe

cl /nologo /W4 /EHsc /O2 /MT /std:c++17 /LD ^
   /I"%~dp0..\ngxbridge\reshade" /Fo:"%~dp0bin\\" ^
   "%~dp0src\capture.cpp" /link /OUT:"%~dp0bin\gym-capture.addon64" /IMPLIB:"%~dp0bin\gym-capture.lib"
if errorlevel 1 exit /b 1

rem The Vulkan host. Optional: it needs the Vulkan SDK, and the D3D11 half is useful
rem without it, so a missing SDK is a message rather than a failed build.
set VKSDK=%VULKAN_SDK%
if "%VKSDK%"=="" set VKSDK=C:\VulkanSDK\1.4.357.0
if not exist "%VKSDK%\Include\vulkan\vulkan.h" (
  echo build.cmd: no Vulkan SDK at "%VKSDK%" -- skipping ngxGym-vk.exe
  goto :done
)

rem SPIR-V, generated with glslc into a bare {0x..,..} initialiser list and included
rem INSIDE an array initialiser in vk.cpp. No wrapping step and no runtime compiler:
rem glslc is a program, not a library, and a host that shells out to it at startup
rem would be one more thing that can fail during a test.
if not exist "%~dp0src\generated" mkdir "%~dp0src\generated"
"%VKSDK%\Bin\glslc.exe" -fshader-stage=vert "%~dp0src\scene.vert" -o "%~dp0src\generated\scene_vert.h" -mfmt=c
if errorlevel 1 exit /b 1
"%VKSDK%\Bin\glslc.exe" -fshader-stage=frag "%~dp0src\scene.frag" -o "%~dp0src\generated\scene_frag.h" -mfmt=c
if errorlevel 1 exit /b 1
"%VKSDK%\Bin\glslc.exe" -fshader-stage=comp "%~dp0src\probe.comp" -o "%~dp0src\generated\probe_comp.h" -mfmt=c
if errorlevel 1 exit /b 1

cl /nologo /W4 /EHsc /O2 /MT /std:c++17 ^
   /I"%VKSDK%\Include" /I"%NGX%\include" ^
   /Fe:"%~dp0bin\ngxGym-vk.exe" /Fo:"%~dp0bin\vk_" ^
   "%~dp0src\vk.cpp" ^
   /link "%VKSDK%\Lib\vulkan-1.lib" user32.lib advapi32.lib shlwapi.lib ^
         "%NGX%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib"
if errorlevel 1 exit /b 1
echo built: %~dp0bin\ngxGym-vk.exe

:done
endlocal
