#!/usr/bin/env python3
#
# MMIX concurrent memory and atomic test cases
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *
from .smp import (
    MMIXSMPTest,
    SMPProgram,
    SMPProgramImage,
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


SMP_SHARED_BASE = 0x00200100
SMP_SHARED_VALUE = 0x00
SMP_SHARED_START = 0x08
SMP_SHARED_DONE = 0x10
SMP_SHARED_PROGRESS = 0x18
SMP_SHARED_PAYLOAD = 0x20
SMP_SHARED_FLAG = 0x28
SMP_SHARED_ACK = 0x30
SMP_STRESS_ITERATIONS = 0x10000
SMP_PATTERN_A = 0x00ff00ffff0000ff
SMP_PATTERN_B = 0xff00ff0000ffff00


def smp_memory_test(name, image):
    return MMIXSMPTest(
        name,
        smp_elf_image(image.code),
        pc=image.success_pc,
        regs=image.success_regs,
        thread_mode=TCG_THREAD_MULTI,
    )


def aligned_octa_atomicity_program():
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        *set_octa(R40, SMP_SHARED_BASE),
        *set_octa(R50, SMP_PATTERN_A),
        *set_octa(R51, SMP_PATTERN_B),
        *set_octa(R52, SMP_STRESS_ITERATIONS),
        wyde(SETL, R53, 1),
    )
    program.emit_branch(BNZ, R32, "writer_wait")
    program.emit(
        smp_store(R50, R40, SMP_SHARED_VALUE),
        smp_store(R254, R40, SMP_SHARED_DONE),
        smp_sync(1),
        smp_store(R53, R40, SMP_SHARED_START),
        *set_octa(R60, SMP_STRESS_ITERATIONS),
    )
    program.mark("reader_loop")
    program.emit(
        smp_load(R61, R40, SMP_SHARED_VALUE),
        insn(CMPU, R62, R61, R50),
    )
    program.emit_branch(BZ, R62, "reader_value_valid")
    program.emit(insn(CMPU, R62, R61, R51))
    program.emit_branch(BNZ, R62, "failure")
    program.mark("reader_value_valid")
    program.emit(insn(SUBUI, R60, R60, 1))
    program.emit_branch(BNZ, R60, "reader_loop")
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHARED_DONE,
        expected=R53,
        value=R63,
        compare=R64,
        counter=R65,
        label="reader_done",
        timeout_label="failure",
    )
    program.emit(
        smp_sync(2),
        smp_load(R66, R40, SMP_SHARED_PROGRESS),
        wyde(SETL, R90, 1),
    )
    program.mark("success")
    program.emit(halt())

    program.mark("writer_wait")
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHARED_START,
        expected=R53,
        value=R70,
        compare=R71,
        counter=R72,
        label="writer_start",
        timeout_label="failure",
    )
    program.emit(*set_octa(R73, SMP_STRESS_ITERATIONS))
    program.mark("writer_loop")
    program.emit(
        smp_store(R50, R40, SMP_SHARED_VALUE),
        smp_store(R51, R40, SMP_SHARED_VALUE),
        insn(SUBUI, R73, R73, 1),
    )
    program.emit_branch(BNZ, R73, "writer_loop")
    program.emit(
        *set_octa(R74, SMP_STRESS_ITERATIONS),
        smp_store(R74, R40, SMP_SHARED_PROGRESS),
        smp_sync(1),
        smp_store(R53, R40, SMP_SHARED_DONE),
    )
    program.mark("writer_idle")
    smp_emit_unconditional_branch(program, "writer_idle")

    program.mark("failure")
    wyde_failure = wyde(SETL, R90, 0xdead)
    program.emit(wyde_failure)
    program.mark("failure_halt")
    program.emit(halt())

    return SMPProgramImage(
        code=program.build(),
        success_pc=program.address("success"),
        timeout_pc=program.address("failure_halt"),
        success_regs={
            R32: 0,
            R60: 0,
            R63: 1,
            R66: SMP_STRESS_ITERATIONS,
            R90: 1,
        },
    )


