# SPDX-License-Identifier: GPL-2.0-only
# Actual Windows DESKTOP diagnostic. This target is not a UWP qualification.
cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
project(ProtonDesktopWindowsCore LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" OR NOT MSVC
   OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Use Windows x64 clang-cl from a Visual Studio developer environment")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  message(FATAL_ERROR "This desktop target does not qualify the UWP API partition")
endif()
set(BOXEDWINE_SOURCE "" CACHE PATH "Pristine pinned Boxedwine64 checkout")
get_filename_component(BOXEDWINE_SOURCE "${BOXEDWINE_SOURCE}" REALPATH)
if(NOT EXISTS "${BOXEDWINE_SOURCE}/include/boxedwine.h")
  message(FATAL_ERROR "Set BOXEDWINE_SOURCE to the full pinned source checkout")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BOXEDWINE_SOURCE}" rev-parse HEAD
  OUTPUT_VARIABLE source_pin OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT source_pin STREQUAL "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4")
  message(FATAL_ERROR "Unexpected Boxedwine64 source revision: ${source_pin}")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BOXEDWINE_SOURCE}" status --porcelain --untracked-files=all
  OUTPUT_VARIABLE source_status OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT source_status STREQUAL "")
  message(FATAL_ERROR "Pinned upstream checkout must remain pristine: ${source_status}")
endif()

