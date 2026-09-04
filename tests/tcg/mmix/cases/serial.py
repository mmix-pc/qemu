#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

SERIAL_TX_PROGRAM = serial_tx_program()

SERIAL_TESTS = [
    MMIXSerialTest(
        "serial-tx-output",
        raw_direct_image(SERIAL_TX_PROGRAM[0]),
        pc=MMIX_RAW_ENTRY + SERIAL_TX_PROGRAM[1],
        output=b"MMIX\n",
    ),
]

LLVM_SMOKE_CONTRACT = hosted_llvm_smoke_program()

SEMIHOSTING_CONSOLE_TESTS = [
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
