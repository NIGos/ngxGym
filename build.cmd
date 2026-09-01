@echo off
rem ngxhost -- build. Two compiler invocations, no build system, on purpose: the
rem thing under test is built with one cl.exe line and this should not be harder to
rem build than what it tests.
rem
rem Phase 0 builds the D3D11 host only. The Vulkan host arrives in phase 4 and needs
rem the Vulkan SDK, which is NOT installed on this machine as of 2026-09-01.

setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo build.cmd: no vcvars64.bat at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul 2>&1

if not exist "%~dp0bin" mkdir "%~dp0bin"

cl /nologo /W4 /EHsc /O2 /MT /std:c++17 ^
   /Fe:"%~dp0bin\ngxhost.exe" /Fo:"%~dp0bin\\" ^
   "%~dp0src\d3d11.cpp" ^
   /link d3d11.lib dxgi.lib user32.lib
if errorlevel 1 exit /b 1

echo built: %~dp0bin\ngxhost.exe
endlocal
