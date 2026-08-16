cmake_minimum_required(VERSION 3.16)
project(anoa VERSION 0.10.1 LANGUAGES CXX)

# CI injects the git tag here so the tag is the single source of truth for the
# version — CMakeLists.txt never needs a manual bump for releases. The value
# above is just the default for local builds.
set(ANOA_VERSION_OVERRIDE "" CACHE STRING "Version override (CI passes the git tag)")
if(ANOA_VERSION_OVERRIDE)
    string(REGEX REPLACE "^v" "" PROJECT_VERSION "${ANOA_VERSION_OVERRIDE}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ── Static build ────────────────────────────────────────────────────────────
option(STATIC_BUILD "Link Qt and runtime libraries statically" OFF)

if(STATIC_BUILD)
    set(BUILD_SHARED_LIBS OFF)
    cmake_policy(SET CMP0091 NEW)
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    elseif(UNIX AND NOT APPLE)
        # Static libgcc/libstdc++; glibc stays dynamic (musl toolchain needed for full static)
        add_link_options(-static-libgcc -static-libstdc++)
    endif()
    # macOS: full static is unsupported by the OS; Qt .a libs are picked up automatically
    # when CMAKE_PREFIX_PATH points to a static Qt installation.
endif()
# ────────────────────────────────────────────────────────────────────────────

find_package(Qt6 6.4 REQUIRED COMPONENTS
    WebEngineWidgets
    WebEngineCore
    Network
    WebSockets
    Widgets
)

set(SOURCES
    src/main.cpp
)

# The window icon, compiled in rather than read from disk: the bundle is
# relocatable and the launcher rewrites the paths it runs from, so an icon
# found by path is an icon that is sometimes missing.
qt_add_resources(SOURCES resources/icons/anoa.qrc)

# Windows reads an executable's icon from a resource compiled into it. Listed
# only there because .rc is meaningless to the other two toolchains.
if(WIN32)
    list(APPEND SOURCES resources/icons/anoa.rc)
endif()

add_executable(anoa ${SOURCES})

if(APPLE)
    # The .icns has to be INSIDE the bundle as well as named in Info.plist —
    # naming it alone gives a bundle that asks for an icon it does not carry,
    # which Finder renders as the generic application square.
    target_sources(anoa PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/anoa.icns")
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/anoa.icns"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    set_target_properties(anoa PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE "anoa.icns")
endif()

target_sources(anoa PRIVATE
    src/config/config.cpp
    src/common/url_input.cpp
    src/browser/anoa_browser.cpp
    src/browser/browser_window.cpp
    src/browser/browser_window.h
    src/browser/tab_ids.cpp
    src/browser/tab_ids.h
    src/http/http_server.cpp
    src/cdp/cdp_proxy.cpp
    src/cdp/cdp_client.cpp
    src/cdp/cdp_client.h
    src/cdp/cdp_extensions.cpp
    src/agent/agent_cli.cpp
    src/agent/agent_help.cpp
    src/agent/agent_skill.cpp
    src/pdf/pdf_handler.cpp
)

# Terminal mode (`anoa terminal`) is POSIX-only — termios, select() on
# STDIN and SIGWINCH have no MSVC equivalent. On Windows the sources are left
# out entirely and main.cpp reports the unsupported platform at runtime.
if(NOT WIN32)
    target_sources(anoa PRIVATE
        src/terminal/terminal_app.cpp
        src/terminal/terminal_ui.cpp
        src/terminal/frame_bytes.cpp
        src/terminal/render_http_client.cpp
        src/terminal/cdp_frame_backend.cpp
        src/terminal/inprocess_frame_backend.cpp
        # frame_backend.h has no .cpp of its own, so AUTOMOC only sees it (and
        # generates the FrameBackend metaobject) when it is listed here.
        src/terminal/frame_backend.h
        src/terminal/terminal_ui.h
        src/terminal/render_http_client.h
        src/terminal/cdp_frame_backend.h
        src/terminal/inprocess_frame_backend.h
    )
endif()

target_include_directories(anoa PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(anoa PRIVATE
    ANOA_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(anoa PRIVATE
    Qt6::WebEngineWidgets
    Qt6::WebEngineCore
    Qt6::Network
    Qt6::WebSockets
    Qt6::Widgets
)

# ── Static plugin imports ───────────────────────────────────────────────────
# Required when Qt is built statically; dynamic builds ignore this block.
if(STATIC_BUILD)
    if(WIN32)
        set(_qt_platform_plugins Qt6::QWindowsIntegrationPlugin)
    elseif(APPLE)
        set(_qt_platform_plugins Qt6::QCocoaIntegrationPlugin)
    else()
        set(_qt_platform_plugins Qt6::QXcbIntegrationPlugin)
    endif()

    qt_import_plugins(anoa
        INCLUDE ${_qt_platform_plugins}
        EXCLUDE_BY_TYPE imageformats
        EXCLUDE_BY_TYPE sqldrivers
        EXCLUDE_BY_TYPE printsupport
    )

    if(APPLE)
        target_link_libraries(anoa PRIVATE
            "-framework AppKit"
            "-framework CoreFoundation"
            "-framework CoreGraphics"
            "-framework CoreServices"
            "-framework CoreText"
            "-framework IOKit"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework Security"
            "-framework SystemConfiguration"
        )
    elseif(UNIX)
        target_link_libraries(anoa PRIVATE
            pthread dl m resolv
        )
    endif()
endif()
# ────────────────────────────────────────────────────────────────────────────

# Compiler warnings
if(MSVC)
    target_compile_options(anoa PRIVATE /W4)
    set_property(TARGET anoa PROPERTY VS_PLATFORM_TOOLSET "v143")
else()
    target_compile_options(anoa PRIVATE -Wall -Wextra -Wpedantic)
endif()

# RPATH for portable Linux distribution (binary finds ./lib at runtime)
if(UNIX AND NOT APPLE)
    set_target_properties(anoa PROPERTIES
        INSTALL_RPATH "$ORIGIN/lib"
        BUILD_RPATH "$ORIGIN/lib"
        INSTALL_RPATH_USE_LINK_RPATH FALSE
    )
endif()

# Release strip flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        target_link_options(anoa PRIVATE /OPT:REF /OPT:ICF)
    elseif(UNIX AND NOT APPLE)
        target_link_options(anoa PRIVATE -s)
    endif()
endif()

# Install rule
include(GNUInstallDirs)
if(APPLE)
    install(TARGETS anoa
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
else()
    install(TARGETS anoa
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(FILES resources/anoa.desktop
    DESTINATION share/applications
)
install(FILES resources/anoa.svg
    DESTINATION share/icons/hicolor/scalable/apps
)

# Tests
option(BUILD_TESTS "Build unit and integration test targets" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()