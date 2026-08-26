#!/usr/bin/env python3
#
# MMIX inter-processor interrupt lifecycle test case
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


SMP_IPI_MAILBOX_BASE = 0x00200c00
SMP_IPI_MAILBOX_SLOT_SIZE = 0x100

SMP_IPI_COMMAND = 0x00
SMP_IPI_STAGE = 0x08
SMP_IPI_SEND_MASK = 0x10
SMP_IPI_GENERATION = 0x18
SMP_IPI_PAYLOAD = 0x20
SMP_IPI_ACK = 0x28
SMP_IPI_HANDLER_COUNT = 0x30
SMP_IPI_HANDLER_DONE = 0x38
SMP_IPI_IPI_COUNT = 0x40
SMP_IPI_SHARED_COUNT = 0x48
SMP_IPI_TIMER_COUNT = 0x50
SMP_IPI_SHARED_RELEASE = 0x58
SMP_IPI_TARGET_ID = 0x60
SMP_IPI_STATUS_ENTRY = 0x68
SMP_IPI_CLAIM = 0x70
SMP_IPI_HANDLER_RQ = 0x78
SMP_IPI_HANDLER_RK = 0x80
SMP_IPI_RWW = 0x88
SMP_IPI_RXX = 0x90
SMP_IPI_RYY = 0x98
SMP_IPI_RZZ = 0xa0
SMP_IPI_RBB = 0xa8
SMP_IPI_RO_ENTRY = 0xb0
SMP_IPI_RS_ENTRY = 0xb8
SMP_IPI_RO_FINAL = 0xc0
SMP_IPI_RS_FINAL = 0xc8
SMP_IPI_RQ_FINAL = 0xd0
SMP_IPI_RK_FINAL = 0xd8
SMP_IPI_SENTINEL_FINAL = 0xe0
SMP_IPI_PROGRESS = 0xe8
SMP_IPI_SENDER_ACK = 0xf0
SMP_IPI_FAILURE = 0xf8

SMP_IPI_CMD_SEND = 1
SMP_IPI_CMD_SEND_TWICE = 2
SMP_IPI_CMD_MASK = 3
SMP_IPI_CMD_ENABLE = 4
SMP_IPI_CMD_TIMER = 5
SMP_IPI_CMD_FINALIZE = 6
SMP_IPI_CMD_HALT = 7
SMP_IPI_CMD_OBSERVE_ACK = 8

SMP_IPI_STAGE_READY = 1
SMP_IPI_STAGE_SENT = 2
SMP_IPI_STAGE_MASKED = 3
SMP_IPI_STAGE_ENABLED = 4
SMP_IPI_STAGE_TIMER = 5
SMP_IPI_STAGE_FINAL = 6
SMP_IPI_STAGE_ACKED = 7
SMP_IPI_STAGE_FAILURE = 0xdead

SMP_IPI_SHARED_IRQ = 4
SMP_IPI_HANDLER0 = 0x2c00
SMP_IPI_HANDLER1 = 0x3000
SMP_IPI_SENTINELS = (0x680, 0x681)


