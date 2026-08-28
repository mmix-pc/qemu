#!/usr/bin/env python3
#
# MMIX replacement-platform SMP acceptance case
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses

from .common import *
from .smp import (
    SMPProgram,
    SMP_ENTRY,
    TCG_THREAD_MULTI,
    smp_elf_image,
    smp_load,
    smp_store,
)


L3_CPU_IDS = (0, 63)
L3_CPU_COUNT = 64
L3_MAILBOX_BASE = 0x00300000
L3_MAILBOX_STRIDE = 0x100
L3_READY = 0x00
L3_IPI_COUNT = 0x08
L3_TIMER_COUNT = 0x10
L3_IPI_RQ = 0x18
L3_TIMER_RQ = 0x20
L3_TIMER_CLAIM = 0x28
L3_HANDLER_RO = 0x30
L3_HANDLER_RS = 0x38
L3_HALT = 0x40
L3_HANDLER0 = 0x4000
L3_HANDLER63 = 0x4400


@dataclasses.dataclass(frozen=True)
class MMIXL3SMPTest:
    name: str
    image: bytes
    main_end: int
    cpu_count: int = L3_CPU_COUNT
    thread_mode: str = TCG_THREAD_MULTI

    cpu_ids = L3_CPU_IDS
    mailbox_base = L3_MAILBOX_BASE
    mailbox_stride = L3_MAILBOX_STRIDE
    ready_offset = L3_READY
    ipi_count_offset = L3_IPI_COUNT
    timer_count_offset = L3_TIMER_COUNT
    ipi_rq_offset = L3_IPI_RQ
    timer_rq_offset = L3_TIMER_RQ
    timer_claim_offset = L3_TIMER_CLAIM
    handler_ro_offset = L3_HANDLER_RO
    handler_rs_offset = L3_HANDLER_RS
    halt_offset = L3_HALT
    ipi_request = RQ_IPI
    timer_request = RQ_INTERRUPT_CONTROLLER
    initial_stack = INITIAL_STACK
    initial_stack_slot_size = MMIX_VIRT_INITIAL_STACK_SLOT_SIZE
    main_start = SMP_ENTRY
    ipi_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0]
    timer_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0]

    @property
    def qemu_args(self):
        return (
            "-smp", str(self.cpu_count),
            "-accel", f"tcg,thread={self.thread_mode}",
        )

    def mailbox(self, cpu, offset=0):
        return self.mailbox_base + cpu * self.mailbox_stride + offset

    def timer_context(self, cpu, register):
        return (
            self.timer_base + MMIX_VIRT_TIMER_CONTEXT_BASE +
            cpu * MMIX_VIRT_TIMER_CONTEXT_STRIDE + register
        )


def _mailbox(cpu):
    return L3_MAILBOX_BASE + cpu * L3_MAILBOX_STRIDE


def _ipi_context(cpu, register):
    return (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] + MMIX_VIRT_IPI_CONTEXT_BASE +
        cpu * MMIX_VIRT_IPI_CONTEXT_STRIDE + register
    )


def _timer_context(cpu, register):
    return (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0] +
        MMIX_VIRT_TIMER_CONTEXT_BASE +
        cpu * MMIX_VIRT_TIMER_CONTEXT_STRIDE + register
    )


def _intc_context(cpu, register):
    return (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0] +
        MMIX_VIRT_INTC_CONTEXT_BASE +
        cpu * MMIX_VIRT_INTC_CONTEXT_STRIDE + register
    )


