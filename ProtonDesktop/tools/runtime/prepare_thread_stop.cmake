# SPDX-License-Identifier: GPL-2.0-only
# Apply only after WineSession and owned workers; pristine sources stay intact.
function(proton_prepare_thread_stop source_root profile_root result_variable)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/prepare_thread_stop.py"
    --source "${source_root}" --profile "${profile_root}" --output "${profile_root}"
    OUTPUT_VARIABLE stop_manifest COMMAND_ERROR_IS_FATAL ANY)
  string(JSON source_count LENGTH "${stop_manifest}" source_files)
  math(EXPR last_source "${source_count} - 1")
  set(replaced_core "${core_sources}")
  set(stop_sources)
  foreach(index RANGE 0 ${last_source})
    string(JSON relative GET "${stop_manifest}" source_files ${index})
    list(REMOVE_ITEM replaced_core "${source_root}/${relative}" "${profile_root}/${relative}")
    list(APPEND stop_sources "${profile_root}/${relative}")
  endforeach()
  set(core_sources "${replaced_core}" PARENT_SCOPE)
  set(${result_variable} "${stop_sources}" PARENT_SCOPE)
endfunction()
