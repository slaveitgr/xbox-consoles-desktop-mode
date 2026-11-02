// SPDX-License-Identifier: GPL-2.0-only
#ifndef PROTON_SDL_VIDEO_HOST_INTERNAL_H
#define PROTON_SDL_VIDEO_HOST_INTERNAL_H
#include "ProtonSdlVideo.h"
#ifdef __cplusplus
extern "C" {
#endif
int ProtonSdlHostConfigured(void);
int ProtonSdlHostAcquire(ProtonSdlDisplay* display);
int ProtonSdlHostReadDisplay(ProtonSdlDisplay* display);
int ProtonSdlHostPresent(const ProtonSdlFrame* frame);
void ProtonSdlHostRelease(void);
void ProtonSdlHostFail(const char* diagnostic);
#ifdef __cplusplus
}
#endif
#endif
