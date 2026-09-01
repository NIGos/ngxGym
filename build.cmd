@echo off
rem ngxhost -- build. One compiler invocation, no build system, on purpose: the
rem thing under test is built with a single cl.exe line and this should not be
rem harder to build than what it tests.
rem
rem The Vulkan host arrives in phase 4. The Vulkan SDK is installed
rem (C:\VulkanSDK\1.4.357.0) and brings glslc and the Khronos validation layer,
rem which is that half's only real oracle.

setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo build.cmd: no vcvars64.bat at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul 2>&1

rem NVIDIA's DLSS SDK, already on this disk from another project. Because this
rem builds /MT, the static-CRT NGX library is the right one: nvsdk_ngx_s.lib.
rem nvsdk_ngx_d.lib is the /MD build and linking it here gives duplicate-CRT errors.
set NGX=C:\Users\quali\Documents\Code Projects\BH Solver\profork\build\release\_deps\dlss-src
if not exist "%NGX%\include\nvsdk_ngx.h" (
  echo build.cmd: no DLSS SDK headers at "%NGX%\include"
  exit /b 1
)

if not exist "%~dp0bin" mkdir "%~dp0bin"

cl /nologo /W4 /EHsc /O2 /MT /std:c++17 ^
   /I"%NGX%\include" ^
   /Fe:"%~dp0bin\ngxhost.exe" /Fo:"%~dp0bin\\" ^
   "%~dp0src\d3d11.cpp" ^
   /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib advapi32.lib shlwapi.lib ^
         "%NGX%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib"
if errorlevel 1 exit /b 1

echo built: %~dp0bin\ngxhost.exe

rem The Vulkan host. Optional: it needs the Vulkan SDK, and the D3D11 half is useful
rem without it, so a missing SDK is a message rather than a failed build.
set VKSDK=%VULKAN_SDK%
if "%VKSDK%"=="" set VKSDK=C:\VulkanSDK\1.4.357.0
if not exist "%VKSDK%\Include\vulkan\vulkan.h" (
  echo build.cmd: no Vulkan SDK at "%VKSDK%" -- skipping ngxhost-vk.exe
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

cl /nologo /W4 /EHsc /O2 /MT /std:c++17 ^
   /I"%VKSDK%\Include" /I"%NGX%\include" ^
   /Fe:"%~dp0bin\ngxhost-vk.exe" /Fo:"%~dp0bin\vk_" ^
   "%~dp0src\vk.cpp" ^
   /link "%VKSDK%\Lib\vulkan-1.lib" user32.lib advapi32.lib shlwapi.lib ^
         "%NGX%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib"
if errorlevel 1 exit /b 1
echo built: %~dp0bin\ngxhost-vk.exe

:done
endlocal
