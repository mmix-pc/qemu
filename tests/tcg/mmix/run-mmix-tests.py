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


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True, type=pathlib.Path)
    parser.add_argument("--workdir", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)

    args.workdir.mkdir(parents=True, exist_ok=True)
    for test in TESTS:
        run_one(args.qemu, args.workdir, test)
        print(f"PASS {test.name}")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (AssertionError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as err:
        print(f"FAIL {err}", file=sys.stderr)
        sys.exit(1)
