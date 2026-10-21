# Paso 2 — Backend Vulkan (plan de trabajo)

Objetivo: sacar la rasterización de la CPU (hoy 0–3 FPS en partida, cinemáticas atascadas) y preparar alta resolución / 60 FPS. El rasterizador por CPU se mantiene como referencia seleccionable.

## Arquitectura

```
juego recompilado ─► pushbuffer ─► nv2a_pb_exec.c (decodifica, ejecuta vertex programs en CPU)
                                        │
                     ┌──────────────────┴───────────────────┐
              rasterizador CPU (referencia)        backend registrado (Nv2aBackend)
              RECOMP_RENDERER=cpu                  RECOMP_RENDERER=vk  ──► src/port/vk_*.c
```

- Código nuevo en `ghostrecon2/src/port/` (C, como el resto del proyecto):
  - `vk_window.c` — ventana SDL2 + instancia/dispositivo/swapchain Vulkan (sustituye la ventana GDI de depuración: sin reaperturas al cerrar, maximizar funciona, escalado).
  - `vk_backend.c` — implementa `Nv2aBackend` (clear/draw/flip).
  - `vk_surfaces.c` — superficies del juego (render targets) como imágenes de GPU, indexadas por dirección guest.
  - `vk_textures.c` — caché de texturas (decodificación swizzle/DXT reutilizando el código existente → `VkImage`; DXT → BC1/2/3 nativo más adelante).
  - `vk_shaders.c` — generador GLSL de combinadores + texture shader (modelo: xemu `hw/xbox/nv2a/pgraph/glsl/psh.c`), compilado en runtime con glslang, caché por hash de estado.
- Dependencias por CMake FetchContent (versiones fijas): SDL2, Vulkan-Headers, volk (carga dinámica de `vulkan-1.dll`, no hace falta SDK), glslang.
- Toolkit (`xboxrecomp`): ampliar `nv2a_backend.h` con lo que GR2 necesita: 4 etapas de textura, estado de combinadores y texture shader, superficie destino/origen por dirección, blit 2D. Cambio compatible (la interfaz actual sigue funcionando).

## Fases

| Fase | Contenido | Se ve |
|---|---|---|
| 2a | Toolchain + ventana SDL2/Vulkan que presenta el framebuffer actual (CPU) | Misma imagen, ventana nueva y estable |
| 2b | Diagnóstico de datos: un draw roto de GR2 comparado con xemu; corregir estado/vértices en el ejecutor (afecta a ambos renderers) | Menos geometría rota en CPU |
| 2c | Backend Vulkan mínimo: superficies en GPU, draws con textura etapa 0 + color, blend/depth/alpha/cull, flip | Menús y HUD por GPU |
| 2d | Render-to-texture: superficies usadas como textura (escena → glow → LUT → composición), blit 2D, readback cuando el juego lee memoria | Partida completa por GPU |
| 2e | Combinadores + texture shader en GLSL (4 etapas, dependent AR/GB, final combiner) | Colores/iluminación correctos |
| 2f | Vertex programs en GPU (en vez de CPU), resolución interna escalable (x2, x3, x4), 60 FPS | Alta resolución |

Comparación CPU↔Vulkan: `RECOMP_RENDERER=cpu|vk`, mismo frame trazable con `RECOMP_FRAME_TRACE` en ambos.

## Riesgos conocidos
- Superficies que el juego lee con la CPU (capturas, LUT generado por GPU y leído como textura): requieren seguimiento de qué rango guest está "sucio" en GPU y readback.
- Formatos y swizzle de superficies (tipo 2 swizzled, ya visto con el LUT).
- AMD iGPU: probar con capas de validación (`VK_LAYER_KHRONOS_validation`) cuando haga falta (requiere SDK solo para depurar).

## Registro

