# Ghost Recon 2 — Resumen del XBE (no hace falta re-parsear)

Fuente: `C:\Games\XboxGames\Tom Clancy's Ghost Recon 2 (USA, Europe) (En,Fr,De,Es,It).iso`
Regenerar con: `python -m tools.xbe_parser game_files/default.xbe`

## Identidad
| Campo | Valor |
|---|---|
| Título | Ghost Recon 2 |
| Title ID | `0x55530005` (Ubisoft "US", #5) |
| Tipo | Retail · DVD_X2 · Región NA/JP/RoW · Versión 2 |
| Build | 2004-10-25 (cert. 2004-10-28) |
| Motor | **Stonewall** (Red Storm) — `c:\Develop\stonewall-x\release\StonewallXbox_Release.exe` |
| XDK | 1.0.5849 (todas las librerías) |

## Memoria
| Campo | Valor |
|---|---|
| Base | `0x00010000` |
| Image size | `0x005EFE00` (5.94 MB) |
| **Entry point** | **`0x0001DF71`** (= `xbe_entry_point` en `recomp_0000.c`) |
| **Kernel thunk table** | **`0x0050EC40`** (inicio de `.rdata`), 150 entradas — autodetectado por `xbox_memory_layout.c` |
| TLS index | `0x005B460C`, zero-fill 144 bytes |
| Stack commit | `0x10000` |

## Secciones (19)
| # | Nombre | VA | Tamaño | Notas |
|---|---|---|---|---|
| 0 | .text | 0x00011000 | 0x417C70 (4.1 MB) | código del juego + CRT |
| 1 | D3D | 0x00428C80 | 0x13D3C | Direct3D8 LTCG de Xbox → **reemplazar por xbox_d3d8** |
| 2 | D3DX | 0x0043C9C0 | 0x25448 | D3DX8 |
| 3 | XGRPH | 0x00461E20 | 0x212B8 | XGraphics (swizzle, etc.) |
| 4 | XONLINE | 0x004830E0 | 0x2A0DC | Xbox Live (no aplicable → stub) |
| 5 | XNET | 0x004AD1C0 | 0x12D80 | red/System Link |
| 6 | XMV | 0x004BFF40 | 0x27A94 | decodificador de vídeo XMV |
| 7 | DSOUND | 0x004E79E0 | 0x1E53C | DirectSound Xbox → **xbox_dsound** |
| 8 | XPP | 0x00505F20 | 0x8D08 | periféricos (mandos) → **xbox_input** |
| 9 | .rdata | 0x0050EC40 | 0x6AB18 | thunks + constantes |
| 10 | .data | 0x00579760 | 0x646FC (raw 0x3A874) | resto BSS |
| 11 | XON_RD | 0x005DDE60 | 0x1AD8 | datos XOnline |
| 12 | DOLBY | 0x005DF940 | 0x7180 | microcódigo DSP Dolby |
| 13 | .data1 | 0x005E6AC0 | 0xE0 | |
| 14–18 | $$XTIMAGE, dspimage, discerr, memerr, .XTLID | 0x005E6BA0+ | | imágenes/IDs, no código |

## Librerías XDK enlazadas
XAPILIB, XBOXKRNL, LIBCPMT (C++ STL), LIBCMT, D3D8, D3DX8, XGRAPHC, XVOICE, XONLINES, UIX, XMV, DSOUND, VOICMAIL, XVOCREC — todas 1.0.5849.

## Recompilación (estado del código generado en `src/game/recomp/gen/`)
- 21 chunks `recomp_0000.c … recomp_0020.c` + `recomp_dispatch.c` — ~1.72 M líneas.
- 28 476 funciones declaradas; 8 087 destinos "no detectados" (entradas a mitad de función) como stubs vacíos en `recomp_stubs_unresolved.c` ⇒ **riesgo alto**: llamar a uno de esos stubs no ejecuta nada.
- Cabeceras de los `.c` dicen "Burnout 3" solo porque es una etiqueta fija del recompilador (`tools/recomp/__main__.py`); el entry point coincide con este XBE.

## Otros XBE en el disco (no se recompilan)
`dashupdate.xbe` (actualización del dashboard), `update.xbe`, `StonewallXboxDownloader.xbe` (contenido descargable Live).
