#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

SEMIHOSTING_TESTS = [
    MMIXTest(
        "semihosting-halt",
        insn(TRAP, 0, MMIX_SEMIHOSTING_HALT, 0),
        pc=0x00,
        regs={},
    ),
    MMIXTest(
        "semihosting-halt-exit-status",
        b"".join(
            [
                wyde(SETL, R255, 42),
                insn(TRAP, 0, MMIX_SEMIHOSTING_HALT, 0),
            ]
        ),
        pc=0x04,
        regs={R255: 42},
        exit_status=42,
    ),
    MMIXTest(
        "semihosting-argv-pool-layout",
        b"".join(
            [
                *set_octa(R1, MMIX_POOL_SEGMENT_BASE),
                insn(LDOU, R2, R1, R0),
                insn(LDOUI, R3, R1, 8),
                insn(LDOUI, R4, R1, 16),
                insn(LDOUI, R5, R1, 24),
                insn(LDOUI, R6, R1, 32),
                insn(LDBUI, R10, R3, 0),
                insn(LDBUI, R11, R3, 4),
                insn(LDBUI, R12, R4, 0),
                insn(LDBUI, R13, R4, 3),
                insn(LDBUI, R14, R5, 0),
                insn(LDBUI, R15, R5, 3),
                wyde(SETL, R255, 0),
                halt(),
            ]
        ),
        pc=0x40,
        regs={
            R2: MMIX_POOL_SEGMENT_BASE + 0x40,
            R3: MMIX_POOL_SEGMENT_BASE + 0x28,
            R4: MMIX_POOL_SEGMENT_BASE + 0x30,
            R5: MMIX_POOL_SEGMENT_BASE + 0x38,
            R6: 0,
            R10: ord("p"),
            R11: 0,
            R12: ord("o"),
            R13: 0,
            R14: ord("t"),
            R15: 0,
        },
        qemu_args=("-semihosting-config",
                   "enable=on,arg=prog,arg=one,arg=two"),
    ),
]
