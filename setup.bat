@echo off
setlocal
echo [+] FFPanel dependency setup

where git >nul 2>&1
if errorlevel 1 (
    echo [!] git not found in PATH.
    exit /b 1
)

if not exist "third_party" mkdir "third_party"

if not exist "third_party\minhook\src\hook.c" (
    echo [+] fetching MinHook ...
    git clone --depth 1 https://github.com/TsudaKageyu/minhook.git "third_party\minhook"
    if errorlevel 1 (
        echo [!] MinHook clone failed
        exit /b 1
    )
) else (
    echo [=] MinHook already present
)

if not exist "third_party\json.hpp" (
    echo [+] fetching nlohmann/json single header ...
    powershell -NoProfile -Command "Invoke-WebRequest -UseBasicParsing -Uri 'https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp' -OutFile 'third_party\json.hpp'"
    if errorlevel 1 (
        echo [!] json.hpp download failed
        exit /b 1
    )
) else (
    echo [=] json.hpp already present
)

echo.
echo [OK] dependencies ready. Next: build.bat
endlocal