### 2026-10-19 — Fase 2a hecha
- `src/port/vk_window.c`: ventana SDL2 en su propio hilo + Vulkan (volk, swapchain FIFO, blit escalado 4:3 con bandas negras). Hook nuevo en el toolkit `xbox_FramebufferSetPresentHook` (`src/video/fb_present.c` / `video_player.h`) entrega cada frame terminado ya convertido a X8R8G8B8. Teclado vía `keyboard_sdl.c` (compilado en el juego). F11 / Alt+Enter = pantalla completa; cerrar la ventana termina el proceso. `run_gr2.bat` usa `RECOMP_WINDOW=sdl`.
- Probado: AMD Radeon(TM) Graphics, Vulkan 1.3, ventana 1280x960, sin crash en partida.
- `RECOMP_FB_DUMP`/`FB_DUMP_FLIPS` siguen funcionando (los escribe el ejecutor).
- Bugs propios arreglados por el camino: `pb_follow` leía fuera de los 64 MB con un CALL/JUMP basura (AV) → límites en todos los recorridos.
- Diagnóstico 2b (en curso): atributos de vértice sin array ahora usan el último valor fijado (`imm_attr`, como xemu `inline_value`). Sin cambio visible. Las superficies planas enormes aparecen con la cámara pegada a objetos (3.ª persona) → sospecha principal: **recorte en el plano cercano** del rasterizador CPU (`raster_xf_clipped`). Con Vulkan el recorte lo hace el hardware en espacio de clip, así que el backend recibirá posiciones en clip (antes de la división) y eso separará "datos malos" de "raster malo".

### 2026-10-19 — Fase 2c (rebanada 1) funcionando
- `src/port/vk_backend.c` (Nv2aBackend: clear, draw, draw_xf, blit, present), `vk_glsl.c` (glslang en runtime), `vk_ctx.h` (contexto compartido con la ventana, lock de cola). Vulkan 1.3: dynamic rendering + push descriptors. Superficies = VkImage por dirección guest (color B8G8R8A8, profundidad D32S8); textura que apunta a una superficie → se muestrea la imagen (copia si es el destino actual); resto: decodificador del ejecutor + caché por hash (revalidado 1 vez/frame). Pipelines cacheados por estado. Shader provisional: etapa 0 × difuso + alpha test.
- Toolkit: `nv2a_backend.h` ampliado (`Nv2aVertexXf`, `Nv2aPixelState`, `Nv2aBatchXf`, `draw_xf`, `blit`, `present`); `nv2a_pb_exec.c` entrega vértices SIN recortar (la GPU recorta) con todas las salidas del vertex program y el estado de combinadores/4 etapas; tiras con el orden de bobinado correcto.
- `RECOMP_RENDERER=vk` (+ `RECOMP_WINDOW=sdl`): `run_gr2_vulkan.bat`. Capturas GPU: `RECOMP_VK_DUMP=prefijo`, `RECOMP_VK_DUMP_EVERY=n`. FPS en el log cada 5 s (`[VKB] x fps`).
- Medido: menús 250–2350 FPS (antes 15–25). En partida el cuello estaba en otro sitio (ver abajo); tras arreglarlo, picos de 176 FPS.
- **Bug grave del recorrido del pushbuffer encontrado y arreglado** (afecta a ambos renderers): los *pushbuffers grabados* (RunPushBuffer; modelos) se entran con JUMP desde el anillo y vuelven con un JUMP que D3D **reescribe en cada uso**; el ejecutor va por detrás de la CPU y leía el retorno del último uso → saltaba comandos (modelos invisibles, releases de fence perdidos → `D3D_BlockOnTime` agotaba 250 ms varias veces por frame = 1–3 FPS) o entraba en bucle. Ahora `pb_follow` recuerda el punto de salida del anillo; D3D deja la dirección de retorno real en la palabra siguiente al JUMP (unos 5 words después), se usa si cae ≤64 bytes más adelante. Resultado: 0 recorridos fallidos (salvo el cambio de device del arranque), aparecen los aviones.
- `gr2_d3d.c`: `D3D_BlockOnTime` ahora hace KickOff siempre que la fence no se ha alcanzado (antes solo si era la más nueva).
- Herramientas: perfilador con desglose por DLL fuera del exe (`[SAMPLE] outside the exe, by module`), `tools/samples.py`.
- Siguiente: portar `psh.c` de xemu (combinadores + texture shader) → cielo, corrección de color, glow; luego niebla, mipmaps, cube maps, paletas.

