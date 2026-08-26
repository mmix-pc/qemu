#!/usr/bin/env python3
#
# MMIX contending guest-driven TLB shootdown test case
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *
from .smp import (
    SMPProgram,
    SMP_ENTRY,
    SMP_WAIT_LIMIT,
    smp_cswap,
    smp_elf_image,
    smp_emit_unconditional_branch,
    smp_load,
    smp_store,
    smp_sync,
)
from .smp_shootdown import MMIXSMPExpectedMemoryTest


STRESS_STATE_PHYS = 0x00201800
STRESS_STATE = (1 << 63) | STRESS_STATE_PHYS
STRESS_LOCK = 0x00
STRESS_GENERATION = 0x08
STRESS_TARGET = 0x10
STRESS_KEY = 0x18
STRESS_ACK0 = 0x20
STRESS_ACK1 = 0x28
STRESS_OWNER = 0x30
STRESS_INIT0 = 0x38
STRESS_INIT1 = 0x40
STRESS_RETRY0 = 0x48
STRESS_RETRY1 = 0x50
STRESS_HANDLER0 = 0x58
STRESS_HANDLER1 = 0x60
STRESS_INVALID0 = 0x68
STRESS_INVALID1 = 0x70
STRESS_RESUME0 = 0x78
STRESS_RESUME1 = 0x80
STRESS_PROGRESS0 = 0x88
STRESS_PROGRESS1 = 0x90
STRESS_DATA_PTE = 0x98
STRESS_INSTRUCTION_PTE = 0xa0
STRESS_SETUP = 0xa8
STRESS_READY0 = 0xb0
STRESS_READY1 = 0xb8
STRESS_START = 0xc0
STRESS_COMPLETE = 0xc8
STRESS_HALT = 0xd0
STRESS_FAILURE = 0xd8

STRESS_ROOT = 0x2000
STRESS_PHYS_A = 0x6000
STRESS_PHYS_B = 0x8000
STRESS_CODE_A = 0xc000
STRESS_CODE_B = 0xe000
STRESS_DATA_VIRTUAL = 0x2000
STRESS_INSTRUCTION_VIRTUAL = 0x4000
STRESS_RV = VM_RV_PAGE0
STRESS_HANDLER0_PHYS = 0xb000
STRESS_HANDLER1_PHYS = 0xb400
STRESS_HANDLER0_ADDRESS = (1 << 63) | STRESS_HANDLER0_PHYS
STRESS_HANDLER1_ADDRESS = (1 << 63) | STRESS_HANDLER1_PHYS
STRESS_ROUNDS = 8
STRESS_ROUTINE_A = 0x5a5a
STRESS_ROUTINE_B = 0xa5a5


def _ipi_context_address(cpu, register):
    return (
        (1 << 63) |
        MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
        MMIX_VIRT_IPI_CONTEXT_BASE +
        cpu * MMIX_VIRT_IPI_CONTEXT_STRIDE +
        register
    )


