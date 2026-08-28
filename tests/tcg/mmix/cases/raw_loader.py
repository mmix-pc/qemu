#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import (
    ADDI,
    GET,
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
    serial_tx_program,
)


RAW_ENTRY = 0x100
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
        bytes(RAW_ENTRY) + STARTUP_PROGRAM,
        pc=RAW_ENTRY + len(STARTUP_PROGRAM) - 4,
        regs={R32: 0, R33: 0, R34: 0, R35: 32},
    ),
]

RAW_DIRECT_TESTS = [
    MMIXSerialTest(
        "raw-direct-serial-output",
        bytes(RAW_ENTRY) + SERIAL_PROGRAM,
        pc=RAW_ENTRY + SERIAL_EXIT_PC,
        output=b"MMIX raw direct boot\n",
    ),
    MMIXSerialTest(
        "raw-direct-semihosting-enabled",
        bytes(RAW_ENTRY) + SERIAL_PROGRAM,
        pc=RAW_ENTRY + SERIAL_EXIT_PC,
        output=b"MMIX raw direct boot\n",
        qemu_args=("-semihosting",),
    ),
]
