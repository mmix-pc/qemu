#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.elf_bare import BARE_ELF_REJECTION_TESTS, BARE_ELF_TESTS
from lib.execution import run_elf_test, run_process_failure


@pytest.mark.boot_integration
@pytest.mark.parametrize("test", BARE_ELF_TESTS, ids=case_id)
def test_elf_bare(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)


@pytest.mark.boot_integration
@pytest.mark.parametrize("test", BARE_ELF_REJECTION_TESTS, ids=case_id)
def test_elf_bare_rejected(qemu, workdir, test):
    run_process_failure(qemu, workdir, test)
