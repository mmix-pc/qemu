#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

EXPECTED_FAILURE_TESTS = [
    MMIXExpectedFailure(
        "unsupported-resume-operands",
        b"".join(
            [
                wyde(SETL, R1, 0x20),
                insn(PUT, SR_W, 0, R1),
                *set_octa(R2, 0x0100000021010001),
                insn(PUT, SR_X, 0, R2),
                insn(RESUME, 0, 0, 0),
            ]
        ),
        ("MMIX unsupported RESUME ropcode 1 operand substitution",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "register-stack-underflow",
        insn(POP, 0, 0, 0),
        ("MMIX register stack underflow during POP",
         "MMIX emulator failure"),
    ),
]

SEMIHOSTING_DISABLED_FAILURE_TESTS = [
    MMIXExpectedFailure(
        "semihosting-fputs-stdout-disabled",
        b"".join(
            [
                wyde(SETL, R10, 0x100),
                insn(PUT, SR_TT, 0, R10),
                *set_octa(R11, RQ_PROGRAM_B),
                insn(PUT, SR_K, 0, R11),
                *set_octa(R255, 0x40),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS,
                     MMIX_SEMIHOSTING_STDOUT),
            ]
        ),
        ("MMIX semihosting disabled for hosted TRAP service 7 handle 1",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "semihosting-fputs-stderr-disabled",
        b"".join(
            [
                *set_octa(R255, 0x40),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS,
                     MMIX_SEMIHOSTING_STDERR),
            ]
        ),
        ("MMIX semihosting disabled for hosted TRAP service 7 handle 2",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "semihosting-fread-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FREAD,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        ("MMIX semihosting disabled for hosted TRAP service 3 handle 3",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "semihosting-fread-stdin-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FREAD, MMIX_SEMIHOSTING_STDIN),
        ("MMIX semihosting disabled for hosted TRAP service 3 handle 0",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "semihosting-fgets-stdin-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FGETS, MMIX_SEMIHOSTING_STDIN),
        ("MMIX semihosting disabled for hosted TRAP service 4 handle 0",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "semihosting-fwrite-stdout-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FWRITE, MMIX_SEMIHOSTING_STDOUT),
        ("MMIX semihosting disabled for hosted TRAP service 6 handle 1",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "semihosting-fseek-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FSEEK,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        ("MMIX semihosting disabled for hosted TRAP service 9 handle 3",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "semihosting-ftell-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FTELL,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
        ("MMIX semihosting disabled for hosted TRAP service 10 handle 3",
         "MMIX emulator failure"),
    ),
]

SEMIHOSTING_EXPECTED_FAILURE_TESTS = [
    MMIXExpectedFailure(
        "semihosting-unsupported-fgetws",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FGETWS,
             MMIX_SEMIHOSTING_STDIN),
        ("MMIX unsupported hosted TRAP service 5 handle 0",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "semihosting-unsupported-trap-service",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTWS, MMIX_SEMIHOSTING_STDOUT),
        ("MMIX unsupported hosted TRAP service 8 handle 1",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "semihosting-fputs-invalid-string-address",
        b"".join(
            [
                *set_octa(R255, MMIX_UNSUPPORTED_HIGH_SEGMENT_ADDRESS),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS,
                     MMIX_SEMIHOSTING_STDOUT),
            ]
        ),
        (f"MMIX hosted Fputs invalid string address "
         f"0x{MMIX_UNSUPPORTED_HIGH_SEGMENT_ADDRESS:016x}",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "semihosting-fputs-unterminated-string",
        b"".join(
            [
                *set_octa(R255, 0x100),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS,
                     MMIX_SEMIHOSTING_STDOUT),
                insn(SWYM, 0, 0, 0) * ((0x100 - 0x14) // 4),
                b"A" * MMIX_SEMIHOSTING_STRING_MAX,
            ]
        ),
        ("MMIX hosted Fputs string at 0x0000000000000100 exceeds 256 bytes "
         "without NUL",
         "MMIX emulator failure"),
    ),
    MMIXExpectedFailure(
        "semihosting-fputs-stdin-handle",
        b"".join(
            [
                *set_octa(R255, 0x40),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS,
                     MMIX_SEMIHOSTING_STDIN),
            ]
        ),
        ("MMIX hosted Fputs unsupported handle 0",
         "MMIX emulator failure"),
    ),
]

SEMIHOSTING_PROCESS_FAILURE_TESTS = [
    MMIXProcessFailure(
        "semihosting-argv-block-outside-ram",
        halt(),
        ("-m", "16M", "-semihosting-config", "enable=on,arg=prog"),
        ("could not set up MMIX semihosting arguments",
         "MMIX semihosting argument block does not fit in machine RAM"),
    ),
    MMIXProcessFailure(
        "semihosting-argv-pool-backing-outside-ram",
        halt(),
        ("-m", "64M", "-semihosting-config", "enable=on,arg=prog"),
        ("could not set up MMIX semihosting arguments",
         "MMIX semihosting argument block does not fit in machine RAM"),
    ),
]
