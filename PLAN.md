# PLAN — Port nativo de PC de *Tom Clancy's Ghost Recon 2* (Xbox) con xboxrecomp

Objetivo: un `.exe` nativo para Windows que ejecute la lógica original del juego (recompilada estáticamente desde `default.xbe`) con una capa de plataforma moderna **SDL2 + Vulkan**, a resolución arbitraria y >60 FPS.

> Ficheros de resumen para no re-analizar el ejecutable en cada sesión:
> - [docs/XBE_SUMMARY.md](docs/XBE_SUMMARY.md) — cabecera, secciones, entry point, thunks, librerías.
> - [docs/KERNEL_IMPORTS.md](docs/KERNEL_IMPORTS.md) — 150 imports del kernel y su estado (BRIDGE/DATA/STUB0).
> - [docs/ASSET_FORMATS.md](docs/ASSET_FORMATS.md) — formatos de archivo del disco.
> - [docs/STATUS.md](docs/STATUS.md) — bitácora: qué se hizo, último resultado de ejecución, siguiente paso.

---

## 1. Por qué recompilación estática (y no reescritura)

GR2 usa el motor propietario **Stonewall** (Red Storm) sin código fuente. Reescribir el juego en C++ "desde cero" no es viable; lo que sí es viable:

1. **Lógica del juego = código recompilado** (`src/game/recomp/gen/*.c`, generado por `tools/recomp`). Se ejecuta tal cual.
2. **Todo lo que toca hardware/SO se sustituye** por implementaciones nativas: kernel Xbox, Direct3D8/NV2A, DirectSound/APU, XPP (mandos), XMV (vídeo), XOnline (stub).
3. Las mejoras (resolución, FPS, widescreen, FOV) se hacen **en la capa de sustitución** y con **parches puntuales** a funciones recompiladas (`recomp_lookup_manual`).

## 2. Estructura del código

```
xboxrecomp/
├── PLAN.md                         ← este documento
├── docs/               ← resúmenes (ver arriba)
├── tools/
│   ├── xiso/                       ← extractor XISO (python -m tools.xiso)
│   └── xbe_parser, disasm, func_id, recomp   ← pipeline existente
├── src/                            ← librerías runtime (C) compartidas por todos los juegos
│   ├── kernel/   xbox_kernel       Kernel Xbox → Win32   (arreglos de ordinales hechos aquí)
│   ├── d3d/      xbox_d3d8         D3D8 → backend gráfico (hoy D3D11 / OpenGL)
│   ├── nv2a/     xbox_nv2a         push-buffer NV2A → backend gráfico
│   ├── audio/    xbox_dsound       DirectSound → mezclador software
│   ├── apu/      xbox_apu          MCPX APU (xemu)
│   ├── input/    xbox_input        XPP → XInput
│   └── platform/                   abstracción de plataforma (ya existe para Linux)
└── src/game/                       ← el port de GR2
    ├── CMakeLists.txt
    ├── recomp/gen/                 código generado (NO editar a mano; regenerar)
    ├── src/main.c                  host: carga XBE, memoria, kernel, entry point
    ├── src/recomp_manual.c         overrides de funciones recompiladas
    └── port/            (NUEVO, C++17)
        ├── platform/sdl_window.cpp     ventana, eventos, fullscreen, DPI
        ├── platform/sdl_input.cpp      SDL_GameController + teclado/ratón → XINPUT_GAMEPAD
        ├── platform/sdl_audio.cpp      salida del mezclador de xbox_dsound vía SDL_AudioStream
        ├── render/vk_device.cpp        instancia, device, swapchain, frames en vuelo
        ├── render/vk_backend.cpp       implementación de d3d8_backend.h sobre Vulkan
        ├── render/vk_shaders.cpp       NV2A VS/combiners → SPIR-V (vía GLSL + shaderc o HLSL + DXC)
        ├── render/vk_textures.cpp      formatos Xbox (unswizzle/DXT) → VkImage, caché
        ├── render/upscale.cpp          escalado interno, MSAA/FXAA, aspect ratio
        ├── game/gr2_patches.cpp        parches GR2: FOV, widescreen, framerate, skip intros
        ├── game/gr2_config.cpp         config.ini (resolución, vsync, FPS cap, escala)
        └── video/xmv_player.cpp        (opcional) reproducción XMV con FFmpeg
```

