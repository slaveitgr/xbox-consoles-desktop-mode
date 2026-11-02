// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// clang-cl implements the arithmetic type, but MSVC's standard headers need
// not provide GCC's spelling used by the pinned upstream interpreter.
typedef unsigned __int128 __uint128_t;