def _stress_handler(cpu, address):
    status = _ipi_context_address(cpu, MMIX_VIRT_IPI_CONTEXT_STATUS)
    clear = _ipi_context_address(cpu, MMIX_VIRT_IPI_CONTEXT_CLEAR)
    own_bit = 1 << cpu
    ack = STRESS_ACK0 if cpu == 0 else STRESS_ACK1
    retry = STRESS_RETRY0 if cpu == 0 else STRESS_RETRY1
    handler = STRESS_HANDLER0 if cpu == 0 else STRESS_HANDLER1
    invalid = STRESS_INVALID0 if cpu == 0 else STRESS_INVALID1
    resume = STRESS_RESUME0 if cpu == 0 else STRESS_RESUME1
    program = SMPProgram()

    program.emit(
        *set_octa(R180, STRESS_STATE),
        *set_octa(R181, status),
        *set_octa(R182, clear),
        *set_octa(R183, RQ_IPI),
        insn(GET, R184, 0, SR_Q),
        insn(AND, R185, R184, R183),
    )
    program.emit_branch(BZ, R185, "failure_request")
    program.emit(
        insn(LDOUI, R186, R181, 0),
        insn(CMPUI, R187, R186, MMIX_VIRT_IPI_STATUS_PENDING),
    )
    program.emit_branch(BNZ, R187, "failure_status")
    program.emit(
        insn(LDOUI, R188, R180, handler),
        insn(ADDUI, R188, R188, 1),
        smp_store(R188, R180, handler),
        smp_sync(2),
        smp_load(R189, R180, STRESS_GENERATION),
        smp_load(R190, R180, STRESS_TARGET),
        smp_load(R191, R180, STRESS_KEY),
        wyde(SETL, R192, own_bit),
        insn(AND, R193, R190, R192),
    )
    program.emit_branch(BZ, R193, "failure_target")
    program.emit(
        insn(PUTI, SR_P, 0, 0),
        wyde(SETL, R194, 1),
        smp_cswap(R194, R180, STRESS_LOCK),
    )
    program.emit_branch(BNZ, R194, "failure_lock")
    program.emit(
        insn(GET, R195, 0, SR_P),
        wyde(SETL, R196, 1),
        insn(CMPU, R197, R195, R196),
    )
    program.emit_branch(BNZ, R197, "failure_lock")
    program.emit(
        insn(LDOUI, R198, R180, retry),
        insn(ADDUI, R198, R198, 1),
        smp_store(R198, R180, retry),
        insn(LDVTS, R199, R191, R254),
        insn(LDOUI, R198, R180, invalid),
        insn(ADDUI, R198, R198, 1),
        smp_store(R198, R180, invalid),
        smp_sync(1),
        smp_store(R189, R180, ack),
        wyde(SETL, R198, MMIX_VIRT_IPI_STATUS_PENDING),
        insn(STOUI, R198, R182, 0),
        insn(PUTI, SR_Q, 0, 0),
        insn(LDOUI, R198, R180, resume),
        insn(ADDUI, R198, R198, 1),
        smp_store(R198, R180, resume),
        *set_octa(R255, RK_IPI),
        insn(RESUME, 0, 0, 1),
    )

    for label, reason in (
        ("failure_request", 0x61),
        ("failure_status", 0x62),
        ("failure_target", 0x63),
        ("failure_lock", 0x64),
    ):
        program.mark(label)
        program.emit(
            wyde(SETL, R199, reason + cpu * 0x10),
            smp_store(R199, R180, STRESS_FAILURE),
        )
        smp_emit_unconditional_branch(program, "failure_idle")
    program.mark("failure_idle")
    smp_emit_unconditional_branch(program, "failure_idle")
    return address, program.build()


def _emit_stress_wait(program, field, expected, label):
    program.emit(
        wyde(SETL, R150, expected),
        *set_octa(R153, SMP_WAIT_LIMIT),
    )
    program.mark(f"{label}_wait")
    program.emit(
        smp_sync(2),
        smp_load(R151, R40, field),
        insn(CMPU, R152, R151, R150),
    )
    program.emit_branch(BZ, R152, f"{label}_done")
    program.emit(insn(SUBUI, R153, R153, 1))
    program.emit_branch(BNZ, R153, f"{label}_wait")
    smp_emit_unconditional_branch(program, "failure_timeout")
    program.mark(f"{label}_done")


def _emit_wait_remote_ack(program):
    program.emit(*set_octa(R153, SMP_WAIT_LIMIT))
    program.mark("remote_ack_wait")
    program.emit_branch(BZ, R32, "wait_cpu1_ack")
    program.emit(smp_load(R151, R40, STRESS_ACK0))
    smp_emit_unconditional_branch(program, "compare_remote_ack")
    program.mark("wait_cpu1_ack")
    program.emit(smp_load(R151, R40, STRESS_ACK1))
    program.mark("compare_remote_ack")
    program.emit(
        smp_sync(2),
        insn(CMPU, R152, R151, R50),
    )
    program.emit_branch(BZ, R152, "remote_ack_done")
    program.emit(insn(SUBUI, R153, R153, 1))
    program.emit_branch(BNZ, R153, "remote_ack_wait")
    smp_emit_unconditional_branch(program, "failure_timeout")
    program.mark("remote_ack_done")


