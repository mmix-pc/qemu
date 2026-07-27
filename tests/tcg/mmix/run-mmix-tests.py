#!/usr/bin/env python3
#
# MMIX raw-image smoke tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import dataclasses
import pathlib
import re
import struct
import subprocess
import sys

MASK64 = (1 << 64) - 1
INITIAL_STACK = 0x00010000
RA_EVENT_X = 0x01
RA_EVENT_Z = 0x02
RA_EVENT_O = 0x08
RA_EVENT_V = 0x40
RA_EVENT_D = 0x80
RA_ENABLE_SHIFT = 8
RQ_PROGRAM_K = 1 << 35


def insn(op, x=0, y=0, z=0):
    return bytes((op & 0xff, x & 0xff, y & 0xff, z & 0xff))


def branch(op, x, yz):
    return insn(op, x, (yz >> 8) & 0xff, yz & 0xff)


def jump(op, xyz):
    return insn(op, (xyz >> 16) & 0xff, (xyz >> 8) & 0xff, xyz & 0xff)


def wyde(op, x, yz):
    return insn(op, x, (yz >> 8) & 0xff, yz & 0xff)


def halt():
    return insn(0x00, 0, 0, 0)


def set_octa(reg, value):
    return [
        wyde(0xe0, reg, (value >> 48) & 0xffff),
        wyde(0xe5, reg, (value >> 32) & 0xffff),
        wyde(0xe6, reg, (value >> 16) & 0xffff),
        wyde(0xe7, reg, value & 0xffff),
    ]


