#!/usr/bin/env python3
#
# Minimal MMIX virt firmware and next-stage fixture generator
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import pathlib
import struct


BZ = 0x42
BNZ = 0x4a
CMPU = 0x32
CMPUI = 0x33
ANDI = 0xc9
SLUI = 0x3b
LDBU = 0x82
LDBUI = 0x83
LDTUI = 0x8b
LDOUI = 0x8f
STBUI = 0xa3
STWUI = 0xa7
STOUI = 0xaf
OR = 0xc0
ADDU = 0x22
ADDUI = 0x23
SUBUI = 0x27
SETH = 0xe0
SETL = 0xe3
INCMH = 0xe5
INCML = 0xe6
INCL = 0xe7
GOI = 0x9f
GET = 0xfe
SYNC = 0xfc

SR_L = 20
SR_O = 10

FIRMWARE_ALIAS = 0x8001000000000000
FW_CFG_ALIAS = 0x8001000014000000
UART_ALIAS = 0x8001000010000000
UART_LSR = 5
UART_LSR_THRE = 0x20
FDT_ADDRESS = 0x00100000
KERNEL_ADDRESS = 0x00200000
RECORD_ADDRESS = 0x00300000
RELEASE_ADDRESS = 0x00008000
SUCCESS_ADDRESS = 0x00300800
SUCCESS_VALUE = 0x4d4d495846574f4b
FAILURE_VALUE = 0x4d4d495846574241
FDT_MAGIC = 0xd00dfeed
FW_CFG_FILE_DIR = 0x19


def insn(op, x=0, y=0, z=0):
    return struct.pack(">BBBB", op, x, y, z)


def wyde(op, x, yz):
    return struct.pack(">BBH", op, x, yz)


def set_octa(reg, value):
    return (
        wyde(SETH, reg, (value >> 48) & 0xffff),
        wyde(INCMH, reg, (value >> 32) & 0xffff),
        wyde(INCML, reg, (value >> 16) & 0xffff),
        wyde(INCL, reg, value & 0xffff),
    )


class Program:
    def __init__(self):
        self.items = []
        self.labels = {}
        self.branches = []
        self.addresses = []

    def emit(self, *items):
        self.items.extend(items)

    def mark(self, label):
        if label in self.labels:
            raise ValueError(f"duplicate label {label}")
        self.labels[label] = len(self.items)

    def branch(self, op, reg, label):
        self.branches.append((len(self.items), op, reg, label))
        self.items.append(None)

    def address(self, reg, label, base=0):
        self.addresses.append((len(self.items), reg, label, base))
        self.items.extend((None,) * 4)

    def data(self, value):
        if len(value) % 4:
            raise ValueError("program data must be tetra aligned")
        self.items.extend(value[index:index + 4]
                          for index in range(0, len(value), 4))

    def build(self):
        for index, op, reg, label in self.branches:
            displacement = self.labels[label] - index
            if not -(1 << 15) <= displacement < (1 << 15):
                raise ValueError(f"branch to {label} is out of range")
            branch_op = op if displacement >= 0 else op | 1
            self.items[index] = wyde(branch_op, reg,
                                     displacement & 0xffff)
        for index, reg, label, base in self.addresses:
            value = base + self.labels[label] * 4
            self.items[index:index + 4] = set_octa(reg, value)
        return b"".join(self.items)


def emit_read_be(program, destination, byte_count, *, byte=43):
    program.emit(wyde(SETL, destination, 0))
    for _ in range(byte_count):
        program.emit(
            insn(LDBUI, byte, 33, 0),
            insn(SLUI, destination, destination, 8),
            insn(OR, destination, destination, byte),
        )


