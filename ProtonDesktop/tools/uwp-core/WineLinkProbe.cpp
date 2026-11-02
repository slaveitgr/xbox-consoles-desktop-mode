// SPDX-License-Identifier: GPL-2.0-only
// Link qualification only. This console entry is never executed by CI or Xbox.
#include "backend/runtime/WineSession.h"
#include <atomic>
#include <memory>

int main(int argc, char** argv)
{
    if (argc < 4) return 2;
    protondesktop::runtime::SessionRequest request;
    request.rootPath = argv[1];
    request.zipLayers = {argv[2], argv[3]};
    request.argv = {"/usr/lib/wine/wine64", "explorer.exe", "/desktop=shell,1920x1080",
        "explorer.exe", "C:\\"};
    const auto stop = std::make_shared<std::atomic<bool>>(false);
    const auto result = protondesktop::runtime::RunWineSession(request, {}, stop);
    return result.error.empty() ? 0 : 1;
}
