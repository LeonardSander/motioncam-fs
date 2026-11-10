@echo off
REM Use the same Windows configuration as CI.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build_windows.ps1" -Compiler MSVC %*
exit /b %errorlevel%
