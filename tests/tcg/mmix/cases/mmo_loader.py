#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

MMO_LOADER_TESTS = [
    MMIXMMOTest(
        "mmo-straight-line-load",
        mmo_image(
            [
                wyde(0xe3, 1, 0x42),       # SETL r1,0x42
                halt(),
            ]
        ),
        pc=0x04,
        regs={1: 0x42},
    ),
    MMIXMMOTest(
        "mmo-quote-load",
        mmo_image(
            [
                mmo_quote(wyde(0xe3, 1, 0x77)),   # SETL r1,0x77
                halt(),
            ]
        ),
        pc=0x04,
        regs={1: 0x77},
    ),
    MMIXMMOTest(
        "mmo-overlapping-tetrabytes-xor",
        mmo_image(
            [
                wyde(0xe3, 1, 0),         # SETL r1,0 before XOR overlay
                mmo_loc(0),
                struct.pack(">I", 0x00000066),
                halt(),
            ]
        ),
        pc=0x04,
        regs={1: 0x66},
    ),
    MMIXMMOTest(
        "mmo-loc-sparse-load",
        mmo_image(
            [
                jump(0xf0, 8),             # JMP 0x20
                mmo_loc(0x20),
                wyde(0xe3, 1, 0x33),       # SETL r1,0x33
                halt(),
            ]
        ),
        pc=0x24,
        regs={1: 0x33},
    ),
    MMIXMMOTest(
        "mmo-skip-sparse-load",
        mmo_image(
            [
                jump(0xf0, 8),             # JMP 0x20
                mmo_skip(0x1c),
                wyde(0xe3, 1, 0x55),       # SETL r1,0x55
                halt(),
            ]
        ),
        pc=0x24,
        regs={1: 0x55},
    ),
    MMIXMMOTest(
        "mmo-spec-before-code",
        mmo_image(
            [
                mmo_spec(1),
                struct.pack(">I", 0x11223344),
                mmo_quote(mmo_lop(MMIX_MMO_LOP_POST, 0)),
                mmo_loc(0),
                wyde(0xe3, 1, 0x44),       # SETL r1,0x44
                halt(),
            ]
        ),
        pc=0x04,
        regs={1: 0x44},
    ),
    MMIXMMOTest(
        "mmo-spec-between-records",
        mmo_image(
            [
                wyde(0xe3, 1, 0x11),       # SETL r1,0x11
                mmo_spec(2),
                struct.pack(">I", 0x55667788),
                mmo_skip(0),
                wyde(0xe3, 2, 0x22),       # SETL r2,0x22
                halt(),
            ]
        ),
        pc=0x08,
        regs={1: 0x11, 2: 0x22},
    ),
    MMIXMMOTest(
        "mmo-spec-before-postamble",
        mmo_image(
            [
                mmo_loc(0x20),
                wyde(0xe3, 3, 0x33),       # SETL r3,0x33
                halt(),
                mmo_spec(3),
                struct.pack(">I", 0x99aabbcc),
                mmo_post(255, {255: 0x20}),
                mmo_stab_end(),
            ]
        ),
        pc=0x24,
        regs={3: 0x33, 255: 0x20},
    ),
    MMIXMMOTest(
        "mmo-data-segment-load",
        mmo_image(
            [
                *set_octa(1, MMIX_DATA_SEGMENT_BASE + 0x100),
                insn(0x8e, 2, 1, 0),      # LDOU r2,r1,r0
                halt(),
                mmo_loc(MMIX_DATA_SEGMENT_BASE + 0x100),
                struct.pack(">Q", 0x1122334455667788),
            ]
        ),
        pc=0x14,
        regs={2: 0x1122334455667788},
    ),
    MMIXMMOTest(
        "mmo-data-segment-store",
        mmo_image(
            [
                *set_octa(1, MMIX_DATA_SEGMENT_BASE + 0x120),
                *set_octa(2, 0x8877665544332211),
                insn(0xae, 2, 1, 0),      # STOU r2,r1,r0
                insn(0x8e, 3, 1, 0),      # LDOU r3,r1,r0
                halt(),
            ]
        ),
        pc=0x28,
        regs={3: 0x8877665544332211},
    ),
    MMIXMMOTest(
        "mmo-fixo-load",
        mmo_image(
            [
                *set_octa(1, 0x80),
                insn(0x8e, 2, 1, 0),      # LDOU r2,r1,r0
                halt(),
                mmo_fixo(0x80),
            ]
        ),
        pc=0x14,
        regs={2: 0x18},
    ),
    MMIXMMOTest(
        "mmo-fixo-xor-overlay",
        mmo_image(
            [
                *set_octa(1, 0x80),
                insn(0x8e, 2, 1, 0),      # LDOU r2,r1,r0
                halt(),
                mmo_loc(0x80),
                struct.pack(">Q", 0x10),
                mmo_loc(0x18),
                mmo_fixo(0x80),
            ]
        ),
        pc=0x14,
        regs={2: 0x08},
    ),
    MMIXMMOTest(
        "mmo-fixo-data-segment-target",
        mmo_image(
            [
                *set_octa(1, MMIX_DATA_SEGMENT_BASE + 0x200),
                insn(0x8e, 2, 1, 0),      # LDOU r2,r1,r0
                halt(),
                mmo_fixo(MMIX_DATA_SEGMENT_BASE + 0x200),
            ]
        ),
        pc=0x14,
        regs={2: 0x18},
    ),
    MMIXMMOTest(
        "mmo-fixr-branch-forward",
        mmo_image(
            [
                wyde(0xe3, 1, 1),         # SETL r1,1
                branch(0x4a, 1, 0),       # BNZ r1,target, fixed later
                wyde(0xe3, 2, 0xaa),      # skipped after fixup
                mmo_loc(0x0c),
                mmo_fixr(2),
                wyde(0xe3, 2, 0x55),      # target: SETL r2,0x55
                halt(),
            ]
        ),
        pc=0x10,
        regs={2: 0x55},
    ),
    MMIXMMOTest(
        "mmo-fixr-geta-forward",
        mmo_image(
            [
                branch(0xf4, 1, 0),       # GETA r1,target, fixed later
                halt(),
                mmo_loc(0x08),
                mmo_fixr(2),
                wyde(0xe3, 2, 0x33),      # target data/code location
            ]
        ),
        pc=0x04,
        regs={1: 0x08, 2: 0},
    ),
    MMIXMMOTest(
        "mmo-fixrx-probable-branch-backward",
        mmo_image(
            [
                jump(0xf0, 3),            # JMP branch
                mmo_loc(0x0c),
                branch(0x52, 0, 0),       # PBZ r0,target, fixed later
                mmo_loc(0x04),
                mmo_fixrx(16, 0x0100fffe),
                wyde(0xe3, 3, 0x66),      # target: SETL r3,0x66
                halt(),
            ]
        ),
        pc=0x08,
        regs={3: 0x66},
    ),
    MMIXMMOTest(
        "mmo-fixrx-jmp-backward",
        mmo_image(
            [
                mmo_loc(0x08),
                jump(0xf0, 0),            # JMP target, fixed later
                mmo_loc(0x00),
                mmo_fixrx(24, 0x01fffffe),
                wyde(0xe3, 4, 0x77),      # target: SETL r4,0x77
                halt(),
                mmo_post(255, {255: 0x08}),
                mmo_stab_end(),
            ]
        ),
        pc=0x04,
        regs={4: 0x77, 255: 0x08},
    ),
    MMIXMMOTest(
        "mmo-post-entry-globals",
        mmo_image(
            [
                mmo_loc(0x40),
                insn(0xfe, 1, 0, 19),     # GET r1,rG
                insn(0x21, 3, 255, 0),    # ADDI r3,r255,0
                halt(),
                mmo_post(254, {254: 0x123456789ABCDEF0, 255: 0x40}),
                mmo_stab_end(),
            ]
        ),
        pc=0x48,
        regs={
            1: 254,
            3: 0x40,
            254: 0x123456789ABCDEF0,
            255: 0x40,
        },
    ),
]
