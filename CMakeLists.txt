cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(ghostrecon2 C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/gr2_d3d.c
    src/gr2_debug.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    xinput
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)

    # Debug info for the runtime and the host, so crash reports name real
    # functions. Not for the generated chunks: 3.3 M lines of /Zi would
    # multiply build time and PDB size for frames that are named sub_X anyway.
    set_source_files_properties(src/main.c src/recomp_manual.c src/gr2_d3d.c
        src/gr2_debug.c PROPERTIES COMPILE_OPTIONS "/Zi")
    foreach(_lib xbox_kernel xbox_d3d8 xbox_dsound xbox_input xbox_usb
                 xbox_apu xbox_nv2a xbox_video)
        if(TARGET ${_lib})
            target_compile_options(${_lib} PRIVATE /Zi)
        endif()
    endforeach()
    target_link_options(${PROJECT_NAME} PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF
        /MAP:$<TARGET_FILE_DIR:${PROJECT_NAME}>/ghostrecon2.map)
endif()

# ── Port layer: SDL2 window + Vulkan (docs/VULKAN_PLAN.md) ──────
#
# Fetched at configure time at pinned versions, so no SDK install is needed:
# volk loads vulkan-1.dll (shipped with the GPU driver) at runtime.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
FetchContent_Declare(sdl2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-2.30.9
    GIT_SHALLOW    TRUE)
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        vulkan-sdk-1.3.290.0
    GIT_SHALLOW    TRUE)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        vulkan-sdk-1.3.290.0
    GIT_SHALLOW    TRUE)
# glslang: GLSL -> SPIR-V at runtime, for shaders generated from the
# title's combiner / vertex-program state (as xemu does).
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        14.3.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(sdl2 glslang)
# Headers and volk.c only: compiled into the game below, no CMake of theirs.
FetchContent_GetProperties(vulkan_headers)
if(NOT vulkan_headers_POPULATED)
    FetchContent_Populate(vulkan_headers)
endif()
FetchContent_GetProperties(volk)
if(NOT volk_POPULATED)
    FetchContent_Populate(volk)
endif()
# Dear ImGui (MIT): the port options overlay (docs/PORT_MENU_PLAN.md).
# Sources only, compiled into the game below.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.93.0
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

target_sources(${PROJECT_NAME} PRIVATE
    src/port/vk_window.c
    src/port/vk_backend.c
    src/port/vk_glsl.c
    src/port/vk_psh_gen.c
    ${volk_SOURCE_DIR}/volk.c
    # SDL keycodes -> the shared key table (built only on POSIX by xbox_input)
    ${XBOXRECOMP_DIR}/src/input/keyboard_sdl.c
)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${vulkan_headers_SOURCE_DIR}/include
    ${volk_SOURCE_DIR}
    ${XBOXRECOMP_DIR}/src/input
    ${XBOXRECOMP_DIR}/src/video
)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR SDL_MAIN_HANDLED)
target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2-static
    glslang glslang-default-resource-limits SPIRV)
if(MSVC)
    set_source_files_properties(src/port/vk_window.c src/port/vk_backend.c src/port/vk_glsl.c
        PROPERTIES COMPILE_OPTIONS "/Zi")
endif()
# The port options menu (Dear ImGui) and its settings file. ImGui's include
# path and defines go on these files only: target-wide they would rebuild
# every generated chunk.
set(PORT_MENU_SOURCES
    src/port/port_menu.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${PORT_MENU_SOURCES} src/port/port_settings.c)
set_source_files_properties(${PORT_MENU_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${imgui_SOURCE_DIR};${imgui_SOURCE_DIR}/backends"
    COMPILE_DEFINITIONS IMGUI_IMPL_VULKAN_USE_VOLK)
# nv2a_backend.h for the renderer only: a target-wide include would rebuild
# every generated chunk.
set_source_files_properties(src/port/vk_backend.c PROPERTIES
    INCLUDE_DIRECTORIES ${XBOXRECOMP_DIR}/src/kernel)

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
