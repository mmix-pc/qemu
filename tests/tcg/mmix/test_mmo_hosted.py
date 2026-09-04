#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses

import pytest

from cases.common import case_id
from cases.mmo_failures import MMO_FORMAT_FAILURE_TESTS
from cases.mmo_hosted import (
    MMO_HOSTED_FAILURE_TESTS,
    MMO_HOSTED_SEMIHOSTING_CONSOLE_TESTS,
    MMO_HOSTED_SEMIHOSTING_STDIN_TESTS,
    MMO_HOSTED_TESTS,
    mmo_hosted_semihosting_file_test,
)
from lib.execution import (
    run_loader_failure,
    run_mmo_test,
    run_semihosting_console_test,
    run_semihosting_stdin_console_test,
)


@pytest.mark.boot_integration
@pytest.mark.parametrize("test", MMO_HOSTED_TESTS, ids=case_id)
def test_mmo_hosted(qemu, workdir, test):
    run_mmo_test(qemu, workdir, test)


@pytest.mark.parametrize("ram_size", ("128M", "512M", "8G"))
def test_mmo_hosted_ram_budget(qemu, workdir, ram_size):
    test = dataclasses.replace(
        MMO_HOSTED_TESTS[0],
        name=f"mmo-hosted-ram-{ram_size.lower()}",
        qemu_args=("-m", ram_size),
    )
    run_mmo_test(qemu, workdir, test)


@pytest.mark.boot_integration
@pytest.mark.parametrize("test", MMO_HOSTED_FAILURE_TESTS, ids=case_id)
def test_mmo_hosted_failure(qemu, workdir, test):
    run_loader_failure(qemu, workdir, test)


@pytest.mark.parametrize("test", MMO_FORMAT_FAILURE_TESTS, ids=case_id)
def test_mmo_format_failure(qemu, workdir, test):
    run_loader_failure(qemu, workdir, test)


@pytest.mark.parametrize(
    "test", MMO_HOSTED_SEMIHOSTING_CONSOLE_TESTS, ids=case_id
)
def test_mmo_hosted_semihosting_console(qemu, workdir, test):
    run_semihosting_console_test(qemu, workdir, test)


@pytest.mark.parametrize(
    "test", MMO_HOSTED_SEMIHOSTING_STDIN_TESTS, ids=case_id
)
def test_mmo_hosted_semihosting_stdin(qemu, workdir, test):
    run_semihosting_stdin_console_test(qemu, workdir, test)


def test_mmo_hosted_semihosting_file(qemu, workdir):
    host_file = workdir / "mmo-hosted-semihosting-file.txt"
    test = mmo_hosted_semihosting_file_test(host_file)

    run_mmo_test(qemu, workdir, test)
    assert host_file.read_bytes() == b"hosted-file"
