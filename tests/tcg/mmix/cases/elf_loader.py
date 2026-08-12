#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *


def bootinfo_probe_program():
    bootinfo = expected_bootinfo()
    fields = {
        R4: "magic",
        R5: "version",
        R6: "size",
        R7: "ram_size",
        R8: "low_ram_size",
        R9: "pool_logical_base",
        R10: "pool_phys_base",
        R11: "data_logical_base",
        R12: "data_phys_base",
        R13: "stack_logical_base",
        R14: "stack_phys_base",
        R15: "mmio_base",
        R16: "uart_base",
        R17: "timer_base",
        R18: "framebuffer_base",
    }
    program = [
        insn(ADDI, R2, R0, 0),
        insn(ADDI, R3, R1, 0),
    ]

    for reg, field in fields.items():
        offset = MMIX_BOOTINFO_FIELDS.index(field) * 8
        program.extend([
            wyde(SETL, R19, offset),
            insn(LDOU, reg, R1, R19),
        ])

    program.append(halt())
    regs = {
        R2: 0,
        R3: MMIX_BOOTINFO_PHYS_BASE,
        **{reg: bootinfo[field] for reg, field in fields.items()},
    }
    return b"".join(program), (len(program) - 1) * 4, regs


BOOTINFO_PROBE = bootinfo_probe_program()

ELF_LOADER_TESTS = [
    MMIXMMOTest(
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
    MMIXMMOTest(
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
    MMIXMMOTest(
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
        regs={R2: 0, R3: MMIX_BOOTINFO_PHYS_BASE, R4: 0x7e},
    ),
    MMIXMMOTest(
        "elf-bootinfo",
        elf64_image(
            0,
            BOOTINFO_PROBE[0],
        ),
        pc=BOOTINFO_PROBE[1],
        regs=BOOTINFO_PROBE[2],
    ),
]
