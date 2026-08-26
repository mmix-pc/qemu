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
SMP_SHOOTDOWN_SETUP = 0x48
SMP_SHOOTDOWN_CPU1_PRIMED = 0x50
SMP_SHOOTDOWN_PTE_PUBLISHED = 0x58
SMP_SHOOTDOWN_CPU1_STALE = 0x60
SMP_SHOOTDOWN_STALE_VALUE = 0x68
SMP_SHOOTDOWN_POST_VALUE = 0x70
SMP_SHOOTDOWN_HANDLER_COUNT = 0x78
SMP_SHOOTDOWN_HANDLER_DONE = 0x80
SMP_SHOOTDOWN_HANDLER_RQ = 0x88
SMP_SHOOTDOWN_CPU1_RESUMED = 0x90
SMP_SHOOTDOWN_FINAL_RQ = 0x98
SMP_SHOOTDOWN_FINAL_RK = 0xa0
SMP_SHOOTDOWN_FINAL_SENTINEL = 0xa8
SMP_SHOOTDOWN_SENDER_ACK = 0xb0
SMP_SHOOTDOWN_LOCK_RESULT = 0xb8
SMP_SHOOTDOWN_COMPLETE = 0xc0
SMP_SHOOTDOWN_HALT = 0xc8
SMP_SHOOTDOWN_FAILURE = 0xd0

SMP_SHOOTDOWN_ROOT = 0x2000
SMP_SHOOTDOWN_PHYS_A = 0x6000
SMP_SHOOTDOWN_PHYS_B = 0x8000
SMP_SHOOTDOWN_VIRTUAL = 0x2000
SMP_SHOOTDOWN_VALUE_A = 0x1111222233334444
SMP_SHOOTDOWN_VALUE_B = 0xaaaabbbbccccdddd
SMP_SHOOTDOWN_RV = VM_RV_PAGE0

SMP_SHOOTDOWN_HANDLER0_PHYS = 0xa000
SMP_SHOOTDOWN_HANDLER1_PHYS = 0xa400
SMP_SHOOTDOWN_HANDLER0 = (1 << 63) | SMP_SHOOTDOWN_HANDLER0_PHYS
SMP_SHOOTDOWN_HANDLER1 = (1 << 63) | SMP_SHOOTDOWN_HANDLER1_PHYS
SMP_SHOOTDOWN_SENTINEL = 0x491

SMP_SHOOTDOWN_GENERATION_VALUE = 1
SMP_SHOOTDOWN_TARGET_CPU1 = 1 << 1
SMP_SHOOTDOWN_OPERATION_PAGE = 1
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
    cpu1_primed_offset = SMP_SHOOTDOWN_CPU1_PRIMED
    pte_published_offset = SMP_SHOOTDOWN_PTE_PUBLISHED
    cpu1_stale_offset = SMP_SHOOTDOWN_CPU1_STALE
    stale_value_offset = SMP_SHOOTDOWN_STALE_VALUE
    post_value_offset = SMP_SHOOTDOWN_POST_VALUE
    handler_count_offset = SMP_SHOOTDOWN_HANDLER_COUNT
    handler_done_offset = SMP_SHOOTDOWN_HANDLER_DONE
    handler_rq_offset = SMP_SHOOTDOWN_HANDLER_RQ
    cpu1_resumed_offset = SMP_SHOOTDOWN_CPU1_RESUMED
    final_rq_offset = SMP_SHOOTDOWN_FINAL_RQ
    final_rk_offset = SMP_SHOOTDOWN_FINAL_RK
    final_sentinel_offset = SMP_SHOOTDOWN_FINAL_SENTINEL
    sender_ack_offset = SMP_SHOOTDOWN_SENDER_ACK
    lock_offset = SMP_SHOOTDOWN_LOCK
    lock_result_offset = SMP_SHOOTDOWN_LOCK_RESULT
    complete_offset = SMP_SHOOTDOWN_COMPLETE
    halt_offset = SMP_SHOOTDOWN_HALT
    failure_offset = SMP_SHOOTDOWN_FAILURE
    generation = SMP_SHOOTDOWN_GENERATION_VALUE
    target_mask = SMP_SHOOTDOWN_TARGET_CPU1
    key = SMP_SHOOTDOWN_VIRTUAL
    operation = SMP_SHOOTDOWN_OPERATION_PAGE
    ldvts_result = SMP_SHOOTDOWN_LDVTS_DATA
    value_a = SMP_SHOOTDOWN_VALUE_A
    value_b = SMP_SHOOTDOWN_VALUE_B
    ipi_request = RQ_IPI
    request_mask = RK_IPI
    sentinel = SMP_SHOOTDOWN_SENTINEL

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
        wyde(SETL, R193, SMP_SHOOTDOWN_GENERATION_VALUE),
        insn(CMPU, R194, R188, R193),
    )
    program.emit_branch(BNZ, R194, "failure_generation")
    program.emit(
        wyde(SETL, R193, SMP_SHOOTDOWN_TARGET_CPU1),
        insn(CMPU, R194, R189, R193),
    )
    program.emit_branch(BNZ, R194, "failure_target")
    program.emit(
        wyde(SETL, R193, SMP_SHOOTDOWN_OPERATION_PAGE),
        insn(CMPU, R194, R191, R193),
    )
    program.emit_branch(BNZ, R194, "failure_operation")
    program.emit(insn(CMPU, R194, R192, R188))
    program.emit_branch(BZ, R194, "already_acknowledged")
    program.emit(
        insn(LDVTS, R195, R190, R254),
        smp_load(R196, R190, 0),
        smp_store(R195, R180, SMP_SHOOTDOWN_RESULT1),
        smp_store(R196, R180, SMP_SHOOTDOWN_POST_VALUE),
        insn(LDOUI, R197, R180, SMP_SHOOTDOWN_HANDLER_COUNT),
        insn(ADDUI, R197, R197, 1),
        smp_store(R197, R180, SMP_SHOOTDOWN_HANDLER_COUNT),
        smp_sync(1),
        smp_store(R188, R180, SMP_SHOOTDOWN_ACK1),
    )

    program.mark("already_acknowledged")
    program.emit(
        wyde(SETL, R198, MMIX_VIRT_IPI_STATUS_PENDING),
        insn(STOUI, R198, R182, 0),
        insn(PUTI, SR_Q, 0, 0),
        smp_sync(1),
        smp_store(R188, R180, SMP_SHOOTDOWN_HANDLER_DONE),
        *set_octa(R255, RK_IPI),
        insn(RESUME, 0, 0, 1),
    )

    for label, reason in (
        ("failure_request", 0x21),
        ("failure_status", 0x22),
        ("failure_generation", 0x23),
        ("failure_target", 0x24),
        ("failure_operation", 0x25),
    ):
        program.mark(label)
        program.emit(wyde(SETL, R199, reason))
        smp_emit_unconditional_branch(program, "failure")

    program.mark("failure")
    program.emit(smp_store(R199, R180, SMP_SHOOTDOWN_FAILURE))
    program.mark("failure_idle")
    smp_emit_unconditional_branch(program, "failure_idle")
    return SMP_SHOOTDOWN_HANDLER1_PHYS, program.build()


