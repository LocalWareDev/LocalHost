@echo off
setlocal
REM ---------------------------------------------------------------------------
REM  LocalHost -- unattended kernel-debugger session.
REM
REM  WHY THIS EXISTS
REM  This host boots with ~0.8GB of its 7.8GB physically free, and VS Code plus
REM  the Claude extension hold ~1.1GB of that. A 1536MB WinPE guest needs ~2.05GB
REM  available, so the session cannot be driven from inside the editor -- closing
REM  VS Code is exactly what frees the memory the guest needs.
REM
REM  So: close VS Code, double-click this file, wait for it to finish, then
REM  reopen VS Code. The logs it writes are the whole point; nothing is lost if
REM  the window is closed early, they are flushed as they go.
REM
REM  Guest RAM and the break-in moment are the two knobs:
REM      run_kd_session.bat [guestMB] [breakinSeconds] [totalSeconds]
REM  Defaults 1536 / 150 / 240. Hypervisor.exe refuses to start (U70) if the host
REM  cannot back the guest, so a bad size fails fast instead of freezing the box.
REM ---------------------------------------------------------------------------

set GUEST_MB=%1
if "%GUEST_MB%"=="" set GUEST_MB=1536
set BREAKIN_SEC=%2
if "%BREAKIN_SEC%"=="" set BREAKIN_SEC=150
set TOTAL_SEC=%3
if "%TOTAL_SEC%"=="" set TOTAL_SEC=240

set PROJ=C:\Users\DELL\OneDrive\Desktop\LocalHost py
set FIRMWARE=C:\Users\DELL\Downloads\RELEASEX64_OVMF.fd
set VHD=C:\VHDs\win10_installer.vhd
set KD=C:\LocalHost-evidence\tools\kd\kd.exe
set SYMS=C:\LocalHost-evidence
set OUTDIR=C:\LocalHost-evidence\kd-sessions

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
for /f "tokens=1-4 delims=/:. " %%a in ("%TIME%") do set STAMP=%%a%%b%%c
set VMLOG=%OUTDIR%\vm_%STAMP%.log
set KDLOG=%OUTDIR%\kd_%STAMP%.log
set CMDS=%OUTDIR%\cmds_%STAMP%.txt

REM Commands kd runs once the injected break-in lands. !process 0 0 is the whole
REM question: does winpeshl.exe / setup.exe ever exist? qd detaches and leaves
REM the guest running rather than killing it.
> "%CMDS%" echo .echo ===PROCESSES===
>> "%CMDS%" echo !process 0 0
>> "%CMDS%" echo .echo ===MODULES===
>> "%CMDS%" echo lm
>> "%CMDS%" echo .echo ===END===
>> "%CMDS%" echo qd

echo.
echo   guest RAM      : %GUEST_MB% MB
echo   break-in at    : %BREAKIN_SEC%s
echo   total run      : %TOTAL_SEC%s
echo   logs           : %OUTDIR%
echo.

REM Local symbols ONLY: msdl.microsoft.com is unreachable from this host, and a
REM network symbol path makes kd stall on a timeout right after it connects.
set _NT_SYMBOL_PATH=%SYMS%
set LOCALHOST_KD_BREAKIN_SEC=%BREAKIN_SEC%

echo [1/4] starting the hypervisor...
start "LocalHost VM" /min cmd /c ""%PROJ%\Hypervisor.exe" KDSession "%FIRMWARE%" "%VHD%" %GUEST_MB% > "%VMLOG%" 2>&1"

REM Give the pipe time to exist before kd reaches for it.
timeout /t 3 /nobreak >nul

REM No unbounded console output: kd's reconnect loop can write without limit, and
REM that write storm is what forced earlier hard shutdowns. Straight to a file.
echo [2/4] attaching kd...
start "LocalHost KD" /min cmd /c ""%KD%" -k com:pipe,port=\\.\pipe\LocalHostKD,resets=0,reconnect -y "%SYMS%" -c ".symopt+0x40;g" < "%CMDS%" > "%KDLOG%" 2>&1"

echo [3/4] running for %TOTAL_SEC%s -- break-in fires at %BREAKIN_SEC%s...
timeout /t %TOTAL_SEC% /nobreak

REM Kill kd BY NAME: killing only the cmd wrapper orphans kd.exe, and the pipe
REM allows a single instance, so a stray kd blocks the next session.
echo [4/4] stopping...
taskkill /IM kd.exe /F >nul 2>&1
taskkill /IM Hypervisor.exe /F >nul 2>&1

echo.
echo === did the break-in fire? ===
findstr /C:"injected break-in" "%VMLOG%"
if errorlevel 1 echo   (not found -- check %VMLOG%)
echo.
echo === kd session ===
findstr /C:"Connected to Windows" /C:"Kernel Debugger connection" /C:"PROCESSES" "%KDLOG%"
echo.
echo Logs written to:
echo   %VMLOG%
echo   %KDLOG%
echo.
echo Done. Reopen VS Code and tell Claude the session is finished.
pause
