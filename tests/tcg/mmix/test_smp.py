#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.smp import SMP_MTTCG_TESTS, SMP_TESTS
from cases.smp_interrupts import SMP_INTERRUPT_TESTS
from cases.smp_ipi import SMP_IPI_TESTS
from cases.smp_memory import SMP_CSWAP_TESTS, SMP_MEMORY_TESTS
from cases.smp_shared_interrupts import SMP_SHARED_INTERRUPT_TESTS
from cases.smp_shootdown import SMP_SHOOTDOWN_TESTS
from cases.smp_state import SMP_RESET_TESTS, SMP_STATE_TESTS
from cases.smp_timers import SMP_TIMER_TESTS
from cases.smp_translation import SMP_TRANSLATION_TESTS
from lib.execution import (
    run_elf_test,
    run_mttcg_elf_test,
    run_mttcg_interrupt_test,
    run_mttcg_ipi_test,
    run_mttcg_reset_test,
    run_mttcg_shared_interrupt_test,
    run_mttcg_shootdown_test,
    run_mttcg_timer_test,
)


@pytest.mark.parametrize("test", SMP_TESTS, ids=case_id)
def test_smp(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_MTTCG_TESTS, ids=case_id)
def test_smp_mttcg(qemu, workdir, test):
    run_mttcg_elf_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_INTERRUPT_TESTS, ids=case_id)
def test_smp_interrupts(qemu, workdir, test):
    run_mttcg_interrupt_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_IPI_TESTS, ids=case_id)
def test_smp_ipi(qemu, workdir, test):
    run_mttcg_ipi_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_SHOOTDOWN_TESTS, ids=case_id)
def test_smp_shootdown(qemu, workdir, test):
    run_mttcg_shootdown_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_TIMER_TESTS, ids=case_id)
def test_smp_timers(qemu, workdir, test):
    run_mttcg_timer_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_SHARED_INTERRUPT_TESTS, ids=case_id)
def test_smp_shared_interrupts(qemu, workdir, test):
    run_mttcg_shared_interrupt_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_MEMORY_TESTS, ids=case_id)
def test_smp_memory(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_CSWAP_TESTS, ids=case_id)
def test_smp_cswap(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_STATE_TESTS, ids=case_id)
def test_smp_state(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_RESET_TESTS, ids=case_id)
def test_smp_reset(qemu, workdir, test):
    run_mttcg_reset_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_TRANSLATION_TESTS, ids=case_id)
def test_smp_translation(qemu, workdir, test):
    run_elf_test(qemu, workdir, test)
