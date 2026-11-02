#include "runtime/GuestEnvironment.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#  include <sys/mman.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

using namespace xcdm::guest;

// --- Layout contract: Wine x64 offsets (winternl.h offset comments) --------
#define XCDM_OFF(type, field, value) \
    static_assert(offsetof(type, field) == (value), #type "." #field " offset differs from Wine x64")

static_assert(sizeof(NtTib64) == 0x38, "NT_TIB64 size");
XCDM_OFF(NtTib64, StackBase, 0x08);
XCDM_OFF(NtTib64, StackLimit, 0x10);
XCDM_OFF(NtTib64, FiberData, 0x20);
XCDM_OFF(NtTib64, Self, 0x30);
static_assert(sizeof(ActivationContextStack64) == 0x28, "ACTIVATION_CONTEXT_STACK64 size");
static_assert(sizeof(GdiTebBatch64) == 0x4e8, "GDI_TEB_BATCH64 size");

XCDM_OFF(Teb64, EnvironmentPointer, 0x0038);
XCDM_OFF(Teb64, ClientId, 0x0040);
XCDM_OFF(Teb64, ActiveRpcHandle, 0x0050);
XCDM_OFF(Teb64, ThreadLocalStoragePointer, 0x0058);
XCDM_OFF(Teb64, Peb, 0x0060);
XCDM_OFF(Teb64, LastErrorValue, 0x0068);
XCDM_OFF(Teb64, CountOfOwnedCriticalSections, 0x006c);
XCDM_OFF(Teb64, CsrClientThread, 0x0070);
XCDM_OFF(Teb64, Win32ThreadInfo, 0x0078);
XCDM_OFF(Teb64, User32Reserved, 0x0080);
XCDM_OFF(Teb64, UserReserved, 0x00e8);
XCDM_OFF(Teb64, WOW32Reserved, 0x0100);
XCDM_OFF(Teb64, CurrentLocale, 0x0108);
XCDM_OFF(Teb64, FpSoftwareStatusRegister, 0x010c);
XCDM_OFF(Teb64, ReservedForDebuggerInstrumentation, 0x0110);
XCDM_OFF(Teb64, SystemReserved1, 0x0190);
XCDM_OFF(Teb64, PlaceholderCompatibilityMode, 0x0280);
XCDM_OFF(Teb64, ProxiedProcessId, 0x028c);
XCDM_OFF(Teb64, ActivationContextStack, 0x0290);
XCDM_OFF(Teb64, WorkingOnBehalfOfTicket, 0x02b8);
XCDM_OFF(Teb64, ExceptionCode, 0x02c0);
XCDM_OFF(Teb64, ActivationContextStackPointer, 0x02c8);
XCDM_OFF(Teb64, InstrumentationCallbackSp, 0x02d0);
XCDM_OFF(Teb64, TxFsContext, 0x02e8);
XCDM_OFF(Teb64, GdiTebBatch, 0x02f0);
XCDM_OFF(Teb64, RealClientId, 0x07d8);
XCDM_OFF(Teb64, GdiCachedProcessHandle, 0x07e8);
XCDM_OFF(Teb64, GdiClientPID, 0x07f0);
XCDM_OFF(Teb64, GdiThreadLocaleInfo, 0x07f8);
XCDM_OFF(Teb64, Win32ClientInfo, 0x0800);
XCDM_OFF(Teb64, glDispatchTable, 0x09f0);
XCDM_OFF(Teb64, glReserved1, 0x1138);
XCDM_OFF(Teb64, glReserved2, 0x1220);
XCDM_OFF(Teb64, glContext, 0x1248);
XCDM_OFF(Teb64, LastStatusValue, 0x1250);
XCDM_OFF(Teb64, StaticUnicodeString, 0x1258);
XCDM_OFF(Teb64, StaticUnicodeBuffer, 0x1268);
XCDM_OFF(Teb64, DeallocationStack, 0x1478);
XCDM_OFF(Teb64, TlsSlots, 0x1480);
XCDM_OFF(Teb64, TlsLinks, 0x1680);
XCDM_OFF(Teb64, Vdm, 0x1690);
XCDM_OFF(Teb64, ReservedForNtRpc, 0x1698);
XCDM_OFF(Teb64, DbgSsReserved, 0x16a0);
XCDM_OFF(Teb64, HardErrorMode, 0x16b0);
XCDM_OFF(Teb64, Instrumentation, 0x16b8);
XCDM_OFF(Teb64, ActivityId, 0x1710);
XCDM_OFF(Teb64, SubProcessTag, 0x1720);
XCDM_OFF(Teb64, WinSockData, 0x1738);
XCDM_OFF(Teb64, GdiBatchCount, 0x1740);
XCDM_OFF(Teb64, IdealProcessorValue, 0x1744);
XCDM_OFF(Teb64, GuaranteedStackBytes, 0x1748);
XCDM_OFF(Teb64, ReservedForPerf, 0x1750);
XCDM_OFF(Teb64, ReservedForOle, 0x1758);
XCDM_OFF(Teb64, WaitingOnLoaderLock, 0x1760);
XCDM_OFF(Teb64, SavedPriorityState, 0x1768);
XCDM_OFF(Teb64, ReservedForCodeCoverage, 0x1770);
XCDM_OFF(Teb64, ThreadPoolData, 0x1778);
XCDM_OFF(Teb64, TlsExpansionSlots, 0x1780);
XCDM_OFF(Teb64, DeallocationBStore, 0x1788);
XCDM_OFF(Teb64, BStoreLimit, 0x1790);
XCDM_OFF(Teb64, MuiGeneration, 0x1798);
XCDM_OFF(Teb64, IsImpersonating, 0x179c);
XCDM_OFF(Teb64, NlsCache, 0x17a0);
XCDM_OFF(Teb64, ShimData, 0x17a8);
XCDM_OFF(Teb64, HeapVirtualAffinity, 0x17b0);
XCDM_OFF(Teb64, CurrentTransactionHandle, 0x17b8);
XCDM_OFF(Teb64, ActiveFrame, 0x17c0);
XCDM_OFF(Teb64, FlsSlots, 0x17c8);
XCDM_OFF(Teb64, PreferredLanguages, 0x17d0);
XCDM_OFF(Teb64, MuiImpersonation, 0x17e8);
XCDM_OFF(Teb64, CrossTebFlags, 0x17ec);
XCDM_OFF(Teb64, SameTebFlags, 0x17ee);
XCDM_OFF(Teb64, TxnScopeEnterCallback, 0x17f0);
XCDM_OFF(Teb64, LockCount, 0x1808);
XCDM_OFF(Teb64, WowTebOffset, 0x180c);
XCDM_OFF(Teb64, ResourceRetValue, 0x1810);
XCDM_OFF(Teb64, ReservedForCrt, 0x1820);
XCDM_OFF(Teb64, EffectiveContainerId, 0x1828);
static_assert(sizeof(Teb64) == 0x1838, "TEB64 size differs from Wine x64");

XCDM_OFF(Peb64, BeingDebugged, 0x002);
XCDM_OFF(Peb64, Mutant, 0x008);
XCDM_OFF(Peb64, ImageBaseAddress, 0x010);
XCDM_OFF(Peb64, LdrData, 0x018);
XCDM_OFF(Peb64, ProcessParameters, 0x020);
XCDM_OFF(Peb64, SubSystemData, 0x028);
XCDM_OFF(Peb64, ProcessHeap, 0x030);
XCDM_OFF(Peb64, FastPebLock, 0x038);
XCDM_OFF(Peb64, IFEOKey, 0x048);
XCDM_OFF(Peb64, KernelCallbackTable, 0x058);
XCDM_OFF(Peb64, ApiSetMap, 0x068);
XCDM_OFF(Peb64, TlsExpansionCounter, 0x070);
XCDM_OFF(Peb64, TlsBitmap, 0x078);
XCDM_OFF(Peb64, TlsBitmapBits, 0x080);
XCDM_OFF(Peb64, ReadOnlySharedMemoryBase, 0x088);
XCDM_OFF(Peb64, AnsiCodePageData, 0x0a0);
XCDM_OFF(Peb64, OemCodePageData, 0x0a8);
XCDM_OFF(Peb64, UnicodeCaseTableData, 0x0b0);
XCDM_OFF(Peb64, NumberOfProcessors, 0x0b8);
XCDM_OFF(Peb64, NtGlobalFlag, 0x0bc);
XCDM_OFF(Peb64, CriticalSectionTimeout, 0x0c0);
XCDM_OFF(Peb64, NumberOfHeaps, 0x0e8);
XCDM_OFF(Peb64, ProcessHeaps, 0x0f0);
XCDM_OFF(Peb64, GdiSharedHandleTable, 0x0f8);
XCDM_OFF(Peb64, LoaderLock, 0x110);
XCDM_OFF(Peb64, OSMajorVersion, 0x118);
XCDM_OFF(Peb64, OSBuildNumber, 0x120);
XCDM_OFF(Peb64, OSPlatformId, 0x124);
XCDM_OFF(Peb64, ImageSubSystem, 0x128);
XCDM_OFF(Peb64, ImageSubSystemMinorVersion, 0x130);
XCDM_OFF(Peb64, ActiveProcessAffinityMask, 0x138);
XCDM_OFF(Peb64, GdiHandleBuffer, 0x140);
XCDM_OFF(Peb64, PostProcessInitRoutine, 0x230);
XCDM_OFF(Peb64, TlsExpansionBitmap, 0x238);
XCDM_OFF(Peb64, TlsExpansionBitmapBits, 0x240);
XCDM_OFF(Peb64, SessionId, 0x2c0);
XCDM_OFF(Peb64, AppCompatFlags, 0x2c8);
XCDM_OFF(Peb64, CSDVersion, 0x2e8);
XCDM_OFF(Peb64, ActivationContextData, 0x2f8);
XCDM_OFF(Peb64, SystemAssemblyStorageMap, 0x310);
XCDM_OFF(Peb64, MinimumStackCommit, 0x318);
XCDM_OFF(Peb64, ChpeV2ProcessInfo, 0x338);
XCDM_OFF(Peb64, AppModelFeatureState, 0x340);
XCDM_OFF(Peb64, ActiveCodePage, 0x34c);
XCDM_OFF(Peb64, OemCodePage, 0x34e);
XCDM_OFF(Peb64, WerRegistrationData, 0x358);
XCDM_OFF(Peb64, TracingFlags, 0x378);
XCDM_OFF(Peb64, CsrServerReadOnlySharedMemoryBase, 0x380);
XCDM_OFF(Peb64, TppWorkerpList, 0x390);
XCDM_OFF(Peb64, WaitOnAddressHashTable, 0x3a0);
XCDM_OFF(Peb64, TelemetryCoverageHeader, 0x7a0);
XCDM_OFF(Peb64, CloudFileFlags, 0x7a8);
XCDM_OFF(Peb64, PlaceholderCompatibilityMode, 0x7b0);
XCDM_OFF(Peb64, LeapSecondData, 0x7b8);
XCDM_OFF(Peb64, NtGlobalFlag2, 0x7c4);
static_assert(sizeof(Peb64) == 0x7c8, "PEB64 size differs from Wine x64");

XCDM_OFF(ProcessParameters64, ConsoleHandle, 0x010);
XCDM_OFF(ProcessParameters64, hStdInput, 0x020);
XCDM_OFF(ProcessParameters64, CurrentDirectory, 0x038);
XCDM_OFF(ProcessParameters64, DllPath, 0x050);
XCDM_OFF(ProcessParameters64, ImagePathName, 0x060);
XCDM_OFF(ProcessParameters64, CommandLine, 0x070);
XCDM_OFF(ProcessParameters64, Environment, 0x080);
XCDM_OFF(ProcessParameters64, wShowWindow, 0x0a8);
XCDM_OFF(ProcessParameters64, WindowTitle, 0x0b0);
XCDM_OFF(ProcessParameters64, Desktop, 0x0c0);
XCDM_OFF(ProcessParameters64, DLCurrentDirectory, 0x0f0);
XCDM_OFF(ProcessParameters64, EnvironmentSize, 0x3f0);
XCDM_OFF(ProcessParameters64, ProcessGroupId, 0x408);
static_assert(sizeof(ProcessParameters64) == 0x410, "RTL_USER_PROCESS_PARAMETERS64 size");
static_assert(sizeof(PebLdrData64) == 0x58, "PEB_LDR_DATA64 size");
static_assert(sizeof(GdiHandleEntry64) == 0x18, "GDI_HANDLE_ENTRY size");
#undef XCDM_OFF

namespace
{
    thread_local GuestThread* t_current = nullptr;

    // Thread-exit release. C++ thread_local destructors are not reliable on
    // every Windows runtime (mingw emutls, UWP), so use the OS primitive: an
    // FLS callback on Windows and a pthread key destructor on POSIX. Both run
    // on the exiting thread with the stored GuestThread*.
    void ReleaseOnThreadExit(void* value);
#if defined(_WIN32)
    void WINAPI FlsRelease(void* value) { if (value) ReleaseOnThreadExit(value); }
    DWORD ExitKey()
    {
        static const DWORD key = FlsAlloc(&FlsRelease);
        return key;
    }
    bool ArmThreadExit(GuestThread* thread)
    {
        const DWORD key = ExitKey();
        return key != FLS_OUT_OF_INDEXES && FlsSetValue(key, thread);
    }
    GuestThread* ThreadExitValue()
    {
        const DWORD key = ExitKey();
        return key == FLS_OUT_OF_INDEXES ? nullptr : static_cast<GuestThread*>(FlsGetValue(key));
    }
    bool DisarmThreadExit()
    {
        const DWORD key = ExitKey();
        return key != FLS_OUT_OF_INDEXES && FlsSetValue(key, nullptr);
    }
#else
    void KeyRelease(void* value) { if (value) ReleaseOnThreadExit(value); }
    pthread_key_t ExitKey(bool* ok)
    {
        static pthread_key_t key;
        static const bool created = pthread_key_create(&key, &KeyRelease) == 0;
        *ok = created;
        return key;
    }
    bool ArmThreadExit(GuestThread* thread)
    {
        bool ok = false;
        const pthread_key_t key = ExitKey(&ok);
        return ok && pthread_setspecific(key, thread) == 0;
    }
    GuestThread* ThreadExitValue()
    {
        bool ok = false;
        const pthread_key_t key = ExitKey(&ok);
        return ok ? static_cast<GuestThread*>(pthread_getspecific(key)) : nullptr;
    }
    bool DisarmThreadExit()
    {
        bool ok = false;
        const pthread_key_t key = ExitKey(&ok);
        return ok && pthread_setspecific(key, nullptr) == 0;
    }
#endif

    void SetError(std::string* error, const char* message)
    {
        if (error) *error = message;
    }

    uint64_t Addr(const void* p) { return reinterpret_cast<uint64_t>(p); }

    void SelfLink(ListEntry64* entry)
    {
        entry->Flink = Addr(entry);
        entry->Blink = Addr(entry);
    }

    // Bump allocator over the single process-parameters block.
    struct BlockWriter
    {
        uint8_t* base;
        size_t size;
        size_t used;

        void* Take(size_t bytes, size_t align)
        {
            used = (used + align - 1) & ~(align - 1);
            if (bytes > size - used) return nullptr;
            void* p = base + used;
            used += bytes;
            return p;
        }

        bool String(const std::u16string& text, UnicodeString64* out)
        {
            const size_t bytes = text.size() * sizeof(char16_t);
            if (bytes + sizeof(char16_t) > 0xfffe) return false;
            auto* buffer = static_cast<char16_t*>(Take(bytes + sizeof(char16_t), 2));
            if (!buffer) return false;
            if (bytes) std::memcpy(buffer, text.data(), bytes);
            buffer[text.size()] = 0;
            out->Length = static_cast<uint16_t>(bytes);
            out->MaximumLength = static_cast<uint16_t>(bytes + sizeof(char16_t));
            out->Buffer = Addr(buffer);
            return true;
        }
    };

    size_t StringBytes(const std::u16string& s) { return (s.size() + 1) * sizeof(char16_t) + 2; }
}

// --- Default host services -------------------------------------------------

namespace
{
#if defined(_WIN32)
    bool HostStack(uint64_t* base, uint64_t* limit, uint64_t* deallocation)
    {
        // Read the *host* TEB. This is bridge code, never guest code.
        const auto* tib = reinterpret_cast<const NT_TIB*>(::NtCurrentTeb());
        *base = reinterpret_cast<uint64_t>(tib->StackBase);
        *limit = reinterpret_cast<uint64_t>(tib->StackLimit);
        *deallocation = *reinterpret_cast<const uint64_t*>(
            reinterpret_cast<const uint8_t*>(tib) + offsetof(Teb64, DeallocationStack));
        return *base > *limit;
    }
    uint64_t HostThreadId() { return ::GetCurrentThreadId(); }
#else
    bool HostStack(uint64_t* base, uint64_t* limit, uint64_t* deallocation)
    {
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) != 0) return false;
        void* address = nullptr;
        size_t size = 0;
        const bool ok = pthread_attr_getstack(&attr, &address, &size) == 0 && size != 0;
        pthread_attr_destroy(&attr);
        if (!ok) return false;
        *limit = reinterpret_cast<uint64_t>(address);
        *base = *limit + size;
        *deallocation = *limit;
        return true;
    }
    uint64_t HostThreadId() { return static_cast<uint64_t>(::syscall(SYS_gettid)); }
