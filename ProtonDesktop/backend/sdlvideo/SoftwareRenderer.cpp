// SPDX-License-Identifier: GPL-2.0-only
#include "ProtonSdlVideo.h"
#include <SDL.h>
extern "C" SDL_Renderer* ProtonSdlVideoCreateSoftwareRenderer(SDL_Window* window)
{
    if (!SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_OVERRIDE) ||
        !SDL_SetHintWithPriority(SDL_HINT_RENDER_VSYNC, "0", SDL_HINT_OVERRIDE) ||
        !SDL_SetHintWithPriority(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0", SDL_HINT_OVERRIDE)) {
        SDL_SetError("Unable to select the real SDL software framebuffer renderer");
        return nullptr;
    }
    return SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
}
