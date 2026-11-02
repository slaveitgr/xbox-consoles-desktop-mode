// SPDX-License-Identifier: GPL-2.0-only
#include "core/BootRequest.h"
#include "backend/NativeBackend.h"
#include <algorithm>
#include <iostream>

using namespace protondesktop;
int main()
{
    unsigned checks = 0, failures = 0;
    auto check = [&](bool value) { ++checks; if (!value) ++failures; };
    const std::vector<std::string> layers = {"glibc-rootfs64.zip", "wine64.zip", "prefix64.zip"};
    BootRequest request;
    std::string error;
    check(BuildDesktopBootRequest("app-local/session", layers, 1920, 1080, &request, &error));
    check(request.backendRevision == BackendRevision && error.empty());
    check(request.kind == BootKind::ExplorerDesktop);
    check(request.guestArgv == std::vector<std::string>({"/usr/lib/wine/wine64", "explorer.exe", "/desktop=ProtonDesktop,1920x1080"}));
    check(std::find(request.environment.begin(), request.environment.end(), "WINESERVER=/usr/lib/wine/wineserver64") != request.environment.end());
    check(request.rootfsLayers == layers && request.writableRoot == "app-local/session");
    const auto original = request.guestArgv;
    check(!BuildDesktopBootRequest("", layers, 1920, 1080, &request, &error) && !error.empty());
    check(request.guestArgv == original); // Failure cannot publish a partial plan.
    check(!BuildDesktopBootRequest(std::string("root\0escape", 11), layers, 1920, 1080, &request, &error));
    check(!BuildDesktopBootRequest("root\nextra", layers, 1920, 1080, &request, &error));
    check(!BuildDesktopBootRequest("root", layers, 0, 1080, &request, &error));
    check(!BuildDesktopBootRequest("root", layers, 1920, 99999, &request, &error));
    check(!BuildDesktopBootRequest("root", {"a", "b"}, 1920, 1080, &request, &error));
    check(!BuildDesktopBootRequest("root", {"a", "a", "c"}, 1920, 1080, &request, &error));
    check(!BuildDesktopBootRequest("root", {"a", "", "c"}, 1920, 1080, &request, &error));
    check(!BuildDesktopBootRequest("root", layers, 1920, 1080, nullptr, &error));
    const std::vector<std::string> baseLayers = {"glibc-rootfs64.zip", "wine64.zip"};
    const WritableOverlayDescriptor emptyOverlay{"app-local/fresh-prefix", true, true};
    check(BuildPrefixBootRequest(emptyOverlay, baseLayers, 1920, 1080, &request, &error));
    check(request.kind == BootKind::PrefixInitialization && error.empty());
    check(request.backendRevision == BackendRevision && request.writableRoot == emptyOverlay.rootPath);
    check(request.rootfsLayers == baseLayers && request.rootfsLayers.size() == 2);
    check(request.guestArgv == std::vector<std::string>({"/usr/lib/wine/wine64", "wineboot.exe", "--init"}));
    check(request.width == 1920 && request.height == 1080);
    const std::vector<std::string> firstBootEnvironment = {
        "HOME=/home/username", "USER=username", "WINEARCH=win64",
        "WINEPREFIX=/home/username/.wine", "WINELOADER=/usr/lib/wine/wine64",
        "WINESERVER=/usr/lib/wine/wineserver64", "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine",
        "DISPLAY=:0", "WINEDLLOVERRIDES=mscoree,mshtml="};
    check(request.environment == firstBootEnvironment);
    const auto sameRequest = [](const BootRequest& left, const BootRequest& right)
    {
        return left.kind == right.kind && left.backendRevision == right.backendRevision &&
            left.writableRoot == right.writableRoot && left.rootfsLayers == right.rootfsLayers &&
            left.environment == right.environment && left.guestArgv == right.guestArgv &&
            left.width == right.width && left.height == right.height;
    };
    const auto rejectPrefix = [&](const WritableOverlayDescriptor& overlay,
                                  const std::vector<std::string>& bases,
                                  uint32_t width = 1920, uint32_t height = 1080)
    {
        const auto before = request;
        error.clear();
        check(!BuildPrefixBootRequest(overlay, bases, width, height, &request, &error) && !error.empty());
        check(sameRequest(request, before));
    };
    rejectPrefix({"app-local/fresh-prefix", false, true}, baseLayers);
    rejectPrefix({"app-local/existing-prefix", true, false}, baseLayers);
    rejectPrefix({"", true, true}, baseLayers);
    rejectPrefix({std::string("root\0escape", 11), true, true}, baseLayers);
    rejectPrefix({"root\nextra", true, true}, baseLayers);
    rejectPrefix(emptyOverlay, layers); // Prepared-prefix input belongs to desktop boot.
    rejectPrefix(emptyOverlay, {});
    rejectPrefix(emptyOverlay, {"wine64.zip"});
    rejectPrefix(emptyOverlay, {"glibc-rootfs64.zip", "glibc-rootfs64.zip"});
    rejectPrefix(emptyOverlay, {"glibc-rootfs64.zip", ""});
    rejectPrefix(emptyOverlay, {"glibc-rootfs64.zip", "wine64.zip\runsafe"});
    rejectPrefix(emptyOverlay, baseLayers, 639, 1080);
    rejectPrefix(emptyOverlay, baseLayers, 1920, 2161);
    check(!BuildPrefixBootRequest(emptyOverlay, baseLayers, 1920, 1080, nullptr, &error));
    check(!BuildPrefixBootRequest({}, baseLayers, 1920, 1080, &request, nullptr));
    const auto beforeDesktopFailure = request;
    check(!BuildDesktopBootRequest(emptyOverlay.rootPath, baseLayers, 1920, 1080, &request, &error));
    check(sameRequest(request, beforeDesktopFailure));
    check(BuildDesktopBootRequest("app-local/session", layers, 1920, 1080, &request, &error));
    check(request.kind == BootKind::ExplorerDesktop && request.rootfsLayers == layers);
    check(request.guestArgv == std::vector<std::string>({"/usr/lib/wine/wine64", "explorer.exe", "/desktop=ProtonDesktop,1920x1080"}));
    check(request.environment == firstBootEnvironment);
    check(BuildPrefixBootRequest(emptyOverlay, baseLayers, 640, 480, &request, nullptr));
    check(request.kind == BootKind::PrefixInitialization && request.width == 640 && request.height == 480);
    check(BuildPrefixBootRequest(emptyOverlay, baseLayers, 3840, 2160, &request, &error));
    check(request.width == 3840 && request.height == 2160 && error.empty());
    check(!CreateNativeBackend());
    check(std::string(NativeBackendStatus()).find("not linked") != std::string::npos);
    std::cout << "ProtonDesktop boot contract: " << checks - failures << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