def smp_contending_shootdown_program():
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        *set_octa(R40, STRESS_STATE),
        wyde(SETL, R41, 1),
        *set_octa(R42, (1 << 63) | STRESS_ROOT),
        *set_octa(R43, STRESS_DATA_VIRTUAL),
        *set_octa(R44, STRESS_INSTRUCTION_VIRTUAL),
        *set_octa(R45, STRESS_RV),
        *set_octa(R46, (1 << 63) | MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
                  MMIX_VIRT_IPI_SEND),
    )
    program.emit_branch(BNZ, R32, "cpu1_setup")
    program.emit(
        *set_octa(R47, STRESS_HANDLER0_ADDRESS),
        wyde(SETL, R63, 1),
        wyde(SETL, R64, 2),
        *set_octa(R65, STRESS_STATE + STRESS_READY0),
        *set_octa(R66, STRESS_STATE + STRESS_PROGRESS0),
        *set_octa(R67, STRESS_STATE + STRESS_INIT0),
        *set_octa(R68, STRESS_STATE + STRESS_ACK0),
        wyde(SETL, R70, 7),
        smp_store(R70, R42, 0),
        *set_octa(R71, STRESS_PHYS_A | 7),
        smp_store(R71, R42, 8),
        *set_octa(R71, STRESS_CODE_A | 7),
        smp_store(R71, R42, 16),
        *set_octa(R72, (1 << 63) | STRESS_PHYS_A),
        *set_octa(R73, 0x1111222233334444),
        smp_store(R73, R72, 0),
        *set_octa(R72, (1 << 63) | STRESS_PHYS_B),
        *set_octa(R73, 0xaaaabbbbccccdddd),
        smp_store(R73, R72, 0),
        smp_sync(1),
        smp_store(R41, R40, STRESS_SETUP),
    )
    smp_emit_unconditional_branch(program, "setup_complete")

    program.mark("cpu1_setup")
    program.emit(
        *set_octa(R47, STRESS_HANDLER1_ADDRESS),
        wyde(SETL, R63, 2),
        wyde(SETL, R64, 1),
        *set_octa(R65, STRESS_STATE + STRESS_READY1),
        *set_octa(R66, STRESS_STATE + STRESS_PROGRESS1),
        *set_octa(R67, STRESS_STATE + STRESS_INIT1),
        *set_octa(R68, STRESS_STATE + STRESS_ACK1),
    )
    _emit_stress_wait(program, STRESS_SETUP, 1, "stress_setup")

    program.mark("setup_complete")
    program.emit(
        insn(PUT, SR_TT, 0, R47),
        *set_octa(R69, RK_IPI),
        insn(PUT, SR_K, 0, R69),
        insn(PUT, SR_V, 0, R45),
        smp_load(R74, R43, 0),
        insn(GO, R100, R44, R254),
        smp_sync(1),
        insn(STOUI, R41, R65, 0),
    )
    program.emit_branch(BNZ, R32, "wait_for_start")
    _emit_stress_wait(program, STRESS_READY1, 1, "cpu1_stress_ready")
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, STRESS_START),
    )
    program.mark("wait_for_start")
    _emit_stress_wait(program, STRESS_START, 1, "stress_start")
    program.emit(wyde(SETL, R50, 1))

    program.mark("round_begin")
    program.emit(*set_octa(R159, SMP_WAIT_LIMIT))
    program.mark("lock_retry")
    program.emit(
        smp_sync(2),
        smp_load(R75, R40, STRESS_GENERATION),
        insn(CMPU, R76, R75, R50),
    )
    program.emit_branch(BNN, R76, "round_published")
    program.emit(
        insn(PUTI, SR_P, 0, 0),
        wyde(SETL, R77, 1),
        smp_cswap(R77, R40, STRESS_LOCK),
    )
    program.emit_branch(BZ, R77, "lock_failed")
    program.emit(
        smp_sync(0),
        insn(ANDI, R78, R50, 1),
        insn(CMPU, R79, R78, R32),
    )
    program.emit_branch(BZ, R79, "owner_acquired")
    program.emit(
        smp_sync(1),
        smp_store(R254, R40, STRESS_LOCK),
    )
    smp_emit_unconditional_branch(program, "lock_retry")

    program.mark("lock_failed")
    program.emit(insn(SUBUI, R159, R159, 1))
    program.emit_branch(BNZ, R159, "lock_retry")
    smp_emit_unconditional_branch(program, "failure_timeout")

    program.mark("owner_acquired")
    program.emit(
        insn(SUBUI, R80, R50, 1),
        insn(CMPU, R81, R75, R80),
    )
    program.emit_branch(BNZ, R81, "failure_generation")
    program.emit(
        insn(ANDI, R82, R50, 2),
    )
    program.emit_branch(BZ, R82, "use_physical_a")
    program.emit(*set_octa(R83, STRESS_PHYS_B | 7))
    smp_emit_unconditional_branch(program, "physical_selected")
    program.mark("use_physical_a")
    program.emit(*set_octa(R83, STRESS_PHYS_A | 7))
    program.mark("physical_selected")
    program.emit(insn(ANDI, R82, R50, 1))
    program.emit_branch(BZ, R82, "publish_data")
    program.emit(insn(ANDI, R82, R50, 2))
    program.emit_branch(BZ, R82, "use_code_a")
    program.emit(*set_octa(R83, STRESS_CODE_B | 7))
    smp_emit_unconditional_branch(program, "code_selected")
    program.mark("use_code_a")
    program.emit(*set_octa(R83, STRESS_CODE_A | 7))
    program.mark("code_selected")
    program.emit(
        smp_store(R83, R42, 16),
        smp_store(R83, R40, STRESS_INSTRUCTION_PTE),
        *set_octa(R84, STRESS_INSTRUCTION_VIRTUAL),
        wyde(SETL, R85, 3),
    )
    smp_emit_unconditional_branch(program, "publish_descriptor")

    program.mark("publish_data")
    program.emit(
        smp_store(R83, R42, 8),
        smp_store(R83, R40, STRESS_DATA_PTE),
        *set_octa(R84, STRESS_DATA_VIRTUAL),
        wyde(SETL, R85, 2),
    )
    program.mark("publish_descriptor")
    program.emit(
        smp_store(R32, R40, STRESS_OWNER),
        smp_store(R85, R40, STRESS_TARGET),
        smp_store(R84, R40, STRESS_KEY),
        smp_sync(1),
        smp_store(R50, R40, STRESS_GENERATION),
        insn(AND, R86, R85, R63),
    )
    program.emit_branch(BZ, R86, "send_remote")
    program.emit(
        insn(LDVTS, R87, R84, R254),
        smp_sync(1),
        insn(STOUI, R50, R68, 0),
    )
    program.mark("send_remote")
    program.emit(
        smp_sync(1),
        insn(STOUI, R64, R46, 0),
    )
    _emit_wait_remote_ack(program)
    program.emit(
        insn(LDOUI, R88, R67, 0),
        insn(ADDUI, R88, R88, 1),
        insn(STOUI, R88, R67, 0),
        smp_sync(1),
        smp_store(R254, R40, STRESS_LOCK),
    )

    program.mark("round_published")
    program.emit(
        smp_sync(2),
        smp_load(R89, R40, STRESS_TARGET),
        insn(AND, R90, R89, R63),
    )
    program.emit_branch(BZ, R90, "record_progress")
    program.emit(*set_octa(R153, SMP_WAIT_LIMIT))
    program.mark("own_ack_wait")
    program.emit(
        insn(LDOUI, R91, R68, 0),
        insn(CMPU, R92, R91, R50),
    )
    program.emit_branch(BZ, R92, "own_ack_done")
    program.emit(insn(SUBUI, R153, R153, 1))
    program.emit_branch(BNZ, R153, "own_ack_wait")
    smp_emit_unconditional_branch(program, "failure_timeout")
    program.mark("own_ack_done")
    program.emit(insn(ANDI, R93, R50, 1))
    program.emit_branch(BZ, R93, "prime_data")
    program.emit(insn(GO, R100, R44, R254))
    smp_emit_unconditional_branch(program, "record_progress")
    program.mark("prime_data")
    program.emit(smp_load(R94, R43, 0))

    program.mark("record_progress")
    program.emit(
        insn(LDOUI, R95, R66, 0),
        insn(ADDUI, R95, R95, 1),
        insn(STOUI, R95, R66, 0),
        insn(ADDUI, R50, R50, 1),
        insn(CMPUI, R96, R50, STRESS_ROUNDS + 1),
    )
    program.emit_branch(BNZ, R96, "round_begin")
    program.emit_branch(BNZ, R32, "secondary_idle")
    _emit_stress_wait(program, STRESS_PROGRESS1, STRESS_ROUNDS,
                      "stress_cpu1_complete")
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, STRESS_COMPLETE),
    )
    _emit_stress_wait(program, STRESS_HALT, 1, "stress_host_halt")
    program.emit(
        insn(ADDU, R255, R254, R254),
        halt(),
    )

    program.mark("secondary_idle")
    smp_emit_unconditional_branch(program, "secondary_idle")

    for label, reason in (
        ("failure_timeout", 0x81),
        ("failure_generation", 0x82),
    ):
        program.mark(label)
        program.emit(
            wyde(SETL, R99, reason),
            smp_store(R99, R40, STRESS_FAILURE),
        )
        smp_emit_unconditional_branch(program, "failure_idle")
    program.mark("failure_idle")
    smp_emit_unconditional_branch(program, "failure_idle")

    main = program.build()
    routine_a = b"".join([
        wyde(SETL, R110, STRESS_ROUTINE_A),
        insn(GO, R111, R100, R254),
    ])
    routine_b = b"".join([
        wyde(SETL, R110, STRESS_ROUTINE_B),
        insn(GO, R111, R100, R254),
    ])
    expected = (
        (STRESS_STATE_PHYS + STRESS_GENERATION, STRESS_ROUNDS),
        (STRESS_STATE_PHYS + STRESS_TARGET, 2),
        (STRESS_STATE_PHYS + STRESS_KEY, STRESS_DATA_VIRTUAL),
        (STRESS_STATE_PHYS + STRESS_ACK0, STRESS_ROUNDS - 1),
        (STRESS_STATE_PHYS + STRESS_ACK1, STRESS_ROUNDS),
        (STRESS_STATE_PHYS + STRESS_OWNER, 0),
        (STRESS_STATE_PHYS + STRESS_INIT0, STRESS_ROUNDS // 2),
        (STRESS_STATE_PHYS + STRESS_INIT1, STRESS_ROUNDS // 2),
        (STRESS_STATE_PHYS + STRESS_RETRY0, STRESS_ROUNDS // 2),
        (STRESS_STATE_PHYS + STRESS_RETRY1, STRESS_ROUNDS // 2),
        (STRESS_STATE_PHYS + STRESS_HANDLER0, STRESS_ROUNDS // 2),
        (STRESS_STATE_PHYS + STRESS_HANDLER1, STRESS_ROUNDS // 2),
        (STRESS_STATE_PHYS + STRESS_INVALID0, STRESS_ROUNDS // 2),
        (STRESS_STATE_PHYS + STRESS_INVALID1, STRESS_ROUNDS // 2),
        (STRESS_STATE_PHYS + STRESS_RESUME0, STRESS_ROUNDS // 2),
        (STRESS_STATE_PHYS + STRESS_RESUME1, STRESS_ROUNDS // 2),
        (STRESS_STATE_PHYS + STRESS_PROGRESS0, STRESS_ROUNDS),
        (STRESS_STATE_PHYS + STRESS_PROGRESS1, STRESS_ROUNDS),
        (STRESS_STATE_PHYS + STRESS_DATA_PTE, STRESS_PHYS_A | 7),
        (STRESS_STATE_PHYS + STRESS_INSTRUCTION_PTE, STRESS_CODE_B | 7),
        (STRESS_STATE_PHYS + STRESS_FAILURE, 0),
    )
    return MMIXSMPExpectedMemoryTest(
        name="smp-multi-thread-contending-shootdowns",
        image=smp_elf_image(
            main,
            (STRESS_CODE_A, routine_a),
            (STRESS_CODE_B, routine_b),
            _stress_handler(0, STRESS_HANDLER0_PHYS),
            _stress_handler(1, STRESS_HANDLER1_PHYS),
        ),
        main_end=SMP_ENTRY + len(main),
        expected_memory=expected,
        state_base=STRESS_STATE_PHYS,
        complete_offset=STRESS_COMPLETE,
        halt_offset=STRESS_HALT,
        failure_offset=STRESS_FAILURE,
    )


SMP_SHOOTDOWN_STRESS_TESTS = [smp_contending_shootdown_program()]
