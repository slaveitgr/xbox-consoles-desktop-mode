// SPDX-License-Identifier: GPL-2.0-only
#include "backend/uwp/UwpGuestWorkers.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <system_error>

using namespace protondesktop::uwp;
namespace
{
    unsigned checks = 0;
    void Require(bool condition, const char* message)
    { ++checks; if (!condition) throw std::runtime_error(message); }
    template<class Predicate> void Wait(Predicate condition)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!condition())
        {
            if (std::chrono::steady_clock::now() >= until) throw std::runtime_error("worker fixture deadline");
            std::this_thread::yield();
        }
    }
    void LifecycleChecks()
    {
        GuestWorkerRegistry workers;
        std::string error;
        Require(!workers.Managed() && !workers.Snapshot().accepting, "registry starts closed");
        Require(workers.Begin(error), "begin owned worker session");
        Require(!workers.Begin(error), "nested session rejected");
        Require(!workers.Join(error), "join requires admission closed");
        Require(!workers.End(error), "end requires admission closed and barrier");
        std::atomic<unsigned> ran{0};
        Require(workers.Spawn([&] { ++ran; }, 41, std::make_shared<int>(1), error), "own a real first phase thread");
        Wait([&] { return workers.Snapshot().finished == 1; });
        Require(workers.ProcessActive(41), "returned worker remains active until real join");
        Require(!workers.ProcessActive(42), "process tracking is isolated by pid");
        Require(workers.ReapCompleted(error) && !workers.ProcessActive(41), "reap establishes per-process join barrier");
        Require(workers.Managed() && workers.Snapshot().accepting, "idle phase does not end session or close admission");
        Require(workers.Spawn([&] { ++ran; }, 42, std::make_shared<int>(2), error), "next command shares the kernel session");
        workers.CloseAdmission();
        Require(workers.Join(error), "join all after caller requests kernel stop");
        const auto joined = workers.Snapshot();
        Require(ran == 2 && joined.admitted == 2 && joined.finished == 2 && joined.joined == 2 && joined.running == 0,
                "actual execution and final join counts agree");
        Require(!joined.failed, "successful session has no hidden errors");
        Require(workers.End(error) && !workers.Managed(), "end only after both phases joined");
        Require(workers.Snapshot().finished == 2, "ending preserves final evidence counters");
        Require(workers.Begin(error) && workers.Snapshot().admitted == 0, "new explicit session resets counters");
        workers.CloseAdmission();
        Require(!workers.Spawn([] {}, 1, std::make_shared<int>(0), error), "stop closes later worker admission");
        Require(workers.Snapshot().failed && workers.Snapshot().spawnFailures == 1 && workers.Snapshot().admitted == 0,
                "rejected spawn is visible and not counted as a started thread");
        Require(workers.Join(error) && workers.End(error), "failed admission still permits safe empty teardown");
    }
    void SpawnFailureChecks()
    {
        GuestWorkerRegistry workers([](std::function<void()>) -> std::thread {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "fixture thread creation");
        });
        std::string error;
        Require(workers.Begin(error), "begin constructor-failure session");
        struct Pin
        {
            GuestWorkerRegistry* owner;
            std::atomic<bool>* released;
            ~Pin() { (void)owner->Snapshot(); released->store(true); }
        };
        std::atomic<bool> released{false}, ran{false};
        auto pin = std::make_shared<Pin>(); pin->owner = &workers; pin->released = &released;
        Require(!workers.Spawn([&] { ran = true; }, 9, pin, error), "std::thread constructor failure reaches caller");
        pin.reset();
        Require(released && !ran && !error.empty(), "failed entry never runs and retention releases outside mutex");
        const auto state = workers.Snapshot();
        Require(state.admitted == 0 && state.spawnFailures == 1 && state.failed && !workers.ProcessActive(9),
                "constructor failure creates no phantom registry worker");
        workers.CloseAdmission();
        Require(workers.Join(error) && workers.End(error), "constructor failure has no unowned host thread");
    }
    void WorkerFailureChecks()
    {
        GuestWorkerRegistry workers;
        std::string error;
        Require(workers.Begin(error), "begin exception session");
        Require(workers.Spawn([] { throw std::runtime_error("fixture entry failure"); }, 7, std::make_shared<int>(1), error),
                "launch throwing entry as owned thread");
        workers.CloseAdmission();
        Require(workers.Join(error), "exception does not bypass actual host thread join");
        const auto state = workers.Snapshot();
        Require(state.failed && state.workerFailures == 1 && state.joined == 1 && state.error == "fixture entry failure",
                "worker exception is captured without success or process termination");
        Require(workers.End(error), "failed worker retention can be released after join");
    }
    struct ExitGate
    {
        std::promise<void> entered;
        std::promise<void> release;
        std::shared_future<void> allowed{release.get_future().share()};
        std::atomic<bool> finished{false};
    };
    struct ThreadLocalExit
    {
        std::shared_ptr<ExitGate> gate;
        ~ThreadLocalExit()
        {
            if (!gate) return;
            gate->entered.set_value();
            gate->allowed.wait();
            gate->finished = true;
        }
    };
    thread_local ThreadLocalExit threadExit;
    void ThreadLocalBarrierChecks()
    {
        GuestWorkerRegistry workers;
        std::string error;
        Require(workers.Begin(error), "begin TLS-finalization session");
        auto gate = std::make_shared<ExitGate>();
        auto entered = gate->entered.get_future();
        struct Pin
        {
            GuestWorkerRegistry* owner;
            std::shared_ptr<ExitGate> gate;
            std::atomic<bool>* early;
            ~Pin() { if (!gate->finished) *early = true; (void)owner->Snapshot(); }
        };
        std::atomic<bool> early{false};
        auto pin = std::make_shared<Pin>(); pin->owner = &workers; pin->gate = gate; pin->early = &early;
        std::weak_ptr<Pin> retained = pin;
        Require(workers.Spawn([gate] { threadExit.gate = gate; }, 77, pin, error), "launch entry with blocking real TLS destructor");
        pin.reset();
        entered.wait();
        const auto beforeReap = workers.Snapshot();
        const bool retainedDuringTls = beforeReap.finished == 0 && workers.ProcessActive(77) && !retained.expired();
        struct ReapResult { bool success = false; std::string error; };
        std::promise<void> reaperEntered;
        auto reaperStarted = reaperEntered.get_future();
        std::promise<ReapResult> reapCompleted;
        auto reapResult = reapCompleted.get_future();
        // A dedicated thread avoids measuring std::async's implementation or
        // thread startup against the nonblocking registry operation deadline.
        std::thread reaper;
        try
        {
            reaper = std::thread([&] {
                reaperEntered.set_value();
                ReapResult result;
                try { result.success = workers.ReapCompleted(result.error); }
                catch (const std::exception& failure) { result.error = failure.what(); }
                catch (...) { result.error = "nonstandard reaper exception"; }
                reapCompleted.set_value(std::move(result));
            });
        }
        catch (...) { gate->release.set_value(); throw; }
        const bool started = reaperStarted.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        const bool nonblocking = started &&
            reapResult.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
        // Release the real TLS destructor before any assertion/diagnostic that
        // might throw, and join the test helper before inspecting its result.
        gate->release.set_value();
        reaper.join();
        const auto reaped = reapResult.get();
        const auto afterReap = workers.Snapshot();
        const std::string diagnostic = "reap does not join a worker still running TLS destructors: started=" +
            std::to_string(started) + " nonblocking=" + std::to_string(nonblocking) +
            " reap_success=" + std::to_string(reaped.success) + " reap_error=" + reaped.error +
            " before_finished=" + std::to_string(beforeReap.finished) +
            " after_finished=" + std::to_string(afterReap.finished) +
            " after_joined=" + std::to_string(afterReap.joined) +
            " after_running=" + std::to_string(afterReap.running) +
            " registry_failed=" + std::to_string(afterReap.failed) +
            " registry_error=" + afterReap.error;
        Require(retainedDuringTls, "function return cannot advertise completion before TLS destruction");
        Require(reaped.success && nonblocking, diagnostic.c_str());
        workers.CloseAdmission();
        Require(workers.Join(error), "final OS thread join waits for full TLS completion");
        Require(gate->finished && retained.expired() && !early, "process pin releases only after final cleanup and join");
        Require(workers.End(error), "TLS-qualified session ends safely");
    }
    void SelfJoinChecks()
    {
        GuestWorkerRegistry workers;
        std::string error;
        Require(workers.Begin(error), "begin self-join rejection session");
        std::atomic<bool> joinRejected{false}, reapRejected{false};
        Require(workers.Spawn([&] {
            workers.CloseAdmission();
            std::string local;
            joinRejected = !workers.Join(local);
            reapRejected = !workers.ReapCompleted(local);
            (void)workers.Snapshot();
        }, 8, std::make_shared<int>(1), error), "launch worker that tries forbidden joining");
        Wait([&] { return workers.Snapshot().finished == 1; });
        Require(joinRejected && reapRejected, "worker cannot self-join or monopolize reaping");
        Require(workers.Join(error) && workers.End(error), "session owner joins rejected self-join fixture");
    }
}
int main()
{
    try
    {
        LifecycleChecks(); SpawnFailureChecks(); WorkerFailureChecks(); ThreadLocalBarrierChecks(); SelfJoinChecks();
        std::printf("Owned guest workers: %u checks passed (self-authored host threads; no guest/core executed)\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Owned worker check %u failed: %s\n", checks, error.what());
        return 1;
    }
}
