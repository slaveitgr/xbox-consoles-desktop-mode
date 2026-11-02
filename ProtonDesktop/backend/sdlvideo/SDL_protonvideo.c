/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2020 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  ProtonDesktop alteration: actual host-backed software framebuffer driver.
  Allocation/lifetime derived from SDL 2.0.12's dummy framebuffer by Sam
  Lantinga and contributors. No dummy geometry, discarded display updates,
  desktop window API, simulated focus or input device is supplied here.
*/
#include "../../SDL_internal.h"
#if SDL_VIDEO_DRIVER_PROTON_UWP
#include "../SDL_sysvideo.h"
#include "ProtonSdlVideoHostInternal.h"

#if SDL_BYTEORDER != SDL_LIL_ENDIAN
#error The ProtonDesktop frame ABI is little-endian BGRA.
#endif

typedef struct ProtonVideoData {
    ProtonSdlDisplay display;
    SDL_Window* window;
} ProtonVideoData;
typedef struct ProtonWindowData {
    SDL_Surface* framebuffer;
    int width, height;
} ProtonWindowData;

static int HostError(void)
{
    char diagnostic[256];
    ProtonSdlVideoCopyError(diagnostic, sizeof(diagnostic));
    return SDL_SetError("%s", diagnostic[0] ? diagnostic : "ProtonDesktop SDL host failed");
}
static SDL_bool ValidSize(int width, int height)
{
    return width > 0 && height > 0 &&
        width <= PROTON_SDL_VIDEO_MAX_DIMENSION && height <= PROTON_SDL_VIDEO_MAX_DIMENSION;
}
static int PROTON_Available(void) { return ProtonSdlHostConfigured(); }

static int PROTON_VideoInit(_THIS)
{
    ProtonVideoData* data = (ProtonVideoData*)_this->driverdata;
    SDL_DisplayMode mode;
    if (!ProtonSdlHostAcquire(&data->display)) return HostError();
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.w = (int)data->display.width;
    mode.h = (int)data->display.height;
    mode.refresh_rate = (int)data->display.refresh_hz;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        ProtonSdlHostRelease();
        return -1;
    }
    SDL_AddDisplayMode(&_this->displays[0], &mode);
    return 0;
}
static void PROTON_VideoQuit(_THIS) { ProtonSdlHostRelease(); }