def sync_message_passing_program():
    rounds = (
        (1, 0x1111222233334444, 1, 2),
        (2, 0x5555666677778888, 0, 0),
        (3, 0x9999aaaabbbbcccc, 3, 3),
    )
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        *set_octa(R40, SMP_SHARED_BASE),
        wyde(SETL, R41, 1),
    )
    program.emit_branch(BNZ, R32, "producer_wait")
    program.emit(
        smp_store(R254, R40, SMP_SHARED_FLAG),
        smp_store(R254, R40, SMP_SHARED_ACK),
        smp_sync(0),
        smp_store(R41, R40, SMP_SHARED_START),
    )
    for index, (round_number, payload, _, consumer_sync) in enumerate(rounds):
        expected_reg = R50 + index
        value_reg = R60 + index
        flag_reg = R70 + index
        program.emit(
            *set_octa(expected_reg, payload),
            wyde(SETL, flag_reg, round_number),
        )
        smp_emit_wait_equal(
            program,
            address=R40,
            field=SMP_SHARED_FLAG,
            expected=flag_reg,
            value=R80,
            compare=R81,
            counter=R82,
            label=f"consumer_round_{index}",
            timeout_label="failure",
        )
        program.emit(
            smp_sync(consumer_sync),
            smp_load(value_reg, R40, SMP_SHARED_PAYLOAD),
            insn(CMPU, R83, value_reg, expected_reg),
        )
        program.emit_branch(BNZ, R83, "failure")
        program.emit(
            smp_sync(1),
            smp_store(flag_reg, R40, SMP_SHARED_ACK),
        )
    program.emit(wyde(SETL, R90, 1))
    program.mark("success")
    program.emit(halt())

    program.mark("producer_wait")
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHARED_START,
        expected=R41,
        value=R91,
        compare=R92,
        counter=R93,
        label="producer_start",
        timeout_label="failure",
    )
    for index, (round_number, payload, producer_sync, _) in enumerate(rounds):
        payload_reg = R100 + index
        round_reg = R110 + index
        program.emit(
            *set_octa(payload_reg, payload),
            wyde(SETL, round_reg, round_number),
            smp_store(payload_reg, R40, SMP_SHARED_PAYLOAD),
            smp_sync(producer_sync),
            smp_store(round_reg, R40, SMP_SHARED_FLAG),
        )
        smp_emit_wait_equal(
            program,
            address=R40,
            field=SMP_SHARED_ACK,
            expected=round_reg,
            value=R120,
            compare=R121,
            counter=R122,
            label=f"producer_ack_{index}",
            timeout_label="failure",
        )
    program.mark("producer_idle")
    smp_emit_unconditional_branch(program, "producer_idle")

    program.mark("failure")
    program.emit(wyde(SETL, R90, 0xdead))
    program.mark("failure_halt")
    program.emit(halt())

    return SMPProgramImage(
        code=program.build(),
        success_pc=program.address("success"),
        timeout_pc=program.address("failure_halt"),
        success_regs={
            R50: rounds[0][1],
            R51: rounds[1][1],
            R52: rounds[2][1],
            R60: rounds[0][1],
            R61: rounds[1][1],
            R62: rounds[2][1],
            R90: 1,
        },
    )


