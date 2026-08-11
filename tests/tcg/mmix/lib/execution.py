#!/usr/bin/env python3
#
# MMIX softmmu test execution helpers
#
# SPDX-License-Identifier: GPL-2.0-or-later

import subprocess

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
        run_kernel(qemu, image, trace="unimp,int", log=log,
                   qemu_args=getattr(test, "qemu_args", ()),
                   check=False, timeout=2)
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
        qemu_args=getattr(test, "qemu_args", ()),
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
