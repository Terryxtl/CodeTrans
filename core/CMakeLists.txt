# CodeTrans shared core (C11). Used by windows/ (add_subdirectory) and by the
# desktop test suite. Android builds its own target list from the same files.
#
# Standalone (tests only):
#   cmake -S core -B core/build -G Ninja --toolchain windows/cmake/mingw-x64.cmake
#   ninja -C core/build && core/build/cp_core_tests.exe
cmake_minimum_required(VERSION 3.20)
project(CodeTransCore LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_FLAGS_RELEASE "-O2")

set(CPC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(CPC_TP  ${CPC_DIR}/third_party)

# x86: SSE2 + SSE math, never x87 / -ffast-math / -march=native (PLAN.md §7).
set(CPC_ARCH_OPTS "")
if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "86")
  list(APPEND CPC_ARCH_OPTS -msse2 -mfpmath=sse)
endif()

# ---- third party -----------------------------------------------------------
add_library(cp_miniz STATIC ${CPC_TP}/miniz/miniz.c)
target_include_directories(cp_miniz PUBLIC ${CPC_TP}/miniz)
target_compile_options(cp_miniz PRIVATE ${CPC_ARCH_OPTS} -w)

# nanorq without lib/io.c (mmap file I/O; core supplies its own memory ioctx).
add_library(cp_nanorq STATIC
  ${CPC_TP}/nanorq/lib/chooser.c
  ${CPC_TP}/nanorq/lib/nanorq.c
  ${CPC_TP}/nanorq/lib/nanorq_core.c
  ${CPC_TP}/nanorq/lib/ops.c
  ${CPC_TP}/nanorq/lib/params.c
  ${CPC_TP}/nanorq/lib/partition.c
  ${CPC_TP}/nanorq/lib/precode.c
  ${CPC_TP}/nanorq/lib/rand.c
  ${CPC_TP}/nanorq/lib/sopi.c
  ${CPC_TP}/nanorq/lib/tuple.c
  ${CPC_TP}/nanorq/lib/uvec.c
  ${CPC_TP}/nanorq/deps/obl/oblas_common.c
  ${CPC_TP}/nanorq/deps/obl/oblas_lite.c
)
target_include_directories(cp_nanorq PUBLIC ${CPC_TP}/nanorq/include ${CPC_TP}/nanorq/deps ${CPC_TP}/nanorq/deps/obl)
target_compile_options(cp_nanorq PRIVATE ${CPC_ARCH_OPTS} -w)

add_library(cp_qrcodegen STATIC ${CPC_TP}/qrcodegen/qrcodegen.c)
target_include_directories(cp_qrcodegen PUBLIC ${CPC_TP}/qrcodegen)
target_compile_options(cp_qrcodegen PRIVATE ${CPC_ARCH_OPTS} -w)

add_library(cp_quirc STATIC
  ${CPC_TP}/quirc/lib/decode.c
  ${CPC_TP}/quirc/lib/identify.c
  ${CPC_TP}/quirc/lib/quirc.c
  ${CPC_TP}/quirc/lib/version_db.c
)
target_include_directories(cp_quirc PUBLIC ${CPC_TP}/quirc/lib)
# 1080p+ camera frames / large test renders exceed the default 254 regions.
target_compile_definitions(cp_quirc PUBLIC QUIRC_MAX_REGIONS=65534)
target_compile_options(cp_quirc PRIVATE ${CPC_ARCH_OPTS} -w)

# ---- protocol core ---------------------------------------------------------
add_library(cp_core STATIC
  src/container.c
  src/crc32.c
  src/d3.c
  src/fec_rq.c
  src/frame_cp.c
  src/ob.c
  src/ob_fec.c
  src/ob_locate.c
  src/output.c
  src/platform.c
  src/rx.c
  src/sha256.c
  src/tx.c
  src/tx_render.c
)
target_include_directories(cp_core PUBLIC include PRIVATE src)
target_compile_options(cp_core PRIVATE ${CPC_ARCH_OPTS} -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(cp_core PRIVATE cp_nanorq cp_qrcodegen PUBLIC cp_miniz)
if(NOT WIN32)
  target_link_libraries(cp_core PUBLIC m)   # ob_locate.c / ob.c: sqrt, hypot
endif()

# ---- camera scanner (quirc + tracking + contrast retry + optical boards) ----
# Core contrast-retry tests exercise quirc in isolation. The Windows camera
# builds the same sources with the optional ZXing fallback enabled.
function(cp_add_scanner target)
  set(dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scan")
  add_library(${target} STATIC "${dir}/scanner.c" "${dir}/tracked_grid.c" "${dir}/quad_tracker.c"
    "${dir}/confirmed_regions.c")
  target_include_directories(${target} PUBLIC "${dir}")
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter ${ARGN})
  target_link_libraries(${target} PUBLIC cp_core cp_quirc)
endfunction()
cp_add_scanner(cp_scan ${CPC_ARCH_OPTS})

# ---- desktop tests (encode -> render -> quirc -> rx) -----------------------
option(CP_CORE_TESTS "Build core tests" ON)
if(CP_CORE_TESTS)
  add_executable(cp_core_tests tests/test_core.c)
  target_include_directories(cp_core_tests PRIVATE src)
  target_compile_options(cp_core_tests PRIVATE ${CPC_ARCH_OPTS} -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(cp_core_tests PRIVATE cp_core cp_qrcodegen cp_quirc cp_scan)
  # Decimen golden vectors produced by the reference implementation
  target_compile_definitions(cp_core_tests PRIVATE CP_TEST_DATA="${CPC_DIR}/tests/data")
  # Decimen interoperability tool (frames files in / out), see tests/d3_tool.c
  add_executable(cp_d3_tool tests/d3_tool.c)
  target_include_directories(cp_d3_tool PRIVATE src)
  target_compile_options(cp_d3_tool PRIVATE ${CPC_ARCH_OPTS} -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(cp_d3_tool PRIVATE cp_core)
  if(WIN32)
    target_link_options(cp_core_tests PRIVATE -static -mconsole)
    target_link_options(cp_d3_tool PRIVATE -static -mconsole)
  else()
    target_link_libraries(cp_core_tests PRIVATE pthread m)
    target_link_libraries(cp_d3_tool PRIVATE pthread m)
  endif()
endif()
