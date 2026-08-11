#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import (
    MMIX_SEMIHOSTING_FIRST_FILE_HANDLE,
    MMIX_SEMIHOSTING_STDERR,
    MMIX_SEMIHOSTING_STDOUT,
    MASK64,
    case_id,
)
from cases.expected_failures import (
    SEMIHOSTING_DISABLED_FAILURE_TESTS,
    SEMIHOSTING_EXPECTED_FAILURE_TESTS,
    SEMIHOSTING_PROCESS_FAILURE_TESTS,
)
from cases.semihosting import (
    SEMIHOSTING_TESTS,
    fclose_failure_test,
    fgets_failure_test,
    fgets_file_test,
    fgets_stdin_bad_buffer_test,
    fgets_stdin_test,
    fread_bad_buffer_test,
    fread_failure_test,
    fread_file_test,
    fread_standard_handle_failure_test,
    fread_stdin_bad_buffer_test,
    fread_stdin_test,
    fwrite_console_test,
    fseek_failure_test,
    fseek_ftell_test,
    ftell_failure_test,
    fwrite_failure_test,
    fwrite_file_test,
    fwrite_readonly_file_test,
    fopen_failure_test,
    fopen_fclose_test,
)
from cases.serial import SEMIHOSTING_SERIAL_TESTS
from lib.execution import (
    run_expected_failure,
    run_process_failure,
    run_semihosting_expected_failure,
    run_semihosting_one,
    run_semihosting_serial_test,
    run_semihosting_stdin_one,
)


@pytest.mark.parametrize("test", SEMIHOSTING_TESTS, ids=case_id)
def test_semihosting(qemu, workdir, test):
    run_semihosting_one(qemu, workdir, test)


def test_semihosting_fopen_fclose(qemu, workdir):
    host_file = workdir / "semihosting-fopen-fclose.txt"
    host_file.write_text("MMIX semihosting\n", encoding="utf-8")

    run_semihosting_one(qemu, workdir, fopen_fclose_test(host_file))


def test_semihosting_fopen_missing_file(qemu, workdir):
    missing_file = workdir / "missing-file.txt"

    run_semihosting_one(
        qemu,
        workdir,
        fopen_failure_test("semihosting-fopen-missing-file", missing_file, 0),
    )


def test_semihosting_fopen_invalid_mode(qemu, workdir):
    host_file = workdir / "semihosting-fopen-invalid-mode.txt"
    host_file.write_text("MMIX semihosting\n", encoding="utf-8")

    run_semihosting_one(
        qemu,
        workdir,
        fopen_failure_test("semihosting-fopen-invalid-mode", host_file, 99),
    )


def test_semihosting_fopen_standard_handle(qemu, workdir):
    host_file = workdir / "semihosting-fopen-standard-handle.txt"
    host_file.write_text("MMIX semihosting\n", encoding="utf-8")

    run_semihosting_one(
        qemu,
        workdir,
        fopen_failure_test(
            "semihosting-fopen-standard-handle",
            host_file,
            0,
            handle=MMIX_SEMIHOSTING_STDOUT,
        ),
    )


def test_semihosting_fclose_standard_handle(qemu, workdir):
    run_semihosting_one(
        qemu,
        workdir,
        fclose_failure_test(
            "semihosting-fclose-standard-handle",
            MMIX_SEMIHOSTING_STDOUT,
        ),
    )


def test_semihosting_fclose_unopened_handle(qemu, workdir):
    run_semihosting_one(
        qemu,
        workdir,
        fclose_failure_test("semihosting-fclose-unopened-handle", 3),
    )


def test_semihosting_fread_file(qemu, workdir):
    host_file = workdir / "semihosting-fread-file.txt"
    host_file.write_bytes(b"abcdef")

    run_semihosting_one(
        qemu,
        workdir,
        fread_file_test(host_file, 3, b"abc", 0),
    )


