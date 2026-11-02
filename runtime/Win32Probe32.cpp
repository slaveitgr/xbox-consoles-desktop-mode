#include "Win32Probe32.h"

#include "Pe32Image.h"
#include "X86Execution.h"
#include "LowAddressProbe.h"
#include "../samples/X86Win32Fixture.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace xcdm::runtime {
namespace {
constexpr std::uint32_t StackBase = 0x10000000;
constexpr std::uint32_t StackSize = 64 * 1024;
constexpr std::uint32_t GateBase = 0x7ff00000;
constexpr std::uint32_t TebBase = 0x7ffde000;

bool Range(std::size_t at, std::size_t size, std::size_t total) noexcept {
    return at <= total && size <= total - at;
}
std::string Hex(std::uint32_t n) {
    std::ostringstream out; out << "0x" << std::hex << n; return out.str();
}
struct Guest {
    Pe32Image image;
    std::vector<std::uint8_t> stack = std::vector<std::uint8_t>(StackSize);
    std::vector<std::uint8_t> environment = std::vector<std::uint8_t>(8192);
    std::uint32_t lastError = 0, exitCode = 0;
    bool exited = false;
    std::string debug, failure;
    std::vector<std::string> calls;

    std::uint8_t* Pointer(std::uint32_t address, std::size_t size, bool write, bool execute) noexcept {
        if (address >= StackBase && Range(address - StackBase, size, stack.size()))
            return execute ? nullptr : stack.data() + (address - StackBase);
        if (address >= TebBase && Range(address - TebBase, size, environment.size()))
            return execute ? nullptr : environment.data() + (address - TebBase);
        if (address < image.base) return nullptr;
        const auto rva = address - image.base;
        if (!Range(rva, size, image.bytes.size())) return nullptr;
        if (!write && !execute && Range(rva, size, image.headerSize)) return image.bytes.data() + rva;
        for (const auto& section : image.sections) {
            if (rva >= section.rva && Range(rva - section.rva, size, section.size) &&
                section.readable && (!write || section.writable) && (!execute || section.executable))
                return image.bytes.data() + rva;
        }
        return nullptr;
    }
    bool Read32(std::uint32_t address, std::uint32_t& result) noexcept {
        const auto* p = Pointer(address, 4, false, false);
        if (!p) return false;
        result = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
            (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        return true;
    }
    bool Write32(std::uint32_t address, std::uint32_t value) noexcept {
        auto* p = Pointer(address, 4, true, false);
        if (!p) return false;
        for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (i * 8));
        return true;
    }
};

bool Supported(const Pe32Import& imported) {
    auto library = imported.library;
    std::transform(library.begin(), library.end(), library.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    });
    return library == "KERNEL32.DLL" && !imported.byOrdinal &&
        (imported.name == "SetLastError" || imported.name == "GetLastError" ||
         imported.name == "OutputDebugStringA" || imported.name == "ExitProcess" ||
         imported.name == "GetVersionExA" || imported.name == "GetModuleHandleA");
}

std::string ImportName(const Pe32Import& imported) {
    return imported.library + "!" + (imported.byOrdinal ? "#" + std::to_string(imported.ordinal) : imported.name);
}

bool Read(void* opaque, std::uint32_t address, std::uint8_t* out, std::size_t size, bool fetch) noexcept {
    if (!out || !size) return false;
    auto& guest = *static_cast<Guest*>(opaque);
    const auto* p = guest.Pointer(address, size, false, fetch);
    if (!p) return false;
    std::memcpy(out, p, size);
    return true;
}
bool Write(void* opaque, std::uint32_t address, const std::uint8_t* input, std::size_t size) noexcept {
    if (!input || !size) return false;
    auto& guest = *static_cast<Guest*>(opaque);
    auto* p = guest.Pointer(address, size, true, false);
    if (!p) return false;
    std::memcpy(p, input, size);
    return true;
}

