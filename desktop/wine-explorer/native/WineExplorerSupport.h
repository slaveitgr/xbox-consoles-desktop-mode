/* SPDX-License-Identifier: MIT
 * Windows SDK glue only. The actual Explorer remains the pinned Wine source.
 */
#ifndef MIHAWK_WINE_EXPLORER_SUPPORT_H
#define MIHAWK_WINE_EXPLORER_SUPPORT_H
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commoncontrols.h>
#include <commctrl.h>
#include <exdisp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "SupportAlgorithms.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(value) (sizeof(value) / sizeof((value)[0]))
#endif

void *MihawkWineMalloc(size_t size);
void *MihawkWineCalloc(size_t count, size_t size);
void MihawkWineFree(void *memory);
size_t MihawkWineWideLength(const wchar_t *text);
int MihawkWineWideCompare(const wchar_t *left, const wchar_t *right, size_t count);
void MihawkWineStartupFailure(const char *stage, HRESULT status);
void MihawkWineStage(const char *stage, HRESULT status);

#ifndef MIHAWK_WINE_SUPPORT_IMPLEMENTATION
#undef iswspace
#define malloc MihawkWineMalloc
#define calloc MihawkWineCalloc
#define free MihawkWineFree
#define memcpy MwCopyBytes
#define memset MwFillBytes
#define memcmp MwCompareBytes
#define wcslen MihawkWineWideLength
#define wcsnicmp MihawkWineWideCompare
#define iswspace MwIsCommandSpace
#endif
#endif
