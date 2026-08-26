#!/usr/bin/env python3
#
# MMIX shared interrupt routing test cases
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses

from .common import *
from .smp import (
    SMPProgram,
    SMP_ENTRY,
    SMP_WAIT_LIMIT,
    TCG_THREAD_MULTI,
    smp_elf_image,
    smp_load,
    smp_store,
)


SMP_SHARED_MAILBOX_BASE = 0x00200800
SMP_SHARED_MAILBOX_SLOT_SIZE = 0x100

SMP_SHARED_STAGE = 0x00
SMP_SHARED_PROGRESS = 0x08
SMP_SHARED_HANDLER_COUNT = 0x10
SMP_SHARED_HANDLER_DONE = 0x18
SMP_SHARED_HANDLER_ACK = 0x20
SMP_SHARED_CLAIM = 0x28
SMP_SHARED_SOURCE_COUNT = 0x30
SMP_SHARED_TIMER_COUNT = 0x38
SMP_SHARED_RESUME_COUNT = 0x40
SMP_SHARED_RWW = 0x48
SMP_SHARED_RXX = 0x50
SMP_SHARED_RYY = 0x58
SMP_SHARED_RZZ = 0x60
SMP_SHARED_RBB = 0x68
SMP_SHARED_RO_ENTRY = 0x70
SMP_SHARED_RS_ENTRY = 0x78
SMP_SHARED_HANDLER_RQ = 0x80
SMP_SHARED_RO_FINAL = 0x88
SMP_SHARED_RS_FINAL = 0x90
SMP_SHARED_RQ_FINAL = 0x98
SMP_SHARED_RK_FINAL = 0xa0
SMP_SHARED_SENTINEL_FINAL = 0xa8
SMP_SHARED_HALT = 0xb0

SMP_SHARED_STAGE_READY = 1
SMP_SHARED_STAGE_FAILURE = 0xdead

SMP_SHARED_IRQ = 4
SMP_SHARED_HANDLER0 = 0x2800
SMP_SHARED_HANDLER1 = 0x2a00
SMP_SHARED_SENTINELS = (0x660, 0x661)


@dataclasses.dataclass(frozen=True)
class MMIXSMPSharedInterruptTest:
    name: str
    image: bytes
    main_end: int
    thread_mode: str = TCG_THREAD_MULTI
    cpu_count: int = 2

    mailbox_base = SMP_SHARED_MAILBOX_BASE
    mailbox_slot_size = SMP_SHARED_MAILBOX_SLOT_SIZE
    stage_offset = SMP_SHARED_STAGE
    progress_offset = SMP_SHARED_PROGRESS
    handler_count_offset = SMP_SHARED_HANDLER_COUNT
    handler_done_offset = SMP_SHARED_HANDLER_DONE
    handler_ack_offset = SMP_SHARED_HANDLER_ACK
    claim_offset = SMP_SHARED_CLAIM
    shared_count_offset = SMP_SHARED_SOURCE_COUNT
    timer_count_offset = SMP_SHARED_TIMER_COUNT
    resume_count_offset = SMP_SHARED_RESUME_COUNT
    rww_offset = SMP_SHARED_RWW
    rxx_offset = SMP_SHARED_RXX
    ryy_offset = SMP_SHARED_RYY
    rzz_offset = SMP_SHARED_RZZ
    rbb_offset = SMP_SHARED_RBB
    ro_entry_offset = SMP_SHARED_RO_ENTRY
    rs_entry_offset = SMP_SHARED_RS_ENTRY
    handler_rq_offset = SMP_SHARED_HANDLER_RQ
    ro_final_offset = SMP_SHARED_RO_FINAL
    rs_final_offset = SMP_SHARED_RS_FINAL
    rq_final_offset = SMP_SHARED_RQ_FINAL
    rk_final_offset = SMP_SHARED_RK_FINAL
    sentinel_final_offset = SMP_SHARED_SENTINEL_FINAL
    halt_offset = SMP_SHARED_HALT
    stage_ready = SMP_SHARED_STAGE_READY
    stage_failure = SMP_SHARED_STAGE_FAILURE
    shared_irq = SMP_SHARED_IRQ
    interrupt_request = RQ_INTERRUPT_CONTROLLER
    dynamic_trap_resume_next = DYNAMIC_TRAP_RESUME_NEXT
    timer_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0]
    intc_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0]
    timer_irq_base = MMIX_VIRT_TIMER_IRQ_BASE
    timer_context_base = MMIX_VIRT_TIMER_CONTEXT_BASE
    timer_context_stride = MMIX_VIRT_TIMER_CONTEXT_STRIDE
    timer_compare = MMIX_VIRT_TIMER_CONTEXT_COMPARE
    timer_control = MMIX_VIRT_TIMER_CONTEXT_CONTROL
    timer_status = MMIX_VIRT_TIMER_CONTEXT_STATUS
    timer_control_enabled = (
        MMIX_VIRT_TIMER_CONTROL_ENABLE |
        MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE
    )
    intc_context_base = MMIX_VIRT_INTC_CONTEXT_BASE
    intc_context_stride = MMIX_VIRT_INTC_CONTEXT_STRIDE
    intc_enable = MMIX_VIRT_INTC_CONTEXT_ENABLE
    sentinels = SMP_SHARED_SENTINELS
    main_start = SMP_ENTRY
    initial_stack = INITIAL_STACK
    initial_stack_slot_size = MMIX_VIRT_INITIAL_STACK_SLOT_SIZE

    @property
    def qemu_args(self):
        return (
            "-smp", str(self.cpu_count),
            "-accel", f"tcg,thread={self.thread_mode}",
        )