def program_with_handler(prefix, handler_addr, handler):
    prefix = b"".join(prefix)
    handler = b"".join(handler)
    if len(prefix) > handler_addr:
        raise ValueError("handler address overlaps program prefix")
    if (handler_addr - len(prefix)) % 4 != 0:
        raise ValueError("handler address is not instruction-aligned")
    padding = insn(0xfd, 0, 0, 0) * ((handler_addr - len(prefix)) // 4)
    return prefix + padding + handler


def register_stack_spill_fill_program(depth):
    sub_base = 0x20
    body_size = 6 * 4
    program = [
        branch(0xf2, 31, sub_base // 4),   # PUSHJ 31,sub0
        insn(0x21, 60, 31, 0),             # ADDI r60,r31,0
        insn(0xfe, 50, 0, 10),             # GET r50,rO
        insn(0xfe, 51, 0, 11),             # GET r51,rS
        halt(),
    ]

    program.extend([insn(0xfd, 0, 0, 0)] * ((sub_base - len(program) * 4) // 4))

    for level in range(depth):
        program.extend(
            [
                insn(0xfe, 40 + level, 0, 4),  # GET global,rJ
                wyde(0xe3, 31, level + 1),     # SETL r31,level+1
                branch(0xf2, 31, 4),           # PUSHJ 31,next
                insn(0x21, 0, 31, 1),          # ADDI r0,r31,1
                insn(0xf6, 4, 0, 40 + level),  # PUT rJ,global
                insn(0xf8, 1, 0, 0),           # POP 1,0
            ]
        )

    program.extend(
        [
            wyde(0xe3, 0, 1),                  # SETL r0,1
            insn(0xf8, 1, 0, 0),               # POP 1,0
        ]
    )

    image = b"".join(program)
    expected_image_len = sub_base + depth * body_size + 2 * 4
    if len(image) != expected_image_len:
        raise AssertionError("register-stack spill/fill image layout changed")
    return image, 4 * 4, depth + 1


def register_stack_save_unsave_program(depth):
    sub_base = 0x20
    body_size = 6 * 4
    program = [
        branch(0xf2, 31, sub_base // 4),   # PUSHJ 31,sub0
        insn(0x21, 60, 31, 0),             # ADDI r60,r31,0
        insn(0xfe, 50, 0, 10),             # GET r50,rO
        insn(0xfe, 51, 0, 11),             # GET r51,rS
        halt(),
    ]

    program.extend([insn(0xfd, 0, 0, 0)] * ((sub_base - len(program) * 4) // 4))

    for level in range(depth):
        program.extend(
            [
                insn(0xfe, 40 + level, 0, 4),  # GET global,rJ
                wyde(0xe3, 31, level + 1),     # SETL r31,level+1
                branch(0xf2, 31, 4),           # PUSHJ 31,next
                insn(0x21, 0, 31, 1),          # ADDI r0,r31,1
                insn(0xf6, 4, 0, 40 + level),  # PUT rJ,global
                insn(0xf8, 1, 0, 0),           # POP 1,0
            ]
        )

    program.extend(
        [
            wyde(0xe3, 0, 0x55),               # SETL r0,0x55
            insn(0xfa, 32, 0, 0),              # SAVE r32,0
            wyde(0xe3, 0, 0xaa),               # SETL r0,0xaa
            insn(0xfb, 0, 0, 32),              # UNSAVE 0,r32
            insn(0xf8, 1, 0, 0),               # POP 1,0
        ]
    )

    image = b"".join(program)
    expected_image_len = sub_base + depth * body_size + 5 * 4
    if len(image) != expected_image_len:
        raise AssertionError("register-stack save/unsave image layout changed")
    return image, 4 * 4, 0x55 + depth


def save_state_after_save_program():
    program = [
        wyde(0xe3, 0, 0x11),               # SETL r0,0x11
        wyde(0xe3, 1, 0x22),               # SETL r1,0x22
        insn(0xfa, 32, 0, 0),              # SAVE r32,0
        insn(0xfe, 33, 0, 20),             # GET r33,rL
        insn(0xfe, 34, 0, 10),             # GET r34,rO
        insn(0xfe, 35, 0, 11),             # GET r35,rS
        insn(0x21, 36, 32, 0),             # ADDI r36,r32,0
        halt(),
    ]
    return b"".join(program), (len(program) - 1) * 4


def save_unsave_roundtrip_program():
    program = [
        wyde(0xe3, 0, 0x11),               # SETL r0,0x11
        wyde(0xe3, 1, 0x22),               # SETL r1,0x22
        wyde(0xe3, 2, 0x33),               # SETL r2,0x33
        *set_octa(40, 0x1111222233334444),
        *set_octa(41, 0x5555666677778888),
        *set_octa(42, 0x0000000000001234),
        insn(0xf6, 4, 0, 42),              # PUT rJ,r42
        insn(0xf7, 5, 0, 0x5a),            # PUTI rM,0x5a
        insn(0xf7, 23, 0, 0x6b),           # PUTI rP,0x6b
        *set_octa(43, 0x000000000003ffff),
        insn(0xf6, 21, 0, 43),             # PUT rA,r43
        insn(0xfa, 32, 0, 0),              # SAVE r32,0
        insn(0x21, 33, 32, 0),             # ADDI r33,r32,0
        wyde(0xe3, 40, 0),                 # SETL r40,0
        wyde(0xe3, 41, 0),                 # SETL r41,0
        insn(0xf7, 4, 0, 0),               # PUTI rJ,0
        insn(0xf7, 5, 0, 0),               # PUTI rM,0
        insn(0xf7, 23, 0, 0),              # PUTI rP,0
        insn(0xf7, 21, 0, 0),              # PUTI rA,0
        wyde(0xe3, 0, 0xee),               # SETL r0,0xee
        wyde(0xe3, 1, 0xff),               # SETL r1,0xff
        wyde(0xe3, 2, 0xaa),               # SETL r2,0xaa
        insn(0xfb, 0, 0, 33),              # UNSAVE 0,r33
        insn(0x21, 50, 0, 0),              # ADDI r50,r0,0
        insn(0x21, 51, 1, 0),              # ADDI r51,r1,0
        insn(0x21, 52, 2, 0),              # ADDI r52,r2,0
        insn(0xfe, 53, 0, 4),              # GET r53,rJ
        insn(0xfe, 54, 0, 5),              # GET r54,rM
        insn(0xfe, 55, 0, 23),             # GET r55,rP
        insn(0xfe, 56, 0, 21),             # GET r56,rA
        insn(0xfe, 57, 0, 20),             # GET r57,rL
        insn(0xfe, 58, 0, 10),             # GET r58,rO
        insn(0xfe, 59, 0, 11),             # GET r59,rS
        insn(0x21, 60, 40, 0),             # ADDI r60,r40,0
        insn(0x21, 61, 41, 0),             # ADDI r61,r41,0
        insn(0x21, 62, 32, 0),             # ADDI r62,r32,0
        insn(0x21, 63, 33, 0),             # ADDI r63,r33,0
        halt(),
    ]
    return b"".join(program), (len(program) - 1) * 4


def lane_difference(y, z, lane_bits):
    result = 0
    mask = (1 << lane_bits) - 1
    for shift in range(0, 64, lane_bits):
        y_lane = (y >> shift) & mask
        z_lane = (z >> shift) & mask
        if y_lane > z_lane:
            result |= (y_lane - z_lane) << shift
    return result


def sadd(y, z):
    return (y & (~z & MASK64)).bit_count()


def matrix_byte(value, row):
    return (value >> ((7 - row) * 8)) & 0xff


def matrix_multiply(y, z, exclusive):
    result = 0
    for i in range(8):
        z_row = matrix_byte(z, i)
        x_row = 0
        for j in range(8):
            bit = 0
            for k in range(8):
                y_bit = matrix_byte(y, k) & (0x80 >> j)
                z_bit = z_row & (0x80 >> k)
                if exclusive:
                    bit ^= bool(y_bit and z_bit)
                else:
                    bit |= bool(y_bit and z_bit)
            if bit:
                x_row |= 0x80 >> j
        result |= x_row << ((7 - i) * 8)
    return result


def mux(y, z, mask):
    return ((y & mask) | (z & ~mask)) & MASK64


def signed_div(y, z):
    y = s64(y)
    z = s64(z)
    if z == 0:
        return 0, y & MASK64
    quotient = y // z
    remainder = y - quotient * z
    return quotient & MASK64, remainder & MASK64


def unsigned_div(high, low, divisor):
    if divisor == 0 or high >= divisor:
        return high & MASK64, low & MASK64
    dividend = (high << 64) | low
    return (dividend // divisor) & MASK64, (dividend % divisor) & MASK64


def s64(value):
    value &= MASK64
    return value - (1 << 64) if value & (1 << 63) else value


def f64(value):
    return struct.unpack(">Q", struct.pack(">d", value))[0]


def f32(value):
    return struct.unpack(">I", struct.pack(">f", value))[0]


@dataclasses.dataclass(frozen=True)
class MMIXTest:
    name: str
    program: bytes
    pc: int
    regs: dict[int, int]


@dataclasses.dataclass(frozen=True)
class MMIXExpectedFailure:
    name: str
    program: bytes
    patterns: tuple[str, ...]
    absent: tuple[str, ...] = ("MMIX test exit",)


REGISTER_STACK_SPILL_FILL = register_stack_spill_fill_program(10)
REGISTER_STACK_SAVE_UNSAVE = register_stack_save_unsave_program(10)
SAVE_STATE_AFTER_SAVE = save_state_after_save_program()
SAVE_UNSAVE_ROUNDTRIP = save_unsave_roundtrip_program()


TESTS = [
    MMIXTest(
        "alu-logical",
        b"".join(
            [
                insn(0x21, 1, 0, 5),      # ADDI r1,r0,5
                insn(0x21, 2, 0, 7),      # ADDI r2,r0,7
                insn(0x20, 3, 1, 2),      # ADD r3,r1,r2
                insn(0x25, 4, 3, 2),      # SUBI r4,r3,2
                insn(0xc1, 5, 4, 0x80),   # ORI r5,r4,0x80
                insn(0xc7, 6, 5, 0xff),   # XORI r6,r5,0xff
                insn(0xc9, 7, 6, 0x0f),   # ANDI r7,r6,0x0f
                halt(),
            ]
        ),
        pc=0x1c,
        regs={1: 5, 2: 7, 3: 0x0c, 4: 0x0a, 5: 0x8a, 6: 0x75, 7: 5},
    ),
    MMIXTest(
        "compare",
        b"".join(
            [
                insn(0x21, 1, 0, 5),      # ADDI r1,r0,5
                insn(0x21, 2, 0, 7),      # ADDI r2,r0,7
                insn(0x30, 3, 1, 2),      # CMP r3,r1,r2
                insn(0x30, 4, 2, 2),      # CMP r4,r2,r2
                insn(0x30, 5, 2, 1),      # CMP r5,r2,r1
                insn(0x25, 6, 0, 1),      # SUBI r6,r0,1
                insn(0x32, 7, 6, 1),      # CMPU r7,r6,r1
                halt(),
            ]
        ),
        pc=0x1c,
        regs={3: MASK64, 4: 0, 5: 1, 6: MASK64, 7: 1},
    ),
    MMIXTest(
        "compare-immediate-boundaries",
        b"".join(
            [
                wyde(0xe0, 1, 0xffff),    # SETH r1,0xffff
                wyde(0xe5, 1, 0xffff),    # INCMH r1,0xffff
                wyde(0xe6, 1, 0xffff),    # INCML r1,0xffff
                wyde(0xe7, 1, 0xffff),    # INCL r1,0xffff
                wyde(0xe0, 2, 0x8000),    # SETH r2,0x8000
                wyde(0xe0, 3, 0x7fff),    # SETH r3,0x7fff
                wyde(0xe5, 3, 0xffff),    # INCMH r3,0xffff
                wyde(0xe6, 3, 0xffff),    # INCML r3,0xffff
                wyde(0xe7, 3, 0xffff),    # INCL r3,0xffff
                insn(0x31, 4, 0, 0),      # CMPI r4,r0,0
                insn(0x31, 5, 1, 0),      # CMPI r5,r1,0
                insn(0x31, 6, 3, 0),      # CMPI r6,r3,0
                insn(0x30, 7, 2, 3),      # CMP r7,r2,r3
                insn(0x32, 8, 2, 3),      # CMPU r8,r2,r3
                insn(0x33, 9, 0, 1),      # CMPUI r9,r0,1
                insn(0x33, 10, 1, 0xff),  # CMPUI r10,r1,0xff
                insn(0x33, 11, 0, 0),     # CMPUI r11,r0,0
                halt(),
            ]
        ),
        pc=0x44,
        regs={
            1: MASK64,
            2: 0x8000000000000000,
            3: 0x7fffffffffffffff,
            4: 0,
            5: MASK64,
            6: 1,
            7: MASK64,
            8: 1,
            9: MASK64,
            10: 1,
            11: 0,
        },
    ),
    MMIXTest(
        "branch-taken",
        b"".join(
            [
                insn(0x21, 1, 0, 0),      # ADDI r1,r0,0
                branch(0x42, 1, 2),       # BZ r1,+2
                insn(0x21, 2, 0, 9),      # skipped
                insn(0x21, 2, 0, 5),      # ADDI r2,r0,5
                halt(),
            ]
        ),
        pc=0x10,
        regs={2: 5},
    ),
    MMIXTest(
        "branch-not-taken",
        b"".join(
            [
                insn(0x21, 1, 0, 1),      # ADDI r1,r0,1
                branch(0x42, 1, 2),       # BZ r1,+2
                insn(0x21, 2, 0, 9),      # ADDI r2,r0,9
                halt(),
            ]
        ),
        pc=0x0c,
        regs={2: 9},
    ),
    MMIXTest(
        "branch-backward",
        b"".join(
            [
                insn(0x21, 1, 0, 0),      # ADDI r1,r0,0
                insn(0x21, 2, 0, 3),      # ADDI r2,r0,3
                insn(0x21, 1, 1, 1),      # ADDI r1,r1,1
                insn(0x25, 2, 2, 1),      # SUBI r2,r2,1
                branch(0x4b, 2, 0xfffe),  # BNZB r2,-2
                halt(),
            ]
        ),
        pc=0x14,
        regs={1: 3, 2: 0},
    ),
    MMIXTest(
        "branch-existing-variants",
        b"".join(
            [
                branch(0x42, 0, 3),       # BZ r0,+3
                wyde(0xe3, 2, 1),         # target for BZB
                halt(),
                branch(0x43, 0, 0xfffe),  # BZB r0,-2
            ]
        ),
        pc=0x08,
        regs={2: 1},
    ),
    MMIXTest(
        "branch-bnz-forward",
        b"".join(
            [
                wyde(0xe3, 1, 1),         # SETL r1,1
                branch(0x4a, 1, 2),       # BNZ r1,+2
                wyde(0xe3, 2, 9),         # skipped
                wyde(0xe3, 2, 5),         # SETL r2,5
                halt(),
            ]
        ),
        pc=0x10,
        regs={2: 5},
    ),
    MMIXTest(
        "ordinary-branches-true",
        b"".join(
            [
                insn(0x25, 1, 0, 1),      # SUBI r1,r0,1
                wyde(0xe3, 3, 5),         # SETL r3,5
                wyde(0xe3, 4, 4),         # SETL r4,4
                wyde(0xe3, 5, 0x55),      # SETL r5,0x55
                branch(0x40, 1, 2),       # BN r1,+2
                wyde(0xe3, 10, 0xaa),     # skipped
                wyde(0xe3, 10, 0x55),
                branch(0x42, 0, 2),       # BZ r0,+2
                wyde(0xe3, 11, 0xaa),     # skipped
                wyde(0xe3, 11, 0x55),
                branch(0x44, 3, 2),       # BP r3,+2
                wyde(0xe3, 12, 0xaa),     # skipped
                wyde(0xe3, 12, 0x55),
                branch(0x46, 3, 2),       # BOD r3,+2
                wyde(0xe3, 13, 0xaa),     # skipped
                wyde(0xe3, 13, 0x55),
                branch(0x48, 0, 2),       # BNN r0,+2
                wyde(0xe3, 14, 0xaa),     # skipped
                wyde(0xe3, 14, 0x55),
                branch(0x4a, 3, 2),       # BNZ r3,+2
                wyde(0xe3, 15, 0xaa),     # skipped
                wyde(0xe3, 15, 0x55),
                branch(0x4c, 1, 2),       # BNP r1,+2
                wyde(0xe3, 16, 0xaa),     # skipped
                wyde(0xe3, 16, 0x55),
                branch(0x4e, 4, 2),       # BEV r4,+2
                wyde(0xe3, 17, 0xaa),     # skipped
                wyde(0xe3, 17, 0x55),
                halt(),
            ]
        ),
        pc=0x70,
        regs={reg: 0x55 for reg in range(10, 18)},
    ),
    MMIXTest(
        "ordinary-branches-false",
        b"".join(
            [
                insn(0x25, 1, 0, 1),      # SUBI r1,r0,1
                wyde(0xe3, 3, 5),         # SETL r3,5
                wyde(0xe3, 4, 4),         # SETL r4,4
                wyde(0xe3, 5, 0x55),      # SETL r5,0x55
                branch(0x40, 3, 2),       # BN false
                wyde(0xe3, 10, 0x55),
                branch(0x42, 3, 2),       # BZ false
                wyde(0xe3, 11, 0x55),
                branch(0x44, 1, 2),       # BP false
                wyde(0xe3, 12, 0x55),
                branch(0x46, 4, 2),       # BOD false
                wyde(0xe3, 13, 0x55),
                branch(0x48, 1, 2),       # BNN false
                wyde(0xe3, 14, 0x55),
                branch(0x4a, 0, 2),       # BNZ false
                wyde(0xe3, 15, 0x55),
                branch(0x4c, 3, 2),       # BNP false
                wyde(0xe3, 16, 0x55),
                branch(0x4e, 3, 2),       # BEV false
                wyde(0xe3, 17, 0x55),
                halt(),
            ]
        ),
        pc=0x50,
        regs={reg: 0x55 for reg in range(10, 18)},
    ),
    MMIXTest(
        "probable-branches-true",
        b"".join(
            [
                insn(0x25, 1, 0, 1),      # SUBI r1,r0,1
                wyde(0xe3, 3, 5),         # SETL r3,5
                wyde(0xe3, 4, 4),         # SETL r4,4
                wyde(0xe3, 5, 0x55),      # SETL r5,0x55
                branch(0x50, 1, 2),       # PBN r1,+2
                wyde(0xe3, 10, 0xaa),     # skipped
                wyde(0xe3, 10, 0x55),
                branch(0x52, 0, 2),       # PBZ r0,+2
                wyde(0xe3, 11, 0xaa),     # skipped
                wyde(0xe3, 11, 0x55),
                branch(0x54, 3, 2),       # PBP r3,+2
                wyde(0xe3, 12, 0xaa),     # skipped
                wyde(0xe3, 12, 0x55),
                branch(0x56, 3, 2),       # PBOD r3,+2
                wyde(0xe3, 13, 0xaa),     # skipped
                wyde(0xe3, 13, 0x55),
                branch(0x58, 0, 2),       # PBNN r0,+2
                wyde(0xe3, 14, 0xaa),     # skipped
                wyde(0xe3, 14, 0x55),
                branch(0x5a, 3, 2),       # PBNZ r3,+2
                wyde(0xe3, 15, 0xaa),     # skipped
                wyde(0xe3, 15, 0x55),
                branch(0x5c, 1, 2),       # PBNP r1,+2
                wyde(0xe3, 16, 0xaa),     # skipped
                wyde(0xe3, 16, 0x55),
                branch(0x5e, 4, 2),       # PBEV r4,+2
                wyde(0xe3, 17, 0xaa),     # skipped
                wyde(0xe3, 17, 0x55),
                halt(),
            ]
        ),
        pc=0x70,
        regs={reg: 0x55 for reg in range(10, 18)},
    ),
    MMIXTest(
        "probable-branches-false-backward",
        b"".join(
            [
                wyde(0xe3, 1, 1),         # SETL r1,1
                branch(0x5a, 0, 2),       # PBNZ false
                wyde(0xe3, 2, 7),         # SETL r2,7
                branch(0x52, 0, 3),       # PBZ r0,+3
                wyde(0xe3, 3, 9),         # skipped
                halt(),
                branch(0x5b, 1, 0xffff),  # PBNZB r1,-1
            ]
        ),
        pc=0x14,
        regs={2: 7, 3: 0},
    ),
    MMIXTest(
        "address-geta",
        b"".join(
            [
                branch(0xf4, 1, 2),       # GETA r1,+2
                branch(0xf5, 2, 0xffff),  # GETAB r2,-1
                halt(),
            ]
        ),
        pc=0x08,
        regs={1: 0x08, 2: 0},
    ),
    MMIXTest(
        "jump-forward-backward",
        b"".join(
            [
                jump(0xf0, 3),            # JMP +3
                wyde(0xe3, 1, 9),         # skipped
                halt(),
                wyde(0xe3, 1, 5),         # SETL r1,5
                jump(0xf1, 0xfffffe),     # JMPB -2
            ]
        ),
        pc=0x08,
        regs={1: 5},
    ),
    MMIXTest(
        "go-register-immediate",
        b"".join(
            [
                wyde(0xe3, 1, 17),        # SETL r1,17
                insn(0x9e, 2, 1, 0),      # GO r2,r1,r0
                wyde(0xe3, 3, 9),         # skipped
                halt(),                   # skipped
                wyde(0xe3, 3, 5),         # SETL r3,5
                insn(0x9f, 4, 1, 13),     # GOI r4,r1,13
                wyde(0xe3, 5, 9),         # skipped
                halt(),
            ]
        ),
        pc=0x1c,
        regs={2: 0x08, 3: 5, 4: 0x18, 5: 0},
    ),
    MMIXTest(
        "pushj-pop-single-result",
        b"".join(
            [
                branch(0xf2, 0, 4),       # PUSHJ 0,sub
                halt(),
                insn(0xfd, 0, 0, 0),
                insn(0xfd, 0, 0, 0),
                wyde(0xe3, 0, 42),        # sub: SETL r0,42
                insn(0xf8, 1, 0, 0),      # POP 1,0
            ]
        ),
        pc=0x04,
        regs={0: 42},
    ),
    MMIXTest(
        "pushjb-pop-single-result",
        b"".join(
            [
                jump(0xf0, 3),            # JMP caller
                wyde(0xe3, 0, 9),         # sub: SETL r0,9
                insn(0xf8, 1, 0, 0),      # POP 1,0
                branch(0xf3, 0, 0xfffe),  # caller: PUSHJB 0,sub
                halt(),
            ]
        ),
        pc=0x10,
        regs={0: 9},
    ),
    MMIXTest(
        "pushgo-pop-single-result",
        b"".join(
            [
                wyde(0xe3, 1, 0x10),      # SETL r1,sub
                insn(0xbe, 0, 1, 0),      # PUSHGO 0,r1,r0
                halt(),
                insn(0xfd, 0, 0, 0),
                wyde(0xe3, 0, 33),        # sub: SETL r0,33
                insn(0xf8, 1, 0, 0),      # POP 1,0
            ]
        ),
        pc=0x08,
        regs={0: 33},
    ),
    MMIXTest(
        "pushgoi-pop-single-result",
        b"".join(
            [
                wyde(0xe3, 1, 0x0c),      # SETL r1,sub - 4
                insn(0xbf, 0, 1, 4),      # PUSHGOI 0,r1,4
                halt(),
                insn(0xfd, 0, 0, 0),
                wyde(0xe3, 0, 44),        # sub: SETL r0,44
                insn(0xf8, 1, 0, 0),      # POP 1,0
            ]
        ),
        pc=0x08,
        regs={0: 44},
    ),
    MMIXTest(
        "pop-multiple-results",
        b"".join(
            [
                branch(0xf2, 0, 4),       # PUSHJ 0,sub
                halt(),
                insn(0xfd, 0, 0, 0),
                insn(0xfd, 0, 0, 0),
                wyde(0xe3, 0, 0xaa),      # sub: SETL r0,0xaa
                wyde(0xe3, 1, 0xbb),      # SETL r1,0xbb
                insn(0xf8, 2, 0, 0),      # POP 2,0
            ]
        ),
        pc=0x04,
        regs={0: 0xbb, 1: 0xaa},
    ),
    MMIXTest(
        "nested-pushj-pop",
        b"".join(
            [
                branch(0xf2, 0, 4),       # PUSHJ 0,sub1
                halt(),
                insn(0xfd, 0, 0, 0),
                insn(0xfd, 0, 0, 0),
                insn(0xfe, 40, 0, 4),     # sub1: GET r40,rJ
                branch(0xf2, 0, 4),       # PUSHJ 0,sub2
                insn(0x21, 0, 0, 1),      # ADDI r0,r0,1
                insn(0xf6, 4, 0, 40),     # PUT rJ,r40
                insn(0xf8, 1, 0, 0),      # POP 1,0
                wyde(0xe3, 0, 7),         # sub2: SETL r0,7
                insn(0xf8, 1, 0, 0),      # POP 1,0
            ]
        ),
        pc=0x04,
        regs={0: 8},
    ),
    MMIXTest(
        "register-stack-spill-fill",
        REGISTER_STACK_SPILL_FILL[0],
        pc=REGISTER_STACK_SPILL_FILL[1],
        regs={
            50: INITIAL_STACK,
            51: INITIAL_STACK,
            60: REGISTER_STACK_SPILL_FILL[2],
        },
    ),
    MMIXTest(
        "save-state-after-save",
        SAVE_STATE_AFTER_SAVE[0],
        pc=SAVE_STATE_AFTER_SAVE[1],
        regs={
            32: INITIAL_STACK + 0x778,
            33: 0,
            34: INITIAL_STACK + 0x780,
            35: INITIAL_STACK + 0x780,
            36: INITIAL_STACK + 0x778,
        },
    ),
    MMIXTest(
        "save-unsave-roundtrip",
        SAVE_UNSAVE_ROUNDTRIP[0],
        pc=SAVE_UNSAVE_ROUNDTRIP[1],
        regs={
            50: 0x11,
            51: 0x22,
            52: 0x33,
            53: 0x1234,
            54: 0x5a,
            55: 0x6b,
            56: 0x3ffff,
            57: 3,
            58: INITIAL_STACK,
            59: INITIAL_STACK,
            60: 0x1111222233334444,
            61: 0x5555666677778888,
            62: 0,
            63: 0,
        },
    ),
    MMIXTest(
        "register-stack-save-unsave-spill-fill",
        REGISTER_STACK_SAVE_UNSAVE[0],
        pc=REGISTER_STACK_SAVE_UNSAVE[1],
        regs={
            50: INITIAL_STACK,
            51: INITIAL_STACK,
            60: REGISTER_STACK_SAVE_UNSAVE[2],
        },
    ),
    MMIXTest(
        "load-store",
        b"".join(
            [
                insn(0x21, 1, 0, 0x40),   # ADDI r1,r0,0x40
                insn(0x21, 2, 0, 0x5a),   # ADDI r2,r0,0x5a
                insn(0xac, 2, 1, 0),      # STO r2,r1,r0
                insn(0x8c, 3, 1, 0),      # LDO r3,r1,r0
                halt(),
            ]
        ),
        pc=0x10,
        regs={1: 0x40, 2: 0x5a, 3: 0x5a},
    ),
    MMIXTest(
        "memory-octa-variants",
        b"".join(
            [
                wyde(0xe3, 1, 0x0200),    # SETL r1,0x200
                wyde(0xe3, 2, 8),         # SETL r2,8
                wyde(0xe0, 3, 0x1122),    # SETH r3,0x1122
                wyde(0xe5, 3, 0x3344),    # INCMH r3,0x3344
                wyde(0xe6, 3, 0x5566),    # INCML r3,0x5566
                wyde(0xe7, 3, 0x7788),    # INCL r3,0x7788
                insn(0xac, 3, 1, 2),      # STO r3,r1,r2
                insn(0x8c, 4, 1, 2),      # LDO r4,r1,r2
                insn(0xad, 3, 1, 16),     # STOI r3,r1,16
                insn(0x8d, 5, 1, 16),     # LDOI r5,r1,16
                insn(0xae, 3, 1, 2),      # STOU r3,r1,r2
                insn(0x8e, 6, 1, 2),      # LDOU r6,r1,r2
                insn(0xaf, 3, 1, 24),     # STOUI r3,r1,24
                insn(0x8f, 7, 1, 24),     # LDOUI r7,r1,24
                halt(),
            ]
        ),
        pc=0x38,
        regs={
            3: 0x1122334455667788,
            4: 0x1122334455667788,
            5: 0x1122334455667788,
            6: 0x1122334455667788,
            7: 0x1122334455667788,
        },
    ),
    MMIXTest(
        "memory-load-extension",
        b"".join(
            [
                wyde(0xe3, 1, 0x0220),    # SETL r1,0x220
                wyde(0xe3, 2, 1),         # SETL r2,1
                wyde(0xe3, 3, 2),         # SETL r3,2
                wyde(0xe3, 4, 4),         # SETL r4,4
                wyde(0xe3, 10, 0x80),     # SETL r10,0x80
                insn(0xa3, 10, 1, 1),     # STBUI r10,r1,1
                insn(0x80, 11, 1, 2),     # LDB r11,r1,r2
                insn(0x81, 12, 1, 1),     # LDBI r12,r1,1
                insn(0x82, 13, 1, 2),     # LDBU r13,r1,r2
                insn(0x83, 14, 1, 1),     # LDBUI r14,r1,1
                wyde(0xe3, 15, 0x8001),   # SETL r15,0x8001
                insn(0xa4, 15, 1, 3),     # STW r15,r1,r3
                insn(0x84, 16, 1, 3),     # LDW r16,r1,r3
                insn(0x85, 17, 1, 2),     # LDWI r17,r1,2
                insn(0x86, 18, 1, 3),     # LDWU r18,r1,r3
                insn(0x87, 19, 1, 2),     # LDWUI r19,r1,2
                wyde(0xe2, 20, 0x8000),   # SETML r20,0x8000
                wyde(0xe7, 20, 1),        # INCL r20,1
                insn(0xa9, 20, 1, 4),     # STTI r20,r1,4
                insn(0x88, 21, 1, 4),     # LDT r21,r1,r4
                insn(0x89, 22, 1, 4),     # LDTI r22,r1,4
                insn(0x8a, 23, 1, 4),     # LDTU r23,r1,r4
                insn(0x8b, 24, 1, 4),     # LDTUI r24,r1,4
                halt(),
            ]
        ),
        pc=0x5c,
        regs={
            11: 0xffffffffffffff80,
            12: 0xffffffffffffff80,
            13: 0x80,
            14: 0x80,
            16: 0xffffffffffff8001,
            17: 0xffffffffffff8001,
            18: 0x8001,
            19: 0x8001,
            20: 0x80000001,
            21: 0xffffffff80000001,
            22: 0xffffffff80000001,
            23: 0x80000001,
            24: 0x80000001,
        },
    ),
    MMIXTest(
        "memory-store-widths",
        b"".join(
            [
                wyde(0xe3, 1, 0x0240),    # SETL r1,0x240
                wyde(0xe3, 2, 0x2a),      # SETL r2,0x2a
                insn(0xa0, 2, 1, 0),      # STB r2,r1,r0
                insn(0x82, 20, 1, 0),     # LDBU r20,r1,r0
                wyde(0xe3, 3, 0x3b),      # SETL r3,0x3b
                insn(0xa1, 3, 1, 1),      # STBI r3,r1,1
                insn(0x83, 21, 1, 1),     # LDBUI r21,r1,1
                wyde(0xe3, 4, 0xcc),      # SETL r4,0xcc
                insn(0xa2, 4, 1, 2),      # STBU r4,r1,r2
                insn(0x82, 22, 1, 2),     # LDBU r22,r1,r2
                wyde(0xe3, 5, 0xdd),      # SETL r5,0xdd
                insn(0xa3, 5, 1, 3),      # STBUI r5,r1,3
                insn(0x83, 23, 1, 3),     # LDBUI r23,r1,3
                wyde(0xe3, 6, 0x1234),    # SETL r6,0x1234
                insn(0xa4, 6, 1, 0),      # STW r6,r1,r0
                insn(0x86, 24, 1, 0),     # LDWU r24,r1,r0
                wyde(0xe3, 7, 0x5678),    # SETL r7,0x5678
                insn(0xa5, 7, 1, 6),      # STWI r7,r1,6
                insn(0x87, 25, 1, 6),     # LDWUI r25,r1,6
                wyde(0xe3, 8, 0x9abc),    # SETL r8,0x9abc
                insn(0xa6, 8, 1, 0),      # STWU r8,r1,r0
                insn(0x86, 26, 1, 0),     # LDWU r26,r1,r0
                wyde(0xe3, 9, 0xdef0),    # SETL r9,0xdef0
                insn(0xa7, 9, 1, 10),     # STWUI r9,r1,10
                insn(0x87, 27, 1, 10),    # LDWUI r27,r1,10
                wyde(0xe2, 10, 0x1122),   # SETML r10,0x1122
                wyde(0xe7, 10, 0x3344),   # INCL r10,0x3344
                insn(0xa8, 10, 1, 0),     # STT r10,r1,r0
                insn(0x8a, 28, 1, 0),     # LDTU r28,r1,r0
                wyde(0xe2, 11, 0x5566),   # SETML r11,0x5566
                wyde(0xe7, 11, 0x7788),   # INCL r11,0x7788
                insn(0xa9, 11, 1, 16),    # STTI r11,r1,16
                insn(0x8b, 29, 1, 16),    # LDTUI r29,r1,16
                wyde(0xe2, 12, 0x99aa),   # SETML r12,0x99aa
                wyde(0xe7, 12, 0xbbcc),   # INCL r12,0xbbcc
                insn(0xaa, 12, 1, 0),     # STTU r12,r1,r0
                insn(0x8a, 30, 1, 0),     # LDTU r30,r1,r0
                wyde(0xe2, 13, 0xddee),   # SETML r13,0xddee
                wyde(0xe7, 13, 0xff00),   # INCL r13,0xff00
                insn(0xab, 13, 1, 24),    # STTUI r13,r1,24
                insn(0x8b, 31, 1, 24),    # LDTUI r31,r1,24
                halt(),
            ]
        ),
        pc=0xa4,
        regs={
            20: 0x2a,
            21: 0x3b,
            22: 0xcc,
            23: 0xdd,
            24: 0x1234,
            25: 0x5678,
            26: 0x9abc,
            27: 0xdef0,
            28: 0x11223344,
            29: 0x55667788,
            30: 0x99aabbcc,
            31: 0xddeeff00,
        },
    ),
    MMIXTest(
        "memory-high-tetra",
        b"".join(
            [
                wyde(0xe3, 1, 0x0280),    # SETL r1,0x280
                wyde(0xe0, 2, 0x1234),    # SETH r2,0x1234
                wyde(0xe5, 2, 0x5678),    # INCMH r2,0x5678
                wyde(0xe6, 2, 0x9abc),    # INCML r2,0x9abc
                wyde(0xe7, 2, 0xdef0),    # INCL r2,0xdef0
                insn(0xb2, 2, 1, 0),      # STHT r2,r1,r0
                insn(0x92, 3, 1, 0),      # LDHT r3,r1,r0
                insn(0xb3, 2, 1, 4),      # STHTI r2,r1,4
                insn(0x93, 4, 1, 4),      # LDHTI r4,r1,4
                halt(),
            ]
        ),
        pc=0x24,
        regs={
            2: 0x123456789abcdef0,
            3: 0x1234567800000000,
            4: 0x1234567800000000,
        },
    ),
    MMIXTest(
        "memory-uncached-octa",
        b"".join(
            [
                wyde(0xe3, 1, 0x0300),    # SETL r1,0x300
                *set_octa(2, 0x1122334455667788),
                insn(0xb6, 2, 1, 0),      # STUNC r2,r1,r0
                insn(0x8e, 3, 1, 0),      # LDOU r3,r1,r0
                *set_octa(4, 0x99aabbccddeeff00),
                insn(0xae, 4, 1, 0),      # STOU r4,r1,r0
                insn(0x96, 5, 1, 0),      # LDUNC r5,r1,r0
                insn(0xb7, 2, 1, 8),      # STUNCI r2,r1,8
                insn(0x97, 6, 1, 8),      # LDUNCI r6,r1,8
                halt(),
            ]
        ),
        pc=0x3c,
        regs={
            1: 0x0300,
            2: 0x1122334455667788,
            3: 0x1122334455667788,
            4: 0x99aabbccddeeff00,
            5: 0x99aabbccddeeff00,
            6: 0x1122334455667788,
        },
    ),
    MMIXTest(
        "memory-prefetch-sync-hints",
        b"".join(
            [
                wyde(0xe3, 1, 0x0380),    # SETL r1,0x380
                *set_octa(2, 0x0123456789abcdef),
                insn(0xae, 2, 1, 0),      # STOU r2,r1,r0
                *set_octa(3, 0xfffffffffffffff8),
                insn(0x9a, 15, 3, 3),     # PRELD 15,r3,r3
                insn(0x9b, 16, 3, 0xff),  # PRELDI 16,r3,0xff
                insn(0xba, 17, 3, 3),     # PREST 17,r3,r3
                insn(0xbb, 18, 3, 0xff),  # PRESTI 18,r3,0xff
                insn(0x9c, 19, 3, 3),     # PREGO 19,r3,r3
                insn(0x9d, 20, 3, 0xff),  # PREGOI 20,r3,0xff
                insn(0xb8, 21, 3, 3),     # SYNCD 21,r3,r3
                insn(0xb9, 22, 3, 0xff),  # SYNCDI 22,r3,0xff
                insn(0xbc, 23, 3, 3),     # SYNCID 23,r3,r3
                insn(0xbd, 24, 3, 0xff),  # SYNCIDI 24,r3,0xff
                jump(0xfc, 0),            # SYNC 0
                jump(0xfc, 1),            # SYNC 1
                jump(0xfc, 2),            # SYNC 2
                jump(0xfc, 3),            # SYNC 3
                insn(0x8e, 4, 1, 0),      # LDOU r4,r1,r0
                halt(),
            ]
        ),
        pc=0x64,
        regs={
            1: 0x0380,
            2: 0x0123456789abcdef,
            3: 0xfffffffffffffff8,
            4: 0x0123456789abcdef,
        },
    ),
    MMIXTest(
        "memory-compare-swap",
        b"".join(
            [
                wyde(0xe3, 1, 0x0400),    # SETL r1,0x400
                *set_octa(2, 0x1111222233334444),
                *set_octa(3, 0xaaaabbbbccccdddd),
                insn(0xae, 2, 1, 0),      # STOU r2,r1,r0
                insn(0xf6, 23, 0, 2),     # PUT rP,r2
                insn(0x94, 3, 1, 0),      # CSWAP r3,r1,r0
                insn(0x8e, 4, 1, 0),      # LDOU r4,r1,r0
                insn(0xfe, 5, 0, 23),     # GET r5,rP
                *set_octa(6, 0x5555666677778888),
                *set_octa(7, 0x9999aaaabbbbcccc),
                insn(0xf6, 23, 0, 7),     # PUT rP,r7
                insn(0x94, 6, 1, 0),      # CSWAP r6,r1,r0
                insn(0x8e, 8, 1, 0),      # LDOU r8,r1,r0
                insn(0xfe, 9, 0, 23),     # GET r9,rP
                *set_octa(10, 0x0102030405060708),
                insn(0xaf, 10, 1, 16),    # STOUI r10,r1,16
                *set_octa(11, 0x1020304050607080),
                insn(0xf6, 23, 0, 10),    # PUT rP,r10
                insn(0x95, 11, 1, 16),    # CSWAPI r11,r1,16
                insn(0x8f, 12, 1, 16),    # LDOUI r12,r1,16
                insn(0xfe, 13, 0, 23),    # GET r13,rP
                *set_octa(14, 0x0f0e0d0c0b0a0908),
                insn(0xf6, 23, 0, 14),    # PUT rP,r14
                *set_octa(15, 0x8877665544332211),
                insn(0x95, 15, 1, 16),    # CSWAPI r15,r1,16
                insn(0x8f, 16, 1, 16),    # LDOUI r16,r1,16
                insn(0xfe, 17, 0, 23),    # GET r17,rP
                halt(),
            ]
        ),
        pc=0xcc,
        regs={
            1: 0x0400,
            3: 1,
            4: 0xaaaabbbbccccdddd,
            5: 0x1111222233334444,
            6: 0,
            8: 0xaaaabbbbccccdddd,
            9: 0xaaaabbbbccccdddd,
            11: 1,
            12: 0x1020304050607080,
            13: 0x0102030405060708,
            15: 0,
            16: 0x1020304050607080,
            17: 0x1020304050607080,
        },
    ),
    MMIXTest(
        "special-register-get-reset",
        b"".join(
            [
                insn(0xfe, 33, 0, 15),    # GET r33,rK
                insn(0xfe, 34, 0, 13),    # GET r34,rT
                insn(0xfe, 35, 0, 14),    # GET r35,rTT
                insn(0xfe, 36, 0, 18),    # GET r36,rV
                insn(0xfe, 37, 0, 19),    # GET r37,rG
                insn(0xfe, 38, 0, 20),    # GET r38,rL
                insn(0xfe, 39, 0, 21),    # GET r39,rA
                halt(),
            ]
        ),
        pc=0x1c,
        regs={
            33: 0,
            34: 0x8000000500000000,
            35: 0x8000000600000000,
            36: 0x369c200400000000,
            37: 32,
            38: 0,
            39: 0,
        },
    ),
    MMIXTest(
        "privileged-register-user-trap",
        program_with_handler(
            [
                wyde(0xe3, 1, 0x40),      # SETL r1,handler
                insn(0xf6, 14, 0, 1),     # PUT rTT,r1
                *set_octa(2, RQ_PROGRAM_K),
                insn(0xf6, 15, 0, 2),     # PUT rK,r2
                insn(0xf7, 8, 0, 0xaa),   # PUTI rC,0xaa
                wyde(0xe3, 3, 0xee),      # skipped after dynamic trap
            ],
            0x40,
            [
                insn(0xfe, 40, 0, 8),     # GET r40,rC
                insn(0xfe, 41, 0, 16),    # GET r41,rQ
                insn(0xfe, 42, 0, 29),    # GET r42,rXX
                insn(0xfe, 43, 0, 28),    # GET r43,rWW
                insn(0xfe, 44, 0, 15),    # GET r44,rK
                halt(),
            ],
        ),
        pc=0x54,
        regs={
            3: 0,
            40: 0,
            41: RQ_PROGRAM_K,
            42: RQ_PROGRAM_K,
            43: 0x20,
            44: 0,
        },
    ),
    MMIXTest(
        "special-register-get-all",
        b"".join(
            [
                *[
                    insn(0xfe, 33 + reg, 0, reg)
                    for reg in range(32)
                ],
                halt(),
            ]
        ),
        pc=0x80,
        regs={
            **{33 + reg: 0 for reg in range(32)},
            33 + 10: INITIAL_STACK,
            33 + 11: INITIAL_STACK,
            33 + 13: 0x8000000500000000,
            33 + 14: 0x8000000600000000,
            33 + 15: 0,
            33 + 18: 0x369c200400000000,
            33 + 19: 32,
        },
    ),
    MMIXTest(
        "special-register-put-readback",
        b"".join(
            [
                wyde(0xe0, 1, 0xfeed),    # SETH r1,0xfeed
                wyde(0xe5, 1, 0xcafe),    # INCMH r1,0xcafe
                wyde(0xe6, 1, 0x1234),    # INCML r1,0x1234
                wyde(0xe7, 1, 0x5678),    # INCL r1,0x5678
                insn(0xf6, 4, 0, 1),      # PUT rJ,r1
                insn(0xfe, 2, 0, 4),      # GET r2,rJ
                insn(0xf7, 5, 0, 0x7b),   # PUTI rM,0x7b
                insn(0xfe, 3, 0, 5),      # GET r3,rM
                insn(0xf6, 28, 0, 1),     # PUT rWW,r1
                insn(0xfe, 4, 0, 28),     # GET r4,rWW
                insn(0xf7, 8, 0, 0x11),   # PUTI rC,0x11
                insn(0xf7, 12, 0, 0x12),  # PUTI rI,0x12
                insn(0xf7, 15, 0, 0x13),  # PUTI rK,0x13
                insn(0xf7, 16, 0, 0x14),  # PUTI rQ,0x14
                insn(0xf7, 13, 0, 0x15),  # PUTI rT,0x15
                insn(0xf7, 17, 0, 0x16),  # PUTI rU,0x16
                insn(0xf7, 14, 0, 0x17),  # PUTI rTT,0x17
                insn(0xf7, 23, 0, 0x18),  # PUTI rP,0x18
                insn(0xfe, 40, 0, 8),     # GET r40,rC
                insn(0xfe, 41, 0, 12),    # GET r41,rI
                insn(0xfe, 42, 0, 15),    # GET r42,rK
                insn(0xfe, 43, 0, 16),    # GET r43,rQ
                insn(0xfe, 44, 0, 13),    # GET r44,rT
                insn(0xfe, 45, 0, 17),    # GET r45,rU
                insn(0xfe, 46, 0, 14),    # GET r46,rTT
                insn(0xfe, 47, 0, 23),    # GET r47,rP
                halt(),
            ]
        ),
        pc=0x68,
        regs={
            1: 0xfeedcafe12345678,
            2: 0xfeedcafe12345678,
            3: 0x7b,
            4: 0xfeedcafe12345678,
            40: 0x11,
            41: 0x12,
            42: 0x13,
            43: 0x14,
            44: 0x15,
            45: 0x16,
            46: 0x17,
            47: 0x18,
        },
    ),
    MMIXTest(
        "special-register-ra-mask",
        b"".join(
            [
                *set_octa(1, 0xffffffff0003ffff),
                insn(0xf6, 21, 0, 1),     # PUT rA,r1
                insn(0xfe, 33, 0, 21),    # GET r33,rA
                halt(),
            ]
        ),
        pc=0x18,
        regs={33: 0x3ffff},
    ),
    MMIXTest(
        "special-register-rg-rl-policy",
        b"".join(
            [
                wyde(0xe3, 1, 64),        # SETL r1,64
                insn(0xf6, 19, 0, 1),     # PUT rG,r1
                wyde(0xe3, 40, 0x00aa),   # SETL r40,0xaa
                insn(0xfe, 70, 0, 20),    # GET r70,rL
                wyde(0xe3, 2, 40),        # SETL r2,40
                insn(0xf6, 19, 0, 2),     # PUT rG,r2
                insn(0xfe, 65, 0, 19),    # GET r65,rG
                insn(0xfe, 66, 0, 20),    # GET r66,rL
                halt(),
            ]
        ),
        pc=0x20,
        regs={
            65: 40,
            66: 40,
            70: 41,
        },
    ),
    MMIXTest(
        "local-global-registers",
        b"".join(
            [
                insn(0x21, 2, 1, 5),      # ADDI r2,r1,5
                insn(0xfe, 33, 0, 20),    # GET r33,rL
                wyde(0xe3, 10, 0x00aa),   # SETL r10,0xaa
                insn(0xfe, 34, 0, 20),    # GET r34,rL
                wyde(0xe3, 32, 0x0044),   # SETL r32,0x44
                insn(0xfe, 35, 0, 20),    # GET r35,rL
                halt(),
            ]
        ),
        pc=0x18,
        regs={
            1: 0,
            2: 5,
            9: 0,
            10: 0xaa,
            32: 0x44,
            33: 3,
            34: 11,
            35: 11,
        },
    ),
    MMIXTest(
        "put-rl-narrowing",
        b"".join(
            [
                wyde(0xe3, 10, 0x00aa),   # SETL r10,0xaa
                insn(0xfe, 33, 0, 20),    # GET r33,rL
                wyde(0xe3, 2, 5),         # SETL r2,5
                insn(0xf6, 20, 0, 2),     # PUT rL,r2
                insn(0xfe, 34, 0, 20),    # GET r34,rL
                insn(0x21, 11, 10, 0),    # ADDI r11,r10,0
                insn(0xfe, 35, 0, 20),    # GET r35,rL
                halt(),
            ]
        ),
        pc=0x1c,
        regs={
            2: 5,
            10: 0,
            11: 0,
            33: 11,
            34: 5,
            35: 12,
        },
    ),
    MMIXTest(
        "existing-integer-logical-variants",
        b"".join(
            [
                insn(0x21, 1, 0, 0xf0),   # ADDI r1,r0,0xf0
                insn(0x21, 2, 0, 0x0f),   # ADDI r2,r0,0x0f
                insn(0xfd, 0, 0, 0),      # SWYM
                insn(0x24, 3, 1, 2),      # SUB r3,r1,r2
                insn(0x22, 4, 3, 2),      # ADDU r4,r3,r2
                insn(0x23, 5, 4, 1),      # ADDUI r5,r4,1
                insn(0x26, 6, 5, 2),      # SUBU r6,r5,r2
                insn(0x27, 7, 6, 2),      # SUBUI r7,r6,2
                insn(0xc0, 8, 1, 2),      # OR r8,r1,r2
                insn(0xc6, 9, 1, 2),      # XOR r9,r1,r2
                insn(0xc8, 10, 1, 2),     # AND r10,r1,r2
                halt(),
            ]
        ),
        pc=0x2c,
        regs={
            1: 0xf0,
            2: 0x0f,
            3: 0xe1,
            4: 0xf0,
            5: 0xf1,
            6: 0xe2,
            7: 0xe0,
            8: 0xff,
            9: 0xff,
            10: 0,
        },
    ),
    MMIXTest(
        "wyde-constants",
        b"".join(
            [
                wyde(0xe0, 1, 0x1234),    # SETH r1,0x1234
                wyde(0xe1, 2, 0x5678),    # SETMH r2,0x5678
                wyde(0xe2, 3, 0x9abc),    # SETML r3,0x9abc
                wyde(0xe3, 4, 0xdef0),    # SETL r4,0xdef0
                wyde(0xe0, 5, 0x1111),    # SETH r5,0x1111
                wyde(0xe5, 5, 0x2222),    # INCMH r5,0x2222
                wyde(0xe6, 5, 0x3333),    # INCML r5,0x3333
                wyde(0xe7, 5, 0x4444),    # INCL r5,0x4444
                wyde(0xe3, 6, 0xffff),    # SETL r6,0xffff
                wyde(0xe7, 6, 0x0001),    # INCL r6,1
                wyde(0xe0, 7, 0xffff),    # SETH r7,0xffff
                wyde(0xe4, 7, 0x0001),    # INCH r7,1
                halt(),
            ]
        ),
        pc=0x30,
        regs={
            1: 0x1234000000000000,
            2: 0x0000567800000000,
            3: 0x000000009abc0000,
            4: 0x000000000000def0,
            5: 0x1111222233334444,
            6: 0x0000000000010000,
            7: 0,
        },
    ),
    MMIXTest(
        "scaled-unsigned-add",
        b"".join(
            [
                wyde(0xe3, 1, 7),         # SETL r1,7
                wyde(0xe3, 2, 3),         # SETL r2,3
                insn(0x28, 3, 1, 2),      # 2ADDU r3,r1,r2
                insn(0x29, 4, 1, 5),      # 2ADDUI r4,r1,5
                insn(0x2a, 5, 1, 2),      # 4ADDU r5,r1,r2
                insn(0x2b, 6, 1, 5),      # 4ADDUI r6,r1,5
                insn(0x2c, 7, 1, 2),      # 8ADDU r7,r1,r2
                insn(0x2d, 8, 1, 5),      # 8ADDUI r8,r1,5
                insn(0x2e, 9, 1, 2),      # 16ADDU r9,r1,r2
                insn(0x2f, 10, 1, 5),     # 16ADDUI r10,r1,5
                wyde(0xe0, 11, 0x8000),   # SETH r11,0x8000
                insn(0x29, 12, 11, 0),    # 2ADDUI r12,r11,0
                halt(),
            ]
        ),
        pc=0x30,
        regs={3: 17, 4: 19, 5: 31, 6: 33, 7: 59, 8: 61, 9: 115, 10: 117, 12: 0},
    ),
    MMIXTest(
        "logical-complement",
        b"".join(
            [
                wyde(0xe0, 1, 0xf0f0),    # SETH r1,0xf0f0
                wyde(0xe5, 1, 0xf0f0),    # INCMH r1,0xf0f0
                wyde(0xe6, 1, 0xf0f0),    # INCML r1,0xf0f0
                wyde(0xe7, 1, 0xf0f0),    # INCL r1,0xf0f0
                wyde(0xe0, 2, 0x0ff0),    # SETH r2,0x0ff0
                wyde(0xe5, 2, 0x0ff0),    # INCMH r2,0x0ff0
                wyde(0xe6, 2, 0x0ff0),    # INCML r2,0x0ff0
                wyde(0xe7, 2, 0x0ff0),    # INCL r2,0x0ff0
                insn(0xca, 3, 1, 2),      # ANDN r3,r1,r2
                insn(0xc2, 4, 1, 2),      # ORN r4,r1,r2
                insn(0xc4, 5, 1, 2),      # NOR r5,r1,r2
                insn(0xcc, 6, 1, 2),      # NAND r6,r1,r2
                insn(0xce, 7, 1, 2),      # NXOR r7,r1,r2
                wyde(0xe3, 8, 0x00f0),    # SETL r8,0xf0
                insn(0xcb, 9, 8, 0x0f),   # ANDNI r9,r8,0x0f
                insn(0xc3, 10, 8, 0x0f),  # ORNI r10,r8,0x0f
                insn(0xc5, 11, 8, 0x0f),  # NORI r11,r8,0x0f
                insn(0xcd, 12, 8, 0x0f),  # NANDI r12,r8,0x0f
                insn(0xcf, 13, 8, 0x0f),  # NXORI r13,r8,0x0f
                halt(),
            ]
        ),
        pc=0x4c,
        regs={
            1: 0xf0f0f0f0f0f0f0f0,
            2: 0x0ff00ff00ff00ff0,
            3: 0xf000f000f000f000,
            4: 0xf0fff0fff0fff0ff,
            5: 0x000f000f000f000f,
            6: 0xff0fff0fff0fff0f,
            7: 0x00ff00ff00ff00ff,
            8: 0xf0,
            9: 0xf0,
            10: 0xfffffffffffffff0,
            11: 0xffffffffffffff00,
            12: MASK64,
            13: 0xffffffffffffff00,
        },
    ),
    MMIXTest(
        "wyde-logical-immediates",
        b"".join(
            [
                *set_octa(1, 0x1111222233334444),
                wyde(0xe8, 1, 0x8000),    # ORH r1,0x8000
                wyde(0xe9, 1, 0x0800),    # ORMH r1,0x0800
                wyde(0xea, 1, 0x0080),    # ORML r1,0x0080
                wyde(0xeb, 1, 0x0008),    # ORL r1,0x0008
                *set_octa(2, MASK64),
                wyde(0xec, 2, 0xf0f0),    # ANDNH r2,0xf0f0
                wyde(0xed, 2, 0x0f0f),    # ANDNMH r2,0x0f0f
                wyde(0xee, 2, 0xaaaa),    # ANDNML r2,0xaaaa
                wyde(0xef, 2, 0x5555),    # ANDNL r2,0x5555
                halt(),
            ]
        ),
        pc=0x40,
        regs={1: 0x91112a2233b3444c, 2: 0x0f0ff0f05555aaaa},
    ),
    MMIXTest(
        "unsigned-negate",
        b"".join(
            [
                wyde(0xe3, 1, 5),         # SETL r1,5
                insn(0x36, 2, 10, 1),     # NEGU r2,10,r1
                insn(0x37, 3, 1, 2),      # NEGUI r3,1,2
                insn(0x36, 4, 0, 1),      # NEGU r4,0,r1
                insn(0x37, 5, 0, 0),      # NEGUI r5,0,0
                halt(),
            ]
        ),
        pc=0x14,
        regs={1: 5, 2: 5, 3: MASK64, 4: MASK64 - 4, 5: 0},
    ),
    MMIXTest(
        "low-risk-shifts",
        b"".join(
            [
                wyde(0xe3, 1, 1),         # SETL r1,1
                wyde(0xe3, 2, 64),        # SETL r2,64
                insn(0x37, 3, 0, 8),      # NEGUI r3,0,8
                insn(0x3b, 4, 1, 63),     # SLUI r4,r1,63
                insn(0x3a, 5, 1, 2),      # SLU r5,r1,r2
                insn(0x3d, 6, 3, 1),      # SRI r6,r3,1
                insn(0x3c, 7, 3, 2),      # SR r7,r3,r2
                insn(0x3f, 8, 3, 1),      # SRUI r8,r3,1
                insn(0x3e, 9, 3, 2),      # SRU r9,r3,r2
                insn(0x3f, 10, 1, 0),     # SRUI r10,r1,0
                halt(),
            ]
        ),
        pc=0x28,
        regs={
            1: 1,
            2: 64,
            3: MASK64 - 7,
            4: 0x8000000000000000,
            5: 0,
            6: MASK64 - 3,
            7: MASK64,
            8: 0x7ffffffffffffffc,
            9: 0,
            10: 1,
        },
    ),
    MMIXTest(
        "bit-difference",
        b"".join(
            [
                *set_octa(1, 0x1020304050607080),
                *set_octa(2, 0x0111223344556677),
                insn(0xd0, 3, 1, 2),      # BDIF r3,r1,r2
                insn(0xd1, 4, 1, 0x10),   # BDIFI r4,r1,0x10
                insn(0xd2, 5, 1, 2),      # WDIF r5,r1,r2
                insn(0xd4, 6, 1, 2),      # TDIF r6,r1,r2
                insn(0xd6, 7, 1, 2),      # ODIF r7,r1,r2
                insn(0xd7, 8, 1, 0x80),   # ODIFI r8,r1,0x80
                halt(),
            ]
        ),
        pc=0x38,
        regs={
            3: lane_difference(0x1020304050607080, 0x0111223344556677, 8),
            4: lane_difference(0x1020304050607080, 0x10, 8),
            5: lane_difference(0x1020304050607080, 0x0111223344556677, 16),
            6: lane_difference(0x1020304050607080, 0x0111223344556677, 32),
            7: 0x0f0f0e0d0c0b0a09,
            8: 0x1020304050607000,
        },
    ),
    MMIXTest(
        "sideways-add",
        b"".join(
            [
                *set_octa(1, MASK64),
                *set_octa(2, 0xf0f0f0f0f0f0f0f0),
                insn(0xda, 3, 1, 0),      # SADD r3,r1,r0
                insn(0xda, 4, 1, 2),      # SADD r4,r1,r2
                insn(0xdb, 5, 2, 0xf0),   # SADDI r5,r2,0xf0
                insn(0xdb, 6, 0, 0xff),   # SADDI r6,r0,0xff
                halt(),
            ]
        ),
        pc=0x30,
        regs={
            3: sadd(MASK64, 0),
            4: sadd(MASK64, 0xf0f0f0f0f0f0f0f0),
            5: sadd(0xf0f0f0f0f0f0f0f0, 0xf0),
            6: 0,
        },
    ),
    MMIXTest(
        "bit-matrix",
        b"".join(
            [
                *set_octa(1, 0x1122334455667788),
                *set_octa(2, 0x8040201008040201),
                *set_octa(3, 0x0102040810204080),
                insn(0xdc, 4, 1, 2),      # MOR r4,r1,r2
                insn(0xde, 5, 1, 2),      # MXOR r5,r1,r2
                insn(0xdc, 6, 1, 3),      # MOR r6,r1,r3
                insn(0xde, 7, 1, 3),      # MXOR r7,r1,r3
                insn(0xdd, 8, 1, 0xff),   # MORI r8,r1,0xff
                insn(0xdf, 9, 1, 0xff),   # MXORI r9,r1,0xff
                halt(),
            ]
        ),
        pc=0x48,
        regs={
            4: matrix_multiply(0x1122334455667788, 0x8040201008040201, False),
            5: matrix_multiply(0x1122334455667788, 0x8040201008040201, True),
            6: matrix_multiply(0x1122334455667788, 0x0102040810204080, False),
            7: matrix_multiply(0x1122334455667788, 0x0102040810204080, True),
            8: matrix_multiply(0x1122334455667788, 0xff, False),
            9: matrix_multiply(0x1122334455667788, 0xff, True),
        },
    ),
    MMIXTest(
        "integer-multiply",
        b"".join(
            [
                *set_octa(1, 0xfffffffffffffff0),
                wyde(0xe3, 2, 3),         # SETL r2,3
                insn(0x18, 3, 1, 2),      # MUL r3,r1,r2
                insn(0x19, 4, 1, 5),      # MULI r4,r1,5
                *set_octa(5, MASK64),
                insn(0x1a, 6, 5, 5),      # MULU r6,r5,r5
                insn(0xfe, 7, 0, 3),      # GET r7,rH
                insn(0x1b, 8, 5, 2),      # MULUI r8,r5,2
                insn(0xfe, 9, 0, 3),      # GET r9,rH
                halt(),
            ]
        ),
        pc=0x3c,
        regs={
            3: (-16 * 3) & MASK64,
            4: (-16 * 5) & MASK64,
            6: 1,
            7: MASK64 - 1,
            8: MASK64 - 1,
            9: 1,
        },
    ),
    MMIXTest(
        "integer-multiply-overflow-status",
        b"".join(
            [
                *set_octa(1, 0x7fffffffffffffff),
                wyde(0xe3, 2, 2),         # SETL r2,2
                insn(0x18, 3, 1, 2),      # MUL r3,r1,r2
                insn(0xfe, 4, 0, 21),     # GET r4,rA
                halt(),
            ]
        ),
        pc=0x1c,
        regs={3: MASK64 - 1, 4: RA_EVENT_V},
    ),
    MMIXTest(
        "enabled-integer-multiply-overflow-trip",
        program_with_handler(
            [
                *set_octa(1, 0x7fffffffffffffff),
                wyde(0xe3, 2, 2),                         # SETL r2,2
                wyde(0xe3, 4, RA_EVENT_V << RA_ENABLE_SHIFT),
                insn(0xf6, 21, 0, 4),                     # PUT rA,r4
                insn(0x18, 3, 1, 2),                      # MUL r3,r1,r2
            ],
            32,
            [
                insn(0xfe, 40, 0, 24),                    # GET r40,rW
                insn(0xfe, 41, 0, 25),                    # GET r41,rX
                insn(0xfe, 42, 0, 26),                    # GET r42,rY
                insn(0xfe, 43, 0, 27),                    # GET r43,rZ
                insn(0xfe, 44, 0, 21),                    # GET r44,rA
                halt(),
            ],
        ),
        pc=0x34,
        regs={
            40: 0x20,
            41: 0x8000000018030102,
            42: 0x7fffffffffffffff,
            43: 2,
            44: RA_EVENT_V << RA_ENABLE_SHIFT,
        },
    ),
    MMIXTest(
        "integer-divide",
        b"".join(
            [
                *set_octa(1, (-7) & MASK64),
                wyde(0xe3, 2, 3),         # SETL r2,3
                insn(0x1c, 3, 1, 2),      # DIV r3,r1,r2
                insn(0xfe, 4, 0, 6),      # GET r4,rR
                wyde(0xe3, 5, 7),         # SETL r5,7
                *set_octa(6, (-3) & MASK64),
                insn(0x1c, 7, 5, 6),      # DIV r7,r5,r6
                insn(0xfe, 8, 0, 6),      # GET r8,rR
                insn(0x1d, 9, 1, 3),      # DIVI r9,r1,3
                insn(0xfe, 10, 0, 6),     # GET r10,rR
                insn(0x1c, 11, 5, 0),     # DIV r11,r5,r0
                insn(0xfe, 12, 0, 6),     # GET r12,rR
                insn(0xfe, 13, 0, 21),    # GET r13,rA
                halt(),
            ]
        ),
        pc=0x4c,
        regs={
            3: signed_div((-7) & MASK64, 3)[0],
            4: signed_div((-7) & MASK64, 3)[1],
            7: signed_div(7, (-3) & MASK64)[0],
            8: signed_div(7, (-3) & MASK64)[1],
            9: signed_div((-7) & MASK64, 3)[0],
            10: signed_div((-7) & MASK64, 3)[1],
            11: 0,
            12: 7,
            13: RA_EVENT_D,
        },
    ),
    MMIXTest(
        "integer-divide-overflow-status",
        b"".join(
            [
                *set_octa(1, 0x8000000000000000),
                *set_octa(2, MASK64),
                insn(0x1c, 3, 1, 2),      # DIV r3,r1,r2
                insn(0xfe, 4, 0, 6),      # GET r4,rR
                insn(0xfe, 5, 0, 21),     # GET r5,rA
                halt(),
            ]
        ),
        pc=0x2c,
        regs={3: 0x8000000000000000, 4: 0, 5: RA_EVENT_V},
    ),
    MMIXTest(
        "integer-unsigned-divide",
        b"".join(
            [
                wyde(0xe3, 1, 1),         # SETL r1,1
                insn(0xf6, 1, 0, 1),      # PUT rD,r1
                wyde(0xe3, 3, 2),         # SETL r3,2
                insn(0x1e, 4, 0, 3),      # DIVU r4,r0,r3
                insn(0xfe, 5, 0, 6),      # GET r5,rR
                insn(0xfe, 6, 0, 1),      # GET r6,rD
                wyde(0xe3, 7, 5),         # SETL r7,5
                insn(0xf6, 1, 0, 7),      # PUT rD,r7
                wyde(0xe3, 8, 0x1234),    # SETL r8,0x1234
                insn(0x1e, 9, 8, 7),      # DIVU r9,r8,r7
                insn(0xfe, 10, 0, 6),     # GET r10,rR
                wyde(0xe3, 11, 1),        # SETL r11,1
                insn(0xf6, 1, 0, 11),     # PUT rD,r11
                insn(0x1f, 12, 0, 2),     # DIVUI r12,r0,2
                insn(0xfe, 13, 0, 6),     # GET r13,rR
                halt(),
            ]
        ),
        pc=0x3c,
        regs={
            4: unsigned_div(1, 0, 2)[0],
            5: unsigned_div(1, 0, 2)[1],
            6: 1,
            9: 5,
            10: 0x1234,
            12: unsigned_div(1, 0, 2)[0],
            13: unsigned_div(1, 0, 2)[1],
        },
    ),
    MMIXTest(
        "bit-mux",
        b"".join(
            [
                *set_octa(1, 0xff00ff00ff00ff00),
                insn(0xf6, 5, 0, 1),      # PUT rM,r1
                *set_octa(2, MASK64),
                *set_octa(3, 0x123456789abcdef0),
                insn(0xd8, 4, 2, 3),      # MUX r4,r2,r3
                insn(0xd9, 5, 3, 0xaa),   # MUXI r5,r3,0xaa
                insn(0xf7, 5, 0, 0),      # PUTI rM,0
                insn(0xd8, 6, 2, 3),      # MUX r6,r2,r3
                *set_octa(7, MASK64),
                insn(0xf6, 5, 0, 7),      # PUT rM,r7
                insn(0xd8, 8, 2, 3),      # MUX r8,r2,r3
                insn(0xfe, 9, 0, 5),      # GET r9,rM
                halt(),
            ]
        ),
        pc=0x60,
        regs={
            4: mux(MASK64, 0x123456789abcdef0, 0xff00ff00ff00ff00),
            5: mux(0x123456789abcdef0, 0xaa, 0xff00ff00ff00ff00),
            6: 0x123456789abcdef0,
            8: MASK64,
            9: MASK64,
        },
    ),
    MMIXTest(
        "integer-overflow-status",
        b"".join(
            [
                *set_octa(1, 0x7fffffffffffffff),
                wyde(0xe3, 2, 1),         # SETL r2,1
                insn(0x20, 3, 1, 2),      # ADD r3,r1,r2
                insn(0xfe, 4, 0, 21),     # GET r4,rA
                halt(),
            ]
        ),
        pc=0x1c,
        regs={3: 0x8000000000000000, 4: RA_EVENT_V},
    ),
    MMIXTest(
        "enabled-integer-overflow-trip",
        program_with_handler(
            [
                *set_octa(1, 0x7fffffffffffffff),
                wyde(0xe3, 2, 1),                         # SETL r2,1
                wyde(0xe3, 4, RA_EVENT_V << RA_ENABLE_SHIFT),
                insn(0xf6, 21, 0, 4),                     # PUT rA,r4
                insn(0x20, 3, 1, 2),                      # ADD r3,r1,r2
            ],
            32,
            [
                insn(0xfe, 40, 0, 24),                    # GET r40,rW
                insn(0xfe, 41, 0, 25),                    # GET r41,rX
                insn(0xfe, 42, 0, 26),                    # GET r42,rY
                insn(0xfe, 43, 0, 27),                    # GET r43,rZ
                insn(0xfe, 44, 0, 21),                    # GET r44,rA
                halt(),
            ],
        ),
        pc=0x34,
        regs={
            40: 0x20,
            41: 0x8000000020030102,
            42: 0x7fffffffffffffff,
            43: 1,
            44: RA_EVENT_V << RA_ENABLE_SHIFT,
        },
    ),
    MMIXTest(
        "floating-point-compare",
        b"".join(
            [
                *set_octa(1, f64(1.0)),
                *set_octa(2, f64(2.0)),
                *set_octa(3, 0x8000000000000000),
                *set_octa(5, 0x7ff8000000000001),
                insn(0x01, 10, 1, 2),     # FCMP r10,r1,r2
                insn(0x01, 11, 2, 1),     # FCMP r11,r2,r1
                insn(0x01, 12, 1, 1),     # FCMP r12,r1,r1
                insn(0x03, 13, 3, 0),     # FEQL r13,-0.0,+0.0
                insn(0x02, 14, 1, 5),     # FUN r14,r1,NaN
                insn(0x01, 15, 1, 5),     # FCMP r15,r1,NaN
                insn(0xfe, 16, 0, 21),    # GET r16,rA
                insn(0x11, 17, 1, 2),     # FCMPE r17,r1,r2
                insn(0x13, 18, 1, 1),     # FEQLE r18,r1,r1
                insn(0x12, 19, 1, 5),     # FUNE r19,r1,NaN
                halt(),
            ]
        ),
        pc=0x68,
        regs={
            10: MASK64,
            11: 1,
            12: 0,
            13: 1,
            14: 1,
            15: 0,
            16: 0x10,
            17: MASK64,
            18: 1,
            19: 1,
        },
    ),
    MMIXTest(
        "floating-point-arithmetic",
        b"".join(
            [
                *set_octa(1, f64(1.0)),
                *set_octa(2, f64(2.0)),
                *set_octa(3, f64(3.0)),
                *set_octa(4, f64(4.0)),
                *set_octa(5, f64(5.0)),
                *set_octa(6, f64(1.5)),
                insn(0x04, 10, 1, 2),     # FADD r10,r1,r2
                insn(0x06, 11, 2, 1),     # FSUB r11,r2,r1
                insn(0x10, 12, 2, 3),     # FMUL r12,r2,r3
                insn(0x14, 13, 4, 2),     # FDIV r13,r4,r2
                insn(0x16, 14, 5, 2),     # FREM r14,r5,r2
                insn(0x15, 15, 0, 4),     # FSQRT r15,r4
                insn(0x17, 16, 0, 6),     # FINT r16,r6
                halt(),
            ]
        ),
        pc=0x7c,
        regs={
            10: f64(3.0),
            11: f64(1.0),
            12: f64(6.0),
            13: f64(2.0),
            14: f64(1.0),
            15: f64(2.0),
            16: f64(2.0),
        },
    ),
    MMIXTest(
        "floating-point-conversion",
        b"".join(
            [
                wyde(0xe3, 1, 42),        # SETL r1,42
                insn(0x08, 10, 0, 1),     # FLOT r10,r1
                insn(0x09, 11, 0, 42),    # FLOTI r11,42
                insn(0x0a, 12, 0, 1),     # FLOTU r12,r1
                insn(0x0d, 13, 0, 42),    # SFLOTI r13,42
                *set_octa(2, f64(42.0)),
                insn(0x05, 14, 0, 2),     # FIX r14,r2
                insn(0x07, 15, 0, 2),     # FIXU r15,r2
                halt(),
            ]
        ),
        pc=0x2c,
        regs={
            10: f64(42.0),
            11: f64(42.0),
            12: f64(42.0),
            13: f64(42.0),
            14: 42,
            15: 42,
        },
    ),
    MMIXTest(
        "floating-point-status",
        b"".join(
            [
                *set_octa(1, f64(1.0)),
                *set_octa(3, f64(3.0)),
                insn(0x14, 10, 1, 0),     # FDIV r10,r1,+0.0
                insn(0xfe, 11, 0, 21),    # GET r11,rA
                insn(0x14, 12, 1, 3),     # FDIV r12,r1,r3
                insn(0xfe, 13, 0, 21),    # GET r13,rA
                halt(),
            ]
        ),
        pc=0x30,
        regs={
            10: f64(float("inf")),
            11: 0x02,
            12: f64(1.0 / 3.0),
            13: 0x03,
        },
    ),
    MMIXTest(
        "enabled-floating-divide-trip",
        program_with_handler(
            [
                *set_octa(1, f64(1.0)),
                wyde(0xe3, 2, RA_EVENT_Z << RA_ENABLE_SHIFT),
                insn(0xf6, 21, 0, 2),     # PUT rA,r2
                insn(0x14, 3, 1, 0),      # FDIV r3,r1,+0.0
            ],
            112,
            [
                insn(0xfe, 40, 0, 24),    # GET r40,rW
                insn(0xfe, 41, 0, 25),    # GET r41,rX
                insn(0xfe, 42, 0, 26),    # GET r42,rY
                insn(0xfe, 43, 0, 27),    # GET r43,rZ
                insn(0xfe, 44, 0, 21),    # GET r44,rA
                halt(),
            ],
        ),
        pc=0x84,
        regs={
            40: 0x1c,
            41: 0x8000000014030100,
            42: f64(1.0),
            43: 0,
            44: RA_EVENT_Z << RA_ENABLE_SHIFT,
        },
    ),
    MMIXTest(
        "arithmetic-trip-priority",
        program_with_handler(
            [
                *set_octa(1, 0x7fefffffffffffff),
                *set_octa(2, f64(2.0)),
                wyde(0xe3, 3, (RA_EVENT_O | RA_EVENT_X) << RA_ENABLE_SHIFT),
                insn(0xf6, 21, 0, 3),     # PUT rA,r3
                insn(0x10, 4, 1, 2),      # FMUL r4,r1,r2
            ],
            80,
            [
                insn(0xfe, 40, 0, 24),    # GET r40,rW
                insn(0xfe, 41, 0, 25),    # GET r41,rX
                insn(0xfe, 42, 0, 21),    # GET r42,rA
                halt(),
            ],
        ),
        pc=0x5c,
        regs={
            40: 0x2c,
            41: 0x8000000010040102,
            42: (RA_EVENT_O | RA_EVENT_X) << RA_ENABLE_SHIFT,
        },
    ),
    MMIXTest(
        "explicit-trip-resume",
        b"".join(
            [
                branch(0x42, 10, 12),     # BZ r10,main
                insn(0xfe, 40, 0, 24),    # GET r40,rW
                insn(0xfe, 41, 0, 25),    # GET r41,rX
                insn(0xfe, 42, 0, 26),    # GET r42,rY
                insn(0xfe, 43, 0, 27),    # GET r43,rZ
                insn(0xfe, 44, 0, 0),     # GET r44,rB
                insn(0xf9, 0, 0, 0),      # RESUME 0
                insn(0xfd, 0, 0, 0),      # padding
                insn(0xfd, 0, 0, 0),      # padding
                insn(0xfd, 0, 0, 0),      # padding
                insn(0xfd, 0, 0, 0),      # padding
                insn(0xfd, 0, 0, 0),      # padding
                wyde(0xe3, 10, 1),        # main: SETL r10,1
                wyde(0xe3, 1, 0x00aa),    # SETL r1,0xaa
                wyde(0xe3, 2, 0x00bb),    # SETL r2,0xbb
                insn(0xff, 7, 1, 2),      # TRIP 7,1,2
                wyde(0xe3, 11, 0x55),     # SETL r11,0x55
                halt(),
            ]
        ),
        pc=0x44,
        regs={
            11: 0x55,
            40: 0x40,
            41: 0x80000000ff070102,
            42: 0xaa,
            43: 0xbb,
            44: 0,
        },
    ),
    MMIXTest(
        "explicit-trap-state",
        program_with_handler(
            [
                wyde(0xe3, 1, 0x40),      # SETL r1,handler
                insn(0xf6, 13, 0, 1),     # PUT rT,r1
                wyde(0xe3, 2, 0x00aa),    # SETL r2,0xaa
                wyde(0xe3, 3, 0x00bb),    # SETL r3,0xbb
                wyde(0xe3, 4, 0x00dd),    # SETL r4,0xdd
                insn(0xf6, 4, 0, 4),      # PUT rJ,r4
                wyde(0xe3, 255, 0x00cc),  # SETL r255,0xcc
                insn(0x00, 1, 2, 3),      # TRAP 1,2,3
            ],
            0x40,
            [
                insn(0xfe, 40, 0, 28),    # GET r40,rWW
                insn(0xfe, 41, 0, 29),    # GET r41,rXX
                insn(0xfe, 42, 0, 30),    # GET r42,rYY
                insn(0xfe, 43, 0, 31),    # GET r43,rZZ
                insn(0xfe, 44, 0, 7),     # GET r44,rBB
                insn(0xfe, 45, 0, 15),    # GET r45,rK
                insn(0x21, 46, 255, 0),   # ADDI r46,r255,0
                halt(),
            ],
        ),
        pc=0x5c,
        regs={
            40: 0x20,
            41: 0x8000000000010203,
            42: 0xaa,
            43: 0xbb,
            44: 0xcc,
            45: 0,
            46: 0xdd,
        },
    ),
    MMIXTest(
        "explicit-trap-resume",
        program_with_handler(
            [
                wyde(0xe3, 1, 0x40),      # SETL r1,handler
                insn(0xf6, 13, 0, 1),     # PUT rT,r1
                wyde(0xe3, 4, 0x00dd),    # SETL r4,0xdd
                insn(0xf6, 4, 0, 4),      # PUT rJ,r4
                wyde(0xe3, 255, 0x00cc),  # SETL r255,0xcc
                insn(0x00, 1, 0, 0),      # TRAP 1,0,0
                wyde(0xe3, 10, 0x55),     # SETL r10,0x55
                insn(0xfe, 11, 0, 15),    # GET r11,rK
                insn(0x21, 12, 255, 0),   # ADDI r12,r255,0
                halt(),
            ],
            0x40,
            [
                wyde(0xe3, 255, 0x0123),  # SETL r255,0x123
                insn(0xf9, 0, 0, 1),      # RESUME 1
            ],
        ),
        pc=0x24,
        regs={10: 0x55, 11: 0x123, 12: 0xcc},
    ),
    MMIXTest(
        "arithmetic-trip-resume",
        program_with_handler(
            [
                *set_octa(1, f64(1.0)),
                wyde(0xe3, 2, RA_EVENT_Z << RA_ENABLE_SHIFT),
                insn(0xf6, 21, 0, 2),     # PUT rA,r2
                insn(0x14, 3, 1, 0),      # FDIV r3,r1,+0.0
                wyde(0xe3, 10, 0x55),     # SETL r10,0x55
                insn(0xfe, 11, 0, 21),    # GET r11,rA
                halt(),
            ],
            112,
            [
                insn(0xf9, 0, 0, 0),      # RESUME 0
            ],
        ),
        pc=0x24,
        regs={3: 0, 10: 0x55, 11: RA_EVENT_Z << RA_ENABLE_SHIFT},
    ),
    MMIXTest(
        "resume-ropcode-result",
        program_with_handler(
            [
                wyde(0xe3, 1, 0x40),      # SETL r1,target
                insn(0xf6, 24, 0, 1),     # PUT rW,r1
                *set_octa(2, 0x0200000021050007),
                insn(0xf6, 25, 0, 2),     # PUT rX,r2
                wyde(0xe3, 3, 0x77),      # SETL r3,0x77
                insn(0xf6, 27, 0, 3),     # PUT rZ,r3
                insn(0xf9, 0, 0, 0),      # RESUME 0
            ],
            0x40,
            [
                halt(),
            ],
        ),
        pc=0x40,
        regs={5: 0x77},
    ),
    MMIXTest(
        "floating-point-exceptions",
        b"".join(
            [
                *set_octa(1, 0x7ff8000000001234),
                *set_octa(2, 0x7ff0000000001234),
                *set_octa(3, f64(1.0)),
                *set_octa(4, 0x7fefffffffffffff),
                *set_octa(5, f64(2.0)),
                *set_octa(6, 0x0010000000000000),
                *set_octa(7, 0x8000000000000000),
                insn(0x04, 10, 1, 3),     # FADD r10,qNaN,1.0
                insn(0x04, 11, 2, 3),     # FADD r11,sNaN,1.0
                insn(0xfe, 12, 0, 21),    # GET r12,rA
                insn(0x10, 13, 4, 5),     # FMUL r13,max,2.0
                insn(0xfe, 14, 0, 21),    # GET r14,rA
                insn(0x10, 15, 6, 6),     # FMUL r15,min-normal,min-normal
                insn(0xfe, 16, 0, 21),    # GET r16,rA
                insn(0x05, 17, 0, 1),     # FIX r17,qNaN
                insn(0x04, 18, 7, 7),     # FADD r18,-0.0,-0.0
                halt(),
            ]
        ),
        pc=0x94,
        regs={
            10: 0x7ff8000000001234,
            11: 0x7ff8000000001234,
            12: 0x10,
            13: f64(float("inf")),
            14: 0x19,
            15: 0,
            16: 0x1d,
            17: 0x7ff8000000001234,
            18: 0x8000000000000000,
        },
    ),
    MMIXTest(
        "floating-point-rounding",
        b"".join(
            [
                *set_octa(1, 0xffffffff00030000),
                insn(0xf6, 21, 0, 1),     # PUT rA,r1
                insn(0xfe, 2, 0, 21),     # GET r2,rA
                *set_octa(5, f64(1.5)),
                insn(0x17, 6, 0, 5),      # FINT r6,r5
                insn(0x17, 7, 4, 5),      # FINT r7,ROUND_NEAR,r5
                halt(),
            ]
        ),
        pc=0x30,
        regs={2: 0x30000, 6: f64(1.0), 7: f64(2.0)},
    ),
    MMIXTest(
        "short-float-memory",
        b"".join(
            [
                wyde(0xe3, 1, 0x0300),    # SETL r1,0x300
                wyde(0xe2, 2, f32(1.5) >> 16),
                insn(0xaa, 2, 1, 0),      # STTU r2,r1,r0
                insn(0x90, 3, 1, 0),      # LDSF r3,r1,r0
                *set_octa(4, f64(2.0)),
                insn(0xb1, 4, 1, 4),      # STSFI r4,r1,4
                insn(0x8b, 5, 1, 4),      # LDTUI r5,r1,4
                *set_octa(6, f64(1.0 / 3.0)),
                insn(0xb1, 6, 1, 8),      # STSFI r6,r1,8
                insn(0xfe, 7, 0, 21),     # GET r7,rA
                halt(),
            ]
        ),
        pc=0x40,
        regs={3: f64(1.5), 5: f32(2.0), 7: 0x01},
    ),
    MMIXTest(
        "conditional-set",
        b"".join(
            [
                wyde(0xe0, 1, 0xffff),    # SETH r1,0xffff
                wyde(0xe5, 1, 0xffff),    # INCMH r1,0xffff
                wyde(0xe6, 1, 0xffff),    # INCML r1,0xffff
                wyde(0xe7, 1, 0xffff),    # INCL r1,0xffff
                wyde(0xe3, 3, 5),         # SETL r3,5
                wyde(0xe3, 4, 4),         # SETL r4,4
                wyde(0xe3, 5, 0x55),      # SETL r5,0x55
                wyde(0xe3, 30, 0xaaaa),   # SETL r30,0xaaaa
                insn(0x60, 10, 1, 5),     # CSN r10,r1,r5
                insn(0x62, 11, 0, 5),     # CSZ r11,r0,r5
                insn(0x64, 12, 3, 5),     # CSP r12,r3,r5
                insn(0x66, 13, 3, 5),     # CSOD r13,r3,r5
                insn(0x68, 14, 0, 5),     # CSNN r14,r0,r5
                insn(0x6a, 15, 3, 5),     # CSNZ r15,r3,r5
                insn(0x6c, 16, 1, 5),     # CSNP r16,r1,r5
                insn(0x6e, 17, 4, 5),     # CSEV r17,r4,r5
                wyde(0xe3, 18, 0xaaaa),   # SETL r18,0xaaaa
                insn(0x60, 18, 3, 5),     # CSN false preserves r18
                wyde(0xe3, 19, 0xaaaa),   # SETL r19,0xaaaa
                insn(0x62, 19, 3, 5),     # CSZ false preserves r19
                wyde(0xe3, 20, 0xaaaa),   # SETL r20,0xaaaa
                insn(0x64, 20, 1, 5),     # CSP false preserves r20
                wyde(0xe3, 21, 0xaaaa),   # SETL r21,0xaaaa
                insn(0x66, 21, 4, 5),     # CSOD false preserves r21
                wyde(0xe3, 22, 0xaaaa),   # SETL r22,0xaaaa
                insn(0x68, 22, 1, 5),     # CSNN false preserves r22
                wyde(0xe3, 23, 0xaaaa),   # SETL r23,0xaaaa
                insn(0x6a, 23, 0, 5),     # CSNZ false preserves r23
                wyde(0xe3, 24, 0xaaaa),   # SETL r24,0xaaaa
                insn(0x6c, 24, 3, 5),     # CSNP false preserves r24
                wyde(0xe3, 25, 0xaaaa),   # SETL r25,0xaaaa
                insn(0x6e, 25, 3, 5),     # CSEV false preserves r25
                wyde(0xe3, 26, 0xaaaa),   # SETL r26,0xaaaa
                insn(0x63, 26, 0, 0x77),  # CSZI r26,r0,0x77
                wyde(0xe3, 27, 0xaaaa),   # SETL r27,0xaaaa
                insn(0x6b, 27, 0, 0x77),  # CSNZI false preserves r27
                halt(),
            ]
        ),
        pc=0x90,
        regs={
            10: 0x55,
            11: 0x55,
            12: 0x55,
            13: 0x55,
            14: 0x55,
            15: 0x55,
            16: 0x55,
            17: 0x55,
            18: 0xaaaa,
            19: 0xaaaa,
            20: 0xaaaa,
            21: 0xaaaa,
            22: 0xaaaa,
            23: 0xaaaa,
            24: 0xaaaa,
            25: 0xaaaa,
            26: 0x77,
            27: 0xaaaa,
        },
    ),
    MMIXTest(
        "zero-or-set",
        b"".join(
            [
                wyde(0xe0, 1, 0xffff),    # SETH r1,0xffff
                wyde(0xe5, 1, 0xffff),    # INCMH r1,0xffff
                wyde(0xe6, 1, 0xffff),    # INCML r1,0xffff
                wyde(0xe7, 1, 0xffff),    # INCL r1,0xffff
                wyde(0xe3, 3, 5),         # SETL r3,5
                wyde(0xe3, 4, 4),         # SETL r4,4
                wyde(0xe3, 5, 0x55),      # SETL r5,0x55
                insn(0x70, 10, 1, 5),     # ZSN r10,r1,r5
                insn(0x72, 11, 0, 5),     # ZSZ r11,r0,r5
                insn(0x74, 12, 3, 5),     # ZSP r12,r3,r5
                insn(0x76, 13, 3, 5),     # ZSOD r13,r3,r5
                insn(0x78, 14, 0, 5),     # ZSNN r14,r0,r5
                insn(0x7a, 15, 3, 5),     # ZSNZ r15,r3,r5
                insn(0x7c, 16, 1, 5),     # ZSNP r16,r1,r5
                insn(0x7e, 17, 4, 5),     # ZSEV r17,r4,r5
                insn(0x70, 18, 3, 5),     # ZSN false writes zero
                insn(0x72, 19, 3, 5),     # ZSZ false writes zero
                insn(0x74, 20, 1, 5),     # ZSP false writes zero
                insn(0x76, 21, 4, 5),     # ZSOD false writes zero
                insn(0x78, 22, 1, 5),     # ZSNN false writes zero
                insn(0x7a, 23, 0, 5),     # ZSNZ false writes zero
                insn(0x7c, 24, 3, 5),     # ZSNP false writes zero
                insn(0x7e, 25, 3, 5),     # ZSEV false writes zero
                insn(0x73, 26, 0, 0x77),  # ZSZI r26,r0,0x77
                insn(0x7b, 27, 0, 0x77),  # ZSNZI false writes zero
                halt(),
            ]
        ),
        pc=0x64,
        regs={
            10: 0x55,
            11: 0x55,
            12: 0x55,
            13: 0x55,
            14: 0x55,
            15: 0x55,
            16: 0x55,
            17: 0x55,
            18: 0,
            19: 0,
            20: 0,
            21: 0,
            22: 0,
            23: 0,
            24: 0,
            25: 0,
            26: 0x77,
            27: 0,
        },
    ),
]


EXPECTED_FAILURES = [
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
        "unknown-opcode",
        insn(0x34, 0, 0, 0),             # NEG is still unknown
        ("MMIX unknown opcode 0x34", "MMIX illegal instruction"),
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
        "privileged-sync",
        jump(0xfc, 4),                   # SYNC 4
        ("MMIX privileged SYNC 4", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "invalid-sync",
        jump(0xfc, 8),                   # SYNC 8
        ("MMIX invalid SYNC 8", "MMIX illegal instruction"),
    ),
]


def parse_log(log_text):
    if "MMIX test exit" not in log_text:
        raise AssertionError("missing MMIX test exit line")

    pc_match = re.search(r"pc=0x([0-9a-fA-F]+)\s+npc=0x([0-9a-fA-F]+)", log_text)
    if pc_match is None:
        raise AssertionError("missing pc/npc line")

    regs = {}
    for reg, value in re.findall(r"\br(\d+)\s*=0x([0-9a-fA-F]+)", log_text):
        regs[int(reg)] = int(value, 16)

    return int(pc_match.group(1), 16), int(pc_match.group(2), 16), regs


def run_one(qemu, workdir, test):
    image = workdir / f"{test.name}.bin"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.program)
    if log.exists():
        log.unlink()

    cmd = [
        str(qemu),
        "-machine",
        "virt",
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "none",
        "-kernel",
        str(image),
        "-d",
        "int",
        "-D",
        str(log),
    ]
    subprocess.run(cmd, check=True, timeout=10)

    pc, npc, regs = parse_log(log.read_text(encoding="utf-8"))
    if pc != test.pc:
        raise AssertionError(f"{test.name}: pc expected 0x{test.pc:x}, got 0x{pc:x}")
    if npc != test.pc + 4:
        raise AssertionError(
            f"{test.name}: npc expected 0x{test.pc + 4:x}, got 0x{npc:x}"
        )

    for reg, expected in test.regs.items():
        actual = regs.get(reg)
        if actual is None:
            raise AssertionError(f"{test.name}: missing r{reg} in log")
        expected &= MASK64
        if actual != expected:
            raise AssertionError(
                f"{test.name}: r{reg} expected 0x{expected:016x}, "
                f"got 0x{actual:016x}"
            )


def run_expected_failure(qemu, workdir, test):
    image = workdir / f"{test.name}.bin"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.program)
    if log.exists():
        log.unlink()

    cmd = [
        str(qemu),
        "-machine",
        "virt",
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "none",
        "-kernel",
        str(image),
        "-d",
        "unimp,int",
        "-D",
        str(log),
    ]
    try:
        subprocess.run(cmd, check=False, timeout=2)
    except subprocess.TimeoutExpired:
        pass

    if not log.exists():
        raise AssertionError(f"{test.name}: missing log")

    log_text = log.read_text(encoding="utf-8")
    for pattern in test.patterns:
        if pattern not in log_text:
            raise AssertionError(f"{test.name}: missing expected log pattern {pattern!r}")
    for pattern in test.absent:
        if pattern in log_text:
            raise AssertionError(f"{test.name}: unexpected log pattern {pattern!r}")


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True, type=pathlib.Path)
    parser.add_argument("--workdir", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)

    args.workdir.mkdir(parents=True, exist_ok=True)
    for test in TESTS:
        run_one(args.qemu, args.workdir, test)
        print(f"PASS {test.name}")
    for test in EXPECTED_FAILURES:
        run_expected_failure(args.qemu, args.workdir, test)
        print(f"PASS {test.name}")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (AssertionError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as err:
        print(f"FAIL {err}", file=sys.stderr)
        sys.exit(1)