X86HostCallAction HostCall(void* opaque, X86Registers& state) noexcept {
    auto& guest = *static_cast<Guest*>(opaque);
    if (state.eip < GateBase || state.eip >= GateBase + 8192U * 16U) return X86HostCallAction::NotHandled;
    try {
        const auto offset = state.eip - GateBase;
        const auto index = offset / 16;
        if (offset % 16 || index >= guest.image.imports.size()) {
            guest.failure = "Invalid Win32 guest import gate."; return X86HostCallAction::Fault;
        }
        const auto& imported = guest.image.imports[index];
        const auto& name = imported.name;
        if (guest.calls.size() >= 128) { guest.failure = "Win32 call trace limit reached."; return X86HostCallAction::Fault; }
        if (!Supported(imported)) {
            guest.calls.push_back(ImportName(imported));
            guest.failure = "Missing Win32 API: " + ImportName(imported);
            return X86HostCallAction::Fault;
        }
        const std::uint32_t arguments = name == "GetLastError" ? 0 : 4;
        std::uint32_t returnAddress = 0, argument = 0;
        if (state.esp > UINT32_MAX - 8 || !guest.Read32(state.esp, returnAddress) ||
            (arguments && !guest.Read32(state.esp + 4, argument))) {
            guest.failure = "Win32 stdcall stack is outside guest memory."; return X86HostCallAction::Fault;
        }
        guest.calls.push_back(name);
        if (name == "ExitProcess") {
            guest.exitCode = argument;
            guest.exited = true;
            // Never invoke the host process's ExitProcess.
            return X86HostCallAction::Exit;
        }
        if (!guest.Pointer(returnAddress, 1, false, true)) {
            guest.failure = "Win32 return address is outside executable guest memory."; return X86HostCallAction::Fault;
        }
        if (name == "SetLastError") {
            guest.lastError = argument;
            if (!guest.Write32(TebBase + 0x34, argument)) return X86HostCallAction::Fault;
            state.eax = 0;
        } else if (name == "GetLastError") {
            if (!guest.Read32(TebBase + 0x34, guest.lastError)) return X86HostCallAction::Fault;
            state.eax = guest.lastError;
        }
        else if (name == "GetModuleHandleA") {
            if (!argument) {
                // This is the mapped guest executable, never a host HMODULE.
                // A successful query does not change the guest's last error.
                state.eax = guest.image.base;
            } else {
                // Named modules do not have a guest loader/registry yet. Still
                // validate the entire bounded ANSI name before returning the
                // truthful lookup failure; do not pass guest pointers to a
                // host API or manufacture a handle for a DLL name.
                constexpr std::uint32_t NameLimit = 512;
                bool ended = false;
                for (std::uint32_t offset = 0; offset < NameLimit; ++offset) {
                    if (argument > UINT32_MAX - offset) break;
                    const auto* character = guest.Pointer(argument + offset, 1, false, false);
                    if (!character) break;
                    if (!*character) { ended = true; break; }
                }
                if (!ended) {
                    guest.failure = "GetModuleHandleA requires a readable NUL-terminated ANSI guest name within 512 bytes.";
                    return X86HostCallAction::Fault;
                }
                if (!guest.Write32(TebBase + 0x34, 126)) return X86HostCallAction::Fault;
                guest.lastError = 126; // ERROR_MOD_NOT_FOUND
                state.eax = 0;
            }
        }
        else if (name == "GetVersionExA") {
            // These are the ANSI Win32 layouts, independent of the x64 host
            // ABI. Accept only the two supported sizes; a caller-provided
            // size must never control an unchecked host memset/copy.
            constexpr std::uint32_t InfoSize = 148, ExSize = 156;
            std::uint32_t size = 0;
            if (!guest.Read32(argument, size)) {
                guest.failure = "GetVersionExA requires a readable guest buffer size.";
                return X86HostCallAction::Fault;
            }
            if (size != InfoSize && size != ExSize) {
                // Bridge policy matches the existing x64 version shim.
                // Native Windows does not document this exact error code.
                guest.lastError = 122; // ERROR_INSUFFICIENT_BUFFER
                if (!guest.Write32(TebBase + 0x34, guest.lastError))
                    return X86HostCallAction::Fault;
                state.eax = 0;
            } else {
                auto* output = guest.Pointer(argument, size, true, false);
                const auto end = static_cast<std::uint64_t>(argument) + size;
                const auto frameEnd = static_cast<std::uint64_t>(state.esp) + 8;
                if (!output || (argument < frameEnd && state.esp < end)) {
                    guest.failure = "GetVersionExA requires a complete writable guest buffer outside its live stdcall frame.";
                    return X86HostCallAction::Fault;
                }
                std::array<std::uint8_t, ExSize> version{};
                const auto put = [&version](std::size_t at, std::uint32_t value) {
                    for (unsigned byte = 0; byte < 4; ++byte)
                        version[at + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
                };
                // Explicit virtual desktop profile, matching the upstream
                // x64 shim. It is not the Xbox OS or manifest-sensitive
                // native GetVersionEx behavior. szCSDVersion stays empty.
                put(0, size);
                put(4, 10);
                put(8, 0);
                put(12, 19045);
                put(16, 2); // VER_PLATFORM_WIN32_NT
                if (size == ExSize) version[154] = 1; // VER_NT_WORKSTATION
                std::memcpy(output, version.data(), size);
                state.eax = 1;
                // Successful version queries preserve the guest's last error.
            }
        }
        else if (name == "OutputDebugStringA") {
            std::string output;
            bool ended = false;
            for (std::uint32_t n = 0; n < 512; ++n) {
                if (argument > UINT32_MAX - n) break;
                const auto* p = guest.Pointer(argument + n, 1, false, false);
                if (!p) break;
                if (!*p) { ended = true; break; }
                if (*p < 0x20 || *p > 0x7e) break;
                output.push_back(static_cast<char>(*p));
            }
            if (!ended) { guest.failure = "OutputDebugStringA requires a readable bounded ASCII guest string."; return X86HostCallAction::Fault; }
            if (guest.debug.size() + output.size() > 4096) {
                guest.failure = "Guest debug output limit reached."; return X86HostCallAction::Fault;
            }
            guest.debug += output;
            state.eax = 0;
        } else { guest.failure = "Unsupported Win32 call."; return X86HostCallAction::Fault; }
        state.esp += 4 + arguments;
        state.eip = returnAddress;
        return X86HostCallAction::Continue;
    } catch (...) { return X86HostCallAction::Fault; }
}

Win32Probe32Result Execute(const std::uint8_t* bytes, std::size_t size, std::uint64_t limit, bool verifyFixture, bool startupTrace = false) {
    std::ostringstream report;
    report << "Xbox Desktop Mode / x86 Win32 execution\n"
           << "Engine: libx86emu, bounded protected-mode integer interpreter\n"
           << "Native guest code/JIT: no\nFPU/MMX/SSE: not implemented by this engine\n"
           << "Host API profile: KERNEL32 SetLastError, GetLastError, OutputDebugStringA, ExitProcess, GetVersionExA, GetModuleHandleA\n"
           << "GetVersionExA policy: virtual Windows 10.0.19045 workstation, not the host OS; ANSI 148/156-byte structures, no manifest version negotiation.\n"
           << "GetModuleHandleA policy: NULL returns the guest executable base; named lookup is unavailable and returns NULL with ERROR_MOD_NOT_FOUND (126) after validating a NUL-terminated ANSI name within 512 readable guest bytes. No host modules are exposed.\n"
           << "Operation: " << (startupTrace ? "bounded supplied-image startup trace" : "self-authored fixture execution") << '\n';
    if (!limit || limit > 1000000) return {false, "Invalid x86 instruction budget.", report.str()};
    auto mapped = LoadPe32Image(bytes, size, startupTrace ? Pe32Profile::StartupTrace : Pe32Profile::Fixture);
    if (!mapped.ok) {
        report << "Loader stopped: " << mapped.message << '\n';
        return {false, "PE32 guest mapping failed: " + mapped.message, report.str()};
    }
    Guest guest;
    guest.image = std::move(mapped.image);
    const auto imageEnd = static_cast<std::uint64_t>(guest.image.base) + guest.image.bytes.size();
    if (guest.image.base < StackBase + StackSize && imageEnd > StackBase)
        return {false, "PE32 image overlaps the reserved guest stack.", report.str()};
    // A deliberately small, virtual single-thread environment, not the host's
    // TEB/PEB. Supports initial CRT SEH setup and a truthful empty process state.
    guest.Write32(TebBase, 0xffffffffU); // NT_TIB.ExceptionList
    guest.Write32(TebBase + 4, StackBase + StackSize);
    guest.Write32(TebBase + 8, StackBase);
    guest.Write32(TebBase + 0x18, TebBase); // NT_TIB.Self
    guest.Write32(TebBase + 0x20, 1); // Emulated process identity
    guest.Write32(TebBase + 0x24, 1); // Emulated thread identity
    guest.Write32(TebBase + 0x30, TebBase + 4096);
    guest.Write32(TebBase + 4096 + 8, guest.image.base);
    for (std::size_t i = 0; i < guest.image.imports.size(); ++i) {
        const auto& imported = guest.image.imports[i];
        if (!Supported(imported) && !startupTrace) {
            report << "Unresolved import: " << imported.library << '!' <<
                (imported.byOrdinal ? "#" + std::to_string(imported.ordinal) : imported.name) << '\n';
            return {false, "Unsupported PE32 import; guest execution was not started.", report.str()};
        }
        // Loader initialization is distinct from a guest instruction writing
        // memory. Windows images may place the IAT in a read-only section.
        // The loader already validated the entire slot; recheck vector bounds
        // here and initialize only those four bytes without changing guest
        // section permissions or making any host memory executable.
        if (!Range(imported.iatRva, 4, guest.image.bytes.size()))
            return {false, "PE32 loader IAT slot is outside the guest image at RVA " +
                Hex(imported.iatRva) + ".", report.str()};
        const auto gate = GateBase + static_cast<std::uint32_t>(i * 16);
        for (unsigned byte = 0; byte < 4; ++byte)
            guest.image.bytes[imported.iatRva + byte] = static_cast<std::uint8_t>(gate >> (byte * 8));
    }
    X86ExecutionConfig config;
    config.initial.eip = guest.image.entry;
    config.initial.esp = StackBase + StackSize - 16;
    config.initial.eflags = 2;
    config.instructionLimit = limit;
    if (startupTrace) config.fsBase = TebBase;
    config.context = &guest;
    config.readMemory = Read;
    config.writeMemory = Write;
    config.hostCall = HostCall;
    const auto executed = RunX86(config);
    bool passed = executed.ok && guest.exited && guest.exitCode == 0;
    if (verifyFixture) {
        const std::vector<std::string> expected{"SetLastError", "GetLastError", "OutputDebugStringA", "ExitProcess"};
        passed = passed && guest.lastError == 0x1234 && guest.debug == "x86 Win32 host calls passed" && guest.calls == expected;
    }
    report << "Guest format: PE32/i386\nGuest image base: " << Hex(guest.image.base)
           << "\nEntry point: " << Hex(guest.image.entry) << "\nStop EIP: " << Hex(executed.registers.eip)
           << "\nInstruction budget: " << limit << "\nInstructions/host gates: " << executed.instructions
           << "\nEngine stop: " << executed.message << "\nGuest ExitProcess reached: " << (guest.exited ? "yes" : "no")
           << "\nGuest exit code: " << guest.exitCode << "\nGuest last error: " << Hex(guest.lastError)
           << "\nDebug output: " << guest.debug << "\nWin32 call trace:\n";
    for (const auto& call : guest.calls) report << "  " << call << '\n';
    if (!guest.failure.empty()) report << "Host-call failure: " << guest.failure << '\n';
    if (startupTrace) {
        if (guest.image.certificateSize)
            report << "SECURITY certificate table: file offset " << Hex(guest.image.certificateFileOffset)
                   << ", size " << guest.image.certificateSize << ", records " << guest.image.certificateCount
                   << "; structural revision 0x200/type 0x2 checks only. Certificate bytes are not mapped; Authenticode signature/trust is not verified.\n";
        report << "Mapped sections:\n";
        for (const auto& section : guest.image.sections)
            report << "  " << Hex(guest.image.base + section.rva) << " size " << section.size << " "
                   << (section.readable ? 'R' : '-') << (section.writable ? 'W' : '-') << (section.executable ? 'X' : '-') << '\n';
        report << "Registered import gates: " << guest.image.imports.size() << "\nEntry bytes:";
        const auto rva = guest.image.entry - guest.image.base;
        for (std::size_t n = 0; n < 32 && n < guest.image.bytes.size() - rva; ++n)
            report << ' ' << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(guest.image.bytes[rva + n]);
        report << std::dec << '\n';
    }
    report << "Result: " << (passed ? "guest exited 0" : (startupTrace ? "startup stopped" : "failed")) << '\n'
           << "Scope: " << (startupTrace ? "bounded integer-only startup trace; unknown APIs stop execution" : "self-authored integer fixture and four guest Win32 host calls") << ".\n"
           << "San Andreas, graphics, audio, filesystem, threads and general Windows APIs are not implemented.\n";
    if (startupTrace) {
        const auto reason = guest.failure.empty() ? executed.message : guest.failure;
        return {passed, (passed ? "Guest exited 0. " : "Stopped at " + Hex(executed.registers.eip) + ": ") + reason, report.str()};
    }
    return {passed, passed ? "PASS: PE32 x86 program executed with four Win32 host calls." :
        "FAIL: x86/Win32 program stopped; see x86-runtime.txt for the exact reason.", report.str()};
}
} // namespace

Win32Probe32Result ExecuteWin32IntegerImage(const std::uint8_t* bytes, std::size_t size, std::uint64_t limit) noexcept {
    try { return Execute(bytes, size, limit, false); }
    catch (...) { return {}; }
}
Win32Probe32Result RunWin32Probe32() noexcept {
    try {
        const auto& fixture = samples::X86Win32PeBytes();
        auto result = Execute(fixture.data(), fixture.size(), 100000, true);
        const auto low = RunLowAddressProbe();
        result.report += "\nSeparate native low-address capability diagnostic (not a CPU-test pass criterion):\n";
        result.report += low.status == ProbeStatus::Passed ? "Status: Passed\n" :
            (low.status == ProbeStatus::Unsupported ? "Status: Unsupported\n" : "Status: Failed\n");
        result.report += low.message + "\nError code: " + std::to_string(low.errorCode) + "\n";
        return result;
    } catch (...) { return {}; }
}
Win32Probe32Result TraceWin32Startup32(const std::uint8_t* bytes, std::size_t size, std::uint64_t limit) noexcept {
    try { return Execute(bytes, size, limit, false, true); }
    catch (...) { return {}; }
}
} // namespace xcdm::runtime
