@echo off
rem Builds the driver with WDK 7.1.0, as an internal beta or a release:
rem
rem   build.cmd [WDK root]           next beta:    releases\rNN-betaK\
rem   build.cmd release [WDK root]   the release:  releases\rNN\  (removes its betas)
rem
rem Each build is also zipped whole to releases\<name>.zip for moving to a test PC.
rem
rem NN is one more than the highest releases\rNN folder; K counts that
rem release's betas. Folders are never overwritten. The debug log's first line
rem says "revision NN beta K" (or "revision NN"); the INF DriverVer is
rem 0.1.NN.K for betas and 0.1.NN.100 for the release, so a release always
rem outranks its betas. Each folder contains:
rem   x86         XP x86 release
rem   releases\rNN\x86-chk     XP x86 debug (KdPrint logging for DebugView)
rem   releases\rNN\amd64       XP x64 release
rem   releases\rNN\amd64-chk   XP x64 debug
rem   releases\rNN\x86-x360    XP x86 release, presented to games as an Xbox 360 controller
rem   releases\rNN\amd64-x360  XP x64 release, presented to games as an Xbox 360 controller
rem The first four use the pad's native layout (XGIP_X360_MODE=0); the -x360
rem ones mimic Microsoft's Xbox 360 driver (XGIP_X360_MODE=1, see xboxgip.h).
rem Each folder has the INF, both drivers, the KMDF co-installer, the
rem xusbtest.exe and rumbletest.exe tools, LICENSE and symbols.
rem
rem WDK root defaults to C:\WinDDK\7600.16385.1. WDK 7.1's build.exe cannot
rem handle spaces in paths, so the project is mapped to a spare drive letter
rem with subst for the duration of the build.

setlocal
set MODE=beta
if /i "%~1"=="release" (
    set MODE=release
    shift
)

set WDK=%~1
if "%WDK%"=="" set WDK=C:\WinDDK\7600.16385.1
if not exist "%WDK%\bin\setenv.bat" (
    echo WDK 7.1.0 not found at %WDK%
    exit /b 1
)

set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%

rem Next release number and the next beta of it.
for /f "tokens=1,2" %%A in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\nextrev.ps1" "%ROOT%\releases"') do (
    set XGIP_REVISION=%%A
    set NEXT_BETA=%%B
)
if %XGIP_REVISION% LSS 10 (set REV=r0%XGIP_REVISION%) else (set REV=r%XGIP_REVISION%)

if "%MODE%"=="release" (
    set XGIP_BETA=0
    set VERBUILD=100
    set NAME=%REV%
) else (
    set XGIP_BETA=%NEXT_BETA%
    set VERBUILD=%NEXT_BETA%
    set NAME=%REV%-beta%NEXT_BETA%
)

set REL=%ROOT%\releases\%NAME%
if exist "%REL%" (
    echo %REL% already exists; builds are never overwritten.
    exit /b 1
)
echo Building %NAME%

set DRV=X:
subst %DRV% /d >nul 2>&1
subst %DRV% "%ROOT%" || (echo subst %DRV% failed & exit /b 1)

set FAIL=0

rem Native layout: release and debug builds.
set XGIP_X360_MODE=0
for %%C in (fre chk) do (
    rem no_oacr: skip the OACR code-analysis wrapper (optional WDK component).
    cmd /c "call %WDK%\bin\setenv.bat %WDK%\ %%C x86 WXP no_oacr && cd /d %DRV%\ && build -cZg"
    call :stage x86   i386  obj%%C_wxp_x86     %%C
    cmd /c "call %WDK%\bin\setenv.bat %WDK%\ %%C x64 WNET no_oacr && cd /d %DRV%\ && build -cZg"
    call :stage amd64 amd64 obj%%C_wnet_amd64  %%C
)

