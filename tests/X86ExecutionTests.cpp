#include "runtime/X86Execution.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace xcdm::runtime;
constexpr std::uint32_t kBase = 0x400000;
constexpr std::uint32_t kTicks = 0x7f000001;
constexpr std::uint32_t kExit = 0x7f000002;
int failures = 0;
std::size_t checks = 0;

void Check(bool condition, const char* name) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}

struct Guest {
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(0x5000);
    unsigned tickCalls = 0, exitCalls = 0, memoryCalls = 0, writes = 0;
    std::uint32_t exitCode = 0, ticksReturnEsp = 0;
    bool denyExecute = false, denyHost = false, continueForever = false;
};

bool Read(void* opaque, std::uint32_t address, std::uint8_t* output,
          std::size_t size, bool execute) noexcept {
    auto& guest = *static_cast<Guest*>(opaque);
    ++guest.memoryCalls;
    if (address < kBase) return false;
    const std::uint64_t offset = static_cast<std::uint64_t>(address) - kBase;
    if (offset > guest.memory.size() || size > guest.memory.size() - offset ||
        (execute && (guest.denyExecute || offset >= 0x1000 || size > 0x1000 - offset))) return false;
    std::copy_n(guest.memory.data() + static_cast<std::size_t>(offset), size, output);
    return true;
}

bool Write(void* opaque, std::uint32_t address, const std::uint8_t* input, std::size_t size) noexcept {
    auto& guest = *static_cast<Guest*>(opaque);
    ++guest.memoryCalls;
    if (address < kBase + 0x2000) return false; // Code/read-only data cannot be written.
    const std::uint64_t offset = static_cast<std::uint64_t>(address) - kBase;
    if (offset > guest.memory.size() || size > guest.memory.size() - offset) return false;
    std::copy_n(input, size, guest.memory.data() + static_cast<std::size_t>(offset));
    ++guest.writes;
    return true;
}

bool Get(Guest& guest, std::uint32_t address, std::uint32_t& value) noexcept {
    std::array<std::uint8_t, 4> bytes{};
    if (!Read(&guest, address, bytes.data(), bytes.size(), false)) return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[i]) << (i * 8);
    return true;
}