### 2026-10-19 (tarde) — combinadores en GPU, crash, culling, uploads
- **Crash de ambos modos** = desbordamiento de pila nativa (0xC00000FD en `sub_0042F8B0`/`sub_0042F970`): `D3D_BlockOnTime` hacía KickOff y KickOff vuelve a esperar fences → recursión infinita. Arreglado con guardia por hilo en `gr2_d3d.c`. (Diagnóstico: Visor de eventos de Windows + `ghostrecon2.map`.)
- **Combinadores en GPU**: shader único (uber-shader) que es un port directo de `nv2a_combiner.c` + modos de etapa de `rc_stage_fetch` (PROJECT2D/3D, CUBE simplificado, PASSTHRU, CLIPPLANE, DEPENDENT_AR/GB), niebla (`fog_factor`), estado por draw en un UBO (binding 1, push descriptor). `Nv2aPixelState` ampliado con `rc_seen`, `fog_mode`, `fog_param`.
- **Front face**: mapeo directo como xemu (CW 0x0900 → `VK_FRONT_FACE_CLOCKWISE`). Invertido, se descartaban los quads de menús/cielo/post-proceso.
- **Uploads de textura perdidos**: algunas subidas grabadas en el command buffer del frame nunca llegaban a la GPU (la tira del LUT salía vacía → escena gris/roja). Ahora cada subida se envía de inmediato (ponytail: entender la causa). Clave de caché sin `pitch` para formatos swizzled/DXT.
- Herramientas Vulkan: `RECOMP_VK_FS=magenta|d0|t0` (salida del shader), `RECOMP_VK_NOCULL=1`, `RECOMP_VK_DUMP_SURFACES=1` (todas las superficies + canal alfa), `RECOMP_VK_WATCH=<va>` (+`_DUMP=n`, `_TEX=<va>`: volcado justo tras el n-ésimo draw), `RECOMP_VK_DUMP_TEX=<va>` (textura tal cual se sube y leída de la GPU).
- Audio: `RECOMP_AUDIO_VOLUME=0..100` (toolkit `apu_core.c`); lanzadores al 30 %, pruebas automáticas a 0.
- Estado: menús/HUD correctos; cinemática in-engine con color correcto; **en partida tinte verde**. El LUT (256x256) ya se genera bien; el verde viene de la consulta dependiente `.ar`: el alfa de la escena es demasiado alto. Siguiente: comparar el alfa de la escena CPU vs GPU en partida.

### 2026-10-19 (noche) — color en partida
- Tinte rojo/verde/cian en partida resuelto en 3 pasos: (1) texturas pequeñas con hash completo (la tira del LUT se reescribe cada frame); (2) coordenadas escaladas cuando la superficie muestreada es mayor que la textura (el LUT 256x256 vive en una dirección que también es buffer 640x480); (3) **WORKAROUND**: la consulta DEPENDENT_AR del pase de corrección usa u=0.5 en vez del alfa de la escena (`RECOMP_VK_GRADE_ALPHA=1` restaura). Causa real pendiente: algunos draws (personajes, troncos, paredes) dejan en la escena un alfa que el hardware no dejaría → buscar ese draw (comparar alfa CPU vs GPU o con xemu).
- Pendiente: destello rojo al inicio de misión (primeros frames sin LUT).
- Superficies escritas por la CPU (blit 2D desde una fuente que no es nuestra): `cpu_newer` → al muestrearlas se lee la memoria guest hasta que la GPU vuelva a dibujar en ellas.
- Destello rojo: 1 frame al empezar la cinemática de la misión. La dirección del LUT `0x00A17000` es compartida por: blit 640x480 de la pantalla de equipo (`0x018F6000 → 0x00A17000`, rojiza), un pase 640x480 (276 vértices, textura `0x81DF4000`) y el LUT 256x256. El primer pase final lee aún la copia de la pantalla de equipo. Pendiente: ver si en hardware el LUT se dibuja antes en ese frame o si hay que distinguir subsuperficies por tamaño/formato (swizzled 256x256 vs lineal 640x480 en la misma dirección).
- Fuego con centro verde/rosa y algún terreno de fondo verdoso: probablemente el mismo alfa erróneo (ver WORKAROUND) en partículas aditivas.
- **Causa real del cian (resuelto, workaround retirado)**: los objetos opacos (alfa = 1) consultan el LUT en u = 1, el borde exacto de la textura 256x256; como la imagen GPU de esa dirección es 640x480, el filtrado bilineal mezclaba con lo que hay fuera del LUT. El hardware hace clamp al borde de la propia textura. Ahora `samp()` limita las coordenadas a medio texel dentro de la textura cuando la superficie es mayor (`clampmax` en push constants, 128 bytes). `RECOMP_VK_FS=noalpha` queda solo como depuración.
- Todas las superficies de GR2 son A8R8G8B8 salvo un pase R5G6B5 en `0x00A17000` (log `[VKB] surface ... colour format`).
- **Superficies por dirección + ancho** (`find_surface_w`, `find_surface_for_tex`, `written` secuencial): `0x00A17000` tiene 3 imágenes (LUT 256x256, buffer 640x480, 320x300). Arregló el HUD roto (caja de munición negra). NO arregló: veil rojo/blanco intermitente al final de la cinemática, cuadrante brillante abajo-derecha, parches cian en troncos/suelo/campo lejano.
- Descartado (medido): el LUT nunca está desfasado respecto al buffer 640 (`grade lookup: table ... written later` = 0 casos).
- Textura 300x300 en `0x00A17000` se lee desde la superficie 320x300 (ancho de pitch 320): pendiente revisar si es la sombra/máscara de HUD.
- Siguiente hipótesis: el veil/cuadrante viene de la cadena de glow 320x200 (`0x00A57000`/`0x00A95800`, texturas lineales fmt 0x12 con coordenadas en texeles) — comprobar escala de coordenadas texel cuando la imagen es mayor que el clip, y el blend aditivo del pase final. Herramientas: `RECOMP_VK_WATCH=A57000 RECOMP_VK_WATCH_DUMP=n`, `RECOMP_VK_DUMP_SURFACES=1`.

