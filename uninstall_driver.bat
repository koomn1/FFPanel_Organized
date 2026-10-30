@echo off
setlocal
pushd "%~dp0"

echo [+] FFPanel driver uninstall

net session >nul 2>&1
if errorlevel 1 (
    echo [x] Run this from an elevated prompt.
    goto :fail
)

sc query FFPanel >nul 2>&1
if errorlevel 1 (
    echo [=] service FFPanel is not installed
) else (
    sc stop FFPanel >nul 2>&1
    sc delete FFPanel >nul 2>&1
    echo [=] service removed
)

if /i "%~1"=="--remove-cert" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "driver\remove_test_cert.ps1"
)

echo.
echo To turn test signing back off:  bcdedit /set testsigning off   then reboot
popd
exit /b 0

:fail
popd
exit /b 1
