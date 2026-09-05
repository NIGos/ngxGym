@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 1
cl /nologo /W4 /EHsc /O2 /MT /std:c++17 /LD /I"%~dp0..\ngxbridge\reshade" /Fo:"%~dp0bin\\" "%~dp0src\scene-probe.cpp" /link /OUT:"%~dp0bin\scene-probe.addon64" /IMPLIB:"%~dp0bin\scene-probe.lib"
exit /b %errorlevel%
