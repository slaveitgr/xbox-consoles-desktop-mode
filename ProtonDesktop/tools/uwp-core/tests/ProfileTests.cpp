// SPDX-License-Identifier: GPL-2.0-only
// Portable behavior tests against real pinned upstream declarations.
// These do not qualify the Windows SDK, AppContainer, renderer or Wine.
#include "boxedwine.h"
#include "pixelformat.h"
#include "knativeaudio.h"
#include <array>
#include <cstring>
#include <stdexcept>

namespace {
unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
}
int main()
{
    try {
        std::array<PixelFormat, 3> formats;
        std::memset(formats.data(), 0xa5, sizeof(formats));
        const auto original = formats;
        Check(getPixelFormats(nullptr, 2) == 0, "null output must be rejected");
        for (const int capacity : {-1, 0, 1}) {
            Check(getPixelFormats(formats.data(), capacity) == 0, "short output must be rejected");
            Check(std::memcmp(formats.data(), original.data(), sizeof(formats)) == 0,
                  "short output must not be modified");
        }
        Check(getPixelFormats(formats.data(), 2) == 2, "one-based format count");
        PixelFormat zero = {};
        Check(std::memcmp(&formats[0], &zero, sizeof(zero)) == 0, "reserved zero index");
        Check(std::memcmp(&formats[2], &original[2], sizeof(PixelFormat)) == 0, "output bound");
        const auto& p = formats[1];
        Check(p.nSize == 40 && p.nVersion == 1, "pixel ABI");
        Check(p.dwFlags == (K_PFD_DRAW_TO_BITMAP | K_PFD_GENERIC_FORMAT), "bitmap-only flags");
        Check(!(p.dwFlags & (K_PFD_SUPPORT_OPENGL | K_PFD_SUPPORT_GDI |
              K_PFD_DRAW_TO_WINDOW | K_PFD_DOUBLEBUFFER | K_PFD_GENERIC_ACCELERATED)),
              "unsupported renderer capabilities must not be advertised");
        Check(p.cColorBits == 32 && p.iPixelType == K_PFD_TYPE_RGBA, "32-bit RGBA components");
        Check(p.cRedBits == 8 && p.cGreenBits == 8 && p.cBlueBits == 8 && p.cAlphaBits == 8,
              "component widths");
        const std::array<U8, 4> bgra{0x13, 0x57, 0x9b, 0xdf};
        U32 packed = 0;
        std::memcpy(&packed, bgra.data(), sizeof(packed));
        Check(((packed >> p.cBlueShift) & 255) == bgra[0], "blue byte layout");
        Check(((packed >> p.cGreenShift) & 255) == bgra[1], "green byte layout");
        Check(((packed >> p.cRedShift) & 255) == bgra[2], "red byte layout");
        Check(((packed >> p.cAlphaShift) & 255) == bgra[3], "alpha byte layout");
        Check(p.cDepthBits == 0 && p.cStencilBits == 0 && p.cAccumBits == 0,
              "no invented depth/stencil/accumulation buffers");
        Check(!KNativeAudio::createNativeAudio(), "no audio backend before init");
        KNativeAudio::init();
        Check(KNativeAudio::availableAudio.empty(), "init must not publish devices");
        Check(!KNativeAudio::createNativeAudio(), "no Fake Midi backend after init");
        KNativeAudio::shutdown();
        Check(!KNativeAudio::createNativeAudio(), "shutdown stays unavailable");
        for (const U32 length : {0u, 4u}) {
            std::array<U8, 4> destination{1, 2, 3, 4};
            const auto before = destination;
            bool called = false;
            bool rejected = false;
            try {
                Platform::writeCodeToMemory(destination.data(), length, [&] {
                    called = true;
                    destination.fill(0);
                });
            } catch (const std::runtime_error& error) {
                rejected = std::strstr(error.what(), "unsupported") != nullptr;
            }
            Check(rejected, "code-memory write must fail explicitly");
            Check(!called, "unsupported code-memory callback must never run");
            Check(destination == before, "rejected code memory must remain unchanged");
        }
        std::cout << checks << " UWP profile behavior checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UWP profile test failed: " << error.what() << '\n';
        return 1;
    }
}
