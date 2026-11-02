// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace protondesktop::runtime
{
    enum class SessionPhase
    {
        Validation, WineVersion, PrefixInitialization, ExplorerDesktop,
        Stopping, Finished
    };

    struct SessionRequest
    {
        // UTF-8 native paths. The runtime verifies app-local ownership and
        // rejects native paths that its upstream narrow filesystem cannot use
        // losslessly. ZIP order: glibc base, Wine64 base; no pretend prefix ZIP.
        std::string rootPath;
        std::vector<std::string> zipLayers;
        std::vector<std::string> argv;
        std::vector<std::string> environment;
        std::uint32_t width = 1920, height = 1080;
        bool initializePrefixIfNeeded = true;
    };

    struct GuestCommandResult
    {
        bool attempted = false, loaded = false, exited = false;
        std::uint32_t pid = 0;
        std::int32_t exitCode = -1;
    };

    struct SessionResult
    {
        GuestCommandResult version, prefix, requested, shutdown;
        bool stopped = false, cleanShutdown = false;
        // Live registry/file checks do not qualify a redistributable snapshot.
        bool prefixFilesPresent = false;
        std::uint64_t guestFramesSubmitted = 0;
        std::string error;
    };

    using SessionObserver = std::function<void(SessionPhase, const std::string&)>;

    // Synchronous, one session per process, called on SDL/WinRT's owner thread.
    // Owns SDL, the pinned kernel, ZIP handles and guest workers until teardown.
    // Runs real Wine --version, initializes a missing prefix with wineboot,
    // then starts argv in that SAME kernel. Keeps pumping the desktop after
    // its launcher exits, until stop is set, SDL closes, or a real failure.
    // The observer runs on this same thread and must not throw or start SDL.
    SessionResult RunWineSession(const SessionRequest& request,
        const SessionObserver& observer,
        const std::shared_ptr<std::atomic<bool>>& stop);
}
