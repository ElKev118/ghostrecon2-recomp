# Port roadmap after the XMV fix (2026-10-22)

Order agreed with the user:

1. **Game menus / flow bugs** (first). The user is testing every menu option
   and will report what fails. Known: on death, "Retry" shows the "problem
   with your Xbox, restart it" fatal-error screen (likely a failed file read
   or unimplemented kernel call on mission reload / save load). Every menu
   option must lead where it should.
2. **In-game port options menu** (this document).
3. **Later / at the end**: FSR (and SMAA) toggles in the same menu, then the
   items already in VULKAN_PLAN.md (far-LOD cyan, moving the Vulkan backend
   to the toolkit for Summit Strike, optional recompiler work).

## Step 2 -- port options menu (Dear ImGui overlay)

An OptiScaler-style overlay: Dear ImGui (MIT) with its Vulkan + SDL backends,
drawn by our renderer on top of the game. Open/close with Delete or Home.
English UI. While it is open, pad/keyboard input does not reach the game.

Settings live in an `.ini` next to the exe; the menu only reads/writes it, so
it can also be edited by hand.

| Option | Applies |
|---|---|
| Window mode: windowed / fullscreen / borderless fullscreen (SDL) | live |
| FPS limit: 30 / 60 / 120 / unlimited (today RECOMP_GR2_FRAME_MS, read at boot -> make it live) | live |
| Volume and reverb (our APU mixer; reverb default 0.08) | live |
| Internal resolution (recreate render targets; a brief flicker) | live |
| Language: English / French / German / Italian / Spanish | restart |

Language: the game does not have its own setting. It reads the console's
dashboard language once at boot (ExQueryNonVolatileSetting XC_LANGUAGE,
hard-coded to 1 = English in src/kernel/kernel_xbox.c). Make it configurable
(Xbox codes: 1 en, 3 de, 4 fr, 5 es, 6 it); the game then picks texts, voices
and the `_f/_g/_i/_s` videos itself. The menu shows "Restart required" and a
"Restart now" button that relaunches the exe.

Implementation order:
1. Integrate ImGui into the Vulkan renderer + hotkey + input capture.
2. `.ini` read/write.
3. Live options: window mode, FPS limit, volume, reverb.
4. Internal resolution.
5. Language + restart.

Future (step 3): FSR / SMAA toggles and quality presets in the same menu.

## Status (2026-10-22)

Done: ImGui v1.93.0 overlay (src/port/port_menu.cpp), gr2.ini
(src/port/port_settings.c), window mode, VSync, frame limit, volume, reverb
live; language with "Restart now" (RECOMP_LANGUAGE -> XC_LANGUAGE; checked:
Spanish loads fv_spanish.xwb + intro_s.xmv, French fv_french.xwb).
gr2.ini only exists once the menu has saved; then it wins over the
launcher's variables. Test scripts set RECOMP_NO_INI=1.
RECOMP_MENU_OPEN=1 starts with the menu open (screenshots / tests).

Next: internal resolution (renderer work: scale render targets, viewports,
surface copies), gamepad navigation of the menu (SDL game controller
subsystem), then FSR / SMAA (step 3).
