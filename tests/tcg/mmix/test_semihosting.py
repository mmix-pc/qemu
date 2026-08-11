#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import MMIX_SEMIHOSTING_STDOUT, case_id
from cases.expected_failures import (
    SEMIHOSTING_DISABLED_FAILURE_TESTS,
    SEMIHOSTING_EXPECTED_FAILURE_TESTS,
    SEMIHOSTING_PROCESS_FAILURE_TESTS,
)
from cases.semihosting import (
    SEMIHOSTING_TESTS,
    fclose_failure_test,
    fopen_failure_test,
    fopen_fclose_test,
)
from cases.serial import SEMIHOSTING_SERIAL_TESTS
from lib.execution import (
    run_expected_failure,
    run_process_failure,
    run_semihosting_expected_failure,
    run_semihosting_one,
    run_semihosting_serial_test,
)


@pytest.mark.parametrize("test", SEMIHOSTING_TESTS, ids=case_id)
def test_semihosting(qemu, workdir, test):
    run_semihosting_one(qemu, workdir, test)


def test_semihosting_fopen_fclose(qemu, workdir):
    host_file = workdir / "semihosting-fopen-fclose.txt"
    host_file.write_text("MMIX semihosting\n", encoding="utf-8")

    run_semihosting_one(qemu, workdir, fopen_fclose_test(host_file))


def test_semihosting_fopen_missing_file(qemu, workdir):
    missing_file = workdir / "missing-file.txt"

    run_semihosting_one(
        qemu,
        workdir,
        fopen_failure_test("semihosting-fopen-missing-file", missing_file, 0),
    )


def test_semihosting_fopen_invalid_mode(qemu, workdir):
    host_file = workdir / "semihosting-fopen-invalid-mode.txt"
    host_file.write_text("MMIX semihosting\n", encoding="utf-8")

    run_semihosting_one(
        qemu,
        workdir,
        fopen_failure_test("semihosting-fopen-invalid-mode", host_file, 99),
    )


def test_semihosting_fopen_standard_handle(qemu, workdir):
    host_file = workdir / "semihosting-fopen-standard-handle.txt"
    host_file.write_text("MMIX semihosting\n", encoding="utf-8")

    run_semihosting_one(
        qemu,
        workdir,
        fopen_failure_test(
            "semihosting-fopen-standard-handle",
            host_file,
            0,
            handle=MMIX_SEMIHOSTING_STDOUT,
        ),
    )


def test_semihosting_fclose_standard_handle(qemu, workdir):
    run_semihosting_one(
        qemu,
        workdir,
        fclose_failure_test(
            "semihosting-fclose-standard-handle",
            MMIX_SEMIHOSTING_STDOUT,
        ),
    )


def test_semihosting_fclose_unopened_handle(qemu, workdir):
    run_semihosting_one(
        qemu,
        workdir,
        fclose_failure_test("semihosting-fclose-unopened-handle", 3),
    )


@pytest.mark.parametrize("test", SEMIHOSTING_SERIAL_TESTS, ids=case_id)
def test_semihosting_serial(qemu, workdir, test):
    run_semihosting_serial_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SEMIHOSTING_EXPECTED_FAILURE_TESTS,
                         ids=case_id)
def test_semihosting_expected_failure(qemu, workdir, test):
    run_semihosting_expected_failure(qemu, workdir, test)


@pytest.mark.parametrize("test", SEMIHOSTING_DISABLED_FAILURE_TESTS,
                         ids=case_id)
def test_semihosting_disabled_expected_failure(qemu, workdir, test):
    run_expected_failure(qemu, workdir, test)


@pytest.mark.parametrize("test", SEMIHOSTING_PROCESS_FAILURE_TESTS,
                         ids=case_id)
def test_semihosting_process_failure(qemu, workdir, test):
    run_process_failure(qemu, workdir, test)
