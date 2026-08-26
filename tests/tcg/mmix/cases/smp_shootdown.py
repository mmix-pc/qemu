#!/usr/bin/env python3
#
# MMIX guest-driven TLB shootdown test cases
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses

from .common import *
from .smp import (
    SMPProgram,
    SMP_ENTRY,
    SMP_WAIT_LIMIT,
    TCG_THREAD_MULTI,
    smp_cswap,
    smp_elf_image,
    smp_emit_unconditional_branch,
    smp_emit_wait_equal,
    smp_load,
    smp_store,
    smp_sync,
)


SMP_SHOOTDOWN_STATE_PHYS = 0x00201000
SMP_SHOOTDOWN_STATE = (1 << 63) | SMP_SHOOTDOWN_STATE_PHYS

SMP_SHOOTDOWN_LOCK = 0x00
SMP_SHOOTDOWN_GENERATION = 0x08
SMP_SHOOTDOWN_TARGET_MASK = 0x10
SMP_SHOOTDOWN_KEY = 0x18
SMP_SHOOTDOWN_OPERATION = 0x20
SMP_SHOOTDOWN_ACK0 = 0x28
SMP_SHOOTDOWN_ACK1 = 0x30
SMP_SHOOTDOWN_RESULT0 = 0x38
SMP_SHOOTDOWN_RESULT1 = 0x40
SMP_SHOOTDOWN_POST0 = 0x48
SMP_SHOOTDOWN_POST1 = 0x50
SMP_SHOOTDOWN_SETUP = 0x58
SMP_SHOOTDOWN_CPU1_READY = 0x60
SMP_SHOOTDOWN_PTE_PUBLISHED = 0x68
SMP_SHOOTDOWN_CPU1_OBSERVED = 0x70
SMP_SHOOTDOWN_OBSERVED_VALUE = 0x78
SMP_SHOOTDOWN_HANDLER_COUNT = 0x80
SMP_SHOOTDOWN_HANDLER_DONE = 0x88
SMP_SHOOTDOWN_HANDLER_RQ = 0x90
SMP_SHOOTDOWN_CPU1_RESUMED = 0x98
SMP_SHOOTDOWN_SENDER_ACK = 0xa0
SMP_SHOOTDOWN_LOCK_RESULT = 0xa8
SMP_SHOOTDOWN_LOCK_BLOCKED = 0xb0
SMP_SHOOTDOWN_LOCK_OBSERVED = 0xb8
SMP_SHOOTDOWN_INVALIDATION_COUNT = 0xc0
SMP_SHOOTDOWN_DUPLICATE_COUNT = 0xc8
SMP_SHOOTDOWN_COMPLETE = 0xd0
SMP_SHOOTDOWN_HALT = 0xd8
SMP_SHOOTDOWN_FAILURE = 0xe0

SMP_SHOOTDOWN_HISTORY_PHYS = SMP_SHOOTDOWN_STATE_PHYS + 0x100
SMP_SHOOTDOWN_HISTORY = (1 << 63) | SMP_SHOOTDOWN_HISTORY_PHYS
SMP_SHOOTDOWN_G1_ACK0 = 0x00
SMP_SHOOTDOWN_G1_ACK1 = 0x08
SMP_SHOOTDOWN_G1_RESULT1 = 0x10
SMP_SHOOTDOWN_G1_POST1 = 0x18
SMP_SHOOTDOWN_G1_STALE = 0x20
SMP_SHOOTDOWN_G2_ACK0 = 0x28
SMP_SHOOTDOWN_G2_ACK1 = 0x30
SMP_SHOOTDOWN_G2_RESULT0 = 0x38
SMP_SHOOTDOWN_G2_POST0 = 0x40
SMP_SHOOTDOWN_G2_REMOTE_BEFORE = 0x48
SMP_SHOOTDOWN_G2_REMOTE_AFTER = 0x50
SMP_SHOOTDOWN_G3_ACK0 = 0x58
SMP_SHOOTDOWN_G3_ACK1 = 0x60
SMP_SHOOTDOWN_G3_RESULT0 = 0x68
SMP_SHOOTDOWN_G3_RESULT1 = 0x70
SMP_SHOOTDOWN_G3_POST0 = 0x78
SMP_SHOOTDOWN_G3_POST1 = 0x80
SMP_SHOOTDOWN_G4_ACK0 = 0x88
SMP_SHOOTDOWN_G4_ACK1 = 0x90
SMP_SHOOTDOWN_G4_RESULT1 = 0x98
SMP_SHOOTDOWN_DUPLICATE_ACK1 = 0xa0
SMP_SHOOTDOWN_FINAL_HANDLER_COUNT = 0xa8
SMP_SHOOTDOWN_FINAL_INVALIDATION_COUNT = 0xb0
SMP_SHOOTDOWN_FINAL_DUPLICATE_COUNT = 0xb8
SMP_SHOOTDOWN_G2_HANDLER_COUNT = 0xc0