### Pendiente para la próxima sesión (2026-10-20)
Observado por el usuario en `run_gr2_vulkan.bat` (commit 77b5b81):
- **Diagonal fina de colores glitch** de la esquina sup-izq a la inf-der: costura entre los 2 triángulos del quad a pantalla completa del post-proceso → los dos triángulos muestrean distinto.
- **Mitad izquierda correcta / mitad derecha cian**, corte vertical en el centro (y veil rojo/blanco que aparece también jugando, no solo en la cinemática).
- Hipótesis única: el pase final lee una superficie que también es el destino actual → se usa la copia (`texture_view`, rama `s == B.rt_color`, `static Surface copy`). Revisar: (1) que la copia se rehaga en cada uso y no quede desfasada; (2) escala/clamp de coordenadas por triángulo (texscale, clampmin/max); (3) que `B.rt_color = NULL` tras la copia no deje un rendering a medias (dynamic rendering reiniciado a mitad del quad).
- Prueba: `RECOMP_VK_WATCH=670000 RECOMP_VK_WATCH_DUMP=n RECOMP_VK_DUMP_SURFACES=1` en el frame con el fallo; comparar con `RECOMP_VK_FS=t0`.
- Sigue pendiente: destello rojo inicial, parches cian, icono de salud (izquierda) en magenta/verde; después cinemáticas que se pausan y 6–8 FPS (vertex programs a GPU).

