// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace protondesktop
{
    inline constexpr char BackendRevision[] = "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4";
    struct BootRequest
    {
        std::string backendRevision;
        std::string writableRoot;
        std::vector<std::string> rootfsLayers;
        std::vector<std::string> environment;
        std::vector<std::string> guestArgv;
        uint32_t width = 0, height = 0;
    };
    // A launch description only. It neither imports rootfs files nor starts a
    // guest. Rootfs hashes/licenses and the backend are independent gates.
    bool BuildDesktopBootRequest(const std::string& writableRoot,
                                 const std::vector<std::string>& rootfsLayers,
                                 uint32_t width, uint32_t height,
                                 BootRequest* result, std::string* error);
}
