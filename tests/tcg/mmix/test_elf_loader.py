#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.elf_loader import (
    ELF_LOADER_TESTS,
    ELF_STARTUP_PROCESS_FAILURE_TESTS,
)
from lib.execution import run_elf_test, run_process_failure


@pytest.mark.parametrize("test", ELF_LOADER_TESTS, ids=case_id)
def test_elf_loader(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)


@pytest.mark.parametrize("test", ELF_STARTUP_PROCESS_FAILURE_TESTS,
                         ids=case_id)
def test_elf_startup_process_failure(qemu, workdir, test):
    run_process_failure(qemu, workdir, test)
