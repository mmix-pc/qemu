#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.elf_linux import (
    LINUX_PREFLIGHT_REJECTION_TESTS,
    LINUX_PREFLIGHT_TESTS,
)
from lib.execution import run_process_failure


@pytest.mark.parametrize("test", LINUX_PREFLIGHT_TESTS, ids=case_id)
def test_elf_linux_preflight(qemu, workdir, test):
    run_process_failure(qemu, workdir, test)


@pytest.mark.parametrize(
    "test", LINUX_PREFLIGHT_REJECTION_TESTS, ids=case_id
)
def test_elf_linux_preflight_rejected(qemu, workdir, test):
    run_process_failure(qemu, workdir, test)
