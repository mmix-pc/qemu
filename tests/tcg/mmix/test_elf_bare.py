#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.elf_bare import BARE_ELF_TESTS
from lib.execution import run_elf_test


@pytest.mark.boot_integration
@pytest.mark.parametrize("test", BARE_ELF_TESTS, ids=case_id)
def test_elf_bare(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)
