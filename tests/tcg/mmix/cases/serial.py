#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

SERIAL_TX_PROGRAM = serial_tx_program()

SERIAL_TESTS = [
    MMIXSerialTest(
        "serial-tx-output",
        SERIAL_TX_PROGRAM[0],
        pc=SERIAL_TX_PROGRAM[1],
        output=b"MMIX\n",
    ),
    MMIXSerialTest(
        "mmo-serial-tx-output",
        mmo_image(
            [
                mmo_file(1, "mmo-serial.mms"),
                mmo_line(1),
                SERIAL_TX_PROGRAM[0],
                mmo_post(R255, {R255: 0}),
                mmo_stab_end(),
            ]
        ),
        pc=SERIAL_TX_PROGRAM[1],
        output=b"MMIX\n",
    ),
]

LLVM_SMOKE_CONTRACT = hosted_llvm_smoke_program()

SEMIHOSTING_SERIAL_TESTS = [
    MMIXSerialTest(
        "semihosting-fputs-stdout",
        hosted_fputs_program(),
        pc=0x18,
        output=b"Hosted MMIX\n",
    ),
    MMIXSerialTest(
        "semihosting-fputs-stderr",
        hosted_fputs_program(
            handle=MMIX_SEMIHOSTING_STDERR,
            message=b"Hosted MMIX stderr\n",
        ),
        pc=0x18,
        output=b"Hosted MMIX stderr\n",
    ),
    MMIXSerialTest(
        "semihosting-llvm-smoke-contract",
        LLVM_SMOKE_CONTRACT[0],
        pc=LLVM_SMOKE_CONTRACT[1],
        output=b"LLVM smoke\n",
    ),
]