#endif
}

HostServices xcdm::guest::DefaultHostServices() noexcept
{
    HostServices services;
    services.currentStack = &HostStack;
    services.currentThreadId = &HostThreadId;
    return services;
}

// --- Guest memory ----------------------------------------------------------

void* xcdm::guest::AllocateGuestBlock(size_t bytes) noexcept
{
    if (bytes == 0) bytes = 1;
#if defined(_WIN32)
    return VirtualAllocFromApp(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    // A 16-byte header records the mapping length for FreeGuestBlock.
    if (bytes > std::numeric_limits<size_t>::max() - 16) return nullptr;
    const size_t total = bytes + 16;
    void* p = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    *static_cast<size_t*>(p) = total;
    return static_cast<uint8_t*>(p) + 16;
#endif
}

void xcdm::guest::FreeGuestBlock(void* block) noexcept
{
    if (!block) return;
#if defined(_WIN32)
    VirtualFree(block, 0, MEM_RELEASE);
#else
    uint8_t* head = static_cast<uint8_t*>(block) - 16;
    munmap(head, *reinterpret_cast<size_t*>(head));
#endif
}

// --- GuestThread -----------------------------------------------------------

GuestThread::GuestThread(GuestProcess* process, uint64_t hostThreadId)
    : m_process(process), m_hostThreadId(hostThreadId),
      m_teb(static_cast<Teb64*>(AllocateGuestBlock(sizeof(Teb64))))
{
    if (!m_teb) return;
    Teb64& teb = *m_teb;
    teb.Tib.Self = Addr(&teb);
    teb.Peb = Addr(process->m_peb);
    teb.ClientId.UniqueProcess = process->m_config.processId;
    teb.ClientId.UniqueThread = hostThreadId;
    teb.RealClientId = teb.ClientId;
    teb.CurrentLocale = process->m_config.locale;
    teb.StaticUnicodeString.Length = 0;
    teb.StaticUnicodeString.MaximumLength = sizeof(teb.StaticUnicodeBuffer);
    teb.StaticUnicodeString.Buffer = Addr(teb.StaticUnicodeBuffer);
    SelfLink(&teb.ActivationContextStack.FrameListCache);
    teb.ActivationContextStackPointer = Addr(&teb.ActivationContextStack);
    SelfLink(&teb.TlsLinks);
    // Win32ClientInfo/Win32ThreadInfo stay zero: Wine user32/win32u own them.
    // GdiBatchCount stays zero: non-zero means "WoW64 TEB64 pointer" to Wine.
    // ThreadLocalStoragePointer stays zero: no implicit-TLS guest modules are
    // admitted (tools/audit_wine_guest.py rejects .tls directories).
    SyncStack();
}

GuestThread::~GuestThread()
{
    FreeGuestBlock(m_teb);
}

void GuestThread::SyncStack() noexcept
{
    if (!m_teb || !m_process->m_host.currentStack) return;
    uint64_t base = 0, limit = 0, deallocation = 0;
    if (!m_process->m_host.currentStack(&base, &limit, &deallocation)) return;
    m_teb->Tib.StackBase = base;
    m_teb->Tib.StackLimit = limit;
    m_teb->DeallocationStack = deallocation;
}

// --- GuestProcess ----------------------------------------------------------

GuestProcess::GuestProcess(const ProcessConfig& config, const HostServices& host)
    : m_config(config), m_host(host)
{
}

std::unique_ptr<GuestProcess> GuestProcess::Create(const ProcessConfig& config, const HostServices& host,
                                                   std::string* error)
{
    if (!host.currentStack || !host.currentThreadId)
    {
        SetError(error, "Host services for stack bounds and thread id are required.");
        return nullptr;
    }
    if (config.processId == 0 || (config.processId & 3) != 0)
    {
        SetError(error, "Guest process id must be non-zero and a multiple of four.");
        return nullptr;
    }
    if (config.imagePath.empty() || config.currentDirectory.empty() ||
        config.currentDirectory.back() != u'\\')
    {
        SetError(error, "Image path and a backslash-terminated current directory are required.");
        return nullptr;
    }
    if (config.numberOfProcessors == 0 || config.numberOfProcessors > 64)
    {
        SetError(error, "Processor count must be between 1 and 64.");
        return nullptr;
    }
    for (const auto& entry : config.environment)
    {
        if (entry.empty() || entry.find(u'=', entry[0] == u'=' ? 1 : 0) == std::u16string::npos ||
            entry.find(u'\0') != std::u16string::npos)
        {
            SetError(error, "Environment entries must be non-empty NAME=value strings.");
            return nullptr;
        }
    }
    std::unique_ptr<GuestProcess> process(new (std::nothrow) GuestProcess(config, host));
    if (!process)
    {
        SetError(error, "Out of memory.");
        return nullptr;
    }
    if (!process->Initialize(error)) return nullptr;
    return process;
}

bool GuestProcess::Initialize(std::string* error)
{
    m_peb = static_cast<Peb64*>(AllocateGuestBlock(sizeof(Peb64)));
    m_ldr = static_cast<PebLdrData64*>(AllocateGuestBlock(sizeof(PebLdrData64) + 2 * sizeof(RtlBitmap64) +
                                                          16 * sizeof(uint64_t)));
    if (!m_peb || !m_ldr)
    {
        SetError(error, "PEB allocation failed.");
        return false;
    }
    m_tlsBitmap = reinterpret_cast<RtlBitmap64*>(m_ldr + 1);
    m_tlsExpansionBitmap = m_tlsBitmap + 1;
    auto* heaps = reinterpret_cast<uint64_t*>(m_tlsExpansionBitmap + 1);

    m_ldr->Length = sizeof(PebLdrData64);
    m_ldr->Initialized = 1;
    SelfLink(&m_ldr->InLoadOrderModuleList);
    SelfLink(&m_ldr->InMemoryOrderModuleList);
    SelfLink(&m_ldr->InInitializationOrderModuleList);

    // Process parameters: one normalized block, pointers not offsets.
    size_t envChars = 1;
    for (const auto& e : m_config.environment) envChars += e.size() + 1;
    if (m_config.environment.empty()) envChars = 2;
    const std::u16string& effectiveCommandLine = m_config.commandLine.empty()
        ? m_config.imagePath : m_config.commandLine;
    m_paramBlockSize = sizeof(ProcessParameters64) + StringBytes(m_config.currentDirectory) +
        StringBytes(m_config.dllPath) + 2 * StringBytes(m_config.imagePath) +
        StringBytes(effectiveCommandLine) + StringBytes(m_config.desktop) + 3 * StringBytes(u"") +
        envChars * sizeof(char16_t) + 64;
    if (m_paramBlockSize > std::numeric_limits<uint32_t>::max())
    {
        SetError(error, "Process parameters are too large.");
        return false;
    }
    m_paramBlock = AllocateGuestBlock(m_paramBlockSize);
    if (!m_paramBlock)
    {
        SetError(error, "Process parameter allocation failed.");
        return false;
    }
    BlockWriter w{ static_cast<uint8_t*>(m_paramBlock), m_paramBlockSize, 0 };
    m_params = static_cast<ProcessParameters64*>(w.Take(sizeof(ProcessParameters64), 16));
    ProcessParameters64& p = *m_params;
    p.AllocationSize = static_cast<uint32_t>(m_paramBlockSize);
    p.Size = static_cast<uint32_t>(m_paramBlockSize);
    p.Flags = ProcessParamsFlagNormalized;
    p.wShowWindow = 10; // SW_SHOWDEFAULT
    bool ok = w.String(m_config.currentDirectory, &p.CurrentDirectory.DosPath) &&
        w.String(m_config.dllPath, &p.DllPath) &&
        w.String(m_config.imagePath, &p.ImagePathName) &&
        w.String(effectiveCommandLine, &p.CommandLine) &&
        w.String(m_config.imagePath, &p.WindowTitle) &&
        w.String(m_config.desktop, &p.Desktop) &&
        w.String(u"", &p.ShellInfo) &&
        w.String(u"", &p.RuntimeInfo);
    auto* env = static_cast<char16_t*>(w.Take(envChars * sizeof(char16_t), 16));
    if (!ok || !env)
    {
        SetError(error, "A process parameter string is too long.");
        return false;
    }
    char16_t* cursor = env;
    for (const auto& e : m_config.environment)
    {
        std::memcpy(cursor, e.data(), e.size() * sizeof(char16_t));
        cursor += e.size();
        *cursor++ = 0;
    }
    *cursor++ = 0;
    if (m_config.environment.empty()) *cursor++ = 0;
    p.Environment = Addr(env);
    p.EnvironmentSize = static_cast<uint64_t>(cursor - env) * sizeof(char16_t);

    if (m_config.allocateGdiSharedTable)
    {
        m_gdiShared = AllocateGuestBlock(GdiSharedMemorySize);
        if (!m_gdiShared)
        {
            SetError(error, "GDI shared handle table allocation failed.");
            return false;
        }
    }

    m_tlsBitmap->SizeOfBitMap = TlsMinimumAvailable;
    m_tlsBitmap->Buffer = Addr(m_peb->TlsBitmapBits);
    m_tlsExpansionBitmap->SizeOfBitMap = TlsExpansionSlotCount;
    m_tlsExpansionBitmap->Buffer = Addr(m_peb->TlsExpansionBitmapBits);
    heaps[0] = m_config.processHeap;

    Peb64& peb = *m_peb;
    peb.ImageBaseAddress = m_config.imageBase;
    peb.LdrData = Addr(m_ldr);
    peb.ProcessParameters = Addr(m_params);
    peb.ProcessHeap = m_config.processHeap;
    peb.TlsBitmap = Addr(m_tlsBitmap);
    peb.TlsExpansionBitmap = Addr(m_tlsExpansionBitmap);
    peb.AnsiCodePageData = m_config.ansiCodePageData;
    peb.OemCodePageData = m_config.oemCodePageData;
    peb.UnicodeCaseTableData = m_config.unicodeCaseTableData;
    peb.NumberOfProcessors = m_config.numberOfProcessors;
    peb.ActiveProcessAffinityMask = m_config.numberOfProcessors == 64
        ? ~0ull : ((1ull << m_config.numberOfProcessors) - 1);
    peb.CriticalSectionTimeout = -2592000ll * 10000000ll; // 30 days, NT default
    peb.NumberOfHeaps = 1;
    peb.MaximumNumberOfHeaps = 16;
    peb.ProcessHeaps = Addr(heaps);
    peb.GdiSharedHandleTable = Addr(m_gdiShared);
    peb.OSMajorVersion = m_config.osMajor;
    peb.OSMinorVersion = m_config.osMinor;
    peb.OSBuildNumber = m_config.osBuild;
    peb.OSPlatformId = 2;          // VER_PLATFORM_WIN32_NT
    peb.ImageSubSystem = 2;        // IMAGE_SUBSYSTEM_WINDOWS_GUI
    peb.ImageSubSystemMajorVersion = 6;
    peb.SessionId = m_config.sessionId;
    peb.ActiveCodePage = m_config.ansiCodePage;
    peb.OemCodePage = m_config.oemCodePage;
    peb.BeingDebugged = 0;
    SelfLink(&peb.TppWorkerpList);
    // KernelCallbackTable: written by Wine user32 DllMain.
    return true;
}

GuestProcess::~GuestProcess()
{
    {
        std::lock_guard<std::mutex> guard(m_lock);
        const uint64_t self = m_host.currentThreadId ? m_host.currentThreadId() : 0;
        for (const auto& thread : m_threads)
        {
            // Destroying a process while another host thread still runs guest
            // code would leave that thread with a dangling TEB. Fail closed.
            if (thread->HostThreadId() != self) std::abort();
            if (thread.get() == t_current)
            {
                // FLS is fiber-local, whereas the cache is thread-local. A
                // process must not free another live fiber's binding. Compare
                // owned pointers first; never dereference an unmatched cache.
                if (ThreadExitValue() != t_current || !DisarmThreadExit()) std::abort();
                t_current = nullptr;
            }
        }
        m_threads.clear();
    }
    FreeGuestBlock(m_gdiShared);
    FreeGuestBlock(m_paramBlock);
    FreeGuestBlock(m_ldr);
    FreeGuestBlock(m_peb);
}

GuestThread* GuestProcess::AttachCurrentThread(std::string* error)
{
    if (t_current)
    {
        if (ThreadExitValue() != t_current)
        {
            SetError(error, "Guest binding does not belong to the current host fiber/thread storage.");
            return nullptr;
        }
        if (&t_current->Process() == this) return t_current;
        SetError(error, "The host thread is already bound to another guest process.");
        return nullptr;
    }
    std::unique_ptr<GuestThread> thread(new (std::nothrow) GuestThread(this, m_host.currentThreadId()));
    if (!thread || !thread->Teb())
    {
        SetError(error, "Guest TEB allocation failed.");
        return nullptr;
    }
    const Teb64& teb = *thread->Teb();
    if (!teb.Tib.StackLimit || teb.Tib.StackBase <= teb.Tib.StackLimit ||
        !teb.DeallocationStack || teb.DeallocationStack > teb.Tib.StackLimit)
    {
        SetError(error, "Initial host stack bounds are unavailable or invalid.");
        return nullptr;
    }
    GuestThread* raw = thread.get();
    // Complete the potentially allocating vector insertion before publishing
    // any raw pointer to the OS exit hook. A failed insertion cannot leave an
    // armed hook pointing at the local unique_ptr's destroyed GuestThread.
    try
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_threads.push_back(std::move(thread));
    }
    catch (const std::bad_alloc&)
    {
        SetError(error, "Guest thread registration allocation failed.");
        return nullptr;
    }
    if (!ArmThreadExit(raw))
    {
        ReleaseThread(raw);
        SetError(error, "Thread-exit notification could not be registered.");
        return nullptr;
    }
    t_current = raw;
    return raw;
}

