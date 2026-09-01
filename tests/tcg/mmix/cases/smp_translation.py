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

SMP_UNSAVE_STATE = (1 << 63) | 0x00200400
SMP_UNSAVE_READY = 0x00
SMP_UNSAVE_RESULT = 0x08
SMP_UNSAVE_HANDLER_STATE = 0x10
SMP_UNSAVE_RWW = 0x18
SMP_UNSAVE_RXX = 0x20
SMP_UNSAVE_RYY = 0x28
SMP_UNSAVE_RZZ = 0x30
SMP_UNSAVE_RBB = 0x38
SMP_UNSAVE_INTERRUPT_HANDLED = 0x40
SMP_UNSAVE_TARGET = 0x12080
SMP_UNSAVE_UPPER_PAGE = SMP_UNSAVE_TARGET & ~0x1fff
SMP_UNSAVE_LOWER_PAGE = SMP_UNSAVE_UPPER_PAGE - 0x2000
SMP_UNSAVE_HANDLER = 0x5000
SMP_UNSAVE_INTERRUPT_HANDLER = 0x6000


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


def unsave_context_migration_program(full_context_roundtrip=False,
                                     interrupt_roundtrip=False):
    program = SMPProgram()
    handler = SMPProgram()
    interrupt_handler = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        *set_octa(R40, SMP_UNSAVE_STATE),
        wyde(SETL, R41, 1),
    )
    program.emit_branch(BNZ, R32, "secondary_wait")

    program.emit(
        *set_octa(R100, 0x1122334455667788),
        wyde(SETL, R255, 0),
        insn(SAVE, R200, 0, 0),
        *set_octa(R50, INITIAL_STACK),
        *set_octa(R51, SMP_UNSAVE_TARGET),
        insn(SUBU, R52, R200, R50),
        insn(SUBU, R51, R51, R52),
        *set_octa(R53, SMP_UNSAVE_TARGET),
    )
    program.mark("copy_context")
    program.emit(
        insn(LDOU, R54, R50, R254),
        insn(STOU, R54, R51, R254),
        insn(ADDUI, R50, R50, 8),
        insn(ADDUI, R51, R51, 8),
        insn(CMPU, R55, R51, R53),
    )
    program.emit_branch(BNP, R55, "copy_context")
    program.emit(
        *set_octa(R60, (1 << 63) | VM_PAGE_TABLE),
        wyde(SETL, R61, 7),
        insn(STOU, R61, R60, R254),
        *set_octa(R62, SMP_UNSAVE_UPPER_PAGE | 7),
        insn(STOUI, R62, R60, (SMP_UNSAVE_UPPER_PAGE >> 13) * 8),
        insn(STOUI, R254, R60,
             (SMP_UNSAVE_LOWER_PAGE >> 13) * 8),
        *set_octa(R63, (1 << 63) | SMP_UNSAVE_HANDLER),
        insn(PUT, SR_T, 0, R63),
        *set_octa(R64, VM_RV_PAGE0),
        *set_octa(R68, VM_RV_SOFTWARE),
        *set_octa(R65, RQ_PROGRAM_R),
        insn(PUT, SR_K, 0, R65),
    )
    program.emit_branch(GETA, R66, "enable_translation")
    program.emit(
        *set_octa(R67, 1 << 63),
        insn(OR, R66, R66, R67),
        insn(GO, R67, R66, R254),
    )
    program.mark("enable_translation")
    program.emit(
        insn(PUT, SR_V, 0, R64),
        *set_octa(R255, SMP_UNSAVE_TARGET),
        insn(LDOU, R69, R255, R254),
        insn(PUT, SR_V, 0, R68),
    )
    program.mark("unsave_restore_site")
    program.emit(insn(UNSAVE, 0, 0, R255))
    if interrupt_roundtrip:
        program.emit(
            *set_octa(R101, SMP_UNSAVE_STATE),
            smp_load(R102, R101, SMP_UNSAVE_READY),
            insn(CMPUI, R102, R102, 1),
        )
        program.emit_branch(BZ, R102, "restore_outer_trap")
    program.emit(
        *set_octa(R40, SMP_UNSAVE_STATE),
        wyde(SETL, R41, 1),
    )
    if interrupt_roundtrip:
        smp_emit_wait_equal(
            program,
            address=R40,
            field=SMP_UNSAVE_INTERRUPT_HANDLED,
            expected=R41,
            value=R42,
            compare=R43,
            counter=R44,
            label="unsave_interrupt_handled",
            timeout_label="failure",
        )
    program.emit(
        smp_sync(1),
        smp_store(R41, R40, SMP_UNSAVE_RESULT),
    )
    program.mark("resumed_idle")
    smp_emit_unconditional_branch(program, "resumed_idle")

    program.mark("secondary_wait")
    smp_emit_wait_equal(
        program,
        address=R40,
        field=SMP_UNSAVE_READY,
        expected=R41,
        value=R42,
        compare=R43,
        counter=R44,
        label="unsave_context_ready",
        timeout_label="failure",
    )
    program.emit(
        smp_load(R200, R40, SMP_UNSAVE_HANDLER_STATE),
        *set_octa(R64, VM_RV_PAGE0),
    )
    program.emit_branch(GETA, R66, "restore_context")
    program.emit(
        *set_octa(R67, 1 << 63),
        insn(OR, R66, R66, R67),
        insn(GO, R67, R66, R254),
    )
    program.mark("restore_context")
    if full_context_roundtrip:
        program.emit(
            insn(SAVE, R201, 0, 0),
            *set_octa(R60, (1 << 63) | VM_PAGE_TABLE),
            *set_octa(R68, VM_RV_SOFTWARE),
            *set_octa(R63, (1 << 63) | SMP_UNSAVE_HANDLER),
            insn(PUT, SR_T, 0, R63),
            insn(STOUI, R254, R60,
                 (SMP_UNSAVE_LOWER_PAGE >> 13) * 8),
            *set_octa(R62, SMP_UNSAVE_LOWER_PAGE),
            insn(LDVTS, R63, R62, R254),
            insn(PUT, SR_V, 0, R68),
        )
        program.emit_branch(GETA, R66, "restore_nested_context")
        program.emit(
            *set_octa(R67, 1 << 63),
            insn(OR, R66, R66, R67),
            insn(GO, R67, R66, R254),
        )
        program.mark("restore_nested_context")
    else:
        program.emit(insn(PUT, SR_V, 0, R64))
    if interrupt_roundtrip:
        program.emit(
            insn(OR, R255, R200, R254),
        )
        program.emit_branch(GETA, R66, "unsave_restore_site")
        program.emit(
            *set_octa(R67, 1 << 63),
            insn(OR, R66, R66, R67),
            insn(GO, R67, R66, R254),
        )
        program.mark("restore_outer_trap")
        program.emit(
            *set_octa(R40, SMP_UNSAVE_STATE),
            wyde(SETL, R41, 2),
            smp_store(R41, R40, SMP_UNSAVE_READY),
        )
    else:
        program.emit(insn(UNSAVE, 0, 0, R200))
    program.emit(
        *set_octa(R40, SMP_UNSAVE_STATE),
        *set_octa(R68, VM_RV_SOFTWARE),
        smp_load(R180, R40, SMP_UNSAVE_RWW),
        smp_load(R181, R40, SMP_UNSAVE_RXX),
        smp_load(R182, R40, SMP_UNSAVE_RYY),
        smp_load(R184, R40, SMP_UNSAVE_RBB),
        insn(PUT, SR_WW, 0, R180),
        insn(PUT, SR_XX, 0, R181),
        insn(PUT, SR_YY, 0, R182),
        *set_octa(R183, SMP_UNSAVE_LOWER_PAGE | 7),
        insn(PUT, SR_ZZ, 0, R183),
        insn(PUT, SR_BB, 0, R184),
        insn(PUT, SR_V, 0, R68),
    )
    if interrupt_roundtrip:
        program.emit(
            *set_octa(R185, (1 << 63) | SMP_UNSAVE_INTERRUPT_HANDLER),
            insn(PUT, SR_TT, 0, R185),
            *set_octa(R186, (1 << 63) |
                      (MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
                       MMIX_VIRT_IPI_SEND)),
            wyde(SETL, R187, 2),
            insn(STOUI, R187, R186, 0),
            smp_sync(1),
            *set_octa(R188, (1 << 63) |
                      (MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
                       MMIX_VIRT_IPI_CONTEXT_BASE +
                       MMIX_VIRT_IPI_CONTEXT_STRIDE +
                       MMIX_VIRT_IPI_CONTEXT_STATUS)),
            wyde(SETL, R189, MMIX_VIRT_IPI_STATUS_PENDING),
        )
        smp_emit_wait_equal(
            program,
            address=R188,
            field=0,
            expected=R189,
            value=R190,
            compare=R191,
            counter=R192,
            label="unsave_ipi_pending",
            timeout_label="failure",
        )
        program.emit(*set_octa(R255, RK_IPI))
    else:
        program.emit(wyde(SETL, R255, 0))
    program.emit(insn(RESUME, 0, 0, 1))

    program.mark("wait_result")
    program.emit(smp_sync(2), smp_load(R70, R40, SMP_UNSAVE_RESULT))
    program.emit_branch(BZ, R70, "wait_result")
    program.emit(
        insn(CMPUI, R71, R70, 1),
    )
    program.emit_branch(BNZ, R71, "failure")
    program.mark("success")
    program.emit(halt())

    program.mark("failure")
    program.emit(wyde(SETL, R90, 0xdead))
    program.mark("failure_halt")
    program.emit(halt())

    handler.emit(
        *set_octa(R40, SMP_UNSAVE_STATE),
        smp_load(R170, R40, SMP_UNSAVE_READY),
    )
    handler.emit_branch(
        BNZ, R170,
        "nested_restore" if full_context_roundtrip else "replay_failed",
    )
    handler.emit(
        insn(GET, R180, 0, SR_WW),
        insn(GET, R181, 0, SR_XX),
        insn(GET, R182, 0, SR_YY),
        insn(GET, R183, 0, SR_ZZ),
        insn(GET, R184, 0, SR_BB),
        smp_store(R180, R40, SMP_UNSAVE_RWW),
        smp_store(R181, R40, SMP_UNSAVE_RXX),
        smp_store(R182, R40, SMP_UNSAVE_RYY),
        smp_store(R183, R40, SMP_UNSAVE_RZZ),
        smp_store(R184, R40, SMP_UNSAVE_RBB),
        *set_octa(R60, (1 << 63) | VM_PAGE_TABLE),
        *set_octa(R61, SMP_UNSAVE_LOWER_PAGE | 2),
        insn(STOUI, R61, R60, (SMP_UNSAVE_LOWER_PAGE >> 13) * 8),
        insn(SAVE, R200, 0, 0),
    )
    if full_context_roundtrip:
        # Model a normal read-trap context restored on this CPU while its
        # private restart record remains on the CPU where the trap began.
        handler.emit_branch(GETA, R185, "migrated_trap_resumed")
        migrated_exec = DYNAMIC_TRAP_RESUME_NEXT | RQ_PROGRAM_R
        if interrupt_roundtrip:
            migrated_exec = RQ_PROGRAM_R | int.from_bytes(
                insn(ADDU, R186, R187, R188), "big"
            )
        handler.emit(
            *set_octa(R186, migrated_exec),
            *set_octa(R187, 1 << 63),
            insn(ADDUI, R185, R185, 4),
            insn(OR, R185, R185, R187),
            insn(PUT, SR_WW, 0, R185),
            insn(PUT, SR_XX, 0, R186),
            wyde(SETL, R255, 0),
            insn(RESUME, 0, 0, 1),
        )
        handler.mark("migrated_trap_resumed")
        handler.emit(
            insn(SWYM, 0, 0, 0),
            insn(UNSAVE, 0, 0, R200),
            insn(SAVE, R200, 0, 0),
            *set_octa(R40, SMP_UNSAVE_STATE),
            wyde(SETL, R41, 1),
        )
    handler.emit(
        smp_store(R200, R40, SMP_UNSAVE_HANDLER_STATE),
        *set_octa(R61, SMP_UNSAVE_LOWER_PAGE | 7),
        insn(STOUI, R61, R60, (SMP_UNSAVE_LOWER_PAGE >> 13) * 8),
        *set_octa(R62, SMP_UNSAVE_LOWER_PAGE),
        insn(LDVTS, R63, R62, R254),
        smp_sync(1),
        smp_store(R41, R40, SMP_UNSAVE_READY),
    )
    if full_context_roundtrip:
        handler.emit_branch(BZ, R254, "wait_result")
        handler.mark("nested_restore")
        handler.emit(
            *set_octa(R183, SMP_UNSAVE_LOWER_PAGE | 7),
            insn(PUT, SR_ZZ, 0, R183),
            insn(RESUME, 0, 0, 1),
        )
    handler.mark("wait_result")
    handler.emit(smp_sync(2), smp_load(R70, R40, SMP_UNSAVE_RESULT))
    handler.emit_branch(BZ, R70, "wait_result")
    handler.emit(
        insn(CMPUI, R71, R70, 1),
    )
    handler.emit_branch(BNZ, R71, "replay_failed")
    handler.mark("success")
    handler.emit(halt())
    handler.mark("replay_failed")
    handler.emit(
        wyde(SETL, R70, 2),
        smp_sync(1),
        smp_store(R70, R40, SMP_UNSAVE_RESULT),
    )
    handler.mark("failure_idle")
    handler.emit_branch(BZ, R254, "failure_idle")

    interrupt_handler.emit(
        *set_octa(R180, (1 << 63) |
                  (MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
                   MMIX_VIRT_IPI_CONTEXT_BASE +
                   MMIX_VIRT_IPI_CONTEXT_STRIDE +
                   MMIX_VIRT_IPI_CONTEXT_CLEAR)),
        *set_octa(R184, (1 << 63) |
                  (MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
                   MMIX_VIRT_IPI_CONTEXT_BASE +
                   MMIX_VIRT_IPI_CONTEXT_CLEAR)),
        wyde(SETL, R181, MMIX_VIRT_IPI_STATUS_PENDING),
        insn(STOUI, R181, R180, 0),
        insn(STOUI, R181, R184, 0),
        insn(PUTI, SR_Q, 0, 0),
        *set_octa(R182, SMP_UNSAVE_STATE),
        wyde(SETL, R183, 1),
        smp_sync(1),
        smp_store(R183, R182, SMP_UNSAVE_INTERRUPT_HANDLED),
        wyde(SETL, R255, 0),
        insn(RESUME, 0, 0, 1),
    )

    return SMPProgramImage(
        code=smp_elf_image(
            program.build(),
            (SMP_UNSAVE_HANDLER, handler.build()),
            (SMP_UNSAVE_INTERRUPT_HANDLER, interrupt_handler.build()),
        ),
        success_pc=(1 << 63) | handler.address(
            "success", SMP_UNSAVE_HANDLER),
        timeout_pc=program.address("failure_halt"),
        success_regs={},
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
SMP_UNSAVE_CONTEXT_MIGRATION = unsave_context_migration_program()
SMP_UNSAVE_FULL_CONTEXT_MIGRATION = unsave_context_migration_program(True)
SMP_UNSAVE_INTERRUPT_CONTEXT_MIGRATION = unsave_context_migration_program(
    True, True
)

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
    MMIXSMPTest(
        "smp-multi-thread-unsave-context-migration",
        SMP_UNSAVE_CONTEXT_MIGRATION.code,
        pc=SMP_UNSAVE_CONTEXT_MIGRATION.success_pc,
        regs=SMP_UNSAVE_CONTEXT_MIGRATION.success_regs,
        thread_mode=TCG_THREAD_MULTI,
    ),
    MMIXSMPTest(
        "smp-multi-thread-unsave-full-context-migration",
        SMP_UNSAVE_FULL_CONTEXT_MIGRATION.code,
        pc=SMP_UNSAVE_FULL_CONTEXT_MIGRATION.success_pc,
        regs=SMP_UNSAVE_FULL_CONTEXT_MIGRATION.success_regs,
        thread_mode=TCG_THREAD_MULTI,
    ),
    MMIXSMPTest(
        "smp-multi-thread-unsave-interrupt-context-migration",
        SMP_UNSAVE_INTERRUPT_CONTEXT_MIGRATION.code,
        pc=SMP_UNSAVE_INTERRUPT_CONTEXT_MIGRATION.success_pc,
        regs=SMP_UNSAVE_INTERRUPT_CONTEXT_MIGRATION.success_regs,
        thread_mode=TCG_THREAD_MULTI,
    ),
]