SMP_SHOOTDOWN_ROOT = 0x2000
SMP_SHOOTDOWN_PHYS_A = 0x6000
SMP_SHOOTDOWN_PHYS_B = 0x8000
SMP_SHOOTDOWN_VIRTUAL = 0x2000
SMP_SHOOTDOWN_UNCACHED_VIRTUAL = 0x4000
SMP_SHOOTDOWN_VALUE_A = 0x1111222233334444
SMP_SHOOTDOWN_VALUE_B = 0xaaaabbbbccccdddd
SMP_SHOOTDOWN_RV = VM_RV_PAGE0

SMP_SHOOTDOWN_HANDLER0_PHYS = 0xa000
SMP_SHOOTDOWN_HANDLER1_PHYS = 0xa400
SMP_SHOOTDOWN_HANDLER0 = (1 << 63) | SMP_SHOOTDOWN_HANDLER0_PHYS
SMP_SHOOTDOWN_HANDLER1 = (1 << 63) | SMP_SHOOTDOWN_HANDLER1_PHYS
SMP_SHOOTDOWN_SENTINEL = 0x491

SMP_SHOOTDOWN_GENERATION_REMOTE = 1
SMP_SHOOTDOWN_GENERATION_LOCAL = 2
SMP_SHOOTDOWN_GENERATION_ALL = 3
SMP_SHOOTDOWN_GENERATION_EMPTY = 4
SMP_SHOOTDOWN_TARGET_CPU0 = 1 << 0
SMP_SHOOTDOWN_TARGET_CPU1 = 1 << 1
SMP_SHOOTDOWN_TARGET_ALL = SMP_SHOOTDOWN_TARGET_CPU0 | SMP_SHOOTDOWN_TARGET_CPU1
SMP_SHOOTDOWN_OPERATION_DATA = 1
SMP_SHOOTDOWN_OPERATION_INVALIDATE_ONLY = 2
SMP_SHOOTDOWN_LDVTS_DATA = 2


