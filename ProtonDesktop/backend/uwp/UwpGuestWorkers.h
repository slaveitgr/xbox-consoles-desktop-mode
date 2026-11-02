// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace protondesktop::uwp
{
    struct GuestWorkerSnapshot
    {
        bool managed = false, accepting = false, failed = false;
        std::uint64_t admitted = 0, finished = 0, joined = 0, running = 0;
        std::uint64_t spawnFailures = 0, workerFailures = 0;
        std::string error;
    };

    // Owns real host threads. The kernel must request termination/wake blocked
    // guests before Join; closing admission is not a cancellation operation.
    // No process/kernel/VFS teardown is permitted until the join barrier passes.
    class GuestWorkerRegistry
    {
    public:
        using ThreadFactory = std::function<std::thread(std::function<void()>)>;
        // Optional factory permits deterministic constructor-failure testing;
        // it must either return a joinable thread or throw before entry runs.
        explicit GuestWorkerRegistry(ThreadFactory factory = {});
        ~GuestWorkerRegistry();
        GuestWorkerRegistry(const GuestWorkerRegistry&) = delete;
        GuestWorkerRegistry& operator=(const GuestWorkerRegistry&) = delete;
        bool Begin(std::string& error);
        void CloseAdmission();
        bool Spawn(std::function<void()> entry, std::uint32_t processId,
                   std::shared_ptr<void> processPin, std::string& error);
        // Reaps only futures made ready at thread exit (after TLS destructors),
        // then joins outside the registry mutex. Joining from a worker fails.
        bool ReapCompleted(std::string& error);
        // Admission must already be closed. Blocking, no timeout/cancellation.
        // Success means all admitted threads joined; inspect Snapshot.failed
        // separately for exceptions or rejected thread creation during the run.
        bool Join(std::string& error);
        bool End(std::string& error);
        // Active includes completed-but-not-joined workers, making false a
        // genuine per-process join barrier, rather than a guest-thread count.
        bool ProcessActive(std::uint32_t processId) const;
        GuestWorkerSnapshot Snapshot() const;
        bool Managed() const;
    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // One active kernel session per process; used by the checked pinned-source
    // scheduler overlay. A closed session continues to be managed until End.
    bool BeginGuestWorkerSession(std::string& error);
    void CloseGuestWorkerAdmission();
    bool SpawnGuestWorker(std::function<void()> entry, std::uint32_t processId,
                          std::shared_ptr<void> processPin, std::string& error);
    bool ReapCompletedGuestWorkers(std::string& error);
    bool JoinGuestWorkers(std::string& error);
    bool EndGuestWorkerSession(std::string& error);
    bool ProcessHasActiveGuestWorkers(std::uint32_t processId);
    GuestWorkerSnapshot GetGuestWorkerSnapshot();
    bool GuestWorkerSessionManaged();
}