def cswap_contention_program():
    iterations = 0x4000
    expected_count = iterations * 2
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        *set_octa(R40, SMP_SHARED_BASE),
        wyde(SETL, R41, 1),
        *set_octa(R42, iterations),
        wyde(SETL, R43, 0),
    )
    program.emit_branch(BNZ, R32, "counter_wait")
    program.emit(
        smp_store(R254, R40, SMP_SHARED_VALUE),
        smp_store(R254, R40, SMP_SHARED_DONE),
        smp_sync(0),
        smp_store(R41, R40, SMP_SHARED_START),
    )
    program.mark("counter_begin")
    program.mark("counter_retry")
    program.emit(
        smp_load(R50, R40, SMP_SHARED_VALUE),
        insn(PUT, SR_P, 0, R50),
        insn(ADDUI, R51, R50, 1),
        smp_cswap(R51, R40, SMP_SHARED_VALUE),
    )
    program.emit_branch(BNZ, R51, "counter_success")
    program.emit(
        insn(GET, R52, 0, SR_P),
        insn(ADDUI, R43, R43, 1),
    )
    smp_emit_unconditional_branch(program, "counter_retry")
    program.mark("counter_success")
    program.emit(
        insn(GET, R53, 0, SR_P),
        insn(CMPU, R54, R53, R50),
    )
    program.emit_branch(BNZ, R54, "failure")
    program.emit(insn(SUBUI, R42, R42, 1))
    program.emit_branch(BNZ, R42, "counter_retry")
    program.emit_branch(BNZ, R32, "counter_secondary_done")

    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHARED_DONE,
        expected=R41,
        value=R60,
        compare=R61,
        counter=R62,
        label="counter_peer_done",
        timeout_label="failure",
    )
    program.emit(
        smp_sync(2),
        smp_load(R63, R40, SMP_SHARED_VALUE),
        *set_octa(R64, expected_count),
        insn(CMPU, R65, R63, R64),
    )
    program.emit_branch(BNZ, R65, "failure")

    program.emit(
        wyde(SETL, R66, 0x55),
        smp_store(R66, R40, SMP_SHARED_PROGRESS),
        insn(ADDUI, R76, R40, SMP_SHARED_PROGRESS),
        wyde(SETL, R67, 0x44),
        insn(PUT, SR_P, 0, R67),
        wyde(SETL, R68, 0x66),
        insn(CSWAP, R68, R76, R0),
        insn(GET, R69, 0, SR_P),
        smp_load(R70, R40, SMP_SHARED_PROGRESS),
    )
    program.emit_branch(BNZ, R68, "failure")
    program.emit(
        wyde(SETL, R71, 0x55),
        insn(CMPU, R72, R69, R71),
    )
    program.emit_branch(BNZ, R72, "failure")
    program.emit(insn(CMPU, R72, R70, R71))
    program.emit_branch(BNZ, R72, "failure")
    program.emit(
        insn(PUT, SR_P, 0, R71),
        wyde(SETL, R73, 0x66),
        insn(CSWAP, R73, R76, R0),
        insn(GET, R74, 0, SR_P),
        smp_load(R75, R40, SMP_SHARED_PROGRESS),
    )
    program.emit_branch(BZ, R73, "failure")
    program.emit(wyde(SETL, R90, 1))
    program.mark("success")
    program.emit(halt())

    program.mark("counter_wait")
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_SHARED_START,
        expected=R41,
        value=R80,
        compare=R81,
        counter=R82,
        label="counter_start",
        timeout_label="failure",
    )
    smp_emit_unconditional_branch(program, "counter_begin")
    program.mark("counter_secondary_done")
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, SMP_SHARED_DONE),
    )
    program.mark("counter_secondary_idle")
    smp_emit_unconditional_branch(program, "counter_secondary_idle")

    program.mark("failure")
    program.emit(wyde(SETL, R90, 0xdead))
    program.mark("failure_halt")
    program.emit(halt())

    return SMPProgramImage(
        code=program.build(),
        success_pc=program.address("success"),
        timeout_pc=program.address("failure_halt"),
        success_regs={
            R32: 0,
            R42: 0,
            R63: expected_count,
            R68: 0,
            R69: 0x55,
            R70: 0x55,
            R73: 1,
            R74: 0x55,
            R75: 0x66,
            R90: 1,
        },
    )


SMP_MEMORY_TESTS = [
    smp_memory_test(
        "smp-multi-thread-aligned-octa-atomicity",
        aligned_octa_atomicity_program(),
    ),
    smp_memory_test(
        "smp-multi-thread-sync-message-passing",
        sync_message_passing_program(),
    ),
]

SMP_CSWAP_TESTS = [
    smp_memory_test(
        "smp-multi-thread-cswap-contention",
        cswap_contention_program(),
    ),
]