@dataclasses.dataclass(frozen=True)
class MMIXSMPShootdownTest:
    name: str
    image: bytes
    main_end: int
    thread_mode: str = TCG_THREAD_MULTI
    cpu_count: int = 2

    state_base = SMP_SHOOTDOWN_STATE_PHYS
    generation_offset = SMP_SHOOTDOWN_GENERATION
    target_mask_offset = SMP_SHOOTDOWN_TARGET_MASK
    key_offset = SMP_SHOOTDOWN_KEY
    operation_offset = SMP_SHOOTDOWN_OPERATION
    ack0_offset = SMP_SHOOTDOWN_ACK0
    ack1_offset = SMP_SHOOTDOWN_ACK1
    result0_offset = SMP_SHOOTDOWN_RESULT0
    result1_offset = SMP_SHOOTDOWN_RESULT1
    post0_offset = SMP_SHOOTDOWN_POST0
    post1_offset = SMP_SHOOTDOWN_POST1
    cpu1_ready_offset = SMP_SHOOTDOWN_CPU1_READY
    pte_published_offset = SMP_SHOOTDOWN_PTE_PUBLISHED
    cpu1_observed_offset = SMP_SHOOTDOWN_CPU1_OBSERVED
    observed_value_offset = SMP_SHOOTDOWN_OBSERVED_VALUE
    handler_count_offset = SMP_SHOOTDOWN_HANDLER_COUNT
    handler_done_offset = SMP_SHOOTDOWN_HANDLER_DONE
    handler_rq_offset = SMP_SHOOTDOWN_HANDLER_RQ
    cpu1_resumed_offset = SMP_SHOOTDOWN_CPU1_RESUMED
    sender_ack_offset = SMP_SHOOTDOWN_SENDER_ACK
    lock_offset = SMP_SHOOTDOWN_LOCK
    lock_result_offset = SMP_SHOOTDOWN_LOCK_RESULT
    lock_blocked_offset = SMP_SHOOTDOWN_LOCK_BLOCKED
    lock_observed_offset = SMP_SHOOTDOWN_LOCK_OBSERVED
    invalidation_count_offset = SMP_SHOOTDOWN_INVALIDATION_COUNT
    duplicate_count_offset = SMP_SHOOTDOWN_DUPLICATE_COUNT
    complete_offset = SMP_SHOOTDOWN_COMPLETE
    halt_offset = SMP_SHOOTDOWN_HALT
    failure_offset = SMP_SHOOTDOWN_FAILURE
    history_base = SMP_SHOOTDOWN_HISTORY_PHYS
    g1_ack0_offset = SMP_SHOOTDOWN_G1_ACK0
    g1_ack1_offset = SMP_SHOOTDOWN_G1_ACK1
    g1_result1_offset = SMP_SHOOTDOWN_G1_RESULT1
    g1_post1_offset = SMP_SHOOTDOWN_G1_POST1
    g1_stale_offset = SMP_SHOOTDOWN_G1_STALE
    g2_ack0_offset = SMP_SHOOTDOWN_G2_ACK0
    g2_ack1_offset = SMP_SHOOTDOWN_G2_ACK1
    g2_result0_offset = SMP_SHOOTDOWN_G2_RESULT0
    g2_post0_offset = SMP_SHOOTDOWN_G2_POST0
    g2_remote_before_offset = SMP_SHOOTDOWN_G2_REMOTE_BEFORE
    g2_remote_after_offset = SMP_SHOOTDOWN_G2_REMOTE_AFTER
    g3_ack0_offset = SMP_SHOOTDOWN_G3_ACK0
    g3_ack1_offset = SMP_SHOOTDOWN_G3_ACK1
    g3_result0_offset = SMP_SHOOTDOWN_G3_RESULT0
    g3_result1_offset = SMP_SHOOTDOWN_G3_RESULT1
    g3_post0_offset = SMP_SHOOTDOWN_G3_POST0
    g3_post1_offset = SMP_SHOOTDOWN_G3_POST1
    g4_ack0_offset = SMP_SHOOTDOWN_G4_ACK0
    g4_ack1_offset = SMP_SHOOTDOWN_G4_ACK1
    g4_result1_offset = SMP_SHOOTDOWN_G4_RESULT1
    duplicate_ack1_offset = SMP_SHOOTDOWN_DUPLICATE_ACK1
    final_handler_count_offset = SMP_SHOOTDOWN_FINAL_HANDLER_COUNT
    final_invalidation_count_offset = SMP_SHOOTDOWN_FINAL_INVALIDATION_COUNT
    final_duplicate_count_offset = SMP_SHOOTDOWN_FINAL_DUPLICATE_COUNT
    g2_handler_count_offset = SMP_SHOOTDOWN_G2_HANDLER_COUNT
    generation_remote = SMP_SHOOTDOWN_GENERATION_REMOTE
    generation_local = SMP_SHOOTDOWN_GENERATION_LOCAL
    generation_all = SMP_SHOOTDOWN_GENERATION_ALL
    generation_empty = SMP_SHOOTDOWN_GENERATION_EMPTY
    target_cpu1 = SMP_SHOOTDOWN_TARGET_CPU1
    uncached_key = SMP_SHOOTDOWN_UNCACHED_VIRTUAL
    operation_invalidate_only = SMP_SHOOTDOWN_OPERATION_INVALIDATE_ONLY
    ldvts_data = SMP_SHOOTDOWN_LDVTS_DATA
    value_a = SMP_SHOOTDOWN_VALUE_A
    value_b = SMP_SHOOTDOWN_VALUE_B
    ipi_request = RQ_IPI

    @property
    def qemu_args(self):
        return (
            "-smp", str(self.cpu_count),
            "-accel", f"tcg,thread={self.thread_mode}",
        )


def _ipi_context_address(cpu, register):
    return (
        (1 << 63) |
        MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
        MMIX_VIRT_IPI_CONTEXT_BASE +
        cpu * MMIX_VIRT_IPI_CONTEXT_STRIDE +
        register
    )


def _emit_page_table_setup(program):
    program.emit(
        *set_octa(R100, (1 << 63) | SMP_SHOOTDOWN_ROOT),
        wyde(SETL, R101, 7),
        smp_store(R101, R100, 0),
        *set_octa(R102, SMP_SHOOTDOWN_PHYS_A | 7),
        smp_store(R102, R100, 8),
        *set_octa(R103, (1 << 63) | SMP_SHOOTDOWN_PHYS_A),
        *set_octa(R104, SMP_SHOOTDOWN_VALUE_A),
        smp_store(R104, R103, 0),
        *set_octa(R105, (1 << 63) | SMP_SHOOTDOWN_PHYS_B),
        *set_octa(R106, SMP_SHOOTDOWN_VALUE_B),
        smp_store(R106, R105, 0),
    )


def _unexpected_cpu0_handler():
    program = SMPProgram()
    program.emit(
        *set_octa(R180, SMP_SHOOTDOWN_STATE),
        insn(GET, R182, 0, SR_Q),
        smp_store(R182, R180, SMP_SHOOTDOWN_HANDLER_RQ),
        wyde(SETL, R181, 0x10),
        smp_store(R181, R180, SMP_SHOOTDOWN_FAILURE),
    )
    program.mark("failure_idle")
    smp_emit_unconditional_branch(program, "failure_idle")
    return SMP_SHOOTDOWN_HANDLER0_PHYS, program.build()


