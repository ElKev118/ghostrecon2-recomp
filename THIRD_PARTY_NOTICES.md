# Third-party components

Fetched at configure time by CMake (FetchContent) and linked into
`ghostrecon2.exe`, or compiled into it as shader source:

| Component | Version | Licence | Use |
|---|---|---|---|
| [SDL2](https://github.com/libsdl-org/SDL) | release-2.30.9 | zlib | window, input |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | vulkan-sdk-1.3.290.0 | Apache-2.0 / MIT | Vulkan API |
| [volk](https://github.com/zeux/volk) | vulkan-sdk-1.3.290.0 | MIT | Vulkan loader |
| [glslang](https://github.com/KhronosGroup/glslang) | 14.3.0 | BSD-3-Clause and others | GLSL → SPIR-V at runtime |
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.93.0 | MIT | options menu |
| [SMAA](https://github.com/iryoku/smaa) | 71c806a | MIT | anti-aliasing |
| [FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | v1.0.2 | MIT | upscaling, sharpening |

The [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit (kernel,
NV2A executor, APU, recompiler) is MIT, except the parts derived from
[xemu](https://github.com/xemu-project/xemu): the MCPX APU (`src/apu/`) and
the NV2A register definitions are LGPL-2.1-or-later (see the toolkit's
`LICENSES/` and README). A binary distribution must offer those parts'
source -- this repository and the toolkit's provide it.

Each component's licence text is in its source tree (`build/_deps/<name>-src/`
after configuring) and must accompany binary distributions.
