#!/usr/bin/env python3
#
# MMIX SMP execution test cases
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses
from typing import Optional

from .common import *


SMP_CPU_COUNT = 2
SMP_ENTRY = 0x1000
SMP_MAILBOX_BASE = 0x00200000
SMP_MAILBOX_SLOT_SIZE = 0x40
SMP_WAIT_LIMIT = 1 << 28
SMP_TIMEOUT_WAIT_LIMIT = 1024
SMP_TIMEOUT_RESULT = 0xdead

SMP_MAILBOX_CPU_ID = 0x00
SMP_MAILBOX_BOOTINFO = 0x08
SMP_MAILBOX_ENTRY = 0x10
SMP_MAILBOX_RO = 0x18
SMP_MAILBOX_RS = 0x20
SMP_MAILBOX_PROGRESS = 0x28
SMP_MAILBOX_READY = 0x30
SMP_MAILBOX_RESULT = 0x38

TCG_THREAD_SINGLE = "single"
TCG_THREAD_MULTI = "multi"
TCG_THREAD_MODES = (TCG_THREAD_SINGLE, TCG_THREAD_MULTI)


@dataclasses.dataclass(frozen=True)
class MMIXSMPTest:
    name: str
    image: bytes
    pc: int
    regs: dict[int, int]
    thread_mode: str
    cpu_count: int = SMP_CPU_COUNT
    output: Optional[bytes] = None
    exit_status: int = 0

    @property
    def qemu_args(self):
        if self.thread_mode not in TCG_THREAD_MODES:
            raise ValueError(f"unsupported TCG thread mode: {self.thread_mode}")
        return (
            "-smp", str(self.cpu_count),
            "-accel", f"tcg,thread={self.thread_mode}",
        )


@dataclasses.dataclass(frozen=True)
class MMIXSMPResetTest:
    name: str
    image: bytes
    idle_pc: int
    reset_idle_pc: int
    reset_regs: tuple[dict[str, int], ...]
    thread_mode: str = TCG_THREAD_MULTI
    cpu_count: int = SMP_CPU_COUNT

    @property
    def qemu_args(self):
        return MMIXSMPTest(
            self.name, self.image, self.idle_pc, {}, self.thread_mode,
            cpu_count=self.cpu_count,
        ).qemu_args


@dataclasses.dataclass(frozen=True)
class SMPProgramImage:
    code: bytes
    success_pc: int
    timeout_pc: int
    success_regs: dict[int, int]


class SMPProgram:
    def __init__(self):
        self.instructions = []
        self.labels = {}
        self.fixups = []

    def emit(self, *instructions):
        self.instructions.extend(instructions)

    def mark(self, label):
        if label in self.labels:
            raise ValueError(f"duplicate SMP program label: {label}")
        self.labels[label] = len(self.instructions)

    def emit_branch(self, op, reg, label):
        self.fixups.append((len(self.instructions), op, reg, label))
        self.instructions.append(None)

    def build(self):
        for index, op, reg, label in self.fixups:
            if label not in self.labels:
                raise ValueError(f"unknown SMP program label: {label}")
            displacement = self.labels[label] - index
            if not -(1 << 15) <= displacement < (1 << 15):
                raise ValueError(f"SMP branch to {label} is out of range")
            branch_op = op if displacement >= 0 else op | 1
            self.instructions[index] = branch(
                branch_op, reg, displacement & 0xffff
            )
        return b"".join(self.instructions)

    def address(self, label, base=SMP_ENTRY):
        return base + self.labels[label] * 4


def smp_elf_image(code, *regions):
    all_regions = ((SMP_ENTRY, code), *regions)
    end = max(address + len(data) for address, data in all_regions)
    image = bytearray(end - SMP_ENTRY)

    for address, data in all_regions:
        if address < SMP_ENTRY:
            raise ValueError("SMP ELF region precedes the entry point")
        offset = address - SMP_ENTRY
        image[offset:offset + len(data)] = data
    return elf64_image(SMP_ENTRY, bytes(image), entry=SMP_ENTRY)


def smp_emit_unconditional_branch(program, label, zero=R254):
    program.emit_branch(BZ, zero, label)


def smp_emit_wait_equal(program, *, address, field, expected, value, compare,
                        counter, label, timeout_label):
    program.emit(*set_octa(counter, SMP_WAIT_LIMIT))
    program.mark(f"{label}_wait")
    program.emit(
        smp_load(value, address, field),
        insn(CMPU, compare, value, expected),
    )
    program.emit_branch(BZ, compare, f"{label}_done")
    program.emit(insn(SUBUI, counter, counter, 1))
    program.emit_branch(BNZ, counter, f"{label}_wait")
    smp_emit_unconditional_branch(program, timeout_label)
    program.mark(f"{label}_done")


def smp_mailbox_address(dst, cpu_id, scratch):
    return [
        *set_octa(dst, SMP_MAILBOX_BASE),
        insn(SLUI, scratch, cpu_id, 6),
        insn(ADDU, dst, dst, scratch),
    ]


def smp_load(dst, mailbox, field):
    return insn(LDOUI, dst, mailbox, field)


def smp_store(src, mailbox, field):
    return insn(STOUI, src, mailbox, field)


def smp_sync(mode=0):
    return insn(SYNC, 0, 0, mode)


def smp_cswap(value, address, offset=0):
    return insn(CSWAPI, value, address, offset)


