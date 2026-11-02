# SPDX-License-Identifier: GPL-2.0-only
# Apply AFTER the UWP capability profile, leaving pristine external untouched.
function(proton_prepare_wine_session source_root profile_root result_variable)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  # boxedwine.h includes ksystem.h with quotes. Shadow the complete upstream
  # include tree so quoted sibling lookup sees the added lifecycle declaration.
  file(COPY "${source_root}/include/" DESTINATION "${profile_root}/include")
  execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/prepare_wine_session.py"
    --source "${source_root}" --profile "${profile_root}" --output "${profile_root}"
    COMMAND_ERROR_IS_FATAL ANY)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/prepare_owned_workers.cmake")
  proton_prepare_owned_workers(
    "${profile_root}/source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp"
    "${profile_root}/source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp")
  set(replaced_core "${core_sources}")
  list(REMOVE_ITEM replaced_core "${source_root}/source/kernel/kprocess.cpp"
    "${source_root}/source/kernel/kmemory64.cpp"
    "${source_root}/source/kernel/syscall64.cpp"
    "${source_root}/source/sdl/multiThreaded/threadedMainloop.cpp"
    "${source_root}/source/x11wire/xwirepresentSDL.cpp")
  set(core_sources "${replaced_core}" PARENT_SCOPE)
  set(replaced_platform "${platform_sources}")
  list(REMOVE_ITEM replaced_platform "${source_root}/platform/sdl/knativeinputSDL.cpp")
  set(platform_sources "${replaced_platform}" PARENT_SCOPE)
  set(${result_variable}
    "${profile_root}/source/sdl/startupArgs.cpp"
    "${profile_root}/source/kernel/kprocess.cpp"
    "${profile_root}/source/kernel/kmemory64.cpp"
    "${profile_root}/source/kernel/syscall64.cpp"
    "${profile_root}/source/sdl/multiThreaded/threadedMainloop.cpp"
    "${profile_root}/source/x11wire/xwirepresentSDL.cpp"
    "${profile_root}/platform/sdl/knativeinputSDL.cpp" PARENT_SCOPE)
endfunction()
