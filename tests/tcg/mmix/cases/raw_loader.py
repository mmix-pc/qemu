#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import (
    ADDI,
    GET,
    MMIX_RAW_ENTRY,
    MMIXSerialTest,
    MMIXTest,
    R0,
    R1,
    R32,
    R33,
    R34,
    R35,
    SR_G,
    SR_L,
    halt,
    insn,
    raw_direct_image,
    serial_tx_program,
)


SERIAL_PROGRAM, SERIAL_EXIT_PC = serial_tx_program(b"MMIX raw direct boot\n")

STARTUP_PROGRAM = b"".join(
    (
        insn(ADDI, R32, R0, 0),
        insn(ADDI, R33, R1, 0),
        insn(GET, R34, 0, SR_L),
        insn(GET, R35, 0, SR_G),
        halt(),
    )
)

RAW_DIRECT_ISA_TESTS = [
    MMIXTest(
        "raw-direct-startup-registers",
        raw_direct_image(STARTUP_PROGRAM),
        pc=MMIX_RAW_ENTRY + len(STARTUP_PROGRAM) - 4,
        regs={R32: 0, R33: 0, R34: 0, R35: 32},
    ),
]

RAW_DIRECT_TESTS = [
    MMIXSerialTest(
        "raw-direct-serial-output",
        raw_direct_image(SERIAL_PROGRAM),
        pc=MMIX_RAW_ENTRY + SERIAL_EXIT_PC,
        output=b"MMIX raw direct boot\n",
    ),
    MMIXSerialTest(
        "raw-direct-semihosting-enabled",
        raw_direct_image(SERIAL_PROGRAM),
        pc=MMIX_RAW_ENTRY + SERIAL_EXIT_PC,
        output=b"MMIX raw direct boot\n",
        qemu_args=("-semihosting",),
    ),
]
