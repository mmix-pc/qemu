#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *


def bootinfo_probe_program():
    bootinfo = expected_bootinfo()
    fields = {
        index + R4: field
        for index, field in enumerate(MMIX_BOOTINFO_FIELDS)
    }
    program = [
        insn(ADDI, R2, R0, 0),
        insn(ADDI, R3, R1, 0),
    ]

    for reg, field in fields.items():
        offset = MMIX_BOOTINFO_FIELDS.index(field) * 8
        program.extend([
            wyde(SETL, R50, offset),
            insn(LDOU, reg, R1, R50),
        ])

    program.append(halt())
    regs = {
        R2: 0,
        R3: MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0],
        **{reg: bootinfo[field] for reg, field in fields.items()},
    }
    return b"".join(program), (len(program) - 1) * 4, regs


BOOTINFO_PROBE = bootinfo_probe_program()


def platform_probe_program():
    entry = 0x1000
    kernel_stack_top = 0x00500000
    kernel_stack_value = 0x1122334455667788
    segment_stack_value = 0x8877665544332211

    def bootinfo_load(reg, field):
        offset = MMIX_BOOTINFO_FIELDS.index(field) * 8
        return insn(LDOUI, reg, R1, offset)

    program = [
        # Preserve the QEMU-provided ELF entry state before setting up a stack.
        insn(ADDI, R20, R0, 0),
        insn(ADDI, R21, R1, 0),
        insn(ADDI, R22, R254, 0),
        *set_octa(R254, kernel_stack_top),
        insn(SUBUI, R254, R254, 8),
        *set_octa(R23, kernel_stack_value),
        insn(STOU, R23, R254, R0),
        insn(LDOU, R24, R254, R0),

        # Discover the logical Stack Segment and exercise one bounded octa.
        bootinfo_load(R25, "stack_logical_base"),
        bootinfo_load(R26, "stack_size"),
        wyde(SETL, R46, 0x100),
        insn(ADDU, R27, R25, R46),
        *set_octa(R28, segment_stack_value),
        insn(STOU, R28, R27, R0),
        insn(LDOU, R29, R27, R0),

        # Read the early platform-discovery fields used by an OS kernel.
        bootinfo_load(R30, "magic"),
        bootinfo_load(R31, "cpu_count"),
        bootinfo_load(R32, "boot_cpu_id"),
        bootinfo_load(R33, "low_ram_base"),
        bootinfo_load(R34, "low_ram_size"),
        bootinfo_load(R35, "uart_base"),
        bootinfo_load(R36, "timer_base"),
        bootinfo_load(R37, "intc_base"),
        bootinfo_load(R38, "timer_irq_base"),
        bootinfo_load(R39, "intc_irq_count"),

        # Inspect deterministic CPU0 timer and interrupt-controller state.
        wyde(SETL, R45, MMIX_VIRT_TIMER_CONTEXT_BASE),
        insn(LDOU, R40, R36, R45),
        insn(ADDUI, R45, R45, MMIX_VIRT_TIMER_CONTEXT_STATUS),
        insn(LDOU, R41, R36, R45),
        insn(LDTUI, R42, R37, MMIX_VIRT_INTC_PENDING),

        # Emit one polling-UART byte through the discovered 16550 base.
        insn(LDBUI, R43, R35, MMIX_VIRT_UART0_LSR),
        insn(ANDI, R43, R43, MMIX_VIRT_UART0_LSR_THRE),
        branch(BZB, R43, 0xfffe),
        wyde(SETL, R44, ord("P")),
        insn(STBI, R44, R35, MMIX_VIRT_UART0_THR),
        halt(),
    ]
    regs = {
        R20: 0,
        R21: MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0],
        R22: 0,
        R24: kernel_stack_value,
        R25: MMIX_STACK_SEGMENT_BASE,
        R26: MMIX_STACK_SEGMENT_SIZE,
        R27: MMIX_STACK_SEGMENT_BASE + 0x100,
        R29: segment_stack_value,
        R30: MMIX_BOOTINFO_MAGIC,
        R31: 1,
        R32: 0,
        R33: MMIX_LOW_RAM_BASE,
        R34: MMIX_LOW_RAM_SIZE,
        R35: MMIX_VIRT_UART0_BASE,
        R36: MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0],
        R37: MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0],
        R38: MMIX_VIRT_TIMER_IRQ_BASE,
        R39: MMIX_VIRT_INTC_IRQ_COUNT,
        R40: 0,
        R41: 0,
        R42: 0,
    }
    return b"".join(program), entry + (len(program) - 1) * 4, regs


PLATFORM_PROBE = platform_probe_program()

ELF_LOADER_TESTS = [
    MMIXELFTest(
        "elf-load-low-ram-segment",
        elf64_image(
            0,
            b"".join([
                wyde(SETL, R1, 0x42),
                halt(),
            ]),
        ),
        pc=0x04,
        regs={R1: 0x42},
    ),
    MMIXELFTest(
        "elf-load-bss-zero-fill",
        elf64_image(
            0,
            b"".join([
                *set_octa(R1, 0x100),
                insn(LDOU, R2, R1, R0),
                halt(),
            ]),
            mem_size=0x108,
        ),
        pc=0x14,
        regs={R2: 0},
    ),
    MMIXELFTest(
        "elf-entry-state",
        elf64_image(
            0x100,
            b"".join([
                insn(ADDI, R2, R0, 0),
                insn(ADDI, R3, R1, 0),
                wyde(SETL, R4, 0x7e),
                halt(),
            ]),
            entry=0x100,
        ),
        pc=0x10c,
        regs={R2: 0, R3: MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0], R4: 0x7e},
    ),
    MMIXELFTest(
        "elf-bootinfo",
        elf64_image(
            0,
            BOOTINFO_PROBE[0],
        ),
        pc=BOOTINFO_PROBE[1],
        regs=BOOTINFO_PROBE[2],
    ),
    MMIXELFTest(
        "elf-single-core-platform",
        elf64_image(
            0x1000,
            PLATFORM_PROBE[0],
            entry=0x1000,
        ),
        pc=PLATFORM_PROBE[1],
        regs=PLATFORM_PROBE[2],
        output=b"P",
    ),
]
