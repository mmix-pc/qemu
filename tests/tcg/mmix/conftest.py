#!/usr/bin/env python3
#
# pytest configuration for MMIX softmmu tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib

import pytest


def pytest_addoption(parser):
    group = parser.getgroup("mmix")
    group.addoption("--qemu", action="store", help="qemu-system-mmix binary")
    group.addoption("--workdir", action="store", help="MMIX test work directory")


@pytest.fixture(scope="session")
def qemu(pytestconfig):
    value = pytestconfig.getoption("--qemu")
    if value is None:
        pytest.fail("--qemu is required")
    return pathlib.Path(value)


@pytest.fixture(scope="session")
def workdir(pytestconfig):
    value = pytestconfig.getoption("--workdir")
    if value is None:
        pytest.fail("--workdir is required")
    path = pathlib.Path(value)
    path.mkdir(parents=True, exist_ok=True)
    return path
