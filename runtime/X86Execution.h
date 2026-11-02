#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace xcdm::runtime {

struct X86Registers {
    std::uint32_t eax = 0, ecx = 0, edx = 0, ebx = 0;
    std::uint32_t esp = 0, ebp = 0, esi = 0, edi = 0;
    std::uint32_t eip = 0, eflags = 2;
};

enum class X86HostCallAction { NotHandled, Continue, Exit, Fault };
enum class X86StopReason {
    Exited, InstructionLimit, MemoryFault, UnsupportedInstruction,
    IoDenied, HostFault, Halted, InvalidConfiguration, InitializationFailed
};

struct X86ExecutionConfig {
    X86Registers initial;
    // Guest FS segment base (for a virtual x86 TEB); host FS is never changed.
    std::uint32_t fsBase = 0;
    std::uint64_t instructionLimit = 100000;
    void* context = nullptr;
    // Guest addresses are virtual: callbacks copy bytes; never cast a guest
    // address to a host pointer. Reads marked instructionFetch require X access.
    bool (*readMemory)(void*, std::uint32_t address, std::uint8_t* output,
                       std::size_t size, bool instructionFetch) noexcept = nullptr;
    bool (*writeMemory)(void*, std::uint32_t address, const std::uint8_t* input,
                        std::size_t size) noexcept = nullptr;
    // Called at EIP before decoding. Continue resumes at the updated registers
    // (including EIP/ESP for stdcall); Exit finishes successfully. Unhandled
    // addresses execute through the core. Host calls consume instruction budget.
    X86HostCallAction (*hostCall)(void*, X86Registers&) noexcept = nullptr;
};

struct X86ExecutionResult {
    bool ok = false; // True only when the host-call callback explicitly exits.
    X86StopReason reason = X86StopReason::InvalidConfiguration;
    X86Registers registers;
    std::uint64_t instructions = 0;
    std::string message;
};

// Portable regular x86 integer interpreter backed by vendored libx86emu.
// No FPU/MMX/SSE, Win32 API implementation, native guest execution or JIT.
X86ExecutionResult RunX86(const X86ExecutionConfig& config) noexcept;

} // namespace xcdm::runtime