def _cpu1_shootdown_handler():
    status = _ipi_context_address(1, MMIX_VIRT_IPI_CONTEXT_STATUS)
    clear = _ipi_context_address(1, MMIX_VIRT_IPI_CONTEXT_CLEAR)
    program = SMPProgram()

    program.emit(
        *set_octa(R180, SMP_SHOOTDOWN_STATE),
        *set_octa(R181, status),
        *set_octa(R182, clear),
        *set_octa(R183, RQ_IPI),
        insn(GET, R184, 0, SR_Q),
        smp_store(R184, R180, SMP_SHOOTDOWN_HANDLER_RQ),
        insn(LDOUI, R200, R180, SMP_SHOOTDOWN_HANDLER_COUNT),
        insn(ADDUI, R200, R200, 1),
        smp_store(R200, R180, SMP_SHOOTDOWN_HANDLER_COUNT),
        insn(AND, R185, R184, R183),
    )
    program.emit_branch(BZ, R185, "failure_request")
    program.emit(
        insn(LDOUI, R186, R181, 0),
        insn(CMPUI, R187, R186, MMIX_VIRT_IPI_STATUS_PENDING),
    )
    program.emit_branch(BNZ, R187, "failure_status")
    program.emit(
        smp_sync(2),
        smp_load(R188, R180, SMP_SHOOTDOWN_GENERATION),
        smp_load(R189, R180, SMP_SHOOTDOWN_TARGET_MASK),
        smp_load(R190, R180, SMP_SHOOTDOWN_KEY),
        smp_load(R191, R180, SMP_SHOOTDOWN_OPERATION),
        smp_load(R192, R180, SMP_SHOOTDOWN_ACK1),
    )
    program.emit_branch(BZ, R188, "failure_generation")
    program.emit(
        wyde(SETL, R193, SMP_SHOOTDOWN_TARGET_CPU1),
        insn(AND, R194, R189, R193),
    )
    program.emit_branch(BZ, R194, "failure_target")
    program.emit(insn(CMPU, R194, R192, R188))
    program.emit_branch(BZ, R194, "already_acknowledged")
    program.emit_branch(BP, R194, "failure_generation")
    program.emit(
        wyde(SETL, R193, SMP_SHOOTDOWN_OPERATION_DATA),
        insn(CMPU, R194, R191, R193),
    )
    program.emit_branch(BZ, R194, "operation_valid")
    program.emit(
        wyde(SETL, R193, SMP_SHOOTDOWN_OPERATION_INVALIDATE_ONLY),
        insn(CMPU, R194, R191, R193),
    )
    program.emit_branch(BNZ, R194, "failure_operation")

    program.mark("operation_valid")
    program.emit(
        insn(PUTI, SR_P, 0, 0),
        wyde(SETL, R195, 1),
        smp_cswap(R195, R180, SMP_SHOOTDOWN_LOCK),
    )
    program.emit_branch(BNZ, R195, "failure_lock")
    program.emit(
        insn(GET, R196, 0, SR_P),
        wyde(SETL, R197, 1),
        insn(CMPU, R198, R196, R197),
    )
    program.emit_branch(BNZ, R198, "failure_lock")
    program.emit(
        smp_store(R197, R180, SMP_SHOOTDOWN_LOCK_BLOCKED),
        smp_store(R196, R180, SMP_SHOOTDOWN_LOCK_OBSERVED),
        insn(LDVTS, R195, R190, R254),
        smp_store(R195, R180, SMP_SHOOTDOWN_RESULT1),
        smp_store(R254, R180, SMP_SHOOTDOWN_POST1),
        wyde(SETL, R193, SMP_SHOOTDOWN_OPERATION_DATA),
        insn(CMPU, R194, R191, R193),
    )
    program.emit_branch(BNZ, R194, "record_invalidation")
    program.emit(
        smp_load(R196, R190, 0),
        smp_store(R196, R180, SMP_SHOOTDOWN_POST1),
    )
    program.mark("record_invalidation")
    program.emit(
        insn(LDOUI, R197, R180, SMP_SHOOTDOWN_INVALIDATION_COUNT),
        insn(ADDUI, R197, R197, 1),
        smp_store(R197, R180, SMP_SHOOTDOWN_INVALIDATION_COUNT),
        smp_sync(1),
        smp_store(R188, R180, SMP_SHOOTDOWN_ACK1),
    )
    smp_emit_unconditional_branch(program, "complete_interrupt")

    program.mark("already_acknowledged")
    program.emit(
        insn(LDOUI, R197, R180, SMP_SHOOTDOWN_DUPLICATE_COUNT),
        insn(ADDUI, R197, R197, 1),
        smp_store(R197, R180, SMP_SHOOTDOWN_DUPLICATE_COUNT),
    )

    program.mark("complete_interrupt")
    program.emit(
        wyde(SETL, R198, MMIX_VIRT_IPI_STATUS_PENDING),
        insn(STOUI, R198, R182, 0),
        insn(PUTI, SR_Q, 0, 0),
        smp_sync(1),
        smp_store(R200, R180, SMP_SHOOTDOWN_HANDLER_DONE),
        *set_octa(R255, RK_IPI),
        insn(RESUME, 0, 0, 1),
    )

    for label, reason in (
        ("failure_request", 0x21),
        ("failure_status", 0x22),
        ("failure_generation", 0x23),
        ("failure_target", 0x24),
        ("failure_operation", 0x25),
        ("failure_lock", 0x26),
    ):
        program.mark(label)
        program.emit(wyde(SETL, R199, reason))
        smp_emit_unconditional_branch(program, "failure")

    program.mark("failure")
    program.emit(smp_store(R199, R180, SMP_SHOOTDOWN_FAILURE))
    program.mark("failure_idle")
    smp_emit_unconditional_branch(program, "failure_idle")
    return SMP_SHOOTDOWN_HANDLER1_PHYS, program.build()


