@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
taskkill /IM seek.exe /F >nul 2>&1
rem /GS stack cookies and /guard:cf stay on; /Gy + /OPT:REF,ICF drop unused code
rem /O1 + LTCG, and the hybrid CRT: vcruntime static, the universal CRT from ucrtbase.dll
rem (part of Windows 10+), so no redistributable is needed and the exe is a fraction of the size
cl /nologo /O1 /GL /GS /guard:cf /Gy /Gw /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS seek.c /Fe:seek.exe /link /SUBSYSTEM:WINDOWS /LTCG /MANIFEST:EMBED /GUARD:CF /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT /NODEFAULTLIB:libucrt.lib ucrt.lib
set RC=%ERRORLEVEL%
if exist seek.obj del seek.obj
exit /b %RC%
