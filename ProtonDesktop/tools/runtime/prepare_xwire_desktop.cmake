# SPDX-License-Identifier: GPL-2.0-only
# Complete Wine scene and input shadows, after the Wine-session presenter.
function(proton_prepare_xwire_desktop source_root profile_root result_variable)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  foreach(helper prepare_xwire_desktop.py prepare_xwire_input.py)
    execute_process(COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/${helper}"
      --source-root "${source_root}" --overlay-root "${profile_root}"
      OUTPUT_VARIABLE desktop_manifest COMMAND_ERROR_IS_FATAL ANY)
    string(JSON source_count LENGTH "${desktop_manifest}" source_files)
    if(NOT source_count EQUAL 3)
      message(FATAL_ERROR "The complete XWire desktop requires all three matching translation units")
    endif()
  endforeach()
  set(replaced_core "${core_sources}")
  set(desktop_sources)
  foreach(index RANGE 0 2)
    string(JSON relative GET "${desktop_manifest}" source_files ${index})
    list(REMOVE_ITEM replaced_core "${source_root}/${relative}" "${profile_root}/${relative}")
    list(APPEND desktop_sources "${profile_root}/${relative}")
  endforeach()
  set(core_sources "${replaced_core}" PARENT_SCOPE)
  set(${result_variable} "${desktop_sources}" PARENT_SCOPE)
endfunction()
