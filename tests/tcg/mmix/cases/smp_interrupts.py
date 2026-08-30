#!/usr/bin/env python3
#
# MMIX per-CPU interrupt lifecycle test cases
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses

from .common import *
from .smp import (
    SMP_CPU_ID_STACK_PHYS,
    SMPProgram,
    SMP_ENTRY,
    SMP_WAIT_LIMIT,
    TCG_THREAD_MULTI,
    smp_cpu_id_from_stack,
    smp_elf_image,
    smp_load,
    smp_store,
)


SMP_INTERRUPT_MAILBOX_BASE = 0x00200600
SMP_INTERRUPT_MAILBOX_SLOT_SIZE = 0x100

SMP_INTERRUPT_COMMAND = 0x00
SMP_INTERRUPT_STAGE = 0x08
SMP_INTERRUPT_RK_INITIAL = 0x10
SMP_INTERRUPT_RQ_INITIAL = 0x18
SMP_INTERRUPT_RQ_AFTER = 0x20
SMP_INTERRUPT_CLAIM = 0x28
SMP_INTERRUPT_HANDLER_COUNT = 0x30
SMP_INTERRUPT_HANDLER_DONE = 0x38
SMP_INTERRUPT_HANDLER_ACK = 0x40
SMP_INTERRUPT_RWW = 0x48
SMP_INTERRUPT_RXX = 0x50
SMP_INTERRUPT_RYY = 0x58
SMP_INTERRUPT_RZZ = 0x60
SMP_INTERRUPT_RBB = 0x68
SMP_INTERRUPT_RO_ENTRY = 0x70
SMP_INTERRUPT_RS_ENTRY = 0x78
SMP_INTERRUPT_RO_FINAL = 0x80
SMP_INTERRUPT_RS_FINAL = 0x88
SMP_INTERRUPT_RQ_FINAL = 0x90
SMP_INTERRUPT_RK_FINAL = 0x98
SMP_INTERRUPT_SENTINEL_FINAL = 0xa0
SMP_INTERRUPT_HANDLER_RQ = 0xa8

SMP_INTERRUPT_CMD_WAIT_RQ = 1
SMP_INTERRUPT_CMD_CLAIM_ACK = 2
SMP_INTERRUPT_CMD_COMPLETE = 3
SMP_INTERRUPT_CMD_SNAPSHOT = 4
SMP_INTERRUPT_CMD_ENABLE = 5
SMP_INTERRUPT_CMD_FINALIZE = 6
SMP_INTERRUPT_CMD_HALT = 7

SMP_INTERRUPT_STAGE_READY = 1
SMP_INTERRUPT_STAGE_RQ_LATCHED = 2
SMP_INTERRUPT_STAGE_CLAIMED = 3
SMP_INTERRUPT_STAGE_COMPLETED = 4
SMP_INTERRUPT_STAGE_SNAPSHOT = 5
SMP_INTERRUPT_STAGE_ENABLED = 6
SMP_INTERRUPT_STAGE_FINAL = 7
SMP_INTERRUPT_STAGE_FAILURE = 0xdead

SMP_INTERRUPT_HANDLER0 = 0x2000
SMP_INTERRUPT_HANDLER1 = 0x2200
SMP_INTERRUPT_SENTINELS = (0x550, 0x551)


