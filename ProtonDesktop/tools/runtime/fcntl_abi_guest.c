// SPDX-License-Identifier: GPL-2.0-only
// Self-authored static Linux x86-64 guest. Execute ONLY through CPU64.
typedef unsigned long long U64;
typedef unsigned int U32;

struct guest_flock {
    short type, whence;
    U32 pad0;
    long start, length;
    int pid;
    U32 pad1;
};
_Static_assert(sizeof(struct guest_flock) == 32, "x64 flock size");
_Static_assert(__builtin_offsetof(struct guest_flock, start) == 8, "x64 flock start");
_Static_assert(__builtin_offsetof(struct guest_flock, length) == 16, "x64 flock length");
_Static_assert(__builtin_offsetof(struct guest_flock, pid) == 24, "x64 flock pid");

static volatile U64 private_word = 0x1122334455667788ULL;

static long call6(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long out;
    __asm__ volatile("syscall" : "=a"(out)
        : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return out;
}

#define SAY(s) call6(1, 1, (long)(s), sizeof(s) - 1, 0, 0, 0)
#define CHECK(c, s, n) do { \
    if (!(c)) { SAY("PASSFAIL_FCNTL FAIL: " s "\n"); return n; } \
    SAY("PASSFAIL_FCNTL PASS: " s "\n"); \
} while (0)

static struct guest_flock make_lock(short type, long start, long length)
{
    struct guest_flock lock = {type, 0, 0x5a17c0deU, start, length, 0, 0xc0de5a17U};
    return lock;
}

static int child_test(long fd, int after_unlock)
{
    const long start = 0x100000020L, length = 0x100000080L;
    // getppid independently identifies the real lock owner's process ID. The
    // pinned getpid/gettid implementation has a separate ID-conflation bug.
    const long parent = call6(110, 0, 0, 0, 0, 0, 0);
    CHECK(parent > 0, "child getppid identifies actual parent process", 19);
    struct guest_flock lock = make_lock(1, start, length);
    CHECK(call6(72, fd, 5, (long)&lock, 0, 0, 0) == 0, "F_GETLK succeeds", 20);
    if (!after_unlock)
    {
        CHECK(lock.type == 1, "F_GETLK reports conflicting write lock", 21);
        CHECK(lock.pid == parent && lock.pid != 0,
              "F_GETLK returns the actual nonzero parent owner PID at x64 offset24", 22);
        CHECK(lock.whence == 0 && lock.start == start && lock.length == length,
              "F_GETLK preserves 64-bit lock range fields", 23);
        CHECK(lock.pad0 == 0x5a17c0deU && lock.pad1 == 0xc0de5a17U,
              "F_GETLK preserves x64 ABI padding", 24);
        lock = make_lock(1, start, length);
        const long result = call6(72, fd, 6, (long)&lock, 0, 0, 0);
        CHECK(result == -11 || result == -13,
              "F_SETLK rejects conflicting lock from another process", 25);
        lock = make_lock(1, start + length + 0x100, length);
        CHECK(call6(72, fd, 5, (long)&lock, 0, 0, 0) == 0 && lock.type == 2,
              "F_GETLK reports non-overlapping range unlocked", 26);
    }
    else
    {
        CHECK(lock.type == 2, "F_GETLK reports parent-unlocked range unlocked", 27);
        CHECK(lock.start == start && lock.length == length && lock.pid == 0 &&
              lock.pad0 == 0x5a17c0deU && lock.pad1 == 0xc0de5a17U,
              "unlocked query changes only lock type", 28);
        lock = make_lock(1, start, length);
        CHECK(call6(72, fd, 6, (long)&lock, 0, 0, 0) == 0,
              "F_SETLK can acquire range after parent unlocks", 29);
    }
    private_word = 0x8877665544332211ULL;
    return 0;
}

static int fork_and_check(long fd, int after_unlock)
{
    const long child = call6(56, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0, "real guest fork succeeds", 30);
    if (!child)
    {
        call6(60, child_test(fd, after_unlock), 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }
    int status = 0;
    long result;
    do { result = call6(61, child, (long)&status, 0, 0, 0, 0); } while (result == -4);
    CHECK(result == child, "parent reaps actual child PID", 31);
    if (status)
    {
        SAY("PASSFAIL_FCNTL FAIL: child lock test exit status\n");
        return (status >> 8) ? status >> 8 : 32;
    }
    CHECK(private_word == 0x1122334455667788ULL,
          "fork child writes preserve parent private memory", 33);
    return 0;
}

static int test(void)
{
    const long fd = call6(257, -100, (long)"/tmp/flock-abi-regression.dat", 66, 384, 0, 0);
    CHECK(fd >= 0, "open real guest lock file", 10);
    CHECK(call6(39, 0, 0, 0, 0, 0, 0) > 0, "actual guest parent ID exists", 11);
    struct guest_flock lock = make_lock(1, 0x100000020L, 0x100000080L);
    CHECK(call6(72, fd, 6, (long)&lock, 0, 0, 0) == 0,
          "parent F_SETLK acquires 64-bit byte range", 12);
    int result = fork_and_check(fd, 0);
    if (result) return result;
    lock.type = 2;
    CHECK(call6(72, fd, 6, (long)&lock, 0, 0, 0) == 0,
          "parent F_SETLK unlocks byte range", 13);
    result = fork_and_check(fd, 1);
    if (result) return result;
    CHECK(call6(3, fd, 0, 0, 0, 0, 0) == 0, "close guest descriptor", 14);
    return 0;
}

void _start(void)
{
    call6(60, test(), 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
