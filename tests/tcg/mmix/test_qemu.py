#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from lib.qemu import (
    QEMU_SEMIHOSTING_STDIN_ARGS,
    QEMU_SEMIHOSTING_STDIN_CHARDEV,
)


def test_semihosting_stdin_args_use_stdio_chardev():
    assert QEMU_SEMIHOSTING_STDIN_ARGS == (
        "-chardev",
        f"stdio,id={QEMU_SEMIHOSTING_STDIN_CHARDEV},signal=off",
        "-semihosting-config",
        f"enable=on,chardev={QEMU_SEMIHOSTING_STDIN_CHARDEV}",
    )
