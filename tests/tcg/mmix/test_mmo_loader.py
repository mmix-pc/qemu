#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.mmo_loader import MMO_LOADER_TESTS
from lib.execution import run_mmo_test


@pytest.mark.parametrize("test", MMO_LOADER_TESTS, ids=case_id)
def test_mmo_loader(qemu, workdir, test):
    run_mmo_test(qemu, workdir, test)
