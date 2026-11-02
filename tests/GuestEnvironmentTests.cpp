// Host contract tests for the Wine guest TEB/PEB environment.
// `guest_environment_tests --dump-layout` prints the modeled offsets as JSON
// for tools/verify_guest_layout.py.
#include "runtime/GuestEnvironment.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#    define XCDM_DESKTOP_FIBER_FIXTURE 1
#  endif
#endif
#if defined(_MSC_VER)
#  include <intrin.h>
#endif

using namespace xcdm::guest;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; } } while (0)

    uint64_t A(const void* p) { return reinterpret_cast<uint64_t>(p); }

    template <class T>
    bool AllZero(const T& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(T); ++i) if (bytes[i]) return false;
        return true;
    }

    std::u16string Read(const UnicodeString64& s)
    {
        return std::u16string(reinterpret_cast<const char16_t*>(s.Buffer), s.Length / sizeof(char16_t));
    }

    ProcessConfig BaseConfig()
    {
        ProcessConfig c;
        c.processId = 0x20;
        c.imageBase = 0x140000000ull;
        c.processHeap = 0x7ff700010000ull;
        c.imagePath = u"C:\\windows\\explorer.exe";
        c.commandLine = u"C:\\windows\\explorer.exe /desktop";
        c.currentDirectory = u"C:\\windows\\system32\\";
        c.dllPath = u"C:\\windows\\system32";
        c.environment = { u"SystemRoot=C:\\windows", u"PATH=C:\\windows\\system32", u"=C:=C:\\windows" };
        c.numberOfProcessors = 6;
        return c;
    }

    bool FailingStack(uint64_t*, uint64_t*, uint64_t*) { return false; }
    bool g_failStackQuery = false;
    bool ControllableStack(uint64_t* base, uint64_t* limit, uint64_t* deallocation)
    {
        return !g_failStackQuery && DefaultHostServices().currentStack(base, limit, deallocation);
    }

    void DumpLayout()
    {
#define F(type, field) std::printf("    \"%s.%s\": %zu,\n", #type, #field, offsetof(type, field))
        std::printf("{\n");
        F(Teb64, Tib.StackBase); F(Teb64, Tib.StackLimit); F(Teb64, Tib.FiberData); F(Teb64, Tib.Self);
        F(Teb64, EnvironmentPointer); F(Teb64, ClientId); F(Teb64, ActiveRpcHandle);
        F(Teb64, ThreadLocalStoragePointer); F(Teb64, Peb); F(Teb64, LastErrorValue);
        F(Teb64, CountOfOwnedCriticalSections); F(Teb64, CsrClientThread); F(Teb64, Win32ThreadInfo);
        F(Teb64, User32Reserved); F(Teb64, UserReserved); F(Teb64, WOW32Reserved); F(Teb64, CurrentLocale);
        F(Teb64, FpSoftwareStatusRegister); F(Teb64, ReservedForDebuggerInstrumentation);
        F(Teb64, SystemReserved1); F(Teb64, PlaceholderCompatibilityMode);
        F(Teb64, PlaceholderHydrationAlwaysExplicit); F(Teb64, PlaceholderReserved);
        F(Teb64, ProxiedProcessId); F(Teb64, ActivationContextStack); F(Teb64, WorkingOnBehalfOfTicket);
        F(Teb64, ExceptionCode); F(Teb64, ActivationContextStackPointer); F(Teb64, InstrumentationCallbackSp);
        F(Teb64, InstrumentationCallbackPreviousPc); F(Teb64, InstrumentationCallbackPreviousSp);
        F(Teb64, TxFsContext); F(Teb64, InstrumentationCallbackDisabled); F(Teb64, UnalignedLoadStoreExceptions);
        F(Teb64, GdiTebBatch); F(Teb64, RealClientId); F(Teb64, GdiCachedProcessHandle); F(Teb64, GdiClientPID);
        F(Teb64, GdiClientTID); F(Teb64, GdiThreadLocaleInfo); F(Teb64, Win32ClientInfo);
        F(Teb64, glDispatchTable); F(Teb64, glReserved1); F(Teb64, glReserved2); F(Teb64, glSectionInfo);
        F(Teb64, glSection); F(Teb64, glTable); F(Teb64, glCurrentRC); F(Teb64, glContext);
        F(Teb64, LastStatusValue); F(Teb64, StaticUnicodeString); F(Teb64, StaticUnicodeBuffer);
        F(Teb64, DeallocationStack); F(Teb64, TlsSlots); F(Teb64, TlsLinks); F(Teb64, Vdm);
        F(Teb64, ReservedForNtRpc); F(Teb64, DbgSsReserved); F(Teb64, HardErrorMode); F(Teb64, Instrumentation);
        F(Teb64, ActivityId); F(Teb64, SubProcessTag); F(Teb64, PerflibData); F(Teb64, EtwTraceData);
        F(Teb64, WinSockData); F(Teb64, GdiBatchCount); F(Teb64, IdealProcessorValue);
        F(Teb64, GuaranteedStackBytes); F(Teb64, ReservedForPerf); F(Teb64, ReservedForOle);
        F(Teb64, WaitingOnLoaderLock); F(Teb64, SavedPriorityState); F(Teb64, ReservedForCodeCoverage);
        F(Teb64, ThreadPoolData); F(Teb64, TlsExpansionSlots); F(Teb64, DeallocationBStore); F(Teb64, BStoreLimit);
        F(Teb64, MuiGeneration); F(Teb64, IsImpersonating); F(Teb64, NlsCache); F(Teb64, ShimData);
        F(Teb64, HeapVirtualAffinity); F(Teb64, CurrentTransactionHandle); F(Teb64, ActiveFrame);
        F(Teb64, FlsSlots); F(Teb64, PreferredLanguages); F(Teb64, UserPrefLanguages);
        F(Teb64, MergedPrefLanguages); F(Teb64, MuiImpersonation); F(Teb64, CrossTebFlags);
        F(Teb64, SameTebFlags); F(Teb64, TxnScopeEnterCallback); F(Teb64, TxnScopeExitCallback);
        F(Teb64, TxnScopeContext); F(Teb64, LockCount); F(Teb64, WowTebOffset); F(Teb64, ResourceRetValue);
        F(Teb64, ReservedForWdf); F(Teb64, ReservedForCrt); F(Teb64, EffectiveContainerId);

        F(Peb64, InheritedAddressSpace); F(Peb64, ReadImageFileExecOptions); F(Peb64, BeingDebugged);
        F(Peb64, BitField); F(Peb64, CrossProcessFlags); F(Peb64, TracingFlags);
        F(Peb64, Mutant); F(Peb64, ImageBaseAddress); F(Peb64, LdrData); F(Peb64, ProcessParameters);
        F(Peb64, SubSystemData); F(Peb64, ProcessHeap); F(Peb64, FastPebLock); F(Peb64, AtlThunkSListPtr);
        F(Peb64, IFEOKey); F(Peb64, KernelCallbackTable); F(Peb64, Reserved); F(Peb64, AtlThunkSListPtr32);
        F(Peb64, ApiSetMap); F(Peb64, TlsExpansionCounter); F(Peb64, TlsBitmap); F(Peb64, TlsBitmapBits);
        F(Peb64, ReadOnlySharedMemoryBase); F(Peb64, SharedData); F(Peb64, ReadOnlyStaticServerData);
        F(Peb64, AnsiCodePageData); F(Peb64, OemCodePageData); F(Peb64, UnicodeCaseTableData);
        F(Peb64, NumberOfProcessors); F(Peb64, NtGlobalFlag); F(Peb64, CriticalSectionTimeout);
        F(Peb64, HeapSegmentReserve); F(Peb64, HeapSegmentCommit); F(Peb64, HeapDeCommitTotalFreeThreshold);
        F(Peb64, HeapDeCommitFreeBlockThreshold); F(Peb64, NumberOfHeaps); F(Peb64, MaximumNumberOfHeaps);
        F(Peb64, ProcessHeaps); F(Peb64, GdiSharedHandleTable); F(Peb64, ProcessStarterHelper);
        F(Peb64, GdiDCAttributeList); F(Peb64, LoaderLock); F(Peb64, OSMajorVersion); F(Peb64, OSMinorVersion);
        F(Peb64, OSBuildNumber); F(Peb64, OSPlatformId); F(Peb64, ImageSubSystem);
        F(Peb64, ImageSubSystemMajorVersion); F(Peb64, ImageSubSystemMinorVersion);
        F(Peb64, ActiveProcessAffinityMask); F(Peb64, GdiHandleBuffer); F(Peb64, PostProcessInitRoutine);
        F(Peb64, TlsExpansionBitmap); F(Peb64, TlsExpansionBitmapBits); F(Peb64, SessionId);
        F(Peb64, AppCompatFlags); F(Peb64, AppCompatFlagsUser); F(Peb64, ShimData); F(Peb64, AppCompatInfo);
        F(Peb64, CSDVersion); F(Peb64, ActivationContextData); F(Peb64, ProcessAssemblyStorageMap);
        F(Peb64, SystemDefaultActivationData); F(Peb64, SystemAssemblyStorageMap); F(Peb64, MinimumStackCommit);
        F(Peb64, SparePointers); F(Peb64, PatchLoaderData); F(Peb64, ChpeV2ProcessInfo);
        F(Peb64, AppModelFeatureState); F(Peb64, SpareUlongs); F(Peb64, ActiveCodePage); F(Peb64, OemCodePage);
        F(Peb64, UseCaseMapping); F(Peb64, UnusedNlsField); F(Peb64, WerRegistrationData);
        F(Peb64, WerShipAssertPtr); F(Peb64, EcCodeBitMap); F(Peb64, pImageHeaderHash);
        F(Peb64, CsrServerReadOnlySharedMemoryBase); F(Peb64, TppWorkerpListLock); F(Peb64, TppWorkerpList);
        F(Peb64, WaitOnAddressHashTable); F(Peb64, TelemetryCoverageHeader); F(Peb64, CloudFileFlags);
        F(Peb64, CloudFileDiagFlags); F(Peb64, PlaceholderCompatibilityMode);
        F(Peb64, PlaceholderCompatibilityModeReserved); F(Peb64, LeapSecondData); F(Peb64, LeapSecondFlags);
        F(Peb64, NtGlobalFlag2);
        std::printf("    \"sizeof.Teb64\": %zu,\n    \"sizeof.Peb64\": %zu\n}\n", sizeof(Teb64), sizeof(Peb64));
#undef F
    }

    void TestRejectsInvalidConfiguration()
    {
        const HostServices host = DefaultHostServices();
        std::string error;
        ProcessConfig c = BaseConfig();
        c.processId = 0;
        CHECK(!GuestProcess::Create(c, host, &error) && !error.empty());
        c = BaseConfig(); c.processId = 0x21;
        CHECK(!GuestProcess::Create(c, host, &error));
        c = BaseConfig(); c.currentDirectory = u"C:\\windows";
        CHECK(!GuestProcess::Create(c, host, &error));
        c = BaseConfig(); c.environment = { u"NOEQUALS" };
        CHECK(!GuestProcess::Create(c, host, &error));
        c = BaseConfig(); c.numberOfProcessors = 0;
        CHECK(!GuestProcess::Create(c, host, &error));
        c = BaseConfig(); c.imagePath = std::u16string(40000, u'a');
        CHECK(!GuestProcess::Create(c, host, &error));
        HostServices none;
        CHECK(!GuestProcess::Create(BaseConfig(), none, &error));
    }

    void TestUnattachedThreadHasNoTeb()
    {
        CHECK(CurrentThread() == nullptr);
        CHECK(xcdm_guest_current_teb() == nullptr);
        uint32_t value = 7;
        CHECK(!SetGuestLastError(5));
        CHECK(!GetGuestLastError(&value) && value == 7);
    }

    void TestPebAndParameters()
    {
        std::string error;
        auto process = GuestProcess::Create(BaseConfig(), DefaultHostServices(), &error);
        CHECK(process != nullptr);
        if (!process) { std::cerr << error << "\n"; return; }
        const Peb64& peb = *process->Peb();
        CHECK(peb.ImageBaseAddress == 0x140000000ull);
        CHECK(peb.ProcessHeap == 0x7ff700010000ull);
        CHECK(peb.ProcessParameters == A(process->Parameters()));
        CHECK(peb.NumberOfProcessors == 6 && peb.ActiveProcessAffinityMask == 0x3f);
        CHECK(peb.OSMajorVersion == 10 && peb.OSBuildNumber == 19045 && peb.OSPlatformId == 2);
        CHECK(peb.ImageSubSystem == 2 && peb.SessionId == 1);
        CHECK(peb.ActiveCodePage == 1252 && peb.OemCodePage == 437);
        CHECK(peb.KernelCallbackTable == 0);
        CHECK(peb.BeingDebugged == 0);

        const auto* tls = reinterpret_cast<const RtlBitmap64*>(peb.TlsBitmap);
        CHECK(tls && tls->SizeOfBitMap == 64 && tls->Buffer == A(peb.TlsBitmapBits));
        const auto* tlsx = reinterpret_cast<const RtlBitmap64*>(peb.TlsExpansionBitmap);
        CHECK(tlsx && tlsx->SizeOfBitMap == 1024 && tlsx->Buffer == A(peb.TlsExpansionBitmapBits));
        const auto* heaps = reinterpret_cast<const uint64_t*>(peb.ProcessHeaps);
        CHECK(heaps && heaps[0] == peb.ProcessHeap && peb.NumberOfHeaps == 1);

        const auto* ldr = reinterpret_cast<const PebLdrData64*>(peb.LdrData);
        CHECK(ldr && ldr->Length == sizeof(PebLdrData64) && ldr->Initialized == 1);
        CHECK(ldr && ldr->InLoadOrderModuleList.Flink == A(&ldr->InLoadOrderModuleList));
        CHECK(ldr && ldr->InMemoryOrderModuleList.Blink == A(&ldr->InMemoryOrderModuleList));

        // gdi32 reads Peb->GdiSharedHandleTable->Handles[] directly.
        CHECK(peb.GdiSharedHandleTable == A(process->GdiSharedTable()) && peb.GdiSharedHandleTable != 0);
        auto* gdi = static_cast<unsigned char*>(process->GdiSharedTable());
        CHECK(gdi[0] == 0 && gdi[GdiSharedMemorySize - 1] == 0);
        gdi[GdiSharedMemorySize - 1] = 1; // Whole table is writable.

        const ProcessParameters64& p = *process->Parameters();
        CHECK(p.Flags == ProcessParamsFlagNormalized);
        CHECK(p.Size == p.AllocationSize && p.Size >= sizeof(ProcessParameters64));
        CHECK(Read(p.ImagePathName) == u"C:\\windows\\explorer.exe");
        CHECK(Read(p.CommandLine) == u"C:\\windows\\explorer.exe /desktop");
        CHECK(Read(p.CurrentDirectory.DosPath) == u"C:\\windows\\system32\\");
        CHECK(Read(p.DllPath) == u"C:\\windows\\system32");
        CHECK(Read(p.Desktop) == u"WinSta0\\Default");
        CHECK(p.ImagePathName.MaximumLength == p.ImagePathName.Length + 2);
        CHECK(reinterpret_cast<const char16_t*>(p.ImagePathName.Buffer)[p.ImagePathName.Length / 2] == 0);
        CHECK(p.ConsoleHandle == 0 && p.hStdInput == 0 && p.hStdOutput == 0 && p.hStdError == 0);

        // Environment: NAME=value\0 ... \0, size in bytes including the final NUL.
        const auto* env = reinterpret_cast<const char16_t*>(p.Environment);
        std::vector<std::u16string> entries;
        const char16_t* cursor = env;
        while (*cursor)
        {
            std::u16string entry(cursor);
            cursor += entry.size() + 1;
            entries.push_back(entry);
        }
        CHECK(entries.size() == 3 && entries[0] == u"SystemRoot=C:\\windows" && entries[2] == u"=C:=C:\\windows");
        CHECK(p.EnvironmentSize == static_cast<uint64_t>(cursor - env + 1) * 2);

        // Command line defaults to the image path.
        ProcessConfig c = BaseConfig();
        c.commandLine.clear();
        c.environment.clear();
        c.allocateGdiSharedTable = false;
        auto second = GuestProcess::Create(c, DefaultHostServices(), &error);
        CHECK(second && Read(second->Parameters()->CommandLine) == c.imagePath);
        CHECK(second && second->Peb()->GdiSharedHandleTable == 0);
        if (second)
        {
            const auto* e = reinterpret_cast<const char16_t*>(second->Parameters()->Environment);
            CHECK(e[0] == 0 && e[1] == 0 && second->Parameters()->EnvironmentSize == 4);
        }

        c.imagePath = u"C:\\" + std::u16string(240, u'a') + u"\\explorer.exe";
        auto longPath = GuestProcess::Create(c, DefaultHostServices(), &error);
        CHECK(longPath != nullptr);
        CHECK(longPath && Read(longPath->Parameters()->CommandLine) == c.imagePath);
        CHECK(longPath && Read(longPath->Parameters()->ImagePathName) == c.imagePath);
        CHECK(longPath && Read(longPath->Parameters()->WindowTitle) == c.imagePath);
    }

    void TestTebInitialization()
    {
        std::string error;
        auto process = GuestProcess::Create(BaseConfig(), DefaultHostServices(), &error);
        CHECK(process != nullptr);
        if (!process) return;
        GuestThread* thread = process->AttachCurrentThread(&error);
        CHECK(thread != nullptr);
        if (!thread) return;
        CHECK(process->AttachCurrentThread(&error) == thread); // Idempotent.
        CHECK(CurrentThread() == thread);

        Teb64* teb = static_cast<Teb64*>(xcdm_guest_current_teb());
        CHECK(teb == thread->Teb());
        CHECK(teb->Tib.Self == A(teb));
        CHECK(teb->Peb == A(process->Peb()));
        CHECK(teb->ClientId.UniqueProcess == 0x20 && teb->ClientId.UniqueThread == thread->HostThreadId());
        CHECK(teb->ClientId.UniqueThread != 0);
        CHECK(teb->CurrentLocale == 0x0409);
        CHECK(teb->LastErrorValue == 0);
        CHECK(teb->StaticUnicodeString.Buffer == A(teb->StaticUnicodeBuffer));
        CHECK(teb->StaticUnicodeString.MaximumLength == sizeof(teb->StaticUnicodeBuffer));
        CHECK(teb->ActivationContextStackPointer == A(&teb->ActivationContextStack));
        CHECK(teb->ActivationContextStack.FrameListCache.Flink == A(&teb->ActivationContextStack.FrameListCache));
        CHECK(teb->TlsLinks.Flink == A(&teb->TlsLinks) && teb->TlsLinks.Blink == A(&teb->TlsLinks));
        // Fields owned by Wine user32/win32u or meaning WoW64 when non-zero.
        CHECK(AllZero(teb->Win32ClientInfo) && teb->Win32ThreadInfo == 0);
        CHECK(teb->GdiBatchCount == 0);
        CHECK(teb->ThreadLocalStoragePointer == 0);
        CHECK(teb->TlsExpansionSlots == 0 && teb->FlsSlots == 0);
        CHECK(AllZero(teb->TlsSlots));

        // Inline winbase.h helpers read raw pointer-indexed slots.
        const auto* raw = reinterpret_cast<void* const*>(teb);
        CHECK(reinterpret_cast<uint64_t>(raw[8]) == 0x20);                       // GetCurrentProcessId
        CHECK(reinterpret_cast<uint64_t>(raw[9]) == thread->HostThreadId());     // GetCurrentThreadId
        const auto* pebSlots = reinterpret_cast<uint64_t* const*>(teb)[12];
        CHECK(pebSlots[6] == 0x7ff700010000ull);                                 // GetProcessHeap
        *reinterpret_cast<uint32_t*>(const_cast<void**>(raw) + 13) = 1234;       // inline SetLastError
        uint32_t lastError = 0;
        CHECK(GetGuestLastError(&lastError) && lastError == 1234);
        CHECK(SetGuestLastError(87) && teb->LastErrorValue == 87);

        // Stack bounds mirror the live host thread stack.
        // Use the real frame address: ASan's fake stacks move locals elsewhere.
#if defined(_MSC_VER)
        const uint64_t sp = A(_AddressOfReturnAddress());
#else
        const uint64_t sp = A(__builtin_frame_address(0));
#endif
        CHECK(teb->Tib.StackLimit <= sp && sp < teb->Tib.StackBase);
        CHECK(teb->DeallocationStack != 0 && teb->DeallocationStack <= teb->Tib.StackLimit);

        process->DetachCurrentThread();
        CHECK(CurrentThread() == nullptr && xcdm_guest_current_teb() == nullptr);
        CHECK(process->ThreadCount() == 0);
    }

    void TestFailedStackQueryPreservesBounds()
    {
        HostServices host = DefaultHostServices();
        host.currentStack = &ControllableStack;
        std::string error;
        auto process = GuestProcess::Create(BaseConfig(), host, &error);
        CHECK(process != nullptr);
        if (!process) return;
        GuestThread* thread = process->AttachCurrentThread(&error);
        CHECK(thread != nullptr);
        if (!thread) return;
        const uint64_t base = thread->Teb()->Tib.StackBase;
        CHECK(base != 0);
        const uint64_t limit = thread->Teb()->Tib.StackLimit;
        const uint64_t deallocation = thread->Teb()->DeallocationStack;
        // A later failed refresh preserves the last qualified bounds.
        g_failStackQuery = true;
        thread->SyncStack();
        CHECK(thread->Teb()->Tib.StackBase == base && thread->Teb()->Tib.StackLimit == limit &&
              thread->Teb()->DeallocationStack == deallocation);
        g_failStackQuery = false;
        process->DetachCurrentThread();

        host.currentStack = &FailingStack;
        auto failing = GuestProcess::Create(BaseConfig(), host, &error);
        CHECK(failing != nullptr);
        if (!failing) return;
        GuestThread* t = failing->AttachCurrentThread(&error);
        CHECK(t == nullptr && !error.empty());
        CHECK(failing->ThreadCount() == 0 && CurrentThread() == nullptr);
    }

    void TestThreadIsolationAndBinding()
    {
        std::string error;
        auto process = GuestProcess::Create(BaseConfig(), DefaultHostServices(), &error);
        ProcessConfig other = BaseConfig();
        other.processId = 0x24;
        auto process2 = GuestProcess::Create(other, DefaultHostServices(), &error);
        CHECK(process && process2);
        if (!process || !process2) return;

        GuestThread* main = process->AttachCurrentThread(&error);
        CHECK(main != nullptr);
        CHECK(process2->AttachCurrentThread(&error) == nullptr && !error.empty());
        SetGuestLastError(5);

        std::atomic<bool> ok{ true };
        std::atomic<uint64_t> workerTeb{ 0 };
        std::thread worker([&] {
            std::string e;
            GuestThread* t = process->AttachCurrentThread(&e);
            if (!t || t == main) { ok = false; return; }
            uint32_t value = 99;
            if (!GetGuestLastError(&value) || value != 0) ok = false; // fresh TEB
            SetGuestLastError(1450);
            if (t->Teb()->Peb != A(process->Peb())) ok = false;
            if (t->Teb()->ClientId.UniqueThread == main->HostThreadId()) ok = false;
            workerTeb = A(t->Teb());
            // Exit without DetachCurrentThread: the thread-exit hook must
            // release the TEB.
        });
        worker.join();
        CHECK(ok.load());
        CHECK(workerTeb.load() != 0 && workerTeb.load() != A(main->Teb()));
        uint32_t value = 0;
        CHECK(GetGuestLastError(&value) && value == 5);
        CHECK(process->ThreadCount() == 1);

        // The second process gets its own PEB and TEB once this thread is free.
        process->DetachCurrentThread();
        GuestThread* t2 = process2->AttachCurrentThread(&error);
        CHECK(t2 && t2->Teb()->Peb == A(process2->Peb()) && t2->Teb()->ClientId.UniqueProcess == 0x24);
        CHECK(process2->Peb() != process->Peb());
        process2->DetachCurrentThread();
    }

    void TestProcessDestructionClearsCurrentBinding()
    {
        std::string error;
        auto process = GuestProcess::Create(BaseConfig(), DefaultHostServices(), &error);
        CHECK(process != nullptr);
        if (!process) return;
        CHECK(process->AttachCurrentThread(&error) != nullptr);
        process.reset();
        CHECK(CurrentThread() == nullptr && xcdm_guest_current_teb() == nullptr);

        auto replacement = GuestProcess::Create(BaseConfig(), DefaultHostServices(), &error);
        CHECK(replacement != nullptr);
        if (!replacement) return;
        CHECK(replacement->AttachCurrentThread(&error) != nullptr);
        replacement->DetachCurrentThread();
        CHECK(replacement->ThreadCount() == 0 && CurrentThread() == nullptr);
    }

