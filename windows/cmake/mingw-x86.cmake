# CMake toolchain: llvm-mingw (msvcrt runtime) -> i686 Windows.
# Usage: cmake -S windows -B windows/build-x86 -G Ninja --toolchain windows/cmake/mingw-x86.cmake
set(CP_MINGW_TRIPLE i686-w64-mingw32)
set(CMAKE_SYSTEM_PROCESSOR x86)
include(${CMAKE_CURRENT_LIST_DIR}/mingw-common.cmake)