static void PROTON_PumpEvents(_THIS)
{
    /* This driver does not claim input support. Poll only actual host display
       geometry; input must later arrive through an explicit native queue and
       the existing single SDL event consumer. */
    ProtonVideoData* data = (ProtonVideoData*)_this->driverdata;
    ProtonSdlDisplay actual;
    if (!ProtonSdlHostReadDisplay(&actual)) { HostError(); return; }
    data->display = actual;
    _this->displays[0].desktop_mode.w = (int)actual.width;
    _this->displays[0].desktop_mode.h = (int)actual.height;
    _this->displays[0].desktop_mode.refresh_rate = (int)actual.refresh_hz;
    _this->displays[0].current_mode = _this->displays[0].desktop_mode;
    if (_this->displays[0].num_display_modes)
        _this->displays[0].display_modes[0] = _this->displays[0].desktop_mode;
}
static int PROTON_GetDisplayBounds(_THIS, SDL_VideoDisplay* display, SDL_Rect* rect)
{
    ProtonVideoData* data = (ProtonVideoData*)_this->driverdata;
    rect->x = rect->y = 0;
    rect->w = (int)data->display.width;
    rect->h = (int)data->display.height;
    return 0;
}
static int PROTON_GetDisplayDPI(_THIS, SDL_VideoDisplay* display, float* diagonal, float* horizontal, float* vertical)
{
    ProtonVideoData* data = (ProtonVideoData*)_this->driverdata;
    if (diagonal) *diagonal = data->display.dpi;
    if (horizontal) *horizontal = data->display.dpi;
    if (vertical) *vertical = data->display.dpi;
    return 0;
}
static int PROTON_SetDisplayMode(_THIS, SDL_VideoDisplay* display, SDL_DisplayMode* mode)
{
    ProtonVideoData* data = (ProtonVideoData*)_this->driverdata;
    if (mode->format != SDL_PIXELFORMAT_ARGB8888 ||
        mode->w != (int)data->display.width || mode->h != (int)data->display.height ||
        (mode->refresh_rate && mode->refresh_rate != (int)data->display.refresh_hz))
        return SDL_SetError("The host owns its display mode; exclusive mode switching is unsupported");
    return 0;
}
static int PROTON_CreateWindow(_THIS, SDL_Window* window)
{
    ProtonVideoData* data = (ProtonVideoData*)_this->driverdata;
    ProtonWindowData* window_data;
    if (window->flags & (SDL_WINDOW_OPENGL | SDL_WINDOW_VULKAN))
        return SDL_SetError("ProtonDesktop SDL video supports software framebuffers only");
    if (data->window) return SDL_SetError("Only one host-backed SDL framebuffer window is supported");
    if (!ValidSize(window->w, window->h)) return SDL_SetError("Invalid or oversized software framebuffer");
    window_data = (ProtonWindowData*)SDL_calloc(1, sizeof(*window_data));
    if (!window_data) return SDL_OutOfMemory();
    window_data->width = window->w;
    window_data->height = window->h;
    window->driverdata = window_data;
    data->window = window;
    return 0;
}
static void PROTON_SetWindowSize(_THIS, SDL_Window* window)
{
    ProtonWindowData* data = (ProtonWindowData*)window->driverdata;
    if (!ValidSize(window->w, window->h)) {
        window->w = data->width;
        window->h = data->height;
        ProtonSdlHostFail("Invalid or oversized SDL framebuffer resize");
        HostError();
        return;
    }
    data->width = window->w;
    data->height = window->h;
}
static int PROTON_CreateFramebuffer(_THIS, SDL_Window* window, Uint32* format, void** pixels, int* pitch)
{
    ProtonWindowData* data = (ProtonWindowData*)window->driverdata;
    SDL_Surface* surface;
    if (!data || !ValidSize(window->w, window->h)) return SDL_SetError("Invalid software framebuffer window");
    surface = SDL_CreateRGBSurfaceWithFormat(0, window->w, window->h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return -1;
    SDL_FreeSurface(data->framebuffer);
    data->framebuffer = surface;
    *format = SDL_PIXELFORMAT_ARGB8888;
    *pixels = surface->pixels;
    *pitch = surface->pitch;
    return 0;
}
static int PROTON_UpdateFramebuffer(_THIS, SDL_Window* window, const SDL_Rect* rects, int count)
{
    ProtonWindowData* data = (ProtonWindowData*)window->driverdata;
    ProtonSdlFrame frame;
    if (!data || !data->framebuffer) return SDL_SetError("No software framebuffer exists");
    if (count <= 0) return count == 0 ? 0 : SDL_SetError("Invalid dirty rectangle count");
    if (!rects) return SDL_SetError("Missing dirty rectangles");
    frame.bgra = (const uint8_t*)data->framebuffer->pixels;
    frame.width = (uint32_t)data->framebuffer->w;
    frame.height = (uint32_t)data->framebuffer->h;
    frame.stride = (uint32_t)data->framebuffer->pitch;
    if (!ProtonSdlHostPresent(&frame)) return HostError();
    return 0;
}
static void PROTON_DestroyFramebuffer(_THIS, SDL_Window* window)
{
    ProtonWindowData* data = (ProtonWindowData*)window->driverdata;
    if (data) { SDL_FreeSurface(data->framebuffer); data->framebuffer = NULL; }
}
static void PROTON_DestroyWindow(_THIS, SDL_Window* window)
{
    ProtonVideoData* data = (ProtonVideoData*)_this->driverdata;
    if (data->window == window) data->window = NULL;
    PROTON_DestroyFramebuffer(_this, window);
    SDL_free(window->driverdata);
    window->driverdata = NULL;
}
static void PROTON_DeleteDevice(SDL_VideoDevice* device)
{
    SDL_free(device->driverdata);
    SDL_free(device);
}
static SDL_VideoDevice* PROTON_CreateDevice(int index)
{
    SDL_VideoDevice* device = (SDL_VideoDevice*)SDL_calloc(1, sizeof(*device));
    if (!device) { SDL_OutOfMemory(); return NULL; }
    device->driverdata = SDL_calloc(1, sizeof(ProtonVideoData));
    if (!device->driverdata) { SDL_free(device); SDL_OutOfMemory(); return NULL; }
    device->VideoInit = PROTON_VideoInit;
    device->VideoQuit = PROTON_VideoQuit;
    device->PumpEvents = PROTON_PumpEvents;
    device->GetDisplayBounds = PROTON_GetDisplayBounds;
    device->GetDisplayUsableBounds = PROTON_GetDisplayBounds;
    device->GetDisplayDPI = PROTON_GetDisplayDPI;
    device->SetDisplayMode = PROTON_SetDisplayMode;
    device->CreateSDLWindow = PROTON_CreateWindow;
    device->SetWindowSize = PROTON_SetWindowSize;
    device->DestroyWindow = PROTON_DestroyWindow;
    device->CreateWindowFramebuffer = PROTON_CreateFramebuffer;
    device->UpdateWindowFramebuffer = PROTON_UpdateFramebuffer;
    device->DestroyWindowFramebuffer = PROTON_DestroyFramebuffer;
    device->free = PROTON_DeleteDevice;
    return device;
}
VideoBootStrap PROTON_UWP_bootstrap = {
    "proton-uwp", "ProtonDesktop host-backed software framebuffer",
    PROTON_Available, PROTON_CreateDevice
};
#endif