#if defined(XCDM_DESKTOP_FIBER_FIXTURE)
    struct FiberBindingFixture
    {
        GuestProcess* process;
        void* caller;
        GuestThread* binding = nullptr;
        std::string error;
    };

    void WINAPI AttachFromFiber(void* value)
    {
        auto* fixture = static_cast<FiberBindingFixture*>(value);
        fixture->binding = fixture->process->AttachCurrentThread(&fixture->error);
        SwitchToFiber(fixture->caller);
        // The test deletes this suspended fiber rather than resuming it.
        std::abort();
    }

    void TestHostFiberCannotInheritGuestBinding()
    {
        // Desktop host fixture only. The runtime itself uses only FLS APIs
        // available to UWP and does not implement or create guest fibers.
        void* caller = ConvertThreadToFiber(nullptr);
        CHECK(caller != nullptr);
        if (!caller) return;
        std::string error;
        auto process = GuestProcess::Create(BaseConfig(), DefaultHostServices(), &error);
        CHECK(process != nullptr);
        if (!process) { CHECK(ConvertFiberToThread()); return; }
        FiberBindingFixture fixture{ process.get(), caller };
        void* fiber = CreateFiber(0, &AttachFromFiber, &fixture);
        CHECK(fiber != nullptr);
        if (!fiber) { CHECK(ConvertFiberToThread()); return; }

        SwitchToFiber(fiber);
        CHECK(fixture.binding != nullptr);
        CHECK(CurrentThread() == nullptr && xcdm_guest_current_teb() == nullptr);
        CHECK(!SetGuestLastError(42));
        uint32_t lastError = 99;
        CHECK(!GetGuestLastError(&lastError) && lastError == 99);
        CHECK(process->AttachCurrentThread(&error) == nullptr && !error.empty());
        process->DetachCurrentThread(); // Must leave the suspended fiber alone.
        CHECK(process->ThreadCount() == (fixture.binding ? 1u : 0u));

        DeleteFiber(fiber); // FLS callback invalidates cache before releasing TEB.
        CHECK(process->ThreadCount() == 0);
        CHECK(CurrentThread() == nullptr && xcdm_guest_current_teb() == nullptr);
        CHECK(process->AttachCurrentThread(&error) != nullptr);
        process->DetachCurrentThread();
        CHECK(process->ThreadCount() == 0 && CurrentThread() == nullptr);
        CHECK(ConvertFiberToThread());
    }
