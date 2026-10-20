# Ghost Recon 2 — Estado / bitácora

Leer esto al empezar cada sesión. Plan general: [PLAN.md](../PLAN.md).

## Rutas clave (desde 2026-10-18, toolkit en upstream v0.14.0)
| Qué | Dónde |
|---|---|
| ISOs | `C:\Games\XboxGames\` (GR2 + Summit Strike) |
| Toolkit (pipeline Python + librerías runtime) | `C:\XboxRecompProject\xboxrecomp` |
| Copia del XBE para el pipeline | `xboxrecomp/game_files/default.xbe` (+ `gr2_analysis.json`) |
| **Proyecto del juego** | `C:\XboxRecompProject\ghostrecon2` (este repo) |
| Código generado | `ghostrecon2/src/recomp/gen/` (no editar a mano) |
| Ejecutable + datos | `ghostrecon2/build/Release/ghostrecon2.exe`, datos en `build/Release/game/` |
| Logs | `build/Release/run.log` (stderr), `build/Release/xbox_kernel.log` |
| Discos virtuales / saves | `%LOCALAPPDATA%\xboxrecomp\` (`PartitionN.img`, `UDATA`, `TDATA`) — cambiable con `RECOMP_SAVE_DIR` |
| Trabajo antiguo (pre-actualización) | `xboxrecomp/backup/` |

## Comandos
```bash
# desde xboxrecomp/
python -m tools.xiso unpack "<iso>" -o ../ghostrecon2/build/Release/game
python -m tools.xbe_parser game_files/default.xbe --json game_files/gr2_analysis.json
python -m tools.disasm game_files/default.xbe --force --seed-functions ../ghostrecon2/seed_functions.json   # SIN --text-only (secciones XDK)
# tras una ejecución con "[ICALL] Failed": añadir semillas y repetir disasm..recomp
python -m tools.seed_from_log <run.log> game_files/default.xbe --functions tools/disasm/output/functions.json --seeds ../ghostrecon2/seed_functions.json --analysis-json game_files/gr2_analysis.json
python -m tools.func_id game_files/default.xbe -v
python -m tools.abi_analysis game_files/default.xbe -v
python -m tools.recomp game_files/default.xbe --all --split 250 --gen-dir ../ghostrecon2/src/recomp/gen --manual-functions ../ghostrecon2/manual_functions.json
# desde ghostrecon2/
cmake -S . -B build && cmake --build build --config Release --parallel
cd build/Release && RECOMP_PB_EXEC=1 RECOMP_FB_WINDOW=1 ./ghostrecon2.exe > run.log 2>&1
```

## Sesión 2026-10-18 (parte 2) — tras actualizar a v0.14.0
- Proyecto `ghostrecon2/` creado desde `templates/new-game`. Pipeline nuevo con `--text-only`: 24 588 funciones, 0 fallos, 510 stubs (antes 8 087). Compila a la primera. 150/150 imports con bridge.
- **Bloqueo 1, resuelto (toolkit):** `XMountUtilityDrive` formatea la partición de caché y luego hace `NtFsControlFile(FSCTL_DISMOUNT_VOLUME)`, que era un stub → **implementado** lock/unlock/dismount (`kernel_bridge.c`).
- **Bloqueo 2, resuelto (toolkit):** después consulta `FileFsSizeInformation` y exige el clúster pedido en las init flags del XBE (`0x80000005` → **64 KB**). El kernel decía 16 KB fijo y además abría la carpeta en vez de la imagen → `STATUS_UNRECOGNIZED_VOLUME` → el juego se relanzaba (`HalReturnToFirmware(2)`). **Arreglo** en `kernel_file.c` (Windows): la apertura de volumen usa la propia `PartitionN.img` en solo lectura, y la consulta lee el clúster de su superbloque FATX. (Falta replicarlo en la rama POSIX.)
- **Resultado:** el juego pasa la init de XAPI y sigue ejecutándose (60 s hasta el timeout).
- **Bloqueo 3, en curso:** bucle infinito de `KeDelayExecutionThread` (>400 M llamadas) + ICALL fallidas a DSOUND/XPP/XNET/XONLINE → el código de las secciones XDK no estaba recompilado. Regenerando **sin `--text-only`**.
- `xboxdash.xbe` falso ya no hace falta (apartado en `xboxrecomp/backup/fake_dash`).

### Sesión 2026-10-18 (parte 3) — secciones XDK + GPU
- Pipeline **sin `--text-only`**: 27 669 funciones (`.text` 23 517, XONLINE 907, XGRPH 678, D3DX 581, DSOUND 465, XNET 430, D3D 294, XPP 257, XMV 64, DOLBY 2), 0 fallos, **12 stubs**. Desaparecen todas las ICALL fallidas.
- El motor arranca: `game/memory.log` (log propio de Stonewall) muestra `Main: begin` y los pools; carga `d:\shell\art\splash.xpr`.
- Luego giraba sin llamadas al kernel = espera de GPU. **Ruta de GPU para GR2: ejecutor de push-buffer** (D3D8 del XDK recompilado), no el HLE `xbox_d3d8`. Se activa con variables de entorno, sin código en `main.c`:
  `RECOMP_PB_EXEC=1 RECOMP_FB_WINDOW=1` (+ `RECOMP_WATCHDOG_SECS=N`, `RECOMP_NV2A_TRACE=1`, `RECOMP_FB_DUMP=x.bmp` para depurar).
- Con eso: **se abre la ventana (640×480) y se ejecuta el primer clear del juego.**
- Direcciones D3D de GR2 (ver `src/gr2_d3d.c`): device ptr global `0x0043A1B8`; `D3D_BlockOnTime(fence,flags)` `0x0042F970` (ret 8); `KickOff(flags)` `0x0042F8B0` (ret 4); espera de registro `0x400B10` en `0x0042F6B0` (la modela el ejecutor).
- `D3D_BlockOnTime` **reemplazada** (versión "honesta" del doc `d3d8ltcg-device-context.md`): pasa el semáforo a `nv2a_pb_set_semaphore_target`, hace kickoff y espera ≤250 ms. Excluida del generado con `manual_functions.json` → **regenerar siempre con `--manual-functions ../ghostrecon2/manual_functions.json`**.
- **HITO: el splash de Ghost Recon 2 se renderiza correctamente** (`build/Release/fb_dump_view.png`): 3 326 draws, 6 652 triángulos en 20 s, sin `bad target`. Lanzador: `run_gr2.bat`.
- Después del splash: lee `commandLine.txt` (no existe, opcional), monta caché en `Z:\mission`, `Z:\shell\art\...`, busca `T:\PlatformOptions.dat` (no existe en 1er arranque) y **se queda en el splash** ≥75 s. La GPU sigue avanzando (DMA_PUT crece, frames continuos); llamadas al kernel ~3/s, sobre todo `KeStallExecutionProcessor` (151) y `KeRaiseIrqlToDpcLevel` (129).

### Sesión 2026-10-18 (parte 4) — "A problem has occurred" resuelto
- Síntoma: tras el splash, pantalla XAPI "A problem has occurred. Please restart your Xbox." (`0x24D30`, imagen de la sección `discerr`).
- Cadena (encontrada con `game/memory.log` del motor + `--trace-functions` + envoltorio `src/gr2_debug.c`):
  `IkeUIMgr::Initialize` (`0x7ACE0`) → `XGetLanguage`=1 → `RSModSystem` find `strings_x.res` (`0x106E80`→`RSMod::FindFile 0x10A4C0`, `lower_bound` sobre índice `[mod+0x14,mod+0x18)`) → **índice vacío** → `0x7C7C0(2,"Could not load string resource %s")` → `0x88EB0` → pantalla de error.
- Causa raíz (toolkit, `kernel_file.c`): `RSMod` indexa el disco con `FindFirstFile("d:\")` → `NtQueryDirectoryFile` con **máscara vacía (Length 0)**; el kernel la añadía tal cual (`game\` + `""`) → Windows err 2 → `STATUS_NO_MORE_FILES` → disco "vacío". **Arreglo:** máscara vacía = todo (`*`). Añadido también log `[DIR] FindFirstFile ... -> none` cuando una búsqueda no encuentra nada.
- **Resultado: `Main: Game started`.** Copia `.xwb` a la caché `Z:` (1er arranque), lee `campaign.xml`, `elorankings.xml`, abre `d:\video\logoubi.xmv` (intro Ubisoft) y dibuja el logo grande de GR2 (`build/Release/fbx_last.png`).
- Herramientas de depuración útiles (ver PLAN): `RECOMP_KERNEL_LOG_BUDGET=100000` (todas las llamadas al kernel), `--trace-functions trace_functions.json`, envoltorios `extern void sub_X_gen(void)` en `src/gr2_debug.c` + `--exclude-manual ../ghostrecon2/src/gr2_debug.c`.
- Pendiente de limpiar: el envoltorio de `0x10A4C0` en `gr2_debug.c` (inofensivo, imprime 4 veces).
- Ventana GDI de depuración: se cuelga al maximizar y no limita FPS — se sustituirá por SDL2 (Fase 3), no se arregla.

### Sesión 2026-10-18 (parte 5) — bucle de UI, crash, audio
- FPS 0 en la pantalla del logo: 227 000 `[ICALL] Failed` a `0x0007ACD0` (método de UI llamado cada frame) + `0x002D6A20` (sonido) + `0x005061A0`/`0x00506BA7` (XPP). Resuelto con `tools.seed_from_log` → `seed_functions.json` → disasm..recomp. Sin ICALL fallidas desde entonces.
- Ahora: ~115-120 FPS, carga `d:\media\stonewall.uix` (UI de menús), pantalla casi negra con parpadeos del logo.
- Crash intermitente visto 1 vez: AV dentro de ntdll (heap) en un hilo nativo (regs Xbox = 0). Para la próxima vez: PDB activado para runtime+host (CMake: `/Zi` + `/DEBUG:FULL`) y el manejador de crash de `main.c` ahora recorre la pila nativa (`StackWalk64`, módulo+offset+símbolo). No se reprodujo en 2 ejecuciones de 75-90 s.
- **Audio**: la plantilla solo arranca el APU con `RECOMP_AC97_READY=1` (añadido a `run_gr2.bat`). Con él: `[APU] started by the title`, backend XAudio2, y **suenan ~100 ms de la intro de Ubisoft**; luego silencio y negro.
- Hipótesis actual: el reproductor XMV (`d:\video\logoubi.xmv`) envía los primeros paquetes de audio al APU y espera la notificación de "paquete consumido" (stream de DSOUND / interrupción del APU) para seguir leyendo y decodificando; esa notificación no llega → solo 9 lecturas del fichero tras abrirlo, vídeo congelado en su 1er frame (negro).
- Ventana GDI: al cerrarla se vuelve a abrir (la recrea `AvSetDisplayMode`/el ejecutor) y el proceso sigue vivo → cerrar `ghostrecon2.exe` desde el Administrador de tareas. Se reemplazará por SDL2.

### Sesión 2026-10-18 (parte 6) — investigando vídeo XMV + audio
- Todos los vídeos del juego (98 `.xmv`: intro, `shellmovie*` de los menús, `outro_*`, `dvdextras`) usan el mismo reproductor XMV → arreglarlo una vez vale para todos.
- Instrumentación añadida al toolkit (`src/apu/`): resumen `[APU] irq totals` cada 2 s y contador `notifies` en la línea `[APU-IRQ]` (`RECOMP_APU_TRACE=1`).
- Medido: interrupciones del APU **continuas** (~20/s, todas `claimed`, es el tick SSL del secuenciador) → la entrega de IRQ funciona. Notificaciones de voz: **92 al principio y luego congeladas** → la voz del stream reproduce los paquetes iniciales (~100 ms) y no recibe más.
- Vídeo: tras abrir `logoubi.xmv` solo lee cabecera (4 KB) + 2 paquetes (2×45 KB) y nunca pide el 3º.
- Descartado (probablemente): traducción física del APU (`mcpx_apu_phys`), upstream tiene audio funcionando en *Black* (mismo XDK 5849).
- Traza de las 64 funciones XMV: el decodificador **sí decodifica** continuamente. Orden real de vídeos: `shellmovie.xmv` (logo animado de GR2, **sin audio, se ve bien**: es el "logo grande", 1 draw = quad texturizado) → precarga `shellmovie_sp/mp.xmv` → `logoubi.xmv` (**con audio**: se queda negro).
- Contadores APU (`RECOMP_APU_TRACE=1`, línea `[APU-IRQ]`, añadidos en `src/apu/apu_core.c`/`apu_vp.c`/`apu_state.h`): `ssl_sets 4` (solo 4 paquetes de stream encolados en total), `stream_frames 531` (~0,35 s) y luego se congela; `stream_offs 0`, `stream_paused 0` → la voz de stream **sale de la lista de voces activas** sin apagarse ni pausarse, y el juego no encola más. El vídeo con audio se sincroniza con ese reloj → congelado en el 1er frame (negro).
- Conclusión: problema en la interacción XMV ↔ DSOUND (recompilados) ↔ APU emulado. Requiere seguir el lado guest (quién desenlaza la voz: métodos de lista de voces del FE / `SET_ANTECEDENT_VOICE`, o el XMV parando el stream).
- Mientras tanto: `run_gr2.bat` lleva `RECOMP_MKB=1` (Enter/Esc=START, Espacio=A) para intentar saltar la intro y ver si el menú (vídeos sin audio) funciona.
- Nota: la build actual aún tiene trazas XMV compiladas (silenciar con `RECOMP_TRACE_BUDGET=0`; quitar regenerando con `trace_functions.json` = `[]`).

### Sesión 2026-10-18 (parte 7) — ¡pantalla de título!
- Workaround vídeos con audio: apartados los vídeos de arranque (`logoubi, logoredstorm, intro*, attract*, esrb*`) a `build/Release/video_disabled_boot/` (**fuera** de `game/`: el índice RSMod busca por nombre en todo `d:\`). La secuencia de `MovieScreen` (`0x2F2BB0` entrar, `0x2F2D00` siguiente/`SkipToMainMenu`; tabla de vídeos en `.data` `0x5B132C`) los da por terminados.
- **Resultado: "Please press START to begin."** — arte de los Ghosts + logo + texto de UI renderizados (~90 FPS).
- Entrada: el XPP del juego (recompilado) habla con el USB emulado (OHCI), no con XInput. La plantilla **nunca llamaba `xbox_OhciInit()`** → añadido en `main.c` tras el APU; requiere `RECOMP_USB=1` (añadido a `run_gr2.bat`). Con ello: 2 controladores OHCI, mando en HC0 puerto 1, ISR atendidas.
- Pero la enumeración USB se para (`setups 4`, `reports 0`) con `[ICALL] Failed 0x005084EC` (callback de XPP) → semilla añadida a `seed_functions.json`, pipeline en curso.
- Con la semilla `0x005084EC`: **el mando funciona** (`reports 4145`); el START se acepta (texto "Press START" queda blanco fijo) y luego se atasca en `[ICALL] Failed 0x00507A87` (XPP) → semilla.
- Semillas "proactivas" (punteros en datos a código XDK no detectado tras `ret`/relleno): de 48, **34 caían dentro de funciones existentes** (tablas de salto de `switch`, sobre todo D3D) y partirlas rompió el arranque (crash en `XBAudioRenderer::InitializeVoiceQs`, esp corrupto). **Regla:** validar candidatas contra un `functions.json` generado SIN ellas (`game_files/functions_base.json`) y quedarse solo con las que caen en huecos (14). Ficheros: `seed_functions.json` (buenas), `seed_candidates*.json` (análisis).
- **HITO (20 semillas válidas): ¡partida en 3D!** Con `RECOMP_PAD_SCRIPT=8000:start,12000:start,16000:a,20000:a,24000:start,28000:a`: START → crea perfil (`U:\132F12855037\SaveMeta.xbx`) → menús (`ui_startbkgd_us.xpr`, `squadstatus.xpr`, `dash_dossier.xpr`) → `HandleLoadMission: Done loading mission` → `HandleGameStateChange: change to Action`. Fotogramas: terreno, cielo, bosque (`build/Release/fbr_1.png`, `fbr_2.png`).
- Audio mejor tras las semillas DSOUND/XMV: `ssl_sets 16`, `stream_frames 7695` (antes 4 / 531).
- **Crash en partida** (~70 s): AV leyendo `0xC2D3B1ED` (ecx basura `0xC2D3B1E9`), pila guest `0x88C71 ← 0x1D977D ← 0x89A07 ← … 0x17CC7`, y antes `[ICALL] Failed 0x00050660` ×4 (dirección en `.text`, posible puntero/vtable corrupto o función no detectada). Siguiente a investigar.
- Herramientas: `RECOMP_USB_STATS=1` (línea `[OHCI0] stats`), `RECOMP_PAD_PRESS=0x10` (pulsa START cada 2 s), `RECOMP_PAD_SCRIPT=...`, `RECOMP_INPUT_DIAG=1`.

### Sesión 2026-10-18 (parte 8) — crash en partida y gráficos 3D en pantalla
- **Crash en partida = bug del recompilador**, no del juego: el `switch` de `sub_0024A2B0` (`cmp eax, 0x128` → 297 casos, tabla `0x251D70`) se leía con tope de **256 entradas** (`tools/recomp/lifter.py` `_read_jump_table`, `translator.py` ×2) → los casos 256..296 caían en ICALL sin resolver (`0x251827`, `0x251CAA`, `0x251C1E`, `0x24B7A0`). Arreglo: `MAX_JUMP_TABLE_ENTRIES = 1024` (lifter) + test `tools/recomp/test_long_jump_table.py`. **Quitadas** esas 3 semillas (partían la función y empeoraban: tabla truncada a 23). Regla: antes de sembrar un ICALL fallido, comprobar si cae DENTRO de una función de `functions_base.json` → entonces es un caso de switch, no una función.
- Semillas `0x13542C`/`0x1354CE`: ramas del despachador x87 del CRT (`_trandisp`, `jmp [ebx]` en `0x135419`, tabla `0x57D638`); empiezan tras `ret`, válidas.
- **Pantalla negra en partida (solo HUD)**: el juego alterna flips "escena" (≈1090 draws a su RT 0x670000/0x79C000) y "post-proceso" (glow + corrección de color + HUD sobre el mismo RT). La corrección de color usa texture shader `0x041E1` = PROJECT2D + **DEPENDENT_AR/GB** (LUT 256x256 en 0xA17000) que el rasterizador devolvía como 0 → negro. Implementado en `src/kernel/nv2a_pb_exec.c` (`rc_stage_fetch`, `rc_input_stage`).
- **Colores verde/magenta**: el LUT se renderiza cada frame en una superficie **swizzled** (`SET_SURFACE_FORMAT` tipo 2) y el rasterizador la escribía lineal → `put_pixel` direcciona en Morton para tipo 2. Colores correctos (cielo, bosque, menús, vídeo de fondo del menú).
- **Motor 2D**: implementado `NV062` surfaces2D (subcanal 3) + `NV09F` image blit SRCCOPY (subcanal 2) en el ejecutor. GR2 copia 640x480 `0x84000 → 0x2FC040` por frame.
- Diagnóstico nuevo: `RECOMP_FRAME_TRACE_FRAMES=n` (traza n frames seguidos), `RECOMP_APU_STREAM_TRACE=1` (eventos de voces stream: SSL, done, ran dry, idle).
- **Parpadeo del logo ARREGLADO** (`src/kernel/xbox_memory_layout.c`, scan por segmentos): en cada vuelta del anillo de pushbuffer el escáner reanudaba en "el PUT más bajo jamás visto" = `0x2000`, que es el anillo del **splash de arranque** (`sub_000591A0` crea su propio device y dibuja `splash.xpr`) → re-ejecutaba init de D3D + logo y se presentaba entre frames. Ahora reanuda en el destino del JUMP de fin de vuelta (`nv2a_pb_segment_jump`, `src/kernel/nv2a_pb_scan.c`) o, si no se encuentra (el scan empieza a mitad de método y lee vértices como JUMP), en el último destino bueno / el propio PUT. Anillo de partida: `0x42E000..~0x66xxxx`. Medido: 0 de 23 frames con logo (antes 7/29).
- Test de 4 min en partida tras p10: **0 ICALL fallidas, 0 crashes**.
- **Bucle de 2 imágenes congeladas** (reportado por el usuario tras el arreglo del logo: alterna una vista con barras de cine y otra sin ellas, idénticas cada vuelta, el audio sigue). Causa: el scan por segmentos terminaba en PUT aunque PUT cayera **a mitad de un método** (lista larga de parámetros: vértices inline, constantes) y olvidaba el método; el segmento siguiente leía el resto de parámetros como cabeceras. En el wrap, el JUMP real no se reconocía (todas las líneas `[PB] wrap ... lap JUMP <basura> unusable`) y se re-ejecutaba la vuelta anterior del anillo. **Arreglo** (`src/kernel/nv2a_pb_scan.c`, `s_seg_pend` + `pb_seg_params`): el método pendiente se arrastra al siguiente segmento, como ya hacía el DMA walk (`s_pend_*`). Compilado (2026-10-18 23:29), **SIN PROBAR**. Probable efecto secundario positivo: modelos rotos (personajes, aviones, edificios) si era por vértices/estado perdidos.
- Pendiente: (a) Verificar el arreglo anterior. (b) Bloques grises en árboles / modelos rotos. (c) Vídeo de fondo del menú: el usuario lo vio animado en una build y estático en la siguiente (¿regresión o temporización?; mismo reproductor XMV que las cinemáticas). (d) Cinemáticas con audio (XMV + stream APU; usar `RECOMP_APU_STREAM_TRACE=1`). (e) 1–4 FPS → backend Vulkan.

### Sesión 2026-10-19 — pushbuffer: seguir los JUMP (aparecen los soldados)
- Prueba del usuario con el arreglo de "método pendiente": sin crashes ni ICALL, logo eliminado, pero aún algunos bucles y **modelos (personajes, aviones) invisibles**. Todas las vueltas: `[PB] wrap ... lap JUMP <x> unusable`.
- Diagnóstico (`nv2a_pb_segment_dump`): la decodificación SÍ estaba sincronizada; el "JUMP basura" (`0x021E5961` en `0x66DBC8`) era un **JUMP real a un pushbuffer grabado** (XDK `RunPushBuffer`: el anillo salta al buffer grabado y éste salta de vuelta). El scan por segmentos tomaba TODO JUMP como fin de anillo → perdía los modelos grabados (personajes/vehículos), el resto del segmento, y en el wrap confundía el salto → bucles.
- **Arreglo** (`src/kernel/nv2a_pb_scan.c` `nv2a_pb_scan_follow` / `pb_follow`; `xbox_memory_layout.c` llama a eso en lugar del scan por segmentos): recorrer desde el PUT anterior hasta el nuevo **siguiendo todos los JUMP** (y CALL/RETURN), como el DMA del GPU pero sin publicar GET. Eliminados `put_lo/put_hi`, `nv2a_pb_scan_segment`, `nv2a_pb_segment_jump`.
  - Primera versión: al cambiar de device (anillo de arranque `0x2000` → anillo de partida `0x42E000`) no hay camino; recorrió 4 M palabras dando vueltas al anillo viejo ejecutando comandos rancios → arranque lentísimo y START no respondía. **Arreglo:** pasada en seco (`s_dry`, decodifica sin ejecutar) que confirma que se llega al PUT sin volver al inicio; si no, `[PB] no path ... ring changed, not walked` y no se ejecuta nada. Estado pendiente (`PbPending`) guardado/restaurado entre pasadas.
  - Resultado (test 150 s): 0 crashes, 0 ICALL, 0 "lost sync", 8 "no path" (1 = cambio de device; 7 dentro del mismo anillo → **investigar**: se pierden esos comandos). **Se ven los soldados** (con textura en partida; con arma en la cinemática in-engine).
- Nuevos artefactos visibles ahora que llegan esos draws: **modelos blancos** (árboles/soldados: etapa de textura ilegible → `rc_texel` devuelve blanco) y **aspecto granulado/arenoso** en laderas/edificios (¿precisión de profundidad / alpha test / sombras?). Siguiente: `RECOMP_FRAME_TRACE` sobre un modelo blanco.
- **Cinemáticas (`s01_briefing.xmv`)**, con `RECOMP_APU_STREAM_TRACE=1` + log `[READ]`: el reproductor lee cabecera 4 KB + 120 KB + 124 KB (síncrono, `ev=0 apc=0`) y **nunca vuelve a leer**; envía 2 paquetes de audio (voz 216) que SÍ se reproducen y consumen (`SSL 0/1 done`, rms > 0), los libera (count 0) y no encola más → "ran dry" hasta `VOICE_OFF`. El APU y la E/S funcionan; **el decodificador XMV deja de avanzar** (no decodifica frames de vídeo → negro, no necesita más datos). Hipótesis principal: XMV comprueba si la textura YUV destino está libre (D3D `IsBusy`/fence de recurso) antes de decodificar y nuestra emulación de fences nunca la da por libre. Plan: `trace_functions.json` con las funciones XMV (sección `0x4BFF40..`) + D3D fence/`BlockOnResource`, o mirar a qué espera el hilo del decodificador.
- Nota: no lanzar el juego a la vez que una prueba automática (comparten guardado y mando).

### Sesión 2026-10-19 (parte 2) — vídeos, granulado, perfilador
- **Referencias**: clonados (shallow) `C:/XboxRecompProject/refs/xemu` y `refs/Cxbx-Reloaded`, solo lectura. Verificado contra xemu `hw/xbox/nv2a/pgraph/glsl/psh.c`: DEPENDENT_AR/GB (`.ar`/`.gb`, `input_tex[2/3]` = bits 16/20 de `0x1E78`) coincide; `hw/xbox/mcpx/apu/vp/vp.c`: la lógica de stream/SSL de nuestro APU es idéntica.
- **VBlank**: `RECOMP_VBLANK=1` añadido a `run_gr2.bat`. XMV usa `D3DDevice_GetDisplayFieldStatus` (`sub_0042C3C0`, lee `device+0x1DE8` = contador de VBlank que incrementa el ISR de D3D `0x435510`). Sin VBlank el vídeo del menú se congelaba; con él se anima. Verificado con `RECOMP_PEEK_CHAIN=0x43A1B8,0x1DE8,0` (avanza).
- **Granulado**: medido con `zfail` en `[FTRACE]` (nuevo): terreno multipasada, pasada base LEQUAL 263 px, capas EQUAL 185 px. Arreglo: profundidad cuantizada a entero (Z16/Z24) en `xf_rows` (`T->zint`, salvo `SET_CONTROL0` bit 12 = Z float). **SIN VERIFICAR** visualmente.
- **Herramientas nuevas**:
  - `RECOMP_TRACE_FLAG=<fichero>` (`src/kernel/recomp_trace.c`): la traza de funciones solo se emite mientras existe el fichero.
  - **Perfilador por muestreo** (`ghostrecon2/src/main.c`): `RECOMP_SAMPLE=<ms>` muestrea la pila nativa del hilo principal (o del ejecutor de pushbuffer con `RECOMP_SAMPLE_THREAD=pb`) a `samples.bin`; pausa mientras exista `samples.stop`. Análisis con el mapa del linker (`/MAP` → `build/Release/ghostrecon2.map`) y `tools/samples.py` del proyecto (self / inclusive por `sub_X`).
  - `trace_functions.json` = las 65 funciones de la sección XMV + 8 de DSOUND/D3D que llama (silenciosas con `RECOMP_TRACE_BUDGET=0`).
- **Cinemática (`s01_briefing.xmv`)** — cadena de causas encontrada con el perfilador:
  1. 99 % del hilo principal en `sub_0042F790` (KickOff de D3D): espera a que el GPU limpie el bit 16 de `0xFD100410` (flush PFB). Lo limpia `xbox_Nv2aAckBusyBits()`, que corre en el mismo hilo que recorre el pushbuffer; el recorrido nuevo (`pb_follow`) no lo llamaba → cada kick esperaba a que se rasterizara el frame entero. **Arreglado** (ack cada 256 palabras en `pb_follow` y `pb_seg_params`). Efecto: menú de ~15 a ~25 FPS (usuario); la cinemática ya muestra vídeo real (88–211 FPS con 1 draw).
  2. Después, el hilo principal está ~92 % en `D3D_BlockOnTime` (`gr2_d3d.c`): espera honesta a fences que **sí avanzan pero lento** (GPU 10–50 fences por detrás). El ejecutor (perfil `RECOMP_SAMPLE_THREAD=pb`) está saturado rasterizando en CPU: `nv2a_rc_eval` 31 %, `sample_tex`/`read_rgb`/DXT ~30 %, `xf_rows` 11 %, `nv2a_vsh_run` 7 % — trabajo 3D de fondo durante la cinemática. Ya usa 12 hilos de raster (`RECOMP_RASTER_THREADS` por defecto = núcleos − 4).
  3. El reproductor no pide el 4.º paquete (`@249856`) y el audio queda "ran dry"; a veces el juego da la cinemática por terminada (pasa a Equipment Setup), otras se queda congelada en 0 FPS.
- **Conclusión**: el cuello de botella de las cinemáticas y de los 1–3 FPS en partida es la **rasterización por software**. Siguiente gran paso = backend GPU (fase Vulkan del PLAN). La interfaz existente `Nv2aBackend` (`src/kernel/nv2a_backend.h`) solo pasa la etapa de textura 0 y el color difuso — insuficiente para GR2 (combinadores, LUT, 4 etapas): hay que ampliarla con el estado de combinadores / texture shader y generar shaders como `psh.c` de xemu (xemu tiene renderer Vulkan en `hw/xbox/nv2a/pgraph/vk/` como referencia).
- `NtReadFileScatter` es un stub que devuelve éxito sin leer (`kernel_bridge.c`); GR2 no lo importa, pero es un bug latente para otros títulos.

### Sesión 2026-10-19 (parte 3) — paso 1 del diagnóstico (antes de Vulkan)
- Trazas nuevas por draw en `[FTRACE]`: `zfail` (píxeles que fallan profundidad), `tfail` (lecturas de textura que caen al blanco de reserva), `rgb` (color medio escrito). `RECOMP_Z_FLOAT=1` desactiva la profundidad entera (para A/B).
- **Modelos blancos NO son texturas ilegibles**: `tfail` total = 64 en dos frames completos (solo un LUT 300x300 fmt 0x30). Los draws casi blancos (`rgb` > 220) tienen textura válida (DXT1/DXT5) → el blanco sale del **cálculo** (combinadores / texture shader / iluminación / estado), no de la lectura.
- **A/B profundidad entera vs float** (misma partida, mismo guion): ambos igual de rotos → la profundidad entera no es la regresión. Se mantiene (corrige el EQUAL multipasada).
- Patrón de los artefactos (capturas del usuario y propias): triángulos enormes fuera de sitio, bloques de color plano (verde, rojo, blanco), sombras/rayas, partes del frame que pierden la escena. Es típico de **estado o datos de vértice equivocados** (programas de vértice/constantes de un modelo aplicados a otro, matrices de skinning), no de texturas. Encaja con: (a) los comandos que el recorrido nuevo salta ("no path" dentro del mismo anillo) y (b) el estado que pasa entre pushbuffers grabados y el anillo.
- El personaje principal (3.ª persona) nunca se ha visto.
- FPS en partida 0–2: la rasterización por CPU; confirmado con el perfilador (`nv2a_rc_eval`, muestreo de texturas, `xf_rows`).
- **Conclusión del paso 1**: lo que queda es mayoritariamente (1) estado/datos de vértices y (2) combinadores. (1) también afecta a Vulkan → conviene un diagnóstico dirigido (comparar un draw roto con xemu) al principio de la fase Vulkan o justo antes; (2) se rehace en shaders con Vulkan.
- **Paso 2 (Vulkan) NO iniciado** — esperando confirmación del usuario.

### Plan de pruebas para la próxima sesión (2026-10-19)
1. Lanzar `run_gr2.bat`, entrar en campaña: ¿desaparece el bucle de 2 imágenes? ¿avanza la cinemática inicial? ¿mejoran los modelos? Revisar `build/Release/run.log`: `grep "\[PB\] wrap"` debería salir vacío o casi (JUMP encontrado).
2. Si algo empeora, comparar con `RECOMP_PB_WALK=dma` (el walk DMA ya manejaba métodos pendientes; ojo: puede congelar si D3D espera la interrupción de software-method).
3. Modelos: `RECOMP_FRAME_TRACE=ftrace.flag` + `RECOMP_FRAME_TRACE_FRAMES=2` + `RECOMP_FRAME_TRACE_METHODS=1` con un personaje en pantalla (crear el fichero `ftrace.flag` en `build/Release` cuando se vea).
4. Cinemáticas: restaurar `logoubi.xmv` desde `video_disabled_boot/` y lanzar con `RECOMP_APU_STREAM_TRACE=1`.

### Cambios sin commitear en el toolkit (xboxrecomp) — candidatos a PR upstream
- `tools/recomp/lifter.py`, `translator.py`: `MAX_JUMP_TABLE_ENTRIES = 1024` (+ `test_long_jump_table.py`).
- `src/kernel/nv2a_pb_exec.c`: motor 2D (NV062/NV09F SRCCOPY), DEPENDENT_AR/GB, superficies swizzled en `put_pixel`, `RECOMP_FRAME_TRACE_FRAMES`.
- `src/kernel/nv2a_pb_scan.c`: JUMP de fin de vuelta (`nv2a_pb_segment_jump`), método pendiente entre segmentos.
- `src/kernel/xbox_memory_layout.c`: reinicio del anillo en el destino del JUMP (no en el PUT más bajo).
- `src/apu/apu_vp.c`: `RECOMP_APU_STREAM_TRACE`; `apu_core.c`/`apu_state.h`: contadores IRQ.
- `src/kernel/kernel_bridge.c` (FSCTL), `kernel_file.c` (FATX, máscara vacía) — de sesiones anteriores.

### Siguiente paso (histórico)
0. Confirmar la hipótesis XMV/APU: traza de `KeSetEvent`/`NtWaitForSingleObject`/esperas en el hilo del reproductor con `RECOMP_KERNEL_LOG_BUDGET`, y del lado APU `RECOMP_APU_VOICE_DUMP` (¿se marcan los paquetes de stream como completados?).
1. Avisos del ejecutor: métodos NV2A sin implementar — revisar con `RECOMP_PB_SCAN=1` en escenas 3D.
2. Replicar el arreglo FATX en la rama POSIX de `kernel_file.c` y proponer los arreglos del kernel como PR al upstream.

## Sesión 2026-10-18

### Revisión de los cambios locales previos
| Cambio | Veredicto | Acción |
|---|---|---|
| `kernel_thunks.c`: stub `PsQueryStatistics` (ord. 256) que devolvía éxito sin rellenar nada | Ocultaba, y además está en la tabla *legacy* que el juego no usa | **Revertido** |
| `kernel_thunks.c`: comentario "147 ordinales de Ghost Recon 2" | Falso: la lista es la de Burnout 3 (GR2 importa 150 distintos) | **Revertido** |
| `kernel_thunks.c`: saltar ordinal 0 | Correcto (378 slots, 147 rellenos → 231 errores falsos) | Conservado |
| `CMakeLists.txt`: `add_subdirectory(src/game)` | Necesario | Conservado |

**Por qué no arrancaba**: (1) faltaban los ficheros del juego (diálogo "Failed to load default.xbe"); (2) bugs reales en `kernel_bridge.c` (abajo); (3) `main.c` sin configurar.

### Bug corregido en `src/kernel/kernel_bridge.c` (ruta real de dispatch)
El commit upstream `8a0bf9d` corrigió ordinales desplazados en `kernel_thunks.c` pero **no** en `kernel_bridge.c`:
- Exports de datos: 17 (`ExFreePool`, función) y 327/328 (`XeLoadSection`/`XeUnloadSection`) se trataban como datos; 326/355/356/357 apuntaban a estructuras equivocadas; faltaban 40/41/42/71/353/354. → Corregido + nuevos offsets `KDATA_*` en `xbox_memory_layout.h`.
- Dispatch: 15/16/24/47/63/67 desplazados (`ExAllocatePoolWithTag`, `ExQueryPoolBlockSize`, `HalReadSMCTrayState`, `IoCreateFile`, `IoCreateSymbolicLink`); `NtQuerySymbolicLinkObject` implementado pero sin conectar. → Corregido.
- Tabla de bytes stdcall: reescrita entera con firmas reales (antes p. ej. `NtWaitForSingleObject` limpiaba 20 bytes en vez de 12 → pila simulada corrupta).

### `src/game/src/main.c`
Entry point `0x0001DF71`, saves en `%LOCALAPPDATA%\GhostRecon2`, hilo que actualiza `KeTickCount` (se para antes de liberar memoria).

### Primera ejecución
- XBE carga, 19/19 secciones, 150/150 thunks (68 con bridge, 82 stub).
- Entra en `xbe_entry_point` → `PsCreateSystemThreadEx(routine=0x0002174D)` → **"start routine not found in dispatch"** → el juego termina.
- Causa: `0x0002174D` (arranque del hilo principal de XAPI, empieza con `push 0x18; push 0x50FC00; call __SEH_prolog@0x130B10`) no fue detectada por el desensamblador. Escaneo: solo 4 funciones con ese patrón faltaban (`0x2174D, 0x23F56, 0x137754, 0x3C525C`) → `seed_seh_gr2.json`.
- En curso: regenerar disasm + C con esas semillas.

### Segunda y tercera ejecución
- Con semillas (`tools/disasm/seed_gr2.json`: 4 entradas SEH + 1160 punteros a código con frontera de función) → 21 554 funciones. El hilo principal `0x2174D` → `0x1DEFD` ya se ejecuta.
- XAPI hace su init y luego **`XLaunchNewImage` → `HalReturnToFirmware(2)`**: es la comprobación de actualización del dashboard (`0x22474`: compara `d:\dashupdate.xbe` con `y:\xboxdash.xbe` / `y:\XODash\xonlinedash.xbe`). Se copiaron esos XBE a la carpeta del juego (Y: → carpeta del juego), sin efecto todavía: solo se ve un `NtOpenFile` y ningún `NtReadFile`, falta ver qué ruta abre.
- Por eso "el .exe no abre nada": el juego se cierra antes de crear ventana.

### Decisión: actualizar a upstream (773 commits por delante)
`origin/main` (v0.14.0) ya tiene: `kernel_bridge.c` con ordinales corregidos y bridges reales (10 426 líneas vs ~1 800), recuperación de callbacks no detectados (#176/#182), `tools/seed_from_log`, `tools/doctor.py`, `tools/kernel_audit`, su propio `tools/xiso`, ventana/audio SDL2 e input ratón+teclado. Nuestros arreglos del kernel quedan superados.
Plan: guardar en `backup/` (patch de cambios + `src/game` viejo + mi `tools/xiso`) → fast-forward a `origin/main` → regenerar el juego desde cero con la plantilla nueva. **Pendiente de permiso del usuario** (descarta cambios locales del working tree).

### Pendiente inmediato
1. Recompilar y volver a ejecutar tras la regeneración.
2. Implementar bridges prioritarios (PLAN §4) según los `WARNING: no bridge for ordinal` del log.
3. Ojo: solo se recompila `.text`. Las secciones de código del XDK (`D3D`, `D3DX`, `XGRPH`, `XMV`, `DSOUND`, `XPP`, `XNET`, `XONLINE`) **no** se recompilan: las llamadas a ellas acaban en stubs. Hay que decidir por librería: recompilar (`--extra-sections`) o reemplazar por HLE (xbox_d3d8/xbox_dsound/xbox_input).
