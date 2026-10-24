# Ghost Recon 2 port - make the release folder.
#   powershell -File tools\package.ps1 [-Version 1.0] [-WithGame]
# Builds dist\GhostRecon2-v<Version>\ from build\Release: the exe, LEEME /
# README, licences, and an empty game\ folder. -WithGame also copies
# build\Release\game (your own game files) in, for a ready-to-play copy --
# never publish that one.
param([string]$Version = "1.0", [switch]$WithGame)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$rel  = Join-Path $root "build\Release"
$deps = Join-Path $root "build\_deps"
$out  = Join-Path $root "dist\GhostRecon2-v$Version"

if (-not (Test-Path (Join-Path $rel "ghostrecon2.exe"))) { throw "build\Release\ghostrecon2.exe not found: build first" }
if (Test-Path $out) { throw "$out already exists: remove it first" }
New-Item -ItemType Directory -Force -Path (Join-Path $out "game"), (Join-Path $out "licenses") | Out-Null

Copy-Item (Join-Path $rel "ghostrecon2.exe") $out
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.md") $out
$lic = @{ "SDL2" = "sdl2-src\LICENSE.txt"; "Vulkan-Headers" = "vulkan_headers-src\LICENSE.md";
          "volk" = "volk-src\LICENSE.md"; "glslang" = "glslang-src\LICENSE.txt";
          "imgui" = "imgui-src\LICENSE.txt"; "SMAA" = "smaa-src\LICENSE.txt";
          "FidelityFX-FSR" = "fsr1-src\license.txt" }
foreach ($k in $lic.Keys) { Copy-Item (Join-Path $deps $lic[$k]) (Join-Path $out "licenses\$k.txt") }
$tk = Join-Path $root "..\xboxrecomp"
Copy-Item (Join-Path $tk "LICENSE") (Join-Path $out "licenses\xboxrecomp.txt")
Copy-Item (Join-Path $tk "LICENSES\LGPL-2.1.txt") (Join-Path $out "licenses\LGPL-2.1.txt")

@"
Ghost Recon 2 - port nativo para PC v$Version / native PC port v$Version
=====================================================================

ES
1. Copia el contenido completo de tu disco de Ghost Recon 2 (Xbox) --
   default.xbe y todas sus carpetas -- dentro de la carpeta game\.
2. Ejecuta ghostrecon2.exe. Esta carpeta se puede mover a donde quieras.
Requisitos: Windows 10/11 x64, GPU con Vulkan 1.3 y Microsoft Visual C++
2015-2022 Redistributable (x64): https://aka.ms/vs/17/release/vc_redist.x64.exe
Supr o Inicio: menu de opciones (ventana, resolucion interna, 16:9, SMAA,
FSR 1, nitidez, sonido, idioma...). F11 o Alt+Enter: pantalla completa.
Partidas guardadas, opciones (gr2.ini), cache de shaders y logs:
%LOCALAPPDATA%\GhostRecon2

EN
1. Copy the whole contents of your Ghost Recon 2 Xbox disc -- default.xbe
   and all its folders -- into the game\ folder.
2. Run ghostrecon2.exe. This folder can be moved anywhere.
Requirements: Windows 10/11 x64, a Vulkan 1.3 GPU and the Microsoft Visual
C++ 2015-2022 Redistributable (x64).
Delete or Home: options menu. F11 or Alt+Enter: fullscreen.
Saves, options (gr2.ini), shader cache and logs: %LOCALAPPDATA%\GhostRecon2

No game data is included. Ghost Recon 2 is (c) Ubisoft / Red Storm
Entertainment; this project is not affiliated with them. Licences of the
components: THIRD_PARTY_NOTICES.md and licenses\.
"@ | Set-Content -Encoding utf8 (Join-Path $out "LEEME - README.txt")
"Copia aqui el contenido de tu disco de Ghost Recon 2 (default.xbe y sus carpetas).`r`nCopy the contents of your Ghost Recon 2 disc here (default.xbe and its folders)." |
    Set-Content -Encoding utf8 (Join-Path $out "game\PON_AQUI_EL_JUEGO - PUT_GAME_HERE.txt")

if ($WithGame) {
    Write-Host "copying game files (several GB)..."
    # Only the disc's files: not what the title writes into its D:\ (memory.log, CACHE\).
    robocopy (Join-Path $rel "game") (Join-Path $out "game") /E /NFL /NDL /NJH /NJS /XF *.log /XD CACHE | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    Remove-Item (Join-Path $out "game\PON_AQUI_EL_JUEGO - PUT_GAME_HERE.txt")
}
Write-Host "done: $out"
