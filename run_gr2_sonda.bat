@echo off
rem Ghost Recon 2 - Vulkan con la sonda de cian lista (sin sonido).
rem En la entrada de la base, con el suelo negro en la mitad de abajo de la pantalla, ejecuta marcar_cian.bat.
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
set RECOMP_VK_PROBE=60,360;110,420;420,380;520,340;600,400;80,300
ghostrecon2.exe > run_probe.log 2>&1
echo Listo. Avisale a Claude.
pause
