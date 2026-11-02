// SPDX-License-Identifier: GPL-2.0-only
// Offline Linux regression probe using the REAL pinned kernel and ELF loader.
// No Wine runs. Rejected launches create no guest worker; the two successful
// controls execute ONLY our self-authored static ELF instruction sequences.
#include "boxedwine.h"
#include "kmemory64.h"
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
    {
        KMemory64 parent(nullptr), child(nullptr);
        const U64 inherited = parent.mmapReserveAndMap(8192, 3);
        parent.writeq(inherited, 0x1122334455667788ULL);
        const U64 reserved = parent.mmapReserveAndMap(8192, 0);
        child.cloneFrom(&parent);
        check(child.readq(inherited) == 0x1122334455667788ULL,
              "fork inherits committed guest bytes");
        const U64 next = child.mmapReserveAndMap(4096, 3);
        check(next >= reserved + 8192,
              "child mmap preserves committed and untouched PROT_NONE reservations");
        check(child.readq(inherited) == 0x1122334455667788ULL,
              "child mmap does not overwrite inherited dynamic-linker data");
        child.writeq(inherited, 0x8877665544332211ULL);
        check(parent.readq(inherited) == 0x1122334455667788ULL,
              "private fork writes leave the parent address space unchanged");
    }
    {
        // Use the real registry-backed MAP_SHARED implementation, not a host
        // allocation or a copied test implementation. A fork must preserve
        // this alias while the preceding anonymous mapping remains private.
        KMemory64 parent(nullptr);
        const U64 shared = parent.mmapReserveAndMap(K64_PAGE_SIZE, 3);
        const U64 seed = 0x123456789abcdef0ULL;
        constexpr const char* sharedPath = "/protondesktop-fixtures/shared-fork.page";
        check(parent.mmapSharedFile(shared, K64_PAGE_SIZE, 3, sharedPath, 0,
                                    reinterpret_cast<const U8*>(&seed), sizeof(seed)) == shared,
              "real shared file mapping is seeded through the shared-page registry");
        {
            KMemory64 child(nullptr);
            child.cloneFrom(&parent);
            check(child.readq(shared) == seed,
                  "fork inherits committed shared bytes");
            check((child.getPageFlags(shared >> K64_PAGE_SHIFT) & K64_PAGE_SHARED) != 0,
                  "fork retains the shared page flag");
            check(parent.getCommittedPagePtr(shared >> K64_PAGE_SHIFT) ==
                  child.getCommittedPagePtr(shared >> K64_PAGE_SHIFT),
                  "fork retains the actual shared backing buffer");
            parent.writeq(shared, 0x1122334455667788ULL);
            check(child.readq(shared) == 0x1122334455667788ULL,
                  "parent writes are visible through the inherited shared mapping");
            child.writeq(shared, 0x8877665544332211ULL);
            check(parent.readq(shared) == 0x8877665544332211ULL,
                  "child writes are visible through the parent shared mapping");
        }
        check(parent.readq(shared) == 0x8877665544332211ULL,
              "destroying the fork child preserves registry-owned shared bytes");
        KMemory64 peer(nullptr);
        const U64 peerAddress = peer.mmapReserveAndMap(K64_PAGE_SIZE, 3);
        const U64 replacementSeed = 0;
        check(peer.mmapSharedFile(peerAddress, K64_PAGE_SIZE, 3, sharedPath, 0,
                                  reinterpret_cast<const U8*>(&replacementSeed),
                                  sizeof(replacementSeed)) == peerAddress &&
              peer.readq(peerAddress) == 0x8877665544332211ULL,
              "later shared mapping aliases the registry without reseeding it");
        peer.writeq(peerAddress, 0xfedcba9876543210ULL);
        check(parent.readq(shared) == 0xfedcba9876543210ULL,
              "registry alias remains shared after fork child destruction");
    }
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
