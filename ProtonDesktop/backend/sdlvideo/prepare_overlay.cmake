# SPDX-License-Identifier: GPL-2.0-only
function(proton_sdlvideo_replace variable before after)
  string(FIND "${${variable}}" "${before}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Pinned SDL video edit no longer matches: ${before}")
  endif()
  string(LENGTH "${before}" length)
  math(EXPR tail_start "${position}+${length}")
  string(SUBSTRING "${${variable}}" ${tail_start} -1 tail)
  string(FIND "${tail}" "${before}" duplicate)
  if(NOT duplicate EQUAL -1)
    message(FATAL_ERROR "Pinned SDL video edit is ambiguous: ${before}")
  endif()
  string(REPLACE "${before}" "${after}" output "${${variable}}")
  set(${variable} "${output}" PARENT_SCOPE)
endfunction()

function(proton_prepare_sdlvideo_overlay source destination)
  # Reuse the already reviewed clang-cl/intrinsics/CRT corrections. They copy
  # the pristine SDL tree; this function adds only our native video selection.
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../tools/windows-core/prepare_sdl_overlay.cmake")
  proton_prepare_sdl_overlay("${source}" "${destination}")
  file(READ "${destination}/CMakeLists.txt" cmake_text)
  set(old_include [=[if((USE_GCC OR USE_CLANG) AND NOT MSVC)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -idirafter ${SDL2_SOURCE_DIR}/src/video/khronos")
else()
  include_directories(${SDL2_SOURCE_DIR}/src/video/khronos)
endif()]=])
  # The old raw GCC flag also splits source paths containing spaces. This
  # software-only build has no GL header precedence requirement; let CMake
  # quote the real include path for every supported host compiler.
  proton_sdlvideo_replace(cmake_text "${old_include}"
    "include_directories(\"\${SDL2_SOURCE_DIR}/src/video/khronos\")")
  set(native_video [=[if(SDL_VIDEO)
  if(VIDEO_DUMMY)]=])
  set(native_replacement [=[if(SDL_VIDEO)
  # ProtonDesktop: real host-backed software framebuffer, not a dummy device.
  set(SDL_VIDEO_DRIVER_PROTON_UWP 1)
  list(APPEND SOURCE_FILES ${SDL2_SOURCE_DIR}/src/video/proton/SDL_protonvideo.c)
  set(HAVE_SDL_VIDEO TRUE)
  if(VIDEO_DUMMY)]=])
  proton_sdlvideo_replace(cmake_text "${native_video}" "${native_replacement}")
  # The pinned CMake otherwise selects desktop HWND/IME/DLL-loading sources
  # whenever SDL_VIDEO is enabled on Windows. No such source is admitted here.
  proton_sdlvideo_replace(cmake_text
    "  if(SDL_VIDEO)\n    # requires SDL_LOADSO on Windows (IME, DX, etc.)"
    "  if(SDL_VIDEO AND NOT SDL_VIDEO_DRIVER_PROTON_UWP)\n    # requires SDL_LOADSO on Windows (IME, DX, etc.)")
  file(WRITE "${destination}/CMakeLists.txt" "${cmake_text}")
  file(READ "${destination}/include/SDL_config.h.cmake" config)
  # The old __LP64__ test incorrectly says 4 on Windows LLP64 x64. Use the
  # actual compiler pointer-size probe already recorded by upstream CMake.
  set(pointer_guess [=[#ifdef __LP64__
#define SIZEOF_VOIDP 8
#else
#define SIZEOF_VOIDP 4
#endif]=])
  proton_sdlvideo_replace(config "${pointer_guess}" "#define SIZEOF_VOIDP @SIZEOF_VOIDP@")
  proton_sdlvideo_replace(config "#cmakedefine SDL_VIDEO_DRIVER_DUMMY @SDL_VIDEO_DRIVER_DUMMY@"
    "#cmakedefine SDL_VIDEO_DRIVER_PROTON_UWP @SDL_VIDEO_DRIVER_PROTON_UWP@\n#cmakedefine SDL_VIDEO_DRIVER_DUMMY @SDL_VIDEO_DRIVER_DUMMY@")
  file(WRITE "${destination}/include/SDL_config.h.cmake" "${config}")
  file(READ "${destination}/src/video/SDL_sysvideo.h" sysvideo)
  proton_sdlvideo_replace(sysvideo "extern VideoBootStrap DUMMY_bootstrap;"
    "extern VideoBootStrap PROTON_UWP_bootstrap;\nextern VideoBootStrap DUMMY_bootstrap;")
  file(WRITE "${destination}/src/video/SDL_sysvideo.h" "${sysvideo}")
  file(READ "${destination}/src/video/SDL_video.c" video)
  proton_sdlvideo_replace(video "static VideoBootStrap *bootstrap[] = {"
    "static VideoBootStrap *bootstrap[] = {\n#if SDL_VIDEO_DRIVER_PROTON_UWP\n    &PROTON_UWP_bootstrap,\n#endif")
  file(WRITE "${destination}/src/video/SDL_video.c" "${video}")
  file(READ "${destination}/src/dynapi/SDL_dynapi.h" dynapi)
  proton_sdlvideo_replace(dynapi
    "#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE  /* probably not useful on iOS. */"
    "/* ProtonDesktop: this explicitly static, restricted-subsystem build has\n   no replaceable SDL DLL/dlopen route. Updates require rebuilding the host. */\n#if defined(PROTON_SDLVIDEO_STATIC) && PROTON_SDLVIDEO_STATIC\n#define SDL_DYNAMIC_API 0\n#elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE  /* probably not useful on iOS. */")
  file(WRITE "${destination}/src/dynapi/SDL_dynapi.h" "${dynapi}")
  file(READ "${destination}/src/video/SDL_pixels.c" pixel_code)
  set(map_rgba [=[        return (r >> format->Rloss) << format->Rshift
            | (g >> format->Gloss) << format->Gshift
            | (b >> format->Bloss) << format->Bshift
            | ((a >> format->Aloss) << format->Ashift & format->Amask);]=])
  set(map_rgba_defined [=[        /* ProtonDesktop: unsigned shifts also define opaque alpha << 24. */
        return ((Uint32)r >> format->Rloss) << format->Rshift
            | ((Uint32)g >> format->Gloss) << format->Gshift
            | ((Uint32)b >> format->Bloss) << format->Bshift
            | (((Uint32)a >> format->Aloss) << format->Ashift & format->Amask);]=])
  proton_sdlvideo_replace(pixel_code "${map_rgba}" "${map_rgba_defined}")
  file(WRITE "${destination}/src/video/SDL_pixels.c" "${pixel_code}")
  file(READ "${destination}/src/render/SDL_render.c" render_code)
  proton_sdlvideo_replace(render_code
    "    const Uint32 color = ((a << 24) | (r << 16) | (g << 8) | b);"
    "    /* ProtonDesktop: pack alpha using defined unsigned arithmetic. */\n    const Uint32 color = (((Uint32)a << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | b);")
  file(WRITE "${destination}/src/render/SDL_render.c" "${render_code}")
  file(MAKE_DIRECTORY "${destination}/src/video/proton")
  file(COPY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SDL_protonvideo.c"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ProtonSdlVideo.h"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ProtonSdlVideoHostInternal.h"
    DESTINATION "${destination}/src/video/proton")
endfunction()

if(DEFINED SDL_VIDEO_PINNED_SOURCE AND DEFINED SDL_VIDEO_OVERLAY_OUTPUT)
  proton_prepare_sdlvideo_overlay("${SDL_VIDEO_PINNED_SOURCE}" "${SDL_VIDEO_OVERLAY_OUTPUT}")
endif()
