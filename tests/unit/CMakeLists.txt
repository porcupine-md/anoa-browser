# Gui is for the test binary only: it verifies that QImage decodes PNG under a
# plain QCoreApplication, the assumption terminal mode is built on. It is
# deliberately NOT linked into anoa-config-lib.
find_package(Qt6 6.4 REQUIRED COMPONENTS Test Core Gui)

# Optional ASan/UBSan build of the unit targets. The parsers in frame_bytes.cpp
# read length-prefixed bytes off a socket and size a copy from numbers the peer
# chose, so an out-of-bounds read and an unsigned wrap are exactly the failures
# worth making loud rather than silent. CI runs one Linux job with this on.
option(ANOA_TEST_SANITIZE "Build the unit test targets with ASan + UBSan" OFF)

# Optional gcov instrumentation. Kept separate from ANOA_TEST_SANITIZE because
# the two want different builds: the sanitizer answers "is this memory-safe",
# gcov answers "did the suite reach this line", and combining them makes the
# arc counts harder to trust. Coverage is measured over the three Qt6::Core-only
# libraries (config, frame_bytes, terminal_ui) — those hold the logic the unit
# suite can reach at all, and none of them needs WebEngine to build.
option(ANOA_TEST_COVERAGE "Build the unit test targets with gcov instrumentation" OFF)

if(ANOA_TEST_SANITIZE AND ANOA_TEST_COVERAGE)
    message(FATAL_ERROR
        "ANOA_TEST_SANITIZE and ANOA_TEST_COVERAGE are separate builds; enable one.")
endif()

# Every target below is warning-clean under the same flags as the main binary.
function(anoa_test_target_flags target)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(ANOA_TEST_SANITIZE AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g
        )
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# Applied to the libraries *and* to the executables that link them: --coverage
# is both a compile flag (emit the .gcno arc graph) and a link flag (pull in
# libgcov, which is what writes the .gcda counts at exit).
function(anoa_test_target_coverage target)
    if(ANOA_TEST_COVERAGE AND NOT MSVC)
        target_compile_options(${target} PRIVATE --coverage -O0 -g)
        target_link_options(${target} PRIVATE --coverage)
    endif()
endfunction()

