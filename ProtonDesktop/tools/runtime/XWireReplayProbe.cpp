// SPDX-License-Identifier: GPL-2.0-only
// Static self-authored scene through the genuine SDL presenter, no Wine/rootfs.
#include "boxedwine.h"
#include "xwireserver.h"
#include "xwirepresent.h"
#include "knativesystem.h"
#include "knativeinput.h"
#include <SDL.h>
#include <cstdio>
#include <vector>

void protonConfigureXWireDesktop(unsigned, unsigned);
bool doMainLoop();
static unsigned frameHooks = 0;
static unsigned failures = 0;
void protonWineGuestFrameSubmitted(unsigned, unsigned) { ++frameHooks; }
static void check(bool ok, const char* label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

int main() {
    Platform::init();
    if (!KNativeSystem::init(VIDEO_NORMAL, false)) return 64;
    KSystem::init();
    KSystem::videoOption = VIDEO_NORMAL;
    // The actual main-loop entry binds this thread even with no guest workers.
    doMainLoop();
    KNativeSystem::initWindow(640, 480, 32, 100, 100, B("nearest"), 0, 0);
    auto screen = KNativeSystem::getScreen();
    if (!screen) return 65;
    protonConfigureXWireDesktop(640, 480);
    auto& server = XWireServer::instance();
    auto& root = server.windows[0x260];
    root.isRoot = root.mapped = true;
    root.width = 640; root.height = 480;
    server.presentWindow = 0x260;
    installXWireSink();
    if (!g_xwirePresentSink) return 66;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    for (unsigned id = 1; id < 32 && !renderer; ++id) {
        window = SDL_GetWindowFromID(id);
        if (window) renderer = SDL_GetRenderer(window);
    }
    check(renderer != nullptr, "actual SDL software renderer exists");
    if (!renderer) return 67;
    const auto windowId = SDL_GetWindowID(window);
    auto event = [&](unsigned type) {
        // Remove only this isolated fixture's automatically generated events.
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        SDL_Event e{}; e.type = SDL_WINDOWEVENT;
        e.window.windowID = windowId; e.window.event = static_cast<Uint8>(type);
        check(SDL_PushEvent(&e) == 1, "actual SDL window event queued");
        check(KNativeSystem::getCurrentInput()->processEvents(), "sole native SDL pump forwards event");
    };
    g_xwirePresentSink->onWindowMapped(0x260, 640, 480);
    event(SDL_WINDOWEVENT_SHOWN);
    screen->presentedSinceLastCheck();
    g_xwirePresentSink->tickMainThread();
    check(frameHooks == 0 && !screen->presentedSinceLastCheck(), "window event cannot fabricate a guest frame");

    constexpr U32 pixel = 0xff2468ac;
    std::vector<U32> source(640 * 480, pixel);
    g_xwirePresentSink->submitFrame(0x260, 640, 480,
                                   reinterpret_cast<const U8*>(source.data()), 640 * 4);
    auto series = [&](const char* name) {
        const auto before = frameHooks;
        for (unsigned tick = 1; tick <= 3; ++tick) {
            screen->presentedSinceLastCheck();
            g_xwirePresentSink->tickMainThread();
            check(frameHooks == before + tick && screen->presentedSinceLastCheck(), name);
            U32 actual = 0;
            SDL_Rect point{100, 100, 1, 1};
            check(SDL_RenderReadPixels(renderer, &point, SDL_PIXELFORMAT_ARGB8888, &actual, 4) == 0 &&
                  actual == pixel, "actual SDL renderer retains the submitted pixel");
        }
        screen->presentedSinceLastCheck();
        g_xwirePresentSink->tickMainThread();
        check(frameHooks == before + 3 && !screen->presentedSinceLastCheck(),
              "finite replay stops without another frame copy/present");
    };
    series("static first frame is presented on three ticks without resubmission");
    for (unsigned type : {SDL_WINDOWEVENT_SHOWN, SDL_WINDOWEVENT_RESTORED,
                          SDL_WINDOWEVENT_EXPOSED, SDL_WINDOWEVENT_SIZE_CHANGED}) {
        event(type);
        series("window visibility/size event replays retained genuine frame three times");
    }
    const auto before = frameHooks;
    g_xwirePresentSink->submitFrame(0x260, 640, 480,
                                   reinterpret_cast<const U8*>(source.data()), 640 * 4);
    g_xwirePresentSink->tickMainThread();
    check(frameHooks == before + 1, "ordinary same-size scene update presents once");
    screen->presentedSinceLastCheck();
    g_xwirePresentSink->tickMainThread();
    check(frameHooks == before + 1 && !screen->presentedSinceLastCheck(),
          "ordinary update does not start continuous idle rendering");
    g_xwirePresentSink = nullptr;
    KSystem::destroy();
    KNativeSystem::shutdown(); KNativeSystem::cleanup();
    std::printf("REAL_SDL_REPLAY_RESULT failed=%u hooks=%u\n", failures, frameHooks);
    return failures ? 1 : 0;
}
