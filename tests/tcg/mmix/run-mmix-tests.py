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
