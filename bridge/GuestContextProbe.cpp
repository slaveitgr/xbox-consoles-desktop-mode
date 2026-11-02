// SPDX-License-Identifier: MIT
// Native bridge integration checks. No guest PE entry, TLS callback or Wine
// DLL is executed. The only PE supplied is our return42 fixture, whose strict
// Run must stop at the unconditional execution gate.
#include "Bridge/GuestModule.h"
#include "Bridge/GuestRuntime.h"
#include "Bridge/Kernel32Shims.h"
#include "Bridge/Win32Shims.h"
#include "bridge/GuestContextShims.h"
#include "runtime/GuestExecution.h"
#include "samples/Return42Fixture.h"

#include <Windows.h>
#include <atomic>
#include <cstring>
#include <future>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace Win32Bridge::Bridge;
using namespace xcdm::guest;
using TebEntry = void*(WINAPI*)();

namespace
{
    // Scalar-only SEH leaf: keep C++ object lifetimes in its caller.
    bool MissingTebFaults(TebEntry entry)
    {
        __try { entry(); return false; }
        __except (GetExceptionCode() == 0xc0000005u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        { return true; }
    }

    struct Checks
    {
        unsigned count = 0, failed = 0, firstFailed = 0;
        void Check(bool ok) { ++count; if (!ok) { ++failed; if (!firstFailed) firstFailed = count; } }
    };

    std::shared_ptr<GuestExecutionContext> Context(uint32_t id)
    {
        ProcessConfig config;
        config.processId = id;
        config.imagePath = u"C:\\diagnostics\\context-probe.exe";
        config.currentDirectory = u"C:\\diagnostics\\";
        // No heap/NLS provider is fabricated by this scope-only diagnostic.
        config.allocateGdiSharedTable = false;
        std::string error;
        auto process = GuestProcess::Create(config, DefaultHostServices(), &error);
        if (!process) throw std::runtime_error(error);
        auto context = GuestExecutionContext::Create(std::move(process), &error);
        if (!context) throw std::runtime_error(error);
        return context;
    }

    std::shared_ptr<GuestModuleLoader> Loader(const std::shared_ptr<GuestExecutionContext>& context)
    {
        return std::make_shared<GuestModuleLoader>(nullptr, &ResolveRuntimeImport, nullptr,
            xcdm::runtime::ModuleOwnership(xcdm::runtime::ModuleProfile::StrictWine), context);
    }

    std::atomic<unsigned> unexpectedEntries{ 0 };
    DWORD WINAPI MustNotRun(LPVOID) { ++unexpectedEntries; return 0; }

    std::string Probe()
    {
        Checks checks;
        checks.Check(CurrentGuestModuleLoader() == nullptr && CurrentExecutionContext() == nullptr);
        auto context = Context(0x100);
        auto loader = Loader(context);
        ImportedSymbol tebSymbol{};
        tebSymbol.library = L"ntdll.dll";
        tebSymbol.name = L"NtCurrentTeb";
        const auto resolved = loader->ResolveImport(tebSymbol);
        checks.Check(resolved.targetAddress != 0);
        auto entry = reinterpret_cast<TebEntry>(resolved.targetAddress);
        if (!entry) throw std::runtime_error("The strict TEB import was not resolved.");
        checks.Check(MissingTebFaults(entry));
        {
            // A raw manual binding still must not bypass owned entry scopes.
            std::string error;
            checks.Check(context->Process().AttachCurrentThread(&error) != nullptr);
            checks.Check(MissingTebFaults(entry));
            context->Process().DetachCurrentThread();
        }
        auto other = Context(0x104);
        auto otherLoader = Loader(other);
        {
            GuestModuleScope outer(loader.get());
            auto* teb = static_cast<Teb64*>(entry());
            checks.Check(teb && teb == xcdm_guest_current_teb());
            checks.Check(teb && teb->Peb == reinterpret_cast<uint64_t>(context->Process().Peb()));
            checks.Check(CurrentGuestModuleLoader() == loader.get() && context->ActiveEntries() == 1);
            const auto* hostTeb = reinterpret_cast<const BYTE*>(::NtCurrentTeb());
            checks.Check(reinterpret_cast<const void*>(teb) != hostTeb);
            const auto hostPeb = *reinterpret_cast<void* const*>(hostTeb + 0x60);
            BridgeSetLastError(0x12345678);
            checks.Check(teb->LastErrorValue == 0x12345678 && BridgeGetLastError() == 0x12345678);
            checks.Check(*reinterpret_cast<void* const*>(hostTeb + 0x60) == hostPeb);
            {
                GuestModuleScope nested(loader.get());
                checks.Check(entry() == teb && context->ActiveEntries() == 2);
                BridgeSetLastError(0x98765432);
            }
            checks.Check(entry() == teb && context->ActiveEntries() == 1);
            checks.Check(BridgeGetLastError() == 0x98765432);
            try { GuestModuleScope wrong(otherLoader.get()); checks.Check(false); }
            catch (const std::runtime_error&) { checks.Check(true); }
            try { GuestModuleScope downgrade(nullptr); checks.Check(false); }
            catch (const std::runtime_error&) { checks.Check(true); }
            checks.Check(CurrentGuestModuleLoader() == loader.get() && entry() == teb);
            const auto ntdll = BridgeGetModuleHandleW(L"ntdll.dll");
            const auto dynamic = ntdll ? BridgeGetProcAddress(ntdll, "NtCurrentTeb") : nullptr;
            checks.Check(dynamic && reinterpret_cast<TebEntry>(dynamic)() == teb);
            ImportedSymbol pebSymbol{};
            pebSymbol.library = L"ntdll.dll"; pebSymbol.name = L"RtlGetCurrentPeb";
            const auto peb = loader->ResolveImport(pebSymbol);
            checks.Check(peb.targetAddress && reinterpret_cast<TebEntry>(peb.targetAddress)() == context->Process().Peb());
        }
        checks.Check(context->Process().ThreadCount() == 0 && context->ActiveEntries() == 0);
        checks.Check(CurrentGuestModuleLoader() == nullptr && xcdm_guest_current_teb() == nullptr);
        checks.Check(MissingTebFaults(entry));
        {
            auto missing = Loader({});
            try { GuestModuleScope wrong(missing.get()); checks.Check(false); }
            catch (const std::runtime_error&) { checks.Check(true); }
            checks.Check(CurrentGuestModuleLoader() == nullptr);
        }
        {
            // The real worker uses the same bridge scope, not a mock router.
            // Join it so diagnostics cannot leave an orphan callback behind.
            std::promise<void> ready, release;
            auto released = release.get_future();
            std::atomic<bool> workerPassed{ false };
            std::weak_ptr<GuestModuleLoader> weak = loader;
            auto workerOwner = loader;
            std::thread worker([workerOwner, &ready, &released, &workerPassed, entry]()
            {
                try
                {
                    GuestModuleScope scope(workerOwner.get());
                    auto* teb = static_cast<Teb64*>(entry());
                    const bool active = teb && CurrentGuestModuleLoader() == workerOwner.get();
                    BridgeSetLastError(0x55aa55aa);
                    ready.set_value();
                    released.wait();
                    workerPassed = active && entry() == teb && BridgeGetLastError() == 0x55aa55aa;
                }
                catch (...) { try { ready.set_value(); } catch (...) {} }
            });
            ready.get_future().wait();
            workerOwner.reset(); loader.reset();
            checks.Check(!weak.expired() && context->ActiveEntries() == 1);
            release.set_value(); worker.join();
            checks.Check(workerPassed && weak.expired());
            checks.Check(context->Process().ThreadCount() == 0 && context->ActiveEntries() == 0);
            loader = Loader(context);
        }
        {
            const auto& image = xcdm::samples::Return42PeBytes();
            GuestRuntime runtime(xcdm::runtime::ModuleOwnership(xcdm::runtime::ModuleProfile::StrictWine), context);
            std::wstring error;
            const bool prepared = runtime.Prepare(image.data(), image.size(), &ResolveRuntimeImport, &error);
            checks.Check(prepared);
            int exitCode = -1;
            checks.Check(prepared && !runtime.Run(&exitCode, &error) && exitCode == -1 &&
                error.find(L"native providers and callback/image lifetime") != std::wstring::npos);
            checks.Check(CurrentGuestModuleLoader() == nullptr && context->Process().ThreadCount() == 0);
            DWORD threadId = 99, winError = 0;
            checks.Check(runtime.LaunchThread(0, &MustNotRun, nullptr, 0, &threadId, &winError) == nullptr &&
                threadId == 0 && winError == ERROR_NOT_SUPPORTED);
            PROCESS_INFORMATION child{};
            checks.Check(!runtime.LaunchChildProcess(L"probe.exe", nullptr, nullptr, &child, &winError) &&
                winError == ERROR_NOT_SUPPORTED && unexpectedEntries == 0);
        }
        context->RequestStop();
        try { GuestModuleScope stopped(loader.get()); checks.Check(false); }
        catch (const std::runtime_error&) { checks.Check(true); }
        checks.Check(context->WaitForIdle() && context->Process().ThreadCount() == 0);
        std::ostringstream report;
        report << "{\"passed\":" << (checks.failed ? "false" : "true")
            << ",\"checks\":" << checks.count << ",\"failures\":" << checks.failed
            << ",\"firstFailedCheck\":" << checks.firstFailed
            << ",\"wineExecuted\":false,\"guestPeExecuted\":false"
            << ",\"scopeBackend\":\"actual packaged GuestModuleScope\""
            << ",\"heapNlsQualified\":false,\"asyncImageLifetimeQualified\":false}";
        return report.str();
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI XcdmBridgeProbeGuestContext(char* output, DWORD capacity) noexcept
{
    if (!output || capacity < 2 || capacity > 65536) return 2;
    output[0] = 0;
    try
    {
        const auto report = Probe();
        if (report.size() >= capacity) return 2;
        std::memcpy(output, report.c_str(), report.size() + 1);
        return report.find("\"passed\":true") != std::string::npos ? 0 : 1;
    }
    catch (...)
    {
        constexpr char failure[] = "{\"passed\":false,\"unexpectedException\":true,\"wineExecuted\":false,\"guestPeExecuted\":false}";
        if (capacity >= sizeof(failure)) std::memcpy(output, failure, sizeof(failure));
        return 3;
    }
}
