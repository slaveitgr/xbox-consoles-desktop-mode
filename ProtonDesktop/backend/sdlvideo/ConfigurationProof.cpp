// SPDX-License-Identifier: GPL-2.0-only
// Compiler evidence only, not proof of AppContainer activation or GPU output.
#include <SDL_config.h>
static_assert(SIZEOF_VOIDP == sizeof(void*), "SDL configuration must match the actual pointer ABI");
#if defined(PROTON_SDLVIDEO_APP_HEADERS)
#include <winapifamily.h>
#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) || WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#error The SDL pipeline must compile in the genuine APP SDK partition.
#endif
#if !defined(SDL_BUILDING_WINRT) || SDL_BUILDING_WINRT != 1
#error Select the real SDL WinRT source/API restrictions for the APP diagnostic.
#endif
#if !defined(_M_X64)
#error The first APP pipeline qualification targets the x64 compiler ABI.
#endif
static_assert(sizeof(void*) == 8, "The first APP pipeline qualification is x64");
#endif
