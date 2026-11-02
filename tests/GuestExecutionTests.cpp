// SPDX-License-Identifier: MIT
// Portable ownership/entry tests. No guest PE is loaded or executed.
#include "runtime/GuestExecution.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#    define XCDM_EXECUTION_FIBER_TEST 1
#  endif
#endif

// Fail one C++ allocation deterministically, then allow error reporting and
// rollback to allocate normally. Guest blocks themselves use mmap/VirtualAlloc.
namespace
{
    thread_local int g_allocationsUntilFailure = -1;
    thread_local unsigned g_injectedFailures = 0;
#if defined(_MSC_VER)
    __declspec(noinline)
#else
    __attribute__((noinline))
#endif
    void ReleaseTestAllocation(void* value) noexcept { std::free(value); }
}
void* operator new(std::size_t size)
{
    if (g_allocationsUntilFailure == 0)
    {
        g_allocationsUntilFailure = -1;
        ++g_injectedFailures;
        throw std::bad_alloc();
    }
    if (g_allocationsUntilFailure > 0) --g_allocationsUntilFailure;
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { ReleaseTestAllocation(value); }
void operator delete[](void* value) noexcept { ReleaseTestAllocation(value); }
void operator delete(void* value, std::size_t) noexcept { ReleaseTestAllocation(value); }
void operator delete[](void* value, std::size_t) noexcept { ReleaseTestAllocation(value); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
    return ::operator new(size, tag);
}
void operator delete(void* value, const std::nothrow_t&) noexcept { ReleaseTestAllocation(value); }
void operator delete[](void* value, const std::nothrow_t&) noexcept { ReleaseTestAllocation(value); }

using namespace xcdm::guest;
namespace
{
    unsigned g_checks = 0, g_failures = 0;
#define CHECK(expression) do { ++g_checks; if (!(expression)) { ++g_failures; \
    std::cerr << __LINE__ << ": CHECK failed: " #expression "\n"; } } while (0)

    std::unique_ptr<GuestProcess> Process(uint32_t id = 0x20, HostServices host = DefaultHostServices())
    {
        ProcessConfig config;
        config.processId = id;
        config.imagePath = u"C:\\diagnostic.exe";
        config.currentDirectory = u"C:\\";
        config.allocateGdiSharedTable = false;
        // Heap/NLS stay zero: these tests exercise ownership only.
        std::string error;
        return GuestProcess::Create(config, host, &error);
    }
    std::shared_ptr<GuestExecutionContext> Context(uint32_t id = 0x20)
    {
        std::string error;
        return GuestExecutionContext::Create(Process(id), &error);
    }
    bool NoStack(uint64_t*, uint64_t*, uint64_t*) { return false; }

    void TestMissingInputAndFreshEntry()
    {
        std::string error;
        CHECK(!GuestExecutionContext::Create(nullptr, &error) && !error.empty());
        GuestThreadScope missing(nullptr);
        CHECK(!missing.IsActive() && !missing.Error().empty());
        CHECK(CurrentExecutionContext() == nullptr && CurrentThread() == nullptr);
        auto context = Context();
        CHECK(context && !context->IsStopping() && context->ActiveEntries() == 0);
        {
            GuestThreadScope outer(context);
            CHECK(outer.IsActive() && outer.Error().empty());
            CHECK(CurrentExecutionContext() == context.get());
            CHECK(context->ActiveEntries() == 1 && context->Process().ThreadCount() == 1);
            auto* binding = CurrentThread();
            binding->Teb()->TlsSlots[3] = 101;
            SetGuestLastError(77);
            {
                GuestThreadScope nested(context);
                CHECK(nested.IsActive() && context->ActiveEntries() == 2);
                CHECK(CurrentThread() == binding && binding->Teb()->TlsSlots[3] == 101);
                uint32_t value = 0;
                CHECK(GetGuestLastError(&value) && value == 77);
            }
            CHECK(CurrentThread() == binding && context->ActiveEntries() == 1);
        }
        CHECK(CurrentExecutionContext() == nullptr && CurrentThread() == nullptr);
        CHECK(context->ActiveEntries() == 0 && context->Process().ThreadCount() == 0);
        CHECK(context->WaitForIdle());
    }

    void TestManualAttachmentAndCrossProcess()
    {
        auto first = Context(), second = Context(0x24);
        std::string error;
        auto* manual = first->Process().AttachCurrentThread(&error);
        CHECK(manual != nullptr);
        {
            GuestThreadScope scope(first);
            CHECK(scope.IsActive() && CurrentThread() == manual);
            GuestThreadScope rejected(second);
            CHECK(!rejected.IsActive() && !rejected.Error().empty());
            CHECK(CurrentExecutionContext() == first.get() && CurrentThread() == manual);
            CHECK(first->ActiveEntries() == 1 && second->ActiveEntries() == 0);
            CHECK(second->Process().ThreadCount() == 0);
        }
        CHECK(CurrentExecutionContext() == nullptr && CurrentThread() == manual);
        CHECK(first->Process().ThreadCount() == 1 && first->ActiveEntries() == 0);
        first->Process().DetachCurrentThread();
        CHECK(CurrentThread() == nullptr);
    }

    void TestStopAndIdle()
    {
        auto context = Context();
        {
            GuestThreadScope outer(context);
            CHECK(outer.IsActive() && !context->WaitForIdle());
            context->RequestStop();
            CHECK(context->IsStopping());
            {
                GuestThreadScope nested(context);
                CHECK(nested.IsActive() && context->ActiveEntries() == 2);
            }
            std::atomic<bool> rejected{false};
            std::thread worker([context, &rejected] {
                GuestThreadScope scope(context);
                rejected = !scope.IsActive() && !scope.Error().empty() &&
                           CurrentExecutionContext() == nullptr && CurrentThread() == nullptr;
            });
            worker.join();
            CHECK(rejected.load());
            CHECK(context->ActiveEntries() == 1 && context->Process().ThreadCount() == 1);
        }
        CHECK(context->WaitForIdle() && context->ActiveEntries() == 0);
        GuestThreadScope fresh(context);
        CHECK(!fresh.IsActive() && !fresh.Error().empty());
        CHECK(CurrentThread() == nullptr && context->Process().ThreadCount() == 0);
        std::string error;
        auto* manual = context->Process().AttachCurrentThread(&error);
        {
            GuestThreadScope rejected(context);
            CHECK(!rejected.IsActive() && CurrentThread() == manual);
        }
        context->Process().DetachCurrentThread();
    }

    void TestWorkerIsolationAndDrain()
    {
        auto context = Context();
        std::mutex lock;
        std::condition_variable changed;
        bool ready = false, release = false;
        std::atomic<bool> ok{false}, waitStarted{false}, idleReturned{false};
        std::thread worker([&] {
            GuestThreadScope scope(context);
            bool local = scope.IsActive();
            if (local)
            {
                CurrentThread()->Teb()->TlsSlots[2] = 222;
                SetGuestLastError(22);
            }
            {
                std::unique_lock<std::mutex> guard(lock);
                ready = true;
                changed.notify_all();
                changed.wait(guard, [&] { return release; });
            }
            uint32_t value = 0;
            ok = local && GetGuestLastError(&value) && value == 22 &&
                 CurrentThread()->Teb()->TlsSlots[2] == 222;
        });
        {
            std::unique_lock<std::mutex> guard(lock);
            changed.wait(guard, [&] { return ready; });
        }
        {
            GuestThreadScope main(context);
            CHECK(main.IsActive() && context->ActiveEntries() == 2);
            CHECK(CurrentThread()->Teb()->TlsSlots[2] == 0);
            uint32_t value = 1;
            CHECK(GetGuestLastError(&value) && value == 0);
            CurrentThread()->Teb()->TlsSlots[2] = 111;
            SetGuestLastError(11);
        }
        context->RequestStop();
        std::thread waiter([context, &waitStarted, &idleReturned] {
            waitStarted = true;
            idleReturned = context->WaitForIdle();
        });
        while (!waitStarted) std::this_thread::yield();
        CHECK(!idleReturned.load() && context->ActiveEntries() == 1);
        {
            std::lock_guard<std::mutex> guard(lock);
            release = true;
        }
        changed.notify_all();
        worker.join();
        waiter.join();
        CHECK(ok.load() && idleReturned.load());
        CHECK(context->ActiveEntries() == 0 && context->Process().ThreadCount() == 0);
    }

    void TestLifetimeAndUnwind()
    {
        auto context = Context();
        std::weak_ptr<GuestExecutionContext> weak = context;
        {
            GuestThreadScope scope(context);
            context.reset();
            CHECK(!weak.expired() && scope.IsActive());
        }
        CHECK(weak.expired() && CurrentThread() == nullptr && CurrentExecutionContext() == nullptr);

        context = Context();
        try
        {
            GuestThreadScope outer(context);
            try
            {
                GuestThreadScope nested(context);
                throw 17;
            }
            catch (int) { CHECK(context->ActiveEntries() == 1 && CurrentExecutionContext() == context.get()); }
            throw 42;
        }
        catch (int value) { CHECK(value == 42); }
        CHECK(context->ActiveEntries() == 0 && context->Process().ThreadCount() == 0);
        CHECK(CurrentThread() == nullptr && CurrentExecutionContext() == nullptr);
    }

    void TestFailureRollback()
    {
        HostServices host = DefaultHostServices();
        host.currentStack = &NoStack;
        std::string error;
        auto context = GuestExecutionContext::Create(Process(0x20, host), &error);
        {
            GuestThreadScope failed(context);
            CHECK(!failed.IsActive() && !failed.Error().empty());
            CHECK(context->ActiveEntries() == 0 && context->Process().ThreadCount() == 0);
            CHECK(CurrentThread() == nullptr && CurrentExecutionContext() == nullptr);
        }
        for (int allocation : {0, 1})
        {
            context = Context();
            const unsigned before = g_injectedFailures;
            g_allocationsUntilFailure = allocation; // Thread allocation, then registration vector.
            GuestThreadScope failed(context);
            g_allocationsUntilFailure = -1;
            CHECK(g_injectedFailures == before + 1);
            CHECK(!failed.IsActive() && !failed.Error().empty());
            CHECK(context->ActiveEntries() == 0 && context->Process().ThreadCount() == 0);
            CHECK(CurrentThread() == nullptr && CurrentExecutionContext() == nullptr);
        }
        auto process = Process();
        g_allocationsUntilFailure = 0;
        context = GuestExecutionContext::Create(std::move(process), &error);
        g_allocationsUntilFailure = -1;
        CHECK(!context && !error.empty());
    }

    void TestWorkerRetainsLastContextOwner()
    {
        auto context = Context();
        std::weak_ptr<GuestExecutionContext> weak = context;
        std::mutex lock;
        std::condition_variable changed;
        bool ready = false, release = false;
        std::atomic<bool> active{false};
        std::thread worker([owned = context, &lock, &changed, &ready, &release, &active]() mutable {
            GuestThreadScope scope(owned);
            active = scope.IsActive();
            owned.reset(); // Only the entered scope now owns this worker's pin.
            std::unique_lock<std::mutex> guard(lock);
            ready = true;
            changed.notify_all();
            changed.wait(guard, [&] { return release; });
        });
        context.reset();
        {
            std::unique_lock<std::mutex> guard(lock);
            changed.wait(guard, [&] { return ready; });
            CHECK(active.load() && !weak.expired());
            release = true;
        }
        changed.notify_all();
        worker.join();
        CHECK(weak.expired());
        CHECK(CurrentThread() == nullptr && CurrentExecutionContext() == nullptr);
    }

#if defined(XCDM_EXECUTION_FIBER_TEST)
    struct FiberFixture { std::shared_ptr<GuestExecutionContext>* context; void* caller; bool ok = false; };
    void WINAPI TryOtherFiber(void* value)
    {
        auto* fixture = static_cast<FiberFixture*>(value);
        {
            GuestThreadScope rejected(*fixture->context);
            fixture->ok = !rejected.IsActive() && !rejected.Error().empty() &&
                          CurrentExecutionContext() == nullptr && !(*fixture->context)->WaitForIdle();
        }
        SwitchToFiber(fixture->caller);
        std::abort();
    }
    void TestFiberMismatch()
    {
        void* caller = ConvertThreadToFiber(nullptr);
        CHECK(caller != nullptr);
        if (!caller) return;
        auto context = Context();
        {
            GuestThreadScope outer(context);
            FiberFixture fixture{ &context, caller };
            void* other = CreateFiber(0, &TryOtherFiber, &fixture);
            CHECK(other != nullptr);
            if (other)
            {
                SwitchToFiber(other);
                CHECK(fixture.ok);
                CHECK(CurrentExecutionContext() == context.get() && context->ActiveEntries() == 1);
                DeleteFiber(other);
            }
        }
        CHECK(CurrentExecutionContext() == nullptr && context->Process().ThreadCount() == 0);
        CHECK(ConvertFiberToThread());
    }
#endif
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--wrong-thread-destruction") == 0)
    {
        auto context = Context();
        auto* scope = new GuestThreadScope(context);
        std::thread wrong([scope] { delete scope; });
        wrong.join();
        return 99; // Must abort before this point.
    }
    TestMissingInputAndFreshEntry();
    TestManualAttachmentAndCrossProcess();
    TestStopAndIdle();
    TestWorkerIsolationAndDrain();
    TestLifetimeAndUnwind();
    TestFailureRollback();
    TestWorkerRetainsLastContextOwner();
#if defined(XCDM_EXECUTION_FIBER_TEST)
    TestFiberMismatch();
#endif
    std::cout << "guest execution: " << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures ? 1 : 0;
}
