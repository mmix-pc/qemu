#!/usr/bin/env python3
#
# Assertion helpers for MMIX softmmu tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

MASK64 = (1 << 64) - 1


def assert_exit_pc(name, result, expected_pc):
    if result.pc != expected_pc:
        raise AssertionError(
            f"{name}: pc expected 0x{expected_pc:x}, got 0x{result.pc:x}"
        )
    if result.npc != expected_pc + 4:
        raise AssertionError(
            f"{name}: npc expected 0x{expected_pc + 4:x}, got 0x{result.npc:x}"
        )


def assert_exit_status(name, result, expected_status):
    if result.returncode != expected_status:
        raise AssertionError(
            f"{name}: exit status expected {expected_status}, "
            f"got {result.returncode}"
        )


def assert_regs(name, result, expected_regs):
    for reg, expected in expected_regs.items():
        actual = result.regs.get(reg)
        if actual is None:
            raise AssertionError(f"{name}: missing r{reg} in log")
        expected &= MASK64
        if actual != expected:
            raise AssertionError(
                f"{name}: r{reg} expected 0x{expected:016x}, "
                f"got 0x{actual:016x}"
            )


def assert_log_patterns(name, log_text, present, absent=()):
    for pattern in present:
        if pattern not in log_text:
            raise AssertionError(f"{name}: missing expected log pattern {pattern!r}")
    for pattern in absent:
        if pattern in log_text:
            raise AssertionError(f"{name}: unexpected log pattern {pattern!r}")


def assert_output_patterns(name, output_text, patterns):
    for pattern in patterns:
        if pattern not in output_text:
            raise AssertionError(
                f"{name}: missing expected output pattern {pattern!r}"
            )


def assert_serial_output(name, actual, expected):
    if actual != expected:
        raise AssertionError(
            f"{name}: serial output expected {expected!r}, got {actual!r}"
        )


def assert_console_output(name, actual, expected):
    if actual != expected:
        raise AssertionError(
            f"{name}: console output expected {expected!r}, got {actual!r}"
        )


def assert_process_failed(name, result):
    if result.returncode == 0:
        raise AssertionError(f"{name}: expected loader failure")
