// SPDX-License-Identifier: MIT
// Portable process lifetime and thread-entry scopes. This does not admit Wine,
// validate heap/NLS providers, or retain loader images and callback targets.
#pragma once

#include "runtime/GuestEnvironment.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace xcdm::guest
{
    class GuestThreadScope;

    class GuestExecutionContext final
    {
    public:
        static std::shared_ptr<GuestExecutionContext> Create(std::unique_ptr<GuestProcess> process,
                                                             std::string* error);
        GuestProcess& Process() noexcept { return *m_process; }
        const GuestProcess& Process() const noexcept { return *m_process; }

        void RequestStop() noexcept;
        bool IsStopping() const noexcept;
        size_t ActiveEntries() const noexcept;
        // Returns false instead of waiting on an entry owned by this host
        // thread, including a suspended fiber. RequestStop first when draining.
        bool WaitForIdle();

        GuestExecutionContext(const GuestExecutionContext&) = delete;
        GuestExecutionContext& operator=(const GuestExecutionContext&) = delete;

    private:
        friend class GuestThreadScope;
        explicit GuestExecutionContext(std::unique_ptr<GuestProcess> process) noexcept;
        bool Enter(bool nested);
        void Leave() noexcept;

        std::unique_ptr<GuestProcess> m_process;
        mutable std::mutex m_lock;
        std::condition_variable m_idle;
        bool m_stopping = false;
        size_t m_active = 0;
    };

    // Nonmovable, LIFO scopes must be destroyed on their entering host
    // thread/fiber. Only the outer scope that created a binding detaches it.
    // A pre-existing manual binding remains its caller's responsibility; retain
    // the context until that binding is explicitly detached or its thread exits.
    // Do not manually detach or replace a binding while any scope is active.
    // Normal C++ return/unwind is supported. ExitThread/longjmp/forced fiber or
    // thread destruction that bypasses C++ destructors is not supported.
    class GuestThreadScope final
    {
    public:
        explicit GuestThreadScope(std::shared_ptr<GuestExecutionContext> context);
        ~GuestThreadScope() noexcept;
        bool IsActive() const noexcept { return m_active; }
        const std::string& Error() const noexcept { return m_error; }

        GuestThreadScope(const GuestThreadScope&) = delete;
        GuestThreadScope& operator=(const GuestThreadScope&) = delete;
        GuestThreadScope(GuestThreadScope&&) = delete;
        GuestThreadScope& operator=(GuestThreadScope&&) = delete;

    private:
        std::shared_ptr<GuestExecutionContext> m_context;
        GuestThread* m_binding = nullptr; // Identity only; never dereferenced.
        std::thread::id m_owner;
        size_t m_depth = 0;
        bool m_active = false;
        std::string m_error;
    };

    // Null when no successfully entered scope belongs to this host thread and
    // fiber. Bridge calls must fail closed; never fall back to a host context.
    GuestExecutionContext* CurrentExecutionContext() noexcept;
}
