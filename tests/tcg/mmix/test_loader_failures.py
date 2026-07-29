#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.loader_failures import LOADER_FAILURE_TESTS
from lib.execution import run_loader_failure


@pytest.mark.parametrize("test", LOADER_FAILURE_TESTS, ids=case_id)
def test_loader_failure(qemu, workdir, test):
    run_loader_failure(qemu, workdir, test)
