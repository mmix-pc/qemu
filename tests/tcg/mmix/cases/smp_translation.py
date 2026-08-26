#!/usr/bin/env python3
#
# MMIX concurrent translation-coherency test cases
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *
from .smp import (
    MMIXSMPTest,
    SMPProgram,
    SMPProgramImage,
    TCG_THREAD_MULTI,
    smp_elf_image,
    smp_emit_unconditional_branch,
    smp_emit_wait_equal,
    smp_load,
    smp_store,
    smp_sync,
)


SMP_TRANSLATION_STATE = (1 << 63) | 0x00200300
SMP_TRANSLATION_SETUP = 0x00
SMP_TRANSLATION_CPU0_READY = 0x08
SMP_TRANSLATION_CPU1_READY = 0x10
SMP_TRANSLATION_REMAP_GO = 0x18
SMP_TRANSLATION_CHANGED = 0x20
SMP_TRANSLATION_CPU0_DONE = 0x28
SMP_TRANSLATION_CPU1_DONE = 0x30
SMP_TRANSLATION_CPU1_VALUE = 0x38
SMP_TRANSLATION_CPU1_RV = 0x40

SMP_TRANSLATION_ROOT0 = 0x2000
SMP_TRANSLATION_ROOT1 = 0x4000
SMP_TRANSLATION_PHYS_A = 0x6000
SMP_TRANSLATION_PHYS_B = 0x8000
SMP_TRANSLATION_VIRTUAL_DATA = 0x2000
SMP_TRANSLATION_VIRTUAL_CODE = 0x4000
SMP_TRANSLATION_VALUE_A = 0x1111222233334444
SMP_TRANSLATION_VALUE_B = 0xaaaabbbbccccdddd
SMP_TRANSLATION_RV0 = VM_RV_PAGE0
SMP_TRANSLATION_RV1 = VM_RV_ROOT2 | (1 << 3)


def emit_translation_setup(program, virtual_index):
    program.emit(
        *set_octa(R100, (1 << 63) | SMP_TRANSLATION_ROOT0),
        wyde(SETL, R101, 7),
        smp_store(R101, R100, 0),
        *set_octa(R102, SMP_TRANSLATION_PHYS_A | 7),
        smp_store(R102, R100, virtual_index * 8),
        *set_octa(R103, (1 << 63) | SMP_TRANSLATION_ROOT1),
        wyde(SETL, R104, 0xf),
        smp_store(R104, R103, 0),
        *set_octa(R105, SMP_TRANSLATION_PHYS_A | 0xf),
        smp_store(R105, R103, virtual_index * 8),
    )


def emit_select_translation(program):
    program.emit_branch(BNZ, R32, "use_cpu1_translation")
    program.emit(*set_octa(R110, SMP_TRANSLATION_RV0))
    smp_emit_unconditional_branch(program, "translation_selected")
    program.mark("use_cpu1_translation")
    program.emit(*set_octa(R110, SMP_TRANSLATION_RV1))
    program.mark("translation_selected")
    program.emit(insn(PUT, SR_V, 0, R110))


def emit_wait_for_setup(program):
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_TRANSLATION_SETUP,
        expected=R41,
        value=R42,
        compare=R43,
        counter=R44,
        label="translation_setup",
        timeout_label="failure",
    )


