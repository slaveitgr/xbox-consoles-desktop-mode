// SPDX-License-Identifier: MIT
#pragma once
#include "Bridge/ImportBinder.h"
#include "runtime/GuestEnvironment.h"

namespace Win32Bridge::Bridge
{
    // Called only for Bridge-owned modules in the strict ownership profile.
    ImportResolution ResolveGuestContextImport(const ImportedSymbol& symbol);
    // A missing activation raises a noncontinuable native fault. Never returns
    // the host TEB, and never manufactures a guest environment on demand.
    xcdm::guest::Teb64* RequireGuestTeb() noexcept;
}
