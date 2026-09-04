#!/usr/bin/env python3
#
# MMIX per-CPU timer lifecycle test cases
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


SMP_TIMER_MAILBOX_BASE = 0x00200800
SMP_TIMER_MAILBOX_SLOT_SIZE = 0x100

SMP_TIMER_COMMAND = 0x00
SMP_TIMER_STAGE = 0x08
SMP_TIMER_DEADLINE = 0x10
SMP_TIMER_HANDLER_COUNT = 0x18
SMP_TIMER_HANDLER_DONE = 0x20
SMP_TIMER_CLAIM = 0x28
SMP_TIMER_STATUS_ENTRY = 0x30
SMP_TIMER_RWW = 0x38
SMP_TIMER_RXX = 0x40
SMP_TIMER_RYY = 0x48
SMP_TIMER_RZZ = 0x50
SMP_TIMER_RBB = 0x58
SMP_TIMER_RO_ENTRY = 0x60
SMP_TIMER_RS_ENTRY = 0x68
SMP_TIMER_HANDLER_RQ = 0x70
SMP_TIMER_RO_FINAL = 0x78
SMP_TIMER_RS_FINAL = 0x80
SMP_TIMER_RQ_FINAL = 0x88
SMP_TIMER_RK_FINAL = 0x90
SMP_TIMER_SENTINEL_FINAL = 0x98

SMP_TIMER_CMD_PROGRAM = 1
SMP_TIMER_CMD_ENABLE = 2
SMP_TIMER_CMD_SNAPSHOT = 3
SMP_TIMER_CMD_FINALIZE = 4
SMP_TIMER_CMD_HALT = 5

SMP_TIMER_STAGE_READY = 1
SMP_TIMER_STAGE_PROGRAMMED = 2
SMP_TIMER_STAGE_ENABLED = 3
SMP_TIMER_STAGE_RESUMED = 4
SMP_TIMER_STAGE_FINAL = 5
SMP_TIMER_STAGE_FAILURE = 0xdead

SMP_TIMER_HANDLER0 = 0x2400
SMP_TIMER_HANDLER1 = 0x2600
SMP_TIMER_SENTINELS = (0x650, 0x651)


@dataclasses.dataclass(frozen=True)
class MMIXSMPTimerTest:
    cpu_id_stack_phys = SMP_CPU_ID_STACK_PHYS
    name: str
    image: bytes
    main_end: int
    thread_mode: str = TCG_THREAD_MULTI
    cpu_count: int = 2

    mailbox_base = SMP_TIMER_MAILBOX_BASE
    mailbox_slot_size = SMP_TIMER_MAILBOX_SLOT_SIZE
    command_offset = SMP_TIMER_COMMAND
    stage_offset = SMP_TIMER_STAGE
    deadline_offset = SMP_TIMER_DEADLINE
    handler_count_offset = SMP_TIMER_HANDLER_COUNT
    handler_done_offset = SMP_TIMER_HANDLER_DONE
    claim_offset = SMP_TIMER_CLAIM
    timer_status_offset = SMP_TIMER_STATUS_ENTRY
    rww_offset = SMP_TIMER_RWW
    rxx_offset = SMP_TIMER_RXX
    ryy_offset = SMP_TIMER_RYY
    rzz_offset = SMP_TIMER_RZZ
    rbb_offset = SMP_TIMER_RBB
    ro_entry_offset = SMP_TIMER_RO_ENTRY
    rs_entry_offset = SMP_TIMER_RS_ENTRY
    handler_rq_offset = SMP_TIMER_HANDLER_RQ
    ro_final_offset = SMP_TIMER_RO_FINAL
    rs_final_offset = SMP_TIMER_RS_FINAL
    rq_final_offset = SMP_TIMER_RQ_FINAL
    rk_final_offset = SMP_TIMER_RK_FINAL
    sentinel_final_offset = SMP_TIMER_SENTINEL_FINAL
    command_program = SMP_TIMER_CMD_PROGRAM
    command_enable = SMP_TIMER_CMD_ENABLE
    command_snapshot = SMP_TIMER_CMD_SNAPSHOT
    command_finalize = SMP_TIMER_CMD_FINALIZE
    command_halt = SMP_TIMER_CMD_HALT
    stage_ready = SMP_TIMER_STAGE_READY
    stage_programmed = SMP_TIMER_STAGE_PROGRAMMED
    stage_enabled = SMP_TIMER_STAGE_ENABLED
    stage_resumed = SMP_TIMER_STAGE_RESUMED
    stage_final = SMP_TIMER_STAGE_FINAL
    stage_failure = SMP_TIMER_STAGE_FAILURE
    interrupt_request = RQ_INTERRUPT_CONTROLLER
    dynamic_trap_resume_next = DYNAMIC_TRAP_RESUME_NEXT
    timer_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0]
    intc_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0]
    timer_irq_base = MMIX_VIRT_TIMER_IRQ_BASE
    timer_time = MMIX_VIRT_TIMER_TIME
    timer_context_status = MMIX_VIRT_TIMER_CONTEXT_STATUS
    timer_status_pending = MMIX_VIRT_TIMER_STATUS_PENDING
    sentinels = SMP_TIMER_SENTINELS
    main_start = SMP_ENTRY
    initial_stack = INITIAL_STACK
    initial_stack_slot_size = MMIX_VIRT_INITIAL_STACK_SLOT_SIZE

    @property
    def qemu_args(self):
        return (
            "-smp", str(self.cpu_count),
            "-accel", f"tcg,thread={self.thread_mode}",
        )

    def timer_context_address(self, cpu, register):
        return (
            self.timer_base + MMIX_VIRT_TIMER_CONTEXT_BASE +
            cpu * MMIX_VIRT_TIMER_CONTEXT_STRIDE + register
        )


