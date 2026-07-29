#!/usr/bin/env python3
#
# pytest collection for MMIX softmmu tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.expected_failures import EXPECTED_FAILURE_TESTS
from cases.isa import ISA_TESTS
from cases.loader_failures import LOADER_FAILURE_TESTS
from cases.mmixal import optional_mmixal_tests
from cases.mmo_loader import MMO_LOADER_TESTS
from cases.serial import SERIAL_TESTS
from lib.execution import (
    run_expected_failure,
    run_loader_failure,
    run_mmo_test,
    run_one,
    run_serial_test,
)


def case_id(test):
    return test.name


@pytest.mark.parametrize("test", ISA_TESTS, ids=case_id)
def test_isa(qemu, workdir, test):
    run_one(qemu, workdir, test)


@pytest.mark.parametrize("test", EXPECTED_FAILURE_TESTS, ids=case_id)
def test_expected_failure(qemu, workdir, test):
    run_expected_failure(qemu, workdir, test)


@pytest.mark.parametrize("test", SERIAL_TESTS, ids=case_id)
def test_serial(qemu, workdir, test):
    run_serial_test(qemu, workdir, test)


@pytest.mark.parametrize("test", LOADER_FAILURE_TESTS, ids=case_id)
def test_loader_failure(qemu, workdir, test):
    run_loader_failure(qemu, workdir, test)


@pytest.mark.parametrize("test", MMO_LOADER_TESTS, ids=case_id)
def test_mmo_loader(qemu, workdir, test):
    run_mmo_test(qemu, workdir, test)


def test_optional_mmixal_serial(qemu, workdir):
    serial_tests, _ = optional_mmixal_tests(workdir)
    if not serial_tests:
        pytest.skip("mmixal not found")
    for test in serial_tests:
        run_serial_test(qemu, workdir, test)


def test_optional_mmixal_mmo(qemu, workdir):
    _, mmo_tests = optional_mmixal_tests(workdir)
    if not mmo_tests:
        pytest.skip("mmixal not found")
    for test in mmo_tests:
        run_mmo_test(qemu, workdir, test)
