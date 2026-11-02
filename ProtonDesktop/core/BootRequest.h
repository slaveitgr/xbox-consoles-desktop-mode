// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace protondesktop
{
    inline constexpr char BackendRevision[] = "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4";
    enum class BootKind
    {
        ExplorerDesktop,
        PrefixInitialization
    };
    // Caller-supplied preconditions, not filesystem evidence. The native backend
    // must verify app-local ownership and actual emptiness before first boot.
    struct WritableOverlayDescriptor
    {
        std::string rootPath;
        bool appOwned = false;
        bool initiallyEmpty = false;
    };
    struct BootRequest
    {
        BootKind kind = BootKind::ExplorerDesktop;
        std::string backendRevision;
        // PrefixInitialization requires this app-owned overlay to be empty;
        // ExplorerDesktop uses a separate prepared-prefix layer beneath it.
        std::string writableRoot;
        std::vector<std::string> rootfsLayers;
        std::vector<std::string> environment;
        std::vector<std::string> guestArgv;
        uint32_t width = 0, height = 0;
    };
    // Launch descriptions only. They neither inspect/import rootfs files nor
    // start a guest. Hashes/licenses and a working backend are independent gates.
    bool BuildDesktopBootRequest(const std::string& writableRoot,
                                 const std::vector<std::string>& rootfsLayers,
                                 uint32_t width, uint32_t height,
                                 BootRequest* result, std::string* error);
    // Requires exactly glibc + Wine64 base layers, never a prepared-prefix layer.
    // Successful construction does not mean wineboot ran or a prefix is ready.
    bool BuildPrefixBootRequest(const WritableOverlayDescriptor& writableOverlay,
                                const std::vector<std::string>& baseLayers,
                                uint32_t width, uint32_t height,
                                BootRequest* result, std::string* error);
}
