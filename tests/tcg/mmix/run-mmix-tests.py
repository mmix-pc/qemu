#!/usr/bin/env python3
#
# MMIX raw-image smoke tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import dataclasses
import pathlib
import re
import subprocess
import sys

MASK64 = (1 << 64) - 1


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
        "special-register-get-reset",
        b"".join(
            [
                insn(0xfe, 33, 0, 15),    # GET r33,rK
                insn(0xfe, 34, 0, 13),    # GET r34,rT
                insn(0xfe, 35, 0, 14),    # GET r35,rTT
                insn(0xfe, 36, 0, 18),    # GET r36,rV
                insn(0xfe, 37, 0, 19),    # GET r37,rG
                insn(0xfe, 38, 0, 20),    # GET r38,rL
                halt(),
            ]
        ),
        pc=0x18,
        regs={
            33: MASK64,
            34: 0x8000000500000000,
            35: 0x8000000600000000,
            36: 0x369c200400000000,
            37: 32,
            38: 0,
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
                halt(),
            ]
        ),
        pc=0x28,
        regs={
            1: 0xfeedcafe12345678,
            2: 0xfeedcafe12345678,
            3: 0x7b,
            4: 0xfeedcafe12345678,
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
        "unsupported-trap",
        insn(0x00, 1, 0, 0),             # TRAP 1,0,0
        ("MMIX decoded unimplemented TRAP", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "unsupported-trip",
        insn(0xff, 0, 0, 0),             # TRIP 0,0,0
        ("MMIX decoded unimplemented TRIP", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "unsupported-resume",
        insn(0xf9, 0, 0, 0),             # RESUME 0
        ("MMIX decoded unimplemented RESUME", "MMIX illegal instruction"),
    ),
    MMIXExpectedFailure(
        "unknown-opcode",
        insn(0xf8, 0, 0, 0),             # POP is still unknown
        ("MMIX unknown opcode 0xf8", "MMIX illegal instruction"),
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