def data_translation_coherency_program():
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        *set_octa(R40, SMP_TRANSLATION_STATE),
        wyde(SETL, R41, 1),
    )
    program.emit_branch(BNZ, R32, "setup_wait")
    emit_translation_setup(program, 1)
    program.emit(
        *set_octa(R106, (1 << 63) | SMP_TRANSLATION_PHYS_A),
        *set_octa(R107, SMP_TRANSLATION_VALUE_A),
        smp_store(R107, R106, 0),
        *set_octa(R108, (1 << 63) | SMP_TRANSLATION_PHYS_B),
        *set_octa(R109, SMP_TRANSLATION_VALUE_B),
        smp_store(R109, R108, 0),
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_SETUP),
    )
    smp_emit_unconditional_branch(program, "setup_complete")
    program.mark("setup_wait")
    emit_wait_for_setup(program)
    program.mark("setup_complete")
    emit_select_translation(program)
    program.emit(
        *set_octa(R50, SMP_TRANSLATION_VIRTUAL_DATA),
        smp_load(R60, R50, 0),
    )
    program.emit_branch(BNZ, R32, "data_cpu1_ready")
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_CPU0_READY),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_TRANSLATION_CPU1_READY,
        expected=R41,
        value=R61,
        compare=R62,
        counter=R63,
        label="data_cpu1_cached",
        timeout_label="failure",
    )
    program.emit(
        smp_store(R41, R40, SMP_TRANSLATION_REMAP_GO),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_TRANSLATION_CHANGED,
        expected=R41,
        value=R64,
        compare=R65,
        counter=R66,
        label="data_pte_changed",
        timeout_label="failure",
    )
    program.emit(
        smp_sync(2),
        smp_load(R67, R50, 0),
        insn(LDVTS, R68, R50, R254),
        smp_load(R69, R50, 0),
        insn(GET, R70, 0, SR_V),
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_CPU0_DONE),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_TRANSLATION_CPU1_DONE,
        expected=R41,
        value=R71,
        compare=R72,
        counter=R73,
        label="data_cpu1_done",
        timeout_label="failure",
    )
    program.emit(
        smp_sync(2),
        smp_load(R74, R40, SMP_TRANSLATION_CPU1_VALUE),
        smp_load(R75, R40, SMP_TRANSLATION_CPU1_RV),
        wyde(SETL, R90, 1),
    )
    program.mark("success")
    program.emit(halt())

    program.mark("data_cpu1_ready")
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_CPU1_READY),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_TRANSLATION_REMAP_GO,
        expected=R41,
        value=R80,
        compare=R81,
        counter=R82,
        label="data_remap_go",
        timeout_label="failure",
    )
    program.emit(
        *set_octa(R83, (1 << 63) | (SMP_TRANSLATION_ROOT0 + 8)),
        *set_octa(R84, SMP_TRANSLATION_PHYS_B | 7),
        smp_store(R84, R83, 0),
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_CHANGED),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_TRANSLATION_CPU0_DONE,
        expected=R41,
        value=R85,
        compare=R86,
        counter=R87,
        label="data_cpu0_done",
        timeout_label="failure",
    )
    program.emit(
        smp_sync(2),
        smp_load(R88, R50, 0),
        insn(GET, R89, 0, SR_V),
        smp_store(R88, R40, SMP_TRANSLATION_CPU1_VALUE),
        smp_store(R89, R40, SMP_TRANSLATION_CPU1_RV),
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_CPU1_DONE),
    )
    program.mark("secondary_idle")
    smp_emit_unconditional_branch(program, "secondary_idle")

    program.mark("failure")
    program.emit(wyde(SETL, R90, 0xdead))
    program.mark("failure_halt")
    program.emit(halt())

    return SMPProgramImage(
        code=smp_elf_image(program.build()),
        success_pc=program.address("success"),
        timeout_pc=program.address("failure_halt"),
        success_regs={
            R60: SMP_TRANSLATION_VALUE_A,
            R67: SMP_TRANSLATION_VALUE_A,
            R68: 2,
            R69: SMP_TRANSLATION_VALUE_B,
            R70: SMP_TRANSLATION_RV0,
            R74: SMP_TRANSLATION_VALUE_A,
            R75: SMP_TRANSLATION_RV1,
            R90: 1,
        },
    )