def test_semihosting_fread_short_eof(qemu, workdir):
    host_file = workdir / "semihosting-fread-short-eof.txt"
    host_file.write_bytes(b"xyz")

    run_semihosting_one(
        qemu,
        workdir,
        fread_file_test(host_file, 8, b"xyz", MASK64 - 4),
    )


def test_semihosting_fread_unopened_handle(qemu, workdir):
    run_semihosting_one(
        qemu,
        workdir,
        fread_failure_test(
            "semihosting-fread-unopened-handle",
            MMIX_SEMIHOSTING_FIRST_FILE_HANDLE,
            0x140,
            4,
        ),
    )


def test_semihosting_fread_stdin(qemu, workdir):
    run_semihosting_stdin_one(
        qemu,
        workdir,
        fread_stdin_test("semihosting-fread-stdin", 5, b"input", b"input", 0),
    )


def test_semihosting_fread_stdin_short(qemu, workdir):
    run_semihosting_stdin_one(
        qemu,
        workdir,
        fread_stdin_test(
            "semihosting-fread-stdin-short",
            8,
            b"abc",
            b"abc",
            MASK64 - 4,
        ),
    )


def test_semihosting_fread_stdin_bad_buffer(qemu, workdir):
    run_semihosting_stdin_one(
        qemu,
        workdir,
        fread_stdin_bad_buffer_test(0x6000000000000000, 4, b"input"),
    )


def test_semihosting_fread_stdout_bad_handle(qemu, workdir):
    run_semihosting_one(
        qemu,
        workdir,
        fread_standard_handle_failure_test(
            "semihosting-fread-stdout-bad-handle",
            MMIX_SEMIHOSTING_STDOUT,
            4,
        ),
    )


def test_semihosting_fread_stderr_bad_handle(qemu, workdir):
    run_semihosting_one(
        qemu,
        workdir,
        fread_standard_handle_failure_test(
            "semihosting-fread-stderr-bad-handle",
            MMIX_SEMIHOSTING_STDERR,
            4,
        ),
    )


def test_semihosting_fread_bad_buffer(qemu, workdir):
    host_file = workdir / "semihosting-fread-bad-buffer.txt"
    host_file.write_bytes(b"input")

    run_semihosting_one(
        qemu,
        workdir,
        fread_bad_buffer_test(host_file, 0x6000000000000000, 4),
    )


def test_semihosting_fgets_stdin_newline(qemu, workdir):
    run_semihosting_stdin_one(
        qemu,
        workdir,
        fgets_stdin_test(
            "semihosting-fgets-stdin-newline",
            8,
            b"hello\nextra",
            b"hello\n\0",
            6,
        ),
    )


def test_semihosting_fgets_stdin_size_limit(qemu, workdir):
    run_semihosting_stdin_one(
        qemu,
        workdir,
        fgets_stdin_test(
            "semihosting-fgets-stdin-size-limit",
            4,
            b"abcdef",
            b"abc\0",
            3,
        ),
    )


def test_semihosting_fgets_stdin_bad_buffer(qemu, workdir):
    run_semihosting_stdin_one(
        qemu,
        workdir,
        fgets_stdin_bad_buffer_test(0x6000000000000000, 4, b"input\n"),
    )


def test_semihosting_fgets_file_newline(qemu, workdir):
    host_file = workdir / "semihosting-fgets-file-newline.txt"
    host_file.write_bytes(b"line\nnext")

    run_semihosting_one(
        qemu,
        workdir,
        fgets_file_test(
            "semihosting-fgets-file-newline",
            host_file,
            8,
            b"line\n\0",
            5,
        ),
    )


def test_semihosting_fgets_file_eof(qemu, workdir):
    host_file = workdir / "semihosting-fgets-file-eof.txt"
    host_file.write_bytes(b"abc")

    run_semihosting_one(
        qemu,
        workdir,
        fgets_file_test(
            "semihosting-fgets-file-eof",
            host_file,
            8,
            b"",
            MASK64,
        ),
    )


