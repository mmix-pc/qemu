#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *


EXPECTED_FAILURE_TESTS = []


def semihosting_disabled_trap_test(name, program):
    entry = MMIX_NEGATIVE_ALIAS_BIT | MMIX_RAW_ENTRY
    handler = entry + 0x100
    image = elf64_image(
        MMIX_RAW_ENTRY,
        program_with_handler(
            [
                *set_octa(R1, handler),
                insn(PUT, SR_T, 0, R1),
                program,
            ],
            0x100,
            [jump(JMP, 0)],
        ),
        entry=entry,
        virtual_address=entry,
    )

    return MMIXExpectedFailure(
        name,
        image,
        ("MMIX trap from",),
        ("MMIX hosted", "MMIX emulator failure", "MMIX test exit",
         "MMIX dynamic trap causes="),
    )


SEMIHOSTING_DISABLED_TRAP_TESTS = [
    semihosting_disabled_trap_test(
        "semihosting-halt-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_HALT, 0),
    ),
    semihosting_disabled_trap_test(
        "semihosting-fputs-stdout-disabled",
        b"".join(
            [
                *set_octa(R255, 0x40),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS,
                     MMIX_SEMIHOSTING_STDOUT),
            ]
        ),
    ),
    semihosting_disabled_trap_test(
        "semihosting-fputs-stderr-disabled",
        b"".join(
            [
                *set_octa(R255, 0x40),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS,
                     MMIX_SEMIHOSTING_STDERR),
            ]
        ),
    ),
    semihosting_disabled_trap_test(
        "semihosting-fread-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FREAD,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
    ),
    semihosting_disabled_trap_test(
        "semihosting-fread-stdin-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FREAD, MMIX_SEMIHOSTING_STDIN),
    ),
    semihosting_disabled_trap_test(
        "semihosting-fgets-stdin-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FGETS, MMIX_SEMIHOSTING_STDIN),
    ),
    semihosting_disabled_trap_test(
        "semihosting-fwrite-stdout-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FWRITE, MMIX_SEMIHOSTING_STDOUT),
    ),
    semihosting_disabled_trap_test(
        "semihosting-fseek-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FSEEK,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
    ),
    semihosting_disabled_trap_test(
        "semihosting-ftell-disabled",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FTELL,
             MMIX_SEMIHOSTING_FIRST_FILE_HANDLE),
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
        (f"MMIX hosted Fputs invalid sparse string address "
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
        "semihosting-argv-below-minimum-ram",
        halt(),
        ("-m", "127M", "-semihosting-config", "enable=on,arg=prog"),
        ("MMIX virt RAM size 0x7f00000 is below the minimum 0x8000000",),
    ),
]
