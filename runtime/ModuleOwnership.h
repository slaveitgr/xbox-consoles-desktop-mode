// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace xcdm::runtime {

enum class ModuleProfile { Legacy, StrictWine };
enum class ModuleOwner { Legacy, Bridge, Wine, Reject };

struct ModuleDecision {
    ModuleOwner owner = ModuleOwner::Reject;
    std::wstring canonicalName;
    std::wstring reason;
};

// An immutable per-guest policy, selected at construction. StrictWine is an
// experimental resolution contract, not an assertion that Wine can execute.
// It accepts bare ASCII module names only. Core Wine DLLs are supplied from
// the guest's app-owned System32 by the bridge overlay, never host System32.
//
// API-set support is deliberately a small exact alias table in the .cpp.
// Unknown contracts are rejected; the legacy bridge's substring heuristics
// must not silently choose an owner for the strict profile.
class ModuleOwnership final {
public:
    explicit ModuleOwnership(ModuleProfile profile = ModuleProfile::Legacy) noexcept
        : profile_(profile) {}

    ModuleProfile Profile() const noexcept { return profile_; }
    bool IsStrict() const noexcept { return profile_ == ModuleProfile::StrictWine; }
    ModuleDecision Decide(std::wstring_view requestedName) const;
    bool AllowsBridgeHandle(std::wstring_view canonicalName) const;

private:
    const ModuleProfile profile_;
};

// The same routing primitive is exercised by portable tests and used by the
// native bridge overlay. Strict owners never try the other provider after a
// missing export. Callbacks can resolve named, ordinal or forwarded imports;
// ownership is a property of their module, never the individual symbol.
// A rejected module invokes neither callback and returns the supplied failure.
template<class BridgeResolver, class WineResolver, class IsResolved, class Result>
Result RouteModuleSymbol(const ModuleDecision& decision,
                         BridgeResolver&& bridge,
                         WineResolver&& wine,
                         IsResolved&& resolved,
                         Result failure) {
    switch (decision.owner) {
    case ModuleOwner::Bridge:
        return std::forward<BridgeResolver>(bridge)();
    case ModuleOwner::Wine:
        return std::forward<WineResolver>(wine)();
    case ModuleOwner::Legacy: {
        auto result = std::forward<BridgeResolver>(bridge)();
        if (resolved(result)) return result;
        auto alternative = std::forward<WineResolver>(wine)();
        return resolved(alternative) ? alternative : result;
    }
    case ModuleOwner::Reject:
        return failure;
    }
    return failure;
}

} // namespace xcdm::runtime
