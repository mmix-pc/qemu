#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.l3_smp import L3_SMP_TESTS
from cases.smp_shared_interrupts import SMP_SHARED_INTERRUPT_TESTS
from lib.execution import (
    run_l3_mttcg_cpu_isolation_test,
    run_l3_mttcg_shared_interrupt_test,
)


@pytest.mark.parametrize("test", SMP_SHARED_INTERRUPT_TESTS, ids=case_id)
def test_l3_shared_interrupts(qemu, workdir, test):
    run_l3_mttcg_shared_interrupt_test(qemu, workdir, test)


@pytest.mark.parametrize("test", L3_SMP_TESTS, ids=case_id)
def test_l3_cpu_isolation(qemu, workdir, test):
    run_l3_mttcg_cpu_isolation_test(qemu, workdir, test)
