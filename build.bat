@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
taskkill /IM seek.exe /F >nul 2>&1
cl /nologo /O2 /GS- /W3 /D_CRT_SECURE_NO_WARNINGS seek.c /Fe:seek.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED
set RC=%ERRORLEVEL%
if exist seek.obj del seek.obj
exit /b %RC%
