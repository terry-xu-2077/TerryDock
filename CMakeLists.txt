cmake_minimum_required(VERSION 3.20)

project(LightDock
    VERSION 0.1.0
    DESCRIPTION "A lightweight Windows application dock"
    LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
# windres on some Chinese Windows code pages mis-decodes UTF-8 resource
# filenames. Keep the repository's original icon name, but copy it to an
# ASCII-only build path and generate the .rc file with absolute ASCII-safe
# resource paths.
set(LIGHTDOCK_ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/图标模板01.ico")
set(LIGHTDOCK_ICON_BINARY "${CMAKE_CURRENT_BINARY_DIR}/LightDock.ico")

if(NOT EXISTS "${LIGHTDOCK_ICON_SOURCE}")
    message(FATAL_ERROR "LightDock icon source not found: ${LIGHTDOCK_ICON_SOURCE}")
endif()

configure_file(
    "${LIGHTDOCK_ICON_SOURCE}"
    "${LIGHTDOCK_ICON_BINARY}"
    COPYONLY)

file(TO_CMAKE_PATH "${LIGHTDOCK_ICON_BINARY}" LIGHTDOCK_ICON_RC_PATH)
file(TO_CMAKE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest"
    LIGHTDOCK_MANIFEST_RC_PATH)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc"
    "${CMAKE_CURRENT_BINARY_DIR}/app.generated.rc"
    @ONLY)

set(LIGHTDOCK_SOURCES
    src/main.cpp
    src/App.cpp
    src/DockWindow.cpp
    src/DockRenderer.cpp
    src/DockLayout.cpp
    src/Animation.cpp
    src/DockItem.cpp
    src/AppLauncher.cpp
    src/ProcessMonitor.cpp
    src/IconLoader.cpp
    src/Config.cpp
    src/Json.cpp
    src/Utils.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/app.generated.rc"
)

set(LIGHTDOCK_HEADERS
    src/Common.h
    src/App.h
    src/DockWindow.h
    src/DockRenderer.h
    src/DockLayout.h
    src/DockTransform.h
    src/Animation.h
    src/DockItem.h
    src/AppLauncher.h
    src/ProcessMonitor.h
    src/IconLoader.h
    src/Config.h
    src/Json.h
    src/Utils.h
)

add_executable(LightDock WIN32 ${LIGHTDOCK_SOURCES} ${LIGHTDOCK_HEADERS})

target_include_directories(LightDock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(LightDock PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# ---------------------------------------------------------------------------
# MSVC specific: static CRT so the binary has no redistributable dependency
# ---------------------------------------------------------------------------
if(MSVC)
    set_property(TARGET LightDock PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    target_compile_options(LightDock PRIVATE
        /utf-8
        /W4
        /EHsc
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /GS-
    )

    set_target_properties(LightDock PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /OPT:REF /OPT:ICF")
else()
    target_compile_options(LightDock PRIVATE -Wall -Wextra -Wno-cast-function-type)

    # Only needed so the project can also be built with MinGW-w64.
    # Statically link the GCC runtime so the binary does not drag along
    # libgcc / libstdc++ / winpthread DLLs.
    if(MINGW)
        target_compile_options(LightDock PRIVATE -municode)
        target_link_options(LightDock PRIVATE
            -municode
            -static
            -static-libgcc
            -static-libstdc++)
        # app.rc embeds our own manifest (Common Controls v6 + PMv2 DPI).
        # The toolchain's auto-added default manifest would collide with it
        # ("multiple non-default manifests"), so default-manifest.o in the
        # MinGW sysroot is renamed to default-manifest.o.disabled.
    endif()
endif()

# ---------------------------------------------------------------------------
# System libraries (no third party runtime)
# ---------------------------------------------------------------------------
target_link_libraries(LightDock PRIVATE
    d2d1
    dwrite
    dwmapi
    windowscodecs
    uuid
    ole32
    oleaut32
    shell32
    shlwapi
    user32
    gdi32
    winmm
    comctl32
    comdlg32
    version
)

# ---------------------------------------------------------------------------
# Install (optional)
# ---------------------------------------------------------------------------
install(TARGETS LightDock RUNTIME DESTINATION .)