@dataclasses.dataclass(frozen=True)
class MMIXSMPIPITest:
    name: str
    image: bytes
    main_end: int
    thread_mode: str = TCG_THREAD_MULTI
    cpu_count: int = 2

    mailbox_base = SMP_IPI_MAILBOX_BASE
    mailbox_slot_size = SMP_IPI_MAILBOX_SLOT_SIZE
    command_offset = SMP_IPI_COMMAND
    stage_offset = SMP_IPI_STAGE
    send_mask_offset = SMP_IPI_SEND_MASK
    generation_offset = SMP_IPI_GENERATION
    payload_offset = SMP_IPI_PAYLOAD
    ack_offset = SMP_IPI_ACK
    handler_count_offset = SMP_IPI_HANDLER_COUNT
    handler_done_offset = SMP_IPI_HANDLER_DONE
    ipi_count_offset = SMP_IPI_IPI_COUNT
    shared_count_offset = SMP_IPI_SHARED_COUNT
    timer_count_offset = SMP_IPI_TIMER_COUNT
    shared_release_offset = SMP_IPI_SHARED_RELEASE
    target_id_offset = SMP_IPI_TARGET_ID
    ipi_status_offset = SMP_IPI_STATUS_ENTRY
    claim_offset = SMP_IPI_CLAIM
    handler_rq_offset = SMP_IPI_HANDLER_RQ
    handler_rk_offset = SMP_IPI_HANDLER_RK
    rww_offset = SMP_IPI_RWW
    rxx_offset = SMP_IPI_RXX
    ryy_offset = SMP_IPI_RYY
    rzz_offset = SMP_IPI_RZZ
    rbb_offset = SMP_IPI_RBB
    ro_entry_offset = SMP_IPI_RO_ENTRY
    rs_entry_offset = SMP_IPI_RS_ENTRY
    ro_final_offset = SMP_IPI_RO_FINAL
    rs_final_offset = SMP_IPI_RS_FINAL
    rq_final_offset = SMP_IPI_RQ_FINAL
    rk_final_offset = SMP_IPI_RK_FINAL
    sentinel_final_offset = SMP_IPI_SENTINEL_FINAL
    progress_offset = SMP_IPI_PROGRESS
    sender_ack_offset = SMP_IPI_SENDER_ACK
    failure_offset = SMP_IPI_FAILURE
    command_send = SMP_IPI_CMD_SEND
    command_send_twice = SMP_IPI_CMD_SEND_TWICE
    command_mask = SMP_IPI_CMD_MASK
    command_enable = SMP_IPI_CMD_ENABLE
    command_timer = SMP_IPI_CMD_TIMER
    command_finalize = SMP_IPI_CMD_FINALIZE
    command_halt = SMP_IPI_CMD_HALT
    command_observe_ack = SMP_IPI_CMD_OBSERVE_ACK
    stage_ready = SMP_IPI_STAGE_READY
    stage_sent = SMP_IPI_STAGE_SENT
    stage_masked = SMP_IPI_STAGE_MASKED
    stage_enabled = SMP_IPI_STAGE_ENABLED
    stage_timer = SMP_IPI_STAGE_TIMER
    stage_final = SMP_IPI_STAGE_FINAL
    stage_acked = SMP_IPI_STAGE_ACKED
    stage_failure = SMP_IPI_STAGE_FAILURE
    ipi_request = RQ_IPI
    interrupt_request = RQ_INTERRUPT_CONTROLLER
    request_mask = RK_IPI | RK_INTERRUPT_CONTROLLER
    dynamic_trap_resume_next = DYNAMIC_TRAP_RESUME_NEXT
    shared_irq = SMP_IPI_SHARED_IRQ
    ipi_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0]
    ipi_send = MMIX_VIRT_IPI_SEND
    ipi_context_base = MMIX_VIRT_IPI_CONTEXT_BASE
    ipi_context_stride = MMIX_VIRT_IPI_CONTEXT_STRIDE
    ipi_context_status = MMIX_VIRT_IPI_CONTEXT_STATUS
    ipi_status_pending = MMIX_VIRT_IPI_STATUS_PENDING
    timer_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0]
    timer_context_base = MMIX_VIRT_TIMER_CONTEXT_BASE
    timer_context_stride = MMIX_VIRT_TIMER_CONTEXT_STRIDE
    timer_status = MMIX_VIRT_TIMER_CONTEXT_STATUS
    intc_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0]
    intc_context_base = MMIX_VIRT_INTC_CONTEXT_BASE
    intc_context_stride = MMIX_VIRT_INTC_CONTEXT_STRIDE
    intc_enable = MMIX_VIRT_INTC_CONTEXT_ENABLE
    timer_irq_base = MMIX_VIRT_TIMER_IRQ_BASE
    sentinels = SMP_IPI_SENTINELS
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
    return SMP_IPI_MAILBOX_BASE + cpu * SMP_IPI_MAILBOX_SLOT_SIZE


def _ipi_context_address(cpu, register):
    return (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
        MMIX_VIRT_IPI_CONTEXT_BASE +
        cpu * MMIX_VIRT_IPI_CONTEXT_STRIDE +
        register
    )


def _timer_context_address(cpu):
    return (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0] +
        MMIX_VIRT_TIMER_CONTEXT_BASE +
        cpu * MMIX_VIRT_TIMER_CONTEXT_STRIDE
    )


