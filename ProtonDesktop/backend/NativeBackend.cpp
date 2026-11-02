// SPDX-License-Identifier: GPL-2.0-only
#include "backend/NativeBackend.h"
namespace protondesktop
{
    std::unique_ptr<NativeBackend> CreateNativeBackend() { return nullptr; }
    const char* NativeBackendStatus() noexcept
    {
        return "Boxedwine64 UWP backend is not linked. No Wine desktop has started.";
    }
}
