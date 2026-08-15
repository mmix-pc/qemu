#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *


def masked_interrupt_request_program():
    timer_compare = (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0] +
        MMIX_VIRT_TIMER_CONTEXT_BASE +
        MMIX_VIRT_TIMER_CONTEXT_COMPARE
    )
    timer_control = (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0] +
        MMIX_VIRT_TIMER_CONTEXT_BASE +
        MMIX_VIRT_TIMER_CONTEXT_CONTROL
    )
    intc_enable = (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0] +
        MMIX_VIRT_INTC_CONTEXT_BASE +
        MMIX_VIRT_INTC_CONTEXT_ENABLE
    )
    intc_claim = (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0] +
        MMIX_VIRT_INTC_CONTEXT_BASE +
        MMIX_VIRT_INTC_CONTEXT_CLAIM
    )

    program = [
        insn(PUTI, SR_K, 0, 0),
        *set_octa(R1, intc_enable),
        *set_octa(R2, 1 << MMIX_VIRT_TIMER_IRQ_BASE),
        insn(STTU, R2, R1, R0),
        *set_octa(R3, timer_compare),
        insn(STOU, R0, R3, R0),
        *set_octa(R4, timer_control),
        wyde(SETL, R5, MMIX_VIRT_TIMER_CONTROL_ENABLE |
             MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE),
        insn(STOU, R5, R4, R0),
        *set_octa(R6, RQ_INTERRUPT_CONTROLLER),
        insn(GET, R20, 0, SR_Q),
        insn(AND, R21, R20, R6),
        branch(BZB, R21, 0xfffe),

        # An active level cannot be cleared by software.
        insn(PUTI, SR_Q, 0, 0),
        insn(GET, R22, 0, SR_Q),

        # Claiming withdraws the CPU input, but rQ remains latched until PUT.
        *set_octa(R7, intc_claim),
        insn(LDTU, R23, R7, R0),
        insn(GET, R24, 0, SR_Q),
        insn(PUTI, SR_Q, 0, 0),
        insn(GET, R25, 0, SR_Q),
        insn(GET, R26, 0, SR_K),
        halt(),
    ]
    return b"".join(program), (len(program) - 1) * 4


MASKED_INTERRUPT_REQUEST = masked_interrupt_request_program()


def external_dynamic_trap_program():
    handler = 0x100
    timer_compare = (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0] +
        MMIX_VIRT_TIMER_CONTEXT_BASE +
        MMIX_VIRT_TIMER_CONTEXT_COMPARE
    )
    timer_control = (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0] +
        MMIX_VIRT_TIMER_CONTEXT_BASE +
        MMIX_VIRT_TIMER_CONTEXT_CONTROL
    )
    intc_enable = (
        MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0] +
        MMIX_VIRT_INTC_CONTEXT_BASE +
        MMIX_VIRT_INTC_CONTEXT_ENABLE
    )
    prefix = [
        insn(PUTI, SR_K, 0, 0),
        *set_octa(R1, intc_enable),
        *set_octa(R2, 1 << MMIX_VIRT_TIMER_IRQ_BASE),
        insn(STTU, R2, R1, R0),
        *set_octa(R3, timer_compare),
        insn(STOU, R0, R3, R0),
        *set_octa(R4, timer_control),
        wyde(SETL, R5, MMIX_VIRT_TIMER_CONTROL_ENABLE |
             MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE),
        insn(STOU, R5, R4, R0),
        *set_octa(R6, RQ_INTERRUPT_CONTROLLER),
        insn(GET, R20, 0, SR_Q),
        insn(AND, R21, R20, R6),
        branch(BZB, R21, 0xfffe),
        wyde(SETL, R8, 0x55),
        insn(ADDU, R255, R8, R0),
        wyde(SETL, R9, 0x1122),
        insn(PUT, SR_J, 0, R9),
        wyde(SETL, R10, handler),
        insn(PUT, SR_TT, 0, R10),
        insn(PUT, SR_K, 0, R6),
    ]
    resume_pc = len(b"".join(prefix))
    prefix.extend([
        wyde(SETL, R30, 0xee),
        halt(),
    ])
    program = program_with_handler(
        prefix,
        handler,
        [
            insn(GET, R40, 0, SR_Q),
            insn(GET, R41, 0, SR_WW),
            insn(GET, R42, 0, SR_XX),
            insn(GET, R43, 0, SR_YY),
            insn(GET, R44, 0, SR_ZZ),
            insn(GET, R45, 0, SR_BB),
            insn(GET, R46, 0, SR_K),
            insn(ADDU, R47, R255, R0),
            insn(PUTI, SR_Q, 0, 0),
            insn(ADDU, R255, R0, R0),
            halt(),
        ],
    )
    return program, handler + 10 * 4, resume_pc


EXTERNAL_DYNAMIC_TRAP = external_dynamic_trap_program()