def test_semihosting_fgets_stdout_bad_handle(qemu, workdir):
    run_semihosting_one(
        qemu,
        workdir,
        fgets_failure_test(
            "semihosting-fgets-stdout-bad-handle",
            MMIX_SEMIHOSTING_STDOUT,
            0x140,
            4,
        ),
    )


def test_semihosting_fwrite_file(qemu, workdir):
    host_file = workdir / "semihosting-fwrite-file.txt"
    data = b"written by MMIX\n"

    run_semihosting_one(qemu, workdir, fwrite_file_test(host_file, data))

    actual = host_file.read_bytes()
    if actual != data:
        raise AssertionError(
            f"semihosting-fwrite-file: expected {data!r}, got {actual!r}"
        )


def test_semihosting_fwrite_readonly_file(qemu, workdir):
    host_file = workdir / "semihosting-fwrite-readonly-file.txt"
    host_file.write_bytes(b"original")

    run_semihosting_one(
        qemu,
        workdir,
        fwrite_readonly_file_test(host_file, b"new!"),
    )

    actual = host_file.read_bytes()
    if actual != b"original":
        raise AssertionError(
            "semihosting-fwrite-readonly-file: read-only test changed file"
        )


def test_semihosting_fwrite_stdout(qemu, workdir):
    run_semihosting_serial_test(
        qemu,
        workdir,
        fwrite_console_test(
            "semihosting-fwrite-stdout",
            MMIX_SEMIHOSTING_STDOUT,
            b"counted stdout\n",
        ),
    )


def test_semihosting_fwrite_stderr(qemu, workdir):
    run_semihosting_serial_test(
        qemu,
        workdir,
        fwrite_console_test(
            "semihosting-fwrite-stderr",
            MMIX_SEMIHOSTING_STDERR,
            b"counted stderr\n",
        ),
    )


def test_semihosting_fwrite_bad_buffer(qemu, workdir):
    run_semihosting_one(
        qemu,
        workdir,
        fwrite_failure_test(
            "semihosting-fwrite-bad-buffer",
            MMIX_SEMIHOSTING_STDOUT,
            0x6000000000000000,
            4,
        ),
    )


def test_semihosting_fseek_ftell(qemu, workdir):
    host_file = workdir / "semihosting-fseek-ftell.txt"
    host_file.write_bytes(b"abcdef")

    run_semihosting_one(qemu, workdir, fseek_ftell_test(host_file))


def test_semihosting_fseek_unopened_handle(qemu, workdir):
    run_semihosting_one(
        qemu,
        workdir,
        fseek_failure_test(
            "semihosting-fseek-unopened-handle",
            MMIX_SEMIHOSTING_FIRST_FILE_HANDLE,
            0,
        ),
    )


def test_semihosting_ftell_standard_handle(qemu, workdir):
    run_semihosting_one(
        qemu,
        workdir,
        ftell_failure_test(
            "semihosting-ftell-standard-handle",
            MMIX_SEMIHOSTING_STDOUT,
        ),
    )


@pytest.mark.parametrize("test", SEMIHOSTING_SERIAL_TESTS, ids=case_id)
def test_semihosting_serial(qemu, workdir, test):
    run_semihosting_serial_test(qemu, workdir, test)


@pytest.mark.parametrize("test", SEMIHOSTING_EXPECTED_FAILURE_TESTS,
                         ids=case_id)
def test_semihosting_expected_failure(qemu, workdir, test):
    run_semihosting_expected_failure(qemu, workdir, test)


@pytest.mark.parametrize("test", SEMIHOSTING_DISABLED_FAILURE_TESTS,
                         ids=case_id)
def test_semihosting_disabled_expected_failure(qemu, workdir, test):
    run_expected_failure(qemu, workdir, test)


@pytest.mark.parametrize("test", SEMIHOSTING_PROCESS_FAILURE_TESTS,
                         ids=case_id)
def test_semihosting_process_failure(qemu, workdir, test):
    run_process_failure(qemu, workdir, test)
