#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *


def argv_layout_program(argv_indices, byte_checks=()):
    program = [
        insn(ADDI, R32, R0, 0),
        insn(ADDI, R33, R1, 0),
        insn(GET, R34, 0, SR_L),
        *set_octa(R1, MMIX_POOL_SEGMENT_BASE),
        insn(LDOU, R2, R1, R0),
    ]

    for reg, index in argv_indices:
        program.append(insn(LDOUI, reg, R1, 8 * (index + 1)))
    for reg, base_reg, offset in byte_checks:
        program.append(insn(LDBUI, reg, base_reg, offset))
    program.extend([wyde(SETL, R255, 0), halt()])
    return b"".join(program)


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
        "semihosting-argv-one-argument",
        argv_layout_program(
            [(R3, 0), (R4, 1)],
            [(R10, R3, 0), (R11, R3, 4)],
        ),
        pc=0x34,
        regs={
            R32: 1,
            R33: MMIX_POOL_SEGMENT_BASE + 8,
            R34: 2,
            R2: MMIX_POOL_SEGMENT_BASE + 0x20,
            R3: MMIX_POOL_SEGMENT_BASE + 0x18,
            R4: 0,
            R10: ord("p"),
            R11: 0,
        },
        qemu_args=("-semihosting-config", "enable=on,arg=prog"),
    ),
    MMIXTest(
        "semihosting-argv-multiple-arguments",
        argv_layout_program(
            [(R3, 0), (R4, 1), (R5, 2), (R6, 3)],
            [
                (R10, R3, 0), (R11, R3, 4),
                (R12, R4, 0), (R13, R4, 3),
                (R14, R5, 0), (R15, R5, 3),
            ],
        ),
        pc=0x4c,
        regs={
            R32: 3,
            R33: MMIX_POOL_SEGMENT_BASE + 8,
            R34: 2,
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
    MMIXTest(
        "semihosting-argv-long-argument",
        argv_layout_program(
            [(R3, 0), (R4, 1), (R5, 2)],
            [(R10, R4, 0), (R11, R4, 16), (R12, R4, 17)],
        ),
        pc=0x3c,
        regs={
            R32: 2,
            R33: MMIX_POOL_SEGMENT_BASE + 8,
            R34: 2,
            R2: MMIX_POOL_SEGMENT_BASE + 0x40,
            R3: MMIX_POOL_SEGMENT_BASE + 0x20,
            R4: MMIX_POOL_SEGMENT_BASE + 0x28,
            R5: 0,
            R10: ord("a"),
            R11: ord("q"),
            R12: 0,
        },
        qemu_args=("-semihosting-config",
                   "enable=on,arg=prog,arg=abcdefghijklmnopq"),
    ),
]
