// SPDX-License-Identifier: GPL-2.0-only
#include "UwpStringSearch.h"

namespace protondesktop::uwp
{
    namespace
    {
        unsigned char Fold(unsigned char value) noexcept
        {
            return value >= 'A' && value <= 'Z' ?
                static_cast<unsigned char>(value + ('a' - 'A')) : value;
        }
    }

    const char* FindAsciiCaseInsensitive(const char* haystack, const char* needle) noexcept
    {
        if (!haystack || !needle) return nullptr;
        if (!*needle) return haystack;
        for (auto candidate = haystack; *candidate; ++candidate)
        {
            auto input = candidate;
            auto match = needle;
            while (*input && *match &&
                   Fold(static_cast<unsigned char>(*input)) ==
                       Fold(static_cast<unsigned char>(*match)))
            {
                ++input;
                ++match;
            }
            if (!*match) return candidate;
        }
        return nullptr;
    }
}