def external_dynamic_trap_resume_program():
    handler = 0x200
    timer_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0]
    intc_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0]
    timer_compare = (timer_base + MMIX_VIRT_TIMER_CONTEXT_BASE +
                     MMIX_VIRT_TIMER_CONTEXT_COMPARE)
    timer_control = (timer_base + MMIX_VIRT_TIMER_CONTEXT_BASE +
                     MMIX_VIRT_TIMER_CONTEXT_CONTROL)
    timer_status = (timer_base + MMIX_VIRT_TIMER_CONTEXT_BASE +
                    MMIX_VIRT_TIMER_CONTEXT_STATUS)
    intc_enable = (intc_base + MMIX_VIRT_INTC_CONTEXT_BASE +
                   MMIX_VIRT_INTC_CONTEXT_ENABLE)
    intc_claim = (intc_base + MMIX_VIRT_INTC_CONTEXT_BASE +
                  MMIX_VIRT_INTC_CONTEXT_CLAIM)
    intc_complete = (intc_base + MMIX_VIRT_INTC_CONTEXT_BASE +
                     MMIX_VIRT_INTC_CONTEXT_COMPLETE)

    prefix = [
        insn(PUTI, SR_K, 0, 0),
        *set_octa(R60, timer_compare),
        *set_octa(R61, timer_control),
        *set_octa(R62, timer_status),
        *set_octa(R63, intc_claim),
        *set_octa(R64, intc_complete),
        *set_octa(R65, intc_enable),
        *set_octa(R66, 1 << MMIX_VIRT_TIMER_IRQ_BASE),
        wyde(SETL, R67, MMIX_VIRT_TIMER_IRQ_BASE),
        wyde(SETL, R68, MMIX_VIRT_TIMER_STATUS_PENDING),
        wyde(SETL, R69, MMIX_VIRT_TIMER_CONTROL_ENABLE |
             MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE),
        *set_octa(R70, RK_INTERRUPT_CONTROLLER),
        insn(STTU, R66, R65, R0),
        insn(STOU, R0, R60, R0),
        insn(STOU, R69, R61, R0),
        insn(GET, R20, 0, SR_Q),
        insn(AND, R21, R20, R70),
        branch(BZB, R21, 0xfffe),
        wyde(SETL, R8, 0x55),
        insn(ADDU, R255, R8, R0),
        wyde(SETL, R9, 0x1122),
        insn(PUT, SR_J, 0, R9),
        wyde(SETL, R10, handler),
        insn(PUT, SR_TT, 0, R10),
        insn(PUT, SR_K, 0, R70),
    ]
    first_resume_pc = len(b"".join(prefix))
    prefix.extend([
        insn(ADDUI, R30, R30, 1),
        insn(ADDU, R31, R255, R0),
        insn(GET, R32, 0, SR_K),
        insn(STOU, R0, R60, R0),
        insn(STOU, R69, R61, R0),
    ])
    second_resume_pc = len(b"".join(prefix))
    prefix.extend([
        branch(BZ, R0, 2),
        wyde(SETL, R35, 0xdead),
    ])
    prefix.extend([
        insn(ADDUI, R30, R30, 1),
        insn(ADDU, R33, R255, R0),
        insn(GET, R34, 0, SR_K),
        insn(ADDU, R255, R0, R0),
        halt(),
    ])
    program = program_with_handler(
        prefix,
        handler,
        [
            insn(ADDUI, R50, R50, 1),
            insn(GET, R51, 0, SR_Q),
            insn(LDTU, R52, R63, R0),
            insn(STOU, R0, R61, R0),
            insn(STOU, R68, R62, R0),
            insn(STTU, R67, R64, R0),
            insn(PUTI, SR_Q, 0, 0),
            insn(GET, R53, 0, SR_Q),
            insn(GET, R54, 0, SR_WW),
            insn(GET, R55, 0, SR_XX),
            insn(ADDU, R255, R70, R0),
            insn(RESUME, 0, 0, 1),
        ],
    )
    return program, len(b"".join(prefix)) - 4, first_resume_pc, second_resume_pc


EXTERNAL_DYNAMIC_TRAP_RESUME = external_dynamic_trap_resume_program()

INTERRUPT_TESTS = [
    MMIXTest(
        "masked-interrupt-request",
        MASKED_INTERRUPT_REQUEST[0],
        pc=MASKED_INTERRUPT_REQUEST[1],
        regs={
            R20: RQ_INTERRUPT_CONTROLLER,
            R21: RQ_INTERRUPT_CONTROLLER,
            R22: RQ_INTERRUPT_CONTROLLER,
            R23: MMIX_VIRT_TIMER_IRQ_BASE,
            R24: RQ_INTERRUPT_CONTROLLER,
            R25: 0,
            R26: 0,
        },
    ),
    MMIXTest(
        "external-dynamic-trap",
        EXTERNAL_DYNAMIC_TRAP[0],
        pc=EXTERNAL_DYNAMIC_TRAP[1],
        regs={
            R30: 0,
            R40: RQ_INTERRUPT_CONTROLLER,
            R41: EXTERNAL_DYNAMIC_TRAP[2],
            R42: DYNAMIC_TRAP_RESUME_NEXT,
            R43: 0,
            R44: 0,
            R45: 0x55,
            R46: 0,
            R47: 0x1122,
        },
    ),
    MMIXTest(
        "external-dynamic-trap-resume",
        EXTERNAL_DYNAMIC_TRAP_RESUME[0],
        pc=EXTERNAL_DYNAMIC_TRAP_RESUME[1],
        regs={
            R30: 2,
            R31: 0x55,
            R32: RK_INTERRUPT_CONTROLLER,
            R33: 0x55,
            R34: RK_INTERRUPT_CONTROLLER,
            R35: 0,
            R50: 2,
            R51: RQ_INTERRUPT_CONTROLLER,
            R52: MMIX_VIRT_TIMER_IRQ_BASE,
            R53: 0,
            R54: EXTERNAL_DYNAMIC_TRAP_RESUME[3],
            R55: DYNAMIC_TRAP_RESUME_NEXT,
        },
    ),
]