def _emit_wait(program, field, expected, label):
    program.emit(
        wyde(SETL, R129, expected),
        *set_octa(R132, SMP_WAIT_LIMIT),
    )
    program.mark(f"{label}_wait")
    program.emit(
        smp_sync(2),
        smp_load(R130, R40, field),
        insn(CMPU, R131, R130, R129),
    )
    program.emit_branch(BZ, R131, f"{label}_done")
    program.emit(insn(SUBUI, R132, R132, 1))
    program.emit_branch(BNZ, R132, f"{label}_wait")
    smp_emit_unconditional_branch(program, "failure_timeout")
    program.mark(f"{label}_done")


def _emit_acquire_lock(program):
    program.emit(
        insn(PUTI, SR_P, 0, 0),
        wyde(SETL, R110, 1),
        smp_cswap(R110, R40, SMP_SHOOTDOWN_LOCK),
        smp_store(R110, R40, SMP_SHOOTDOWN_LOCK_RESULT),
    )
    program.emit_branch(BZ, R110, "failure_lock")
    program.emit(smp_sync(0))


def _emit_publish(program, generation, target, key, operation):
    program.emit(
        wyde(SETL, R111, generation),
        wyde(SETL, R112, target),
        *set_octa(R113, key),
        wyde(SETL, R114, operation),
        smp_store(R254, R40, SMP_SHOOTDOWN_RESULT0),
        smp_store(R254, R40, SMP_SHOOTDOWN_RESULT1),
        smp_store(R254, R40, SMP_SHOOTDOWN_POST0),
        smp_store(R254, R40, SMP_SHOOTDOWN_POST1),
        smp_store(R112, R40, SMP_SHOOTDOWN_TARGET_MASK),
        smp_store(R113, R40, SMP_SHOOTDOWN_KEY),
        smp_store(R114, R40, SMP_SHOOTDOWN_OPERATION),
        smp_sync(1),
        smp_store(R111, R40, SMP_SHOOTDOWN_GENERATION),
        smp_sync(1),
    )


def _emit_local_invalidation(program):
    program.emit(
        insn(LDVTS, R115, R113, R254),
        smp_store(R115, R40, SMP_SHOOTDOWN_RESULT0),
        smp_load(R116, R113, 0),
        smp_store(R116, R40, SMP_SHOOTDOWN_POST0),
        smp_sync(1),
        smp_store(R111, R40, SMP_SHOOTDOWN_ACK0),
    )


def _emit_require_pending_ack(program, ack_field):
    program.emit(
        smp_load(R127, R40, ack_field),
        insn(CMPU, R128, R127, R111),
    )
    program.emit_branch(BZ, R128, "failure_premature_ack")


def _emit_send_ipi(program):
    program.emit(
        *set_octa(R117, (1 << 63) | MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
                  MMIX_VIRT_IPI_SEND),
        wyde(SETL, R119, SMP_SHOOTDOWN_TARGET_CPU1),
        insn(STOUI, R119, R117, 0),
    )


def _emit_release_lock(program):
    program.emit(
        smp_store(R111, R40, SMP_SHOOTDOWN_SENDER_ACK),
        smp_sync(1),
        smp_store(R254, R40, SMP_SHOOTDOWN_LOCK),
    )


def _emit_history(program, state_field, history_field, value=R118):
    program.emit(
        smp_load(value, R40, state_field),
        smp_store(value, R41, history_field),
    )


