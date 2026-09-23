@echo off
rem Starts the game from the project folder so it finds asset\ and saves\.
cd /d "%~dp0"
if not exist "build-clang\apocalypse.exe" (echo Not built yet - run build.bat first & pause & exit /b 1)
start "" "build-clang\apocalypse.exe" %*
