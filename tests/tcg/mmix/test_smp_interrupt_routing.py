#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.smp_interrupt_isolation import SMP_INTERRUPT_ISOLATION_TESTS
from cases.smp_shared_interrupts import SMP_SHARED_INTERRUPT_TESTS
from lib.execution import (
    run_mttcg_cpu_interrupt_isolation_test,
    run_mttcg_shared_interrupt_routing_test,
)


@pytest.mark.parametrize("test", SMP_SHARED_INTERRUPT_TESTS, ids=case_id)
def test_smp_shared_interrupt_routing(qemu, workdir, test):
    run_mttcg_shared_interrupt_routing_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SMP_INTERRUPT_ISOLATION_TESTS, ids=case_id)
def test_smp_cpu_interrupt_isolation(qemu, workdir, test):
    run_mttcg_cpu_interrupt_isolation_test(qemu, workdir, test)
