#include "X86Execution.h"

#include <array>
#include <limits>
#include <memory>
#include <string>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4201) // Upstream C descriptor uses an anonymous struct.
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "third_party/libx86emu/include/x86emu.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace xcdm::runtime {
namespace {

constexpr std::uint64_t kMaxInstructions = 1000000;

struct RunContext {
    const X86ExecutionConfig* config = nullptr;
    std::uint64_t instructions = 0;
    std::uint64_t memoryOperations = 0;
    std::uint64_t memoryLimit = 0;
    X86StopReason reason = X86StopReason::Halted;
    const char* message = "Guest halted without an explicit host exit.";
    bool stopped = false;
    bool hasFaultAddress = false;
    std::uint32_t faultAddress = 0;
    X86HostCallAction action = X86HostCallAction::NotHandled;
    X86Registers hostRegisters;
};

RunContext& Context(x86emu_t* emu) noexcept {
    return *static_cast<RunContext*>(emu->_private);
}

X86Registers Registers(const x86emu_t* emu) noexcept {
    X86Registers registers;
    registers.eax = emu->x86.R_EAX; registers.ecx = emu->x86.R_ECX;
    registers.edx = emu->x86.R_EDX; registers.ebx = emu->x86.R_EBX;
    registers.esp = emu->x86.R_ESP; registers.ebp = emu->x86.R_EBP;
    registers.esi = emu->x86.R_ESI; registers.edi = emu->x86.R_EDI;
    registers.eip = emu->x86.R_EIP; registers.eflags = emu->x86.R_EFLG;
    return registers;
}

void SetRegisters(x86emu_t* emu, const X86Registers& registers) noexcept {
    emu->x86.R_EAX = registers.eax; emu->x86.R_ECX = registers.ecx;
    emu->x86.R_EDX = registers.edx; emu->x86.R_EBX = registers.ebx;
    emu->x86.R_ESP = registers.esp; emu->x86.R_EBP = registers.ebp;
    emu->x86.R_ESI = registers.esi; emu->x86.R_EDI = registers.edi;
    emu->x86.R_EIP = registers.eip; emu->x86.R_EFLG = registers.eflags | 2U;
}

void Stop(x86emu_t* emu, X86StopReason reason, const char* message) noexcept {
    auto& context = Context(emu);
    if (!context.stopped) {
        context.stopped = true;
        context.reason = reason;
        context.message = message;
    }
    x86emu_stop(emu);
}

unsigned Memory(x86emu_t* emu, u32 address, u32* value, unsigned type) noexcept {
    auto& context = Context(emu);
    if (context.stopped) { *value = 0; return 1; }
    if (context.memoryOperations++ >= context.memoryLimit) {
        *value = 0;
        Stop(emu, X86StopReason::InstructionLimit, "Bounded memory-operation budget exhausted (including REP work).");
        return 1;
    }
    const unsigned operation = type & ~0xffU;
    if (operation == X86EMU_MEMIO_I || operation == X86EMU_MEMIO_O) {
        *value = 0;
        Stop(emu, X86StopReason::IoDenied, "Guest hardware port I/O is disabled.");
        return 1;
    }
    std::size_t width;
    switch (type & 0xffU) {
    case X86EMU_MEMIO_8: case X86EMU_MEMIO_8_NOPERM: width = 1; break;
    case X86EMU_MEMIO_16: width = 2; break;
    case X86EMU_MEMIO_32: width = 4; break;
    default:
        *value = 0;
        Stop(emu, X86StopReason::MemoryFault, "Unknown guest memory access width.");
        return 1;
    }
    if (width > 0x100000000ULL - address) {
        *value = 0;
        context.hasFaultAddress = true; context.faultAddress = address;
        Stop(emu, X86StopReason::MemoryFault, "Guest memory access wraps its 32-bit address space.");
        return 1;
    }
    const bool fetch = operation == X86EMU_MEMIO_X;
    // This also terminates a stream consisting only of instruction prefixes.
    // The upstream decoder otherwise keeps fetching prefixes indefinitely.
    if (fetch && (emu->x86.instr_len > 15 || width > 15U - emu->x86.instr_len)) {
        *value = 0;
        Stop(emu, X86StopReason::UnsupportedInstruction, "Guest instruction exceeds the architectural 15-byte limit.");
        return 1;
    }
    std::array<std::uint8_t, 4> bytes{};
    bool valid = false;
    if (operation == X86EMU_MEMIO_W) {
        for (std::size_t i = 0; i < width; ++i) bytes[i] = static_cast<std::uint8_t>(*value >> (i * 8));
        valid = context.config->writeMemory(context.config->context, address, bytes.data(), width);
    } else if (operation == X86EMU_MEMIO_R || fetch) {
        valid = context.config->readMemory(context.config->context, address, bytes.data(), width, fetch);
        *value = 0;
        if (valid) for (std::size_t i = 0; i < width; ++i) *value |= static_cast<u32>(bytes[i]) << (i * 8);
    }
    if (!valid) {
        *value = 0;
        context.hasFaultAddress = true; context.faultAddress = address;
        Stop(emu, X86StopReason::MemoryFault, fetch ? "Guest instruction fetch was refused." : "Guest data memory access was refused.");
        return 1;
    }
    return 0;
}

int BeforeInstruction(x86emu_t* emu) noexcept {
    auto& context = Context(emu);
    if (context.stopped) return 1;
    if (context.instructions >= context.config->instructionLimit) {
        Stop(emu, X86StopReason::InstructionLimit, "Guest instruction budget exhausted.");
        return 1;
    }
    ++context.instructions;
    if (!context.config->hostCall) return 0;
    context.hostRegisters = Registers(emu);
    context.action = context.config->hostCall(context.config->context, context.hostRegisters);
    if (context.action == X86HostCallAction::Fault) {
        Stop(emu, X86StopReason::HostFault, "Host-call dispatcher refused the guest call.");
        return 1;
    }
    return context.action != X86HostCallAction::NotHandled;
}

int Interrupt(x86emu_t* emu, u8 vector, unsigned) noexcept {
    Stop(emu, vector == 6 ? X86StopReason::UnsupportedInstruction : X86StopReason::HostFault,
         vector == 6 ? "Guest instruction is unsupported by the interpreter." : "Guest CPU interrupt or exception was not handled.");
    return 1;
}

void Cpuid(x86emu_t* emu) noexcept {
    const auto leaf = emu->x86.R_EAX;
    emu->x86.R_EAX = emu->x86.R_EBX = emu->x86.R_ECX = emu->x86.R_EDX = 0;
    if (leaf == 0) {
        emu->x86.R_EAX = 1;
        emu->x86.R_EBX = 0x4d444358; // XCDM
        emu->x86.R_EDX = 0x74726956; // Virt
        emu->x86.R_ECX = 0x206c6175; // ual
    } else if (leaf == 1) {
        emu->x86.R_EAX = 0x00000300; // Integer i386 profile; no vector/FPU features advertised.
    }
}

void Msr(x86emu_t* emu) noexcept {
    Stop(emu, X86StopReason::IoDenied, "Guest privileged MSR access is disabled.");
}

std::string Hex(std::uint32_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text = "0x00000000";
    for (unsigned i = 0; i < 8; ++i) { text[9 - i] = digits[value & 15U]; value >>= 4U; }
    return text;
}

void DeleteEmulator(x86emu_t* emu) noexcept { (void)x86emu_done(emu); }

} // namespace

