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
        bool ValidatePathsAndViewport(const std::string& root,
                                      const std::vector<std::string>& layers,
                                      uint32_t width, uint32_t height, std::string* error)
        {
            if (!PathValid(root)) return Fail(error, "A writable app-owned root path is required.");
            if (width < 640 || height < 480 || width > 3840 || height > 2160)
                return Fail(error, "Desktop dimensions are outside the supported launch range.");
            for (size_t index = 0; index < layers.size(); ++index)
            {
                if (!PathValid(layers[index]))
                    return Fail(error, "A rootfs layer path is empty or contains a control character.");
                for (size_t previous = 0; previous < index; ++previous)
                    if (layers[previous] == layers[index])
                        return Fail(error, "Rootfs layers must be distinct.");
            }
            return true;
        }
        BootRequest DescribeBoot(BootKind kind, const std::string& root,
                                 const std::vector<std::string>& layers,
                                 uint32_t width, uint32_t height)
        {
            BootRequest request;
            request.kind = kind;
            request.backendRevision = BackendRevision;
            request.writableRoot = root;
            request.rootfsLayers = layers;
            request.width = width;
            request.height = height;
            // Matches rootfs/assembly-plan.json. Disable unavailable Gecko/Mono
            // during both initial wineboot and later desktop launches.
            request.environment = {"HOME=/home/username", "USER=username", "WINEARCH=win64",
                "WINEPREFIX=/home/username/.wine", "WINELOADER=/usr/lib/wine/wine64",
                "WINESERVER=/usr/lib/wine/wineserver64",
                "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine", "DISPLAY=:0",
                "WINEDLLOVERRIDES=mscoree,mshtml="};
            return request;
        }
    }
    bool BuildDesktopBootRequest(const std::string& root, const std::vector<std::string>& layers,
                                 uint32_t width, uint32_t height,
                                 BootRequest* result, std::string* error)
    {
        if (!result) return Fail(error, "A boot request output is required.");
        if (layers.size() != 3) return Fail(error, "Exactly glibc, Wine64 and prepared-prefix layers are required.");
        if (!ValidatePathsAndViewport(root, layers, width, height, error)) return false;
        auto request = DescribeBoot(BootKind::ExplorerDesktop, root, layers, width, height);
        request.guestArgv = {"/usr/lib/wine/wine64", "explorer.exe",
            "/desktop=ProtonDesktop," + std::to_string(width) + "x" + std::to_string(height)};
        *result = std::move(request);
        if (error) error->clear();
        return true;
    }
    bool BuildPrefixBootRequest(const WritableOverlayDescriptor& overlay,
                                const std::vector<std::string>& layers,
                                uint32_t width, uint32_t height,
                                BootRequest* result, std::string* error)
    {
        if (!result) return Fail(error, "A boot request output is required.");
        if (!overlay.appOwned) return Fail(error, "Prefix initialization requires an app-owned writable overlay descriptor.");
        if (!overlay.initiallyEmpty) return Fail(error, "Prefix initialization requires an initially empty writable overlay descriptor.");
        if (layers.size() != 2) return Fail(error, "Prefix initialization requires exactly glibc and Wine64 base layers.");
        if (!ValidatePathsAndViewport(overlay.rootPath, layers, width, height, error)) return false;
        auto request = DescribeBoot(BootKind::PrefixInitialization, overlay.rootPath, layers, width, height);
        request.guestArgv = {"/usr/lib/wine/wine64", "wineboot.exe", "--init"};
        *result = std::move(request);
        if (error) error->clear();
        return true;
    }
}
