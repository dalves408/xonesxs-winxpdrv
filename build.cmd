@echo off
rem Builds a new numbered release of the driver with WDK 7.1.0:
rem   releases\rNN\x86         XP x86 release
rem   releases\rNN\x86-chk     XP x86 debug (KdPrint logging for DebugView)
rem   releases\rNN\amd64       XP x64 release
rem   releases\rNN\amd64-chk   XP x64 debug
rem Each folder has the INF, both drivers, the KMDF co-installer, xusbtest.exe
rem and symbols. NN is one more than the highest existing release; set
rem XGIP_REVISION to force a number. The revision is compiled into the driver
rem (first debug log line) and stamped into the INF DriverVer.
rem
rem   build.cmd [WDK root]      default: C:\WinDDK\7600.16385.1
rem
rem WDK 7.1's build.exe cannot handle spaces in paths, so the project is
rem mapped to a spare drive letter with subst for the duration of the build.

setlocal
set WDK=%~1
if "%WDK%"=="" set WDK=C:\WinDDK\7600.16385.1
if not exist "%WDK%\bin\setenv.bat" (
    echo WDK 7.1.0 not found at %WDK%
    exit /b 1
)

set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%

rem Next release number.
if not defined XGIP_REVISION (
    for /f %%N in ('powershell -NoProfile -Command "$n = Get-ChildItem -Directory '%ROOT%\releases' -Filter 'r*' -ErrorAction SilentlyContinue | ForEach-Object { [int]($_.Name.Substring(1)) } | Measure-Object -Maximum; [int]$n.Maximum + 1"') do set XGIP_REVISION=%%N
)
if %XGIP_REVISION% LSS 10 (set REV=r0%XGIP_REVISION%) else (set REV=r%XGIP_REVISION%)
set REL=%ROOT%\releases\%REV%
if exist "%REL%" (
    echo %REL% already exists; releases are never overwritten.
    exit /b 1
)
echo Building release %REV%

set DRV=X:
subst %DRV% /d >nul 2>&1
subst %DRV% "%ROOT%" || (echo subst %DRV% failed & exit /b 1)

set FAIL=0
for %%C in (fre chk) do (
    rem no_oacr: skip the OACR code-analysis wrapper (optional WDK component).
    cmd /c "call %WDK%\bin\setenv.bat %WDK%\ %%C x86 WXP no_oacr && cd /d %DRV%\ && build -cZg"
    call :stage x86   i386  obj%%C_wxp_x86     %%C
    cmd /c "call %WDK%\bin\setenv.bat %WDK%\ %%C x64 WNET no_oacr && cd /d %DRV%\ && build -cZg"
    call :stage amd64 amd64 obj%%C_wnet_amd64  %%C
)

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
echo.
echo Release %REV% ready in %REL%
exit /b 0

:stage
rem %1 = package arch, %2 = obj subdir, %3 = obj dir prefix, %4 = fre/chk
if "%4"=="chk" (set OUT=%REL%\%1-chk) else (set OUT=%REL%\%1)
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
if exist "%ROOT%\tools\xusbtest\%3\%2\xusbtest.exe" (
    copy /y "%ROOT%\tools\xusbtest\%3\%2\xusbtest.exe" "%OUT%\" >nul
) else (
    echo missing tools\xusbtest\%3\%2\xusbtest.exe
    set FAIL=1
)
rem Stamp today's date and the revision into DriverVer (0.1.<rev>.0).
powershell -NoProfile -Command "(Get-Content '%ROOT%\inf\xboxgip.inf') -replace '^DriverVer\s*=.*$', ('DriverVer   = ' + (Get-Date -Format 'MM/dd/yyyy') + ',0.1.%XGIP_REVISION%.0') | Set-Content -Encoding ASCII '%OUT%\xboxgip.inf'" || set FAIL=1
copy /y "%WDK%\redist\wdf\%1\WdfCoInstaller01009.dll" "%OUT%\" >nul || set FAIL=1
rem GPL-2.0: ship the license next to the binaries.
copy /y "%ROOT%\LICENSE" "%OUT%\" >nul || set FAIL=1
goto :eof
