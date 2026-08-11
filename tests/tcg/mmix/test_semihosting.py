#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.expected_failures import (
    SEMIHOSTING_DISABLED_FAILURE_TESTS,
    SEMIHOSTING_EXPECTED_FAILURE_TESTS,
    SEMIHOSTING_PROCESS_FAILURE_TESTS,
)
from cases.semihosting import SEMIHOSTING_TESTS
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