def _handler(cpu, address):
    mailbox = _mailbox(cpu)
    ipi_status = _ipi_context(cpu, MMIX_VIRT_IPI_CONTEXT_STATUS)
    ipi_clear = _ipi_context(cpu, MMIX_VIRT_IPI_CONTEXT_CLEAR)
    timer = _timer_context(cpu, 0)
    claim = _intc_context(cpu, MMIX_VIRT_INTC_CONTEXT_CLAIM)
    complete = _intc_context(cpu, MMIX_VIRT_INTC_CONTEXT_COMPLETE)
    timer_irq = MMIX_VIRT_TIMER_IRQ_BASE + cpu
    program = SMPProgram()

    program.emit(
        *set_octa(R180, mailbox),
        *set_octa(R181, ipi_status),
        *set_octa(R182, ipi_clear),
        *set_octa(R183, timer),
        *set_octa(R184, claim),
        *set_octa(R185, complete),
        insn(GET, R190, 0, SR_Q),
        insn(GET, R191, 0, SR_O),
        insn(GET, R192, 0, SR_S),
        smp_store(R191, R180, L3_HANDLER_RO),
        smp_store(R192, R180, L3_HANDLER_RS),
        *set_octa(R193, RQ_IPI),
        insn(AND, R194, R190, R193),
    )
    program.emit_branch(BZ, R194, "timer")
    program.emit(
        smp_store(R190, R180, L3_IPI_RQ),
        insn(LDOUI, R195, R180, L3_IPI_COUNT),
        insn(ADDUI, R195, R195, 1),
        smp_store(R195, R180, L3_IPI_COUNT),
        wyde(SETL, R196, MMIX_VIRT_IPI_STATUS_PENDING),
        insn(STOUI, R196, R182, 0),
    )
    program.mark("timer")
    program.emit(
        *set_octa(R193, RQ_INTERRUPT_CONTROLLER),
        insn(AND, R194, R190, R193),
    )
    program.emit_branch(BZ, R194, "resume")
    program.emit(
        smp_store(R190, R180, L3_TIMER_RQ),
        insn(LDOUI, R196, R184, 0),
        smp_store(R196, R180, L3_TIMER_CLAIM),
        insn(LDOUI, R195, R180, L3_TIMER_COUNT),
        insn(ADDUI, R195, R195, 1),
        smp_store(R195, R180, L3_TIMER_COUNT),
        smp_store(R254, R183, MMIX_VIRT_TIMER_CONTEXT_CONTROL),
        wyde(SETL, R197, MMIX_VIRT_TIMER_STATUS_PENDING),
        smp_store(R197, R183, MMIX_VIRT_TIMER_CONTEXT_STATUS),
        wyde(SETL, R196, timer_irq),
        insn(STOUI, R196, R185, 0),
    )
    program.mark("resume")
    program.emit(
        insn(PUTI, SR_Q, 0, 0),
        *set_octa(R255, RQ_IPI | RQ_INTERRUPT_CONTROLLER),
        insn(RESUME, 0, 0, 1),
    )
    return address, program.build()


def l3_cpu0_cpu63_interrupt_program():
    program = SMPProgram()

    program.emit(
        insn(GET, R32, 0, SR_O),
        *set_octa(R33, INITIAL_STACK),
        insn(SUBU, R32, R32, R33),
        insn(SRUI, R32, R32, 15),
        wyde(SETL, R254, 0),
    )
    program.emit_branch(BZ, R32, "cpu0")
    program.emit(insn(CMPUI, R33, R32, 63))
    program.emit_branch(BZ, R33, "cpu63")
    program.mark("inactive")
    program.emit_branch(BZ, R254, "inactive")

    program.mark("cpu63")
    program.emit(*set_octa(R40, _mailbox(63)))
    program.emit(*set_octa(R41, L3_HANDLER63))
    program.emit_branch(BZ, R254, "setup")

    program.mark("cpu0")
    program.emit(*set_octa(R40, _mailbox(0)))
    program.emit(*set_octa(R41, L3_HANDLER0))

    program.mark("setup")
    irq = MMIX_VIRT_TIMER_IRQ_BASE
    program.emit_branch(BZ, R32, "enable_cpu0")
    irq += 63
    program.emit(
        *set_octa(R42, _intc_context(63, 8)),
        *set_octa(R43, 1 << (irq - 64)),
    )
    program.emit_branch(BZ, R254, "enable")
    program.mark("enable_cpu0")
    program.emit(
        *set_octa(R42, _intc_context(0, 0)),
        *set_octa(R43, 1 << MMIX_VIRT_TIMER_IRQ_BASE),
    )
    program.mark("enable")
    program.emit(
        insn(STOU, R43, R42, R0),
        insn(PUT, SR_TT, 0, R41),
        *set_octa(R44, RQ_IPI | RQ_INTERRUPT_CONTROLLER),
        insn(PUT, SR_K, 0, R44),
        wyde(SETL, R45, 1),
        smp_store(R45, R40, L3_READY),
    )
    program.mark("idle")
    program.emit(
        smp_load(R46, R40, L3_HALT),
    )
    program.emit_branch(BZ, R46, "idle")
    program.emit(halt())

    main = program.build()
    handlers = (
        _handler(0, L3_HANDLER0),
        _handler(63, L3_HANDLER63),
    )
    return MMIXL3SMPTest(
        name="l3-cpu0-cpu63-interrupt-isolation",
        image=smp_elf_image(main, *handlers),
        main_end=SMP_ENTRY + len(main),
    )


L3_SMP_TESTS = [l3_cpu0_cpu63_interrupt_program()]
