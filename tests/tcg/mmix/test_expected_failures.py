#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.expected_failures import EXPECTED_FAILURE_TESTS
from lib.execution import run_expected_failure


@pytest.mark.parametrize("test", EXPECTED_FAILURE_TESTS, ids=case_id)
def test_expected_failure(qemu, workdir, test):
    run_expected_failure(qemu, workdir, test)
