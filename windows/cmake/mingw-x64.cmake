# CMake toolchain: llvm-mingw (msvcrt runtime) -> x86_64 Windows.
# Usage: cmake -S windows -B windows/build-x64 -G Ninja --toolchain windows/cmake/mingw-x64.cmake
set(CP_MINGW_TRIPLE x86_64-w64-mingw32)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
include(${CMAKE_CURRENT_LIST_DIR}/mingw-common.cmake)
