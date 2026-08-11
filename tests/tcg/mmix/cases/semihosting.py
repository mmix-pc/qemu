#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

SEMIHOSTING_TESTS = [
    MMIXTest(
        "hosted-halt",
        insn(TRAP, 0, MMIX_SEMIHOSTING_HALT, 0),
        pc=0x00,
        regs={},
    ),
    MMIXTest(
        "hosted-halt-exit-status",
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
]