### 2026-10-20 — sesión
- **Arreglado (db5b52b)**: bucle de realimentación — el pase de post-proceso muestreaba su propio attachment porque la copia se decidía con el destino del draw ANTERIOR (`B.rt_color`) en vez del actual (`B.next_target`). Eliminó la diagonal, la mitad cian y el velo rojo/blanco (confirmado por el usuario).
- **Cian en troncos/suelo**: medido que **ya está en la escena** (`0x00670000`, R=0 con alfa 128) antes del pase final → lo produce un draw 3D soportado (los modos no implementados — 3 cube, 9, 12, 17 — son todos pases aditivos 1/1 y no pueden poner R a 0). Sospechosos: pases de detalle `blend 306/300` (modulate2x) con textura `fmt 00` (Y8/L8) y la ruta de combinadores con `vD0` sin clamp.
- Herramienta nueva: `RECOMP_VK_PROBE=x,y` + fichero `build/Release/probe.flag`: tras cada draw lee la superficie destino completa y registra `[PROBE] cyan <n> #idx surf ... blend ... prog ... t0/t1 fmt`. El draw donde `cyan` salta es el culpable. Ojo: deja el juego a ~2 FPS; activar el flag cuando el cian esté en pantalla (no por tiempo fijo).
- Pendiente también: suelo negro en el primer edificio (Cxbx tiene el mismo fallo, xemu no), modelos de armas ausentes en Equipment Setup.
- **Cian localizado con la sonda (prueba del usuario)**: la escena `0x00670000` termina con 0 px cian (288 draws); el draw #296 — pase de corrección a pantalla completa sobre `0x0079C000` (prog `041E1`, t0 = escena `0x8079C000` fmt 12, t1 = LUT `0x80A17000` fmt 06) — crea 99 420 px cian. Corrección de la conclusión anterior: el cian NO está en la escena; lo produce la consulta del LUT (rojo ≈ 0 en píxeles oscuros con alfa ~0.5). Siguiente: comparar LUT CPU vs GPU píxel a píxel (`RECOMP_VK_DUMP_SURFACES=1` vs volcado CPU de `0x00A17000`) y las coordenadas de la consulta AR.
- Sonda: ahora cubre el frame entero (hasta 2500 draws) y registra `sum`; lanzadores `run_gr2_sonda.bat` + `marcar_cian.bat`.
- **LUT verificado**: el LUT GPU (256x256 en `0x00A17000`) es una tabla identidad: (x=u, y=v) → R=v, G=u, B=v (p. ej. (128,40) = (40,128,40)). Hipótesis "LUT distinto" descartada. Con LUT identidad la consulta AR(a,r) da R = rojo de la escena, así que el cian (R≈0) lo produce la **combinación** de las dos consultas en los combinadores del pase `prog 041E1` (3 etapas: s0 icw C9C1CAC2/D9D1DAD2, s1 icw CCC10000 ocw 000820D0, s2 icw 11CDCC31/11DDDC31, factores FFFF0000/0000FFFF y 004D961C). Siguiente: evaluar a mano esas etapas con el port GLSL vs `nv2a_combiner.c` (CPU) para un píxel cian (escena ~ (43,56,33), alfa 128) — buscar diferencia en map_in/map_out, mux o el registro r0.a inicial.
- `RECOMP_DUMP_TEX_AT=va,w,h,fmt,every` (toolkit, en flips): vuelca memoria guest como textura; en CPU el LUT salió negro (momento del frame), no válido para comparar.
- Dumps de superficies ahora llevan el ancho en el nombre (`*_w256.bmp`).
- **Cian — siguiente pista (2026-10-20)**: el pase de corrección liga bien sus texturas (st0 escena 640x480, st1/st2 LUT 256x256 escala 1). Con `RECOMP_VK_FS=gradeT0` (salida = escena que recibe el pase) el usuario vio que **la escena de entrada ya tiene oscuros/negros** justo donde luego sale cian (tronco). El pase lee la escena del frame ANTERIOR (`0x0079C000` cuando la actual va a `0x00670000`, y viceversa) mediante la copia por auto-muestreo. Hipótesis: esa superficie tiene zonas no actualizadas/estado intermedio (p. ej. la copia se toma antes de que terminen draws previos, o el pase de glow/DOF sobre la misma superficie). Siguiente: volcar `0x0079C000` justo ANTES del pase `041E1` (sonda) y comparar con la escena completa de su frame.
- Ojo metodológico: comparar salidas de depuración solo dentro de la MISMA ejecución (dos ejecuciones = frames distintos).
- Modos nuevos: `RECOMP_VK_FS=gradeAR` (consulta AR del pase final), `gradeT0` (escena que lee).
- **Causa raíz del cian encontrada (2026-10-20)**: no era la corrección de color. Volcando las superficies justo antes de dos pases finales seguidos (`RECOMP_VK_PREGRADE=n`) se vio que la escena ya tenía **negro** en la base de troncos y en manchas del suelo; la corrección lo convertía en cian. Ese negro venía de 16 pases de **sombras** (`prog 10842`, 4 etapas PROJECT3D sobre `0x80A17000` formato `0x30` = textura de profundidad, blend `0/301` = escena × (1 − resultado)). El shader leía la profundidad como color → basura → oscurecía donde no había sombra. **Arreglo**: shadow maps como xemu (`psh_append_shadowmap`): imágenes de profundidad muestreables, etapas con textura de profundidad marcadas en el UBO (`shadow`, `shadowmax`), comparación `profundidad_guardada <func> z/w` con `SET_SHADOW_DEPTH_FUNC` (0x1E6C, nuevo `Nv2aPixelState.shadow_func`).
- **Bisección (2026-10-20, medida + confirmación visual del usuario)**: el suelo cercano oscuro existe desde siempre (antes salía gris/negro); desde `77b5b81` (superficies por ancho) ese mismo suelo oscuro sale **cian** tras la corrección. Problema raíz = los pases de detalle del suelo cercano (`blend 306/300`, modulate2x, 5 etapas de combinador `ctl 00011105`, t0 Y8 `0x8267C000`, t1 DXT5) oscurecen; saltándolos (`RECOMP_VK_SKIP_BLEND=306/300`) el hormigón deja de ser negro. Descartado: decodificación Y8 (xemu = (Y,Y,Y,1), igual que la nuestra). Añadido clamp de colores de vértice (oD0/oD1) a [0,1] como el hardware.
- Siguiente (recomendado): comparar línea a línea el combinador de esas 5 etapas con xemu `glsl/psh.c` (map_in/map_out, registro r0.a inicial, factores por etapa, mux, salida de etapas no usadas) volcando el estado exacto de un draw `306/300` con `RECOMP_FRAME_TRACE` y evaluándolo a mano con ambos modelos.
- **Descartado (2026-10-20)**: sincronización. `RECOMP_VK_SYNC_EACH=1` (barrera completa + reinicio de rendering por draw) y `=2` (además submit + wait) siguen mostrando cian (confirmado por el usuario: base lejana, muro/montaña). El suelo bien visto durante la sonda era otra vista, no un arreglo.
- Evaluación a mano del pase de detalle `306/300` (5 etapas): resultado ≈ 0.5 gris → no oscurece en teoría. El oscurecimiento viene de otro pase; candidatos sin evaluar: `306/0` (`prog 00421`, 3 etapas; `prog 00001` 1 etapa, 60 draws) y pases con modos no implementados (BUMPENV 6/7, DOT 9/12/17) que el shader devuelve como 0.
- Sugerencia de método para la próxima sesión: comparar con una captura de xemu en el mismo sitio (el usuario tiene xemu) y usar `RECOMP_VK_SKIP_BLEND` por clases de pase midiendo con la misma vista fija.

