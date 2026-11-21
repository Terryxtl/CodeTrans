# Shared part of the mingw toolchain files. Expects CP_MINGW_TRIPLE.
#
# The compilers are looked up on PATH first, then in $ENV{LLVM_MINGW_ROOT}/bin,
# then in the default WinGet install location of llvm-mingw (msvcrt flavour).
# Use the *msvcrt* build of llvm-mingw / MinGW-w64, NOT ucrt (see PLAN.md §7):
# the UCRT runtime is missing on a bare Win7 SP1.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 6.1)

set(_cp_hints "")
if(DEFINED ENV{LLVM_MINGW_ROOT})
  list(APPEND _cp_hints "$ENV{LLVM_MINGW_ROOT}/bin")
endif()
file(GLOB _cp_winget_dirs
  "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages/MartinStorsjo.LLVM-MinGW.MSVCRT_*/llvm-mingw-*-msvcrt-x86_64/bin")
list(APPEND _cp_hints ${_cp_winget_dirs})

find_program(CMAKE_C_COMPILER  NAMES ${CP_MINGW_TRIPLE}-gcc ${CP_MINGW_TRIPLE}-clang HINTS ${_cp_hints} REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${CP_MINGW_TRIPLE}-windres HINTS ${_cp_hints} REQUIRED)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
