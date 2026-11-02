// SPDX-License-Identifier: GPL-2.0-only
// Link diagnostic, not a packaged CoreApplication entry or a Wine launcher.
#include <winapifamily.h>
#include <cstdio>
#include <cstring>
#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) || WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#error This diagnostic must compile in the genuine UWP APP SDK partition.
#endif
static_assert(sizeof(void*) == 8, "The diagnostic requires a 64-bit host");
extern int runX64SelfTest();
extern "C" int runX64RunElf(const char* path);
int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--x64-selftest") == 0)
        return runX64SelfTest();
    if (argc == 3 && std::strcmp(argv[1], "--x64-run-elf") == 0)
        return runX64RunElf(argv[2]);
    std::fputs("ProtonDesktop UWP core link diagnostic; no Wine launch or package activation.\n", stderr);
    return 64;
}
