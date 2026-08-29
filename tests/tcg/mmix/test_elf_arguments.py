#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.elf_arguments import (
    HOSTED_ELF_REJECTION_TESTS,
    HOSTED_ELF_TESTS,
)
from lib.execution import run_elf_test, run_process_failure


@pytest.mark.parametrize("test", HOSTED_ELF_TESTS, ids=case_id)
def test_elf_arguments(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)


@pytest.mark.parametrize("test", HOSTED_ELF_REJECTION_TESTS, ids=case_id)
def test_elf_arguments_rejected(qemu, workdir, test):
    run_process_failure(qemu, workdir, test)