def _mailbox_address(cpu):
    return SMP_TIMER_MAILBOX_BASE + cpu * SMP_TIMER_MAILBOX_SLOT_SIZE


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
        *set_octa(dst, SMP_TIMER_MAILBOX_BASE),
        insn(SLUI, scratch, cpu_id, 8),
        insn(ADDU, dst, dst, scratch),
    ]


def _emit_return_to_command_loop(program, stage):
    program.emit(
        wyde(SETL, R83, stage),
        insn(SYNC, 0, 0, 1),
        smp_store(R83, R40, SMP_TIMER_STAGE),
        *set_octa(R82, SMP_WAIT_LIMIT),
    )
    program.emit_branch(BZ, R254, "command_loop")


def _timer_handler(cpu, handler_address):
    mailbox = _mailbox_address(cpu)
    timer = _timer_context_address(cpu)
    claim = _intc_context_address(cpu, MMIX_VIRT_INTC_CONTEXT_CLAIM)
    complete = _intc_context_address(cpu, MMIX_VIRT_INTC_CONTEXT_COMPLETE)
    irq = MMIX_VIRT_TIMER_IRQ_BASE + cpu
    program = SMPProgram()

    program.emit(
        *set_octa(R180, mailbox),
        *set_octa(R181, timer),
        *set_octa(R182, claim),
        *set_octa(R183, complete),
        wyde(SETL, R184, irq),
        insn(GET, R199, 0, SR_Q),
        insn(GET, R185, 0, SR_WW),
        insn(GET, R186, 0, SR_XX),
        insn(GET, R187, 0, SR_YY),
        insn(GET, R188, 0, SR_ZZ),
        insn(GET, R189, 0, SR_BB),
        insn(GET, R190, 0, SR_O),
        insn(GET, R191, 0, SR_S),
        smp_store(R185, R180, SMP_TIMER_RWW),
        smp_store(R186, R180, SMP_TIMER_RXX),
        smp_store(R187, R180, SMP_TIMER_RYY),
        smp_store(R188, R180, SMP_TIMER_RZZ),
        smp_store(R189, R180, SMP_TIMER_RBB),
        smp_store(R190, R180, SMP_TIMER_RO_ENTRY),
        smp_store(R191, R180, SMP_TIMER_RS_ENTRY),
        smp_store(R199, R180, SMP_TIMER_HANDLER_RQ),
        insn(LDOUI, R192, R180, SMP_TIMER_HANDLER_COUNT),
        insn(ADDUI, R192, R192, 1),
        insn(LDOU, R193, R182, 0),
        smp_store(R193, R180, SMP_TIMER_CLAIM),
        insn(LDOUI, R194, R181, MMIX_VIRT_TIMER_CONTEXT_STATUS),
        smp_store(R194, R180, SMP_TIMER_STATUS_ENTRY),
        insn(SYNC, 0, 0, 1),
        smp_store(R192, R180, SMP_TIMER_HANDLER_COUNT),
        wyde(SETL, R195, MMIX_VIRT_TIMER_STATUS_PENDING),
        smp_store(R254, R181, MMIX_VIRT_TIMER_CONTEXT_CONTROL),
        insn(STOUI, R195, R181, MMIX_VIRT_TIMER_CONTEXT_STATUS),
        insn(STOU, R184, R183, 0),
        insn(PUTI, SR_Q, 0, 0),
        insn(SYNC, 0, 0, 1),
        smp_store(R192, R180, SMP_TIMER_HANDLER_DONE),
        *set_octa(R255, RK_INTERRUPT_CONTROLLER),
        insn(RESUME, 0, 0, 1),
    )
    return handler_address, program.build()