def smp_remote_data_shootdown_program():
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        *set_octa(R40, SMP_SHOOTDOWN_STATE),
        *set_octa(R41, SMP_SHOOTDOWN_HISTORY),
        wyde(SETL, R42, 1),
        *set_octa(R43, SMP_SHOOTDOWN_VIRTUAL),
        *set_octa(R44, SMP_SHOOTDOWN_RV),
    )
    program.emit_branch(BNZ, R32, "cpu1_setup")
    program.emit(
        *set_octa(R45, SMP_SHOOTDOWN_HANDLER0),
        insn(PUT, SR_TT, 0, R45),
    )
    _emit_page_table_setup(program)
    program.emit(
        smp_sync(1),
        smp_store(R42, R40, SMP_SHOOTDOWN_SETUP),
    )
    smp_emit_unconditional_branch(program, "translation_setup")

    program.mark("cpu1_setup")
    program.emit(
        *set_octa(R45, SMP_SHOOTDOWN_HANDLER1),
        insn(PUT, SR_TT, 0, R45),
        *set_octa(R46, SMP_SHOOTDOWN_SENTINEL),
        insn(ADDU, R255, R46, R254),
    )
    _emit_wait(program, SMP_SHOOTDOWN_SETUP, 1, "page_table_setup")

    program.mark("translation_setup")
    program.emit(insn(PUT, SR_V, 0, R44))
    program.emit_branch(BNZ, R32, "cpu1_generations")

    _emit_wait(program, SMP_SHOOTDOWN_CPU1_READY, 1, "g1_cpu1_ready")
    _emit_acquire_lock(program)
    program.emit(
        *set_octa(R120, (1 << 63) | (SMP_SHOOTDOWN_ROOT + 8)),
        *set_octa(R121, SMP_SHOOTDOWN_PHYS_B | 7),
        smp_store(R121, R120, 0),
        smp_sync(1),
        smp_store(R42, R40, SMP_SHOOTDOWN_PTE_PUBLISHED),
    )
    _emit_wait(program, SMP_SHOOTDOWN_CPU1_OBSERVED, 1,
               "g1_cpu1_stale")
    _emit_publish(
        program, SMP_SHOOTDOWN_GENERATION_REMOTE,
        SMP_SHOOTDOWN_TARGET_CPU1, SMP_SHOOTDOWN_VIRTUAL,
        SMP_SHOOTDOWN_OPERATION_DATA,
    )
    _emit_require_pending_ack(program, SMP_SHOOTDOWN_ACK1)
    _emit_send_ipi(program)
    _emit_wait(program, SMP_SHOOTDOWN_ACK1, 1, "g1_remote_ack")
    program.emit(smp_sync(2))
    for state_field, history_field in (
        (SMP_SHOOTDOWN_ACK0, SMP_SHOOTDOWN_G1_ACK0),
        (SMP_SHOOTDOWN_ACK1, SMP_SHOOTDOWN_G1_ACK1),
        (SMP_SHOOTDOWN_RESULT1, SMP_SHOOTDOWN_G1_RESULT1),
        (SMP_SHOOTDOWN_POST1, SMP_SHOOTDOWN_G1_POST1),
        (SMP_SHOOTDOWN_OBSERVED_VALUE, SMP_SHOOTDOWN_G1_STALE),
    ):
        _emit_history(program, state_field, history_field)
    _emit_release_lock(program)
    _emit_wait(program, SMP_SHOOTDOWN_CPU1_RESUMED, 1, "g1_cpu1_resumed")

    program.emit(smp_load(R122, R43, 0))
    _emit_acquire_lock(program)
    program.emit(
        *set_octa(R121, SMP_SHOOTDOWN_PHYS_A | 7),
        smp_store(R121, R120, 0),
        smp_sync(1),
        wyde(SETL, R123, SMP_SHOOTDOWN_GENERATION_LOCAL),
        smp_store(R123, R40, SMP_SHOOTDOWN_PTE_PUBLISHED),
    )
    _emit_wait(program, SMP_SHOOTDOWN_CPU1_OBSERVED, 2,
               "g2_remote_stale")
    _emit_publish(
        program, SMP_SHOOTDOWN_GENERATION_LOCAL,
        SMP_SHOOTDOWN_TARGET_CPU0, SMP_SHOOTDOWN_VIRTUAL,
        SMP_SHOOTDOWN_OPERATION_DATA,
    )
    _emit_require_pending_ack(program, SMP_SHOOTDOWN_ACK0)
    _emit_local_invalidation(program)
    program.emit(
        wyde(SETL, R124, SMP_SHOOTDOWN_LDVTS_DATA),
        insn(CMPU, R125, R115, R124),
    )
    program.emit_branch(BNZ, R125, "failure_result")
    for state_field, history_field in (
        (SMP_SHOOTDOWN_ACK0, SMP_SHOOTDOWN_G2_ACK0),
        (SMP_SHOOTDOWN_ACK1, SMP_SHOOTDOWN_G2_ACK1),
        (SMP_SHOOTDOWN_RESULT0, SMP_SHOOTDOWN_G2_RESULT0),
        (SMP_SHOOTDOWN_POST0, SMP_SHOOTDOWN_G2_POST0),
        (SMP_SHOOTDOWN_OBSERVED_VALUE, SMP_SHOOTDOWN_G2_REMOTE_BEFORE),
        (SMP_SHOOTDOWN_HANDLER_COUNT, SMP_SHOOTDOWN_G2_HANDLER_COUNT),
    ):
        _emit_history(program, state_field, history_field)
    _emit_release_lock(program)
    _emit_wait(program, SMP_SHOOTDOWN_CPU1_RESUMED, 2, "g2_cpu1_resumed")
    _emit_history(
        program, SMP_SHOOTDOWN_OBSERVED_VALUE,
        SMP_SHOOTDOWN_G2_REMOTE_AFTER,
    )

    _emit_acquire_lock(program)
    program.emit(
        *set_octa(R121, SMP_SHOOTDOWN_PHYS_B | 7),
        smp_store(R121, R120, 0),
        smp_sync(1),
        wyde(SETL, R123, SMP_SHOOTDOWN_GENERATION_ALL),
        smp_store(R123, R40, SMP_SHOOTDOWN_PTE_PUBLISHED),
    )
    _emit_publish(
        program, SMP_SHOOTDOWN_GENERATION_ALL,
        SMP_SHOOTDOWN_TARGET_ALL, SMP_SHOOTDOWN_VIRTUAL,
        SMP_SHOOTDOWN_OPERATION_DATA,
    )
    _emit_require_pending_ack(program, SMP_SHOOTDOWN_ACK0)
    _emit_require_pending_ack(program, SMP_SHOOTDOWN_ACK1)
    _emit_local_invalidation(program)
    _emit_send_ipi(program)
    _emit_wait(program, SMP_SHOOTDOWN_ACK1, 3, "g3_remote_ack")
    program.emit(smp_sync(2))
    for state_field, history_field in (
        (SMP_SHOOTDOWN_ACK0, SMP_SHOOTDOWN_G3_ACK0),
        (SMP_SHOOTDOWN_ACK1, SMP_SHOOTDOWN_G3_ACK1),
        (SMP_SHOOTDOWN_RESULT0, SMP_SHOOTDOWN_G3_RESULT0),
        (SMP_SHOOTDOWN_RESULT1, SMP_SHOOTDOWN_G3_RESULT1),
        (SMP_SHOOTDOWN_POST0, SMP_SHOOTDOWN_G3_POST0),
        (SMP_SHOOTDOWN_POST1, SMP_SHOOTDOWN_G3_POST1),
    ):
        _emit_history(program, state_field, history_field)
    _emit_release_lock(program)
    _emit_wait(program, SMP_SHOOTDOWN_CPU1_RESUMED, 3, "g3_cpu1_resumed")

    _emit_acquire_lock(program)
    _emit_publish(
        program, SMP_SHOOTDOWN_GENERATION_EMPTY,
        SMP_SHOOTDOWN_TARGET_CPU1, SMP_SHOOTDOWN_UNCACHED_VIRTUAL,
        SMP_SHOOTDOWN_OPERATION_INVALIDATE_ONLY,
    )
    _emit_require_pending_ack(program, SMP_SHOOTDOWN_ACK1)
    _emit_send_ipi(program)
    _emit_wait(program, SMP_SHOOTDOWN_ACK1, 4, "g4_remote_ack")
    _emit_wait(program, SMP_SHOOTDOWN_CPU1_RESUMED, 4, "g4_cpu1_resumed")
    program.emit(smp_sync(2))
    for state_field, history_field in (
        (SMP_SHOOTDOWN_ACK0, SMP_SHOOTDOWN_G4_ACK0),
        (SMP_SHOOTDOWN_ACK1, SMP_SHOOTDOWN_G4_ACK1),
        (SMP_SHOOTDOWN_RESULT1, SMP_SHOOTDOWN_G4_RESULT1),
    ):
        _emit_history(program, state_field, history_field)
    _emit_send_ipi(program)
    _emit_wait(program, SMP_SHOOTDOWN_HANDLER_DONE, 4,
               "g4_duplicate_handler")
    _emit_wait(program, SMP_SHOOTDOWN_CPU1_RESUMED, 5,
               "g4_duplicate_resumed")
    program.emit(smp_sync(2))
    for state_field, history_field in (
        (SMP_SHOOTDOWN_ACK1, SMP_SHOOTDOWN_DUPLICATE_ACK1),
        (SMP_SHOOTDOWN_HANDLER_COUNT,
         SMP_SHOOTDOWN_FINAL_HANDLER_COUNT),
        (SMP_SHOOTDOWN_INVALIDATION_COUNT,
         SMP_SHOOTDOWN_FINAL_INVALIDATION_COUNT),
        (SMP_SHOOTDOWN_DUPLICATE_COUNT,
         SMP_SHOOTDOWN_FINAL_DUPLICATE_COUNT),
    ):
        _emit_history(program, state_field, history_field)
    _emit_release_lock(program)
    program.emit(
        smp_sync(1),
        smp_store(R42, R40, SMP_SHOOTDOWN_COMPLETE),
    )
    _emit_wait(program, SMP_SHOOTDOWN_HALT, 1, "host_halt")
    program.emit(
        insn(ADDU, R255, R254, R254),
        halt(),
    )

    program.mark("cpu1_generations")
    program.emit(
        *set_octa(R78, RK_IPI),
        insn(PUT, SR_K, 0, R78),
        smp_load(R79, R43, 0),
        *set_octa(R80, SMP_SHOOTDOWN_VALUE_A),
        insn(CMPU, R81, R79, R80),
    )
    program.emit_branch(BNZ, R81, "failure_prime")
    program.emit(
        smp_sync(1),
        smp_store(R42, R40, SMP_SHOOTDOWN_CPU1_READY),
    )
    _emit_wait(program, SMP_SHOOTDOWN_PTE_PUBLISHED, 1, "g1_pte")
    program.emit(
        smp_load(R82, R43, 0),
        smp_store(R82, R40, SMP_SHOOTDOWN_OBSERVED_VALUE),
        insn(CMPU, R83, R82, R80),
    )
    program.emit_branch(BNZ, R83, "failure_stale")
    program.emit(
        smp_sync(1),
        smp_store(R42, R40, SMP_SHOOTDOWN_CPU1_OBSERVED),
    )
    _emit_wait(program, SMP_SHOOTDOWN_HANDLER_DONE, 1, "g1_handler")
    program.emit(
        smp_sync(1),
        smp_store(R42, R40, SMP_SHOOTDOWN_CPU1_RESUMED),
    )

    _emit_wait(program, SMP_SHOOTDOWN_PTE_PUBLISHED, 2, "g2_pte")
    program.emit(
        smp_load(R84, R43, 0),
        smp_store(R84, R40, SMP_SHOOTDOWN_OBSERVED_VALUE),
        *set_octa(R85, SMP_SHOOTDOWN_VALUE_B),
        insn(CMPU, R86, R84, R85),
    )
    program.emit_branch(BNZ, R86, "failure_stale")
    program.emit(
        wyde(SETL, R87, SMP_SHOOTDOWN_GENERATION_LOCAL),
        smp_sync(1),
        smp_store(R87, R40, SMP_SHOOTDOWN_CPU1_OBSERVED),
    )
    _emit_wait(program, SMP_SHOOTDOWN_SENDER_ACK, 2, "g2_sender_ack")
    program.emit(
        smp_load(R88, R43, 0),
        smp_store(R88, R40, SMP_SHOOTDOWN_OBSERVED_VALUE),
        insn(CMPU, R89, R88, R85),
    )
    program.emit_branch(BNZ, R89, "failure_stale")
    program.emit(
        smp_sync(1),
        smp_store(R87, R40, SMP_SHOOTDOWN_CPU1_RESUMED),
    )

    _emit_wait(program, SMP_SHOOTDOWN_HANDLER_DONE, 2, "g3_handler")
    program.emit(
        wyde(SETL, R90, SMP_SHOOTDOWN_GENERATION_ALL),
        smp_sync(1),
        smp_store(R90, R40, SMP_SHOOTDOWN_CPU1_RESUMED),
    )
    _emit_wait(program, SMP_SHOOTDOWN_HANDLER_DONE, 3, "g4_handler")
    program.emit(
        wyde(SETL, R90, SMP_SHOOTDOWN_GENERATION_EMPTY),
        smp_sync(1),
        smp_store(R90, R40, SMP_SHOOTDOWN_CPU1_RESUMED),
    )
    _emit_wait(program, SMP_SHOOTDOWN_HANDLER_DONE, 4,
               "g4_duplicate_handler_cpu1")
    program.emit(
        wyde(SETL, R90, SMP_SHOOTDOWN_GENERATION_EMPTY + 1),
        smp_sync(1),
        smp_store(R90, R40, SMP_SHOOTDOWN_CPU1_RESUMED),
    )
    program.mark("cpu1_idle")
    smp_emit_unconditional_branch(program, "cpu1_idle")

    for label, reason in (
        ("failure_timeout", 0x31),
        ("failure_lock", 0x32),
        ("failure_result", 0x33),
        ("failure_prime", 0x35),
        ("failure_stale", 0x36),
        ("failure_premature_ack", 0x37),
    ):
        program.mark(label)
        program.emit(wyde(SETL, R126, reason))
        smp_emit_unconditional_branch(program, "failure")

    program.mark("failure")
    program.emit(smp_store(R126, R40, SMP_SHOOTDOWN_FAILURE))
    program.mark("failure_idle")
    smp_emit_unconditional_branch(program, "failure_idle")

    main = program.build()
    handlers = (_unexpected_cpu0_handler(), _cpu1_shootdown_handler())
    return MMIXSMPShootdownTest(
        name="smp-multi-thread-shootdown-generations",
        image=smp_elf_image(main, *handlers),
        main_end=SMP_ENTRY + len(main),
    )


SMP_SHOOTDOWN_TESTS = [smp_remote_data_shootdown_program()]