def _mailbox_address(cpu):
    return SMP_SHARED_MAILBOX_BASE + cpu * SMP_SHARED_MAILBOX_SLOT_SIZE


def _intc_context_address(cpu, register):
    return (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0] +
        MMIX_VIRT_INTC_CONTEXT_BASE +
        cpu * MMIX_VIRT_INTC_CONTEXT_STRIDE +
        register
    )


def _timer_context_address(cpu):
    return (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0] +
        MMIX_VIRT_TIMER_CONTEXT_BASE +
        cpu * MMIX_VIRT_TIMER_CONTEXT_STRIDE
    )


def _emit_mailbox_address(dst, cpu_id, scratch):
    return [
        *set_octa(dst, SMP_SHARED_MAILBOX_BASE),
        insn(SLUI, scratch, cpu_id, 8),
        insn(ADDU, dst, dst, scratch),
    ]


def _shared_interrupt_handler(cpu, handler_address):
    mailbox = _mailbox_address(cpu)
    claim = _intc_context_address(cpu, MMIX_VIRT_INTC_CONTEXT_CLAIM)
    complete = _intc_context_address(cpu, MMIX_VIRT_INTC_CONTEXT_COMPLETE)
    timer = _timer_context_address(cpu)
    timer_irq = MMIX_VIRT_TIMER_IRQ_BASE + cpu
    program = SMPProgram()

    program.emit(
        *set_octa(R180, mailbox),
        *set_octa(R181, claim),
        *set_octa(R182, complete),
        *set_octa(R183, timer),
        wyde(SETL, R184, timer_irq),
        wyde(SETL, R185, SMP_SHARED_IRQ),
        wyde(SETL, R186, MMIX_VIRT_TIMER_STATUS_PENDING),
        insn(GET, R199, 0, SR_Q),
        insn(GET, R187, 0, SR_WW),
        insn(GET, R188, 0, SR_XX),
        insn(GET, R189, 0, SR_YY),
        insn(GET, R190, 0, SR_ZZ),
        insn(GET, R191, 0, SR_BB),
        insn(GET, R192, 0, SR_O),
        insn(GET, R193, 0, SR_S),
        smp_store(R187, R180, SMP_SHARED_RWW),
        smp_store(R188, R180, SMP_SHARED_RXX),
        smp_store(R189, R180, SMP_SHARED_RYY),
        smp_store(R190, R180, SMP_SHARED_RZZ),
        smp_store(R191, R180, SMP_SHARED_RBB),
        smp_store(R192, R180, SMP_SHARED_RO_ENTRY),
        smp_store(R193, R180, SMP_SHARED_RS_ENTRY),
        smp_store(R199, R180, SMP_SHARED_HANDLER_RQ),
        insn(LDOUI, R194, R180, SMP_SHARED_HANDLER_COUNT),
        insn(ADDUI, R194, R194, 1),
        insn(LDTU, R195, R181, 0),
        smp_store(R195, R180, SMP_SHARED_CLAIM),
        smp_store(R194, R180, SMP_SHARED_HANDLER_COUNT),
        insn(CMPU, R196, R195, R184),
    )
    program.emit_branch(BZ, R196, "timer_source")
    program.emit(insn(CMPU, R196, R195, R185))
    program.emit_branch(BNZ, R196, "failure")

    program.emit(
        insn(LDOUI, R197, R180, SMP_SHARED_SOURCE_COUNT),
        insn(ADDUI, R197, R197, 1),
        smp_store(R197, R180, SMP_SHARED_SOURCE_COUNT),
        insn(SYNC, 0, 0, 1),
        *set_octa(R198, SMP_WAIT_LIMIT),
    )
    program.mark("wait_for_shared_ack")
    program.emit(
        insn(LDOUI, R197, R180, SMP_SHARED_HANDLER_ACK),
        insn(CMPU, R196, R197, R194),
    )
    program.emit_branch(BZ, R196, "complete_source")
    program.emit(insn(SUBUI, R198, R198, 1))
    program.emit_branch(BNZ, R198, "wait_for_shared_ack")
    program.emit_branch(BZ, R254, "failure")

    program.mark("timer_source")
    program.emit(
        insn(LDOUI, R197, R180, SMP_SHARED_TIMER_COUNT),
        insn(ADDUI, R197, R197, 1),
        smp_store(R197, R180, SMP_SHARED_TIMER_COUNT),
        smp_store(R254, R183, MMIX_VIRT_TIMER_CONTEXT_CONTROL),
        smp_store(R186, R183, MMIX_VIRT_TIMER_CONTEXT_STATUS),
    )

    program.mark("complete_source")
    program.emit(
        insn(STTU, R195, R182, 0),
        insn(PUTI, SR_Q, 0, 0),
        insn(SYNC, 0, 0, 1),
        smp_store(R194, R180, SMP_SHARED_HANDLER_DONE),
        *set_octa(R255, RK_INTERRUPT_CONTROLLER),
        insn(RESUME, 0, 0, 1),
    )

    program.mark("failure")
    program.emit(
        wyde(SETL, R197, SMP_SHARED_STAGE_FAILURE),
        smp_store(R197, R180, SMP_SHARED_STAGE),
    )
    program.mark("failure_idle")
    program.emit_branch(BZ, R254, "failure_idle")
    return handler_address, program.build()


