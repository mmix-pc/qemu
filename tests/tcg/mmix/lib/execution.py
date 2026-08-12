#!/usr/bin/env python3
#
# MMIX softmmu test execution helpers
#
# SPDX-License-Identifier: GPL-2.0-or-later

import subprocess

from lib.asserts import (
    assert_exit_pc,
    assert_exit_status,
    assert_log_patterns,
    assert_output_patterns,
    assert_process_failed,
    assert_regs,
    assert_serial_output,
)
from lib.mmo import MMIX_MMO_ESCAPE, MMIX_MMO_LOP_PRE
from lib.qemu import (
    QEMU_SEMIHOSTING_ARGS,
    QEMU_SEMIHOSTING_STDIN_ARGS,
    read_log,
    run_kernel,
)


def _test_stdin_data(test, stdin_data):
    return test.stdin_data if stdin_data is None else stdin_data


def run_one(qemu, workdir, test, *, qemu_args=(), stdin_data=None):
    image = workdir / f"{test.name}.bin"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.program)
    if log.exists():
        log.unlink()

    completed = run_kernel(qemu, image, trace="int", log=log,
                           qemu_args=qemu_args, check=False, timeout=10,
                           stdin_data=_test_stdin_data(test, stdin_data))

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)
    assert_regs(test.name, result, test.regs)


def run_semihosting_one(qemu, workdir, test):
    qemu_args = test.qemu_args if test.qemu_args else QEMU_SEMIHOSTING_ARGS

    run_one(qemu, workdir, test, qemu_args=qemu_args)


def run_semihosting_stdin_one(qemu, workdir, test):
    qemu_args = test.qemu_args if test.qemu_args else QEMU_SEMIHOSTING_STDIN_ARGS

    run_one(qemu, workdir, test, qemu_args=qemu_args)


def run_expected_failure(qemu, workdir, test, *, qemu_args=()):
    image = workdir / f"{test.name}.bin"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.program)
    if log.exists():
        log.unlink()

    try:
        run_kernel(qemu, image, trace="unimp,int", log=log,
                   qemu_args=qemu_args,
                   check=False, timeout=2)
    except subprocess.TimeoutExpired:
        pass

    if not log.exists():
        raise AssertionError(f"{test.name}: missing log")

    log_text = log.read_text(encoding="utf-8")
    assert_log_patterns(test.name, log_text, test.patterns, test.absent)


def run_semihosting_expected_failure(qemu, workdir, test):
    run_expected_failure(qemu, workdir, test,
                         qemu_args=QEMU_SEMIHOSTING_ARGS)


def run_process_failure(qemu, workdir, test):
    image = workdir / f"{test.name}.bin"

    image.write_bytes(test.program)

    result = run_kernel(
        qemu,
        image,
        qemu_args=test.qemu_args,
        check=False,
        timeout=10,
        capture_output=True,
    )
    assert_process_failed(test.name, result)

    output = result.stdout + result.stderr
    text = output.decode("utf-8", errors="replace")
    assert_output_patterns(test.name, text, test.patterns)


def run_serial_test(qemu, workdir, test, *, qemu_args=(), stdin_data=None):
    is_mmo = test.program.startswith(bytes((MMIX_MMO_ESCAPE,
                                            MMIX_MMO_LOP_PRE)))
    suffix = ".mmo" if is_mmo else ".bin"
    image = workdir / f"{test.name}{suffix}"
    log = workdir / f"{test.name}.log"
    serial = workdir / f"{test.name}.serial"

    image.write_bytes(test.program)
    for path in (log, serial):
        if path.exists():
            path.unlink()

    completed = run_kernel(
        qemu,
        image,
        serial=f"file:{serial}",
        trace="int",
        log=log,
        qemu_args=qemu_args,
        check=False,
        timeout=10,
        stdin_data=_test_stdin_data(test, stdin_data),
    )

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)

    actual = serial.read_bytes()
    assert_serial_output(test.name, actual, test.output)


def run_semihosting_serial_test(qemu, workdir, test):
    qemu_args = test.qemu_args if test.qemu_args else QEMU_SEMIHOSTING_ARGS

    run_serial_test(qemu, workdir, test, qemu_args=qemu_args)


def run_semihosting_stdin_serial_test(qemu, workdir, test):
    qemu_args = test.qemu_args if test.qemu_args else QEMU_SEMIHOSTING_STDIN_ARGS

    run_serial_test(qemu, workdir, test, qemu_args=qemu_args)


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

    completed = run_kernel(qemu, image, trace="int", log=log, check=False,
                           timeout=10)

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)
    assert_regs(test.name, result, test.regs)


def run_elf_test(qemu, workdir, test):
    image = workdir / f"{test.name}.elf"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.image)
    if log.exists():
        log.unlink()

    completed = run_kernel(qemu, image, trace="int", log=log, check=False,
                           timeout=10)

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)
    assert_regs(test.name, result, test.regs)