def firmware_image():
    p = Program()
    zero = 250

    p.emit(insn(ADDU, 32, 0, zero))
    p.branch(BNZ, 32, "secondary")
    p.emit(*set_octa(33, FW_CFG_ALIAS))
    p.emit(*set_octa(34, RELEASE_ADDRESS))
    p.emit(*set_octa(35, FDT_ADDRESS))
    p.emit(*set_octa(36, KERNEL_ADDRESS))
    p.address(48, "fdt_name", FIRMWARE_ALIAS)
    p.address(49, "kernel_name", FIRMWARE_ALIAS)
    p.emit(wyde(SETL, 43, FW_CFG_FILE_DIR),
           insn(STWUI, 43, 33, 8))
    emit_read_be(p, 41, 4)
    p.emit(wyde(SETL, 37, 0), wyde(SETL, 39, 0))

    p.mark("directory_entry")
    p.branch(BZ, 41, "directory_done")
    emit_read_be(p, 44, 4)
    emit_read_be(p, 45, 2)
    p.emit(insn(LDBUI, 43, 33, 0), insn(LDBUI, 43, 33, 0),
           wyde(SETL, 46, 0), wyde(SETL, 47, 0),
           wyde(SETL, 52, 0))
    p.mark("name_byte")
    p.emit(
        insn(LDBUI, 43, 33, 0),
        insn(LDBU, 50, 48, 52),
        insn(CMPU, 51, 43, 50),
        insn(OR, 46, 46, 51),
        insn(LDBU, 50, 49, 52),
        insn(CMPU, 51, 43, 50),
        insn(OR, 47, 47, 51),
        insn(ADDUI, 52, 52, 1),
        insn(CMPUI, 51, 52, 56),
    )
    p.branch(BNZ, 51, "name_byte")
    p.branch(BNZ, 46, "not_fdt")
    p.emit(insn(ADDU, 37, 45, zero), insn(ADDU, 38, 44, zero))
    p.mark("not_fdt")
    p.branch(BNZ, 47, "not_kernel")
    p.emit(insn(ADDU, 39, 45, zero), insn(ADDU, 40, 44, zero))
    p.mark("not_kernel")
    p.emit(insn(SUBUI, 41, 41, 1))
    p.branch(BZ, zero, "directory_entry")

    p.mark("directory_done")
    p.branch(BZ, 37, "failure")
    p.branch(BZ, 39, "no_kernel")
    p.emit(insn(STWUI, 37, 33, 8), insn(ADDU, 53, 35, zero),
           insn(ADDU, 52, 38, zero))
    p.mark("copy_fdt")
    p.branch(BZ, 52, "check_fdt")
    p.emit(insn(LDBUI, 43, 33, 0), insn(STBUI, 43, 53, 0),
           insn(ADDUI, 53, 53, 1), insn(SUBUI, 52, 52, 1))
    p.branch(BZ, zero, "copy_fdt")
    p.mark("check_fdt")
    p.emit(insn(LDTUI, 43, 35, 0), *set_octa(54, FDT_MAGIC),
           insn(CMPU, 51, 43, 54))
    p.branch(BNZ, 51, "failure")

    p.emit(insn(STWUI, 39, 33, 8), insn(ADDU, 53, 36, zero),
           insn(ADDU, 52, 40, zero))
    p.mark("copy_kernel")
    p.branch(BZ, 52, "release")
    p.emit(insn(LDBUI, 43, 33, 0), insn(STBUI, 43, 53, 0),
           insn(ADDUI, 53, 53, 1), insn(SUBUI, 52, 52, 1))
    p.branch(BZ, zero, "copy_kernel")
    p.mark("release")
    p.emit(insn(SYNC, 0, 0, 1), insn(STOUI, 35, 34, 0),
           insn(SYNC, 0, 0, 1))
    p.branch(BZ, zero, "handoff")

    p.mark("secondary")
    p.emit(*set_octa(34, RELEASE_ADDRESS), *set_octa(36, KERNEL_ADDRESS))
    p.mark("secondary_wait")
    p.emit(insn(LDOUI, 35, 34, 0))
    p.branch(BZ, 35, "secondary_wait")
    p.emit(insn(SYNC, 0, 0, 1))

    p.mark("handoff")
    p.emit(insn(ADDUI, 1, 35, 0), insn(GOI, 249, 36, 0))
    p.mark("no_kernel")
    p.branch(BZ, zero, "no_kernel")
    p.mark("failure")
    p.emit(*set_octa(43, FAILURE_VALUE), *set_octa(44, SUCCESS_ADDRESS),
           insn(STOUI, 43, 44, 0))
    p.branch(BZ, zero, "failure")

    p.mark("fdt_name")
    p.data(b"etc/fdt\0".ljust(56, b"\0"))
    p.mark("kernel_name")
    p.data(b"opt/mmix/kernel\0".ljust(56, b"\0"))
    return p.build()


def kernel_image():
    p = Program()
    zero = 250
    p.emit(
        *set_octa(32, RECORD_ADDRESS),
        insn(SLUI, 33, 0, 5),
        insn(ADDU, 32, 32, 33),
        insn(STOUI, 0, 32, 0),
        insn(STOUI, 1, 32, 8),
        insn(GET, 34, 0, SR_L),
        insn(STOUI, 34, 32, 16),
        insn(GET, 35, 0, SR_O),
        insn(STOUI, 35, 32, 24),
        insn(LDTUI, 36, 1, 0),
        *set_octa(37, FDT_MAGIC),
        insn(CMPU, 38, 36, 37),
    )
    p.branch(BNZ, 38, "failure")
    p.emit(insn(CMPUI, 38, 34, 2))
    p.branch(BNZ, 38, "failure")
    p.branch(BNZ, 0, "idle")
    p.emit(*set_octa(39, UART_ALIAS))
    for index, byte in enumerate(b"MMIX firmware handoff\n"):
        p.mark(f"uart_{index}")
        p.emit(insn(LDBUI, 40, 39, UART_LSR),
               insn(ANDI, 40, 40, UART_LSR_THRE))
        p.branch(BZ, 40, f"uart_{index}")
        p.emit(wyde(SETL, 40, byte), insn(STBUI, 40, 39, 0))
    p.emit(*set_octa(41, SUCCESS_VALUE), *set_octa(42, SUCCESS_ADDRESS),
           insn(STOUI, 41, 42, 0), insn(SYNC, 0, 0, 1))
    p.mark("idle")
    p.branch(BZ, zero, "idle")
    p.mark("failure")
    p.emit(*set_octa(41, FAILURE_VALUE), *set_octa(42, SUCCESS_ADDRESS),
           insn(STOUI, 41, 42, 0))
    p.branch(BZ, zero, "failure")
    return p.build()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check", action="store_true",
        help="verify the checked-in binaries without rewriting them",
    )
    args = parser.parse_args()
    directory = pathlib.Path(__file__).resolve().parent
    fixtures = {
        directory / "mmix-virt-fw.bin": firmware_image(),
        directory / "mmix-virt-kernel.bin": kernel_image(),
    }

    for path, expected in fixtures.items():
        if args.check:
            if path.read_bytes() != expected:
                raise SystemExit(f"{path.name} is not reproducible")
        else:
            path.write_bytes(expected)


if __name__ == "__main__":
    main()