Regla: lo que sirve para cualquier juego va en `src/<lib>`; lo específico de GR2 en `src/game/port/game/`.

## 3. Fases

### Fase 0 — Entorno y diagnóstico ✅ (esta sesión)
- [x] ISO extraída a `build/src/game/Release/game/Your Game Title/` con `tools/xiso`.
- [x] Revisados los cambios locales al kernel: eran cosméticos/en una ruta que el juego no usa (ver STATUS.md).
- [x] **Bug real encontrado y corregido** en `src/kernel/kernel_bridge.c`: tablas de ordinales desplazadas (datos, dispatch y bytes stdcall).
- [x] `main.c` configurado para GR2 (entry point, saves en `%LOCALAPPDATA%\GhostRecon2`, hilo de `KeTickCount`).

### Fase 1 — Arranque con el runtime actual (D3D11)  *Criterio de salida: llega al primer `Present` / pantalla de logos.*
1. Ejecutar desde consola, guardar el log (`your_game_recomp.exe > run.log 2>&1`), y por cada fallo:
   - `[KERNEL] WARNING: no bridge for ordinal N` → implementar bridge (prioridad en §4).
   - `[ICALL] Failed to resolve VA` → la VA suele ser una entrada "no detectada" (8 087 en `recomp_stubs_unresolved.c`): re-ejecutar `tools.disasm --seed-functions` con esa VA y regenerar.
   - Crash en VEH → dirección Xbox → buscar función en `tools/disasm/output/functions.json`.
2. Revisar los 8 087 stubs vacíos: un stub vacío llamado = lógica perdida silenciosamente. Hacer que el stub registre la llamada (cambio en `tools/recomp`) para detectarlos.
3. CRT: localizar `__active_heap`/`_heap_init` (LIBCMT 5849) y preinicializar si el SBH falla.
4. Threads: Stonewall crea hilos (`PsCreateSystemThreadEx`) — verificar que esperas (`NtWaitForSingleObject`, `KeDelayExecutionThread`) funcionan.

### Fase 2 — Menús y audio  *Salida: menú principal navegable con mando, con sonido.*
- Implementar `ExQueryNonVolatileSetting` (idioma=ES/EN, 16:9, 480p/720p flags) — GR2 decide widescreen con esto.
- XACT: `.xwb` (`WBND`) Xbox ADPCM → PCM en xbox_dsound.
- XMV: o bien dejar correr la sección XMV recompilada (decodifica en CPU y pinta texturas) o saltar intros (parche).
- XOnline/XNET: forzar "sin red" (`PhyGetLinkState` = 0) para que el juego oculte Live.

### Fase 3 — Capa SDL2  *Salida: el juego corre en ventana SDL2 con el backend D3D11 aún.*
- Sustituir la creación de ventana Win32 de xbox_d3d8 por `SDL_CreateWindow` y obtener HWND de SDL (`SDL_GetWindowWMInfo`).
- xbox_input: añadir backend SDL_GameController (soporta mandos de PlayStation/Switch) + mapeo teclado/ratón (ratón → stick derecho con curva configurable).
- Audio: `SDL_OpenAudioDevice` alimentado por el mezclador de xbox_dsound.

### Fase 4 — Backend Vulkan  *Salida: paridad visual con D3D11 en Vulkan.*
1. Extraer una interfaz `d3d8_backend.h` del código actual (crear buffers/texturas, set de estados, draw, present) — hoy D3D11 y GL están mezclados con la lógica D3D8.
2. Implementar `vk_backend.cpp`: pipelines cacheados por hash de estado (blend/depth/raster + shaders), descriptor sets por draw (o push descriptors), ring buffer para `DrawPrimitiveUP`.
3. Shaders: el generador actual emite HLSL desde microcódigo NV2A / register combiners → compilar a SPIR-V con DXC en runtime (caché en disco) o cambiar el emisor a GLSL + shaderc.
4. Texturas: reutilizar el unswizzle; DXT1/3/5 → BC1/2/3 nativos.
5. Validación: capas de validación Vulkan + RenderDoc.

