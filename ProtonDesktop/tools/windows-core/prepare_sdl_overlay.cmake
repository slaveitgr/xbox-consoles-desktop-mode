# SPDX-License-Identifier: GPL-2.0-only
function(proton_prepare_sdl_overlay source destination)
  if(NOT EXISTS "${source}/CMakeLists.txt")
    message(FATAL_ERROR "Missing pinned SDL CMake source")
  endif()
  # A build-local copy avoids editing the pristine upstream checkout. Retain
  # the real SDL sources and notices; only its driver-specific include flag
  # changes. clang-cl takes the MSVC /I path through include_directories.
  file(MAKE_DIRECTORY "${destination}")
  file(COPY "${source}/" DESTINATION "${destination}")
  file(READ "${source}/CMakeLists.txt" sdl_cmake)
  set(original [=[if(USE_GCC OR USE_CLANG)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -idirafter ${SDL2_SOURCE_DIR}/src/video/khronos")
else()
  include_directories(${SDL2_SOURCE_DIR}/src/video/khronos)
endif()]=])
  set(replacement [=[if((USE_GCC OR USE_CLANG) AND NOT MSVC)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -idirafter ${SDL2_SOURCE_DIR}/src/video/khronos")
else()
  include_directories(${SDL2_SOURCE_DIR}/src/video/khronos)
endif()]=])
  string(FIND "${sdl_cmake}" "${original}" match_position)
  if(match_position EQUAL -1)
    message(FATAL_ERROR "Pinned SDL clang-cl include-flag patch no longer matches")
  endif()
  string(REPLACE "${original}" "${replacement}" patched "${sdl_cmake}")
  file(WRITE "${destination}/CMakeLists.txt" "${patched}")
endfunction()

# Standalone patch-application verification, without a compiler or build.
if(DEFINED SDL_PINNED_SOURCE AND DEFINED SDL_OVERLAY_OUTPUT)
  proton_prepare_sdl_overlay("${SDL_PINNED_SOURCE}" "${SDL_OVERLAY_OUTPUT}")
endif()