def smp_remote_data_shootdown_program():
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        *set_octa(R40, SMP_SHOOTDOWN_STATE),
        wyde(SETL, R41, 1),
        *set_octa(R42, SMP_SHOOTDOWN_VIRTUAL),
        *set_octa(R43, SMP_SHOOTDOWN_RV),
    )
    program.emit_branch(BNZ, R32, "cpu1_setup")
    program.emit(
        *set_octa(R44, SMP_SHOOTDOWN_HANDLER0),
        insn(PUT, SR_TT, 0, R44),
    )
    _emit_page_table_setup(program)
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, SMP_SHOOTDOWN_SETUP),
    )
    smp_emit_unconditional_branch(program, "translation_setup")

    program.mark("cpu1_setup")
    program.emit(
        *set_octa(R44, SMP_SHOOTDOWN_HANDLER1),
        insn(PUT, SR_TT, 0, R44),
        *set_octa(R45, SMP_SHOOTDOWN_SENTINEL),
        insn(ADDU, R255, R45, R254),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHOOTDOWN_SETUP,
        expected=R41,
        value=R46,
        compare=R47,
        counter=R48,
        label="page_table_setup",
        timeout_label="failure_timeout",
    )

    program.mark("translation_setup")
    program.emit(insn(PUT, SR_V, 0, R43))
    program.emit_branch(BNZ, R32, "cpu1_prime")

    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHOOTDOWN_CPU1_PRIMED,
        expected=R41,
        value=R50,
        compare=R51,
        counter=R52,
        label="cpu1_primed",
        timeout_label="failure_timeout",
    )
    program.emit(
        insn(PUTI, SR_P, 0, 0),
        wyde(SETL, R53, 1),
        smp_cswap(R53, R40, SMP_SHOOTDOWN_LOCK),
        smp_store(R53, R40, SMP_SHOOTDOWN_LOCK_RESULT),
    )
    program.emit_branch(BZ, R53, "failure_lock")
    program.emit(
        *set_octa(R54, (1 << 63) | (SMP_SHOOTDOWN_ROOT + 8)),
        *set_octa(R55, SMP_SHOOTDOWN_PHYS_B | 7),
        smp_store(R55, R54, 0),
        smp_sync(1),
        smp_store(R41, R40, SMP_SHOOTDOWN_PTE_PUBLISHED),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHOOTDOWN_CPU1_STALE,
        expected=R41,
        value=R56,
        compare=R57,
        counter=R58,
        label="cpu1_stale",
        timeout_label="failure_timeout",
    )
    program.emit(
        wyde(SETL, R59, SMP_SHOOTDOWN_TARGET_CPU1),
        smp_store(R59, R40, SMP_SHOOTDOWN_TARGET_MASK),
        smp_store(R42, R40, SMP_SHOOTDOWN_KEY),
        wyde(SETL, R60, SMP_SHOOTDOWN_OPERATION_PAGE),
        smp_store(R60, R40, SMP_SHOOTDOWN_OPERATION),
        smp_sync(1),
        wyde(SETL, R61, SMP_SHOOTDOWN_GENERATION_VALUE),
        smp_store(R61, R40, SMP_SHOOTDOWN_GENERATION),
        smp_sync(1),
        *set_octa(R62, (1 << 63) | MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
                  MMIX_VIRT_IPI_SEND),
        insn(STOUI, R59, R62, 0),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHOOTDOWN_ACK1,
        expected=R61,
        value=R63,
        compare=R64,
        counter=R65,
        label="cpu1_acknowledged",
        timeout_label="failure_timeout",
    )
    program.emit(
        smp_sync(2),
        smp_load(R66, R40, SMP_SHOOTDOWN_RESULT1),
        wyde(SETL, R67, SMP_SHOOTDOWN_LDVTS_DATA),
        insn(CMPU, R68, R66, R67),
    )
    program.emit_branch(BNZ, R68, "failure_result")
    program.emit(
        smp_load(R69, R40, SMP_SHOOTDOWN_POST_VALUE),
        *set_octa(R70, SMP_SHOOTDOWN_VALUE_B),
        insn(CMPU, R71, R69, R70),
    )
    program.emit_branch(BNZ, R71, "failure_post_value")
    program.emit(
        smp_store(R61, R40, SMP_SHOOTDOWN_SENDER_ACK),
        smp_sync(1),
        smp_store(R254, R40, SMP_SHOOTDOWN_LOCK),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHOOTDOWN_CPU1_RESUMED,
        expected=R41,
        value=R72,
        compare=R73,
        counter=R74,
        label="cpu1_resumed",
        timeout_label="failure_timeout",
    )
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, SMP_SHOOTDOWN_COMPLETE),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHOOTDOWN_HALT,
        expected=R41,
        value=R75,
        compare=R76,
        counter=R77,
        label="host_halt",
        timeout_label="failure_timeout",
    )
    program.emit(
        insn(ADDU, R255, R254, R254),
        halt(),
    )

    program.mark("cpu1_prime")
    program.emit(
        *set_octa(R78, RK_IPI),
        insn(PUT, SR_K, 0, R78),
        smp_load(R79, R42, 0),
        *set_octa(R80, SMP_SHOOTDOWN_VALUE_A),
        insn(CMPU, R81, R79, R80),
    )
    program.emit_branch(BNZ, R81, "failure_prime")
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, SMP_SHOOTDOWN_CPU1_PRIMED),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHOOTDOWN_PTE_PUBLISHED,
        expected=R41,
        value=R82,
        compare=R83,
        counter=R84,
        label="pte_published",
        timeout_label="failure_timeout",
    )
    program.emit(
        smp_sync(2),
        smp_load(R85, R42, 0),
        smp_store(R85, R40, SMP_SHOOTDOWN_STALE_VALUE),
        insn(CMPU, R86, R85, R80),
    )
    program.emit_branch(BNZ, R86, "failure_stale")
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, SMP_SHOOTDOWN_CPU1_STALE),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHOOTDOWN_HANDLER_DONE,
        expected=R41,
        value=R87,
        compare=R88,
        counter=R89,
        label="ipi_handler_done",
        timeout_label="failure_timeout",
    )
    program.emit(
        insn(GET, R90, 0, SR_Q),
        insn(GET, R91, 0, SR_K),
        smp_store(R90, R40, SMP_SHOOTDOWN_FINAL_RQ),
        smp_store(R91, R40, SMP_SHOOTDOWN_FINAL_RK),
        smp_store(R45, R40, SMP_SHOOTDOWN_FINAL_SENTINEL),
        smp_sync(1),
        smp_store(R41, R40, SMP_SHOOTDOWN_CPU1_RESUMED),
    )
    program.mark("cpu1_idle")
    smp_emit_unconditional_branch(program, "cpu1_idle")

    for label, reason in (
        ("failure_timeout", 0x31),
        ("failure_lock", 0x32),
        ("failure_result", 0x33),
        ("failure_post_value", 0x34),
        ("failure_prime", 0x35),
        ("failure_stale", 0x36),
    ):
        program.mark(label)
        program.emit(wyde(SETL, R89, reason))
        smp_emit_unconditional_branch(program, "failure")

    program.mark("failure")
    program.emit(smp_store(R89, R40, SMP_SHOOTDOWN_FAILURE))
    program.mark("failure_idle")
    smp_emit_unconditional_branch(program, "failure_idle")

    main = program.build()
    handlers = (_unexpected_cpu0_handler(), _cpu1_shootdown_handler())
    return MMIXSMPShootdownTest(
        name="smp-multi-thread-remote-data-shootdown",
        image=smp_elf_image(main, *handlers),
        main_end=SMP_ENTRY + len(main),
    )


SMP_SHOOTDOWN_TESTS = [smp_remote_data_shootdown_program()]
