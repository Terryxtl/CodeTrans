cmake_minimum_required(VERSION 3.20)
project(CodeTransWindows LANGUAGES C RC)

# --------------------------------------------------------------------------
# CodeTrans Windows sender (PLAN.md §7). From the repository root:
#   cmake -S windows -B windows/build-x64 -G Ninja --toolchain <abs>/windows/cmake/mingw-x64.cmake
#   cmake -S windows -B windows/build-x86 -G Ninja --toolchain <abs>/windows/cmake/mingw-x86.cmake
# or simply run windows/build.sh. Output: CodeTrans.exe (x86, the one that ships) /
# CodeTrans-x64.exe (development only), + cpwin_tests.exe and cp_core_tests.exe (not shipped).
# --------------------------------------------------------------------------

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(CP_DEBUG_LOG "Write codetrans-debug.log next to the exe (development only)" OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(CP_ARCH x64)
else()
  set(CP_ARCH x86)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
# -O2 only; never -march=native / -ffast-math (PLAN.md §7).
set(CMAKE_C_FLAGS_RELEASE "-O2")

# ---- shared protocol core (cp_core, cp_qrcodegen, cp_quirc, cp_core_tests)
set(CP_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core")
add_subdirectory(${CP_CORE_DIR} core)
set(CP_STB_DIR "${CP_CORE_DIR}/third_party/stb")

set(CP_DEFS UNICODE _UNICODE _WIN32_WINNT=0x0601 WINVER=0x0601 _WIN32_IE=0x0800
            NTDDI_VERSION=0x06010000 __USE_MINGW_ANSI_STDIO=0 WIN32_LEAN_AND_MEAN NOMINMAX)
if(CP_DEBUG_LOG)
  list(APPEND CP_DEFS CP_DEBUG_LOG=1)
endif()

set(CP_COPTS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
if(CP_ARCH STREQUAL "x86")
  list(APPEND CP_COPTS -msse2 -mfpmath=sse)
endif()

set(CP_LOPTS -municode -static -static-libgcc -s)
if(CP_ARCH STREQUAL "x86")
  list(APPEND CP_LOPTS -Wl,--large-address-aware)
endif()

set(CP_SYSLIBS user32 gdi32 shell32 comctl32 dwmapi winmm advapi32)

# ---- helpers shared by the app and the test harness (no window code)
add_library(cpwin_common STATIC
  src/config.c
  src/content.c
  src/frame_render.c
  src/image_enc.c
  src/qr_geom.c
  src/util.c
  src/dbglog.c
)
target_include_directories(cpwin_common PUBLIC src)
target_compile_definitions(cpwin_common PUBLIC ${CP_DEFS})
target_compile_options(cpwin_common PRIVATE ${CP_COPTS})
target_link_libraries(cpwin_common PUBLIC cp_core cp_qrcodegen)
# stb is only visible to image_enc.c
set_source_files_properties(src/image_enc.c PROPERTIES INCLUDE_DIRECTORIES "${CP_STB_DIR}")

# ---- the application
add_executable(CodeTrans WIN32
  src/main.c
  src/app_window.c
  src/settings_bar.c
  src/qr_view.c
  src/producer.c
  src/pacer.c
  src/clipboard.c
  src/snip.c
  src/hotkey.c
  src/tray.c
  src/compat.c
  res/app.rc
)
# The release is ONE exe: the x86 build, which runs on 32- and 64-bit Windows alike.
# The x64 build stays available for development (CodeTrans-x64.exe).
if(CP_ARCH STREQUAL "x86")
  set_target_properties(CodeTrans PROPERTIES OUTPUT_NAME "CodeTrans")
else()
  set_target_properties(CodeTrans PROPERTIES OUTPUT_NAME "CodeTrans-${CP_ARCH}")
endif()
if(CMAKE_OBJDUMP)
  add_custom_command(TARGET CodeTrans POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${CMAKE_OBJDUMP} -DEXE=$<TARGET_FILE:CodeTrans>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_imports.cmake
    COMMENT "Checking that $<TARGET_FILE_NAME:CodeTrans> only imports system DLLs"
    VERBATIM)
else()
  message(WARNING "objdump not found: the system-DLL-only import check is skipped")
endif()
target_compile_options(CodeTrans PRIVATE $<$<COMPILE_LANGUAGE:C>:${CP_COPTS}>)
target_link_options(CodeTrans PRIVATE ${CP_LOPTS} -mwindows)
target_link_libraries(CodeTrans PRIVATE cpwin_common ${CP_SYSLIBS})
# windres: resolve codetrans.ico / app.manifest relative to res/
target_include_directories(CodeTrans PRIVATE $<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}/res>)

# ---- console test harness (not shipped)
add_executable(cpwin_tests tests/test_main.c src/compat.c)
target_compile_options(cpwin_tests PRIVATE ${CP_COPTS})
target_include_directories(cpwin_tests PRIVATE ${CP_STB_DIR})
target_link_options(cpwin_tests PRIVATE ${CP_LOPTS} -mconsole)
target_link_libraries(cpwin_tests PRIVATE cpwin_common cp_quirc ${CP_SYSLIBS})

# ---- dev tool: decode screenshots of the window like the phone would
add_executable(cpwin_qrscan tests/qrscan.c)
target_compile_options(cpwin_qrscan PRIVATE ${CP_COPTS})
target_link_options(cpwin_qrscan PRIVATE ${CP_LOPTS} -mconsole)
target_link_libraries(cpwin_qrscan PRIVATE cpwin_common cp_quirc ${CP_SYSLIBS})
