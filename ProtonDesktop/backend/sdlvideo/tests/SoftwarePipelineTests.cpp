// SPDX-License-Identifier: GPL-2.0-only
// SELF-AUTHORED SDL TEXTURE TEST. This is not Wine, X11 guest execution or an
// Xbox/CoreWindow display test; the native test host acknowledges owned copies.
#include "ProtonSdlVideo.h"
#include <SDL.h>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(std::string(message) + " SDL: " + SDL_GetError());
}
struct Host {
    ProtonSdlDisplay display{64, 48, 75, 110.25f};
    bool declineGeometry = false, throwGeometry = false;
    bool declineFrame = false, throwFrame = false;
    unsigned queries = 0, attempts = 0, accepted = 0;
    uint32_t width = 0, height = 0, stride = 0;
    std::vector<uint8_t> copy;
    static int Query(void* userdata, ProtonSdlDisplay* result)
    {
        auto& host = *static_cast<Host*>(userdata);
        ++host.queries;
        if (host.throwGeometry) throw std::runtime_error("test geometry failure");
        if (host.declineGeometry) return 0;
        *result = host.display;
        return 1;
    }
    static int Present(void* userdata, const ProtonSdlFrame* frame)
    {
        auto& host = *static_cast<Host*>(userdata);
        ++host.attempts;
        if (host.throwFrame) throw std::runtime_error("test copying callback failure");
        if (host.declineFrame) return 0;
        // The host acknowledges only after copying; retain no guest/SDL pointer.
        std::vector<uint8_t> owned(static_cast<size_t>(frame->width) * frame->height * 4);
        for (uint32_t y = 0; y < frame->height; ++y)
            std::memcpy(owned.data() + static_cast<size_t>(y) * frame->width * 4,
                        frame->bgra + static_cast<size_t>(y) * frame->stride, frame->width * 4);
        host.width = frame->width;
        host.height = frame->height;
        host.stride = frame->stride;
        host.copy.swap(owned);
        ++host.accepted;
        return 1;
    }
    ProtonSdlHost Callbacks() { return {PROTON_SDL_VIDEO_ABI, this, Query, Present}; }
};
struct Session {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    ~Session()
    {
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        ProtonSdlVideoReset();
    }
    void Open(Host& host)
    {
        const auto callbacks = host.Callbacks();
        Check(ProtonSdlVideoConfigure(&callbacks) == 1, "configure explicit test host");
        Check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) == 0, "initialize actual SDL video");
        Check(std::strcmp(SDL_GetCurrentVideoDriver(), "proton-uwp") == 0, "custom driver must be selected");
    }
    void CreateWindow()
    {
        window = SDL_CreateWindow("SELF-AUTHORED SDL PIPELINE TEST (NOT WINE)", 0, 0, 11, 5, SDL_WINDOW_HIDDEN);
        Check(window != nullptr, "create actual framebuffer window");
        renderer = ProtonSdlVideoCreateSoftwareRenderer(window);
        Check(renderer != nullptr, "create genuine SDL software renderer");
        SDL_RendererInfo info{};
        Check(SDL_GetRendererInfo(renderer, &info) == 0, "query actual renderer");
        Check(std::strcmp(info.name, "software") == 0, "renderer must be genuine software");
        Check((info.flags & SDL_RENDERER_SOFTWARE) != 0, "software flag");
        Check(!(info.flags & (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)), "no accelerated/vsync claims");
        Check(std::strcmp(SDL_GetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION), "0") == 0, "no recursive framebuffer renderer");
        SDL_ShowWindow(window);
    }
};
uint32_t TexturePixel(unsigned x, unsigned y)
{
    return 0xff000000u | ((17u + x * 31u) << 16) | ((23u + y * 53u) << 8) | (x * 7u + y * 11u);
}
void CheckPixels(const Host& host, uint32_t width, uint32_t height, uint32_t clear)
{
    Check(host.width == width && host.height == height, "presented dimensions follow software surface");
    Check(host.stride >= width * 4, "presented pitch fits full rows");
    Check(host.copy.size() == static_cast<size_t>(width) * height * 4, "host owns complete pixel copy");
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t actual = 0;
            std::memcpy(&actual, host.copy.data() + (static_cast<size_t>(y) * width + x) * 4, 4);
            const uint32_t expected = x >= 2 && x < 7 && y >= 1 && y < 4 ? TexturePixel(x - 2, y - 1) : clear;
            Check(actual == expected, "actual software-rendered pixel differs from independent texture/clear expectation");
        }
    }
}
void Draw(Session& session, uint8_t red, uint8_t green, uint8_t blue)
{
    Check(SDL_SetRenderDrawColor(session.renderer, red, green, blue, 255) == 0, "set clear color");
    Check(SDL_RenderClear(session.renderer) == 0, "actual software clear");
    SDL_Rect destination{2, 1, 5, 3};
    Check(SDL_RenderCopy(session.renderer, session.texture, nullptr, &destination) == 0, "actual SDL texture copy");
    SDL_RenderPresent(session.renderer);
}
void TestActualTextureAndResize()
{
    Host host;
    Session session;
    session.Open(host);
    SDL_DisplayMode desktop{};
    Check(SDL_GetDesktopDisplayMode(0, &desktop) == 0, "query real host-provided geometry");
    Check(desktop.w == 64 && desktop.h == 48 && desktop.refresh_rate == 75, "no dummy desktop mode");
    float dpi = 0;
    Check(SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr) == 0 && dpi == 110.25f, "host-provided DPI");
    session.CreateWindow();
    Check(SDL_GetKeyboardFocus() == nullptr, "driver must not manufacture keyboard focus");
    Check(SDL_GetMouseFocus() == nullptr, "driver must not manufacture mouse focus");
    session.texture = SDL_CreateTexture(session.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 5, 3);
    Check(session.texture != nullptr, "create genuine streaming texture");
    std::array<uint32_t, 15> pixels{};
    for (unsigned y = 0; y < 3; ++y)
        for (unsigned x = 0; x < 5; ++x) pixels[y * 5 + x] = TexturePixel(x, y);
    Check(SDL_UpdateTexture(session.texture, nullptr, pixels.data(), 5 * 4) == 0, "upload self-authored texture");
    Draw(session, 29, 45, 61);
    Check(host.accepted == 1, "one frame actually acknowledged by copying host");
    CheckPixels(host, 11, 5, 0xff1d2d3d);
    const auto retained = host.copy;
    SDL_Surface* surface = SDL_GetWindowSurface(session.window);
    Check(surface != nullptr, "read actual driver surface");
    Check(SDL_FillRect(surface, nullptr, 0x11223344) == 0, "mutate source after host callback");
    Check(host.copy == retained, "host copy must survive mutation of source pixels");
    SDL_SetWindowSize(session.window, 17, 9);
    Check(SDL_RenderSetViewport(session.renderer, nullptr) == 0, "reset viewport after resize");
    Draw(session, 73, 89, 101);
    Check(host.accepted == 2, "resized surface actually reaches host");
    CheckPixels(host, 17, 9, 0xff495965);
    host.display = {80, 60, 60, 96.0f};
    SDL_PumpEvents();
    Check(SDL_GetDesktopDisplayMode(0, &desktop) == 0 && desktop.w == 80 && desktop.h == 60,
          "display pump updates actual changed host geometry");
    Check(ProtonSdlVideoCopyError(nullptr, 0) == 0, "successful real pipeline has no latched error");
    Check(SDL_CreateWindow("unsupported second window", 0, 0, 2, 2, SDL_WINDOW_HIDDEN) == nullptr,
          "do not invent another host window");
    Check(SDL_CreateWindow("unsupported GL", 0, 0, 2, 2, SDL_WINDOW_OPENGL) == nullptr, "reject GL");
    Check(SDL_CreateWindow("unsupported Vulkan", 0, 0, 2, 2, SDL_WINDOW_VULKAN) == nullptr, "reject Vulkan");
}
void TestHostFailures()
{
    SDL_Quit();
    ProtonSdlVideoReset();
    Check(SDL_Init(SDL_INIT_VIDEO) < 0, "absent host must fail video initialization");
    Check(ProtonSdlVideoCopyError(nullptr, 0) == 1, "absent host latches diagnostic");
    SDL_Quit();
    for (unsigned mode = 0; mode < 4; ++mode) {
        Host host;
        host.declineGeometry = mode == 0;
        host.throwGeometry = mode == 1;
        if (mode == 2) host.display.width = 0;
        if (mode == 3) host.display.dpi = std::numeric_limits<float>::quiet_NaN();
        Session session;
        const auto callbacks = host.Callbacks();
        Check(ProtonSdlVideoConfigure(&callbacks) == 1, "register failure-fixture host");
        Check(SDL_Init(SDL_INIT_VIDEO) < 0, "invalid/declined/throwing geometry must fail initialization");
        Check(ProtonSdlVideoCopyError(nullptr, 0) == 1, "geometry failure is latched");
    }
    for (const bool throws : {false, true}) {
        Host host;
        Session session;
        session.Open(host);
        session.CreateWindow();
        host.declineFrame = !throws;
        host.throwFrame = throws;
        Check(SDL_SetRenderDrawColor(session.renderer, 1, 2, 3, 255) == 0, "queue failing frame");
        Check(SDL_RenderClear(session.renderer) == 0, "render failing frame genuinely");
        SDL_RenderPresent(session.renderer);
        Check(host.attempts == 1 && host.accepted == 0, "declined frame is never counted accepted");
        std::array<char, 256> error{};
        Check(ProtonSdlVideoCopyError(error.data(), error.size()) == 1 && error[0], "present failure survives SDL void RenderPresent");
        SDL_RenderPresent(session.renderer);
        Check(host.attempts == 1, "failed session cannot keep invoking host");
        std::array<char, 5> shortError{};
        Check(ProtonSdlVideoCopyError(shortError.data(), shortError.size()) == 1 && shortError.back() == '\0',
              "truncated diagnostic is safely terminated");
    }
}
}
int main()
{
    try {
        SDL_SetMainReady();
        Check(SDL_setenv("SDL_VIDEODRIVER", "proton-uwp", 1) == 0, "select custom driver explicitly");
        TestHostFailures();
        TestActualTextureAndResize();
        Host host;
        {
            Session session;
            session.Open(host);
            Check(ProtonSdlVideoReset() == 0, "active host cannot be reset/freed");
        }
        Check(ProtonSdlVideoReset() == 1, "inactive host can be reset");
        std::cout << checks << " actual SDL software pipeline checks passed; SELF-AUTHORED TEXTURE, NOT WINE\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SDL software pipeline failed: " << error.what() << '\n';
        SDL_Quit();
        ProtonSdlVideoReset();
        return 1;
    }
}
