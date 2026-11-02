// SPDX-License-Identifier: GPL-2.0-only
#include "UwpGuestWorkers.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <future>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace protondesktop::uwp
{
    struct GuestWorkerRegistry::Impl
    {
        struct Record
        {
            std::function<void()> entry;
            std::shared_ptr<void> processPin;
            std::uint32_t processId;
            std::promise<void> exited;
            std::future<void> exitReady{exited.get_future()};
            std::thread thread;
            std::thread::id id;
            bool joining = false, joined = false;
            Record(std::function<void()> task, std::uint32_t pid, std::shared_ptr<void> pin)
                : entry(std::move(task)), processPin(std::move(pin)), processId(pid) {}
            bool Finished() const
            { return exitReady.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
        };
        mutable std::mutex mutex;
        std::mutex drainMutex;
        std::vector<std::unique_ptr<Record>> records;
        ThreadFactory factory;
        bool managed = false, accepting = false, failed = false;
        std::uint64_t admitted = 0, joined = 0, spawnFailures = 0, workerFailures = 0;
        char firstError[256]{};

        void FailureLocked(const char* message) noexcept
        {
            failed = true;
            if (firstError[0]) return;
            if (!message) message = "Guest worker failed without a diagnostic";
            const auto count = std::min(std::strlen(message), sizeof(firstError) - 1);
            std::memcpy(firstError, message, count);
            firstError[count] = 0;
        }
        bool CallerIsWorkerLocked() const noexcept
        {
            const auto current = std::this_thread::get_id();
            for (const auto& record : records)
                if (!record->joined && record->id == current) return true;
            return false;
        }
        void Run(Record* record) noexcept
        {
            try { record->entry(); }
            catch (const std::exception& error)
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++workerFailures;
                FailureLocked(error.what());
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++workerFailures;
                FailureLocked("Guest worker raised a nonstandard C++ exception");
            }
            // The future becomes ready after all thread-local destructors.
            // No registry mutex is retained while those destructors execute.
            record->exited.set_value_at_thread_exit();
        }
        bool Drain(bool all, std::string& error)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (CallerIsWorkerLocked()) { error = "A guest worker cannot reap or join its own registry"; return false; }
                if (!managed) { error = "No managed guest worker session exists"; return false; }
                if (all && accepting) { error = "Close guest worker admission before joining"; return false; }
            }
            std::unique_lock<std::mutex> draining(drainMutex, std::try_to_lock);
            if (!draining.owns_lock()) { error = "Another guest worker drain is already in progress"; return false; }
            struct Pending { Record* record; std::thread thread; };
            std::vector<Pending> pending;
            {
                std::lock_guard<std::mutex> lock(mutex);
                // Reserve before moving any joinable threads: allocation
                // failure cannot destroy an unjoined thread in a local vector.
                pending.reserve(records.size());
                for (const auto& record : records)
                {
                    if (record->joined || record->joining || (!all && !record->Finished())) continue;
                    record->joining = true;
                    pending.push_back({record.get(), std::move(record->thread)});
                }
            }
            bool success = true;
            for (auto& item : pending)
            {
                try { item.thread.join(); }
                catch (const std::system_error& failure)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    item.record->thread = std::move(item.thread);
                    item.record->joining = false;
                    FailureLocked(failure.what());
                    success = false;
                    continue;
                }
                std::shared_ptr<void> releasedPin;
                std::function<void()> releasedEntry;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    item.record->joined = true;
                    item.record->joining = false;
                    ++joined;
                    releasedPin = std::move(item.record->processPin);
                    releasedEntry = std::move(item.record->entry);
                }
                // KProcess/task destructors may call the kernel or registry;
                // release both only after join and outside the registry mutex.
                releasedEntry = {};
                releasedPin.reset();
            }
            if (success) error.clear();
            else error = "A guest worker join failed; its thread remains owned by the registry";
            return success;
        }
    };

    GuestWorkerRegistry::GuestWorkerRegistry(ThreadFactory factory) : impl(std::make_unique<Impl>())
    {
        impl->factory = factory ? std::move(factory) : ThreadFactory([](std::function<void()> entry) {
            return std::thread(std::move(entry));
        });
    }
    GuestWorkerRegistry::~GuestWorkerRegistry()
    {
        CloseAdmission();
        if (Managed())
        {
            std::string error;
            // Freeing a registry from its own worker cannot safely complete.
            // Fail closed instead of detaching or allowing a use-after-free.
            if (!Join(error) || !End(error)) std::terminate();
        }
    }
    bool GuestWorkerRegistry::Begin(std::string& error)
    {
        std::unique_lock<std::mutex> draining(impl->drainMutex, std::try_to_lock);
        if (!draining.owns_lock()) { error = "A guest worker drain is in progress"; return false; }
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->managed || !impl->records.empty()) { error = "The previous guest worker session has not ended"; return false; }
        impl->managed = impl->accepting = true;
        impl->failed = false;
        impl->admitted = impl->joined = impl->spawnFailures = impl->workerFailures = 0;
        impl->firstError[0] = 0;
        error.clear();
        return true;
    }
    void GuestWorkerRegistry::CloseAdmission()
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->accepting = false;
    }
    bool GuestWorkerRegistry::Spawn(std::function<void()> entry, std::uint32_t processId,
                                    std::shared_ptr<void> processPin, std::string& error)
    {
        std::unique_ptr<Impl::Record> created;
        try
        {
            if (entry && processPin)
                created = std::make_unique<Impl::Record>(std::move(entry), processId, std::move(processPin));
        }
        catch (const std::exception& failure)
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            ++impl->spawnFailures;
            impl->FailureLocked(failure.what());
            error = failure.what();
            return false;
        }
        std::unique_lock<std::mutex> lock(impl->mutex);
        if (!impl->managed || !impl->accepting || !created)
        {
            ++impl->spawnFailures;
            error = !impl->managed || !impl->accepting ? "Guest worker admission is closed" : "Guest worker entry/process retention is missing";
            impl->FailureLocked(error.c_str());
            return false;
        }
        try
        {
            const auto slot = created.get();
            impl->records.push_back(std::move(created));
            try
            {
                slot->thread = impl->factory([state = impl.get(), slot] { state->Run(slot); });
                if (!slot->thread.joinable()) throw std::runtime_error("Thread factory returned no joinable guest worker");
                slot->id = slot->thread.get_id();
                ++impl->admitted;
                error.clear();
                return true;
            }
            catch (...)
            {
                created = std::move(impl->records.back());
                impl->records.pop_back();
                throw;
            }
        }
        catch (const std::exception& failure) { error = failure.what(); }
        catch (...) { error = "Guest worker construction raised a nonstandard C++ exception"; }
        ++impl->spawnFailures;
        impl->FailureLocked(error.c_str());
        lock.unlock();
        return false;
    }
    bool GuestWorkerRegistry::ReapCompleted(std::string& error) { return impl->Drain(false, error); }
    bool GuestWorkerRegistry::Join(std::string& error) { return impl->Drain(true, error); }
    bool GuestWorkerRegistry::End(std::string& error)
    {
        std::unique_lock<std::mutex> draining(impl->drainMutex, std::try_to_lock);
        if (!draining.owns_lock()) { error = "A guest worker drain is in progress"; return false; }
        std::vector<std::unique_ptr<Impl::Record>> released;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!impl->managed || impl->accepting || impl->joined != impl->admitted)
            { error = "Close admission and join every guest worker before ending its session"; return false; }
            impl->managed = false;
            released.swap(impl->records);
        }
        released.clear();
        error.clear();
        return true;
    }
    bool GuestWorkerRegistry::ProcessActive(std::uint32_t processId) const
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        for (const auto& record : impl->records)
            if (record->processId == processId && !record->joined) return true;
        return false;
    }
    GuestWorkerSnapshot GuestWorkerRegistry::Snapshot() const
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        GuestWorkerSnapshot result;
        result.managed = impl->managed;
        result.accepting = impl->accepting;
        result.failed = impl->failed;
        result.admitted = impl->admitted;
        result.joined = impl->joined;
        result.running = impl->admitted - impl->joined;
        result.finished = impl->joined;
        for (const auto& record : impl->records) if (!record->joined && record->Finished()) ++result.finished;
        result.spawnFailures = impl->spawnFailures;
        result.workerFailures = impl->workerFailures;
        result.error = impl->firstError;
        return result;
    }
    bool GuestWorkerRegistry::Managed() const
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        return impl->managed;
    }
    namespace { GuestWorkerRegistry& Registry() { static GuestWorkerRegistry registry; return registry; } }
    bool BeginGuestWorkerSession(std::string& error) { return Registry().Begin(error); }
    void CloseGuestWorkerAdmission() { Registry().CloseAdmission(); }
    bool SpawnGuestWorker(std::function<void()> entry, std::uint32_t pid, std::shared_ptr<void> pin, std::string& error)
    { return Registry().Spawn(std::move(entry), pid, std::move(pin), error); }
    bool ReapCompletedGuestWorkers(std::string& error) { return Registry().ReapCompleted(error); }
    bool JoinGuestWorkers(std::string& error) { return Registry().Join(error); }
    bool EndGuestWorkerSession(std::string& error) { return Registry().End(error); }
    bool ProcessHasActiveGuestWorkers(std::uint32_t pid) { return Registry().ProcessActive(pid); }
    GuestWorkerSnapshot GetGuestWorkerSnapshot() { return Registry().Snapshot(); }
    bool GuestWorkerSessionManaged() { return Registry().Managed(); }
}