## Ground darkness: solved (2026-10-21)

The cyan / black ground and the dark shadows were four separate bugs, each
found with the non-stalling probe (RECOMP_VK_PROBE=x,y;x,y..., probe.flag,
per-draw 32x32 copies, one whole frame) and combiner dumps
(RECOMP_VK_RC_DUMP=<prog> [_W=<width>] [_T0=<tex va>]):

1. Sub-pixel snap. The colour-grade table is drawn from 0.53125 (D3D's screen
   offset); without xemu's 1/16 truncation row and column 0 stayed black, and
   grade lookups with scene alpha 0 lost red -> cyan.
2. Z16 depth clears. The clear value was always read as Z24S8; the shadow
   maps (Z16) were cleared to ~0, so uncovered texels compared as shadowed
   (dark patch on tree trunks).
3. Polygon offset (SET_POLY_OFFSET_FILL_ENABLE + factor/bias) for the
   shadow-map renders, in a ZOFS shader variant that writes gl_FragDepth.
4. Border colour. The ground detail decals (Y8 8267C000, modulate2x 306/300,
   5 combiner stages) use address mode 4 (BORDER) with BORDER_SOURCE = colour
   and SET_TEXTURE_BORDER_COLOR = FF7F7F7F (neutral). We sampled transparent
   black outside [0,1], so every detail pass multiplied the ground by ~0.27
   (base entrance pitch black, mid-distance dark, a visible seam at the decal
   edge). The shader now returns the border colour per stage/axis.

Debug knobs added: RECOMP_VK_FS=fog|nofog|stN|dtex0|dd0 (st*/dtex0/dd0 only
on the modulate passes), RECOMP_VK_SHADOW=0|1.
