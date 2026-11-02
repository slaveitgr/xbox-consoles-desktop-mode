# SPDX-License-Identifier: GPL-2.0-only
# Self-authored Linux x86-64 ELF payload: arithmetic, memory, branches, write, exit.
# It runs in the real Boxedwine64 interpreter, never directly on the host.
.global _start
.section .text
_start:
    mov $7, %eax
    imul $6, %rax, %rax
.ifdef NEGATIVE_CONTROL
    cmp $43, %rax
.else
    cmp $42, %rax
.endif
    jne failure
    lea scratch(%rip), %rbx
    mov %rax, (%rbx)
    xor %eax, %eax
    mov (%rbx), %r8
    cmp $42, %r8
    jne failure
    mov $1, %eax
    mov $1, %edi
    lea message(%rip), %rsi
    mov $message_end-message, %edx
    syscall
    cmp $message_end-message, %rax
    jne failure
    mov $60, %eax
    xor %edi, %edi
    syscall
failure:
    mov $60, %eax
    mov $9, %edi
    syscall
.section .rodata
message: .ascii "ProtonDesktop guest-x64 interpreter: PASS\n"
message_end:
.section .bss
.balign 8
scratch: .skip 8
.section .note.GNU-stack,"",@progbits
