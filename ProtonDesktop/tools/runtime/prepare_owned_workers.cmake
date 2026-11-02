# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20)
# Apply AFTER proton_prepare_uwp_profile_overlay to its disposable scheduler.
function(proton_workers_replace variable before after expected)
  set(remaining "${${variable}}")
  set(count 0)
  while(TRUE)
    string(FIND "${remaining}" "${before}" position)
    if(position EQUAL -1)
      break()
    endif()
    math(EXPR count "${count}+1")
    string(LENGTH "${before}" length)
    math(EXPR next "${position}+${length}")
    string(SUBSTRING "${remaining}" ${next} -1 remaining)
  endwhile()
  if(NOT count EQUAL expected)
    message(FATAL_ERROR "Pinned owned-worker edit expected ${expected} occurrences, got ${count}: ${before}")
  endif()
  string(REPLACE "${before}" "${after}" result "${${variable}}")
  set(${variable} "${result}" PARENT_SCOPE)
endfunction()

function(proton_prepare_owned_workers source destination)
  file(READ "${source}" text)
  string(REPLACE "\r\n" "\n" text "${text}")
  proton_workers_replace(text "#include \"ripSampler.h\""
    "#include \"ripSampler.h\"\n#include \"backend/uwp/UwpGuestWorkers.h\"\n#include <stdexcept>\n#ifdef __TEST\n#error The managed UWP worker overlay is not the upstream test scheduler.\n#endif" 1)
  set(schedule [=[void scheduleThread(KThread* thread) {
    platformThreadCount++;
    CPU* cpu = thread->cpu;
#ifdef __TEST
    cpu->nativeHandle = (U64)new std::thread(platformThread, cpu);
#else
    std::thread cppThread = std::thread(platformThread, cpu);
    cpu->nativeHandle = (U64)cppThread.native_handle();
#if defined(_DEBUG) && defined(BOXEDWINE_MSVC)
    platformSetThreadDescription(thread);
#endif
    cppThread.detach();
#endif
    // UWP profile: affinity is applied by the bound worker, before execution.
}]=])
  set(owned [=[void scheduleThread(KThread* thread) {
    if (!thread || !thread->cpu || !thread->process)
        throw std::runtime_error("Cannot schedule an incomplete guest thread");
    KProcessPtr processPin = KSystem::getProcess(thread->process->id);
    if (!processPin)
        throw std::runtime_error("Cannot retain the guest worker process");
    CPU* cpu = thread->cpu;
    platformThreadCount++;
    std::string error;
    bool admitted = false;
    try {
        admitted = protondesktop::uwp::SpawnGuestWorker([cpu] { platformThread(cpu); },
            processPin->id, std::static_pointer_cast<void>(processPin), error);
    } catch (...) {
        platformThreadCount--;
        throw;
    }
    if (!admitted) {
        platformThreadCount--;
        // The owning KProcess start/clone callsite must roll back its not-yet-
        // scheduled KThread and report failure; never advertise a successful
        // guest clone merely because a host thread could not be constructed.
        throw std::runtime_error("Guest worker creation failed: " + error);
    }
}]=])
  proton_workers_replace(text "${schedule}" "${owned}" 1)
  # Preserve both actual architecture-specific interpreter/cleanup bodies.
  # Managed Wine phases share one kernel and may become idle between commands.
  proton_workers_replace(text "if (platformThreadCount == 0) {"
    "if (platformThreadCount == 0 && !protondesktop::uwp::GuestWorkerSessionManaged()) {" 2)
  string(FIND "${text}" "cppThread.detach()" detached)
  if(NOT detached EQUAL -1)
    message(FATAL_ERROR "Managed UWP scheduler still contains a detached worker")
  endif()
  get_filename_component(directory "${destination}" DIRECTORY)
  file(MAKE_DIRECTORY "${directory}")
  file(WRITE "${destination}" "${text}")
endfunction()

if(DEFINED PROFILED_GUEST_WORKERS_SOURCE AND DEFINED OWNED_GUEST_WORKERS_OUTPUT)
  proton_prepare_owned_workers("${PROFILED_GUEST_WORKERS_SOURCE}" "${OWNED_GUEST_WORKERS_OUTPUT}")
endif()
