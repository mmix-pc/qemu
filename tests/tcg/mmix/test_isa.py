#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.isa import ISA_TESTS
from lib.execution import run_one


@pytest.mark.parametrize("test", ISA_TESTS, ids=case_id)
def test_isa(qemu, workdir, test):
    run_one(qemu, workdir, test)
