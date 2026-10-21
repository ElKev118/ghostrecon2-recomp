@echo off
rem Captura un fotograma con la sonda (unos segundos). Se puede usar varias veces.
type nul > "%~dp0build\Release\probe.flag"
echo Sonda activada: captura en unos segundos.
timeout /t 5
