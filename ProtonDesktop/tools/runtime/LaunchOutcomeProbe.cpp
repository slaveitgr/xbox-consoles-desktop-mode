// SPDX-License-Identifier: GPL-2.0-only
// Offline Linux regression probe using the REAL pinned kernel and ELF loader.
// No Wine runs. Rejected launches create no guest worker; the two successful
// controls execute ONLY our self-authored static ELF instruction sequences.
#include "boxedwine.h"
#include "knativesystem.h"
#include "backend/uwp/UwpGuestWorkers.h"
#include <cstdio>
#include <chrono>
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
    KSystem::procNode = Fs::getNodeFromLocalPath(B(""), B("/proc"), true);
    int failed = 0;
    auto check = [&](bool condition, const char* label)
    {
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", label);
        if (!condition) ++failed;
    };
    for (const char* path : {"/missing.elf", "/not-elf", "/missing-interpreter.elf", "/valid-static.elf"})
    {
        auto process = KProcess::create();
        KThread* thread = process->startProcess(B("/"), {BString::copy(path)}, {}, 1000, 1000, 1000, 1000);
        check(thread == nullptr, (std::string(path) + " rejects load/admission failure").c_str());
        check(process->getThreadCount() == 0, "no unscheduled phantom KThread remains");
        check(KThread::currentThread() == nullptr, "initial host thread binding is restored");
        KSystem::eraseProcess(process->id);
    }
    const auto workers = protondesktop::uwp::GetGuestWorkerSnapshot();
    check(workers.admitted == 0 && workers.running == 0, "no host guest worker was created");
    std::string error;
    check(protondesktop::uwp::BeginGuestWorkerSession(error), "begin one managed kernel session");
    for (const auto& command : {std::make_pair("/valid-static.elf", 0u),
                               std::make_pair("/negative-static.elf", 9u)})
    {
        auto process = KProcess::create();
        KThread* thread = process->startProcess(B("/"), {BString::copy(command.first)}, {}, 1000, 1000, 1000, 1000);
        check(thread != nullptr, "real static ELF load admits an owned worker");
        check(KThread::currentThread() == nullptr, "successful launch restores the host thread binding");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (protondesktop::uwp::ProcessHasActiveGuestWorkers(process->id) &&
               std::chrono::steady_clock::now() < deadline)
        {
            protondesktop::uwp::ReapCompletedGuestWorkers(error);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(!protondesktop::uwp::ProcessHasActiveGuestWorkers(process->id), "actual worker join completes before process exit observation");
        check(process->isTerminated() && process->exitCode == command.second,
              "real KProcess guest exit status matches the control");
        KSystem::eraseProcess(process->id);
    }
    protondesktop::uwp::CloseGuestWorkerAdmission();
    check(protondesktop::uwp::JoinGuestWorkers(error), "all managed workers joined");
    check(protondesktop::uwp::EndGuestWorkerSession(error), "owned registry ends before kernel teardown");
    KSystem::destroy();
    KNativeSystem::shutdown();
    KNativeSystem::cleanup();
    std::printf("launch-outcome probe: %d failed\n", failed);
    return failed ? 1 : 0;
}
