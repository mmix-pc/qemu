#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.mmixal import (
    MMIXAL_MMO_TESTS,
    MMIXAL_SEMIHOSTING_CONSOLE_TESTS,
    MMIXAL_SERIAL_TESTS,
    mmixal_file_read_case,
    mmixal_file_write_case,
)
from lib.execution import (
    run_mmo_test,
    run_semihosting_console_test,
    run_semihosting_stdin_console_test,
    run_serial_test,
)


@pytest.mark.parametrize("test_case", MMIXAL_SERIAL_TESTS, ids=case_id)
def test_mmixal_serial(qemu, mmixal, workdir, test_case):
    run_serial_test(qemu, workdir, test_case.build(mmixal, workdir))


@pytest.mark.parametrize("test_case", MMIXAL_SEMIHOSTING_CONSOLE_TESTS,
                         ids=case_id)
def test_mmixal_semihosting_console(qemu, mmixal, workdir, test_case):
    test = test_case.build(mmixal, workdir)
    if test.stdin_data is None:
        run_semihosting_console_test(qemu, workdir, test)
    else:
        run_semihosting_stdin_console_test(qemu, workdir, test)


def test_mmixal_semihosting_file_read(qemu, mmixal, workdir):
    input_file = workdir / "mmixal-file-read-input.txt"
    data = b"MMIX file read\n"

    input_file.write_bytes(data)
    run_semihosting_console_test(
        qemu,
        workdir,
        mmixal_file_read_case(input_file, data).build(mmixal, workdir),
    )


def test_mmixal_semihosting_file_write(qemu, mmixal, workdir):
    output_file = workdir / "mmixal-file-write-output.txt"
    data = b"MMIX file write\n"

    run_semihosting_console_test(
        qemu,
        workdir,
        mmixal_file_write_case(output_file, data).build(mmixal, workdir),
    )

    actual = output_file.read_bytes()
    if actual != data:
        raise AssertionError(
            f"mmixal-mmo-file-write: expected {data!r}, got {actual!r}"
        )


@pytest.mark.parametrize("test_case", MMIXAL_MMO_TESTS, ids=case_id)
def test_mmixal_mmo(qemu, mmixal, workdir, test_case):
    run_mmo_test(qemu, workdir, test_case.build(mmixal, workdir))
