# libcodetrans.so: JNI bridge + quirc scanner + shared C core (PLAN §4, §6, §12).
#
# The core is compiled here from its sources (own target names, cpa_*), as
# core/CMakeLists.txt is the desktop / Windows build. CP_CORE is passed by
# app/build.gradle.kts; the fallback is the path relative to this file.
cmake_minimum_required(VERSION 3.22.1)
project(codetrans C CXX)

if(NOT CP_CORE)
  get_filename_component(CP_CORE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core" ABSOLUTE)
endif()
if(NOT EXISTS "${CP_CORE}/include/cp.h")
  message(FATAL_ERROR "CodeTrans core not found at ${CP_CORE}")
endif()
set(TP "${CP_CORE}/third_party")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Decoding speed matters even in debug builds (RaptorQ / quirc at -O0 cannot keep
# up with the camera), so the native code is always optimised. No -ffast-math
# anywhere (d3.c is integer-only today, but keep the floating point exact).
set(CPA_OPT -O3 -fvisibility=hidden -ffunction-sections -fdata-sections)

# ---- third party ---------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(ZXING_READERS ON)
set(ZXING_WRITERS OFF)
set(ZXING_C_API OFF)
foreach(format 1D AZTEC DATAMATRIX MAXICODE PDF417)
  set(ZXING_ENABLE_${format} OFF)
endforeach()
add_subdirectory(third_party/zxing-cpp/core EXCLUDE_FROM_ALL)
target_compile_options(ZXing PRIVATE ${CPA_OPT})

add_library(cpa_nanorq STATIC
  ${TP}/nanorq/lib/chooser.c
  ${TP}/nanorq/lib/nanorq.c
  ${TP}/nanorq/lib/nanorq_core.c
  ${TP}/nanorq/lib/ops.c
  ${TP}/nanorq/lib/params.c
  ${TP}/nanorq/lib/partition.c
  ${TP}/nanorq/lib/precode.c
  ${TP}/nanorq/lib/rand.c
  ${TP}/nanorq/lib/sopi.c
  ${TP}/nanorq/lib/tuple.c
  ${TP}/nanorq/lib/uvec.c
  ${TP}/nanorq/deps/obl/oblas_common.c
  ${TP}/nanorq/deps/obl/oblas_lite.c
)
# util.h includes "obl/oblas_lite.h", so deps/ itself must be on the path too.
target_include_directories(cpa_nanorq PUBLIC ${TP}/nanorq/include ${TP}/nanorq/deps ${TP}/nanorq/deps/obl)
target_compile_definitions(cpa_nanorq PRIVATE NDEBUG)
target_compile_options(cpa_nanorq PRIVATE ${CPA_OPT} -w)

add_library(cpa_miniz STATIC ${TP}/miniz/miniz.c)
target_include_directories(cpa_miniz PUBLIC ${TP}/miniz)
target_compile_definitions(cpa_miniz PRIVATE MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)
target_compile_options(cpa_miniz PRIVATE ${CPA_OPT} -w)

add_library(cpa_quirc STATIC
  ${TP}/quirc/lib/decode.c
  ${TP}/quirc/lib/identify.c
  ${TP}/quirc/lib/quirc.c
  ${TP}/quirc/lib/version_db.c
)
target_include_directories(cpa_quirc PUBLIC ${TP}/quirc/lib)
# Required for 1080p and above (quirc README): uint16_t region labels.
target_compile_definitions(cpa_quirc PRIVATE QUIRC_MAX_REGIONS=65534 NDEBUG)
target_compile_options(cpa_quirc PRIVATE ${CPA_OPT} -w)
target_link_libraries(cpa_quirc PUBLIC m)

# ---- shared core ---------------------------------------------------------
add_library(cpa_core STATIC
  ${CP_CORE}/src/container.c
  ${CP_CORE}/src/crc32.c
  ${CP_CORE}/src/d3.c
  ${CP_CORE}/src/fec_rq.c
  ${CP_CORE}/src/frame_cp.c
  ${CP_CORE}/src/output.c
  ${CP_CORE}/src/platform.c
  ${CP_CORE}/src/rx.c
  ${CP_CORE}/src/sha256.c
  ${CP_CORE}/src/tx.c
)
target_include_directories(cpa_core PUBLIC ${CP_CORE}/include PRIVATE ${CP_CORE}/src)
target_compile_options(cpa_core PRIVATE ${CPA_OPT} -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(cpa_core PRIVATE cpa_nanorq cpa_miniz)

# ---- JNI library ---------------------------------------------------------
add_library(codetrans SHARED jni_bridge.c scanner.c tracked_grid.c qr_fallback.cpp)
target_compile_definitions(codetrans PRIVATE CP_WITH_ZXING)
target_link_libraries(codetrans PRIVATE ZXing::ZXing)
target_compile_options(codetrans PRIVATE ${CPA_OPT} -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(codetrans PRIVATE cpa_core cpa_quirc cpa_nanorq cpa_miniz log m)
# 16 KB page alignment is the NDK r28+ default; stated explicitly for older NDKs.
target_link_options(codetrans PRIVATE
  -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)
