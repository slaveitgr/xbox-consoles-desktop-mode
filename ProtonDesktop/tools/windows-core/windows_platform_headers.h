// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// Only forced into the two upstream Windows platform translation units.
// WIN32_LEAN_AND_MEAN removes legacy transitive multimedia/shell declarations;
// import their real SDK declarations explicitly without changing host behavior.
#include <Windows.h>
#include <mmsystem.h>
#include <shellapi.h>
