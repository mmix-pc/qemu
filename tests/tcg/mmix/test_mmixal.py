#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.mmixal import MMIXAL_MMO_TESTS, MMIXAL_SERIAL_TESTS
from lib.execution import run_mmo_test, run_serial_test


@pytest.mark.parametrize("test_case", MMIXAL_SERIAL_TESTS, ids=case_id)
def test_mmixal_serial(qemu, mmixal, workdir, test_case):
    run_serial_test(qemu, workdir, test_case.build(mmixal, workdir))


@pytest.mark.parametrize("test_case", MMIXAL_MMO_TESTS, ids=case_id)
def test_mmixal_mmo(qemu, mmixal, workdir, test_case):
    run_mmo_test(qemu, workdir, test_case.build(mmixal, workdir))
