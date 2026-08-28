#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import (
    ADDI,
    GET,
    LDOU,
    MMIXELFTest,
    MMIXProcessFailure,
    R0,
    R1,
    R32,
    R33,
    R34,
    R35,
    R36,
    SR_G,
    SR_L,
    elf64_image,
    halt,
    insn,
    set_octa,
)


BARE_ENTRY = 0x2000
BARE_BSS = BARE_ENTRY + 0x40

BARE_PROGRAM = b"".join((
    insn(ADDI, R32, R0, 0),
    insn(ADDI, R33, R1, 0),
    insn(GET, R34, 0, SR_L),
    insn(GET, R35, 0, SR_G),
    *set_octa(R36, BARE_BSS),
    insn(LDOU, R36, R36, R0),
    halt(),
))

BARE_ELF_TESTS = [
    MMIXELFTest(
        "elf-bare-entry-and-bss",
        elf64_image(
            BARE_ENTRY,
            BARE_PROGRAM,
            mem_size=BARE_BSS + 8 - BARE_ENTRY,
            entry=BARE_ENTRY,
        ),
        pc=BARE_ENTRY + len(BARE_PROGRAM) - 4,
        regs={R32: 0, R33: 0, R34: 0, R35: 32, R36: 0},
    ),
    MMIXELFTest(
        "elf-bare-semihosting-enabled",
        elf64_image(BARE_ENTRY, BARE_PROGRAM, entry=BARE_ENTRY),
        pc=BARE_ENTRY + len(BARE_PROGRAM) - 4,
        regs={R32: 0, R33: 0, R34: 0, R35: 32, R36: 0},
        qemu_args=("-semihosting",),
    ),
    MMIXELFTest(
        "elf-bare-entry-above-4g",
        elf64_image(0x100000000, halt(), entry=0x100000000),
        pc=0x100000000,
        regs={},
        qemu_args=("-m", "8G"),
    ),
]

BARE_ELF_REJECTION_TESTS = [
    MMIXProcessFailure(
        "elf-bare-smp",
        elf64_image(0, halt()),
        ("-smp", "2"),
        ("startup ABI 'bare' requires exactly one CPU",),
    ),
    MMIXProcessFailure(
        "elf-bare-semihosting-arguments",
        elf64_image(0, halt()),
        ("-semihosting-config", "enable=on,arg=program"),
        ("startup ABI 'bare' does not accept semihosting arguments",),
    ),
    MMIXProcessFailure(
        "elf-bare-append",
        elf64_image(0, halt()),
        ("-append", "argument"),
        ("startup ABI 'bare' does not accept -append",),
    ),
    MMIXProcessFailure(
        "elf-bare-initrd",
        elf64_image(0, halt()),
        ("-initrd", "$IMAGE"),
        ("startup ABI 'bare' does not accept -initrd",),
    ),
]
