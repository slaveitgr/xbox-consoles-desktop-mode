# SPDX-License-Identifier: GPL-2.0-only
# Exact pinned-source edits, applied only to a disposable build-local copy.
function(proton_profile_replace text_variable before after)
  string(FIND "${${text_variable}}" "${before}" first)
  if(first EQUAL -1)
    message(FATAL_ERROR "Pinned UWP profile edit no longer matches: ${before}")
  endif()
  string(LENGTH "${before}" match_length)
  math(EXPR after_first "${first}+${match_length}")
  string(SUBSTRING "${${text_variable}}" ${after_first} -1 remaining)
  string(FIND "${remaining}" "${before}" second)
  if(NOT second EQUAL -1)
    message(FATAL_ERROR "Ambiguous pinned UWP profile edit: ${before}")
  endif()
  string(REPLACE "${before}" "${after}" result "${${text_variable}}")
  set(${text_variable} "${result}" PARENT_SCOPE)
endfunction()

function(proton_prepare_uwp_profile_overlay source_root destination_root)
  # These files retain their upstream implementation except for the explicit
  # capability boundaries below. The original checkout is never modified.
  foreach(relative
      platform/sdl/knativeaudio.cpp
      source/kernel/devs/devsequencer.cpp
      source/kernel/devs/devdsp.cpp
      source/kernel/devs/devmixer.cpp
      source/kernel/ksystem.cpp
      source/kernel/sys/sysfs.cpp
      source/sdl/startupArgs.cpp
      source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp
      platform/sdl/knativethread.cpp
      source/kernel/knativesocket.cpp)
    file(READ "${source_root}/${relative}" text)
    string(REPLACE "\r\n" "\n" text "${text}")
    if(relative STREQUAL "platform/sdl/knativeaudio.cpp")
      # Keep the original copyright/license and discard the upstream Fake Midi
      # implementation entirely, rather than exposing success without devices.
      string(FIND "${text}" "#include \"boxedwine.h\"" includes)
      if(includes EQUAL -1)
        message(FATAL_ERROR "Pinned native-audio header no longer matches")
      endif()
      string(SUBSTRING "${text}" 0 ${includes} license)
      set(text "${license}")
      string(APPEND text [=[// ProtonDesktop UWP audio-off profile: no backend/devices are published.
#include "boxedwine.h"
#include "knativeaudio.h"
#ifndef PROTONDESKTOP_UWP_PROFILE
#error This overlay requires the explicit UWP interpreter profile.
#endif
std::vector<std::shared_ptr<KNativeAudio>> KNativeAudio::availableAudio;
std::shared_ptr<KNativeAudio> KNativeAudio::createNativeAudio() { return nullptr; }
void KNativeAudio::init() { availableAudio.clear(); }
void KNativeAudio::shutdown() { availableAudio.clear(); }
]=])
    elseif(relative STREQUAL "source/kernel/devs/devsequencer.cpp")
      proton_profile_replace(text "        audio->load();" "        if (audio) audio->load();")
    elseif(relative STREQUAL "source/kernel/devs/devdsp.cpp")
      proton_profile_replace(text "    return new DevDsp(node, flags);"
        "    return nullptr; // UWP audio-off: opening a DSP device is unsupported.")
    elseif(relative STREQUAL "source/kernel/devs/devmixer.cpp")
      proton_profile_replace(text "    return new DevMixer(node, flags);"
        "    return nullptr; // UWP audio-off: no mixer devices are exposed.")
    elseif(relative STREQUAL "source/kernel/ksystem.cpp")
      proton_profile_replace(text "bool KSystem::soundEnabled = true;"
        "bool KSystem::soundEnabled = false; // UWP audio-off profile.")
    elseif(relative STREQUAL "source/kernel/sys/sysfs.cpp")
      set(frequency_nodes [=[    if (Platform::getCpuFreqMHz()) {
        for (U32 i = 0; i < cpuCount; i++) {
            std::shared_ptr<FsNode> cpuCoreNode = Fs::addFileNode("/sys/devices/system/cpu/cpu" + BString::valueOf(i), B(""), B(""), true, cpuNode);
            std::shared_ptr<FsNode> cpuFreqCoreNode = Fs::addFileNode("/sys/devices/system/cpu/cpu" + BString::valueOf(i)+"/cpufreq", B(""), B(""), true, cpuCoreNode);
            Fs::addVirtualFile("/sys/devices/system/cpu/cpu" + BString::valueOf(i) + "/cpufreq/scaling_cur_freq", openSysCpuScalingCurrentFrequency, K__S_IREAD, k_mdev(0, 0), cpuFreqCoreNode, i);
            Fs::addVirtualFile("/sys/devices/system/cpu/cpu" + BString::valueOf(i) + "/cpufreq/cpuinfo_max_freq", openSysCpuMaxFrequency, K__S_IREAD, k_mdev(0, 0), cpuFreqCoreNode, i);
            Fs::addVirtualFile("/sys/devices/system/cpu/cpu" + BString::valueOf(i) + "/cpufreq/scaling_max_freq", openSysCpuScalingMaxFrequency, K__S_IREAD, k_mdev(0, 0), cpuFreqCoreNode, i);
        }
    }]=])
      proton_profile_replace(text "${frequency_nodes}"
        "    // UWP profile: omit unavailable host CPU-frequency telemetry nodes.")
    elseif(relative STREQUAL "source/sdl/startupArgs.cpp")
      # Not compiled by this diagnostic. Produce the future bootstrap overlay
      # now so admitting it later cannot reintroduce unsupported proc/audio nodes.
      foreach(line
          "    Fs::addVirtualFile(B(\"/proc/stat\"), openProcStat, K__S_IREAD, k_mdev(0, 0), KSystem::procNode);"
          "    Fs::addVirtualFile(B(\"/proc/cpuinfo\"), openCpuInfo, K__S_IREAD, k_mdev(0, 0), KSystem::procNode);"
          "\tFs::addVirtualFile(B(\"/dev/dsp\"), openDevDsp, K__S_IWRITE | K__S_IREAD | K__S_IFCHR, k_mdev(14, 3), devNode);"
          "\tFs::addVirtualFile(B(\"/dev/mixer\"), openDevMixer, K__S_IWRITE | K__S_IREAD | K__S_IFCHR, k_mdev(14, 0), devNode);"
          "    Fs::addVirtualFile(B(\"/dev/sequencer\"), openDevSequencer, K__S_IWRITE | K__S_IREAD | K__S_IFCHR, k_mdev(14, 1), devNode);")
        proton_profile_replace(text "${line}" "    // UWP profile: unsupported node deliberately omitted.")
      endforeach()
    elseif(relative STREQUAL "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp")
      # MSVC std::thread::detach closes its HANDLE. Apply affinity in the
      # worker using GetCurrentThread, never its parent's stale nativeHandle.
      set(parent_affinity [=[    if (!thread->process->isSystemProcess() && KSystem::cpuAffinityCountForApp) {
        Platform::setCpuAffinityForThread(thread, KSystem::cpuAffinityCountForApp);
    }]=])
      proton_profile_replace(text "${parent_affinity}"
        "    // UWP profile: affinity is applied by the bound worker, before execution.")
      set(worker_binding [=[    KThread::setCurrentThread(cpu->thread);
    KProcessPtr process = KSystem::getProcess(cpu->thread->process->id);]=])
      set(worker_affinity [=[    KThread::setCurrentThread(cpu->thread);
    KProcessPtr process = KSystem::getProcess(cpu->thread->process->id);
    if (!cpu->thread->process->isSystemProcess() && KSystem::cpuAffinityCountForApp) {
        Platform::setCpuAffinityForThread(cpu->thread, KSystem::cpuAffinityCountForApp);
    }]=])
      proton_profile_replace(text "${worker_binding}" "${worker_affinity}")
    elseif(relative STREQUAL "platform/sdl/knativethread.cpp")
      proton_profile_replace(text
        "\tresult->nativeThread = SDL_CreateThread(sdl_start_thread, name.c_str(), result);"
        "\tresult->nativeThread = SDL_CreateThread(sdl_start_thread, name.c_str(), result);\n    if (!result->nativeThread) {\n        delete result;\n        return nullptr;\n    }")
    elseif(relative STREQUAL "source/kernel/knativesocket.cpp")
      proton_profile_replace(text "#include \"knativesocket.h\""
        "#include <cstdlib>\n#include \"knativesocket.h\"")
      proton_profile_replace(text "static bool checkWaitingNativeSocketsThreadDone;"
        "static std::atomic<bool> checkWaitingNativeSocketsThreadDone{false};")
      proton_profile_replace(text "static S32 nativeSocketPipe[2];"
        "static S32 nativeSocketPipe[2] = {-1, -1};")
      set(select_result [=[        int result = select(maxSocketId + 1, &waitingReadset, &waitingWriteset, &waitingErrorset, (timeout>=0?&t: nullptr));
        if (result) {]=])
      set(select_checked [=[        int result = select(maxSocketId + 1, &waitingReadset, &waitingWriteset, &waitingErrorset, (timeout>=0?&t: nullptr));
        if (result < 0) {
            if (WSAGetLastError() == WSAEINTR) return true;
            kpanic("ProtonDesktop UWP native socket select failed");
            std::abort();
        }
        if (result) {]=])
      proton_profile_replace(text "${select_result}" "${select_checked}")
      set(wake_read [=[                ::recv(nativeSocketPipe[0], &buf, 1, 0);
                return true;]=])
      set(wake_read_checked [=[                int received = ::recv(nativeSocketPipe[0], &buf, 1, 0);
                if (received == 1 || (received < 0 && WSAGetLastError() == WSAEWOULDBLOCK))
                    return true;
                kpanic("ProtonDesktop UWP native socket wake read failed or closed");
                std::abort();]=])
      proton_profile_replace(text "${wake_read}" "${wake_read_checked}")
      set(wake_sender [=[static void sendNativeSocketWake(char value) {
    int sent = ::send(nativeSocketPipe[1], &value, 1, 0);
    // A full nonblocking stream already contains a pending wake notification.
    if (sent == 1 || (sent < 0 && WSAGetLastError() == WSAEWOULDBLOCK)) return;
    kpanic("ProtonDesktop UWP native socket wake send failed");
    std::abort();
}

static int checkWaitingNativeSockets_thread(void *ptr) {]=])
      proton_profile_replace(text "static int checkWaitingNativeSockets_thread(void *ptr) {" "${wake_sender}")
      foreach(wake_value 0 2 3)
        # The original stop call is indented inside its if block; other wake
        # calls are at function scope. Preserve exact matching in both cases.
        if(wake_value EQUAL 0)
          set(indent "        ")
        else()
          set(indent "    ")
        endif()
        proton_profile_replace(text "${indent}char buf = ${wake_value};\n${indent}::send(nativeSocketPipe[1], &buf, 1, 0);"
          "${indent}sendNativeSocketWake(${wake_value});")
      endforeach()
      set(start_original [=[        Platform::nativeSocketPair(nativeSocketPipe);
        setNativeBlocking(nativeSocketPipe[0], false);
        setNativeBlocking(nativeSocketPipe[1], false);
        checkWaitingNativeSocketsThread = KNativeThread::createAndStartThread(checkWaitingNativeSockets_thread, B("NativeSockeThread"), (void *)nullptr);]=])
      set(start_checked [=[        // The real APP socket pair must exist before FD_SET or worker entry.
        if (Platform::nativeSocketPair(nativeSocketPipe) != 0) {
            kpanic("ProtonDesktop UWP native socket wake-pair creation failed");
            std::abort();
        }
        auto closeWakePair = [] {
            if (nativeSocketPipe[0] >= 0) closesocket(nativeSocketPipe[0]);
            if (nativeSocketPipe[1] >= 0) closesocket(nativeSocketPipe[1]);
            nativeSocketPipe[0] = nativeSocketPipe[1] = -1;
        };
        u_long nonblocking = 1;
        if (ioctlsocket(nativeSocketPipe[0], FIONBIO, &nonblocking) != 0 ||
            ioctlsocket(nativeSocketPipe[1], FIONBIO, &nonblocking) != 0) {
            closeWakePair();
            kpanic("ProtonDesktop UWP native socket wake-pair nonblocking setup failed");
            std::abort();
        }
        try {
            checkWaitingNativeSocketsThread = KNativeThread::createAndStartThread(checkWaitingNativeSockets_thread, B("NativeSockeThread"), (void *)nullptr);
        } catch (...) {
            closeWakePair();
            throw;
        }
        if (!checkWaitingNativeSocketsThread) {
            closeWakePair();
            kpanic("ProtonDesktop UWP native socket worker creation failed");
            std::abort();
        }]=])
      proton_profile_replace(text "${start_original}" "${start_checked}")
      set(stop_original [=[        checkWaitingNativeSocketsThread->wait();
        checkWaitingNativeSocketsThreadDone = false;
        checkWaitingNativeSocketsThread = nullptr;]=])
      set(stop_checked [=[        checkWaitingNativeSocketsThread->wait();
        delete checkWaitingNativeSocketsThread;
        checkWaitingNativeSocketsThread = nullptr;
        closesocket(nativeSocketPipe[0]);
        closesocket(nativeSocketPipe[1]);
        nativeSocketPipe[0] = nativeSocketPipe[1] = -1;
        checkWaitingNativeSocketsThreadDone = false;]=])
      proton_profile_replace(text "${stop_original}" "${stop_checked}")
      # Keep the existing one-second wake timeout valid for Winsock timeval.
      proton_profile_replace(text "    t.tv_sec = 0;\n    t.tv_usec = timeout*1000;"
        "    t.tv_sec = timeout > 0 ? timeout / 1000 : 0;\n    t.tv_usec = timeout > 0 ? (timeout % 1000) * 1000 : 0;")
    endif()
    get_filename_component(directory "${destination_root}/${relative}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${destination_root}/${relative}" "${text}")
  endforeach()
endfunction()

if(DEFINED PINNED_PROFILE_ROOT AND DEFINED UWP_PROFILE_OUTPUT)
  proton_prepare_uwp_profile_overlay("${PINNED_PROFILE_ROOT}" "${UWP_PROFILE_OUTPUT}")
endif()
