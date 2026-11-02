// SPDX-License-Identifier: GPL-2.0-only
#pragma once

namespace protondesktop::uwp
{
    // Search NUL-terminated byte strings, folding ASCII A-Z only. Non-ASCII
    // bytes compare literally: this is not locale or Unicode case folding.
    // Returns the first match in haystack; an empty needle matches its start.
    // Null inputs fail instead of being passed to a C string routine.
    const char* FindAsciiCaseInsensitive(const char* haystack, const char* needle) noexcept;
}