def instruction_translation_coherency_program():
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        *set_octa(R40, SMP_TRANSLATION_STATE),
        wyde(SETL, R41, 1),
    )
    program.emit_branch(BNZ, R32, "setup_wait")
    emit_translation_setup(program, 2)
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_SETUP),
    )
    smp_emit_unconditional_branch(program, "setup_complete")
    program.mark("setup_wait")
    emit_wait_for_setup(program)
    program.mark("setup_complete")
    emit_select_translation(program)
    program.emit(
        *set_octa(R50, SMP_TRANSLATION_VIRTUAL_CODE),
        insn(GO, R100, R50, R254),
        insn(ADDU, R60, R110, R254),
    )
    program.emit_branch(BNZ, R32, "instruction_cpu1_ready")
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_CPU0_READY),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_TRANSLATION_CPU1_READY,
        expected=R41,
        value=R61,
        compare=R62,
        counter=R63,
        label="instruction_cpu1_cached",
        timeout_label="failure",
    )
    program.emit(smp_store(R41, R40, SMP_TRANSLATION_REMAP_GO))
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_TRANSLATION_CHANGED,
        expected=R41,
        value=R64,
        compare=R65,
        counter=R66,
        label="instruction_pte_changed",
        timeout_label="failure",
    )
    program.emit(
        smp_sync(2),
        insn(GO, R100, R50, R254),
        insn(ADDU, R67, R110, R254),
        insn(LDVTS, R68, R50, R254),
        insn(GO, R100, R50, R254),
        insn(ADDU, R69, R110, R254),
        insn(GET, R70, 0, SR_V),
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_CPU0_DONE),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_TRANSLATION_CPU1_DONE,
        expected=R41,
        value=R71,
        compare=R72,
        counter=R73,
        label="instruction_cpu1_done",
        timeout_label="failure",
    )
    program.emit(
        smp_sync(2),
        smp_load(R74, R40, SMP_TRANSLATION_CPU1_VALUE),
        smp_load(R75, R40, SMP_TRANSLATION_CPU1_RV),
        wyde(SETL, R90, 1),
    )
    program.mark("success")
    program.emit(halt())

    program.mark("instruction_cpu1_ready")
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_CPU1_READY),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_TRANSLATION_REMAP_GO,
        expected=R41,
        value=R80,
        compare=R81,
        counter=R82,
        label="instruction_remap_go",
        timeout_label="failure",
    )
    program.emit(
        *set_octa(R83, (1 << 63) | (SMP_TRANSLATION_ROOT0 + 16)),
        *set_octa(R84, SMP_TRANSLATION_PHYS_B | 7),
        smp_store(R84, R83, 0),
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_CHANGED),
    )
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_TRANSLATION_CPU0_DONE,
        expected=R41,
        value=R85,
        compare=R86,
        counter=R87,
        label="instruction_cpu0_done",
        timeout_label="failure",
    )
    program.emit(
        smp_sync(2),
        insn(GO, R100, R50, R254),
        insn(ADDU, R88, R110, R254),
        insn(GET, R89, 0, SR_V),
        smp_store(R88, R40, SMP_TRANSLATION_CPU1_VALUE),
        smp_store(R89, R40, SMP_TRANSLATION_CPU1_RV),
        smp_sync(1),
        smp_store(R41, R40, SMP_TRANSLATION_CPU1_DONE),
    )
    program.mark("secondary_idle")
    smp_emit_unconditional_branch(program, "secondary_idle")

    program.mark("failure")
    program.emit(wyde(SETL, R90, 0xdead))
    program.mark("failure_halt")
    program.emit(halt())

    routine_a = b"".join([
        wyde(SETL, R110, 0x1111),
        insn(GO, R111, R100, R254),
    ])
    routine_b = b"".join([
        wyde(SETL, R110, 0x2222),
        insn(GO, R111, R100, R254),
    ])
    return SMPProgramImage(
        code=smp_elf_image(
            program.build(),
            (SMP_TRANSLATION_PHYS_A, routine_a),
            (SMP_TRANSLATION_PHYS_B, routine_b),
        ),
        success_pc=program.address("success"),
        timeout_pc=program.address("failure_halt"),
        success_regs={
            R60: 0x1111,
            R67: 0x1111,
            R68: 1,
            R69: 0x2222,
            R70: SMP_TRANSLATION_RV0,
            R74: 0x1111,
            R75: SMP_TRANSLATION_RV1,
            R90: 1,
        },
    )


SMP_DATA_TRANSLATION = data_translation_coherency_program()
SMP_INSTRUCTION_TRANSLATION = instruction_translation_coherency_program()

SMP_TRANSLATION_TESTS = [
    MMIXSMPTest(
        "smp-multi-thread-data-translation-coherency",
        SMP_DATA_TRANSLATION.code,
        pc=SMP_DATA_TRANSLATION.success_pc,
        regs=SMP_DATA_TRANSLATION.success_regs,
        thread_mode=TCG_THREAD_MULTI,
    ),
    MMIXSMPTest(
        "smp-multi-thread-instruction-translation-coherency",
        SMP_INSTRUCTION_TRANSLATION.code,
        pc=SMP_INSTRUCTION_TRANSLATION.success_pc,
        regs=SMP_INSTRUCTION_TRANSLATION.success_regs,
        thread_mode=TCG_THREAD_MULTI,
    ),
]
