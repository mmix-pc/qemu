#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

SERIAL_TESTS = [
    MMIXSerialTest(
        "serial-tx-output",
        serial_tx_program(),
        pc=0x38,
        output=b"MMIX\n",
    ),
    MMIXSerialTest(
        "mmo-serial-tx-output",
        mmo_image(
            [
                mmo_file(1, "mmo-serial.mms"),
                mmo_line(1),
                serial_tx_program(),
                mmo_post(R255, {R255: 0}),
                mmo_stab_end(),
            ]
        ),
        pc=0x38,
        output=b"MMIX\n",
    ),
]

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
]
