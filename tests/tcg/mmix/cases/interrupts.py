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


def dynamic_trap_register_stack_program(depth=10):
    interrupted_base = 0x200
    handler_entry = 0x1000
    handler_body = 0x1100
    handler_calls = 0x1200
    image = bytearray()

    def place(addr, instructions):
        code = b"".join(instructions)

        if len(image) > addr:
            raise AssertionError("dynamic-trap register-stack sections overlap")
        image.extend(insn(SWYM, 0, 0, 0) * ((addr - len(image)) // 4))
        image.extend(code)

    def nested_calls(saved_rj_base, leaf):
        instructions = []

        for level in range(depth):
            instructions.extend([
                insn(GET, saved_rj_base + level, 0, SR_J),
                wyde(SETL, R31, level + 1),
                branch(PUSHJ, R31, 4),
                insn(ADDI, R0, R31, 1),
                insn(PUT, SR_J, 0, saved_rj_base + level),
                insn(POP, 1, 0, 0),
            ])
        instructions.extend(leaf)
        return instructions

    main = [
        *set_octa(R80, RQ_PROGRAM_B),
        wyde(SETL, R81, handler_entry),
        insn(PUT, SR_TT, 0, R81),
        wyde(SETL, R82, 0x55),
        insn(ADDU, R255, R82, R83),
        insn(PUT, SR_K, 0, R80),
    ]
    call_pc = len(b"".join(main))
    main.extend([
        branch(PUSHJ, R31, (interrupted_base - call_pc) // 4),
        insn(ADDU, R90, R31, R83),
        insn(GET, R91, 0, SR_O),
        insn(GET, R92, 0, SR_S),
        insn(GET, R93, 0, SR_L),
        insn(GET, R94, 0, SR_J),
        insn(ADDU, R95, R255, R83),
        insn(GET, R96, 0, SR_K),
        insn(ADDU, R255, R83, R83),
        halt(),
    ])
    place(0, main)

    place(interrupted_base, nested_calls(
        R100,
        [
            wyde(SETL, R0, 0x77),
            insn(GET, R211, 0, SR_O),
            insn(GET, R212, 0, SR_S),
            insn(GET, R213, 0, SR_L),
            insn(GET, R20, 3, SR_M),
            insn(GET, R233, 0, SR_O),
            insn(GET, R234, 0, SR_S),
            insn(GET, R235, 0, SR_L),
            insn(ADDU, R210, R0, R83),
            insn(POP, 1, 0, 0),
        ],
    ))

    place(handler_entry, [
        branch(PUSHJ, R255, (handler_body - handler_entry) // 4),
        insn(GET, R230, 0, SR_O),
        insn(GET, R231, 0, SR_S),
        insn(GET, R232, 0, SR_L),
        insn(PUT, SR_J, 0, R200),
        insn(ADDU, R255, R80, R83),
        insn(RESUME, 0, 0, 1),
    ])

    body = [
        insn(ADDU, R200, R255, R83),
        insn(GET, R201, 0, SR_J),
        insn(GET, R203, 0, SR_O),
        insn(GET, R204, 0, SR_S),
        insn(GET, R205, 0, SR_L),
    ]
    body_call_pc = handler_body + len(b"".join(body))
    body.extend([
        branch(PUSHJ, R31, (handler_calls - body_call_pc) // 4),
        insn(ADDU, R202, R31, R83),
        insn(GET, R206, 0, SR_O),
        insn(GET, R207, 0, SR_S),
        insn(GET, R208, 0, SR_L),
        insn(PUT, SR_J, 0, R201),
        insn(POP, 0, 0, 0),
    ])
    place(handler_body, body)

    place(handler_calls, nested_calls(
        R220,
        [
            wyde(SETL, R0, 1),
            insn(POP, 1, 0, 0),
        ],
    ))

    exit_pc = call_pc + 9 * 4
    interrupted_rj = interrupted_base + (depth - 1) * 6 * 4 + 3 * 4
    return bytes(image), exit_pc, call_pc + 4, interrupted_rj


DYNAMIC_TRAP_REGISTER_STACK = dynamic_trap_register_stack_program()


def spill_fault_resume_program(depth=10, protect_before_push=False):
    sub_base = 0x200
    handler = 0x1000
    page_table = VM_PAGE_TABLE
    stack_pte = page_table + (INITIAL_STACK >> 13) * 8
    physical_stack_pte = (1 << 63) | stack_pte
    stack_page_read_only = INITIAL_STACK | 4
    stack_page_rwx = INITIAL_STACK | 7
    image = bytearray()
    protect_level = 6

    def place(addr, instructions):
        code = b"".join(instructions)

        if len(image) > addr:
            raise AssertionError("spill-fault resume sections overlap")
        image.extend(insn(SWYM, 0, 0, 0) * ((addr - len(image)) // 4))
        image.extend(code)

    main = [
        *set_octa(R240, page_table),
        wyde(SETL, R241, 7),
        insn(STOU, R241, R240, R250),
        *set_octa(R242, stack_pte),
        *set_octa(R243, stack_page_read_only),
        *set_octa(R245, stack_page_rwx),
        insn(STOU, R245 if protect_before_push else R243, R242, R250),
        *set_octa(R244, physical_stack_pte),
        *set_octa(R246, VM_RV_PAGE0),
        wyde(SETL, R247, handler),
        insn(PUT, SR_TT, 0, R247),
        *set_octa(R248, RQ_PROGRAM_W),
        insn(PUT, SR_K, 0, R248),
        *set_octa(R249, RQ_PROGRAM_B),
        insn(PUT, SR_Q, 0, R249),
        insn(PUT, SR_V, 0, R246),
    ]
    call_pc = len(b"".join(main))
    main.extend([
        branch(PUSHJ, R31, (sub_base - call_pc) // 4),
        insn(ADDI, R225, R31, 0),
        insn(GET, R226, 0, SR_O),
        insn(GET, R227, 0, SR_S),
        insn(GET, R228, 0, SR_L),
        insn(GET, R229, 0, SR_Q),
        insn(GET, R230, 0, SR_K),
        halt(),
    ])
    place(0, main)

    nested = []
    for level in range(depth):
        nested.extend((
            insn(GET, R100 + level, 0, SR_J),
            wyde(SETL, R31, level + 1),
        ))
        if protect_before_push and level == protect_level:
            # Local growth leaves PUSHJ one additional spill slot short.
            nested.extend((
                insn(STOUI, R243, R244, 0),
                insn(PUT, SR_V, 0, R246),
            ))
        nested.extend((
            branch(PUSHJ,
                   R255 if protect_before_push and level == protect_level
                   else R31,
                   4),
            insn(ADDI, R0, R31, 1),
            insn(PUT, SR_J, 0, R100 + level),
            insn(POP, 1, 0, 0),
        ))
    nested.extend([
        wyde(SETL, R0, 1),
        insn(POP, 1, 0, 0),
    ])
    place(sub_base, nested)

    place(handler, [
        insn(ADDUI, R220, R220, 1),
        insn(GET, R221, 0, SR_Q),
        insn(GET, R222, 0, SR_XX),
        insn(GET, R223, 0, SR_WW),
        insn(GET, R224, 0, SR_S),
        insn(STOUI, R245, R244, 0),
        insn(GET, R231, 0, SR_K),
        insn(PUT, SR_V, 0, R246),
        insn(PUTI, SR_Q, 0, 0),
        insn(ADDU, R255, R248, R250),
        insn(RESUME, 0, 0, 1),
    ])

    exit_pc = call_pc + 7 * 4
    result = depth + 4 if protect_before_push else depth + 1
    return bytes(image), exit_pc, result


SPILL_FAULT_LOCAL_GROWTH = spill_fault_resume_program()
SPILL_FAULT_PUSHJ = spill_fault_resume_program(protect_before_push=True)


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
    MMIXTest(
        "dynamic-trap-register-stack-roundtrip",
        DYNAMIC_TRAP_REGISTER_STACK[0],
        pc=DYNAMIC_TRAP_REGISTER_STACK[1],
        regs={
            R90: 0x77 + 10,
            R91: INITIAL_STACK,
            R92: INITIAL_STACK,
            R93: 32,
            R94: DYNAMIC_TRAP_REGISTER_STACK[2],
            R95: 0x55,
            R96: RQ_PROGRAM_B,
            R200: DYNAMIC_TRAP_REGISTER_STACK[3],
            R202: 1 + 10,
            R203: INITIAL_STACK + 0xb10,
            R204: INITIAL_STACK + 0x318,
            R205: 0,
            R206: INITIAL_STACK + 0xb10,
            R207: INITIAL_STACK + 0xb10,
            R208: 32,
            R210: 0x77,
            R211: INITIAL_STACK + 0xb00,
            R212: INITIAL_STACK + 0x310,
            R213: 1,
            R230: INITIAL_STACK + 0xb00,
            R231: INITIAL_STACK + 0xb00,
            R232: 1,
            R233: INITIAL_STACK + 0xb00,
            R234: INITIAL_STACK + 0xb00,
            R235: 1,
        },
    ),
    MMIXTest(
        "register-stack-local-growth-spill-fault-resume",
        SPILL_FAULT_LOCAL_GROWTH[0],
        pc=SPILL_FAULT_LOCAL_GROWTH[1],
        regs={
            R220: 1,
            R221: RQ_PROGRAM_B | RQ_PROGRAM_W,
            R222: RQ_PROGRAM_W,
            R223: 0x298,
            R224: INITIAL_STACK,
            R225: SPILL_FAULT_LOCAL_GROWTH[2],
            R226: INITIAL_STACK,
            R227: INITIAL_STACK,
            R228: 32,
            R229: 0,
            R230: RQ_PROGRAM_W,
            R231: 0,
        },
    ),
    MMIXTest(
        "register-stack-pushj-spill-fault-resume",
        SPILL_FAULT_PUSHJ[0],
        pc=SPILL_FAULT_PUSHJ[1],
        regs={
            R220: 1,
            R221: RQ_PROGRAM_B | RQ_PROGRAM_W,
            R222: RQ_PROGRAM_W,
            R223: 0x2a4,
            R224: INITIAL_STACK + 8,
            R225: SPILL_FAULT_PUSHJ[2],
            R226: INITIAL_STACK,
            R227: INITIAL_STACK,
            R228: 32,
            R229: 0,
            R230: RQ_PROGRAM_W,
            R231: 0,
        },
    ),
]
