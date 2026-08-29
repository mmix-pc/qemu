#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *


def _hosted_image(items, *, global_base=R255, globals_=None):
    globals_ = {R255: 0} if globals_ is None else globals_
    return mmo_image([*items, mmo_post(global_base, globals_), mmo_stab_end()])


MMO_HOSTED_TESTS = [
    MMIXMMOTest(
        "mmo-hosted-startup",
        _hosted_image(
            [
                insn(GET, R2, R0, SR_L),
                insn(GET, R3, R0, SR_O),
                insn(GET, R4, R0, SR_S),
                insn(GET, R5, R0, SR_K),
                insn(GET, R6, R0, SR_Q),
                insn(GET, R7, R0, SR_T),
                insn(GET, R8, R0, SR_TT),
                insn(GET, R9, R0, SR_V),
                insn(GET, R10, R0, SR_G),
                insn(ADDI, R11, R0, 0),
                insn(ADDI, R12, R1, 0),
                insn(ADDI, R13, R254, 0),
                halt(),
            ],
            global_base=R254,
            globals_={R254: 0x123456789ABCDEF0, R255: 0},
        ),
        pc=0x30,
        regs={
            R0: 1,
            R1: MMIX_POOL_SEGMENT_BASE + 8,
            R2: 2,
            R3: MMIX_STACK_SEGMENT_BASE,
            R4: MMIX_STACK_SEGMENT_BASE,
            R5: MASK64,
            R6: 0,
            R7: 0x8000000500000000,
            R8: 0x8000000600000000,
            R9: 0x369C200400000000,
            R10: R254,
            R11: 1,
            R12: MMIX_POOL_SEGMENT_BASE + 8,
            R13: 0x123456789ABCDEF0,
            R254: 0x123456789ABCDEF0,
        },
    ),
    MMIXMMOTest(
        "mmo-hosted-data-store-load",
        _hosted_image(
            [
                *set_octa(R2, MMIX_DATA_SEGMENT_BASE + 0x100),
                *set_octa(R3, 0x8899AABBCCDDEEFF),
                insn(STOU, R3, R2, R0),
                insn(LDOU, R4, R2, R0),
                wyde(SETL, R255, 0),
                halt(),
            ]
        ),
        pc=0x2c,
        regs={R4: 0x8899AABBCCDDEEFF},
    ),
    MMIXMMOTest(
        "mmo-hosted-text-write-fetch",
        _hosted_image(
            [
                *set_octa(R2, 0x100),
                *set_octa(R3, int.from_bytes(wyde(SETL, R5, 0x77), "big")),
                insn(STTU, R3, R2, R0),
                insn(GO, R7, R2, R0),
                mmo_loc(0x104),
                halt(),
            ]
        ),
        pc=0x104,
        regs={R5: 0x77},
    ),
]


MMO_HOSTED_FAILURE_TESTS = [
    MMIXLoaderFailure(
        "mmo-hosted-explicit-elf-abi",
        _hosted_image([halt()]),
        ("does not accept an explicit ELF startup ABI",),
        qemu_args=("-machine", "elf-startup-abi=argc-argv"),
    ),
    MMIXLoaderFailure(
        "mmo-hosted-fetch-outside-text",
        _hosted_image(
            [
                *set_octa(R2, MMIX_DATA_SEGMENT_BASE),
                insn(GO, R7, R2, R0),
            ]
        ),
        ("MMIX hosted instruction fetch outside Text",),
    ),
]
