// SPDX-License-Identifier: GPL-2.0-only
// Deliberately limited interpreter profile, not a renderer or a JIT backend.
#include "boxedwine.h"
#include "pixelformat.h"
#include <stdexcept>

#ifndef PROTONDESKTOP_UWP_PROFILE
#error Select the explicit ProtonDesktop software-interpreter profile.
#endif
#if defined(BOXEDWINE_JIT) || defined(BOXEDWINE_JIT_X64) || defined(BOXEDWINE_JIT_ARMV8)
#error This profile never permits executable host code.
#endif
static_assert(sizeof(PixelFormat) == 40, "Pinned pixel-format ABI changed");

int getPixelFormats(PixelFormat* formats, int capacity)
{
    // Upstream uses one-based format indices, with index zero reserved.
    if (!formats || capacity < 2) return 0;
    formats[0] = {};
    PixelFormat bgra = {};
    bgra.nSize = sizeof(PixelFormat);
    bgra.nVersion = 1;
    // CPU bitmap layout only. No GL, GDI host, window presentation, hardware
    // acceleration, double buffering, depth or stencil capability is claimed.
    bgra.dwFlags = K_PFD_DRAW_TO_BITMAP | K_PFD_GENERIC_FORMAT;
    bgra.iPixelType = K_PFD_TYPE_RGBA;
    bgra.cColorBits = 32;
    bgra.cRedBits = bgra.cGreenBits = bgra.cBlueBits = bgra.cAlphaBits = 8;
    bgra.cRedShift = 16;
    bgra.cGreenShift = 8;
    bgra.cBlueShift = 0;
    bgra.cAlphaShift = 24;
    bgra.iLayerType = K_PFD_MAIN_PLANE;
    formats[1] = bgra;
    return 2;
}

void Platform::writeCodeToMemory(void*, U32, std::function<void()>)
{
    // Do not execute the callback, dereference the address, or relax host page
    // permissions. Guest executable pages are data interpreted by CPU64.
    throw std::runtime_error("ProtonDesktop UWP interpreter: host code-memory writes are unsupported");
}
