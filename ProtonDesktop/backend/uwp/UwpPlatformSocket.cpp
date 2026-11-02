// SPDX-License-Identifier: GPL-2.0-only
#include "boxedwine.h"
#include "UwpSocketPair.h"
#include <type_traits>
static_assert(sizeof(void*) == 8, "The UWP interpreter target must be x64");
static_assert(std::is_same<S32, std::int32_t>::value, "Pinned Platform socket descriptor ABI changed");
int Platform::nativeSocketPair(S32 sockets[2])
{
    return protondesktop::uwp::CreateNativeSocketPair(sockets);
}
