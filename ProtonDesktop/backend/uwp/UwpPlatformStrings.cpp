// SPDX-License-Identifier: GPL-2.0-only
#include "boxedwine.h"
#include "UwpStringSearch.h"

// Match the pinned Platform declaration used by BString::contains. The pointer
// aliases the caller's haystack just as strstr does; this routine never writes it.
char* platform_strcasestr(const char* haystack, const char* needle)
{
    return const_cast<char*>(protondesktop::uwp::FindAsciiCaseInsensitive(haystack, needle));
}