rem Xbox 360 layout: release builds only.
set XGIP_X360_MODE=1
cmd /c "call %WDK%\bin\setenv.bat %WDK%\ fre x86 WXP no_oacr && cd /d %DRV%\ && build -cZg"
call :stage x86   i386  objfre_wxp_x86     x360
cmd /c "call %WDK%\bin\setenv.bat %WDK%\ fre x64 WNET no_oacr && cd /d %DRV%\ && build -cZg"
call :stage amd64 amd64 objfre_wnet_amd64  x360

subst %DRV% /d >nul 2>&1

if %FAIL%==1 (
    echo.
    echo BUILD FAILED - see build*.err / build*.log in the project folders.
    rem Don't leave a half-built release behind; the number gets reused.
    rmdir /s /q "%REL%" 2>nul
    exit /b 1
)

if exist "%ROOT%\CHANGELOG.md" copy /y "%ROOT%\CHANGELOG.md" "%REL%\" >nul
if exist "%ROOT%\LICENSE" copy /y "%ROOT%\LICENSE" "%REL%\" >nul

rem The release replaces its betas (folders and zips).
if "%MODE%"=="release" (
    for /d %%B in ("%ROOT%\releases\%REV%-beta*") do (
        rmdir /s /q "%%B"
        echo Removed beta folder %%~nxB
    )
    del /q "%ROOT%\releases\%REV%-beta*.zip" 2>nul
)

rem One zip of the whole build, for carrying to the test PC.
powershell -NoProfile -Command "Compress-Archive -Path '%REL%' -DestinationPath '%REL%.zip' -Force" || (
    echo Zipping %NAME% failed.
    exit /b 1
)

echo.
echo %NAME% ready in %REL%
echo                and %REL%.zip
exit /b 0

:stage
rem %1 = package arch, %2 = obj subdir, %3 = obj dir prefix, %4 = fre/chk/x360
set OUT=%REL%\%1
if "%4"=="chk" set OUT=%REL%\%1-chk
if "%4"=="x360" set OUT=%REL%\%1-x360
if not exist "%OUT%" mkdir "%OUT%"
for %%D in (xboxgip xgiphid) do (
    if exist "%ROOT%\%%D\%3\%2\%%D.sys" (
        copy /y "%ROOT%\%%D\%3\%2\%%D.sys" "%OUT%\" >nul
        if exist "%ROOT%\%%D\%3\%2\%%D.pdb" copy /y "%ROOT%\%%D\%3\%2\%%D.pdb" "%OUT%\" >nul
    ) else (
        echo missing %%D\%3\%2\%%D.sys
        set FAIL=1
    )
)
for %%T in (xusbtest rumbletest) do (
    if exist "%ROOT%\tools\%%T\%3\%2\%%T.exe" (
        copy /y "%ROOT%\tools\%%T\%3\%2\%%T.exe" "%OUT%\" >nul
    ) else (
        echo missing tools\%%T\%3\%2\%%T.exe
        set FAIL=1
    )
)
rem Stamp today's date and the version into DriverVer (0.1.<rev>.<beta, or 100 for a release>).
rem The 360 packages also name every controller "Xbox 360 Controller for Windows",
rem as Microsoft's 360 driver does in Device Manager.
powershell -NoProfile -Command "$c = Get-Content '%ROOT%\inf\xboxgip.inf'; $c = $c -replace '^DriverVer\s*=.*$', ('DriverVer   = ' + (Get-Date -Format 'MM/dd/yyyy') + ',0.1.%XGIP_REVISION%.%VERBUILD%'); if ('%4' -eq 'x360') { $c = $c -replace '^(\w+\.DeviceDesc\s*=\s*).*\(GIP\).*$', ('$1' + [char]34 + 'Xbox 360 Controller for Windows' + [char]34) }; $c | Set-Content -Encoding ASCII '%OUT%\xboxgip.inf'" || set FAIL=1
copy /y "%WDK%\redist\wdf\%1\WdfCoInstaller01009.dll" "%OUT%\" >nul || set FAIL=1
rem GPL-2.0: ship the license next to the binaries.
copy /y "%ROOT%\LICENSE" "%OUT%\" >nul || set FAIL=1
goto :eof
