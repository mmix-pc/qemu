#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from lib.execution import run_no_image_mttcg_test


def test_no_image_mttcg_startup(qemu):
    run_no_image_mttcg_test(qemu)