# ── config ─────────────────────────────────────────────────────────────────
# Static library so the test binary can link against config logic without
# bringing in the full browser stack (WebEngine, Widgets, etc.).
add_library(anoa-config-lib STATIC
    ${CMAKE_SOURCE_DIR}/src/config/config.cpp
)
target_include_directories(anoa-config-lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(anoa-config-lib PUBLIC
    Qt6::Core
)
set_target_properties(anoa-config-lib PROPERTIES AUTOMOC ON)
anoa_test_target_coverage(anoa-config-lib)

add_executable(anoa-test-config test_config.cpp)
target_include_directories(anoa-test-config PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(anoa-test-config PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    anoa-config-lib
)
set_target_properties(anoa-test-config PROPERTIES AUTOMOC ON)
anoa_test_target_coverage(anoa-test-config)

add_test(NAME ConfigTests COMMAND anoa-test-config)

# ── tab ids ────────────────────────────────────────────────────────────────
# Its own target, never folded into anoa-config-lib: that one is config.cpp
# against Qt6::Core and nothing else (see AGENTS.md). tab_ids.cpp earns the
# same treatment for the same reason — it is the part of the tab feature that
# needs no WebEngine, so the unit job can reach it at all.
add_library(anoa-tab-ids-lib STATIC
    ${CMAKE_SOURCE_DIR}/src/browser/tab_ids.cpp
)
target_include_directories(anoa-tab-ids-lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(anoa-tab-ids-lib PUBLIC Qt6::Core)
anoa_test_target_flags(anoa-tab-ids-lib)
anoa_test_target_coverage(anoa-tab-ids-lib)

add_executable(anoa-test-tab-ids test_tab_ids.cpp)
target_link_libraries(anoa-test-tab-ids PRIVATE
    Qt6::Test
    Qt6::Core
    anoa-tab-ids-lib
)
set_target_properties(anoa-test-tab-ids PROPERTIES AUTOMOC ON)
anoa_test_target_flags(anoa-test-tab-ids)
anoa_test_target_coverage(anoa-test-tab-ids)

add_test(NAME TabIdsTests COMMAND anoa-test-tab-ids)

# ── mcp tools ──────────────────────────────────────────────────────────────
# The tool table is data and one pure mapping function, against Qt6::Core
# alone — no WebEngine, no event loop — so the unit job can reach the part of
# MCP where thirty hand-written schemas can quietly disagree with themselves.
add_library(anoa-mcp-tools-lib STATIC
    ${CMAKE_SOURCE_DIR}/src/mcp/mcp_tools.cpp
)
target_include_directories(anoa-mcp-tools-lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(anoa-mcp-tools-lib PUBLIC Qt6::Core)
anoa_test_target_flags(anoa-mcp-tools-lib)
anoa_test_target_coverage(anoa-mcp-tools-lib)

add_executable(anoa-test-mcp-tools test_mcp_tools.cpp)
target_link_libraries(anoa-test-mcp-tools PRIVATE
    Qt6::Test
    Qt6::Core
    anoa-mcp-tools-lib
)
set_target_properties(anoa-test-mcp-tools PROPERTIES AUTOMOC ON)
anoa_test_target_flags(anoa-test-mcp-tools)
anoa_test_target_coverage(anoa-test-mcp-tools)

add_test(NAME McpToolsTests COMMAND anoa-test-mcp-tools)

# ── terminal ───────────────────────────────────────────────────────────────
# Both libraries below are separate targets on purpose: anoa-config-lib is
# config.cpp alone against Qt6::Core, and terminal sources must never join it
# (see AGENTS.md). They are also POSIX-only, like the terminal sources in the
# top-level CMakeLists.txt.
if(NOT WIN32)
    # The wire-facing byte parsers. Qt6::Core alone — no Gui, no event loop,
    # nothing that needs a display, which is what makes the sanitizer build
    # above cheap enough to run on every push.
    add_library(anoa-terminal-bytes-lib STATIC
        ${CMAKE_SOURCE_DIR}/src/terminal/frame_bytes.cpp
    )
    target_include_directories(anoa-terminal-bytes-lib PUBLIC
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(anoa-terminal-bytes-lib PUBLIC Qt6::Core)
    anoa_test_target_flags(anoa-terminal-bytes-lib)
    anoa_test_target_coverage(anoa-terminal-bytes-lib)

    add_executable(anoa-test-frame-bytes test_frame_bytes.cpp)
    target_link_libraries(anoa-test-frame-bytes PRIVATE
        Qt6::Test
        Qt6::Core
        anoa-terminal-bytes-lib
    )
    set_target_properties(anoa-test-frame-bytes PROPERTIES AUTOMOC ON)
    anoa_test_target_flags(anoa-test-frame-bytes)
    anoa_test_target_coverage(anoa-test-frame-bytes)

    add_test(NAME FrameBytesTests COMMAND anoa-test-frame-bytes)

    # The viewer UI: the input parser, the cell map and the two image-protocol
    # renderers. frame_backend.h and terminal_ui.h are listed as sources
    # because AUTOMOC only generates a metaobject for a header the target
    # actually names — the same reason the top-level CMakeLists.txt lists them.
    add_library(anoa-terminal-ui-lib STATIC
        ${CMAKE_SOURCE_DIR}/src/terminal/terminal_ui.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal/terminal_ui.h
        ${CMAKE_SOURCE_DIR}/src/terminal/frame_backend.h
        # The Ctrl-L prompt hands what was typed to normalizeUserUrl(), which
        # the GUI address field also uses, so it is its own translation unit
        # rather than a copy in each.
        ${CMAKE_SOURCE_DIR}/src/common/url_input.cpp
    )
    target_include_directories(anoa-terminal-ui-lib PUBLIC
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(anoa-terminal-ui-lib PUBLIC Qt6::Core)
    set_target_properties(anoa-terminal-ui-lib PROPERTIES AUTOMOC ON)
    anoa_test_target_flags(anoa-terminal-ui-lib)
    anoa_test_target_coverage(anoa-terminal-ui-lib)

    add_executable(anoa-test-terminal-ui test_terminal_ui.cpp)
    target_link_libraries(anoa-test-terminal-ui PRIVATE
        Qt6::Test
        Qt6::Core
        anoa-terminal-ui-lib
    )
    set_target_properties(anoa-test-terminal-ui PROPERTIES AUTOMOC ON)
    anoa_test_target_flags(anoa-test-terminal-ui)
    anoa_test_target_coverage(anoa-test-terminal-ui)

    add_test(NAME TerminalUiTests COMMAND anoa-test-terminal-ui)
endif()
