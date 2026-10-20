# Ghost Recon 2 — Formatos de archivo del disco

3 826 ficheros · 2.85 GB · extraídos con `python -m tools.xiso -x <iso> -d <dir>`.
Listado completo: `python -m tools.xiso -l <iso>`.

## Árbol raíz
`RendererData/ actor/ briefings/ character/ cubicmaps/ dvdextras/ effects/ equip/ globs/ kits/ map/ mapobjects/ media/ mission/ model/ motion/ rankings/ refmaps/ script/ shell/ sound/ textures/ vehicles/ video/ weapons/` + `default.xbe`.

## Formatos por categoría

### Binarios específicos de Xbox (necesitan conversión/traducción en runtime)
| Ext | Nº | Magic | Qué es | Plan |
|---|---:|---|---|---|
| `.xpr` | 1209 | `XPR0` | Xbox Packed Resource: texturas D3D (swizzled, DXT/A8R8G8B8…) + cabeceras de recurso | xbox_d3d8 ya desswizzlea; para el port nativo → convertir a DDS/KTX2 offline |
| `.xpob` | 190 | `XPOB` + chunks `TXTR`… | Modelo estático Stonewall (geometría + materiales) | Ingeniería inversa; parser propio |
| `.xqob` | 101 | `XQOB` + `TXTR` | Modelo con skinning (personajes, armas) | Ídem |
| `.xska` | 86 | `#k` + `XQOB` | Esqueleto/animación + malla | Ídem |
| `.joe` | 38 | binario | Vehículos (malla/física) | Pendiente |
| `.glb` | 86 | binario (cabecera `02 00 00 00`, tabla de nombres) | "globs": paquetes de datos por misión | Pendiente |
| `.dat` | 57 | binario (floats) | heightmaps de hierba, datos de movimiento | Pendiente |
| `.xmv` | 98 | `xobX` | Vídeo Xbox Media Video (WMV-like) | Sección XMV recompilada o reemplazo con FFmpeg |
| `.xwb` | 13 | `WBND` | XACT Wave Bank (ADPCM Xbox) — 619 MB en `sound/` | Decodificar Xbox ADPCM → PCM (SDL2/miniaudio) |
| `.xsb` | 2 | | XACT Sound Bank (cues) | Parser XACT |
| `.xsr` | 4 | | Recursos de voz/sonido | Pendiente |
| `.tga` | 44 | TGA estándar | Máscaras de densidad, fuentes | Directo |
| `.ttf` | 1 | TrueType | Fuente de UI | Directo |
| `.uix` | 1 | | UIX (UI del XDK para Live) | Ignorar |

### Texto/XML de Stonewall (legibles, `\r\n`, etiquetas `<Tipo>…`)
`.mis` MissionFile (384) · `.dep` dependencias (375) · `.eft` ParticleEffect (315) · `.cms` CharacterModelSet (166) · `.wsf` WeaponSetFile (131) · `.cbo` ComboEffect (79) · `.gun` GunFile (57) · `.vcl` VehicleFile (36) · `.remap` color remap (32) · `.cgs` CharacterGameSet (32) · `.jef` JefFile/sonidos (27) · `.ren` SceneEnvFile (25) · `.env` EnvironmentFile (25) · `.prj` ProjectileFile (24) · `.sfx` SpecialEffectsFile (19) · `.nff` NearFieldDefinitions (18) · `.vgr` VgrFile/rejillas de helicóptero (17) · `.dso` DestructibleObjectFile (12) · `.res` recursos de fuente · `.ass` scripts (4) · `.txt`.

→ Estos se cargan tal cual: el código recompilado los parsea. Útiles para modding (FOV, HUD, distancias de dibujo).

### Shaders (¡en código fuente!)
- `RendererData/Shaders/PS/Xbox/*.psh` — 11 pixel shaders `xps.1.1` (ensamblador de register combiners).
- `RendererData/Shaders/VS/Xbox/*.vsh` — `vsGrass`, `vsPassThruAll`, `vsRain`, `vsScreen`… (ensamblador `xvs.1.1`).
- Muchos shaders del motor también van compilados dentro del XBE (llegan a xbox_d3d8 como microcódigo NV2A).

## Tamaños por carpeta destacados
`sound/` 619 MB (11 xwb + 4 xsr) · `shell/` 8.8 MB (105 xpr de menús) · `media/` 26 MB · `video/` + `dvdextras/` ≈ vídeos XMV.
