// SPDX-License-Identifier: GPL-2.0-only
// Linux-only driver for our static guest ELF. Uses the real pinned kernel,
// syscall dispatcher and owned workers; no Wine/rootfs or host fixture execution.
#include "boxedwine.h"
#include "knativesystem.h"
#include "backend/uwp/UwpGuestWorkers.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 2) return 64;
    Platform::init();
    if (!KNativeSystem::init(VIDEO_NO_WINDOW, false)) return 65;
    KSystem::init();
    if (!Fs::initFileSystem(BString::copy(argv[1]))) return 66;
    Fs::makeLocalDirs(B("/proc"));
    Fs::makeLocalDirs(B("/tmp"));
    KSystem::procNode = Fs::getNodeFromLocalPath(B(""), B("/proc"), true);

    std::string error;
    if (!protondesktop::uwp::BeginGuestWorkerSession(error)) return 67;
    auto process = KProcess::create();
    if (!process->startProcess(B("/"), {B("/process-identity.elf")}, {}, 1000, 1000, 1000, 1000))
        return 68;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (protondesktop::uwp::ProcessHasActiveGuestWorkers(process->id) &&
           std::chrono::steady_clock::now() < deadline)
    {
        if (!protondesktop::uwp::ReapCompletedGuestWorkers(error)) return 69;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // A failed bounded Linux test terminates its own driver process. Never
    // destroy a kernel while an owned guest worker could still be using it.
    if (protondesktop::uwp::ProcessHasActiveGuestWorkers(process->id)) return 70;
    std::printf("ACTUAL_ELF_PROCESS_IDENTITY terminated=%d exit=%u\n",
                process->isTerminated(), process->exitCode);
    const unsigned result = process->isTerminated() ? process->exitCode : 71;
    KSystem::eraseProcess(process->id);
    protondesktop::uwp::CloseGuestWorkerAdmission();
    if (!protondesktop::uwp::JoinGuestWorkers(error) ||
        !protondesktop::uwp::EndGuestWorkerSession(error)) return 72;
    KSystem::destroy();
    KNativeSystem::shutdown();
    KNativeSystem::cleanup();
    return result;
}