X86ExecutionResult RunX86(const X86ExecutionConfig& config) noexcept {
    X86ExecutionResult result;
    result.registers = config.initial;
    try {
        if (!config.readMemory || !config.writeMemory || !config.instructionLimit ||
            config.instructionLimit > kMaxInstructions) {
            result.message = "A memory backend and an instruction limit in 1..1000000 are required.";
            return result;
        }
        std::unique_ptr<x86emu_t, decltype(&DeleteEmulator)> emu{x86emu_new(0, 0), DeleteEmulator};
        if (!emu) {
            result.reason = X86StopReason::InitializationFailed;
            result.message = "Unable to allocate the x86 interpreter.";
            return result;
        }
        RunContext context;
        context.config = &config;
        context.memoryLimit = config.instructionLimit * 16U + 256U;
        emu->_private = &context;
        SetRegisters(emu.get(), config.initial);
        // Flat protected-mode cached descriptors. All effective memory remains
        // virtual and is resolved only through the callbacks above.
        emu->x86.R_CR0 = 1;
        for (std::size_t i = 0; i < 6; ++i) {
            emu->x86.seg[i].base = 0;
            emu->x86.seg[i].limit = 0xffffffffU;
            // libx86emu stores descriptor flags compactly: D/B is bit 10,
            // rather than bit 14 of the descriptor's original high word.
            emu->x86.seg[i].acc = i == R_CS_INDEX ? 0x04fb : 0x04f3;
            emu->x86.seg[i].sel = i == R_CS_INDEX ? 0x1b : 0x23;
        }
        x86emu_set_memio_handler(emu.get(), Memory);
        x86emu_set_code_handler(emu.get(), BeforeInstruction);
        x86emu_set_intr_handler(emu.get(), Interrupt);
        x86emu_set_cpuid_handler(emu.get(), Cpuid);
        x86emu_set_rdmsr_handler(emu.get(), Msr);
        x86emu_set_wrmsr_handler(emu.get(), Msr);
        for (;;) {
            context.action = X86HostCallAction::NotHandled;
            (void)x86emu_run(emu.get(), 0);
            if (context.stopped) break;
            if (context.action == X86HostCallAction::Continue || context.action == X86HostCallAction::Exit) {
                SetRegisters(emu.get(), context.hostRegisters);
                if (context.action == X86HostCallAction::Continue) continue;
                context.reason = X86StopReason::Exited;
                context.message = "Guest exited through the host-call dispatcher.";
            }
            break;
        }
        result.registers = Registers(emu.get());
        result.instructions = context.instructions;
        result.reason = context.reason;
        result.ok = context.reason == X86StopReason::Exited;
        result.message = context.message;
        if (context.hasFaultAddress) result.message += " Address: " + Hex(context.faultAddress) + ".";
        return result;
    } catch (...) {
        result.ok = false;
        result.reason = X86StopReason::InitializationFailed;
        try { result.message = "Unable to allocate interpreter result state."; } catch (...) {}
        return result;
    }
}

} // namespace xcdm::runtime
