#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import case_id
from cases.mmo_hosted import MMO_HOSTED_FAILURE_TESTS, MMO_HOSTED_TESTS
from lib.execution import run_loader_failure, run_mmo_test


@pytest.mark.parametrize("test", MMO_HOSTED_TESTS, ids=case_id)
def test_mmo_hosted(qemu, workdir, test):
    run_mmo_test(qemu, workdir, test)


@pytest.mark.parametrize("test", MMO_HOSTED_FAILURE_TESTS, ids=case_id)
def test_mmo_hosted_failure(qemu, workdir, test):
    run_loader_failure(qemu, workdir, test)
