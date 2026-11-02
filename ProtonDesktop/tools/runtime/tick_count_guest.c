/* SPDX-License-Identifier: GPL-2.0-only */
/* Self-authored, freestanding Linux x86-64 guest. No Wine code executes.
 * Exit 10 reproduces the missing MAP_SHARED alias for mmap(NULL).
 * The reader uses Wine 8's KSYSTEM_TIME layout and bounded high1/high2 retry.
 * This checks observed clock advancement, not clock origin or sleep support.
 */
typedef unsigned long long U64;
typedef unsigned int U32;

static long syscall6(long number, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long result;
    __asm__ volatile("syscall" : "=a"(result)
        : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return result;
}

static U64 milliseconds(void)
{
    long time[2];
    if (syscall6(228, 1, (long)time, 0, 0, 0, 0) < 0) return 0;
    return (U64)time[0] * 1000 + (U64)time[1] / 1000000;
}

static void publish_time(volatile U32* page, U64 time)
{
    /* Wine 8 server/fd.c: high2, low, high1. No fabricated zero clock. */
    page[0x328 / 4] = (U32)(time >> 32);
    page[0x320 / 4] = (U32)time;
    page[0x324 / 4] = (U32)(time >> 32);
}

static U64 read_tick_count(volatile U32* page)
{
    for (unsigned retry = 0; retry < 1000; ++retry)
    {
        U32 high1 = page[0x324 / 4];
        U32 low = page[0x320 / 4];
        U32 high2 = page[0x328 / 4];
        if (high1 == high2) return ((U64)high1 << 32) | low;
    }
    return ~0ULL;
}

static int test(void)
{
    /* This is a disposable file in the fixture's private virtual /tmp. */
    long fd = syscall6(257, -100, (long)"/tmp/tmpmap-tick-regression.dat", 66, 384, 0, 0);
    if (fd < 0 || syscall6(77, fd, 4096, 0, 0, 0, 0) < 0) return 9;
    long writer = syscall6(9, 0, 4096, 3, 1, fd, 0);
    long reader = syscall6(9, 0x7ffe0000, 4096, 1, 0x11, fd, 0);
    if (writer < 0 || reader < 0) return 9;
    U64 initial = milliseconds();
    if (!initial) return 9;
    publish_time((void*)writer, initial);
    if (read_tick_count((void*)reader) != initial) return 10;

    /* The pinned core's nanosleep is a no-op. Poll real time with a bound
     * rather than treating a successful nanosleep return as elapsed time. */
    U64 next = initial;
    for (unsigned attempt = 0; attempt < 100000 && next <= initial; ++attempt)
        next = milliseconds();
    if (next <= initial) return 11;
    publish_time((void*)writer, next);
    if (read_tick_count((void*)reader) != next) return 12;

    long private_map = syscall6(9, 0, 4096, 3, 2, fd, 0);
    if (private_map < 0) return 9;
    ((volatile U32*)private_map)[0x320 / 4] = 0x12345678;
    publish_time((void*)writer, next + 1);
    if (((volatile U32*)private_map)[0x320 / 4] != 0x12345678) return 13;
    if (read_tick_count((void*)reader) != next + 1) return 14;
    return 0;
}

void _start(void)
{
    syscall6(60, test(), 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