#endif

    void TestKernelCallbackTable()
    {
        std::string error;
        auto process = GuestProcess::Create(BaseConfig(), DefaultHostServices(), &error);
        CHECK(process != nullptr);
        if (!process) return;
        CHECK(process->KernelCallback(0) == 0);
        static uint64_t table[KernelCallbackCount];
        for (uint32_t i = 0; i < KernelCallbackCount; ++i) table[i] = 0x1000 + i;
        // Wine user32 DllMain: NtCurrentTeb()->Peb->KernelCallbackTable = table;
        GuestThread* thread = process->AttachCurrentThread(&error);
        CHECK(thread != nullptr);
        if (!thread) return;
        reinterpret_cast<Peb64*>(thread->Teb()->Peb)->KernelCallbackTable = A(table);
        CHECK(process->KernelCallback(0) == 0x1000);
        CHECK(process->KernelCallback(KernelCallbackCount - 1) == 0x1000 + KernelCallbackCount - 1);
        CHECK(process->KernelCallback(KernelCallbackCount) == 0);
        process->DetachCurrentThread();
    }

    void TestHostTebUntouched()
    {
#if defined(_WIN32)
        // Guest writes to every field Wine user32/gdi32/ole32/kernelbase touch
        // must land in the guest block, never in the host TEB/PEB.
        auto* hostTeb = reinterpret_cast<unsigned char*>(::NtCurrentTeb());
        std::vector<unsigned char> before(hostTeb, hostTeb + sizeof(Teb64));
        const uint64_t hostPeb = *reinterpret_cast<uint64_t*>(hostTeb + offsetof(Teb64, Peb));
        const uint64_t hostCallbacks = *reinterpret_cast<uint64_t*>(hostPeb + offsetof(Peb64, KernelCallbackTable));
        const uint64_t hostGdi = *reinterpret_cast<uint64_t*>(hostPeb + offsetof(Peb64, GdiSharedHandleTable));

        std::string error;
        auto process = GuestProcess::Create(BaseConfig(), DefaultHostServices(), &error);
        CHECK(process != nullptr);
        if (!process) return;
        Teb64* teb = process->AttachCurrentThread(&error)->Teb();
        CHECK(A(teb) != A(hostTeb));
        ::SetLastError(ERROR_SUCCESS);
        teb->LastErrorValue = 0xdead;
        for (auto& v : teb->Win32ClientInfo) v = 0x5555;
        teb->ReservedForOle = 1;
        teb->ReservedForNtRpc = 2;
        teb->TlsSlots[3] = 3;
        teb->WinSockData = 4;
        auto* peb = reinterpret_cast<Peb64*>(teb->Peb);
        peb->KernelCallbackTable = 0x1234;
        CHECK(::GetLastError() == ERROR_SUCCESS);
        CHECK(*reinterpret_cast<uint64_t*>(hostPeb + offsetof(Peb64, KernelCallbackTable)) == hostCallbacks);
        CHECK(*reinterpret_cast<uint64_t*>(hostPeb + offsetof(Peb64, GdiSharedHandleTable)) == hostGdi);
        const size_t w32 = offsetof(Teb64, Win32ClientInfo);
        CHECK(std::memcmp(before.data() + w32, hostTeb + w32, sizeof(teb->Win32ClientInfo)) == 0);
        const size_t ole = offsetof(Teb64, ReservedForOle);
        CHECK(std::memcmp(before.data() + ole, hostTeb + ole, 8) == 0);
        process->DetachCurrentThread();
#else
        // POSIX hosts have no NT TEB; the guest block is the only TEB.
        std::string error;
        auto process = GuestProcess::Create(BaseConfig(), DefaultHostServices(), &error);
        CHECK(process && process->AttachCurrentThread(&error));
        if (process) process->DetachCurrentThread();
#endif
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--dump-layout") == 0)
    {
        DumpLayout();
        return 0;
    }
    TestRejectsInvalidConfiguration();
    TestUnattachedThreadHasNoTeb();
    TestPebAndParameters();
    TestTebInitialization();
    TestFailedStackQueryPreservesBounds();
    TestThreadIsolationAndBinding();
    TestProcessDestructionClearsCurrentBinding();
#if defined(XCDM_DESKTOP_FIBER_FIXTURE)
    TestHostFiberCannotInheritGuestBinding();
#endif
    TestKernelCallbackTable();
    TestHostTebUntouched();
    std::cout << "guest environment: " << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
