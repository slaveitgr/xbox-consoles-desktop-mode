#include "Win32Probe32.h"

#include "Pe32Image.h"
#include "X86Execution.h"
#include "../samples/X86Win32Fixture.h"

#include <algorithm>
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

bool Range(std::size_t at, std::size_t size, std::size_t total) noexcept {
    return at <= total && size <= total - at;
}
std::string Hex(std::uint32_t n) {
    std::ostringstream out; out << "0x" << std::hex << n; return out.str();
}
struct Guest {
    Pe32Image image;
    std::vector<std::uint8_t> stack = std::vector<std::uint8_t>(StackSize);
    std::uint32_t lastError = 0, exitCode = 0;
    bool exited = false;
    std::string debug, failure;
    std::vector<std::string> calls;

    std::uint8_t* Pointer(std::uint32_t address, std::size_t size, bool write, bool execute) noexcept {
        if (address >= StackBase && Range(address - StackBase, size, stack.size()))
            return execute ? nullptr : stack.data() + (address - StackBase);
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
        const auto& name = guest.image.imports[index].name;
        if (guest.calls.size() >= 128) { guest.failure = "Win32 call trace limit reached."; return X86HostCallAction::Fault; }
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
        if (name == "SetLastError") { guest.lastError = argument; state.eax = 0; }
        else if (name == "GetLastError") state.eax = guest.lastError;
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

Win32Probe32Result Execute(const std::uint8_t* bytes, std::size_t size, std::uint64_t limit, bool verifyFixture) {
    std::ostringstream report;
    report << "Xbox Desktop Mode / x86 Win32 execution\n"
           << "Engine: libx86emu, bounded protected-mode integer interpreter\n"
           << "Native guest code/JIT: no\nFPU/MMX/SSE: not implemented by this engine\n"
           << "Host API profile: KERNEL32 SetLastError, GetLastError, OutputDebugStringA, ExitProcess\n";
    if (!limit || limit > 1000000) return {false, "Invalid x86 instruction budget.", report.str()};
    auto mapped = LoadPe32Image(bytes, size);
    if (!mapped.ok) {
        report << "Loader stopped: " << mapped.message << '\n';
        return {false, "PE32 guest mapping failed: " + mapped.message, report.str()};
    }
    Guest guest;
    guest.image = std::move(mapped.image);
    const auto imageEnd = static_cast<std::uint64_t>(guest.image.base) + guest.image.bytes.size();
    if (guest.image.base < StackBase + StackSize && imageEnd > StackBase)
        return {false, "PE32 image overlaps the reserved guest stack.", report.str()};
    for (std::size_t i = 0; i < guest.image.imports.size(); ++i) {
        const auto& imported = guest.image.imports[i];
        auto library = imported.library;
        std::transform(library.begin(), library.end(), library.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        });
        const bool supported = library == "KERNEL32.DLL" && !imported.byOrdinal &&
            (imported.name == "SetLastError" || imported.name == "GetLastError" ||
             imported.name == "OutputDebugStringA" || imported.name == "ExitProcess");
        if (!supported) {
            report << "Unresolved import: " << imported.library << '!' <<
                (imported.byOrdinal ? "#" + std::to_string(imported.ordinal) : imported.name) << '\n';
            return {false, "Unsupported PE32 import; guest execution was not started.", report.str()};
        }
        if (!guest.Write32(guest.image.base + imported.iatRva, GateBase + static_cast<std::uint32_t>(i * 16)))
            return {false, "PE32 IAT could not be bound in guest memory.", report.str()};
    }
    X86ExecutionConfig config;
    config.initial.eip = guest.image.entry;
    config.initial.esp = StackBase + StackSize - 16;
    config.initial.eflags = 2;
    config.instructionLimit = limit;
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
    report << "Result: " << (passed ? "passed" : "failed") << '\n'
           << "Scope: self-authored integer fixture and four guest Win32 host calls.\n"
           << "San Andreas, graphics, audio, filesystem, threads and general Windows APIs are not implemented.\n";
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
        return Execute(fixture.data(), fixture.size(), 100000, true);
    } catch (...) { return {}; }
}
} // namespace xcdm::runtime
