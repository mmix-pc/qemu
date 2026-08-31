#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.isa import HOSTED_ISA_TESTS, ISA_TESTS
from lib.execution import run_one_with_loader, run_semihosting_one


@pytest.mark.parametrize("test", ISA_TESTS, ids=case_id)
def test_isa(qemu, workdir, test):
    run_one_with_loader(qemu, workdir, test)


@pytest.mark.parametrize("test", HOSTED_ISA_TESTS, ids=case_id)
def test_hosted_isa(qemu, workdir, test):
    run_semihosting_one(qemu, workdir, test)
