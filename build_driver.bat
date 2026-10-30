@echo off
setlocal enabledelayedexpansion
pushd "%~dp0"

echo [+] FFPanel driver build

rem %ProgramFiles(x86)% contains parentheses, which break parenthesised blocks,
rem so it is resolved once here, outside of any block.
set "PF86=%ProgramFiles(x86)%"

rem --- 1. MSBuild -------------------------------------------------------------
set "MSBUILD="
set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "!VSWHERE!" (
    for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
        if not defined MSBUILD set "MSBUILD=%%i"
    )
)
if not defined MSBUILD (
    for /f "delims=" %%i in ('where msbuild 2^>nul') do if not defined MSBUILD set "MSBUILD=%%i"
)
if not defined MSBUILD (
    echo [x] MSBuild not found.
    echo     Install Visual Studio 2022 with "Desktop development with C++".
    goto :fail
)
echo [=] MSBuild: !MSBUILD!

rem --- 2. WDK -----------------------------------------------------------------
if not defined WDKContentRoot (
    if exist "!PF86!\Windows Kits\10\Include" set "WDKContentRoot=!PF86!\Windows Kits\10"
)
if not defined WDKContentRoot (
    echo [x] Windows Kits 10 not found under "!PF86!".
    goto :fail
)

set "WDKVER="
for /f "delims=" %%i in ('dir /b /ad /o-n "!WDKContentRoot!\Include" 2^>nul') do (
    if not defined WDKVER if exist "!WDKContentRoot!\Include\%%i\km\ntifs.h" set "WDKVER=%%i"
)
if not defined WDKVER (
    echo [x] Kernel-mode headers ^(km^) are not installed under:
    echo     "!WDKContentRoot!\Include"
    echo     The plain Windows SDK ships there; building a driver needs the
    echo     Windows Driver Kit ^(WDK^) for the same version, plus the
    echo     "Windows Driver Kit" component inside Visual Studio.
    goto :fail
)
echo [=] WDK:   !WDKVER!

rem --- 3. project -------------------------------------------------------------
if not exist "driver\Driver.vcxproj" (
    echo [x] driver\Driver.vcxproj is missing.
    goto :fail
)

rem --- 4. build ---------------------------------------------------------------
"!MSBUILD!" "driver\Driver.vcxproj" /m /nologo /v:minimal ^
    /p:Configuration=Release /p:Platform=x64 ^
    /p:WindowsTargetPlatformVersion=!WDKVER! ^
    "/p:WDKContentRoot=!WDKContentRoot!"
if errorlevel 1 (
    echo [x] Driver build failed
    goto :fail
)

if not exist "driver\x64\Release\FFPanel.sys" (
    echo [x] Build reported success but driver\x64\Release\FFPanel.sys is missing.
    goto :fail
)

echo.
echo [OK] Driver: driver\x64\Release\FFPanel.sys
echo.
echo Before loading: bcdedit /set testsigning on   then reboot
echo Sign with:      signtool sign /fd SHA256 /a /f yourcert.pfx /p password driver\x64\Release\FFPanel.sys
echo Load with:      sc create FFPanel type= kernel binPath= "%CD%\driver\x64\Release\FFPanel.sys"
popd
exit /b 0

:fail
popd
exit /b 1
