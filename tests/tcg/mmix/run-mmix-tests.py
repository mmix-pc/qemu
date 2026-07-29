#!/usr/bin/env python3
#
# MMIX raw-image smoke tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import pathlib
import subprocess
import sys

from cases.expected_failures import EXPECTED_FAILURE_TESTS
from cases.isa import ISA_TESTS
from cases.loader_failures import LOADER_FAILURE_TESTS
from cases.mmixal import optional_mmixal_tests
from cases.mmo_loader import MMO_LOADER_TESTS
from cases.serial import SERIAL_TESTS
from lib.asserts import (
    assert_exit_pc,
    assert_log_patterns,
    assert_output_patterns,
    assert_process_failed,
    assert_regs,
    assert_serial_output,
)
from lib.mmo import MMIX_MMO_ESCAPE, MMIX_MMO_LOP_PRE
from lib.qemu import read_log, run_kernel


def run_one(qemu, workdir, test):
    image = workdir / f"{test.name}.bin"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.program)
    if log.exists():
        log.unlink()

    run_kernel(qemu, image, trace="int", log=log, check=True, timeout=10)

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_regs(test.name, result, test.regs)


def run_expected_failure(qemu, workdir, test):
    image = workdir / f"{test.name}.bin"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.program)
    if log.exists():
        log.unlink()

    try:
        run_kernel(qemu, image, trace="unimp,int", log=log, check=False, timeout=2)
    except subprocess.TimeoutExpired:
        pass

    if not log.exists():
        raise AssertionError(f"{test.name}: missing log")

    log_text = log.read_text(encoding="utf-8")
    assert_log_patterns(test.name, log_text, test.patterns, test.absent)


def run_serial_test(qemu, workdir, test):
    suffix = ".mmo" if test.program.startswith(bytes((MMIX_MMO_ESCAPE,
                                                      MMIX_MMO_LOP_PRE))) else ".bin"
    image = workdir / f"{test.name}{suffix}"
    log = workdir / f"{test.name}.log"
    serial = workdir / f"{test.name}.serial"

    image.write_bytes(test.program)
    for path in (log, serial):
        if path.exists():
            path.unlink()

    run_kernel(
        qemu,
        image,
        serial=f"file:{serial}",
        trace="int",
        log=log,
        check=True,
        timeout=10,
    )

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)

    actual = serial.read_bytes()
    assert_serial_output(test.name, actual, test.output)


def run_loader_failure(qemu, workdir, test):
    image = workdir / f"{test.name}.mmo"

    image.write_bytes(test.image)

    result = run_kernel(
        qemu,
        image,
        check=False,
        timeout=10,
        capture_output=True,
    )
    assert_process_failed(test.name, result)

    output = result.stdout + result.stderr
    text = output.decode("utf-8", errors="replace")
    assert_output_patterns(test.name, text, test.patterns)


def run_mmo_test(qemu, workdir, test):
    image = workdir / f"{test.name}.mmo"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.image)
    if log.exists():
        log.unlink()

    run_kernel(qemu, image, trace="int", log=log, check=True, timeout=10)

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_regs(test.name, result, test.regs)


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True, type=pathlib.Path)
    parser.add_argument("--workdir", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)

    args.workdir.mkdir(parents=True, exist_ok=True)
    mmixal_serial_tests, mmixal_mmo_tests = optional_mmixal_tests(args.workdir)

    for test in ISA_TESTS:
        run_one(args.qemu, args.workdir, test)
        print(f"PASS {test.name}")
    for test in EXPECTED_FAILURE_TESTS:
        run_expected_failure(args.qemu, args.workdir, test)
        print(f"PASS {test.name}")
    for test in [*SERIAL_TESTS, *mmixal_serial_tests]:
        run_serial_test(args.qemu, args.workdir, test)
        print(f"PASS {test.name}")
    for test in LOADER_FAILURE_TESTS:
        run_loader_failure(args.qemu, args.workdir, test)
        print(f"PASS {test.name}")
    for test in [*MMO_LOADER_TESTS, *mmixal_mmo_tests]:
        run_mmo_test(args.qemu, args.workdir, test)
        print(f"PASS {test.name}")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (AssertionError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as err:
        print(f"FAIL {err}", file=sys.stderr)
        sys.exit(1)