@dataclasses.dataclass(frozen=True)
class MMIXSMPInterruptTest:
    cpu_id_stack_phys = SMP_CPU_ID_STACK_PHYS
    name: str
    image: bytes
    main_end: int
    thread_mode: str = TCG_THREAD_MULTI
    cpu_count: int = 2

    mailbox_base = SMP_INTERRUPT_MAILBOX_BASE
    mailbox_slot_size = SMP_INTERRUPT_MAILBOX_SLOT_SIZE
    command_offset = SMP_INTERRUPT_COMMAND
    stage_offset = SMP_INTERRUPT_STAGE
    rk_initial_offset = SMP_INTERRUPT_RK_INITIAL
    rq_initial_offset = SMP_INTERRUPT_RQ_INITIAL
    rq_after_offset = SMP_INTERRUPT_RQ_AFTER
    claim_offset = SMP_INTERRUPT_CLAIM
    handler_count_offset = SMP_INTERRUPT_HANDLER_COUNT
    handler_done_offset = SMP_INTERRUPT_HANDLER_DONE
    handler_ack_offset = SMP_INTERRUPT_HANDLER_ACK
    rww_offset = SMP_INTERRUPT_RWW
    rxx_offset = SMP_INTERRUPT_RXX
    ryy_offset = SMP_INTERRUPT_RYY
    rzz_offset = SMP_INTERRUPT_RZZ
    rbb_offset = SMP_INTERRUPT_RBB
    ro_entry_offset = SMP_INTERRUPT_RO_ENTRY
    rs_entry_offset = SMP_INTERRUPT_RS_ENTRY
    ro_final_offset = SMP_INTERRUPT_RO_FINAL
    rs_final_offset = SMP_INTERRUPT_RS_FINAL
    rq_final_offset = SMP_INTERRUPT_RQ_FINAL
    rk_final_offset = SMP_INTERRUPT_RK_FINAL
    sentinel_final_offset = SMP_INTERRUPT_SENTINEL_FINAL
    handler_rq_offset = SMP_INTERRUPT_HANDLER_RQ
    command_wait_rq = SMP_INTERRUPT_CMD_WAIT_RQ
    command_claim_ack = SMP_INTERRUPT_CMD_CLAIM_ACK
    command_complete = SMP_INTERRUPT_CMD_COMPLETE
    command_snapshot = SMP_INTERRUPT_CMD_SNAPSHOT
    command_enable = SMP_INTERRUPT_CMD_ENABLE
    command_finalize = SMP_INTERRUPT_CMD_FINALIZE
    command_halt = SMP_INTERRUPT_CMD_HALT
    stage_ready = SMP_INTERRUPT_STAGE_READY
    stage_rq_latched = SMP_INTERRUPT_STAGE_RQ_LATCHED
    stage_claimed = SMP_INTERRUPT_STAGE_CLAIMED
    stage_completed = SMP_INTERRUPT_STAGE_COMPLETED
    stage_snapshot = SMP_INTERRUPT_STAGE_SNAPSHOT
    stage_enabled = SMP_INTERRUPT_STAGE_ENABLED
    stage_final = SMP_INTERRUPT_STAGE_FINAL
    stage_failure = SMP_INTERRUPT_STAGE_FAILURE
    interrupt_request = RQ_INTERRUPT_CONTROLLER
    dynamic_trap_resume_next = DYNAMIC_TRAP_RESUME_NEXT
    timer_irq_base = MMIX_VIRT_TIMER_IRQ_BASE
    sentinels = SMP_INTERRUPT_SENTINELS
    initial_rk = (0, RQ_PROGRAM_B)
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
    return SMP_INTERRUPT_MAILBOX_BASE + cpu * SMP_INTERRUPT_MAILBOX_SLOT_SIZE


def _intc_context_address(cpu, register):
    return (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0] +
        MMIX_VIRT_INTC_CONTEXT_BASE +
        cpu * MMIX_VIRT_INTC_CONTEXT_STRIDE +
        register
    )


def _emit_mailbox_address(dst, cpu_id, scratch):
    return [
        *set_octa(dst, SMP_INTERRUPT_MAILBOX_BASE),
        insn(SLUI, scratch, cpu_id, 8),
        insn(ADDU, dst, dst, scratch),
    ]


def _emit_return_to_command_loop(program, stage):
    program.emit(
        wyde(SETL, R83, stage),
        insn(SYNC, 0, 0, 1),
        smp_store(R83, R40, SMP_INTERRUPT_STAGE),
        *set_octa(R82, SMP_WAIT_LIMIT),
    )
    program.emit_branch(BZ, R254, "command_loop")