void GuestProcess::ReleaseThread(GuestThread* thread) noexcept
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_threads.erase(std::remove_if(m_threads.begin(), m_threads.end(),
        [thread](const std::unique_ptr<GuestThread>& t) { return t.get() == thread; }), m_threads.end());
}

void GuestProcess::DetachCurrentThread() noexcept
{
    GuestThread* thread = CurrentThread();
    if (!thread || &thread->Process() != this) return;
    if (!DisarmThreadExit()) std::abort();
    t_current = nullptr;
    ReleaseThread(thread);
}

size_t GuestProcess::ThreadCount() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    return m_threads.size();
}

uint64_t GuestProcess::KernelCallback(uint32_t index) const noexcept
{
    if (!m_peb || !m_peb->KernelCallbackTable || index >= KernelCallbackCount) return 0;
    return reinterpret_cast<const uint64_t*>(m_peb->KernelCallbackTable)[index];
}

namespace
{
    void ReleaseOnThreadExit(void* value)
    {
        // Invalidate the cache before freeing the object. On Windows this
        // callback can run for DeleteFiber as well as thread exit, so the host
        // thread can continue and must never observe the released binding.
        auto* thread = static_cast<GuestThread*>(value);
        if (t_current == thread) t_current = nullptr;
        thread->Process().ReleaseThread(thread);
    }
}

// --- Thread queries --------------------------------------------------------

GuestThread* xcdm::guest::CurrentThread() noexcept
{
    // Guest fibers are not supported. A switched host fiber cannot inherit a
    // cached binding whose lifetime belongs to a different FLS slot.
    return t_current && ThreadExitValue() == t_current ? t_current : nullptr;
}

bool xcdm::guest::SetGuestLastError(uint32_t error) noexcept
{
    GuestThread* thread = CurrentThread();
    if (!thread) return false;
    thread->Teb()->LastErrorValue = error;
    return true;
}

bool xcdm::guest::GetGuestLastError(uint32_t* error) noexcept
{
    GuestThread* thread = CurrentThread();
    if (!thread || !error) return false;
    *error = thread->Teb()->LastErrorValue;
    return true;
}

extern "C" void* xcdm_guest_current_teb(void) noexcept
{
    GuestThread* thread = CurrentThread();
    if (!thread) return nullptr;
    thread->SyncStack();
    return thread->Teb();
}
