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
]
