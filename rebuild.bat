@echo off
REM Rebuild the same Windows tree configured by build.bat and CI.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build_windows.ps1" -Compiler MSVC -Phase Rebuild %*
exit /b %errorlevel%
