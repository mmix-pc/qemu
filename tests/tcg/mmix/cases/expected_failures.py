#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

EXPECTED_FAILURE_TESTS = [
    MMIXExpectedFailure(
        "readonly-put-rn",
        insn(0xf7, 9, 0, 1),             # PUTI rN,1
        ("MMIX illegal instruction",),
    ),
    MMIXExpectedFailure(
        "readonly-put-ro",
        insn(0xf7, 10, 0, 1),            # PUTI rO,1
        ("MMIX illegal instruction",),
    ),
    MMIXExpectedFailure(
        "readonly-put-rs",
        insn(0xf7, 11, 0, 1),            # PUTI rS,1
        ("MMIX illegal instruction",),
    ),
    MMIXExpectedFailure(
        "invalid-put-rg",
        insn(0xf7, 19, 0, 31),           # PUTI rG,31
        ("MMIX illegal instruction",),
    ),
    MMIXExpectedFailure(
        "unsupported-resume-replay",
        b"".join(
            [
                wyde(0xe3, 1, 0x20),      # SETL r1,target
                insn(0xf6, 24, 0, 1),     # PUT rW,r1
                *set_octa(2, 0x0000000021010001),
                insn(0xf6, 25, 0, 2),     # PUT rX,r2
                insn(0xf9, 0, 0, 0),      # RESUME 0
            ]
        ),
        ("MMIX unsupported RESUME ropcode 0 instruction replay",
         "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "invalid-resume-fields",
        insn(0xf9, 1, 0, 0),             # RESUME with nonzero X
        ("MMIX invalid RESUME x=1 y=0 z=0", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "invalid-save-fields",
        insn(0xfa, 32, 1, 0),            # SAVE r32,1,0
        ("MMIX decoded unimplemented SAVE", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "invalid-save-local-destination",
        insn(0xfa, 0, 0, 0),             # SAVE r0,0
        ("MMIX invalid SAVE local destination 0", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "invalid-unsave-fields",
        insn(0xfb, 1, 0, 32),            # UNSAVE 1,0,r32
        ("MMIX decoded unimplemented UNSAVE", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "invalid-sync",
        jump(0xfc, 8),                   # SYNC 8
        ("MMIX invalid SYNC 8", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "unsupported-hosted-trap-service",
        insn(0x00, 0, MMIX_HOSTED_FPUTWS, MMIX_HOSTED_STDOUT),
        ("MMIX unsupported hosted TRAP service 8 handle 1",
         "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "hosted-fputs-invalid-string-address",
        b"".join(
            [
                *set_octa(255, 0x4000000000000000),
                insn(0x00, 0, MMIX_HOSTED_FPUTS, MMIX_HOSTED_STDOUT),
            ]
        ),
        ("MMIX hosted Fputs invalid string address 0x4000000000000000",
         "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "hosted-fputs-unterminated-string",
        b"".join(
            [
                *set_octa(255, 0x100),
                insn(0x00, 0, MMIX_HOSTED_FPUTS, MMIX_HOSTED_STDOUT),
                insn(0xfd, 0, 0, 0) * ((0x100 - 0x14) // 4),
                b"A" * MMIX_HOSTED_STRING_MAX,
            ]
        ),
        ("MMIX hosted Fputs string at 0x0000000000000100 exceeds 256 bytes "
         "without NUL",
         "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "hosted-fputs-unsupported-handle",
        b"".join(
            [
                *set_octa(255, 0x40),
                insn(0x00, 0, MMIX_HOSTED_FPUTS, 2),
            ]
        ),
        ("MMIX hosted Fputs unsupported handle 2",
         "MMIX illegal instruction"),
    ),
]
