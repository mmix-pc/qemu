#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.raw_loader import RAW_DIRECT_ISA_TESTS, RAW_DIRECT_TESTS
from lib.execution import run_one, run_serial_test


@pytest.mark.boot_integration
@pytest.mark.parametrize("test", RAW_DIRECT_ISA_TESTS, ids=case_id)
def test_raw_direct_state(qemu, workdir, test):
    run_one(qemu, workdir, test)


@pytest.mark.boot_integration
@pytest.mark.parametrize("test", RAW_DIRECT_TESTS, ids=case_id)
def test_raw_direct_boot(qemu, workdir, test):
    run_serial_test(qemu, workdir, test)