def _interrupt_handler(cpu, handler_address):
    mailbox = _mailbox_address(cpu)
    claim = _intc_context_address(cpu, MMIX_VIRT_INTC_CONTEXT_CLAIM)
    complete = _intc_context_address(cpu, MMIX_VIRT_INTC_CONTEXT_COMPLETE)
    irq = MMIX_VIRT_TIMER_IRQ_BASE + cpu
    program = SMPProgram()

    program.emit(
        *set_octa(R180, mailbox),
        *set_octa(R181, claim),
        *set_octa(R182, complete),
        wyde(SETL, R183, irq),
        insn(GET, R199, 0, SR_Q),
        insn(GET, R184, 0, SR_WW),
        insn(GET, R185, 0, SR_XX),
        insn(GET, R186, 0, SR_YY),
        insn(GET, R187, 0, SR_ZZ),
        insn(GET, R188, 0, SR_BB),
        insn(GET, R189, 0, SR_O),
        insn(GET, R190, 0, SR_S),
        smp_store(R184, R180, SMP_INTERRUPT_RWW),
        smp_store(R185, R180, SMP_INTERRUPT_RXX),
        smp_store(R186, R180, SMP_INTERRUPT_RYY),
        smp_store(R187, R180, SMP_INTERRUPT_RZZ),
        smp_store(R188, R180, SMP_INTERRUPT_RBB),
        smp_store(R189, R180, SMP_INTERRUPT_RO_ENTRY),
        smp_store(R190, R180, SMP_INTERRUPT_RS_ENTRY),
        smp_store(R199, R180, SMP_INTERRUPT_HANDLER_RQ),
        insn(LDOUI, R191, R180, SMP_INTERRUPT_HANDLER_COUNT),
        insn(ADDUI, R191, R191, 1),
        insn(LDOU, R192, R181, 0),
        smp_store(R192, R180, SMP_INTERRUPT_CLAIM),
        insn(SYNC, 0, 0, 1),
        smp_store(R191, R180, SMP_INTERRUPT_HANDLER_COUNT),
        *set_octa(R193, SMP_WAIT_LIMIT),
    )
    program.mark("wait_for_source_removal")
    program.emit(
        insn(LDOUI, R194, R180, SMP_INTERRUPT_HANDLER_ACK),
        insn(CMPU, R195, R194, R191),
    )
    program.emit_branch(BZ, R195, "source_removed")
    program.emit(insn(SUBUI, R193, R193, 1))
    program.emit_branch(BNZ, R193, "wait_for_source_removal")
    program.emit(
        wyde(SETL, R196, SMP_INTERRUPT_STAGE_FAILURE),
        smp_store(R196, R180, SMP_INTERRUPT_STAGE),
    )
    program.mark("handler_failure")
    program.emit_branch(BZ, R254, "handler_failure")

    program.mark("source_removed")
    program.emit(
        insn(STOU, R183, R182, 0),
        insn(PUTI, SR_Q, 0, 0),
        insn(SYNC, 0, 0, 1),
        smp_store(R191, R180, SMP_INTERRUPT_HANDLER_DONE),
        *set_octa(R255, RK_INTERRUPT_CONTROLLER),
        insn(RESUME, 0, 0, 1),
    )
    return handler_address, program.build()