def smp_shared_interrupt_program():
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        *_emit_mailbox_address(R40, R32, R41),
    )
    program.emit_branch(BZ, R32, "cpu0_setup")
    program.emit(
        *set_octa(R60, _intc_context_address(
            1, MMIX_VIRT_INTC_CONTEXT_ENABLE)),
        *set_octa(R61, (1 << SMP_SHARED_IRQ) |
                  (1 << (MMIX_VIRT_TIMER_IRQ_BASE + 1))),
        *set_octa(R62, SMP_SHARED_HANDLER1),
        *set_octa(R100, SMP_SHARED_SENTINELS[1]),
    )
    program.emit_branch(BZ, R254, "setup_complete")

    program.mark("cpu0_setup")
    program.emit(
        *set_octa(R60, _intc_context_address(
            0, MMIX_VIRT_INTC_CONTEXT_ENABLE)),
        *set_octa(R61, (1 << SMP_SHARED_IRQ) |
                  (1 << MMIX_VIRT_TIMER_IRQ_BASE)),
        *set_octa(R62, SMP_SHARED_HANDLER0),
        *set_octa(R100, SMP_SHARED_SENTINELS[0]),
    )

    program.mark("setup_complete")
    program.emit(
        insn(STTU, R61, R60, 0),
        insn(PUT, SR_TT, 0, R62),
        *set_octa(R63, RK_INTERRUPT_CONTROLLER),
        insn(PUT, SR_K, 0, R63),
        insn(ADDU, R255, R100, R254),
        wyde(SETL, R64, SMP_SHARED_STAGE_READY),
        insn(SYNC, 0, 0, 1),
        smp_store(R64, R40, SMP_SHARED_STAGE),
    )

    program.mark("idle")
    program.emit(
        insn(GET, R70, 0, SR_O),
        insn(GET, R71, 0, SR_S),
        insn(GET, R72, 0, SR_Q),
        insn(GET, R73, 0, SR_K),
        smp_store(R70, R40, SMP_SHARED_RO_FINAL),
        smp_store(R71, R40, SMP_SHARED_RS_FINAL),
        smp_store(R72, R40, SMP_SHARED_RQ_FINAL),
        smp_store(R73, R40, SMP_SHARED_RK_FINAL),
        smp_store(R100, R40, SMP_SHARED_SENTINEL_FINAL),
        smp_load(R74, R40, SMP_SHARED_HANDLER_DONE),
        smp_store(R74, R40, SMP_SHARED_RESUME_COUNT),
        smp_load(R75, R40, SMP_SHARED_PROGRESS),
        insn(ADDUI, R75, R75, 1),
        smp_store(R75, R40, SMP_SHARED_PROGRESS),
        insn(SYNC, 0, 0, 1),
        smp_load(R76, R40, SMP_SHARED_HALT),
    )
    program.emit_branch(BZ, R76, "idle")
    program.emit(
        insn(ADDU, R255, R254, R254),
        halt(),
    )

    main = program.build()
    handlers = (
        _shared_interrupt_handler(0, SMP_SHARED_HANDLER0),
        _shared_interrupt_handler(1, SMP_SHARED_HANDLER1),
    )
    return MMIXSMPSharedInterruptTest(
        name="smp-multi-thread-shared-interrupt-routing",
        image=smp_elf_image(main, *handlers),
        main_end=SMP_ENTRY + len(main),
    )


SMP_SHARED_INTERRUPT_TESTS = [smp_shared_interrupt_program()]
