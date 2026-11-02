// SPDX-License-Identifier: GPL-2.0-only
#include "core/BootRequest.h"
#include <utility>

namespace protondesktop
{
    namespace
    {
        bool PathValid(const std::string& value)
        {
            return !value.empty() && value.find('\0') == std::string::npos &&
                   value.find('\n') == std::string::npos && value.find('\r') == std::string::npos;
        }
        bool Fail(std::string* error, const char* text)
        {
            if (error) *error = text;
            return false;
        }
    }
    bool BuildDesktopBootRequest(const std::string& root, const std::vector<std::string>& layers,
                                 uint32_t width, uint32_t height,
                                 BootRequest* result, std::string* error)
    {
        if (!result) return Fail(error, "A boot request output is required.");
        if (!PathValid(root)) return Fail(error, "A writable app-owned root path is required.");
        if (width < 640 || height < 480 || width > 3840 || height > 2160)
            return Fail(error, "Desktop dimensions are outside the supported launch range.");
        if (layers.size() != 3) return Fail(error, "Exactly glibc, Wine64 and prepared-prefix layers are required.");
        for (const auto& layer : layers)
            if (!PathValid(layer)) return Fail(error, "A rootfs layer path is empty or contains a control character.");
        if (layers[0] == layers[1] || layers[0] == layers[2] || layers[1] == layers[2])
            return Fail(error, "Rootfs layers must be distinct.");
        BootRequest request;
        request.backendRevision = BackendRevision;
        request.writableRoot = root;
        request.rootfsLayers = layers;
        request.width = width;
        request.height = height;
        request.environment = {"HOME=/home/username", "USER=username",
            "WINEPREFIX=/home/username/.wine", "WINELOADER=/usr/lib/wine/wine64",
            "WINESERVER=/usr/lib/wine/wineserver64",
            "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine", "DISPLAY=:0"};
        request.guestArgv = {"/usr/lib/wine/wine64", "explorer.exe",
            "/desktop=ProtonDesktop," + std::to_string(width) + "x" + std::to_string(height)};
        *result = std::move(request);
        if (error) error->clear();
        return true;
    }
}
