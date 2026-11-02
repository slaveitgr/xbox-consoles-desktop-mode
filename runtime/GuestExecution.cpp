// SPDX-License-Identifier: MIT
#include "runtime/GuestExecution.h"

#include <cstdlib>
#include <new>
#include <utility>

namespace xcdm::guest
{
    namespace
    {
        struct EntryState
        {
            GuestExecutionContext* context = nullptr;
            GuestThread* binding = nullptr;
            size_t depth = 0;
            bool ownsBinding = false;
        };
        thread_local EntryState t_entry;

        struct AttachmentRollback
        {
            GuestProcess* process;
            bool owned;
            ~AttachmentRollback() { if (owned) process->DetachCurrentThread(); }
        };
    }

    GuestExecutionContext::GuestExecutionContext(std::unique_ptr<GuestProcess> process) noexcept
        : m_process(std::move(process))
    {
    }

    std::shared_ptr<GuestExecutionContext> GuestExecutionContext::Create(
        std::unique_ptr<GuestProcess> process, std::string* error)
    {
        if (!process)
        {
            if (error) *error = "A live guest process is required.";
            return nullptr;
        }
        try
        {
            return std::shared_ptr<GuestExecutionContext>(new GuestExecutionContext(std::move(process)));
        }
        catch (const std::bad_alloc&)
        {
            if (error) *error = "Guest execution context allocation failed.";
            return nullptr;
        }
    }

    void GuestExecutionContext::RequestStop() noexcept
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_stopping = true;
    }

    bool GuestExecutionContext::IsStopping() const noexcept
    {
        std::lock_guard<std::mutex> guard(m_lock);
        return m_stopping;
    }

    size_t GuestExecutionContext::ActiveEntries() const noexcept
    {
        std::lock_guard<std::mutex> guard(m_lock);
        return m_active;
    }

    bool GuestExecutionContext::Enter(bool nested)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (m_stopping && !nested) return false;
        ++m_active;
        return true;
    }

    void GuestExecutionContext::Leave() noexcept
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (!m_active) std::abort();
        if (--m_active == 0) m_idle.notify_all();
    }

    bool GuestExecutionContext::WaitForIdle()
    {
        // CurrentExecutionContext intentionally hides another fiber's binding.
        // The thread-local entry still identifies that wait as a self-deadlock.
        if (t_entry.context == this && t_entry.depth) return false;
        std::unique_lock<std::mutex> guard(m_lock);
        m_idle.wait(guard, [this] { return m_active == 0; });
        return true;
    }

    GuestThreadScope::GuestThreadScope(std::shared_ptr<GuestExecutionContext> context)
        : m_context(std::move(context)), m_owner(std::this_thread::get_id())
    {
        if (!m_context)
        {
            m_error = "A guest execution context is required.";
            return;
        }
        GuestProcess& process = m_context->Process();
        GuestThread* previousBinding = CurrentThread();
        // Always ask the environment: a null CurrentThread can indicate a
        // conflicting host fiber rather than an unbound host thread.
        GuestThread* binding = process.AttachCurrentThread(&m_error);
        if (!binding) return;
        AttachmentRollback rollback{ &process, previousBinding == nullptr };
        const bool nested = t_entry.context == m_context.get() && t_entry.binding == binding && t_entry.depth;
        if (t_entry.depth && !nested)
        {
            m_error = "The current guest activation does not match this process/thread binding.";
            return;
        }
        if (!m_context->Enter(nested))
        {
            m_error = "Guest execution is stopping; a new external entry is rejected.";
            return;
        }
        // All remaining operations are nonthrowing. Publish only after both
        // attachment and admission have succeeded.
        if (!nested)
            t_entry = { m_context.get(), binding, 0, rollback.owned };
        m_depth = ++t_entry.depth;
        m_binding = binding;
        m_active = true;
        rollback.owned = false;
    }

    GuestThreadScope::~GuestThreadScope() noexcept
    {
        if (!m_active) return;
        GuestThread* live = CurrentThread();
        if (std::this_thread::get_id() != m_owner || !live || live != m_binding ||
            t_entry.context != m_context.get() || t_entry.binding != live || t_entry.depth != m_depth)
            std::abort();
        if (--t_entry.depth == 0)
        {
            if (t_entry.ownsBinding) m_context->Process().DetachCurrentThread();
            t_entry = {};
        }
        m_active = false;
        m_context->Leave();
    }

    GuestExecutionContext* CurrentExecutionContext() noexcept
    {
        GuestThread* live = CurrentThread();
        return live && t_entry.depth && live == t_entry.binding ? t_entry.context : nullptr;
    }
}
