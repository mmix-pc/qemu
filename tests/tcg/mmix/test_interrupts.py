#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.interrupts import INTERRUPT_TESTS
from lib.execution import run_one_with_loader


@pytest.mark.parametrize("test", INTERRUPT_TESTS, ids=case_id)
def test_interrupts(qemu, workdir, test):
    run_one_with_loader(qemu, workdir, test)
