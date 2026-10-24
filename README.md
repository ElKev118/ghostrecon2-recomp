# Ghost Recon 2 — native PC port (static recompilation)

A native Windows build of *Tom Clancy's Ghost Recon 2* (original Xbox, 2004).
The game's own code is statically recompiled from its `default.xbe` into C by
the [xboxrecomp](https://github.com/ElKev118/xboxrecomp/tree/gr2-port) toolkit (a fork of [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp)); the Xbox
hardware and system software are replaced by native code (kernel, NV2A GPU →
Vulkan, MCPX APU → XAudio2, USB pads → SDL2, keyboard and mouse).

**No game data is included.** You need your own copy of the game (Xbox
disc). This repository holds only the port's own source code; the
recompiled game code is generated on your machine from your `default.xbe`.

## Playing (release build)

Requirements: Windows 10/11 x64, a GPU with Vulkan 1.3, and the
[Microsoft Visual C++ 2015–2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).

```
GhostRecon2\
├── ghostrecon2.exe
└── game\              ← the contents of your Ghost Recon 2 disc
    ├── default.xbe
    ├── actor\  character\  media\  ...
```

Copy the whole disc (extracted ISO) into `game\` and run `ghostrecon2.exe`.
The folder can live anywhere and be moved as a whole.

- **Delete** or **Home**: the port's options menu — window mode, VSync,
  frame limit, internal resolution (1x–6x), aspect ratio (4:3 / 16:9
  stretched / fill), anti-aliasing (SMAA), upscaling (FSR 1), sharpening
  (RCAS), downscale filter, shader cache, volume, reverb, rain/ambience
  level, language (restart).
- **F11** or **Alt+Enter**: fullscreen toggle.
- XInput controllers (Xbox 360 / One / Series pads) play as the Xbox pad;
  keyboard and mouse are mapped too (Enter/Esc = START, Space = A, E = B,
  arrows = D-pad, Tab = BACK).

Everything the port writes is in `%LOCALAPPDATA%\GhostRecon2\`:
`SaveData\` (the console's hard disk: saves), `gr2.ini` (options),
`shadercache\`, `logs\`. Nothing is written next to the exe except by the
game itself in `game\`.

## Building from source

1. Clone this repository and the toolkit side by side:
   ```
   XboxRecompProject\
   ├── ghostrecon2\     (this repository)
   └── xboxrecomp\      (branch gr2-port)
   ```
   `git clone -b gr2-port https://github.com/ElKev118/xboxrecomp.git`
2. Put your game's `default.xbe` in `xboxrecomp\game_files\default.xbe` and the
   whole disc in `ghostrecon2\build\Release\game\` (for running).
3. Requirements: Visual Studio 2022 (C/C++), CMake ≥ 3.20, Python 3.10+, Git
   (CMake fetches SDL2, Vulkan headers, volk, glslang, Dear ImGui, SMAA and
   FidelityFX FSR 1).
4. Configure once: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`
5. Generate the recompiled code and build: `bash tools/regen.sh`
   (disassembly → function identification → ABI analysis → C, then the
   build; ~45 min the first time). After that, `cmake --build build --config Release`.

`run_gr2_debug.bat` starts the build with the debugging keys on (F8 log mark,
F7 frame capture, F6 audio capture) and the log in `build\Release\run_debug.log`.


## Legal

Ghost Recon 2 is © Ubisoft / Red Storm Entertainment. This project is not
affiliated with or endorsed by them. It contains no game assets, no Xbox
BIOS or keys, and no recompiled game code; use it only with a copy of the
game you own. Third-party components and their licences are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
