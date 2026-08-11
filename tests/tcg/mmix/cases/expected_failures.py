#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

EXPECTED_FAILURE_TESTS = [
    MMIXExpectedFailure(
        "readonly-put-rn",
        insn(PUTI, SR_N, 0, 1),
        ("MMIX illegal instruction",),
    ),
    MMIXExpectedFailure(
        "readonly-put-ro",
        insn(PUTI, SR_O, 0, 1),
        ("MMIX illegal instruction",),
    ),
    MMIXExpectedFailure(
        "readonly-put-rs",
        insn(PUTI, SR_S, 0, 1),
        ("MMIX illegal instruction",),
    ),
    MMIXExpectedFailure(
        "invalid-put-rg",
        insn(PUTI, SR_G, 0, 31),
        ("MMIX illegal instruction",),
    ),
    MMIXExpectedFailure(
        "unsupported-resume-replay",
        b"".join(
            [
                wyde(SETL, R1, 0x20),  # target address
                insn(PUT, SR_W, 0, R1),
                *set_octa(R2, 0x0000000021010001),
                insn(PUT, SR_X, 0, R2),
                insn(RESUME, 0, 0, 0),
            ]
        ),
        ("MMIX unsupported RESUME ropcode 0 instruction replay",
         "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "invalid-resume-fields",
        insn(RESUME, 1, 0, 0),
        ("MMIX invalid RESUME x=1 y=0 z=0", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "invalid-save-fields",
        insn(SAVE, R32, 1, 0),
        ("MMIX decoded unimplemented SAVE", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "invalid-save-local-destination",
        insn(SAVE, R0, 0, 0),
        ("MMIX invalid SAVE local destination 0", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "invalid-unsave-fields",
        insn(UNSAVE, 1, 0, R32),
        ("MMIX decoded unimplemented UNSAVE", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "invalid-sync",
        jump(SYNC, 8),
        ("MMIX invalid SYNC 8", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "hosted-fputs-semihosting-disabled",
        b"".join(
            [
                *set_octa(R255, 0x40),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS,
                     MMIX_SEMIHOSTING_STDOUT),
            ]
        ),
        ("MMIX semihosting disabled for hosted TRAP service 7 handle 1",
         "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "unsupported-hosted-trap-service",
        insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTWS, MMIX_SEMIHOSTING_STDOUT),
        ("MMIX unsupported hosted TRAP service 8 handle 1",
         "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "hosted-fputs-invalid-string-address",
        b"".join(
            [
                *set_octa(R255, 0x4000000000000000),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS,
                     MMIX_SEMIHOSTING_STDOUT),
            ]
        ),
        ("MMIX hosted Fputs invalid string address 0x4000000000000000",
         "MMIX illegal instruction"),
        qemu_args=MMIX_SEMIHOSTING_ARGS,
    ),
    MMIXExpectedFailure(
        "hosted-fputs-unterminated-string",
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
         "MMIX illegal instruction"),
        qemu_args=MMIX_SEMIHOSTING_ARGS,
    ),
    MMIXExpectedFailure(
        "hosted-fputs-unsupported-handle",
        b"".join(
            [
                *set_octa(R255, 0x40),
                insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS, 2),
            ]
        ),
        ("MMIX hosted Fputs unsupported handle 2",
         "MMIX illegal instruction"),
        qemu_args=MMIX_SEMIHOSTING_ARGS,
    ),
]