### Fase 5 — Alta resolución y FPS  *Salida: 1440p/4K estables, 60/120/144 FPS.*
- **Resolución**: el juego crea un backbuffer de 640×480; el backend lo crea a `W×H` real y escala viewports, scissor, render targets (sombra/reflejos con factor independiente) y coordenadas de `DrawPrimitiveUP` pre-transformadas (XYZRHW, usadas por HUD/menús).
- **Aspect ratio**: corregir proyección (parche al cálculo de FOV de la cámara en Stonewall) y HUD anclado a bordes.
- **FPS**: determinar cómo avanza el tiempo el juego (`KeQueryPerformanceCounter`/`KeTickCount`/`Present` con intervalo). Si la simulación va a paso fijo de 30 Hz → interpolar en render o desacoplar; si es paso variable → quitar `PresentationInterval` y limitar con el cap del config.
- Filtrado anisótropo, MSAA (o FXAA/SMAA post), LOD/draw-distance desde los `.env`/`.ren` (texto).

### Fase 6 — Pulido
- Guardado en `%LOCALAPPDATA%\GhostRecon2` (mapeo de la partición `U:`).
- `config.ini`, overlay de depuración (ImGui sobre Vulkan), empaquetado.
- Summit Strike (la otra ISO) como segundo objetivo: mismo motor, misma infraestructura.

## 4. Prioridad de bridges del kernel pendientes (STUB0)
Del listado en KERNEL_IMPORTS.md, los que bloquearán antes:
1. **Sincronización/tiempo**: `NtWaitForSingleObject(Ex)` 233/234, `NtWaitForMultipleObjectsEx` 235, `KeWaitForMultipleObjects` 158, `NtSetEvent` 225, `KeDelayExecutionThread` 99, `NtCreateTimer`/`NtSetTimerEx` 194/229, `KeCancelTimer` 97, `NtDuplicateObject` 197.
2. **Memoria**: `ExFreePool` 17, `MmAllocateSystemMemory`/`MmFreeSystemMemory` 167/172, `MmQueryAllocationSize` 180, `MmQueryAddressProtect` 179, `NtQueryVirtualMemory` 217, `MmClaimGpuInstanceMemory` 168.
3. **Ajustes**: `ExQueryNonVolatileSetting` 24 (idioma, vídeo, región).
4. **Rtl**: `RtlInitAnsiString` 289, `RtlAnsiStringToUnicodeString` 260, `RtlUnicodeStringToAnsiString` 308, `RtlEqualString`/`RtlCompareString` 279/270, `RtlTimeToTimeFields`/`RtlTimeFieldsToTime` 305/304, `RtlUnwind` 312 (SEH), `RtlCompareMemoryUlong` 269.
5. **Secciones**: `XeLoadSection`/`XeUnloadSection` 327/328 (todas las secciones ya están cargadas → devolver éxito y contar referencias).
6. **Crypto** (`Xc*` 335–349): solo para saves firmados / Live. SHA1/HMAC/RC4 reales son necesarios si el juego valida firmas de partidas guardadas.
7. Resto (Io*, Hal PCI/interrupciones, Phy*): stubs con valores coherentes.

## 5. Riesgos conocidos
| Riesgo | Mitigación |
|---|---|
| 8 087 entradas "no detectadas" → stubs vacíos | Instrumentar stubs, iterar `--seed-functions`, regenerar |
| Código LTCG de D3D8 dentro del XBE (sección `D3D`) escribe push-buffers directamente | xbox_nv2a ya intercepta PGRAPH; documentado en `docs/technical/d3d8ltcg-device-context.md` |
| Lógica atada a 30/60 Hz | Medir en Fase 5 antes de tocar |
| Multihilo del motor | Bridges de sync correctos (Fase 1) |
| Área de datos del kernel en `0x00740000` podría solaparse con allocs del juego | Verificar con `MmAllocateContiguousMemory` cuando el juego reserve memoria |

## 6. Flujo de trabajo por sesión
1. Leer `docs/STATUS.md` (no re-leer el XBE ni el código generado).
2. Compilar: `cmake --build build --config Release --target your_game_recomp --parallel`.
3. Ejecutar desde `build/src/game/Release/`: `./your_game_recomp.exe > run.log 2>&1`.
4. Atacar el primer error del log; actualizar STATUS.md al terminar.
