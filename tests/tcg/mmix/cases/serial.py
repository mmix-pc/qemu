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
        "hosted-fputs-stdout",
        hosted_fputs_program(),
        pc=0x14,
        output=b"Hosted MMIX\n",
        qemu_args=MMIX_SEMIHOSTING_ARGS,
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
