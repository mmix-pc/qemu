#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.elf_linux import (
    LINUX_ENTRY_STATE_TESTS,
    LINUX_PREFLIGHT_REJECTION_TESTS,
    LINUX_SMP_ENTRY_TESTS,
)
from lib.execution import (
    run_linux_entry_state_test,
    run_linux_smp_entry_test,
    run_process_failure,
)


@pytest.mark.parametrize("test", LINUX_ENTRY_STATE_TESTS, ids=case_id)
def test_elf_linux_entry_state(qemu, workdir, test):
    run_linux_entry_state_test(qemu, workdir, test)


@pytest.mark.parametrize("test", LINUX_SMP_ENTRY_TESTS, ids=case_id)
def test_elf_linux_smp_entry(qemu, workdir, test):
    run_linux_smp_entry_test(qemu, workdir, test)


@pytest.mark.parametrize(
    "test", LINUX_PREFLIGHT_REJECTION_TESTS, ids=case_id
)
def test_elf_linux_preflight_rejected(qemu, workdir, test):
    run_process_failure(qemu, workdir, test)
