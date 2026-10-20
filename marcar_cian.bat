@echo off
rem Activa la sonda: el juego se pone muy lento ~1 minuto mientras mide.
type nul > "%~dp0build\Release\probe.flag"
echo Sonda activada. Espera ~1 minuto y luego cierra el juego.
timeout /t 5
