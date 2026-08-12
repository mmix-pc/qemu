#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

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
        regs={R2: 0, R3: 0, R4: 0x7e},
    ),
]
