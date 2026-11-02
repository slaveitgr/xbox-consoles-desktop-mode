// SPDX-License-Identifier: GPL-2.0-only
// Link the actual pinned softfloat_state.c, not substitute state definitions.
#include "backend/uwp/UwpGuestWorkers.h"
extern "C"
{
#include "platform.h"
#include "softfloat.h"
}
#include <atomic>
#include <cstdio>
#include <future>
#include <stdexcept>

using namespace protondesktop::uwp;
namespace
{
    unsigned checks = 0;
    void Require(bool value, const char* message)
    { ++checks; if (!value) throw std::runtime_error(message); }
}
int main()
{
    try
    {
        Require(softfloat_roundingMode == softfloat_round_near_even && softfloat_exceptionFlags == 0 &&
                extF80_roundingPrecision == 80, "actual SoftFloat initial state");
        softfloat_roundingMode = softfloat_round_min;
        softfloat_exceptionFlags = softfloat_flag_inexact;
        softfloat_detectTininess = softfloat_tininess_beforeRounding;
        extF80_roundingPrecision = 32;
        GuestWorkerRegistry workers;
        std::string error;
        Require(workers.Begin(error), "begin actual-state worker isolation test");
        std::promise<void> readyA, readyB;
        auto a = readyA.get_future().share(), b = readyB.get_future().share();
        std::atomic<bool> initialA{false}, initialB{false}, isolatedA{false}, isolatedB{false};
        Require(workers.Spawn([&] {
            initialA = softfloat_roundingMode == softfloat_round_near_even && softfloat_exceptionFlags == 0 && extF80_roundingPrecision == 80;
            softfloat_roundingMode = softfloat_round_max;
            softfloat_exceptionFlags = softfloat_flag_overflow;
            softfloat_detectTininess = softfloat_tininess_afterRounding;
            extF80_roundingPrecision = 64;
            readyA.set_value(); b.wait();
            isolatedA = softfloat_roundingMode == softfloat_round_max && softfloat_exceptionFlags == softfloat_flag_overflow &&
                softfloat_detectTininess == softfloat_tininess_afterRounding && extF80_roundingPrecision == 64;
        }, 1, std::make_shared<int>(1), error), "start first SoftFloat state worker");
        Require(workers.Spawn([&] {
            initialB = softfloat_roundingMode == softfloat_round_near_even && softfloat_exceptionFlags == 0 && extF80_roundingPrecision == 80;
            softfloat_roundingMode = softfloat_round_minMag;
            softfloat_exceptionFlags = softfloat_flag_invalid;
            softfloat_detectTininess = softfloat_tininess_beforeRounding;
            extF80_roundingPrecision = 80;
            readyB.set_value(); a.wait();
            isolatedB = softfloat_roundingMode == softfloat_round_minMag && softfloat_exceptionFlags == softfloat_flag_invalid &&
                softfloat_detectTininess == softfloat_tininess_beforeRounding && extF80_roundingPrecision == 80;
        }, 2, std::make_shared<int>(2), error), "start simultaneous SoftFloat state worker");
        workers.CloseAdmission();
        Require(workers.Join(error), "join actual SoftFloat state threads");
        Require(initialA && initialB, "new workers receive their own initialized floating-point state");
        Require(isolatedA && isolatedB, "all four mutable SoftFloat globals remain independent across threads");
        Require(softfloat_roundingMode == softfloat_round_min && softfloat_exceptionFlags == softfloat_flag_inexact &&
                softfloat_detectTininess == softfloat_tininess_beforeRounding && extF80_roundingPrecision == 32,
                "worker floating-point changes leave owner thread state intact");
        Require(!workers.Snapshot().failed && workers.End(error), "actual-state session ends without worker failure");
        std::printf("Pinned SoftFloat TLS: %u checks passed (actual C state; no guest arithmetic/Wine executed)\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Pinned SoftFloat TLS check %u failed: %s\n", checks, error.what());
        return 1;
    }
}
