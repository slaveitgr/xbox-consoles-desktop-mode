// SPDX-License-Identifier: GPL-2.0-only
#include "backend/uwp/UwpStringSearch.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

using protondesktop::uwp::FindAsciiCaseInsensitive;
namespace
{
    unsigned checks = 0;
    void Require(bool condition, const char* message)
    {
        ++checks;
        if (!condition) throw std::runtime_error(message);
    }
    void SearchChecks()
    {
        const char text[] = "Alpha beta ALPHA";
        Require(FindAsciiCaseInsensitive(text, "aLpHa") == text, "first mixed-case match");
        Require(FindAsciiCaseInsensitive(text, "BETA") == text + 6, "middle word pointer");
        Require(FindAsciiCaseInsensitive(text, "alpha alpha") == nullptr, "absent phrase");
        Require(FindAsciiCaseInsensitive(text, "") == text, "empty needle matches start");
        const char empty[] = "";
        Require(FindAsciiCaseInsensitive(empty, "") == empty, "both empty strings");
        Require(FindAsciiCaseInsensitive(empty, "a") == nullptr, "empty haystack");
        Require(FindAsciiCaseInsensitive(nullptr, "a") == nullptr, "null haystack");
        Require(FindAsciiCaseInsensitive(text, nullptr) == nullptr, "null needle");
        Require(FindAsciiCaseInsensitive(nullptr, nullptr) == nullptr, "both null");
        Require(FindAsciiCaseInsensitive(nullptr, "") == nullptr, "null with empty needle");
        const char overlap[] = "aaABaB";
        Require(FindAsciiCaseInsensitive(overlap, "aab") == overlap + 1,
                "mismatched candidate does not skip an overlapping match");
        Require(FindAsciiCaseInsensitive(overlap, "ABAB") == overlap + 2, "match ends at NUL");
        Require(FindAsciiCaseInsensitive(overlap, "ABABC") == nullptr, "longer suffix fails");
        Require(FindAsciiCaseInsensitive(overlap, "aaababx") == nullptr, "needle longer than input");
        const char punctuation[] = "[@AZ]az_09";
        Require(FindAsciiCaseInsensitive(punctuation, "az_09") == punctuation + 5, "punctuation and digits");
        Require(FindAsciiCaseInsensitive(punctuation, "{az}") == nullptr, "ASCII punctuation not case folded");
        const char highBytes[] = {'x', char(0xC0), 'A', char(0xFF), 0};
        const char highNeedle[] = {char(0xC0), 'a', char(0xFF), 0};
        const char differentHigh[] = {char(0xE0), 'a', char(0xFF), 0};
        Require(FindAsciiCaseInsensitive(highBytes, highNeedle) == highBytes + 1,
                "non-ASCII bytes compare literally beside folded ASCII");
        Require(FindAsciiCaseInsensitive(highBytes, differentHigh) == nullptr,
                "high-byte case or locale mappings are not invented");
        const char embeddedNul[] = {'a', 0, 'B', 0};
        Require(FindAsciiCaseInsensitive(embeddedNul, "B") == nullptr, "search stops at first NUL");
        char writable[] = "AbCdEf";
        const char expected[] = "AbCdEf";
        Require(FindAsciiCaseInsensitive(writable, "CdE") == writable + 2, "writable input match");
        Require(std::memcmp(writable, expected, sizeof(writable)) == 0, "input bytes remain unchanged");
        // Exercise every high byte without signed-char/ctype undefined behavior.
        for (unsigned value = 128; value < 256; ++value)
        {
            const char source[] = {'A', static_cast<char>(value), 'z', 0};
            const char needle[] = {static_cast<char>(value), 'Z', 0};
            Require(FindAsciiCaseInsensitive(source, needle) == source + 1, "all high byte values survive");
        }
    }
}
int main()
{
    try
    {
        SearchChecks();
        std::printf("UWP string search: %u checks passed (portable byte search)\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "UWP string check %u failed: %s\n", checks, error.what());
        return 1;
    }
}