def _intc_context_address(cpu, register):
    return (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0] +
        MMIX_VIRT_INTC_CONTEXT_BASE +
        cpu * MMIX_VIRT_INTC_CONTEXT_STRIDE +
        register
    )


def _emit_mailbox_address(dst, cpu_id, scratch):
    return [
        *set_octa(dst, SMP_IPI_MAILBOX_BASE),
        insn(SLUI, scratch, cpu_id, 8),
        insn(ADDU, dst, dst, scratch),
    ]


def _emit_stage(program, stage):
    program.emit(
        wyde(SETL, R83, stage),
        insn(SYNC, 0, 0, 1),
        smp_store(R83, R40, SMP_IPI_STAGE),
        *set_octa(R82, SMP_WAIT_LIMIT),
    )
    program.emit_branch(BZ, R254, "command_loop")


def _ipi_handler(cpu, handler_address):
    mailbox = _mailbox_address(cpu)
    status = _ipi_context_address(cpu, MMIX_VIRT_IPI_CONTEXT_STATUS)
    clear = _ipi_context_address(cpu, MMIX_VIRT_IPI_CONTEXT_CLEAR)
    claim = _intc_context_address(cpu, MMIX_VIRT_INTC_CONTEXT_CLAIM)
    complete = _intc_context_address(cpu, MMIX_VIRT_INTC_CONTEXT_COMPLETE)
    timer = _timer_context_address(cpu)
    timer_irq = MMIX_VIRT_TIMER_IRQ_BASE + cpu
    program = SMPProgram()

    program.emit(
        *set_octa(R180, mailbox),
        *set_octa(R181, status),
        *set_octa(R182, clear),
        *set_octa(R183, claim),
        *set_octa(R184, complete),
        *set_octa(R185, timer),
        *set_octa(R186, RQ_IPI),
        *set_octa(R187, RQ_INTERRUPT_CONTROLLER),
        wyde(SETL, R188, timer_irq),
        wyde(SETL, R189, SMP_IPI_SHARED_IRQ),
        insn(GET, R199, 0, SR_Q),
        insn(GET, R190, 0, SR_K),
        insn(GET, R191, 0, SR_WW),
        insn(GET, R192, 0, SR_XX),
        insn(GET, R193, 0, SR_YY),
        insn(GET, R194, 0, SR_ZZ),
        insn(GET, R195, 0, SR_BB),
        insn(GET, R196, 0, SR_O),
        insn(GET, R197, 0, SR_S),
        smp_store(R199, R180, SMP_IPI_HANDLER_RQ),
        smp_store(R190, R180, SMP_IPI_HANDLER_RK),
        smp_store(R191, R180, SMP_IPI_RWW),
        smp_store(R192, R180, SMP_IPI_RXX),
        smp_store(R193, R180, SMP_IPI_RYY),
        smp_store(R194, R180, SMP_IPI_RZZ),
        smp_store(R195, R180, SMP_IPI_RBB),
        smp_store(R196, R180, SMP_IPI_RO_ENTRY),
        smp_store(R197, R180, SMP_IPI_RS_ENTRY),
        wyde(SETL, R198, cpu),
        smp_store(R198, R180, SMP_IPI_TARGET_ID),
        insn(LDOUI, R198, R180, SMP_IPI_HANDLER_COUNT),
        insn(ADDUI, R198, R198, 1),
        smp_store(R198, R180, SMP_IPI_HANDLER_COUNT),
        insn(AND, R200, R199, R186),
    )
    program.emit_branch(BZ, R200, "check_controller")
    program.emit(
        insn(LDOUI, R201, R181, 0),
        smp_store(R201, R180, SMP_IPI_STATUS_ENTRY),
        wyde(SETL, R209, 1),
        insn(CMPUI, R202, R201, MMIX_VIRT_IPI_STATUS_PENDING),
    )
    program.emit_branch(BNZ, R202, "failure")
    program.emit(
        insn(SYNC, 0, 0, 2),
        smp_load(R203, R180, SMP_IPI_PAYLOAD),
        insn(LDOUI, R204, R180, SMP_IPI_IPI_COUNT),
        insn(ADDUI, R204, R204, 1),
        smp_store(R204, R180, SMP_IPI_IPI_COUNT),
        wyde(SETL, R205, MMIX_VIRT_IPI_STATUS_PENDING),
        insn(STOUI, R205, R182, 0),
        insn(SYNC, 0, 0, 1),
        smp_store(R203, R180, SMP_IPI_ACK),
    )

    program.mark("check_controller")
    program.emit(insn(AND, R200, R199, R187))
    program.emit_branch(BZ, R200, "resume")
    program.emit(
        insn(LDTUI, R201, R183, 0),
        smp_store(R201, R180, SMP_IPI_CLAIM),
        wyde(SETL, R209, 2),
        insn(CMPU, R202, R201, R188),
    )
    program.emit_branch(BZ, R202, "timer")
    program.emit(insn(CMPU, R202, R201, R189))
    program.emit_branch(BNZ, R202, "failure")
    program.emit(
        insn(LDOUI, R203, R180, SMP_IPI_SHARED_COUNT),
        insn(ADDUI, R203, R203, 1),
        smp_store(R203, R180, SMP_IPI_SHARED_COUNT),
        insn(SYNC, 0, 0, 1),
        *set_octa(R204, SMP_WAIT_LIMIT),
    )
    program.mark("wait_shared_release")
    program.emit(
        smp_load(R205, R180, SMP_IPI_SHARED_RELEASE),
        insn(CMPU, R206, R205, R203),
    )
    program.emit_branch(BZ, R206, "complete_controller")
    program.emit(insn(SUBUI, R204, R204, 1))
    program.emit_branch(BNZ, R204, "wait_shared_release")
    program.emit(wyde(SETL, R209, 3))
    program.emit_branch(BZ, R254, "failure")

    program.mark("timer")
    program.emit(
        insn(LDOUI, R203, R180, SMP_IPI_TIMER_COUNT),
        insn(ADDUI, R203, R203, 1),
        smp_store(R203, R180, SMP_IPI_TIMER_COUNT),
        smp_store(R254, R185, MMIX_VIRT_TIMER_CONTEXT_CONTROL),
        wyde(SETL, R205, MMIX_VIRT_TIMER_STATUS_PENDING),
        smp_store(R205, R185, MMIX_VIRT_TIMER_CONTEXT_STATUS),
    )

    program.mark("complete_controller")
    program.emit(insn(STTUI, R201, R184, 0))

    program.mark("resume")
    program.emit(
        insn(PUTI, SR_Q, 0, 0),
        insn(SYNC, 0, 0, 1),
        smp_store(R198, R180, SMP_IPI_HANDLER_DONE),
        *set_octa(R255, RK_IPI | RK_INTERRUPT_CONTROLLER),
        insn(RESUME, 0, 0, 1),
    )

    program.mark("failure")
    program.emit(
        smp_store(R209, R180, SMP_IPI_FAILURE),
        wyde(SETL, R203, SMP_IPI_STAGE_FAILURE),
        smp_store(R203, R180, SMP_IPI_STAGE),
    )
    program.mark("failure_idle")
    program.emit_branch(BZ, R254, "failure_idle")
    return handler_address, program.build()


