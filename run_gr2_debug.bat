@echo off
rem Ghost Recon 2 - debugging launcher (development builds).
rem ghostrecon2.exe runs on its own; this starts the build in build\Release
rem with the debugging keys on and the log in build\Release\run_debug.log:
rem   F8 = mark the log ("I just saw a glitch")
rem   F7 = cyan capture (frame + per-draw probe) -- can stall a mission load
rem   F6 = audio capture (last 8 s, per mix group)
rem Settings, saves, shader cache: %LOCALAPPDATA%\GhostRecon2
cd /d "%~dp0build\Release"
set RECOMP_DEBUG_KEYS=1
ghostrecon2.exe > run_debug.log 2>&1
echo Exit code: %ERRORLEVEL%  (log: %CD%\run_debug.log)
pause