def smp_timer_lifecycle_program():
    program = SMPProgram()

    program.emit(
        *smp_cpu_id_from_stack(R32, R33, R34),
        wyde(SETL, R254, 0),
        *_emit_mailbox_address(R40, R32, R41),
    )
    program.emit_branch(BZ, R32, "cpu0_setup")

    program.emit(
        *set_octa(R60, _timer_context_address(1)),
        *set_octa(R61, _intc_context_address(
            1, MMIX_VIRT_INTC_CONTEXT_ENABLE)),
        *set_octa(R62, 1 << (MMIX_VIRT_TIMER_IRQ_BASE + 1)),
        *set_octa(R63, SMP_TIMER_HANDLER1),
        *set_octa(R100, SMP_TIMER_SENTINELS[1]),
    )
    program.emit_branch(BZ, R254, "setup_complete")

    program.mark("cpu0_setup")
    program.emit(
        *set_octa(R60, _timer_context_address(0)),
        *set_octa(R61, _intc_context_address(
            0, MMIX_VIRT_INTC_CONTEXT_ENABLE)),
        *set_octa(R62, 1 << MMIX_VIRT_TIMER_IRQ_BASE),
        *set_octa(R63, SMP_TIMER_HANDLER0),
        *set_octa(R100, SMP_TIMER_SENTINELS[0]),
    )

    program.mark("setup_complete")
    program.emit(
        insn(STOU, R62, R61, 0),
        insn(PUT, SR_TT, 0, R63),
        *set_octa(R70, RK_INTERRUPT_CONTROLLER),
        insn(PUT, SR_K, 0, R70),
        insn(ADDU, R255, R100, R254),
        wyde(SETL, R83, SMP_TIMER_STAGE_READY),
        insn(SYNC, 0, 0, 1),
        smp_store(R83, R40, SMP_TIMER_STAGE),
        *set_octa(R82, SMP_WAIT_LIMIT),
    )

    program.mark("command_loop")
    program.emit(smp_load(R80, R40, SMP_TIMER_COMMAND))
    program.emit_branch(BNZ, R80, "dispatch_command")
    program.emit(insn(SUBUI, R82, R82, 1))
    program.emit_branch(BNZ, R82, "command_loop")
    program.emit_branch(BZ, R254, "failure")

    program.mark("dispatch_command")
    program.emit(insn(SYNC, 0, 0, 2))
    for command, label in (
        (SMP_TIMER_CMD_PROGRAM, "program_timer"),
        (SMP_TIMER_CMD_ENABLE, "enable_timer"),
        (SMP_TIMER_CMD_SNAPSHOT, "snapshot"),
        (SMP_TIMER_CMD_FINALIZE, "finalize"),
        (SMP_TIMER_CMD_HALT, "halt"),
    ):
        program.emit(insn(CMPUI, R81, R80, command))
        program.emit_branch(BZ, R81, label)
    program.emit_branch(BZ, R254, "failure")

    program.mark("program_timer")
    program.emit(
        smp_store(R254, R40, SMP_TIMER_COMMAND),
        smp_load(R84, R40, SMP_TIMER_DEADLINE),
        smp_store(R84, R60, MMIX_VIRT_TIMER_CONTEXT_COMPARE),
        smp_store(R254, R60, MMIX_VIRT_TIMER_CONTEXT_CONTROL),
    )
    _emit_return_to_command_loop(program, SMP_TIMER_STAGE_PROGRAMMED)

    program.mark("enable_timer")
    program.emit(
        smp_store(R254, R40, SMP_TIMER_COMMAND),
        wyde(SETL, R84, MMIX_VIRT_TIMER_CONTROL_ENABLE |
             MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE),
        smp_store(R84, R60, MMIX_VIRT_TIMER_CONTEXT_CONTROL),
    )
    _emit_return_to_command_loop(program, SMP_TIMER_STAGE_ENABLED)

    program.mark("snapshot")
    program.emit(smp_store(R254, R40, SMP_TIMER_COMMAND))
    _emit_return_to_command_loop(program, SMP_TIMER_STAGE_RESUMED)

    program.mark("finalize")
    program.emit(
        smp_store(R254, R40, SMP_TIMER_COMMAND),
        insn(GET, R84, 0, SR_O),
        insn(GET, R85, 0, SR_S),
        insn(GET, R86, 0, SR_Q),
        insn(GET, R87, 0, SR_K),
        smp_store(R84, R40, SMP_TIMER_RO_FINAL),
        smp_store(R85, R40, SMP_TIMER_RS_FINAL),
        smp_store(R86, R40, SMP_TIMER_RQ_FINAL),
        smp_store(R87, R40, SMP_TIMER_RK_FINAL),
        smp_store(R100, R40, SMP_TIMER_SENTINEL_FINAL),
    )
    _emit_return_to_command_loop(program, SMP_TIMER_STAGE_FINAL)

    program.mark("halt")
    program.emit(
        insn(ADDU, R255, R254, R254),
        halt(),
    )

    program.mark("failure")
    program.emit(
        wyde(SETL, R83, SMP_TIMER_STAGE_FAILURE),
        smp_store(R83, R40, SMP_TIMER_STAGE),
    )
    program.mark("failure_idle")
    program.emit_branch(BZ, R254, "failure_idle")

    main = program.build()
    handlers = (
        _timer_handler(0, SMP_TIMER_HANDLER0),
        _timer_handler(1, SMP_TIMER_HANDLER1),
    )
    return MMIXSMPTimerTest(
        name="smp-multi-thread-per-cpu-timer-lifecycle",
        image=smp_elf_image(main, *handlers),
        main_end=SMP_ENTRY + len(main),
    )


SMP_TIMER_TESTS = [smp_timer_lifecycle_program()]
