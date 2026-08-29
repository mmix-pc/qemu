#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.elf_registers import (
    ELF_REGISTER_FAILURE_TESTS,
    ELF_REGISTER_TESTS,
)
from lib.execution import run_elf_test, run_process_failure


@pytest.mark.parametrize("test", ELF_REGISTER_TESTS, ids=case_id)
def test_elf_registers(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)


@pytest.mark.parametrize("test", ELF_REGISTER_FAILURE_TESTS, ids=case_id)
def test_elf_registers_rejected(qemu, workdir, test):
    run_process_failure(qemu, workdir, test)
