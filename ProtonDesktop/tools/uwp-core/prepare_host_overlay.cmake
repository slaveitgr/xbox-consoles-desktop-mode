# SPDX-License-Identifier: GPL-2.0-only
function(proton_prepare_uwp_host_overlay source destination)
  file(READ "${source}" native_system)
  string(REPLACE "\r\n" "\n" native_system "${native_system}")
  set(desktop_entry [=[#ifndef __TEST
int boxedmain(int argc, const char** argv);

int main(int argc, char** argv) {
    return boxedmain(argc, (const char**)argv);
}
#endif]=])
  string(FIND "${native_system}" "${desktop_entry}" entry_position)
  if(entry_position EQUAL -1)
    message(FATAL_ERROR "Pinned SDL host desktop-entry exclusion no longer matches")
  endif()
  string(REPLACE "${desktop_entry}"
    "// ProtonDesktop APP diagnostic: the real CPU/ELF routes live in LinkProbe.cpp."
    native_system "${native_system}")
  get_filename_component(destination_directory "${destination}" DIRECTORY)
  file(MAKE_DIRECTORY "${destination_directory}")
  file(WRITE "${destination}" "${native_system}")
endfunction()

if(DEFINED PINNED_NATIVE_SYSTEM AND DEFINED UWP_NATIVE_SYSTEM_OUTPUT)
  proton_prepare_uwp_host_overlay("${PINNED_NATIVE_SYSTEM}" "${UWP_NATIVE_SYSTEM_OUTPUT}")
endif()
