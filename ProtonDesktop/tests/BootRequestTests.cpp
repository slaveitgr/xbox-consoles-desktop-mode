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
    check(!CreateNativeBackend());
    check(std::string(NativeBackendStatus()).find("not linked") != std::string::npos);
    std::cout << "ProtonDesktop boot contract: " << checks - failures << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
