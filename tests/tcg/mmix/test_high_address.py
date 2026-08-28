#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.high_address import HIGH_ADDRESS_TESTS
from lib.execution import run_one_with_loader


@pytest.mark.parametrize("test", HIGH_ADDRESS_TESTS, ids=case_id)
def test_high_address(qemu, workdir, test):
    run_one_with_loader(qemu, workdir, test, qemu_args=("-semihosting",))
