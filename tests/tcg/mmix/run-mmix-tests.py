#!/usr/bin/env python3
#
# MMIX raw-image smoke tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib
import sys

import pytest


def main(argv):
    test_file = pathlib.Path(__file__).resolve().with_name("test_mmix_softmmu.py")
    return pytest.main(["-q", str(test_file), *argv])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