def smp_interrupt_lifecycle_program():
    program = SMPProgram()

    program.emit(
        *smp_cpu_id_from_stack(R32, R33, R34),
        wyde(SETL, R254, 0),
        *_emit_mailbox_address(R40, R32, R41),
    )
    program.emit_branch(BZ, R32, "cpu0_setup")

    program.emit(
        *set_octa(R60, _intc_context_address(
            1, MMIX_VIRT_INTC_CONTEXT_ENABLE)),
        *set_octa(R61, _intc_context_address(
            1, MMIX_VIRT_INTC_CONTEXT_CLAIM)),
        *set_octa(R62, _intc_context_address(
            1, MMIX_VIRT_INTC_CONTEXT_COMPLETE)),
        *set_octa(R63, 1 << (MMIX_VIRT_TIMER_IRQ_BASE + 1)),
        wyde(SETL, R65, MMIX_VIRT_TIMER_IRQ_BASE + 1),
        *set_octa(R66, SMP_INTERRUPT_HANDLER1),
        *set_octa(R67, RQ_PROGRAM_B),
        *set_octa(R100, SMP_INTERRUPT_SENTINELS[1]),
    )
    program.emit_branch(BZ, R254, "setup_complete")

    program.mark("cpu0_setup")
    program.emit(
        *set_octa(R60, _intc_context_address(
            0, MMIX_VIRT_INTC_CONTEXT_ENABLE)),
        *set_octa(R61, _intc_context_address(
            0, MMIX_VIRT_INTC_CONTEXT_CLAIM)),
        *set_octa(R62, _intc_context_address(
            0, MMIX_VIRT_INTC_CONTEXT_COMPLETE)),
        *set_octa(R63, 1 << MMIX_VIRT_TIMER_IRQ_BASE),
        wyde(SETL, R65, MMIX_VIRT_TIMER_IRQ_BASE),
        *set_octa(R66, SMP_INTERRUPT_HANDLER0),
        *set_octa(R67, 0),
        *set_octa(R100, SMP_INTERRUPT_SENTINELS[0]),
    )

    program.mark("setup_complete")
    program.emit(
        *set_octa(R70, RK_INTERRUPT_CONTROLLER),
        insn(STOU, R63, R60, 0),
        insn(PUT, SR_TT, 0, R66),
        insn(PUT, SR_K, 0, R67),
        insn(GET, R68, 0, SR_K),
        smp_store(R68, R40, SMP_INTERRUPT_RK_INITIAL),
        insn(ADDU, R255, R100, R254),
        wyde(SETL, R83, SMP_INTERRUPT_STAGE_READY),
        insn(SYNC, 0, 0, 1),
        smp_store(R83, R40, SMP_INTERRUPT_STAGE),
        *set_octa(R82, SMP_WAIT_LIMIT),
    )

    program.mark("command_loop")
    program.emit(smp_load(R80, R40, SMP_INTERRUPT_COMMAND))
    program.emit_branch(BNZ, R80, "dispatch_command")
    program.emit(insn(SUBUI, R82, R82, 1))
    program.emit_branch(BNZ, R82, "command_loop")
    program.emit_branch(BZ, R254, "failure")

    program.mark("dispatch_command")
    program.emit(insn(SYNC, 0, 0, 2))
    for command, label in (
        (SMP_INTERRUPT_CMD_WAIT_RQ, "wait_rq"),
        (SMP_INTERRUPT_CMD_CLAIM_ACK, "claim_ack"),
        (SMP_INTERRUPT_CMD_COMPLETE, "complete"),
        (SMP_INTERRUPT_CMD_SNAPSHOT, "snapshot"),
        (SMP_INTERRUPT_CMD_ENABLE, "enable"),
        (SMP_INTERRUPT_CMD_FINALIZE, "finalize"),
        (SMP_INTERRUPT_CMD_HALT, "halt"),
    ):
        program.emit(insn(CMPUI, R81, R80, command))
        program.emit_branch(BZ, R81, label)
    program.emit_branch(BZ, R254, "failure")

    program.mark("wait_rq")
    program.emit(
        smp_store(R254, R40, SMP_INTERRUPT_COMMAND),
        *set_octa(R84, SMP_WAIT_LIMIT),
    )
    program.mark("wait_rq_poll")
    program.emit(
        insn(GET, R85, 0, SR_Q),
        insn(AND, R86, R85, R70),
    )
    program.emit_branch(BNZ, R86, "rq_latched")
    program.emit(insn(SUBUI, R84, R84, 1))
    program.emit_branch(BNZ, R84, "wait_rq_poll")
    program.emit_branch(BZ, R254, "failure")
    program.mark("rq_latched")
    program.emit(smp_store(R85, R40, SMP_INTERRUPT_RQ_INITIAL))
    _emit_return_to_command_loop(program, SMP_INTERRUPT_STAGE_RQ_LATCHED)

    program.mark("claim_ack")
    program.emit(
        smp_store(R254, R40, SMP_INTERRUPT_COMMAND),
        insn(LDOU, R84, R61, 0),
        smp_store(R84, R40, SMP_INTERRUPT_CLAIM),
    )
    _emit_return_to_command_loop(program, SMP_INTERRUPT_STAGE_CLAIMED)

    program.mark("complete")
    program.emit(
        smp_store(R254, R40, SMP_INTERRUPT_COMMAND),
        insn(STOU, R65, R62, 0),
        insn(PUTI, SR_Q, 0, 0),
        insn(GET, R85, 0, SR_Q),
        smp_store(R85, R40, SMP_INTERRUPT_RQ_AFTER),
    )
    _emit_return_to_command_loop(program, SMP_INTERRUPT_STAGE_COMPLETED)

    program.mark("snapshot")
    program.emit(
        smp_store(R254, R40, SMP_INTERRUPT_COMMAND),
        insn(GET, R85, 0, SR_Q),
        smp_store(R85, R40, SMP_INTERRUPT_RQ_AFTER),
    )
    _emit_return_to_command_loop(program, SMP_INTERRUPT_STAGE_SNAPSHOT)

    program.mark("enable")
    program.emit(
        smp_store(R254, R40, SMP_INTERRUPT_COMMAND),
        *set_octa(R70, RK_INTERRUPT_CONTROLLER),
        insn(PUT, SR_K, 0, R70),
    )
    _emit_return_to_command_loop(program, SMP_INTERRUPT_STAGE_ENABLED)

    program.mark("finalize")
    program.emit(
        smp_store(R254, R40, SMP_INTERRUPT_COMMAND),
        insn(GET, R84, 0, SR_O),
        insn(GET, R85, 0, SR_S),
        insn(GET, R86, 0, SR_Q),
        insn(GET, R87, 0, SR_K),
        smp_store(R84, R40, SMP_INTERRUPT_RO_FINAL),
        smp_store(R85, R40, SMP_INTERRUPT_RS_FINAL),
        smp_store(R86, R40, SMP_INTERRUPT_RQ_FINAL),
        smp_store(R87, R40, SMP_INTERRUPT_RK_FINAL),
        smp_store(R100, R40, SMP_INTERRUPT_SENTINEL_FINAL),
    )
    _emit_return_to_command_loop(program, SMP_INTERRUPT_STAGE_FINAL)

    program.mark("halt")
    program.emit(
        insn(ADDU, R255, R254, R254),
        halt(),
    )

    program.mark("failure")
    program.emit(
        wyde(SETL, R83, SMP_INTERRUPT_STAGE_FAILURE),
        smp_store(R83, R40, SMP_INTERRUPT_STAGE),
    )
    program.mark("failure_idle")
    program.emit_branch(BZ, R254, "failure_idle")

    main = program.build()
    handlers = (
        _interrupt_handler(0, SMP_INTERRUPT_HANDLER0),
        _interrupt_handler(1, SMP_INTERRUPT_HANDLER1),
    )
    return MMIXSMPInterruptTest(
        name="smp-multi-thread-per-cpu-interrupt-lifecycle",
        image=smp_elf_image(main, *handlers),
        main_end=SMP_ENTRY + len(main),
    )


SMP_INTERRUPT_TESTS = [smp_interrupt_lifecycle_program()]