# Probe the actual compiler + linker. Some clang-cl distributions need an
# explicit compiler-rt builtins library for native 128-bit division/remainder.
include(CheckCXXSourceCompiles)
set(int128_probe "int main() {
  volatile unsigned __int128 un = ~(unsigned __int128)0, ud = 7;
  volatile __int128 sn = -(__int128)123, sd = 5;
  volatile unsigned __int128 uq = un / ud, ur = un % ud;
  volatile __int128 sq = sn / sd, sr = sn % sd;
  return (int)(uq + ur + sq + sr);
}")
check_cxx_source_compiles("${int128_probe}" PROTON_INT128_LINKS_WITHOUT_RT)
set(PROTON_CLANG_BUILTINS "" CACHE FILEPATH "Optional official clang_rt.builtins-x86_64.lib")
set(proton_runtime_libraries)
if(NOT PROTON_INT128_LINKS_WITHOUT_RT)
  if(NOT PROTON_CLANG_BUILTINS)
    get_filename_component(clang_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(clang_root "${clang_bin}" DIRECTORY)
    file(GLOB runtime_candidates LIST_DIRECTORIES FALSE
      "${clang_root}/lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib")
    list(SORT runtime_candidates COMPARE NATURAL ORDER DESCENDING)
    if(runtime_candidates)
      list(GET runtime_candidates 0 PROTON_CLANG_BUILTINS)
    endif()
  endif()
  if(NOT EXISTS "${PROTON_CLANG_BUILTINS}")
    message(FATAL_ERROR "clang-cl needs official compiler-rt builtins for __int128; set PROTON_CLANG_BUILTINS")
  endif()
  set(CMAKE_REQUIRED_LIBRARIES "${PROTON_CLANG_BUILTINS}")
  check_cxx_source_compiles("${int128_probe}" PROTON_INT128_LINKS_WITH_RT)
  unset(CMAKE_REQUIRED_LIBRARIES)
  if(NOT PROTON_INT128_LINKS_WITH_RT)
    message(FATAL_ERROR "Native __int128 arithmetic still fails with the selected compiler-rt library")
  endif()
  list(APPEND proton_runtime_libraries "${PROTON_CLANG_BUILTINS}")
endif()

# SDL is compiled from the SAME pinned source, not a downloaded binary.
# Its CMake Windows path does not run the Unix revision shell script.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(FORCE_STATIC_VCRT OFF CACHE BOOL "" FORCE)
set(LIBC ON CACHE BOOL "" FORCE)
set(VIDEO_OPENGL OFF CACHE BOOL "" FORCE)
set(VIDEO_OPENGLES OFF CACHE BOOL "" FORCE)
set(VIDEO_VULKAN OFF CACHE BOOL "" FORCE)
# SDL2.0.12's Windows LIBC branch skips its normal C header probes. Derive
# these config values from the actual compiler, including intptr_t's width,
# rather than using the incomplete legacy fallback integer typedefs.
include(CheckIncludeFile)
include(CheckCSourceCompiles)
foreach(header stdint.h inttypes.h stdlib.h stddef.h stdarg.h float.h errno.h)
  string(TOUPPER "${header}" header_name)
  string(REPLACE "." "_" header_name "${header_name}")
  check_include_file("${header}" "PROTON_HAVE_${header_name}")
  if(PROTON_HAVE_${header_name})
    set("HAVE_${header_name}" 1)
  endif()
endforeach()
check_c_source_compiles("#include <stdint.h>
typedef char intptr_matches_host[(sizeof(intptr_t) == sizeof(void*)) ? 1 : -1];
int main(void) { intptr_t value = 0; return (int)value; }" PROTON_INTPTR_TYPE_VALID)
if(NOT PROTON_INTPTR_TYPE_VALID)
  message(FATAL_ERROR "The compiler must provide genuine pointer-width intptr_t in stdint.h")
endif()
# SDL's pinned build declares CMake 2.8 compatibility. CMake 4 requires an
# explicit compatibility floor for this unchanged third-party subdirectory.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include("${CMAKE_CURRENT_SOURCE_DIR}/prepare_sdl_overlay.cmake")
set(sdl_overlay "${CMAKE_CURRENT_BINARY_DIR}/upstream-overlay/sdl2")
proton_prepare_sdl_overlay("${BOXEDWINE_SOURCE}/lib/sdl2" "${sdl_overlay}")
add_subdirectory("${sdl_overlay}" "${CMAKE_CURRENT_BINARY_DIR}/sdl2" EXCLUDE_FROM_ALL)

set(core_sources)
foreach(directory emulation io kernel sdl util x11 x11wire opengl vulkan)
  file(GLOB_RECURSE batch CONFIGURE_DEPENDS "${BOXEDWINE_SOURCE}/source/${directory}/*.cpp")
  list(APPEND core_sources ${batch})
endforeach()
list(FILTER core_sources EXCLUDE REGEX "/source/util/networkutils\\.cpp$")
file(GLOB platform_sources CONFIGURE_DEPENDS
  "${BOXEDWINE_SOURCE}/platform/windows/*.cpp"
  "${BOXEDWINE_SOURCE}/platform/sdl/*.cpp")
file(GLOB softfloat_sources CONFIGURE_DEPENDS
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/*.c"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/8086-SSE/*.c")
add_executable(boxedwine64-windows ${core_sources} ${platform_sources} ${softfloat_sources})
set_source_files_properties(
  "${BOXEDWINE_SOURCE}/platform/windows/kwindowsaudio.cpp"
  "${BOXEDWINE_SOURCE}/platform/windows/platform.cpp"
  PROPERTIES COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/windows_platform_headers.h")
target_compile_features(boxedwine64-windows PRIVATE cxx_std_20)
target_compile_definitions(boxedwine64-windows PRIVATE
  BOXEDWINE_GUEST_X64=1 BOXEDWINE_64=1 BOXEDWINE_MSVC BOXEDWINE_MULTI_THREADED
  BOXEDWINE_HAS_SETJMP BOXEDWINE_DISABLE_UI SDL2=1 SDL_MAIN_HANDLED
  SIMDE_SSE2_NO_NATIVE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_include_directories(boxedwine64-windows PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/sdl2/include"
  "${BOXEDWINE_SOURCE}/include" "${BOXEDWINE_SOURCE}/lib/sdl2/include"
  "${BOXEDWINE_SOURCE}/lib/simde" "${BOXEDWINE_SOURCE}/lib/glew/include"
  "${BOXEDWINE_SOURCE}/lib/pugixml/src"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/include"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/8086-SSE")
target_compile_options(boxedwine64-windows PRIVATE
  /O1 /Gy /Gw /W3 /clang:-Wno-invalid-offsetof
  "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
  "$<$<COMPILE_LANGUAGE:CXX>:/FI${CMAKE_CURRENT_SOURCE_DIR}/int128_compat.h>")
target_link_options(boxedwine64-windows PRIVATE /OPT:REF /OPT:ICF /SUBSYSTEM:CONSOLE)
target_link_libraries(boxedwine64-windows PRIVATE SDL2-static ${proton_runtime_libraries}
  ntdll ws2_32 shlwapi crypt32 iphlpapi version setupapi winmm
  user32 gdi32 advapi32 shell32 ole32 oleaut32 uuid imm32)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/core-build-scope.txt" CONTENT
  "source=${source_pin}\ncompiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\nkind=Windows desktop diagnostic; UWP unqualified\nint128_builtins=${PROTON_CLANG_BUILTINS}\n")