def smp_mailbox_baseline_program(wait_limit=SMP_WAIT_LIMIT):
    bootinfo = MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0]
    program = SMPProgram()

    program.emit(
        branch(GETA, R34, 0),
        insn(ADDI, R32, R0, 0),
        insn(ADDI, R33, R1, 0),
        insn(GET, R35, 0, SR_O),
        insn(GET, R36, 0, SR_S),
        *smp_mailbox_address(R40, R32, R41),
        smp_store(R32, R40, SMP_MAILBOX_CPU_ID),
        smp_store(R33, R40, SMP_MAILBOX_BOOTINFO),
        smp_store(R34, R40, SMP_MAILBOX_ENTRY),
        smp_store(R35, R40, SMP_MAILBOX_RO),
        smp_store(R36, R40, SMP_MAILBOX_RS),
        wyde(SETL, R42, 1),
        smp_store(R42, R40, SMP_MAILBOX_PROGRESS),
        smp_sync(1),
        smp_store(R42, R40, SMP_MAILBOX_READY),
        smp_sync(),
    )
    program.emit_branch(BNZ, R32, "secondary_idle")

    program.emit(
        *set_octa(R49, SMP_MAILBOX_BASE + SMP_MAILBOX_SLOT_SIZE),
        *set_octa(R43, wait_limit),
        wyde(SETL, R45, 1),
    )
    program.mark("wait_for_secondary")
    program.emit(smp_load(R44, R49, SMP_MAILBOX_READY))
    program.emit_branch(BNZ, R44, "secondary_ready")
    program.emit(
        insn(ADDUI, R45, R45, 1),
        smp_store(R45, R40, SMP_MAILBOX_PROGRESS),
        insn(SUBUI, R43, R43, 1),
    )
    program.emit_branch(BNZ, R43, "wait_for_secondary")
    program.emit(
        wyde(SETL, R46, SMP_TIMEOUT_RESULT),
        smp_store(R46, R40, SMP_MAILBOX_RESULT),
    )
    program.mark("timeout_halt")
    program.emit(halt())

    program.mark("secondary_ready")
    program.emit(
        smp_sync(2),
        smp_load(R50, R49, SMP_MAILBOX_CPU_ID),
        smp_load(R51, R49, SMP_MAILBOX_BOOTINFO),
        smp_load(R52, R49, SMP_MAILBOX_ENTRY),
        smp_load(R53, R49, SMP_MAILBOX_RO),
        smp_load(R54, R49, SMP_MAILBOX_RS),
        smp_load(R55, R49, SMP_MAILBOX_PROGRESS),
        *set_octa(R59, SMP_MAILBOX_BASE),
        smp_load(R60, R59, SMP_MAILBOX_CPU_ID),
        smp_load(R61, R59, SMP_MAILBOX_BOOTINFO),
        smp_load(R62, R59, SMP_MAILBOX_ENTRY),
        smp_load(R63, R59, SMP_MAILBOX_RO),
        smp_load(R64, R59, SMP_MAILBOX_RS),
        wyde(SETL, R46, 1),
        smp_store(R46, R40, SMP_MAILBOX_RESULT),
    )
    program.mark("success_halt")
    program.emit(halt())

    program.mark("secondary_idle")
    program.emit(jump(JMP, 0))

    image = program.build()
    regs = {
        R32: 0,
        R33: bootinfo,
        R34: SMP_ENTRY,
        R35: INITIAL_STACK,
        R36: INITIAL_STACK,
        R46: 1,
        R50: 1,
        R51: bootinfo,
        R52: SMP_ENTRY,
        R53: INITIAL_STACK + MMIX_VIRT_INITIAL_STACK_SLOT_SIZE,
        R54: INITIAL_STACK + MMIX_VIRT_INITIAL_STACK_SLOT_SIZE,
        R55: 1,
        R60: 0,
        R61: bootinfo,
        R62: SMP_ENTRY,
        R63: INITIAL_STACK,
        R64: INITIAL_STACK,
    }
    return SMPProgramImage(
        code=image,
        success_pc=program.address("success_halt"),
        timeout_pc=program.address("timeout_halt"),
        success_regs=regs,
    )


SMP_MAILBOX_BASELINE = smp_mailbox_baseline_program()
SMP_MAILBOX_TIMEOUT = smp_mailbox_baseline_program(SMP_TIMEOUT_WAIT_LIMIT)

SMP_TESTS = [
    MMIXSMPTest(
        "smp-single-thread-mailbox-baseline",
        elf64_image(
            SMP_ENTRY,
            SMP_MAILBOX_BASELINE.code,
            entry=SMP_ENTRY,
        ),
        pc=SMP_MAILBOX_BASELINE.success_pc,
        regs=SMP_MAILBOX_BASELINE.success_regs,
        thread_mode=TCG_THREAD_SINGLE,
    ),
    MMIXSMPTest(
        "smp-single-thread-missing-peer-timeout",
        elf64_image(
            SMP_ENTRY,
            SMP_MAILBOX_TIMEOUT.code,
            entry=SMP_ENTRY,
        ),
        pc=SMP_MAILBOX_TIMEOUT.timeout_pc,
        regs={
            R32: 0,
            R33: MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0],
            R34: SMP_ENTRY,
            R35: INITIAL_STACK,
            R36: INITIAL_STACK,
            R43: 0,
            R44: 0,
            R46: SMP_TIMEOUT_RESULT,
        },
        thread_mode=TCG_THREAD_SINGLE,
        cpu_count=1,
    ),
]

SMP_MTTCG_TESTS = [
    MMIXSMPTest(
        "smp-multi-thread-mailbox-smoke",
        elf64_image(
            SMP_ENTRY,
            SMP_MAILBOX_BASELINE.code,
            entry=SMP_ENTRY,
        ),
        pc=SMP_MAILBOX_BASELINE.success_pc,
        regs=SMP_MAILBOX_BASELINE.success_regs,
        thread_mode=TCG_THREAD_MULTI,
    ),
]
