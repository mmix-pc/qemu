#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.smp import SMP_TESTS
from lib.execution import run_elf_test


@pytest.mark.parametrize("test", SMP_TESTS, ids=case_id)
def test_smp(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)
