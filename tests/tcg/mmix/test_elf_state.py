#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from cases.elf_state import ELF_STATE_TEST
from lib.execution import run_elf_state_test


def test_elf_reset_and_snapshot_state(qemu, workdir):
    run_elf_state_test(qemu, workdir, ELF_STATE_TEST)
