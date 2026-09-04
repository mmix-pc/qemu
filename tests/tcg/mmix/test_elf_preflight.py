#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.elf_preflight import (
    ELF_PREFLIGHT_FAILURE_TESTS,
    ELF_PREFLIGHT_VALID_TESTS,
)
from lib.execution import run_elf_test, run_process_failure


@pytest.mark.parametrize("test", ELF_PREFLIGHT_VALID_TESTS, ids=case_id)
def test_elf_preflight_valid(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)


@pytest.mark.parametrize("test", ELF_PREFLIGHT_FAILURE_TESTS, ids=case_id)
def test_elf_preflight_failure(qemu, workdir, test):
    run_process_failure(qemu, workdir, test)