def smp_ipi_lifecycle_program():
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        *_emit_mailbox_address(R40, R32, R41),
        *set_octa(R60, MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
                  MMIX_VIRT_IPI_SEND),
        *set_octa(R90, _mailbox_address(0) + SMP_IPI_PAYLOAD),
        *set_octa(R91, _mailbox_address(1) + SMP_IPI_PAYLOAD),
    )
    program.emit_branch(BZ, R32, "cpu0_setup")
    program.emit(
        *set_octa(R61, _ipi_context_address(
            1, MMIX_VIRT_IPI_CONTEXT_STATUS)),
        *set_octa(R62, _ipi_context_address(
            1, MMIX_VIRT_IPI_CONTEXT_CLEAR)),
        *set_octa(R63, _timer_context_address(1)),
        *set_octa(R64, _intc_context_address(
            1, MMIX_VIRT_INTC_CONTEXT_ENABLE)),
        *set_octa(R65, SMP_IPI_HANDLER1),
        *set_octa(R67, (1 << SMP_IPI_SHARED_IRQ) |
                  (1 << (MMIX_VIRT_TIMER_IRQ_BASE + 1))),
        *set_octa(R100, SMP_IPI_SENTINELS[1]),
    )
    program.emit_branch(BZ, R254, "setup_complete")

    program.mark("cpu0_setup")
    program.emit(
        *set_octa(R61, _ipi_context_address(
            0, MMIX_VIRT_IPI_CONTEXT_STATUS)),
        *set_octa(R62, _ipi_context_address(
            0, MMIX_VIRT_IPI_CONTEXT_CLEAR)),
        *set_octa(R63, _timer_context_address(0)),
        *set_octa(R64, _intc_context_address(
            0, MMIX_VIRT_INTC_CONTEXT_ENABLE)),
        *set_octa(R65, SMP_IPI_HANDLER0),
        *set_octa(R67, (1 << SMP_IPI_SHARED_IRQ) |
                  (1 << MMIX_VIRT_TIMER_IRQ_BASE)),
        *set_octa(R100, SMP_IPI_SENTINELS[0]),
    )

    program.mark("setup_complete")
    program.emit(
        *set_octa(R66, RK_IPI | RK_INTERRUPT_CONTROLLER),
        insn(STTUI, R67, R64, 0),
        insn(PUT, SR_TT, 0, R65),
        insn(PUT, SR_K, 0, R66),
        insn(ADDU, R255, R100, R254),
        wyde(SETL, R83, SMP_IPI_STAGE_READY),
        insn(SYNC, 0, 0, 1),
        smp_store(R83, R40, SMP_IPI_STAGE),
        *set_octa(R82, SMP_WAIT_LIMIT),
    )

    program.mark("command_loop")
    program.emit(
        smp_load(R80, R40, SMP_IPI_COMMAND),
        smp_load(R81, R40, SMP_IPI_PROGRESS),
        insn(ADDUI, R81, R81, 1),
        smp_store(R81, R40, SMP_IPI_PROGRESS),
    )
    program.emit_branch(BNZ, R80, "dispatch_command")
    program.emit(insn(SUBUI, R82, R82, 1))
    program.emit_branch(BNZ, R82, "command_loop")
    program.emit_branch(BZ, R254, "failure")

    program.mark("dispatch_command")
    program.emit(insn(SYNC, 0, 0, 2))
    for command, label in (
        (SMP_IPI_CMD_SEND, "send"),
        (SMP_IPI_CMD_SEND_TWICE, "send_twice"),
        (SMP_IPI_CMD_MASK, "mask"),
        (SMP_IPI_CMD_ENABLE, "enable"),
        (SMP_IPI_CMD_TIMER, "timer"),
        (SMP_IPI_CMD_FINALIZE, "finalize"),
        (SMP_IPI_CMD_HALT, "halt"),
        (SMP_IPI_CMD_OBSERVE_ACK, "observe_ack"),
    ):
        program.emit(insn(CMPUI, R81, R80, command))
        program.emit_branch(BZ, R81, label)
    program.emit_branch(BZ, R254, "failure")

    program.mark("send")
    program.emit(wyde(SETL, R87, 0))
    program.emit_branch(BZ, R254, "publish")
    program.mark("send_twice")
    program.emit(wyde(SETL, R87, 1))
    program.mark("publish")
    program.emit(
        smp_load(R84, R40, SMP_IPI_GENERATION),
        smp_load(R85, R40, SMP_IPI_SEND_MASK),
        insn(ANDI, R86, R85, 1),
    )
    program.emit_branch(BZ, R86, "publish_cpu1")
    program.emit(insn(STOUI, R84, R90, 0))
    program.mark("publish_cpu1")
    program.emit(insn(ANDI, R86, R85, 2))
    program.emit_branch(BZ, R86, "send_ipi")
    program.emit(insn(STOUI, R84, R91, 0))
    program.mark("send_ipi")
    program.emit(
        insn(SYNC, 0, 0, 1),
        insn(STOUI, R85, R60, 0),
    )
    program.emit_branch(BZ, R87, "send_done")
    program.emit(insn(STOUI, R85, R60, 0))
    program.mark("send_done")
    program.emit(smp_store(R254, R40, SMP_IPI_COMMAND))
    _emit_stage(program, SMP_IPI_STAGE_SENT)

    program.mark("observe_ack")
    program.emit(
        insn(SYNC, 0, 0, 2),
        smp_load(R84, R40, SMP_IPI_GENERATION),
        smp_load(R85, R40, SMP_IPI_SEND_MASK),
        insn(ANDI, R86, R85, 1),
    )
    program.emit_branch(BZ, R86, "observe_cpu1")
    program.emit(
        insn(LDOUI, R87, R90, SMP_IPI_ACK - SMP_IPI_PAYLOAD),
        insn(CMPU, R88, R87, R84),
    )
    program.emit_branch(BNZ, R88, "failure")
    program.mark("observe_cpu1")
    program.emit(insn(ANDI, R86, R85, 2))
    program.emit_branch(BZ, R86, "observe_done")
    program.emit(
        insn(LDOUI, R87, R91, SMP_IPI_ACK - SMP_IPI_PAYLOAD),
        insn(CMPU, R88, R87, R84),
    )
    program.emit_branch(BNZ, R88, "failure")
    program.mark("observe_done")
    program.emit(
        smp_store(R84, R40, SMP_IPI_SENDER_ACK),
        smp_store(R254, R40, SMP_IPI_COMMAND),
    )
    _emit_stage(program, SMP_IPI_STAGE_ACKED)

    program.mark("mask")
    program.emit(
        smp_store(R254, R40, SMP_IPI_COMMAND),
        *set_octa(R84, RK_INTERRUPT_CONTROLLER),
        insn(PUT, SR_K, 0, R84),
    )
    _emit_stage(program, SMP_IPI_STAGE_MASKED)

    program.mark("enable")
    program.emit(
        smp_store(R254, R40, SMP_IPI_COMMAND),
        wyde(SETL, R83, SMP_IPI_STAGE_ENABLED),
        insn(SYNC, 0, 0, 1),
        smp_store(R83, R40, SMP_IPI_STAGE),
        insn(PUT, SR_K, 0, R66),
        *set_octa(R82, SMP_WAIT_LIMIT),
    )
    program.emit_branch(BZ, R254, "command_loop")

    program.mark("timer")
    program.emit(
        smp_store(R254, R40, SMP_IPI_COMMAND),
        smp_store(R254, R63, MMIX_VIRT_TIMER_CONTEXT_COMPARE),
        wyde(SETL, R84, MMIX_VIRT_TIMER_CONTROL_ENABLE |
             MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE),
        smp_store(R84, R63, MMIX_VIRT_TIMER_CONTEXT_CONTROL),
    )
    _emit_stage(program, SMP_IPI_STAGE_TIMER)

    program.mark("finalize")
    program.emit(
        smp_store(R254, R40, SMP_IPI_COMMAND),
        insn(GET, R84, 0, SR_O),
        insn(GET, R85, 0, SR_S),
        insn(GET, R86, 0, SR_Q),
        insn(GET, R87, 0, SR_K),
        smp_store(R84, R40, SMP_IPI_RO_FINAL),
        smp_store(R85, R40, SMP_IPI_RS_FINAL),
        smp_store(R86, R40, SMP_IPI_RQ_FINAL),
        smp_store(R87, R40, SMP_IPI_RK_FINAL),
        smp_store(R100, R40, SMP_IPI_SENTINEL_FINAL),
    )
    _emit_stage(program, SMP_IPI_STAGE_FINAL)

    program.mark("halt")
    program.emit(
        insn(ADDU, R255, R254, R254),
        halt(),
    )

    program.mark("failure")
    program.emit(
        wyde(SETL, R83, SMP_IPI_STAGE_FAILURE),
        smp_store(R83, R40, SMP_IPI_STAGE),
    )
    program.mark("failure_idle")
    program.emit_branch(BZ, R254, "failure_idle")

    main = program.build()
    handlers = (
        _ipi_handler(0, SMP_IPI_HANDLER0),
        _ipi_handler(1, SMP_IPI_HANDLER1),
    )
    return MMIXSMPIPITest(
        name="smp-multi-thread-ipi-lifecycle",
        image=smp_elf_image(main, *handlers),
        main_end=SMP_ENTRY + len(main),
    )


SMP_IPI_TESTS = [smp_ipi_lifecycle_program()]
