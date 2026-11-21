# Post-build check: the shipped exe must run on its own (PLAN.md §7, "单个便携 exe").
# Fails the build if the exe imports any DLL that is not part of a bare Windows 7 SP1,
# e.g. libgcc_s / libwinpthread / libc++ / UCRT (api-ms-win-crt-*) from a wrong toolchain.
#
#   cmake -DOBJDUMP=<llvm-objdump> -DEXE=<path> -P check_imports.cmake
if(NOT OBJDUMP OR NOT EXE)
  message(FATAL_ERROR "check_imports: OBJDUMP and EXE are required")
endif()

set(_allowed
  kernel32.dll user32.dll gdi32.dll shell32.dll comctl32.dll dwmapi.dll winmm.dll
  advapi32.dll msvcrt.dll ole32.dll oleaut32.dll shlwapi.dll uxtheme.dll comdlg32.dll
  ntdll.dll version.dll)

execute_process(COMMAND "${OBJDUMP}" -p "${EXE}" OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "check_imports: ${OBJDUMP} -p failed (${_rc})")
endif()
string(REGEX MATCHALL "DLL Name: [^\r\n]+" _lines "${_out}")
if(NOT _lines)
  message(FATAL_ERROR "check_imports: no import table found in ${EXE}")
endif()
set(_bad "")
foreach(_l IN LISTS _lines)
  string(REGEX REPLACE "^DLL Name: *" "" _dll "${_l}")
  string(STRIP "${_dll}" _dll)
  string(TOLOWER "${_dll}" _dll_lc)
  if(NOT _dll_lc IN_LIST _allowed)
    list(APPEND _bad "${_dll}")
  endif()
endforeach()
if(_bad)
  message(FATAL_ERROR "check_imports: ${EXE} depends on non-system DLL(s): ${_bad}\n"
                      "Link statically (-static) and use the msvcrt flavour of llvm-mingw.")
endif()
