# RECOMP_CONTEXT — Ghost Recon 2 (Xbox) → PC nativo

Documento de traspaso para cualquier modelo/persona que continúe el trabajo. Léelo entero antes de tocar nada. Es la fuente de verdad del *estado actual*; el historial detallado está en `docs/STATUS.md` (sesiones) y `docs/VULKAN_PLAN.md` (renderer). Fecha de este estado: **2026-10-19**.

---

## 0. Qué es esto (en 6 líneas)

- **Juego**: Tom Clancy's Ghost Recon 2 (Xbox, Title ID `0x55530005`, XDK 5849). Imagen original: `C:\Games\XboxGames\Tom Clancy's Ghost Recon 2 (USA, Europe) (En,Fr,De,Es,It).iso`.
- **Método**: *recompilación estática*. El `default.xbe` (x86) se traduce a C con el toolkit **xboxrecomp**; el resultado se compila como un `.exe` de Windows. No es un emulador: la CPU del juego corre nativa. Lo que se emula es el **hardware** (GPU NV2A, audio APU/MCPX, USB/mando) y el **kernel** de Xbox (reimplementado).
- **Objetivo del usuario**: port nativo de PC con alta resolución, ≥60 FPS, sin errores de texturas (lo que xemu/Cxbx no dan). Ruta: ventana SDL2 + renderer **Vulkan**.
- **No se usan BIOS/firmware de Xbox** (mcpx, flash, eeprom, HDD de xemu): no hacen falta para la recompilación y no deben añadirse al proyecto.

---

## 1. Mapa de carpetas

| Ruta | Qué es | Git |
|---|---|---|
| `C:\XboxRecompProject\xboxrecomp` | Toolkit (upstream v0.14.0 + nuestros cambios). Recompilador Python (`tools/`), runtime C (`src/kernel`, `src/apu`, `src/nv2a`, `src/video`, `src/input`…) | rama **`gr2-port`** (no tocar `main`) |
| `C:\XboxRecompProject\ghostrecon2` | Proyecto del juego: `main.c`, parches, capa de port (`src/port`), docs, scripts | repo propio, rama `main` |
| `ghostrecon2\src\recomp\gen\` | **Código C generado** (112 ficheros, 3,3 M líneas). NO editar a mano; se regenera. Ignorado por git | — |
| `ghostrecon2\build\Release\` | `ghostrecon2.exe`, `.pdb`, `.map`, logs, capturas | ignorado |
| `ghostrecon2\build\Release\game\` | Datos del juego extraídos del ISO (`default.xbe`, `video\`, `sound\`…) + `memory.log` (log interno del motor del juego) | ignorado |
| `ghostrecon2\build\Release\video_disabled_boot\` | Vídeos de arranque apartados (logoubi, intro…) para que no bloqueen el arranque. Deben quedar **fuera** de `game\` | ignorado |
| `xboxrecomp\game_files\` | `default.xbe` + logs del pipeline (`p*_*.log`, `regen_*.log`) + `functions_base.json` | — |
| `C:\XboxRecompProject\refs\xemu`, `refs\Cxbx-Reloaded` | Emuladores de **referencia** (solo lectura, GPL: leer/inspirarse, no copiar ficheros enteros) | — |
| `%LOCALAPPDATA%\xboxrecomp\` | Partición/guardados emulados (perfil `U:\…\SaveMeta.xbx`) | — |

---

## 2. Estado exacto (qué funciona)

| Área | Estado |
|---|---|
| Arranque, XAPI, utility drive, índice de recursos (RSMod) | ✅ |
| Menús, perfil de guardado, navegación con mando/teclado | ✅ (USB emulado `RECOMP_USB=1`) |
| Audio (música, efectos, voces) | ✅ funcional, algo saturado/alto → `RECOMP_AUDIO_VOLUME` (lanzadores al 30 %) |
| Vídeo de fondo del menú (`shellmovie.xmv`) | ✅ animado (requiere `RECOMP_VBLANK=1`) |
| Carga de misión, partida, sin crashes en pruebas de 4 min | ✅ (0 ICALL fallidas) |
| **Renderer CPU** (`run_gr2.bat`) | Imagen bastante completa (bosque, personajes, HUD) pero **0–2 FPS** y artefactos (bloques planos, modelos raros) |
| **Renderer Vulkan** (`run_gr2_vulkan.bat`) | Menús/HUD correctos a 200–2000 FPS; cinemática in-engine (cielo, aviones, bosque) con **color correcto**; en partida se ven terreno, edificios, vegetación, **personajes y el personaje principal** a 6–30 FPS. **Problema actual: tinte verde en partida** (ver §4.1) |
| Cinemáticas FMV (`s01_briefing.xmv` etc.) | ⚠️ arrancan, muestran vídeo real y audio a ratos, pero **se pausan/congelan** cada pocos ms-segundos, también en Vulkan (ver §4.2). Los vídeos de arranque con audio siguen apartados |

Capturas de referencia del avance: el usuario las tiene; las mejores son "cielo + aviones + bosque" en Vulkan (28 FPS) y el personaje principal con terreno/edificios.

---

## 3. Cómo se compila y se prueba

### 3.1 Tiempos (importante para planificar)

| Acción | Cuándo hace falta | Tiempo |
|---|---|---|
| **Compilar solo el exe** (`cmake --build ... --target ghostrecon2`) | Cambios en `ghostrecon2/src/*.c`, `src/port/*`, o en el runtime del toolkit (`xboxrecomp/src/**`) | **~10 s–2 min** (solo recompila lo tocado + enlaza) |
| **Recompilar C (paso recomp)** `tools/regen.sh recomp` | Cambias `trace_functions.json`, `manual_functions.json` o el recompilador (`xboxrecomp/tools/recomp/*`) | ~4 min + **~15 min** de build (recompila los 112 chunks) |
| **Regeneración completa** `tools/regen.sh` | Cambias `seed_functions.json` (funciones nuevas) o el desensamblador | **~30 min** (disasm ~12 + recomp ~4 + build ~15) |
| Configurar CMake | Solo si cambias `CMakeLists.txt` | segundos (descarga SDL2/glslang la 1.ª vez; glslang tarda ~10 min en compilar la 1.ª vez) |

**Regla**: un cambio de `target_compile_definitions`/`include_directories` a nivel de *target* en `CMakeLists.txt` obliga a recompilar los 112 chunks (~15 min). Por eso los includes del port van por `set_source_files_properties` en ficheros concretos.

### 3.1b Entorno de terminal (léelo si un comando falla con "No such file")
- La terminal debe ser **Git Bash** (`/usr/bin/bash` de Git for Windows). No WSL (allí `/c/...` no existe, es `/mnt/c/...`), no PowerShell/cmd (no entienden `&&`, `/c/...`, `grep`).
- El workspace del editor puede ser `C:\XboxRecompProject\xboxrecomp` (el toolkit); el proyecto del juego está al lado, en `C:\XboxRecompProject\ghostrecon2`. Usa **siempre rutas absolutas** y lanza los scripts con `bash`: `bash /c/XboxRecompProject/ghostrecon2/tools/run_test.sh 150 vk`.
- Comprobación rápida del entorno: `ls /c/XboxRecompProject/ghostrecon2/build/Release/ghostrecon2.exe` debe existir (el exe YA está compilado; no lo borres ni reconfigures CMake).
- El exe no acepta argumentos (`--version` no existe): todo se configura con variables de entorno.
- **No reescribas `tools/run_test.sh`, `tools/regen.sh`, los `.bat` ni `CMakeLists.txt`.** Si necesitas otras variables, expórtalas antes de llamar al script (las respeta). Si crees que hay que cambiarlos, proponlo al usuario.
- No muevas ni borres `build/Release/game/memory.log`, `run*.log` ni ficheros de `game/`.

### 3.2 Comandos (Git Bash)

```bash
# Compilar (rápido). Cerrar antes ghostrecon2.exe o el enlace falla (LNK1104).
cd /c/XboxRecompProject/ghostrecon2
cmake --build build --config Release --target ghostrecon2 -- -m:8 -v:m 2>&1 | grep -E " error |ghostrecon2.exe"

# Prueba automática (mudo, mando por guion, resumen al final). Avisar al usuario: la ventana se le abre.
tools/run_test.sh 150 vk      # Vulkan
tools/run_test.sh 180 cpu     # referencia CPU (lento)

# Regenerar
tools/regen.sh recomp         # ~20 min
tools/regen.sh                # ~30 min
```

Lanzadores del usuario: `run_gr2.bat` (CPU, ventana SDL), `run_gr2_vulkan.bat` (Vulkan). Logs: `build/Release/run.log` / `run_vk.log`. **No cambiar su comportamiento** sin avisar.

### 3.3 Guion de mando para pruebas

Solo pulsaciones **A** (no START): ~8 A en ~15 s llevan de la pantalla de título a la misión (título → menú → campaña → briefing → equipo → misión). Por si alguna A cae en una pantalla de carga, se añaden unas tardías (inofensivas en partida). Formato: `RECOMP_PAD_SCRIPT=8000:a,10000:a,...` (ms desde el arranque). Ya viene por defecto en `tools/run_test.sh`. La misión tarda ~60–120 s en estar jugable.

### 3.4 Sonido en pruebas
Siempre `RECOMP_AUDIO_VOLUME=0` en pruebas automáticas (lo pone `run_test.sh`).

---

## 4. Problemas abiertos (por prioridad)

### 4.1 Tinte verde en partida (Vulkan) — PRIORIDAD 1
- La escena se dibuja bien (verificado: se ve en las superficies volcadas). GR2 aplica al final una **corrección de color**: genera cada frame un LUT 256×256 en `0x00A17000` a partir de una tira 256×8 (`0x82711000`, fmt 0x06) con 4 etapas + combinadores, y luego la pasada final consulta el LUT con *texture shader* **DEPENDENT_AR** (u=alfa, v=rojo de la escena) y **DEPENDENT_GB**.
- El LUT ya sale correcto (verde en x, magenta en y). El verde final indica que el **alfa de la escena** (`0x00670000`/`0x0079C000`) llega demasiado alto en la GPU. En CPU la imagen no salía verde.
- Siguiente paso: volcar el alfa de la escena en CPU y en Vulkan en el mismo punto (`RECOMP_VK_DUMP_SURFACES=1` escribe `*_a.bmp`) y localizar qué pasada escribe alfa distinto (blend de alfa, máscara de color, "blue-to-alpha" de los combinadores, final combiner `G`). Referencia: xemu `hw/xbox/nv2a/pgraph/glsl/psh.c` (cómo trata alfa/final combiner) y `vk/draw.c` (blend separado de color/alfa).

### 4.2 Cinemáticas FMV que se pausan — PRIORIDAD 2
- Mecanismo medido con el perfilador: el reproductor XMV (sección `XMV` del XBE, `0x4BFF40..0x4C8180`) decodifica en CPU y espera a la GPU (`D3D_BlockOnTime` en `gr2_d3d.c`, KickOff `sub_0042F790`). Mientras suena la cinemática el juego sigue dibujando 3D de fondo; si la GPU (CPU-raster o Vulkan) va lenta, las *fences* llegan tarde, el decodificador no pide más datos (`[READ]` deja de aparecer) y el audio del stream (voz APU 216) se queda "ran dry".
- En Vulkan va mejor pero aún se pausa → mirar `[D3D] N fence waits timed out` en el log y el perfil `RECOMP_SAMPLE=2` durante la cinemática.
- Herramientas: `RECOMP_APU_STREAM_TRACE=1`, `RECOMP_TRACE_FLAG=trace.flag` (traza XMV solo mientras exista el fichero), `RECOMP_SAMPLE=2 [RECOMP_SAMPLE_THREAD=pb]` + `python tools/samples.py build/Release`.

### 4.3 Otros
- Modelos/terreno con artefactos residuales en Vulkan (texturas "cromadas", bloques); mipmaps no implementados (texturas con ruido a distancia); cube maps simplificados; niebla aproximada.
- Algunas subidas de textura se perdían al ir en lote en el command buffer del frame → ahora se envían una a una (`upload_texture`, "ponytail"). Entender la causa real.
- Ventana SDL: OK. Pantalla completa F11/Alt+Enter.
- Rendimiento Vulkan en partida 6–30 FPS: el cuello es la CPU (vertex programs ejecutados en CPU: `nv2a_vsh_run`, recorrido de pushbuffer). Fase 2f del plan: vertex programs en GPU.

---

## 5. Problemas de bajo nivel encontrados (y cómo se resolvieron) — **lecciones**

1. **`switch` con >256 casos** (`sub_0024A2B0`, `cmp eax,0x128`, tabla `0x251D70`): el recompilador truncaba a 256 → crash en partida. Arreglo: `MAX_JUMP_TABLE_ENTRIES=1024` en `tools/recomp/lifter.py`/`translator.py` + test `test_long_jump_table.py`. **Lección: antes de añadir una "semilla" por un `[ICALL] Failed 0xXXXX`, comprobar si la dirección cae DENTRO de una función existente (`game_files/functions_base.json`); si es así es un caso de `switch`, no una función.** Sembrar casos de switch rompe el juego.
2. **Semillas válidas** (`seed_functions.json`): solo direcciones que empiezan tras `ret`/relleno o que el log demuestra como destino de llamada indirecta. Ej.: ramas del despachador x87 del CRT `0x13542C`, `0x1354CE`.
3. **Pushbuffer (comandos GPU)** — la fuente de la mayoría de artefactos:
   - El ejecutor (`src/kernel/nv2a_pb_scan.c`) va **por detrás** de la CPU del juego.
   - Un PUT puede caer a mitad de un método → el método pendiente se arrastra (`s_seg_pend`).
   - D3D ejecuta *pushbuffers grabados* (modelos) con JUMP de ida y JUMP de vuelta que **D3D reescribe en cada uso** → el ejecutor lee el retorno de otra llamada. Arreglo: recordar el punto de salida del anillo; D3D deja la dirección de retorno real en la palabra siguiente al JUMP (≤64 bytes después). (`pb_follow`, `pb_in_ring`.)
   - Dos *devices* D3D (splash de arranque en `0x2000`, juego en `0x42E000`): cuando no hay camino entre PUTs se hace un paso en seco (`s_dry`) y no se ejecuta nada (`[PB] no path`).
   - Límites: todo recorrido se acota a los 64 MB de RAM (un CALL/JUMP basura provocó un AV).
4. **Direcciones**: DMA_PUT/GET son **físicas**; la ventana contigua `0x80000000 | phys` es la vista física. Superficies/texturas: `dma_resolve()` en `nv2a_pb_exec.c`. Compara siempre con `& 0x0FFFFFFF`.
5. **Recursión → desbordamiento de pila nativa (0xC00000FD)**: no deja `[CRASH]` en el log. Diagnóstico: Visor de eventos de Windows (Application, id 1000, "Desplazamiento de errores") + resolver el offset con `build/Release/ghostrecon2.map` (`tools/samples.py` trae el parser). Caso real: `D3D_BlockOnTime` ↔ KickOff (guardia `s_in_kick`).
6. **Escapes `\n` en C**: al parchear ficheros con heredocs de bash/python, `\n` dentro de cadenas C se convierte en salto de línea real → error C2001. Usar `python tools/fixnl.py <fichero.c>` o editar con herramienta de edición, no con heredoc.
7. **Ficheros CRLF**: casi todo el toolkit es CRLF. Al parchear con Python, leer en binario y respetar `\r\n`.
8. **Atributos de vértice sin array** → usan el último valor fijado (`imm_attr`), como xemu `inline_value`.
9. **GPU NV2A**: front face sin invertir (CW 0x0900 → `VK_FRONT_FACE_CLOCKWISE`); superficies *swizzled* (tipo 2) para render-to-texture; DEPENDENT_AR/GB; motor 2D NV062/NV09F (blit); profundidad entera Z16/Z24 para tests EQUAL.

---

## 6. Archivos clave (qué hace cada uno)

### Proyecto `ghostrecon2`
| Archivo | Qué hace |
|---|---|
| `src/main.c` | Arranque (plantilla toolkit): carga XBE, init APU/USB, **arranca ventana SDL y renderer Vulkan**, perfilador `RECOMP_SAMPLE`, handler de crash con pila nativa |
| `src/gr2_d3d.c` | Sustituye `D3D_BlockOnTime` (`0x0042F970`): espera honesta a fences con KickOff + guardia de recursión, timeout 250 ms |
| `src/port/vk_window.c` | Ventana SDL2 (hilo propio), instancia/dispositivo Vulkan 1.3, swapchain, teclado, F11; presenta frame CPU o imagen GPU |
| `src/port/vk_backend.c` | **Renderer Vulkan** (`Nv2aBackend`): superficies por dirección guest, caché de texturas, pipelines por estado, uber-shader de combinadores (GLSL embebido), clear/draw/blit/present, herramientas de depuración `RECOMP_VK_*` |
| `src/port/vk_glsl.c/.h` | GLSL → SPIR-V en runtime (glslang) |
| `src/port/vk_ctx.h` | Contexto Vulkan compartido ventana↔renderer, lock de cola |
| `manual_functions.json` | Funciones escritas a mano (hoy: `0x0042F970`) |
| `seed_functions.json` | Semillas de funciones para el desensamblador (24) |
| `trace_functions.json` | Funciones con traza compilada (hoy: 73 del XMV/DSOUND; silenciadas con `RECOMP_TRACE_BUDGET=0`) |
| `run_gr2.bat`, `run_gr2_vulkan.bat` | Lanzadores del usuario |
| `tools/run_test.sh`, `tools/regen.sh`, `tools/samples.py`, `tools/fixnl.py` | Prueba automática, regeneración, análisis del perfilador, reparación de `\n` |
| `docs/STATUS.md`, `docs/VULKAN_PLAN.md`, `PLAN.md` | Historial, plan Vulkan, plan general |

### Toolkit `xboxrecomp` (rama `gr2-port`)
| Archivo | Qué hace / qué cambiamos |
|---|---|
| `src/kernel/nv2a_pb_exec.c` | Ejecutor de comandos GPU + rasterizador CPU. Añadido: motor 2D, DEPENDENT_AR/GB, superficies swizzled, profundidad entera, `draw_xf` (entrega vértices sin recortar + estado de píxel al backend), trazas `zfail/tfail/rgb` |
| `src/kernel/nv2a_pb_scan.c` | Recorrido del pushbuffer: `pb_follow`, pasada en seco, pushbuffers grabados, ack de kickoffs |
| `src/kernel/nv2a_backend.h` | Interfaz del backend (ampliada: `Nv2aVertexXf`, `Nv2aPixelState`, `draw_xf`, `blit`, `present`) |
| `src/kernel/xbox_memory_layout.c` | Hilo de sondeo NV2A (llama a `nv2a_pb_scan_follow`) |
| `src/kernel/recomp_trace.c` | `RECOMP_TRACE_FLAG` |
| `src/kernel/kernel_bridge.c`, `kernel_file.c` | FSCTL, FATX, máscara vacía en directorios (sesiones anteriores) |
| `src/apu/apu_core.c`, `apu_vp.c`, `apu_state.h` | `RECOMP_AUDIO_VOLUME`, `RECOMP_APU_STREAM_TRACE`, contadores |
| `src/video/fb_present.c`, `video_player.h` | Hook `xbox_FramebufferSetPresentHook` |
| `tools/recomp/lifter.py`, `translator.py`, `test_long_jump_table.py` | Tablas de salto de >256 entradas |

---

## 7. Variables de entorno útiles

`RECOMP_PB_EXEC=1` (obligatoria) · `RECOMP_WINDOW=sdl` · `RECOMP_RENDERER=vk` · `RECOMP_USB=1` · `RECOMP_VBLANK=1` · `RECOMP_AC97_READY=1` · `RECOMP_AUDIO_VOLUME=0..100` · `RECOMP_PAD_SCRIPT=ms:boton,...` · `RECOMP_TRACE_BUDGET=0`

Diagnóstico: `RECOMP_FRAME_TRACE=<flag>` (+`_FRAMES=n`, `_METHODS=1`) traza draws cuando existe el fichero · `RECOMP_VK_DUMP=prefijo` `RECOMP_VK_DUMP_EVERY=n` (capturas GPU) · `RECOMP_VK_DUMP_SURFACES=1` · `RECOMP_VK_WATCH=<va>` `RECOMP_VK_WATCH_DUMP=n` `RECOMP_VK_WATCH_TEX=<va>` · `RECOMP_VK_DUMP_TEX=<va>` · `RECOMP_VK_FS=magenta|d0|t0` · `RECOMP_VK_NOCULL=1` · `RECOMP_SAMPLE=2` `RECOMP_SAMPLE_THREAD=pb` · `RECOMP_APU_STREAM_TRACE=1` · `RECOMP_TRACE_FLAG=<flag>` · `RECOMP_Z_FLOAT=1` · `RECOMP_FB_DUMP=x.bmp` (CPU) · `RECOMP_WATCHDOG_SECS=n` (mata el proceso, exit 3)

---

## 8. Reglas de trabajo (no romper lo que funciona)

1. **Antes de cambiar algo, haz una prueba de referencia** (`tools/run_test.sh 150 vk`) y guarda el resumen. Después del cambio repite y compara: crashes, ICALL, fence timeouts, FPS, capturas.
2. **No cambies varias cosas a la vez** en el renderer: un cambio → compilar → probar.
3. **No edites `src/recomp/gen/`**. Para cambiar una función del juego: `manual_functions.json` + C a mano (ver `gr2_d3d.c`).
4. **No toques `main` del toolkit**; trabaja en `gr2-port`. Commits pequeños con mensaje claro; no hagas `push` ni `reset --hard` sin permiso del usuario.
5. **No quites diagnósticos que silencian errores** para "arreglar" síntomas (regla del usuario: arreglar la causa raíz).
6. Si la tarea requiere rediseño de arquitectura del renderer, de la interfaz backend, o lleva >2 intentos fallidos con la misma hipótesis: **parar y pedir al usuario que active a Claude**.
7. No añadir BIOS/firmware ni ficheros de Microsoft.
8. Mantén `docs/STATUS.md` y `docs/VULKAN_PLAN.md` al día al final de cada sesión (qué se cambió, qué se midió, qué falta).

## 9. Consejos para ahorrar tokens
- No leas ficheros enteros grandes (`nv2a_pb_exec.c` ~4.5k líneas, `kernel_bridge.c` ~10k): usa `grep -n` y lee rangos.
- Los logs son enormes (MB): filtra siempre (`grep -c`, `grep ... | head`, `tail`). Nunca vuelques un log entero.
- Para ver imágenes: no las describas tú si no puedes verlas; el usuario las pasa por Gemini (descripción técnica). Pide capturas concretas (`tst_*.bmp`, `*_a.bmp`).
- Para mirar el código del juego: desensamblar con capstone desde `xboxrecomp/game_files/default.xbe` (ver STATUS.md) en vez de leer `gen/` (3 M líneas).
