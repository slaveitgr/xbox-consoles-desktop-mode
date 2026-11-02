// SPDX-License-Identifier: GPL-2.0-only
#include "ProtonSdlVideoHostInternal.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace {
struct State {
    std::mutex mutex;
    ProtonSdlHost host{};
    bool configured = false, active = false, failed = false;
    std::thread::id owner;
    char error[256]{};
} state;

void FailLocked(const char* message) noexcept
{
    if (!state.failed) {
        state.failed = true;
        const char* source = message ? message : "SDL host failed without a diagnostic";
        const auto length = std::strlen(source);
        const auto count = length < sizeof(state.error) - 1 ? length : sizeof(state.error) - 1;
        std::memcpy(state.error, source, count);
        state.error[count] = '\0';
    }
}
bool ValidateDisplay(const ProtonSdlDisplay& display) noexcept
{
    return display.width && display.height &&
        display.width <= PROTON_SDL_VIDEO_MAX_DIMENSION &&
        display.height <= PROTON_SDL_VIDEO_MAX_DIMENSION &&
        display.refresh_hz <= 1000 && std::isfinite(display.dpi) && display.dpi > 0;
}
bool CurrentOwnerLocked() noexcept
{
    if (!state.active || state.owner != std::this_thread::get_id()) {
        FailLocked("SDL host accessed outside its active owner thread");
        return false;
    }
    return !state.failed;
}
}

extern "C" int ProtonSdlVideoConfigure(const ProtonSdlHost* host)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.active) { FailLocked("Cannot configure SDL host while video is active"); return 0; }
    if (!host || host->abi_version != PROTON_SDL_VIDEO_ABI ||
        !host->query_display || !host->present) {
        FailLocked("SDL video requires a versioned host with geometry and copying-present callbacks");
        return 0;
    }
    state.host = *host;
    state.configured = true;
    state.failed = false;
    state.error[0] = '\0';
    return 1;
}
extern "C" int ProtonSdlVideoReset(void)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.active) { FailLocked("Cannot reset SDL host before video quit"); return 0; }
    state.host = {};
    state.configured = false;
    // Preserve diagnostics until the next successful Configure.
    return 1;
}
extern "C" int ProtonSdlVideoCopyError(char* destination, size_t capacity)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (destination && capacity) {
        const auto length = std::strlen(state.error);
        const auto count = length < capacity - 1 ? length : capacity - 1;
        std::memcpy(destination, state.error, count);
        destination[count] = '\0';
    }
    return state.failed ? 1 : 0;
}
extern "C" void ProtonSdlHostFail(const char* diagnostic)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    FailLocked(diagnostic);
}
extern "C" int ProtonSdlHostConfigured(void)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.configured) { FailLocked("No native SDL host is configured"); return 0; }
    return !state.failed;
}
extern "C" int ProtonSdlHostAcquire(ProtonSdlDisplay* display)
{
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.configured || state.failed || state.active || !display) {
            FailLocked("Cannot acquire absent, failed or already active SDL host");
            return 0;
        }
        state.active = true;
        state.owner = std::this_thread::get_id();
    }
    if (ProtonSdlHostReadDisplay(display)) return 1;
    ProtonSdlHostRelease();
    return 0;
}
extern "C" int ProtonSdlHostReadDisplay(ProtonSdlDisplay* display)
{
    ProtonSdlHost host{};
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!CurrentOwnerLocked() || !display) return 0;
        host = state.host;
    }
    ProtonSdlDisplay actual{};
    try {
        if (host.query_display(host.userdata, &actual) != 1 || !ValidateDisplay(actual)) {
            ProtonSdlHostFail("Native SDL host declined or returned invalid display geometry");
            return 0;
        }
    } catch (...) {
        ProtonSdlHostFail("Native SDL host geometry callback threw");
        return 0;
    }
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!CurrentOwnerLocked()) return 0;
    *display = actual;
    return 1;
}
extern "C" int ProtonSdlHostPresent(const ProtonSdlFrame* frame)
{
    ProtonSdlHost host{};
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!CurrentOwnerLocked()) return 0;
        if (!frame || !frame->bgra || !frame->width || !frame->height ||
            frame->width > PROTON_SDL_VIDEO_MAX_DIMENSION || frame->height > PROTON_SDL_VIDEO_MAX_DIMENSION ||
            frame->stride < frame->width * 4 || frame->stride > PROTON_SDL_VIDEO_MAX_DIMENSION * 4 ||
            frame->height > std::numeric_limits<size_t>::max() / frame->stride) {
            FailLocked("Native SDL host received invalid framebuffer geometry");
            return 0;
        }
        host = state.host;
    }
    try {
        if (host.present(host.userdata, frame) != 1) {
            ProtonSdlHostFail("Native SDL host did not accept and copy the framebuffer");
            return 0;
        }
    } catch (...) {
        ProtonSdlHostFail("Native SDL host framebuffer callback threw");
        return 0;
    }
    std::lock_guard<std::mutex> lock(state.mutex);
    return CurrentOwnerLocked() ? 1 : 0;
}
extern "C" void ProtonSdlHostRelease(void)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.active) return;
    if (state.owner != std::this_thread::get_id()) {
        FailLocked("SDL video quit must run on its owner thread");
        return;
    }
    state.active = false;
    state.owner = {};
}
