@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
taskkill /IM seek.exe /F >nul 2>&1
rem /GS stack cookies and /guard:cf stay on; /Gy + /OPT:REF,ICF drop unused code
cl /nologo /O2 /GS /guard:cf /Gy /Gw /W4 /D_CRT_SECURE_NO_WARNINGS seek.c /Fe:seek.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /GUARD:CF /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT
set RC=%ERRORLEVEL%
if exist seek.obj del seek.obj
exit /b %RC%
