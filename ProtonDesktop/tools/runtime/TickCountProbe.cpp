// SPDX-License-Identifier: GPL-2.0-only
// Real pinned Linux kernel/CPU64 integration; only our own static ELF executes.
#include "boxedwine.h"
#include "knativesystem.h"
#include "backend/uwp/UwpGuestWorkers.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 3) return 64;
    const unsigned expectedExit = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
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
    KThread* thread = process->startProcess(B("/"), {B("/tick-count.elf")}, {}, 1000, 1000, 1000, 1000);
    if (!thread) return 68;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (protondesktop::uwp::ProcessHasActiveGuestWorkers(process->id) &&
           std::chrono::steady_clock::now() < deadline)
    {
        if (!protondesktop::uwp::ReapCompletedGuestWorkers(error)) return 69;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (protondesktop::uwp::ProcessHasActiveGuestWorkers(process->id)) std::_Exit(70);
    const bool passed = process->isTerminated() && process->exitCode == expectedExit;
    std::printf("ACTUAL_TICK_COUNT_ELF terminated=%d guest_exit=%u expected=%u\n",
        process->isTerminated(), process->exitCode, expectedExit);
    KSystem::eraseProcess(process->id);
    protondesktop::uwp::CloseGuestWorkerAdmission();
    if (!protondesktop::uwp::JoinGuestWorkers(error) ||
        !protondesktop::uwp::EndGuestWorkerSession(error)) return 71;
    KSystem::destroy();
    KNativeSystem::shutdown();
    KNativeSystem::cleanup();
    return passed ? 0 : 72;
}
