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
