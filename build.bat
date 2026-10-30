@echo off
cmake -S . -B build
if errorlevel 1 exit /b 1
cmake --build build --config Release
