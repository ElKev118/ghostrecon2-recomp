@echo off
rem Ghost Recon 2 - Vulkan con el limitador del juego a 60 FPS (prueba de velocidad).
rem Log goes to build\Release\run_60.log
cd /d "%~dp0build\Release"
set RECOMP_PB_EXEC=1
rem Ventana nueva SDL2+Vulkan (F11 o Alt+Enter = pantalla completa).
rem Para volver a la ventana GDI de depuracion: quitar RECOMP_WINDOW y poner RECOMP_FB_WINDOW=1
set RECOMP_WINDOW=sdl
rem Renderer Vulkan (en desarrollo: sombreado provisional, sin combinadores)
set RECOMP_RENDERER=vk
set RECOMP_AC97_READY=1
rem Volumen del juego en % (0 = silencio)
set RECOMP_AUDIO_VOLUME=30
rem Teclado+raton como mando: Enter/Esc=START, Espacio=A, E=B, flechas=cruceta, Tab=BACK, F9=captura
set RECOMP_MKB=1
rem Mando/teclado llegan al juego por el USB emulado (XPP recompilado)
set RECOMP_USB=1
rem Interrupcion VBlank de D3D: es el reloj de los videos XMV (menu, cinematicas)
set RECOMP_VBLANK=1
set RECOMP_TRACE_BUDGET=0
rem Objetivo del limitador de fotogramas del juego en ms (33 = original, 16.667 = 60 FPS, 0 = sin limite)
set RECOMP_GR2_FRAME_MS=16.667
ghostrecon2.exe > run_60.log 2>&1
echo Exit code: %ERRORLEVEL%  (log: %CD%\run_60.log)
pause
