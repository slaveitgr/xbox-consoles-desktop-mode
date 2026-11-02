// SPDX-License-Identifier: GPL-2.0-only
#ifndef PROTON_SDL_VIDEO_H
#define PROTON_SDL_VIDEO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PROTON_SDL_VIDEO_ABI 1u
#define PROTON_SDL_VIDEO_MAX_DIMENSION 8192u

typedef struct ProtonSdlDisplay {
    uint32_t width, height;
    uint32_t refresh_hz; /* Zero means the host does not know the refresh rate. */
    float dpi;
} ProtonSdlDisplay;

typedef struct ProtonSdlFrame {
    const uint8_t* bgra;
    uint32_t width, height, stride;
    /* ARGB8888 on little-endian x64: memory bytes B,G,R,A. The host may ignore
       alpha for X11 BGRX images. Pixels expire when present() returns. */
} ProtonSdlFrame;

typedef struct ProtonSdlHost {
    uint32_t abi_version;
    void* userdata;
    /* Both callbacks execute on the SDL owner thread. Geometry must come from
       a real host snapshot; callbacks must not access CoreWindow off-thread. */
    int (*query_display)(void*, ProtonSdlDisplay*);
    /* Return 1 only after copying the complete frame into host-owned storage.
       Returning zero or throwing latches a failure; no display is fabricated. */
    int (*present)(void*, const ProtonSdlFrame*);
} ProtonSdlHost;

/* Configure before SDL video init; keep userdata/callbacks alive until SDL
   video quit, then Reset. Reconfiguration/reset while active is rejected.
   Successful Configure clears the previous inactive session's error latch. */
int ProtonSdlVideoConfigure(const ProtonSdlHost* host);
int ProtonSdlVideoReset(void);
/* Thread-safe persistent error latch. Returns 1 when failed; optionally copies
   the first diagnostic, NUL terminated when capacity > 0. No implicit reset. */
int ProtonSdlVideoCopyError(char* destination, size_t capacity);

struct SDL_Window;
struct SDL_Renderer;
/* Actual software SDL renderer; explicitly disables VSYNC and texture-backed
   window-framebuffer recursion. This does not create a host GPU presenter. */
struct SDL_Renderer* ProtonSdlVideoCreateSoftwareRenderer(struct SDL_Window* window);

#ifdef __cplusplus
}
#endif
#endif
