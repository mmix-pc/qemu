#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

MMO_LOADER_TESTS = [
    MMIXMMOTest(
        "mmo-elf-startup-abi-ignored",
        mmo_image(
            [
                wyde(SETL, R1, 0x42),
                halt(),
            ]
        ),
        pc=0x04,
        regs={R1: 0x42},
        qemu_args=("-machine", "elf-startup-abi=argc-argv"),
    ),
    MMIXMMOTest(
        "mmo-straight-line-load",
        mmo_image(
            [
                wyde(SETL, R1, 0x42),
                halt(),
            ]
        ),
        pc=0x04,
        regs={R1: 0x42},
    ),
    MMIXMMOTest(
        "mmo-quote-load",
        mmo_image(
            [
                mmo_quote(wyde(SETL, R1, 0x77)),
                halt(),
            ]
        ),
        pc=0x04,
        regs={R1: 0x77},
    ),
    MMIXMMOTest(
        "mmo-overlapping-tetrabytes-xor",
        mmo_image(
            [
                wyde(SETL, R1, 0),
                mmo_loc(0),
                struct.pack(">I", 0x00000066),
                halt(),
            ]
        ),
        pc=0x04,
        regs={R1: 0x66},
    ),
    MMIXMMOTest(
        "mmo-loc-sparse-load",
        mmo_image(
            [
                jump(JMP, 8),
                mmo_loc(0x20),
                wyde(SETL, R1, 0x33),
                halt(),
            ]
        ),
        pc=0x24,
        regs={R1: 0x33},
    ),
    MMIXMMOTest(
        "mmo-direct-high-ram-load",
        mmo_image(
            [
                *set_octa(R1, 0x20000000),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_loc(0x20000000),
                struct.pack(">Q", 0x0123456789abcdef),
            ]
        ),
        pc=0x14,
        regs={R2: 0x0123456789abcdef},
        qemu_args=("-m", "512M"),
    ),
    MMIXMMOTest(
        "mmo-direct-high-ram-last-octa-load",
        mmo_image(
            [
                *set_octa(R1, 0x2ffffff8),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_loc(0x2ffffff8),
                struct.pack(">Q", 0xfedcba9876543210),
            ]
        ),
        pc=0x14,
        regs={R2: 0xfedcba9876543210},
        qemu_args=("-m", "512M"),
    ),
    MMIXMMOTest(
        "mmo-skip-sparse-load",
        mmo_image(
            [
                jump(JMP, 8),
                mmo_skip(0x1c),
                wyde(SETL, R1, 0x55),
                halt(),
            ]
        ),
        pc=0x24,
        regs={R1: 0x55},
    ),
    MMIXMMOTest(
        "mmo-spec-before-code",
        mmo_image(
            [
                mmo_spec(1),
                struct.pack(">I", 0x11223344),
                mmo_quote(mmo_lop(MMIX_MMO_LOP_POST, 0)),
                mmo_loc(0),
                wyde(SETL, R1, 0x44),
                halt(),
            ]
        ),
        pc=0x04,
        regs={R1: 0x44},
    ),
    MMIXMMOTest(
        "mmo-spec-between-records",
        mmo_image(
            [
                wyde(SETL, R1, 0x11),
                mmo_spec(2),
                struct.pack(">I", 0x55667788),
                mmo_skip(0),
                wyde(SETL, R2, 0x22),
                halt(),
            ]
        ),
        pc=0x08,
        regs={R1: 0x11, R2: 0x22},
    ),
    MMIXMMOTest(
        "mmo-spec-before-postamble",
        mmo_image(
            [
                mmo_loc(0x20),
                wyde(SETL, R3, 0x33),
                insn(ADDI, R4, R255, 0),
                wyde(SETL, R255, 0),
                halt(),
                mmo_spec(3),
                struct.pack(">I", 0x99aabbcc),
                mmo_post(R255, {R255: 0x20}),
                mmo_stab_end(),
            ]
        ),
        pc=0x2c,
        regs={R3: 0x33, R4: 0x20},
    ),
    MMIXMMOTest(
        "mmo-data-segment-load",
        mmo_image(
            [
                *set_octa(R1, MMIX_DATA_SEGMENT_BASE + 0x100),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_loc(MMIX_DATA_SEGMENT_BASE + 0x100),
                struct.pack(">Q", 0x1122334455667788),
            ]
        ),
        pc=0x14,
        regs={R2: 0x1122334455667788},
    ),
    MMIXMMOTest(
        "mmo-data-segment-last-octa-load",
        mmo_image(
            [
                *set_octa(R1, MMIX_DATA_SEGMENT_BASE +
                          MMIX_DATA_SEGMENT_SIZE - 8),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_loc(MMIX_DATA_SEGMENT_BASE + MMIX_DATA_SEGMENT_SIZE - 8),
                struct.pack(">Q", 0x99aabbccddeeff00),
            ]
        ),
        pc=0x14,
        regs={R2: 0x99aabbccddeeff00},
    ),
    MMIXMMOTest(
        "mmo-data-segment-store",
        mmo_image(
            [
                *set_octa(R1, MMIX_DATA_SEGMENT_BASE + 0x120),
                *set_octa(R2, 0x8877665544332211),
                insn(STOU, R2, R1, R0),
                insn(LDOU, R3, R1, R0),
                halt(),
            ]
        ),
        pc=0x28,
        regs={R3: 0x8877665544332211},
    ),
    MMIXMMOTest(
        "mmo-pool-segment-load",
        mmo_image(
            [
                *set_octa(R1, MMIX_POOL_SEGMENT_BASE + 0x100),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_loc(MMIX_POOL_SEGMENT_BASE + 0x100),
                struct.pack(">Q", 0x0102030405060708),
            ]
        ),
        pc=0x14,
        regs={R2: 0x0102030405060708},
    ),
    MMIXMMOTest(
        "mmo-pool-segment-last-octa-load",
        mmo_image(
            [
                *set_octa(R1, MMIX_POOL_SEGMENT_BASE +
                          MMIX_POOL_SEGMENT_SIZE - 8),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_loc(MMIX_POOL_SEGMENT_BASE + MMIX_POOL_SEGMENT_SIZE - 8),
                struct.pack(">Q", 0xfedcba9876543210),
            ]
        ),
        pc=0x14,
        regs={R2: 0xfedcba9876543210},
    ),
    MMIXMMOTest(
        "mmo-stack-segment-load",
        mmo_image(
            [
                *set_octa(R1, MMIX_STACK_SEGMENT_BASE + 0x100),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_loc(MMIX_STACK_SEGMENT_BASE + 0x100),
                struct.pack(">Q", 0x0011223344556677),
            ]
        ),
        pc=0x14,
        regs={R2: 0x0011223344556677},
    ),
    MMIXMMOTest(
        "mmo-stack-segment-last-octa-load",
        mmo_image(
            [
                *set_octa(R1, MMIX_STACK_SEGMENT_BASE +
                          MMIX_STACK_SEGMENT_SIZE - 8),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_loc(MMIX_STACK_SEGMENT_BASE + MMIX_STACK_SEGMENT_SIZE - 8),
                struct.pack(">Q", 0x8899aabbccddeeff),
            ]
        ),
        pc=0x14,
        regs={R2: 0x8899aabbccddeeff},
    ),
    MMIXMMOTest(
        "mmo-fixo-load",
        mmo_image(
            [
                *set_octa(R1, 0x80),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_fixo(0x80),
            ]
        ),
        pc=0x14,
        regs={R2: 0x18},
    ),
    MMIXMMOTest(
        "mmo-fixo-xor-overlay",
        mmo_image(
            [
                *set_octa(R1, 0x80),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_loc(0x80),
                struct.pack(">Q", 0x10),
                mmo_loc(0x18),
                mmo_fixo(0x80),
            ]
        ),
        pc=0x14,
        regs={R2: 0x08},
    ),
    MMIXMMOTest(
        "mmo-fixo-data-segment-target",
        mmo_image(
            [
                *set_octa(R1, MMIX_DATA_SEGMENT_BASE + 0x200),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_fixo(MMIX_DATA_SEGMENT_BASE + 0x200),
            ]
        ),
        pc=0x14,
        regs={R2: 0x18},
    ),
    MMIXMMOTest(
        "mmo-fixo-stack-segment-target",
        mmo_image(
            [
                *set_octa(R1, MMIX_STACK_SEGMENT_BASE + 0x200),
                insn(LDOU, R2, R1, R0),
                halt(),
                mmo_fixo(MMIX_STACK_SEGMENT_BASE + 0x200),
            ]
        ),
        pc=0x14,
        regs={R2: 0x18},
    ),
    MMIXMMOTest(
        "mmo-fixr-branch-forward",
        mmo_image(
            [
                wyde(SETL, R1, 1),
                branch(BNZ, R1, 0),  # target, fixed later
                wyde(SETL, R2, 0xaa),      # skipped after fixup
                mmo_loc(0x0c),
                mmo_fixr(2),
                wyde(SETL, R2, 0x55),  # target
                halt(),
            ]
        ),
        pc=0x10,
        regs={R2: 0x55},
    ),
    MMIXMMOTest(
        "mmo-fixr-geta-forward",
        mmo_image(
            [
                branch(GETA, R1, 0),  # target, fixed later
                halt(),
                mmo_loc(0x08),
                mmo_fixr(2),
                wyde(SETL, R2, 0x33),      # target data/code location
            ]
        ),
        pc=0x04,
        regs={R1: 0x08, R2: 0},
    ),
    MMIXMMOTest(
        "mmo-fixrx-probable-branch-backward",
        mmo_image(
            [
                jump(JMP, 3),
                mmo_loc(0x0c),
                branch(PBZ, R0, 0),  # target, fixed later
                mmo_loc(0x04),
                mmo_fixrx(16, 0x0100fffe),
                wyde(SETL, R3, 0x66),  # target
                halt(),
            ]
        ),
        pc=0x08,
        regs={R3: 0x66},
    ),
    MMIXMMOTest(
        "mmo-fixrx-jmp-backward",
        mmo_image(
            [
                mmo_loc(0x08),
                jump(JMP, 0),  # target, fixed later
                mmo_loc(0x00),
                mmo_fixrx(24, 0x01fffffe),
                wyde(SETL, R4, 0x77),  # target
                halt(),
                mmo_post(R255, {R255: 0}),
                mmo_stab_end(),
            ]
        ),
        pc=0x04,
        regs={R4: 0x77},
    ),
    MMIXMMOTest(
        "mmo-post-entry-globals",
        mmo_image(
            [
                mmo_loc(0x40),
                insn(GET, R1, 0, SR_G),
                insn(ADDI, R3, R255, 0),
                wyde(SETL, R255, 0),
                halt(),
                mmo_post(R254, {R254: 0x123456789ABCDEF0, R255: 0x40}),
                mmo_stab_end(),
            ]
        ),
        pc=0x4c,
        regs={R1: 254, R3: 0x40, R254: 0x123456789ABCDEF0},
    ),
]