void Put(Guest& guest, std::uint32_t address, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) guest.memory[address - kBase + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

X86HostCallAction Host(void* opaque, X86Registers& registers) noexcept {
    auto& guest = *static_cast<Guest*>(opaque);
    if (guest.continueForever) return X86HostCallAction::Continue;
    if (registers.eip != kTicks && registers.eip != kExit) return X86HostCallAction::NotHandled;
    if (guest.denyHost) return X86HostCallAction::Fault;
    std::uint32_t returnAddress = 0;
    if (!Get(guest, registers.esp, returnAddress)) return X86HostCallAction::Fault;
    if (registers.eip == kTicks) {
        ++guest.tickCalls;
        registers.eax = 100;
        registers.esp += 4; // stdcall, zero parameters: pop the guest return address.
        registers.eip = returnAddress;
        guest.ticksReturnEsp = registers.esp;
        return X86HostCallAction::Continue;
    }
    if (!Get(guest, registers.esp + 4, guest.exitCode)) return X86HostCallAction::Fault;
    ++guest.exitCalls;
    return X86HostCallAction::Exit;
}

X86ExecutionConfig Config(Guest& guest, std::uint64_t budget = 100) {
    X86ExecutionConfig config;
    config.context = &guest;
    config.readMemory = Read;
    config.writeMemory = Write;
    config.hostCall = Host;
    config.initial.eip = kBase;
    config.initial.esp = kBase + 0x4ff0;
    config.instructionLimit = budget;
    Put(guest, kBase + 0x2000, kTicks);
    Put(guest, kBase + 0x2004, kExit);
    return config;
}

X86ExecutionResult Run(Guest& guest, const std::vector<std::uint8_t>& code, std::uint64_t budget = 100) {
    std::copy(code.begin(), code.end(), guest.memory.begin());
    return RunX86(Config(guest, budget));
}

void HostCallsAndIntegerExecution() {
    Guest guest;
    auto result = Run(guest, {
        0xff, 0x15, 0x00, 0x20, 0x40, 0x00, // call [GetTickCount IAT]
        0x83, 0xc0, 0x05,                   // add eax, 5
        0x50,                               // push eax
        0xff, 0x15, 0x04, 0x20, 0x40, 0x00, // call [ExitProcess IAT]
        0xf4
    });
    Check(result.ok && result.reason == X86StopReason::Exited, "guest exits only through an explicit host call");
    Check(guest.tickCalls == 1 && guest.exitCalls == 1 && guest.exitCode == 105,
          "indirect guest calls and host return value execute with Win32-style stack frames");
    Check(guest.ticksReturnEsp == kBase + 0x4ff0, "stdcall zero-argument return restores ESP");
    Check(result.registers.eax == 105 && result.instructions == 6, "32-bit arithmetic and dispatcher accounting");

    Guest loop;
    result = Run(loop, {
        0xb8, 0x00, 0x00, 0x00, 0x00, // mov eax, 0
        0xb9, 0x05, 0x00, 0x00, 0x00, // mov ecx, 5
        0x01, 0xc8,                   // add eax, ecx
        0xe2, 0xfc,                   // loop back to add
        0x50,
        0xff, 0x15, 0x04, 0x20, 0x40, 0x00
    });
    Check(result.ok && loop.exitCode == 15 && result.registers.ecx == 0,
          "ordinary 32-bit LOOP and arithmetic run through the imported core");

    Guest copy;
    const std::array<std::uint8_t, 8> pattern{{1, 4, 9, 16, 25, 36, 49, 64}};
    std::copy(pattern.begin(), pattern.end(), copy.memory.begin() + 0x2200);
    result = Run(copy, {
        0xb9, 0x08, 0x00, 0x00, 0x00, // mov ecx, 8
        0xbe, 0x00, 0x22, 0x40, 0x00, // mov esi, 0x402200
        0xbf, 0x00, 0x23, 0x40, 0x00, // mov edi, 0x402300
        0xf3, 0xa4,                   // rep movsb
        0x6a, 0x00,
        0xff, 0x15, 0x04, 0x20, 0x40, 0x00
    });
    Check(result.ok && std::equal(pattern.begin(), pattern.end(), copy.memory.begin() + 0x2300),
          "bounded REP MOVSB uses guest virtual memory callbacks");
    Check(result.registers.esi == 0x402208 && result.registers.edi == 0x402308,
          "string instruction updates 32-bit source/destination registers");
}

void FaultsAndBudgets() {
    Guest loop;
    auto result = Run(loop, {0xeb, 0xfe}, 50);
    Check(!result.ok && result.reason == X86StopReason::InstructionLimit && result.instructions == 50,
          "infinite guest loop stops at the instruction budget");
    Guest gate;
    gate.continueForever = true;
    result = Run(gate, {0x90}, 20);
    Check(!result.ok && result.reason == X86StopReason::InstructionLimit && result.instructions == 20,
          "host-call continuations cannot bypass the dispatch budget");
    Guest rep;
    result = Run(rep, {
        0xb9, 0xff, 0xff, 0xff, 0xff,
        0xbe, 0x00, 0x22, 0x40, 0x00,
        0xbf, 0x00, 0x23, 0x40, 0x00,
        0xf3, 0xa4
    }, 10);
    Check(!result.ok && result.reason == X86StopReason::InstructionLimit && rep.memoryCalls < 500,
          "huge REP work stops inside the instruction, without billions of iterations");
    Guest invalidRead;
    result = Run(invalidRead, {0xa1, 0xfc, 0xff, 0xff, 0xff});
    Check(!result.ok && result.reason == X86StopReason::MemoryFault, "invalid guest address is refused, not cast to a native pointer");
    Guest readOnly;
    result = Run(readOnly, {0xa3, 0x00, 0x00, 0x40, 0x00});
    Check(!result.ok && result.reason == X86StopReason::MemoryFault && !readOnly.writes,
          "guest code page writes respect backend permissions");
    Guest nx;
    nx.denyExecute = true;
    result = Run(nx, {0x90});
    Check(!result.ok && result.reason == X86StopReason::MemoryFault, "instruction fetch requests execute access");
    Guest wrap;
    result = Run(wrap, {0xa1, 0xff, 0xff, 0xff, 0xff});
    Check(!result.ok && result.reason == X86StopReason::MemoryFault,
          "32-bit address-space crossing is rejected before the memory backend");
    Guest denied;
    denied.denyHost = true;
    result = Run(denied, {0xff, 0x15, 0x00, 0x20, 0x40, 0x00});
    Check(!result.ok && result.reason == X86StopReason::HostFault && !denied.tickCalls,
          "host dispatcher failure is terminal");
    Guest halt;
    result = Run(halt, {0xf4});
    Check(!result.ok && result.reason == X86StopReason::Halted, "guest HLT is not reported as successful program exit");
    Guest io;
    result = Run(io, {0xec});
    Check(!result.ok && result.reason == X86StopReason::IoDenied, "guest input port instruction never reaches physical I/O");
    Guest msr;
    result = Run(msr, {0x0f, 0x32});
    Check(!result.ok && result.reason == X86StopReason::IoDenied, "guest MSR instruction is refused");
    Guest prefixes;
    std::vector<std::uint8_t> longInstruction(30, 0x66);
    result = Run(prefixes, longInstruction);
    Check(!result.ok && result.reason == X86StopReason::UnsupportedInstruction && prefixes.memoryCalls <= 16,
          "overlong prefix stream stops at the x86 instruction-length limit");
    Guest exactLength;
    std::vector<std::uint8_t> longest(14, 0x66); longest.push_back(0xf4);
    result = Run(exactLength, longest);
    Check(result.reason == X86StopReason::Halted, "15-byte instruction remains within the architectural limit");
}

void GuestThreadSegment() {
    Guest guest;
    auto config = Config(guest);
    config.fsBase = kBase + 0x2100;
    Put(guest, config.fsBase + 4, 0x12345678);
    const std::vector<std::uint8_t> code{
        0x64, 0xa1, 0x04, 0x00, 0x00, 0x00, // mov eax, fs:[4]
        0x64, 0xa3, 0x08, 0x00, 0x00, 0x00, // mov fs:[8], eax
        0x50,
        0xff, 0x15, 0x04, 0x20, 0x40, 0x00
    };
    std::copy(code.begin(), code.end(), guest.memory.begin());
    const auto result = RunX86(config);
    std::uint32_t stored = 0;
    Check(result.ok && guest.exitCode == 0x12345678, "FS-relative read uses the configured guest TEB base");
    Check(Get(guest, config.fsBase + 8, stored) && stored == 0x12345678,
          "FS-relative write reaches guest memory through normal permissions");
    Check(result.registers.eax == 0x12345678, "guest FS operations preserve the expected 32-bit value");

    Guest missing;
    auto invalid = Config(missing);
    invalid.fsBase = 0x7ffde000;
    std::copy(code.begin(), code.end(), missing.memory.begin());
    const auto fault = RunX86(invalid);
    Check(!fault.ok && fault.reason == X86StopReason::MemoryFault,
          "an unmapped guest FS base faults without accessing the host TEB");
}

X86ExecutionResult OneInstruction(const std::vector<std::uint8_t>& instruction,
                                  X86Registers initial) {
    Guest guest;
    std::copy(instruction.begin(), instruction.end(), guest.memory.begin());
    auto config = Config(guest, 1);
    initial.eip = kBase;
    initial.esp = config.initial.esp;
    config.initial = initial;
    return RunX86(config);
}

void DivisionExceptionsAndSignedMultiply() {
    for (bool word : {false, true}) {
        for (bool hostMinimum : {false, true}) {
            X86Registers initial;
            initial.ecx = 0xffffffff;
            // Both the host signed division minimum and the smaller guest
            // quotient overflow must raise guest #DE before host C divides.
            initial.eax = hostMinimum ? 0 : (word ? 0x8000 : 0x80000000);
            initial.edx = hostMinimum ? (word ? 0x8000 : 0x80000000) : 0xffffffff;
            std::vector<std::uint8_t> instruction;
            if (word) instruction.push_back(0x66);
            instruction.insert(instruction.end(), {0xf7, 0xf9}); // idiv cx/ecx
            const auto result = OneInstruction(instruction, initial);
            Check(!result.ok && result.reason == X86StopReason::HostFault,
                  "signed divide overflow raises a guest exception without host undefined division");
        }
    }
    X86Registers initial;
    initial.eax = 255;
    const auto aam = OneInstruction({0xd4, 0x00}, initial);
    Check(!aam.ok && aam.reason == X86StopReason::HostFault,
          "AAM base zero raises guest divide error without dividing the host by zero");

    struct Product { std::uint32_t left, right, low; bool overflow; };
    for (const auto& product : std::array<Product, 4>{{
        {0x80000000, 1, 0x80000000, false},
        {0x80000000, 0xffffffff, 0x80000000, true},
        {0x40000000, 2, 0x80000000, true},
        {0xffffffff, 2, 0xfffffffe, false}
    }}) {
        initial.eax = product.left;
        initial.ecx = product.right;
        const auto result = OneInstruction({0x0f, 0xaf, 0xc1}, initial); // imul eax,ecx
        Check(result.reason == X86StopReason::InstructionLimit && result.registers.eax == product.low,
              "signed multiplication retains the low 32 bits without signed-mask undefined behavior");
        Check((result.registers.eflags & 0x801U) == (product.overflow ? 0x801U : 0),
              "IMUL carry and overflow reflect signed result representability");
    }
    initial.eax = 0x80000000;
    initial.ecx = 31;
    const auto bit = OneInstruction({0x0f, 0xa3, 0xc8}, initial); // bt eax,ecx
    Check(bit.reason == X86StopReason::InstructionLimit && (bit.registers.eflags & 1U),
          "BT bit 31 uses an unsigned mask and reports the original high bit");
}

bool EvenParity(std::uint32_t value) {
    unsigned ones = 0;
    for (unsigned bit = 0; bit < 8; ++bit) ones += (value >> bit) & 1U;
    return (ones & 1U) == 0;
}

#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
struct NativeRotateResult { std::uint32_t value, flags; };
NativeRotateResult NativeRotate(std::uint32_t value, unsigned count, unsigned carry, bool right) {
    unsigned char cf = 0, of = 0;
    if (right) {
        __asm__ volatile("btl $0, %[carry]; rcrl %%cl, %[value]; setc %[cf]; seto %[of]"
            : [value] "+r"(value), [cf] "=qm"(cf), [of] "=qm"(of)
            : "c"(count), [carry] "r"(carry) : "cc");
    } else {
        __asm__ volatile("btl $0, %[carry]; rcll %%cl, %[value]; setc %[cf]; seto %[of]"
            : [value] "+r"(value), [cf] "=qm"(cf), [of] "=qm"(of)
            : "c"(count), [carry] "r"(carry) : "cc");
    }
    return {value, static_cast<std::uint32_t>(cf) | (static_cast<std::uint32_t>(of) << 11)};
}
#endif

void ShiftAndRotateEdges() {
    constexpr std::uint32_t arithmeticFlags = 0x8d5;
    for (unsigned width : {8U, 16U, 32U}) {
        const std::uint32_t mask = width == 32 ? 0xffffffffU : (1U << width) - 1;
        const std::uint32_t sign = 1U << (width - 1);
        for (unsigned operation : {4U, 5U, 7U}) { // SHL, SHR, SAR
            for (unsigned count : {0U, 1U, 7U, 8U, 15U, 16U, 31U, 32U, 63U, 255U}) {
                for (std::uint32_t value : {0U, 1U, sign, mask}) {
                    X86Registers initial;
                    initial.eax = (0xa5a50000U & ~mask) | value;
                    initial.eflags = arithmeticFlags | 2U;
                    std::vector<std::uint8_t> instruction;
                    if (width == 16) instruction.push_back(0x66);
                    instruction.insert(instruction.end(), {
                        static_cast<std::uint8_t>(width == 8 ? 0xc0 : 0xc1),
                        static_cast<std::uint8_t>(0xc0 | (operation << 3)),
                        static_cast<std::uint8_t>(count)});
                    const auto result = OneInstruction(instruction, initial);
                    // Independent single-bit reference avoids C shifts by a
                    // host type's width even for counts 31/32/63/255.
                    std::uint32_t expected = value, carry = 0;
                    const unsigned effective = count & 31U;
                    for (unsigned step = 0; step < effective; ++step) {
                        if (operation == 4) {
                            carry = (expected & sign) != 0;
                            expected = (expected << 1) & mask;
                        } else {
                            carry = expected & 1U;
                            expected = (expected >> 1) | (operation == 7 ? expected & sign : 0);
                        }
                    }
                    expected |= initial.eax & ~mask;
                    Check(result.reason == X86StopReason::InstructionLimit && result.registers.eax == expected,
                          "shift result and untouched high register bits match masked x86 count semantics");
                    if (!effective) {
                        Check((result.registers.eflags & arithmeticFlags) == arithmeticFlags,
                              "zero masked shift count preserves arithmetic flags");
                        continue;
                    }
                    std::uint32_t definedMask = 0xc4; // SF, ZF, PF; AF is undefined.
                    std::uint32_t expectedFlags =
                        ((expected & sign) ? 0x80U : 0) |
                        ((expected & mask) == 0 ? 0x40U : 0) |
                        (EvenParity(expected) ? 4U : 0);
                    // CF is undefined for oversized logical shifts; SAR's
                    // repeated sign bit remains defined for these counts.
                    if (effective < width || operation == 7) {
                        definedMask |= 1U;
                        expectedFlags |= carry;
                    }
                    if (effective == 1) {
                        definedMask |= 0x800U;
                        const bool overflow = operation == 4
                            ? (((expected & sign) != 0) != (carry != 0))
                            : operation == 5 && (value & sign);
                        if (overflow) expectedFlags |= 0x800U;
                    }
                    if ((result.registers.eflags & definedMask) != expectedFlags)
                        std::cerr << "shift width=" << width << " operation=" << operation << " count=" << count
                                  << " value=" << std::hex << value << " expected flags=" << expectedFlags
                                  << " actual=" << (result.registers.eflags & definedMask) << std::dec << '\n';
                    Check((result.registers.eflags & definedMask) == expectedFlags,
                          "shift flags match only architecturally defined result/CF/OF bits");
                }
            }
        }
    }

    for (bool right : {false, true}) {
        for (unsigned count : {0U, 1U, 31U, 32U, 63U, 255U}) {
            for (std::uint32_t value : {0U, 1U, 0x80000000U, 0xffffffffU, 0x12345678U}) {
                for (unsigned carry : {0U, 1U}) {
                    X86Registers initial;
                    initial.eax = value;
                    initial.eflags = (arithmeticFlags & ~1U) | 2U | carry;
                    const auto result = OneInstruction({0xc1,
                        static_cast<std::uint8_t>(right ? 0xd8 : 0xd0),
                        static_cast<std::uint8_t>(count)}, initial);
                    std::uint32_t expected = value;
                    unsigned expectedCarry = carry;
                    for (unsigned step = 0; step < (count & 31U); ++step) {
                        const unsigned nextCarry = right ? expected & 1U : expected >> 31;
                        expected = right ? (expected >> 1) | (expectedCarry << 31)
                                         : (expected << 1) | expectedCarry;
                        expectedCarry = nextCarry;
                    }
                    Check(result.reason == X86StopReason::InstructionLimit && result.registers.eax == expected &&
                          (result.registers.eflags & 1U) == expectedCarry,
                          "RCL/RCR32 result and carry match the 33-bit rotate reference");
                    Check((result.registers.eflags & 0xd4U) == (initial.eflags & 0xd4U),
                          "rotate-through-carry preserves SF/ZF/AF/PF");
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
                    const auto native = NativeRotate(value, count, carry, right);
                    const auto nativeMask = (count & 31U) == 1 ? 0x801U : 1U;
                    Check(native.value == result.registers.eax &&
                          (native.flags & nativeMask) == (result.registers.eflags & nativeMask),
                          "RCL/RCR32 matches native x86 results and defined CF/OF");
#endif
                    if ((count & 31U) <= 1) {
                        const bool overflow = right ? ((expected >> 31) ^ (expected >> 30)) & 1U
                                                    : ((expected >> 31) ^ expectedCarry) & 1U;
                        const unsigned expectedOf = (count & 31U) ? (overflow ? 0x800U : 0) : initial.eflags & 0x800U;
                        Check((result.registers.eflags & 0x800U) == expectedOf,
                              "RCL/RCR32 single-bit overflow and zero-count flags are defined correctly");
                    }
                }
            }
        }
    }
}

void UnsupportedExtensionsAndConfiguration() {
    const std::vector<std::vector<std::uint8_t>> instructions{
        {0x0f, 0x0b},       // UD2
        {0xd9, 0xee},       // FLDZ
        {0xd8, 0xc0},       // FADD ST, ST
        {0x0f, 0x77},       // EMMS
        {0x0f, 0xef, 0xc0}, // PXOR MM0, MM0
        {0x0f, 0x10, 0xc0}, // MOVUPS XMM0, XMM0 (upstream's partial SSE is disabled)
        {0x0f, 0x57, 0xc0}, // XORPS XMM0, XMM0
        {0xf3, 0x0f, 0x58, 0xc0} // ADDSS
    };
    for (const auto& instruction : instructions) {
        Guest guest;
        const auto result = Run(guest, instruction);
        Check(!result.ok && result.reason == X86StopReason::UnsupportedInstruction,
              "FPU/MMX/SSE or undefined instructions are refused, not silently skipped");
    }
    Guest cpu;
    auto result = Run(cpu, {
        0xb8, 0x01, 0x00, 0x00, 0x00,
        0x0f, 0xa2,
        0x52,
        0xff, 0x15, 0x04, 0x20, 0x40, 0x00
    });
    Check(result.ok && cpu.exitCode == 0 && result.registers.ecx == 0,
          "guest CPUID does not advertise host FPU/vector/AVX features");
    Guest guest;
    auto config = Config(guest, 0);
    Check(RunX86(config).reason == X86StopReason::InvalidConfiguration, "zero budget rejected");
    config.instructionLimit = 1000001;
    Check(RunX86(config).reason == X86StopReason::InvalidConfiguration, "oversized budget rejected");
    config.instructionLimit = 10; config.readMemory = nullptr;
    Check(RunX86(config).reason == X86StopReason::InvalidConfiguration, "missing virtual memory callback rejected");
}

} // namespace

int main() {
    HostCallsAndIntegerExecution();
    FaultsAndBudgets();
    GuestThreadSegment();
    DivisionExceptionsAndSignedMultiply();
    ShiftAndRotateEdges();
    UnsupportedExtensionsAndConfiguration();
    std::cout << checks << " x86 interpreter checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
