// Guest TEB/PEB environment for stock Wine PE DLLs running on Win32Bridge.
//
// Wine's PE DLLs read and write TEB/PEB fields directly (inline GetLastError,
// GetProcessHeap, NtUserGetThreadInfo, gdi32's handle table, user32's kernel
// callback table, ole32/rpcrt4 per-thread data, kernelbase TLS/FLS/stack).
// Built with the XCDM_GUEST winnt.h patch, every one of those accesses goes
// through ntdll!NtCurrentTeb, which the bridge's ntdll seam implements with
// xcdm_guest_current_teb(). The real Xbox OS TEB/PEB is never handed to guest
// code and is never written by it.
//
// Layout: Wine x64 TEB/PEB/RTL_USER_PROCESS_PARAMETERS as pinned in
// ValveSoftware/wine 1c78596625336a73dbfc035dbbcf0dbdd95b06eb. Every named
// offset is a static_assert below and tools/verify_guest_layout.py checks the
// same offsets against that header's own offset comments.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xcdm::guest
{
    static_assert(sizeof(void*) == 8, "The guest environment models the x64 TEB/PEB only.");

    struct ListEntry64 { uint64_t Flink; uint64_t Blink; };
    struct UnicodeString64 { uint16_t Length; uint16_t MaximumLength; uint32_t Pad; uint64_t Buffer; };
    struct ClientId64 { uint64_t UniqueProcess; uint64_t UniqueThread; };
    struct Guid { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; };
    struct RtlBitmap64 { uint32_t SizeOfBitMap; uint32_t Pad; uint64_t Buffer; };

    struct NtTib64
    {
        uint64_t ExceptionList;        // 0x00
        uint64_t StackBase;            // 0x08
        uint64_t StackLimit;           // 0x10
        uint64_t SubSystemTib;         // 0x18
        uint64_t FiberData;            // 0x20
        uint64_t ArbitraryUserPointer; // 0x28
        uint64_t Self;                 // 0x30
    };

    struct ActivationContextStack64
    {
        uint64_t ActiveFrame;
        ListEntry64 FrameListCache;
        uint32_t Flags;
        uint32_t NextCookieSequenceNumber;
        uint64_t StackId;
    };

    struct GdiTebBatch64
    {
        uint32_t Offset;
        uint32_t Pad;
        uint64_t HDC;
        uint32_t Buffer[0x136];
    };

    struct Teb64
    {
        NtTib64 Tib;                                   // 0x0000
        uint64_t EnvironmentPointer;                   // 0x0038
        ClientId64 ClientId;                           // 0x0040
        uint64_t ActiveRpcHandle;                      // 0x0050
        uint64_t ThreadLocalStoragePointer;            // 0x0058
        uint64_t Peb;                                  // 0x0060
        uint32_t LastErrorValue;                       // 0x0068
        uint32_t CountOfOwnedCriticalSections;         // 0x006c
        uint64_t CsrClientThread;                      // 0x0070
        uint64_t Win32ThreadInfo;                      // 0x0078
        uint32_t User32Reserved[26];                   // 0x0080
        uint32_t UserReserved[5];                      // 0x00e8
        uint32_t Pad0;
        uint64_t WOW32Reserved;                        // 0x0100
        uint32_t CurrentLocale;                        // 0x0108
        uint32_t FpSoftwareStatusRegister;             // 0x010c
        uint64_t ReservedForDebuggerInstrumentation[16]; // 0x0110
        uint64_t SystemReserved1[30];                  // 0x0190
        int8_t PlaceholderCompatibilityMode;           // 0x0280
        uint8_t PlaceholderHydrationAlwaysExplicit;    // 0x0281
        int8_t PlaceholderReserved[10];                // 0x0282
        uint32_t ProxiedProcessId;                     // 0x028c
        ActivationContextStack64 ActivationContextStack; // 0x0290
        uint8_t WorkingOnBehalfOfTicket[8];            // 0x02b8
        int32_t ExceptionCode;                         // 0x02c0
        uint32_t Pad1;
        uint64_t ActivationContextStackPointer;        // 0x02c8
        uint64_t InstrumentationCallbackSp;            // 0x02d0
        uint64_t InstrumentationCallbackPreviousPc;    // 0x02d8
        uint64_t InstrumentationCallbackPreviousSp;    // 0x02e0
        uint32_t TxFsContext;                          // 0x02e8
        uint8_t InstrumentationCallbackDisabled;       // 0x02ec
        uint8_t UnalignedLoadStoreExceptions;          // 0x02ed
        uint16_t Pad2;
        GdiTebBatch64 GdiTebBatch;                     // 0x02f0
        ClientId64 RealClientId;                       // 0x07d8
        uint64_t GdiCachedProcessHandle;               // 0x07e8
        uint32_t GdiClientPID;                         // 0x07f0
        uint32_t GdiClientTID;                         // 0x07f4
        uint64_t GdiThreadLocaleInfo;                  // 0x07f8
        uint64_t Win32ClientInfo[62];                  // 0x0800
        uint64_t glDispatchTable[233];                 // 0x09f0
        uint64_t glReserved1[29];                      // 0x1138
        uint64_t glReserved2;                          // 0x1220
        uint64_t glSectionInfo;                        // 0x1228
        uint64_t glSection;                            // 0x1230
        uint64_t glTable;                              // 0x1238
        uint64_t glCurrentRC;                          // 0x1240
        uint64_t glContext;                            // 0x1248
        uint32_t LastStatusValue;                      // 0x1250
        uint32_t Pad3;
        UnicodeString64 StaticUnicodeString;           // 0x1258
        uint16_t StaticUnicodeBuffer[261];             // 0x1268
        uint16_t Pad4[3];
        uint64_t DeallocationStack;                    // 0x1478
        uint64_t TlsSlots[64];                         // 0x1480
        ListEntry64 TlsLinks;                          // 0x1680
        uint64_t Vdm;                                  // 0x1690
        uint64_t ReservedForNtRpc;                     // 0x1698
        uint64_t DbgSsReserved[2];                     // 0x16a0
        uint32_t HardErrorMode;                        // 0x16b0
        uint32_t Pad5;
        uint64_t Instrumentation[11];                  // 0x16b8
        Guid ActivityId;                               // 0x1710
        uint64_t SubProcessTag;                        // 0x1720
        uint64_t PerflibData;                          // 0x1728
        uint64_t EtwTraceData;                         // 0x1730
        uint64_t WinSockData;                          // 0x1738
        uint32_t GdiBatchCount;                        // 0x1740
        uint32_t IdealProcessorValue;                  // 0x1744
        uint32_t GuaranteedStackBytes;                 // 0x1748
        uint32_t Pad6;
        uint64_t ReservedForPerf;                      // 0x1750
        uint64_t ReservedForOle;                       // 0x1758
        uint32_t WaitingOnLoaderLock;                  // 0x1760
        uint32_t Pad7;
        uint64_t SavedPriorityState;                   // 0x1768
        uint64_t ReservedForCodeCoverage;              // 0x1770
        uint64_t ThreadPoolData;                       // 0x1778
        uint64_t TlsExpansionSlots;                    // 0x1780
        uint64_t DeallocationBStore;                   // 0x1788
        uint64_t BStoreLimit;                          // 0x1790
        uint32_t MuiGeneration;                        // 0x1798
        uint32_t IsImpersonating;                      // 0x179c
        uint64_t NlsCache;                             // 0x17a0
        uint64_t ShimData;                             // 0x17a8
        uint32_t HeapVirtualAffinity;                  // 0x17b0
        uint32_t Pad8;
        uint64_t CurrentTransactionHandle;             // 0x17b8
        uint64_t ActiveFrame;                          // 0x17c0
        uint64_t FlsSlots;                             // 0x17c8
        uint64_t PreferredLanguages;                   // 0x17d0
        uint64_t UserPrefLanguages;                    // 0x17d8
        uint64_t MergedPrefLanguages;                  // 0x17e0
        uint32_t MuiImpersonation;                     // 0x17e8
        uint16_t CrossTebFlags;                        // 0x17ec
        uint16_t SameTebFlags;                         // 0x17ee
        uint64_t TxnScopeEnterCallback;                // 0x17f0
        uint64_t TxnScopeExitCallback;                 // 0x17f8
        uint64_t TxnScopeContext;                      // 0x1800
        uint32_t LockCount;                            // 0x1808
        int32_t WowTebOffset;                          // 0x180c
        uint64_t ResourceRetValue;                     // 0x1810
        uint64_t ReservedForWdf;                       // 0x1818
        uint64_t ReservedForCrt;                       // 0x1820
        Guid EffectiveContainerId;                     // 0x1828
    };

    struct Peb64
    {
        uint8_t InheritedAddressSpace;                 // 0x000
        uint8_t ReadImageFileExecOptions;              // 0x001
        uint8_t BeingDebugged;                         // 0x002
        uint8_t BitField;                              // 0x003
        uint32_t Pad0;
        uint64_t Mutant;                               // 0x008
        uint64_t ImageBaseAddress;                     // 0x010
        uint64_t LdrData;                              // 0x018
        uint64_t ProcessParameters;                    // 0x020
        uint64_t SubSystemData;                        // 0x028
        uint64_t ProcessHeap;                          // 0x030
        uint64_t FastPebLock;                          // 0x038
        uint64_t AtlThunkSListPtr;                     // 0x040
        uint64_t IFEOKey;                              // 0x048
        uint32_t CrossProcessFlags;                    // 0x050
        uint32_t Pad1;
        uint64_t KernelCallbackTable;                  // 0x058
        uint32_t Reserved;                             // 0x060
        uint32_t AtlThunkSListPtr32;                   // 0x064
        uint64_t ApiSetMap;                            // 0x068
        uint32_t TlsExpansionCounter;                  // 0x070
        uint32_t Pad2;
        uint64_t TlsBitmap;                            // 0x078
        uint32_t TlsBitmapBits[2];                     // 0x080
        uint64_t ReadOnlySharedMemoryBase;             // 0x088
        uint64_t SharedData;                           // 0x090
        uint64_t ReadOnlyStaticServerData;             // 0x098
        uint64_t AnsiCodePageData;                     // 0x0a0
        uint64_t OemCodePageData;                      // 0x0a8
        uint64_t UnicodeCaseTableData;                 // 0x0b0
        uint32_t NumberOfProcessors;                   // 0x0b8
        uint32_t NtGlobalFlag;                         // 0x0bc
        int64_t CriticalSectionTimeout;                // 0x0c0
        uint64_t HeapSegmentReserve;                   // 0x0c8
        uint64_t HeapSegmentCommit;                    // 0x0d0
        uint64_t HeapDeCommitTotalFreeThreshold;       // 0x0d8
        uint64_t HeapDeCommitFreeBlockThreshold;       // 0x0e0
        uint32_t NumberOfHeaps;                        // 0x0e8
        uint32_t MaximumNumberOfHeaps;                 // 0x0ec
        uint64_t ProcessHeaps;                         // 0x0f0
        uint64_t GdiSharedHandleTable;                 // 0x0f8
        uint64_t ProcessStarterHelper;                 // 0x100
        uint64_t GdiDCAttributeList;                   // 0x108
        uint64_t LoaderLock;                           // 0x110
        uint32_t OSMajorVersion;                       // 0x118
        uint32_t OSMinorVersion;                       // 0x11c
        uint32_t OSBuildNumber;                        // 0x120
        uint32_t OSPlatformId;                         // 0x124
        uint32_t ImageSubSystem;                       // 0x128
        uint32_t ImageSubSystemMajorVersion;           // 0x12c
        uint32_t ImageSubSystemMinorVersion;           // 0x130
        uint32_t Pad3;
        uint64_t ActiveProcessAffinityMask;            // 0x138
        uint32_t GdiHandleBuffer[60];                  // 0x140
        uint64_t PostProcessInitRoutine;               // 0x230
        uint64_t TlsExpansionBitmap;                   // 0x238
        uint32_t TlsExpansionBitmapBits[32];           // 0x240
        uint32_t SessionId;                            // 0x2c0
        uint32_t Pad4;
        uint64_t AppCompatFlags;                       // 0x2c8
        uint64_t AppCompatFlagsUser;                   // 0x2d0
        uint64_t ShimData;                             // 0x2d8
        uint64_t AppCompatInfo;                        // 0x2e0
        UnicodeString64 CSDVersion;                    // 0x2e8
        uint64_t ActivationContextData;                // 0x2f8
        uint64_t ProcessAssemblyStorageMap;            // 0x300
        uint64_t SystemDefaultActivationData;          // 0x308
        uint64_t SystemAssemblyStorageMap;             // 0x310
        uint64_t MinimumStackCommit;                   // 0x318
        uint64_t SparePointers[2];                     // 0x320
        uint64_t PatchLoaderData;                      // 0x330
        uint64_t ChpeV2ProcessInfo;                    // 0x338
        uint32_t AppModelFeatureState;                 // 0x340
        uint32_t SpareUlongs[2];                       // 0x344
        uint16_t ActiveCodePage;                       // 0x34c
        uint16_t OemCodePage;                          // 0x34e
        uint16_t UseCaseMapping;                       // 0x350
        uint16_t UnusedNlsField;                       // 0x352
        uint32_t Pad5;
        uint64_t WerRegistrationData;                  // 0x358
        uint64_t WerShipAssertPtr;                     // 0x360
        uint64_t EcCodeBitMap;                         // 0x368
        uint64_t pImageHeaderHash;                     // 0x370
        uint32_t TracingFlags;                         // 0x378
        uint32_t Pad6;
        uint64_t CsrServerReadOnlySharedMemoryBase;    // 0x380
        uint32_t TppWorkerpListLock;                   // 0x388
        uint32_t Pad7;
        ListEntry64 TppWorkerpList;                    // 0x390
        uint64_t WaitOnAddressHashTable[0x80];         // 0x3a0
        uint64_t TelemetryCoverageHeader;              // 0x7a0
        uint32_t CloudFileFlags;                       // 0x7a8
        uint32_t CloudFileDiagFlags;                   // 0x7ac
        int8_t PlaceholderCompatibilityMode;           // 0x7b0
        int8_t PlaceholderCompatibilityModeReserved[7]; // 0x7b1
        uint64_t LeapSecondData;                       // 0x7b8
        uint32_t LeapSecondFlags;                      // 0x7c0
        uint32_t NtGlobalFlag2;                        // 0x7c4
    };

    struct CurDir64 { UnicodeString64 DosPath; uint64_t Handle; };
    struct DriveLetterCurDir64 { uint16_t Flags; uint16_t Length; uint32_t TimeStamp; UnicodeString64 DosPath; };

    struct ProcessParameters64
    {
        uint32_t AllocationSize;                       // 0x000
        uint32_t Size;                                 // 0x004
        uint32_t Flags;                                // 0x008
        uint32_t DebugFlags;                           // 0x00c
        uint64_t ConsoleHandle;                        // 0x010
        uint32_t ConsoleFlags;                         // 0x018
        uint32_t Pad0;
        uint64_t hStdInput;                            // 0x020
        uint64_t hStdOutput;                           // 0x028
        uint64_t hStdError;                            // 0x030
        CurDir64 CurrentDirectory;                     // 0x038
        UnicodeString64 DllPath;                       // 0x050
        UnicodeString64 ImagePathName;                 // 0x060
        UnicodeString64 CommandLine;                   // 0x070
        uint64_t Environment;                          // 0x080
        uint32_t dwX, dwY, dwXSize, dwYSize;           // 0x088
        uint32_t dwXCountChars, dwYCountChars;         // 0x098
        uint32_t dwFillAttribute, dwFlags;             // 0x0a0
        uint32_t wShowWindow;                          // 0x0a8
        uint32_t Pad1;
        UnicodeString64 WindowTitle;                   // 0x0b0
        UnicodeString64 Desktop;                       // 0x0c0
        UnicodeString64 ShellInfo;                     // 0x0d0
        UnicodeString64 RuntimeInfo;                   // 0x0e0
        DriveLetterCurDir64 DLCurrentDirectory[0x20];  // 0x0f0
        uint64_t EnvironmentSize;                      // 0x3f0
        uint64_t EnvironmentVersion;                   // 0x3f8
        uint64_t PackageDependencyData;                // 0x400
        uint32_t ProcessGroupId;                       // 0x408
        uint32_t LoaderThreads;                        // 0x40c
    };

    struct PebLdrData64
    {
        uint32_t Length;
        uint8_t Initialized;
        uint8_t Pad0[3];
        uint64_t SsHandle;
        ListEntry64 InLoadOrderModuleList;
        ListEntry64 InMemoryOrderModuleList;
        ListEntry64 InInitializationOrderModuleList;
        uint64_t EntryInProgress;
        uint8_t ShutdownInProgress;
        uint8_t Pad1[7];
        uint64_t ShutdownThreadId;
    };

    // GDI_HANDLE_ENTRY / GDI_SHARED_MEMORY from Wine include/ntgdi.h.
    struct GdiHandleEntry64
    {
        uint64_t Object;
        uint32_t Owner;
        uint16_t Unique;
        uint8_t Type;
        uint8_t Flags;
        uint64_t UserPointer;
    };
    constexpr size_t GdiMaxHandleCount = 0x10000;
    constexpr size_t GdiSharedMemorySize = GdiMaxHandleCount * sizeof(GdiHandleEntry64);

    constexpr uint32_t ProcessParamsFlagNormalized = 0x00000001;
    constexpr uint32_t TlsMinimumAvailable = 64;
    constexpr uint32_t TlsExpansionSlotCount = 1024;
    constexpr uint64_t ConsoleHandleShellNoWindow = static_cast<uint64_t>(-6);

    // Host services the environment needs. All are required. Tests supply
    // portable implementations; the bridge supplies UWP-allowed ones.
    struct HostServices
    {
        // Current host thread stack: [limit, base) of committed stack and the
        // reservation start (DeallocationStack).
        bool (*currentStack)(uint64_t* stackBase, uint64_t* stackLimit, uint64_t* deallocationStack) = nullptr;
        uint64_t (*currentThreadId)() = nullptr;
    };

    // Windows: reads the host TEB's NT_TIB/DeallocationStack and
    // GetCurrentThreadId (UWP-allowed). POSIX (host tests): pthread stack.
    HostServices DefaultHostServices() noexcept;

    struct ProcessConfig
    {
        uint32_t processId = 0;            // Virtual guest PID, not the host PID.
        uint64_t imageBase = 0;            // Guest EXE image base (PEB.ImageBaseAddress).
        uint64_t processHeap = 0;          // Guest-visible heap handle owned by the ntdll seam.
        std::u16string imagePath;          // e.g. u"C:\\windows\\explorer.exe"
        std::u16string commandLine;
        std::u16string currentDirectory;   // Must end with a backslash.
        std::u16string dllPath;
        std::u16string desktop = u"WinSta0\\Default";
        std::vector<std::u16string> environment; // "NAME=value" entries.
        uint32_t numberOfProcessors = 1;
        uint32_t osMajor = 10, osMinor = 0, osBuild = 19045;
        uint32_t sessionId = 1;
        uint16_t ansiCodePage = 1252, oemCodePage = 437;
        uint32_t locale = 0x0409;
        bool allocateGdiSharedTable = true;
        // NLS tables mapped by the ntdll seam (C_<cp>.NLS / l_intl.nls data).
        // Wine kernelbase initializes its code-page tables from these PEB
        // fields, so the seam must supply them before kernelbase attaches.
        uint64_t ansiCodePageData = 0;
        uint64_t oemCodePageData = 0;
        uint64_t unicodeCaseTableData = 0;
    };

    // Wine ntuser.h: NtUserCallCount. KernelCallbackTable indices are below it.
    constexpr uint32_t KernelCallbackCount = 256;

    class GuestProcess;

    // One guest TEB per (host thread, guest process). Owned by its process.
    class GuestThread final
    {
    public:
        Teb64* Teb() noexcept { return m_teb; }
        const Teb64* Teb() const noexcept { return m_teb; }
        GuestProcess& Process() noexcept { return *m_process; }
        uint64_t HostThreadId() const noexcept { return m_hostThreadId; }

        // Copies the live host stack bounds into Tib/DeallocationStack.
        // Called on every NtCurrentTeb redirect so guard-page growth is seen.
        void SyncStack() noexcept;

        GuestThread(const GuestThread&) = delete;
        GuestThread& operator=(const GuestThread&) = delete;
        ~GuestThread();

    private:
        friend class GuestProcess;
        GuestThread(GuestProcess* process, uint64_t hostThreadId);
        GuestProcess* m_process;
        uint64_t m_hostThreadId;
        Teb64* m_teb;
    };

    class GuestProcess final
    {
    public:
        static std::unique_ptr<GuestProcess> Create(const ProcessConfig& config, const HostServices& host,
                                                    std::string* error);
        ~GuestProcess();
        GuestProcess(const GuestProcess&) = delete;
        GuestProcess& operator=(const GuestProcess&) = delete;

        Peb64* Peb() noexcept { return m_peb; }
        ProcessParameters64* Parameters() noexcept { return m_params; }
        void* GdiSharedTable() noexcept { return m_gdiShared; }
        uint32_t ProcessId() const noexcept { return m_config.processId; }

        // Binds the calling host thread to this process and returns its TEB,
        // creating it on first use. A host thread belongs to at most one
        // guest process; binding to a second one fails.
        GuestThread* AttachCurrentThread(std::string* error);
        // Destroys the calling thread's TEB (DLL_THREAD_DETACH time).
        void DetachCurrentThread() noexcept;
        size_t ThreadCount() const;

        // KernelCallbackTable written by Wine user32's DllMain. The win32u
        // seam dispatches NtUser callbacks through this, never through the
        // host PEB.
        uint64_t KernelCallback(uint32_t index) const noexcept;

    private:
        GuestProcess(const ProcessConfig& config, const HostServices& host);
        bool Initialize(std::string* error);
        friend class GuestThread;
    public:
        // Releases a TEB from the thread-exit callback. Not for general use.
        void ReleaseThread(GuestThread* thread) noexcept;
    private:

        ProcessConfig m_config;
        HostServices m_host;
        Peb64* m_peb = nullptr;
        ProcessParameters64* m_params = nullptr;
        PebLdrData64* m_ldr = nullptr;
        RtlBitmap64* m_tlsBitmap = nullptr;
        RtlBitmap64* m_tlsExpansionBitmap = nullptr;
        void* m_paramBlock = nullptr;
        size_t m_paramBlockSize = 0;
        void* m_gdiShared = nullptr;
        mutable std::mutex m_lock;
        std::vector<std::unique_ptr<GuestThread>> m_threads;
    };

    // The guest thread bound to the calling host thread, or nullptr.
    GuestThread* CurrentThread() noexcept;

    // Last-error discipline for bridge code that serves Wine-profile guests:
    // Wine reads LastErrorValue straight from the guest TEB (inline
    // GetLastError in winbase.h under __WINESRC__). Seam functions must set
    // it here, never with the host ::SetLastError.
    bool SetGuestLastError(uint32_t error) noexcept;
    bool GetGuestLastError(uint32_t* error) noexcept;

    // Raw guest allocations used for guest-visible blocks. Zeroed, 16-byte
    // aligned, readable/writable, never executable.
    void* AllocateGuestBlock(size_t bytes) noexcept;
    void FreeGuestBlock(void* block) noexcept;
}

// ntdll seam export target: returns the guest TEB of the calling thread, or
// nullptr if the thread is not attached to a guest process. The bridge's
// ntdll!NtCurrentTeb must call this and must fault (not fall back to the host
// TEB) on nullptr.
extern "C" void* xcdm_guest_current_teb(void) noexcept;
