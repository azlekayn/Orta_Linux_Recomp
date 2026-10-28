cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Panzer Dragoon Orta - Static Recompilation
#
# Windows host: WIN32 subsystem executable, D3D11 backend (from the
#   xboxrecomp runtime), a D3D11 ImGui overlay, and the Win32 settings dialog.
# POSIX host:   native main(), the OpenGL 3.3/SDL2 backend the runtime selects
#   on a non-Windows configure, an OpenGL ImGui overlay, and no native settings
#   dialog (settings.ini + the F1 overlay stand in for it).
#
# The runtime itself (../xboxrecomp-main) auto-selects its POSIX/SDL2/GL
# branches under add_subdirectory() below; nothing there needs changing here.
# See LINUX_PORT_SCOPE.md.
# ============================================================================

project(orta_recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    # RECOMP_ABI_CHECK  (off for speed; put back to catch register clobbers)
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
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp-main" CACHE PATH
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
    src/launcher.c
)

# The application icon (and its resource document) is a Windows binary
# resource; there is nothing on Linux that reads a .rc.
if(WIN32)
    list(APPEND GAME_SOURCES src/orta.rc)
endif()

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

if(WIN32)
    add_executable(${PROJECT_NAME} WIN32
        ${GAME_SOURCES}
        ${RECOMP_GEN_SOURCES}
    )
else()
    add_executable(${PROJECT_NAME}
        ${GAME_SOURCES}
        ${RECOMP_GEN_SOURCES}
    )
endif()

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

if(WIN32)
    # Windows SDK libraries needed at link time
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        xinput
        comctl32
        winmm
        # Lets the fault handler resolve the host RIP back to the generated
        # function's symbol, i.e. to the guest function that faulted.
        dbghelp
    )
else()
    # dladdr/backtrace for the POSIX crash handler in main.c. _GNU_SOURCE must
    # be defined by the compiler (not in a source) because the forced include
    # of recomp_extra.h pulls in <stdint.h> before any source-level #define.
    target_compile_definitions(${PROJECT_NAME} PRIVATE _GNU_SOURCE)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        /MAP
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
    # dr0..dr7/cr0..cr4 scratch registers, forced into every recompiled TU.
    target_compile_options(${PROJECT_NAME} PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/recomp_extra.h")
else()
    # GCC/Clang forced include of the same scratch registers.
    #   -include must be separate from its argument.
    target_compile_options(${PROJECT_NAME} PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/recomp_extra.h)
endif()

# ── Settings menu (F1): Dear ImGui over the presented frame ──────
# Its own library, so the recompiled code's forced include and options
# stay off the C++.
set(ORTA_IMGUI_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
)

if(WIN32)
    add_library(orta_overlay STATIC
        src/overlay.cpp
        ${ORTA_IMGUI_SOURCES}
        third_party/imgui/imgui_impl_dx11.cpp
    )
    set_target_properties(orta_overlay PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_include_directories(orta_overlay PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    )
    target_link_libraries(orta_overlay PRIVATE d3d11 xinput)
else()
    add_library(orta_overlay STATIC
        src/overlay_gl.cpp
        ${ORTA_IMGUI_SOURCES}
        third_party/imgui/imgui_impl_opengl3.cpp
    )
    set_target_properties(orta_overlay PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    target_include_directories(orta_overlay PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    )
    target_include_directories(orta_overlay PRIVATE ${EPOXY_INCLUDE_DIRS})
    target_link_libraries(orta_overlay PRIVATE SDL2::SDL2 ${EPOXY_LINK_LIBRARIES})
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE orta_overlay)
