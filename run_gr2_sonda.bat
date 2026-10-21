@echo off
rem Ghost Recon 2 - Vulkan con la sonda de cian lista (sin sonido).
rem Cada doble clic en marcar_cian.bat captura un fotograma (se puede repetir en la misma partida).
cd /d "%~dp0build\Release"
del probe.flag 2>nul
set RECOMP_PB_EXEC=1
set RECOMP_WINDOW=sdl
set RECOMP_RENDERER=vk
set RECOMP_AC97_READY=1
set RECOMP_AUDIO_VOLUME=0
set RECOMP_MKB=1
set RECOMP_USB=1
set RECOMP_VBLANK=1
set RECOMP_TRACE_BUDGET=0
set RECOMP_VK_PROBE=320,300;320,420;150,350;500,350;320,200;100,420
ghostrecon2.exe > run_probe.log 2>&1
echo Listo. Avisale a Claude.
pause
