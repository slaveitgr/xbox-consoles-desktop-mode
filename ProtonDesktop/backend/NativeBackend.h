// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "core/BootRequest.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace protondesktop
{
    struct PixelFrame
    {
        const uint8_t* bgra = nullptr;
        uint32_t width = 0, height = 0, stride = 0;
    };
    struct NativeHost
    {
        // Actual X11/Wine pixels only. The UWP adapter must marshal presentation
        // to its window thread and copy pixels before this callback returns.
        std::function<void(const PixelFrame&)> present;
        std::function<void(const std::string&)> diagnostic;
    };
    class NativeBackend
    {
    public:
        virtual ~NativeBackend() = default;
        virtual bool Start(const BootRequest&, NativeHost, std::string* error) = 0;
        virtual void RequestStop() noexcept = 0;
        virtual void Join() noexcept = 0;
    };
    // Returns null until the actual pinned interpreter/kernel/presenter is
    // ported and linked. This factory never substitutes HTML/WASM or a demo.
    std::unique_ptr<NativeBackend> CreateNativeBackend();
    const char* NativeBackendStatus() noexcept;
}
