@echo off
setlocal enabledelayedexpansion
pushd "%~dp0"

echo [+] FFPanel driver install
echo.

rem %ProgramFiles(x86)% has parentheses, which break parenthesised blocks.
set "PF86=%ProgramFiles(x86)%"

rem --- must be elevated --------------------------------------------------------
net session >nul 2>&1
if errorlevel 1 (
    echo [x] Run this from an elevated prompt:
    echo     right click install_driver.bat - "Run as administrator"
    goto :fail
)

rem --- test signing must be on ------------------------------------------------
bcdedit /enum {current} 2>nul | findstr /i "testsigning" | findstr /i "Yes" >nul
if errorlevel 1 (
    echo [x] Test signing is off. Run this, then reboot:
    echo        bcdedit /set testsigning on
    goto :fail
)
echo [=] test signing is on

rem --- build ------------------------------------------------------------------
call build_driver.bat
if errorlevel 1 goto :fail

set "SYS=%CD%\driver\x64\Release\FFPanel.sys"
if not exist "%SYS%" (
    echo [x] %SYS% was not produced
    goto :fail
)

rem --- signtool ---------------------------------------------------------------
set "SIGNTOOL="
for /f "delims=" %%i in ('dir /b /ad /o-n "%PF86%\Windows Kits\10\bin" 2^>nul') do (
    if not defined SIGNTOOL if exist "%PF86%\Windows Kits\10\bin\%%i\x64\signtool.exe" (
        set "SIGNTOOL=%PF86%\Windows Kits\10\bin\%%i\x64\signtool.exe"
    )
)
if not defined SIGNTOOL (
    echo [x] signtool.exe not found under Windows Kits\10\bin
    goto :fail
)
echo [=] signtool: !SIGNTOOL!

rem --- self-signed test certificate -------------------------------------------
set "THUMB="
for /f "usebackq delims=" %%t in (`powershell -NoProfile -ExecutionPolicy Bypass -File "driver\make_test_cert.ps1"`) do (
    if not defined THUMB set "THUMB=%%t"
)
if not defined THUMB (
    echo [x] could not create the test certificate
    goto :fail
)
echo [=] certificate: CN=FFPanelTest  !THUMB!

rem --- sign -------------------------------------------------------------------
"!SIGNTOOL!" sign /fd SHA256 /sha1 !THUMB! "%SYS%"
if errorlevel 1 (
    echo [x] signing failed
    goto :fail
)
echo [=] signed

rem --- service -----------------------------------------------------------------
sc query FFPanel >nul 2>&1
if not errorlevel 1 (
    sc stop FFPanel >nul 2>&1
    sc delete FFPanel >nul 2>&1
)

sc create FFPanel type= kernel binPath= "%SYS%"
if errorlevel 1 (
    echo [x] sc create failed
    goto :fail
)

sc start FFPanel
if errorlevel 1 (
    echo.
    echo [x] the driver did not start. Check the reason with:
    echo        sc query FFPanel
    echo        wevtutil qe System /c:5 /rd:true /f:text
    goto :fail
)

echo.
echo [OK] driver installed and running
echo      device: \\.\_rT7y9Xa2Q
echo.
echo Next: loader.exe HD-Player.exe panel.dll --protect --hide-process
popd
exit /b 0

:fail
popd
exit /b 1
