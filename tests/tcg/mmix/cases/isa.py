#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

FORCED_TRANSLATION_MAIN = 0x100
FORCED_TRANSLATION_HANDLER = 0x200
NEGATIVE_FORCED_TRANSLATION_MAIN = 0x8000000000000100
NEGATIVE_FORCED_TRANSLATION_HANDLER = 0x8000000000000200
FORCED_TRANSLATION_VIRTUAL = 0x2000
FORCED_TRANSLATION_PHYSICAL = 0x4000


def forced_data_translation_program(main, handler, initial_value,
                                    extra_regions=()):
    bootstrap = [
        *set_octa(R1, FORCED_TRANSLATION_PHYSICAL),
        *set_octa(R2, initial_value),
        insn(STOU, R2, R1, R0),
        *set_octa(R3, NEGATIVE_FORCED_TRANSLATION_MAIN),
        insn(GO, R4, R3, R0),
    ]
    main_setup = [
        *set_octa(R20, NEGATIVE_FORCED_TRANSLATION_HANDLER),
        insn(PUT, SR_T, 0, R20),
        *set_octa(R21, VM_RV_SOFTWARE),
        insn(PUT, SR_V, 0, R21),
    ]
    return program_with_regions(
        (0, bootstrap),
        (FORCED_TRANSLATION_MAIN, [*main_setup, *main]),
        (FORCED_TRANSLATION_HANDLER, handler),
        *extra_regions,
    )


def forced_instruction_translation_program(target_address, target_regions,
                                           handler):
    bootstrap = [
        *set_octa(R1, NEGATIVE_FORCED_TRANSLATION_MAIN),
        insn(GO, R2, R1, R0),
    ]
    main = [
        *set_octa(R20, NEGATIVE_FORCED_TRANSLATION_HANDLER),
        insn(PUT, SR_T, 0, R20),
        *set_octa(R21, VM_RV_SOFTWARE),
        insn(PUT, SR_V, 0, R21),
        *set_octa(R22, target_address),
        insn(GO, R23, R22, R0),
    ]
    return program_with_regions(
        (0, bootstrap),
        (FORCED_TRANSLATION_MAIN, main),
        (FORCED_TRANSLATION_HANDLER, handler),
        *target_regions,
    )


def forced_stack_spill_fill_program(depth=40):
    sub_base = 0x800
    bootstrap = [
        *set_octa(R1, NEGATIVE_FORCED_TRANSLATION_MAIN),
        insn(GO, R2, R1, R0),
    ]
    main = [
        *set_octa(R20, NEGATIVE_FORCED_TRANSLATION_HANDLER),
        insn(PUT, SR_T, 0, R20),
        *set_octa(R21, VM_RV_SOFTWARE),
        insn(PUT, SR_V, 0, R21),
    ]
    call_pc = FORCED_TRANSLATION_MAIN + len(b"".join(main))
    main.extend([
        branch(PUSHJ, R31, (sub_base - call_pc) // 4),
        insn(ADDI, R225, R31, 0),
        insn(GET, R226, 0, SR_O),
        insn(GET, R227, 0, SR_S),
        insn(GET, R228, 0, SR_L),
        halt(),
    ])

    nested = []
    for level in range(depth):
        nested.extend([
            insn(GET, R100 + level, 0, SR_J),
            wyde(SETL, R31, level + 1),
            branch(PUSHJ, R31, 4),
            insn(ADDI, R0, R31, 1),
            insn(PUT, SR_J, 0, R100 + level),
            insn(POP, 1, 0, 0),
        ])
    nested.extend([
        *set_octa(R150, INITIAL_STACK),
        insn(LDVTS, R151, R150, R0),
        *set_octa(R152, INITIAL_STACK + 0x2000),
        insn(LDVTS, R153, R152, R0),
        wyde(SETL, R0, 1),
        insn(POP, 1, 0, 0),
    ])

    handler = [
        insn(GET, R240, R0, SR_YY),
        *set_octa(R241, 0x1fff),
        insn(ANDN, R242, R240, R241),
        insn(ORI, R242, R242, 7),
        insn(PUT, SR_ZZ, R0, R242),
        insn(ADDI, R250, R250, 1),
        wyde(SETL, R255, 0),
        insn(RESUME, R0, R0, 1),
    ]
    image = program_with_regions(
        (0, bootstrap),
        (FORCED_TRANSLATION_MAIN, main),
        (FORCED_TRANSLATION_HANDLER, handler),
        (sub_base, nested),
    )
    return image, NEGATIVE_FORCED_TRANSLATION_MAIN + (len(main) - 1) * 4


def forced_stack_save_unsave_program():
    count_address = 0x8000000000003000
    bootstrap = [
        *set_octa(R1, NEGATIVE_FORCED_TRANSLATION_MAIN),
        insn(GO, R2, R1, R0),
    ]
    main = [
        *set_octa(R20, NEGATIVE_FORCED_TRANSLATION_HANDLER),
        insn(PUT, SR_T, 0, R20),
        *set_octa(R21, VM_RV_SOFTWARE),
        insn(PUT, SR_V, 0, R21),
        wyde(SETL, R0, 0x11),
        wyde(SETL, R1, 0x22),
        *set_octa(R40, 0x1122334455667788),
        insn(SAVE, R32, 0, 0),
        insn(ADDUI, R60, R32, 0),
        *set_octa(R61, INITIAL_STACK),
        insn(LDVTS, R62, R61, R0),
        wyde(SETL, R0, 0xaa),
        wyde(SETL, R1, 0xbb),
        wyde(SETL, R40, 0),
        insn(UNSAVE, R0, R0, R60),
        insn(ADDI, R70, R0, 0),
        insn(ADDI, R71, R1, 0),
        insn(ADDUI, R72, R40, 0),
        *set_octa(R73, count_address),
        insn(LDOUI, R74, R73, 0),
        halt(),
    ]
    handler = [
        insn(GET, R240, R0, SR_YY),
        *set_octa(R241, 0x1fff),
        insn(ANDN, R242, R240, R241),
        insn(ORI, R242, R242, 7),
        insn(PUT, SR_ZZ, R0, R242),
        *set_octa(R245, count_address),
        insn(LDOUI, R246, R245, 0),
        insn(ADDI, R246, R246, 1),
        insn(STOUI, R246, R245, 0),
        wyde(SETL, R255, 0),
        insn(RESUME, R0, R0, 1),
    ]
    image = program_with_regions(
        (0, bootstrap),
        (FORCED_TRANSLATION_MAIN, main),
        (FORCED_TRANSLATION_HANDLER, handler),
    )
    return image, NEGATIVE_FORCED_TRANSLATION_MAIN + (len(main) - 1) * 4


def forced_translation_nested_handler_program(depth=40, handler_depth=10):
    sub_base = 0x800
    outer_handler = 0x1000
    handler_body = 0x1100
    handler_calls = 0x1200
    bootstrap = [
        *set_octa(R1, NEGATIVE_FORCED_TRANSLATION_MAIN),
        insn(GO, R2, R1, R0),
    ]
    main = [
        *set_octa(R20, NEGATIVE_FORCED_TRANSLATION_HANDLER),
        insn(PUT, SR_T, 0, R20),
        *set_octa(R21, 0x8000000000001000),
        insn(PUT, SR_TT, 0, R21),
        *set_octa(R80, RQ_PROGRAM_B),
        insn(PUT, SR_K, 0, R80),
        wyde(SETL, R255, 0x55),
        *set_octa(R23, VM_RV_SOFTWARE),
        insn(PUT, SR_V, 0, R23),
    ]
    call_pc = FORCED_TRANSLATION_MAIN + len(b"".join(main))
    main.extend([
        branch(PUSHJ, R31, (sub_base - call_pc) // 4),
        insn(ADDI, R225, R31, 0),
        insn(GET, R226, 0, SR_O),
        insn(GET, R227, 0, SR_S),
        insn(GET, R228, 0, SR_L),
        insn(ADDUI, R229, R255, 0),
        insn(GET, R230, 0, SR_K),
        wyde(SETL, R255, 0),
        halt(),
    ])

    nested = []
    for level in range(depth):
        nested.extend([
            insn(GET, R100 + level, 0, SR_J),
            wyde(SETL, R31, level + 1),
            branch(PUSHJ, R31, 4),
            insn(ADDI, R0, R31, 1),
            insn(PUT, SR_J, 0, R100 + level),
            insn(POP, 1, 0, 0),
        ])
    nested.extend([
        *set_octa(R150, INITIAL_STACK),
        insn(LDVTS, R151, R150, R0),
        *set_octa(R152, INITIAL_STACK + 0x2000),
        insn(LDVTS, R153, R152, R0),
    ])
    illegal_pc = sub_base + len(b"".join(nested))
    illegal = insn(GET, R20, 3, SR_M)
    nested.extend([
        illegal,
        insn(GET, R170, 0, SR_WW),
        insn(GET, R171, 0, SR_XX),
        insn(GET, R172, 0, SR_YY),
        insn(GET, R173, 0, SR_ZZ),
        insn(GET, R174, 0, SR_BB),
        wyde(SETL, R0, 1),
        insn(POP, 1, 0, 0),
    ])

    forced_handler = [
        insn(GET, R240, R0, SR_YY),
        *set_octa(R241, 0x1fff),
        insn(ANDN, R242, R240, R241),
        insn(ORI, R242, R242, 7),
        insn(PUT, SR_ZZ, R0, R242),
        insn(ADDI, R250, R250, 1),
        insn(ADDUI, R255, R80, 0),
        insn(RESUME, R0, R0, 1),
    ]
    handler = [
        insn(GET, R180, 0, SR_WW),
        insn(GET, R181, 0, SR_XX),
        insn(GET, R182, 0, SR_YY),
        insn(GET, R183, 0, SR_ZZ),
        insn(GET, R184, 0, SR_BB),
        insn(ADDUI, R185, R255, 0),
    ]
    handler_call_pc = outer_handler + len(b"".join(handler))
    handler.extend([
        branch(PUSHJ, R255, (handler_body - handler_call_pc) // 4),
        insn(PUT, SR_WW, 0, R180),
        insn(PUT, SR_XX, 0, R181),
        insn(PUT, SR_YY, 0, R182),
        insn(PUT, SR_ZZ, 0, R183),
        insn(PUT, SR_BB, 0, R184),
        insn(PUT, SR_J, 0, R185),
        insn(ADDUI, R255, R80, 0),
        insn(RESUME, R0, R0, 1),
    ])

    body = [insn(GET, R200, 0, SR_J)]
    body_call_pc = handler_body + len(b"".join(body))
    body.extend([
        branch(PUSHJ, R31, (handler_calls - body_call_pc) // 4),
        insn(PUT, SR_J, 0, R200),
        insn(POP, 0, 0, 0),
    ])
    calls = []
    for level in range(handler_depth):
        calls.extend([
            insn(GET, R201 + level, 0, SR_J),
            wyde(SETL, R31, level + 1),
            branch(PUSHJ, R31, 4),
            insn(ADDI, R0, R31, 1),
            insn(PUT, SR_J, 0, R201 + level),
            insn(POP, 1, 0, 0),
        ])
    calls.extend([wyde(SETL, R0, 1), insn(POP, 1, 0, 0)])

    image = program_with_regions(
        (0, bootstrap),
        (FORCED_TRANSLATION_MAIN, main),
        (FORCED_TRANSLATION_HANDLER, forced_handler),
        (sub_base, nested),
        (outer_handler, handler),
        (handler_body, body),
        (handler_calls, calls),
    )
    exit_pc = NEGATIVE_FORCED_TRANSLATION_MAIN + (len(main) - 1) * 4
    return image, exit_pc, 0x8000000000000000 | illegal_pc, illegal


FORCED_STACK_SPILL_FILL = forced_stack_spill_fill_program()
FORCED_STACK_SAVE_UNSAVE = forced_stack_save_unsave_program()
FORCED_TRANSLATION_NESTED_HANDLER = forced_translation_nested_handler_program()


def invalid_forced_data_translation_test(name, main, pte, expected_where,
                                         expected_exec, initial_value):
    return MMIXTest(
        name,
        forced_data_translation_program(
            main,
            [
                insn(GET, R40, R0, SR_WW),
                insn(GET, R41, R0, SR_XX),
                insn(GET, R42, R0, SR_YY),
                *set_octa(R50, pte),
                insn(PUT, SR_ZZ, R0, R50),
                insn(RESUME, R0, R0, 1),
                insn(GET, R46, R0, SR_Q),
                *set_octa(R51, 0x8000000000004000),
                insn(LDOU, R47, R51, R0),
                halt(),
            ],
            initial_value,
        ),
        pc=0x800000000000023c,
        regs={
            R40: expected_where,
            R41: expected_exec,
            R42: FORCED_TRANSLATION_VIRTUAL,
            R46: RQ_PROGRAM_B,
            R47: initial_value,
        },
    )


def rule_break_enabled_test(name, instruction, *, setup=(), y=0, z=0,
                            handler_checks=(), regs=None, old_r255=0):
    handler = 0x100
    prefix = [
        *setup,
        wyde(SETL, R10, handler),
        insn(PUT, SR_TT, 0, R10),
        *set_octa(R2, RQ_PROGRAM_B),
        insn(PUT, SR_K, 0, R2),
    ]
    invalid_pc = len(b"".join(prefix))
    prefix.extend([
        instruction,
        wyde(SETL, R30, 0xdead),
    ])
    trap_handler = [
        insn(GET, R240, 0, SR_Q),
        insn(GET, R241, 0, SR_WW),
        insn(GET, R242, 0, SR_XX),
        insn(GET, R243, 0, SR_YY),
        insn(GET, R244, 0, SR_ZZ),
        insn(GET, R245, 0, SR_BB),
        insn(GET, R246, 0, SR_K),
        insn(ADDU, R247, R255, R0),
        *handler_checks,
        halt(),
    ]
    expected = {
        R30: 0,
        R240: RQ_PROGRAM_B,
        R241: invalid_pc + 4,
        R242: DYNAMIC_TRAP_RESUME_NEXT | RQ_PROGRAM_B |
              int.from_bytes(instruction, "big"),
        R243: y,
        R244: z,
        R245: old_r255,
        R246: 0,
        R247: 0,
    }
    if regs is not None:
        expected.update(regs)
    return MMIXTest(
        name,
        program_with_handler(prefix, handler, trap_handler),
        pc=handler + (len(trap_handler) - 1) * 4,
        regs=expected,
    )


def rule_breaks_masked_program():
    program = [
        wyde(SETL, R20, 0x77),
        insn(PUTI, SR_M, 0, 0x55),
        insn(GET, R21, 0, SR_O),
        insn(GET, R22, 0, SR_S),
    ]
    invalid = [
        insn(GET, R20, 3, SR_M),
        insn(GET, R20, 0, 0xff),
        insn(PUT, SR_M, 3, R4),
        insn(PUT, 0xff, 0, R4),
        insn(PUTI, SR_M, 3, 0xaa),
        insn(PUTI, 0xff, 0, 0xaa),
        insn(SAVE, R32, 3, 0),
        insn(UNSAVE, 3, 0, R32),
        insn(RESUME, 3, 0, 0),
        insn(RESUME, 0, 0, 2),
        jump(SYNC, 8),
    ]
    for instruction in invalid:
        program.extend([instruction, insn(ADDUI, R30, R30, 1)])

    program.extend([
        *set_octa(R40, 0x0400000000000000),
        insn(PUT, SR_XX, 0, R40),
        wyde(SETL, R41, 0x66),
        insn(PUT, SR_BB, 0, R41),
        wyde(SETL, R255, 0x55),
        insn(RESUME, 0, 0, 1),
        insn(ADDUI, R30, R30, 1),
        *set_octa(R40, 0x0300000000000000),
        insn(PUT, SR_X, 0, R40),
        insn(RESUME, 0, 0, 0),
        insn(ADDUI, R30, R30, 1),
        insn(GET, R31, 0, SR_M),
        insn(GET, R24, 0, SR_O),
        insn(GET, R25, 0, SR_S),
        insn(CMP, R32, R21, R24),
        insn(CMP, R33, R22, R25),
        insn(GET, R35, 0, SR_Q),
        insn(GET, R36, 0, SR_K),
        insn(GET, R37, 0, SR_BB),
        insn(ADDU, R38, R255, R0),
        wyde(SETL, R255, 0),
        halt(),
    ])
    return b"".join(program), (len(program) - 1) * 4


RULE_BREAKS_MASKED = rule_breaks_masked_program()


def runtime_rule_breaks_masked_program():
    program = [
        insn(GET, R210, 0, SR_N),
        insn(GET, R211, 0, SR_O),
        insn(GET, R212, 0, SR_S),
        insn(GET, R213, 0, SR_G),
    ]
    invalid_puts = [
        insn(PUTI, SR_N, 0, 1),
        insn(PUTI, SR_O, 0, 1),
        insn(PUTI, SR_S, 0, 1),
        insn(PUTI, SR_G, 0, 31),
    ]
    for instruction in invalid_puts:
        program.extend([instruction, insn(ADDUI, R30, R30, 1)])

    program.extend([
        wyde(SETL, R5, 0x100),
        insn(PUT, SR_G, 0, R5),
        insn(ADDUI, R30, R30, 1),
        insn(GET, R206, 0, SR_L),
        insn(PUTI, SR_L, 0, 0xff),
        insn(GET, R207, 0, SR_L),
        insn(CMP, R208, R206, R207),
        insn(ADDUI, R30, R30, 1),
        insn(SAVE, R0, 0, 0),
        insn(ADDUI, R30, R30, 1),
        *set_octa(R200, 0x1111222233334444),
        *set_octa(R201, 0x5555666677778888),
        *set_octa(R202, 0x9999aaaabbbbcccc),
        *set_octa(R5, f64(1.5)),
        insn(FINT, R200, 5, R5),
        insn(ADDUI, R30, R30, 1),
        insn(FIX, R201, 6, R5),
        insn(ADDUI, R30, R30, 1),
        wyde(SETL, R6, 7),
        insn(FLOT, R202, 7, R6),
        insn(ADDUI, R30, R30, 1),
        insn(GET, R214, 0, SR_N),
        insn(GET, R215, 0, SR_O),
        insn(GET, R216, 0, SR_S),
        insn(GET, R217, 0, SR_G),
        insn(CMP, R218, R210, R214),
        insn(CMP, R219, R211, R215),
        insn(CMP, R220, R212, R216),
        insn(CMP, R221, R213, R217),
        insn(GET, R222, 0, SR_Q),
        insn(GET, R223, 0, SR_K),
        halt(),
    ])
    return b"".join(program), (len(program) - 1) * 4


RUNTIME_RULE_BREAKS_MASKED = runtime_rule_breaks_masked_program()


def resume1_ropcode0_replay_program():
    saved_insn = int.from_bytes(insn(ADDI, R10, R10, 7), "big")
    prefix = [
        *set_octa(R1, NEGATIVE_HANDLER),
        insn(PUT, SR_TT, 0, R1),
        *set_octa(R2, RQ_PROGRAM_B),
        insn(PUT, SR_K, 0, R2),
        wyde(SETL, R255, 0x55),
        wyde(SETL, R10, 0x20),
    ]
    replay_pc = len(b"".join(prefix))
    prefix.extend([
        jump(SYNC, 8),
        insn(GET, R40, 0, SR_WW),
        insn(GET, R41, 0, SR_XX),
        insn(GET, R42, 0, SR_YY),
        insn(GET, R43, 0, SR_ZZ),
        insn(GET, R44, 0, SR_K),
        insn(ADDU, R45, R255, R0),
        wyde(SETL, R255, 0),
        halt(),
    ])
    program = program_with_handler(
        prefix,
        NEGATIVE_HANDLER & ~(1 << 63),
        [
            *set_octa(R3, saved_insn),
            insn(PUT, SR_XX, 0, R3),
            wyde(SETL, R4, 0xaa),
            insn(PUT, SR_YY, 0, R4),
            wyde(SETL, R5, 0xbb),
            insn(PUT, SR_ZZ, 0, R5),
            insn(ADDU, R255, R2, R0),
            insn(RESUME, 0, 0, 1),
        ],
    )
    return program, replay_pc + 8 * 4, replay_pc + 4, saved_insn


RESUME1_ROPCODE0_REPLAY = resume1_ropcode0_replay_program()


def resume1_nested_replay_trap_program():
    handler_phys = 0x200
    handler = (1 << 63) | handler_phys
    saved_insn = int.from_bytes(insn(GET, R10, 3, SR_M), "big")
    prefix = [
        *set_octa(R1, handler),
        insn(PUT, SR_TT, 0, R1),
        *set_octa(R2, RQ_PROGRAM_B),
        insn(PUT, SR_K, 0, R2),
        *set_octa(R3, saved_insn),
        wyde(SETL, R255, 0x55),
    ]
    replay_pc = len(b"".join(prefix))
    prefix.extend([
        jump(SYNC, 8),
        insn(GET, R40, 0, SR_K),
        insn(ADDU, R41, R255, R0),
        wyde(SETL, R255, 0),
        halt(),
    ])

    handler_code = [
        insn(ADDUI, R50, R50, 1),
        insn(CMPI, R51, R50, 1),
        None,
        insn(PUT, SR_XX, 0, R3),
        insn(ADDU, R255, R2, R0),
        insn(RESUME, 0, 0, 1),
    ]
    nested_index = len(handler_code)
    handler_code[2] = branch(BNZ, R51, nested_index - 2)
    handler_code.extend([
        insn(GET, R52, 0, SR_WW),
        insn(GET, R53, 0, SR_XX),
        insn(ADDU, R255, R2, R0),
        insn(RESUME, 0, 0, 1),
    ])

    program = program_with_handler(prefix, handler_phys, handler_code)
    return program, replay_pc + 4 * 4, replay_pc + 4, saved_insn


RESUME1_NESTED_REPLAY_TRAP = resume1_nested_replay_trap_program()


def resume0_replay_setup(instruction, continuation, *, setup=()):
    return [
        *setup,
        *set_octa(R1, continuation),
        insn(PUT, SR_W, 0, R1),
        *set_octa(R2, int.from_bytes(instruction, "big")),
        insn(PUT, SR_X, 0, R2),
        insn(RESUME, 0, 0, 0),
    ]


def resume0_branch_replay_program(taken):
    replay_pc = 0x7c
    continuation = replay_pc + 4
    target = 0x90
    saved_insn = branch(BZ, R10, (target - replay_pc) // 4)
    setup = [wyde(SETL, R10, 0 if taken else 1)]

    return program_with_regions(
        (0, resume0_replay_setup(saved_insn, continuation, setup=setup)),
        (replay_pc, [wyde(SETL, R12, 0x77)]),
        (continuation, [wyde(SETL, R13, 0x55), halt()]),
        (
            target,
            [
                wyde(SETL, R11, 0x33),
                jump(JMPB, ((replay_pc - target - 4) // 4) & 0xffffff),
            ],
        ),
    )


def resume0_arithmetic_trip_replay_program():
    replay_pc = 0x17c
    continuation = replay_pc + 4
    saved_insn = insn(ADD, R12, R10, R11)
    handler = [
        insn(GET, R40, 0, SR_W),
        insn(GET, R41, 0, SR_X),
        insn(GET, R42, 0, SR_Y),
        insn(GET, R43, 0, SR_Z),
        insn(ADDUI, R50, R50, 1),
        insn(PUTI, SR_A, 0, 0),
        wyde(SETH, R51, 0x8000),
        insn(ANDN, R52, R41, R51),
        insn(PUT, SR_X, 0, R52),
        insn(RESUME, 0, 0, 0),
    ]
    setup = [
        *set_octa(R10, 0x7fffffffffffffff),
        wyde(SETL, R11, 1),
        wyde(SETL, R3, RA_EVENT_V << RA_ENABLE_SHIFT),
        insn(PUT, SR_A, 0, R3),
    ]

    return program_with_regions(
        (0, [jump(JMP, 0x100 // 4)]),
        (32, handler),
        (0x100, resume0_replay_setup(saved_insn, continuation, setup=setup)),
        (replay_pc, [wyde(SETL, R12, 0xdead)]),
        (continuation, [insn(GET, R44, 0, SR_A), halt()]),
    )


def resume0_explicit_trip_replay_program():
    replay_pc = 0x17c
    continuation = replay_pc + 4
    saved_insn = insn(TRIP, 7, R10, R11)
    handler = [
        branch(BZ, R50, 0x100 // 4),
        insn(GET, R40, 0, SR_W),
        insn(GET, R41, 0, SR_X),
        insn(GET, R42, 0, SR_Y),
        insn(GET, R43, 0, SR_Z),
        insn(RESUME, 0, 0, 0),
    ]
    setup = [
        wyde(SETL, R50, 1),
        wyde(SETL, R10, 0xaa),
        wyde(SETL, R11, 0xbb),
    ]

    return program_with_regions(
        (0, handler),
        (0x100, resume0_replay_setup(saved_insn, continuation, setup=setup)),
        (replay_pc, [wyde(SETL, R12, 0xdead)]),
        (continuation, [wyde(SETL, R13, 0x55), halt()]),
    )


def resume0_explicit_trap_replay_program():
    replay_pc = 0xfc
    continuation = replay_pc + 4
    handler_phys = 0x200
    handler = (1 << 63) | handler_phys
    saved_insn = insn(TRAP, 1, R10, R11)
    setup = [
        *set_octa(R20, handler),
        insn(PUT, SR_T, 0, R20),
        *set_octa(R21, RQ_PROGRAM_B),
        insn(PUT, SR_K, 0, R21),
        wyde(SETL, R10, 0xaa),
        wyde(SETL, R11, 0xbb),
        wyde(SETL, R22, 0xdd),
        insn(PUT, SR_J, 0, R22),
        wyde(SETL, R255, 0xcc),
    ]
    trap_handler = [
        insn(GET, R40, 0, SR_WW),
        insn(GET, R41, 0, SR_XX),
        insn(GET, R42, 0, SR_YY),
        insn(GET, R43, 0, SR_ZZ),
        insn(GET, R44, 0, SR_BB),
        insn(ADDU, R45, R255, R0),
        insn(ADDU, R255, R21, R0),
        insn(RESUME, 0, 0, 1),
    ]

    return program_with_regions(
        (0, resume0_replay_setup(saved_insn, continuation, setup=setup)),
        (replay_pc, [wyde(SETL, R12, 0xdead)]),
        (
            continuation,
            [
                insn(GET, R46, 0, SR_K),
                insn(ADDU, R47, R255, R0),
                wyde(SETL, R255, 0),
                halt(),
            ],
        ),
        (handler_phys, trap_handler),
    )


RESUME0_BRANCH_TAKEN_REPLAY = resume0_branch_replay_program(True)
RESUME0_BRANCH_NOT_TAKEN_REPLAY = resume0_branch_replay_program(False)
RESUME0_ARITHMETIC_TRIP_REPLAY = resume0_arithmetic_trip_replay_program()
RESUME0_EXPLICIT_TRIP_REPLAY = resume0_explicit_trip_replay_program()
RESUME0_EXPLICIT_TRAP_REPLAY = resume0_explicit_trap_replay_program()


REPLAY_DATA_VIRTUAL = 0x2000
REPLAY_DATA_PHYSICAL = 0x6000
REPLAY_DATA_PTE = VM_PAGE_TABLE + 8
REPLAY_DATA_HANDLER_PHYS = 0x400
REPLAY_DATA_HANDLER = (1 << 63) | REPLAY_DATA_HANDLER_PHYS
REPLAY_DATA_VALUE = 0x80ff12343fc00000


def recoverable_data_access_handler(*, repeated=False):
    handler = [
        insn(GET, R200, 0, SR_WW),
        insn(GET, R201, 0, SR_XX),
        insn(GET, R202, 0, SR_YY),
        insn(GET, R203, 0, SR_ZZ),
        insn(ADDU, R204, R111, R0),
        *set_octa(R205, (1 << 63) | REPLAY_DATA_PHYSICAL),
        insn(LDOU, R207, R205, R0),
        insn(ADDUI, R210, R210, 1),
    ]

    if repeated:
        handler.append(insn(CMPI, R211, R210, 1))
        branch_index = len(handler)
        handler.extend([
            None,
            insn(ADDU, R255, R103, R0),
            insn(RESUME, 0, 0, 1),
        ])
        repair_index = len(handler)
        handler[branch_index] = branch(BNZ, R211,
                                       repair_index - branch_index)

    handler.extend([
        *set_octa(R220, (1 << 63) | REPLAY_DATA_PTE),
        *set_octa(R221, REPLAY_DATA_PHYSICAL | 7),
        insn(STOU, R221, R220, R0),
        insn(PUT, SR_V, 0, R102),
        insn(ADDU, R255, R103, R0),
        insn(RESUME, 0, 0, 1),
    ])
    return handler


def recoverable_data_access_program(instruction, cause, initial_pte,
                                    initial_memory, operand_setup, post,
                                    *, repeated=False):
    prefix = [
        *set_octa(R100, VM_PAGE_TABLE),
        wyde(SETL, R101, 7),
        insn(STOU, R101, R100, R0),
        *set_octa(R100, REPLAY_DATA_PTE),
        *set_octa(R101, initial_pte),
        insn(STOU, R101, R100, R0),
        *set_octa(R100, REPLAY_DATA_PHYSICAL),
        *set_octa(R101, initial_memory),
        insn(STOU, R101, R100, R0),
        *set_octa(R100, REPLAY_DATA_HANDLER),
        insn(PUT, SR_TT, 0, R100),
        *set_octa(R102, VM_RV_PAGE0),
        insn(PUT, SR_V, 0, R102),
        *set_octa(R103, cause),
        insn(PUT, SR_K, 0, R103),
        *operand_setup,
    ]
    fault_pc = len(b"".join(prefix))
    prefix.extend([instruction, *post])
    exit_pc = fault_pc + len(b"".join(post))

    return (
        program_with_regions(
            (0, prefix),
            (
                REPLAY_DATA_HANDLER_PHYS,
                recoverable_data_access_handler(repeated=repeated),
            ),
        ),
        fault_pc,
        exit_pc,
    )


def recoverable_load_test(name, instruction, address, expected,
                          *, missing=False, repeated=False):
    initial_pte = 0 if missing else REPLAY_DATA_PHYSICAL | 3
    marker = 0xdeadbeefcafebabe
    program, fault_pc, exit_pc = recoverable_data_access_program(
        instruction,
        RQ_PROGRAM_R,
        initial_pte,
        REPLAY_DATA_VALUE,
        [*set_octa(R110, address), *set_octa(R111, marker)],
        [halt()],
        repeated=repeated,
    )
    return MMIXTest(
        name,
        program,
        pc=exit_pc,
        regs={
            R111: expected,
            R200: fault_pc + 4,
            R201: RQ_PROGRAM_R | int.from_bytes(instruction, "big"),
            R202: address,
            R203: 0,
            R204: marker,
            R207: REPLAY_DATA_VALUE,
            R210: 2 if repeated else 1,
        },
    )


def recoverable_store_test(name, instruction, address, source, expected,
                           *, missing=False, repeated=False, setup=(),
                           saved_value=None, final_source=None, regs=None,
                           cause=RQ_PROGRAM_W):
    if saved_value is None:
        saved_value = source
    if final_source is None:
        final_source = source
    initial_pte = 0 if missing else REPLAY_DATA_PHYSICAL | 5
    program, fault_pc, exit_pc = recoverable_data_access_program(
        instruction,
        cause,
        initial_pte,
        REPLAY_DATA_VALUE,
        [
            *set_octa(R110, address),
            *set_octa(R111, source),
            *setup,
        ],
        [
            *set_octa(R113, REPLAY_DATA_VIRTUAL),
            insn(LDOU, R112, R113, R0),
            halt(),
        ],
        repeated=repeated,
    )
    expected_regs = {
        R111: final_source,
        R112: expected,
        R200: fault_pc + 4,
        R201: cause | int.from_bytes(instruction, "big"),
        R202: address,
        R203: saved_value,
        R204: source,
        R207: REPLAY_DATA_VALUE,
        R210: 2 if repeated else 1,
    }
    if regs is not None:
        expected_regs.update(regs)
    return MMIXTest(
        name,
        program,
        pc=exit_pc,
        regs=expected_regs,
    )


RECOVERABLE_LOAD_REPLAY_TESTS = [
    recoverable_load_test(
        "resume-ropcode0-load-byte-missing-page",
        insn(LDB, R111, R110, R0),
        REPLAY_DATA_VIRTUAL,
        0xffffffffffffff80,
        missing=True,
        repeated=True,
    ),
    recoverable_load_test(
        "resume-ropcode0-load-wyde-repaired-permission",
        insn(LDWU, R111, R110, R0),
        REPLAY_DATA_VIRTUAL + 2,
        0x1234,
    ),
    recoverable_load_test(
        "resume-ropcode0-load-tetra-repaired-permission",
        insn(LDTU, R111, R110, R0),
        REPLAY_DATA_VIRTUAL + 4,
        0x3fc00000,
    ),
    recoverable_load_test(
        "resume-ropcode0-load-octa-repaired-permission",
        insn(LDOU, R111, R110, R0),
        REPLAY_DATA_VIRTUAL,
        REPLAY_DATA_VALUE,
    ),
    recoverable_load_test(
        "resume-ropcode0-load-short-float-repaired-permission",
        insn(LDSF, R111, R110, R0),
        REPLAY_DATA_VIRTUAL + 4,
        f64(1.5),
    ),
]


RECOVERABLE_STORE_REPLAY_TESTS = [
    recoverable_store_test(
        "resume-ropcode0-store-byte-missing-page",
        insn(STBU, R111, R110, R0),
        REPLAY_DATA_VIRTUAL + 1,
        0xaa,
        0x80aa12343fc00000,
        missing=True,
    ),
    recoverable_store_test(
        "resume-ropcode0-store-wyde-repaired-permission",
        insn(STWU, R111, R110, R0),
        REPLAY_DATA_VIRTUAL + 2,
        0xbbcc,
        0x80ffbbcc3fc00000,
    ),
    recoverable_store_test(
        "resume-ropcode0-store-tetra-repaired-permission",
        insn(STTU, R111, R110, R0),
        REPLAY_DATA_VIRTUAL + 4,
        0xddeeff00,
        0x80ff1234ddeeff00,
    ),
    recoverable_store_test(
        "resume-ropcode0-store-octa-repaired-permission",
        insn(STOU, R111, R110, R0),
        REPLAY_DATA_VIRTUAL,
        0x0102030405060708,
        0x0102030405060708,
    ),
    recoverable_store_test(
        "resume-ropcode0-store-short-float-repaired-permission",
        insn(STSF, R111, R110, R0),
        REPLAY_DATA_VIRTUAL + 4,
        f64(1.5),
        0x80ff12343fc00000,
        saved_value=0x3fc00000,
    ),
    recoverable_store_test(
        "resume-ropcode0-store-constant-repaired-permission",
        insn(STCO, 0x5a, R110, R0),
        REPLAY_DATA_VIRTUAL,
        0x5a,
        0x5a,
    ),
    recoverable_store_test(
        "resume-ropcode0-cswap-repeated-fault-exactly-once",
        insn(CSWAP, R111, R110, R0),
        REPLAY_DATA_VIRTUAL,
        0x0102030405060708,
        0x0102030405060708,
        missing=True,
        repeated=True,
        final_source=1,
        cause=RQ_PROGRAM_R,
        setup=[
            *set_octa(R114, REPLAY_DATA_VALUE),
            insn(PUT, SR_P, 0, R114),
        ],
        regs={R114: REPLAY_DATA_VALUE},
    ),
]


ISA_TESTS = [
    MMIXTest(
        "raw-image-startup-registers",
        b"".join(
            [
                insn(ADDI, R32, R0, 0),
                insn(ADDI, R33, R1, 0),
                insn(GET, R34, 0, SR_L),
                halt(),
            ]
        ),
        pc=0x0c,
        regs={R32: 0, R33: 0, R34: 0},
    ),
    MMIXTest(
        "alu-logical",
        b"".join(
            [
                insn(ADDI, R1, R0, 5),
                insn(ADDI, R2, R0, 7),
                insn(ADD, R3, R1, R2),
                insn(SUBI, R4, R3, 2),
                insn(ORI, R5, R4, 0x80),
                insn(XORI, R6, R5, 0xff),
                insn(ANDI, R7, R6, 0x0f),
                halt(),
            ]
        ),
        pc=0x1c,
        regs={R1: 5, R2: 7, R3: 0x0c, R4: 0x0a, R5: 0x8a, R6: 0x75, R7: 5},
    ),
    MMIXTest(
        "compare",
        b"".join(
            [
                insn(ADDI, R1, R0, 5),
                insn(ADDI, R2, R0, 7),
                insn(CMP, R3, R1, R2),
                insn(CMP, R4, R2, R2),
                insn(CMP, R5, R2, R1),
                insn(SUBI, R6, R0, 1),
                insn(CMPU, R7, R6, R1),
                halt(),
            ]
        ),
        pc=0x1c,
        regs={R3: MASK64, R4: 0, R5: 1, R6: MASK64, R7: 1},
    ),
    MMIXTest(
        "compare-immediate-boundaries",
        b"".join(
            [
                wyde(SETH, R1, 0xffff),
                wyde(INCMH, R1, 0xffff),
                wyde(INCML, R1, 0xffff),
                wyde(INCL, R1, 0xffff),
                wyde(SETH, R2, 0x8000),
                wyde(SETH, R3, 0x7fff),
                wyde(INCMH, R3, 0xffff),
                wyde(INCML, R3, 0xffff),
                wyde(INCL, R3, 0xffff),
                insn(CMPI, R4, R0, 0),
                insn(CMPI, R5, R1, 0),
                insn(CMPI, R6, R3, 0),
                insn(CMP, R7, R2, R3),
                insn(CMPU, R8, R2, R3),
                insn(CMPUI, R9, R0, 1),
                insn(CMPUI, R10, R1, 0xff),
                insn(CMPUI, R11, R0, 0),
                halt(),
            ]
        ),
        pc=0x44,
        regs={
            R1: MASK64,
            R2: 0x8000000000000000,
            R3: 0x7fffffffffffffff,
            R4: 0,
            R5: MASK64,
            R6: 1,
            R7: MASK64,
            R8: 1,
            R9: MASK64,
            R10: 1,
            R11: 0,
        },
    ),
    MMIXTest(
        "branch-taken",
        b"".join(
            [
                insn(ADDI, R1, R0, 0),
                branch(BZ, R1, 2),
                insn(ADDI, R2, R0, 9),      # skipped
                insn(ADDI, R2, R0, 5),
                halt(),
            ]
        ),
        pc=0x10,
        regs={R2: 5},
    ),
    MMIXTest(
        "branch-not-taken",
        b"".join(
            [
                insn(ADDI, R1, R0, 1),
                branch(BZ, R1, 2),
                insn(ADDI, R2, R0, 9),
                halt(),
            ]
        ),
        pc=0x0c,
        regs={R2: 9},
    ),
    MMIXTest(
        "branch-backward",
        b"".join(
            [
                insn(ADDI, R1, R0, 0),
                insn(ADDI, R2, R0, 3),
                insn(ADDI, R1, R1, 1),
                insn(SUBI, R2, R2, 1),
                branch(BNZB, R2, 0xfffe),
                halt(),
            ]
        ),
        pc=0x14,
        regs={R1: 3, R2: 0},
    ),
    MMIXTest(
        "branch-existing-variants",
        b"".join(
            [
                branch(BZ, R0, 3),
                wyde(SETL, R2, 1),  # target
                halt(),
                branch(BZB, R0, 0xfffe),
            ]
        ),
        pc=0x08,
        regs={R2: 1},
    ),
    MMIXTest(
        "branch-bnz-forward",
        b"".join(
            [
                wyde(SETL, R1, 1),
                branch(BNZ, R1, 2),
                wyde(SETL, R2, 9),         # skipped
                wyde(SETL, R2, 5),
                halt(),
            ]
        ),
        pc=0x10,
        regs={R2: 5},
    ),
    MMIXTest(
        "ordinary-branches-true",
        b"".join(
            [
                insn(SUBI, R1, R0, 1),
                wyde(SETL, R3, 5),
                wyde(SETL, R4, 4),
                wyde(SETL, R5, 0x55),
                branch(BN, R1, 2),
                wyde(SETL, R10, 0xaa),     # skipped
                wyde(SETL, R10, 0x55),
                branch(BZ, R0, 2),
                wyde(SETL, R11, 0xaa),     # skipped
                wyde(SETL, R11, 0x55),
                branch(BP, R3, 2),
                wyde(SETL, R12, 0xaa),     # skipped
                wyde(SETL, R12, 0x55),
                branch(BOD, R3, 2),
                wyde(SETL, R13, 0xaa),     # skipped
                wyde(SETL, R13, 0x55),
                branch(BNN, R0, 2),
                wyde(SETL, R14, 0xaa),     # skipped
                wyde(SETL, R14, 0x55),
                branch(BNZ, R3, 2),
                wyde(SETL, R15, 0xaa),     # skipped
                wyde(SETL, R15, 0x55),
                branch(BNP, R1, 2),
                wyde(SETL, R16, 0xaa),     # skipped
                wyde(SETL, R16, 0x55),
                branch(BEV, R4, 2),
                wyde(SETL, R17, 0xaa),     # skipped
                wyde(SETL, R17, 0x55),
                halt(),
            ]
        ),
        pc=0x70,
        regs={reg: 0x55 for reg in range(10, 18)},
    ),
    MMIXTest(
        "ordinary-branches-false",
        b"".join(
            [
                insn(SUBI, R1, R0, 1),
                wyde(SETL, R3, 5),
                wyde(SETL, R4, 4),
                wyde(SETL, R5, 0x55),
                branch(BN, R3, 2),
                wyde(SETL, R10, 0x55),
                branch(BZ, R3, 2),
                wyde(SETL, R11, 0x55),
                branch(BP, R1, 2),
                wyde(SETL, R12, 0x55),
                branch(BOD, R4, 2),
                wyde(SETL, R13, 0x55),
                branch(BNN, R1, 2),
                wyde(SETL, R14, 0x55),
                branch(BNZ, R0, 2),
                wyde(SETL, R15, 0x55),
                branch(BNP, R3, 2),
                wyde(SETL, R16, 0x55),
                branch(BEV, R3, 2),
                wyde(SETL, R17, 0x55),
                halt(),
            ]
        ),
        pc=0x50,
        regs={reg: 0x55 for reg in range(10, 18)},
    ),
    MMIXTest(
        "probable-branches-true",
        b"".join(
            [
                insn(SUBI, R1, R0, 1),
                wyde(SETL, R3, 5),
                wyde(SETL, R4, 4),
                wyde(SETL, R5, 0x55),
                branch(PBN, R1, 2),
                wyde(SETL, R10, 0xaa),     # skipped
                wyde(SETL, R10, 0x55),
                branch(PBZ, R0, 2),
                wyde(SETL, R11, 0xaa),     # skipped
                wyde(SETL, R11, 0x55),
                branch(PBP, R3, 2),
                wyde(SETL, R12, 0xaa),     # skipped
                wyde(SETL, R12, 0x55),
                branch(PBOD, R3, 2),
                wyde(SETL, R13, 0xaa),     # skipped
                wyde(SETL, R13, 0x55),
                branch(PBNN, R0, 2),
                wyde(SETL, R14, 0xaa),     # skipped
                wyde(SETL, R14, 0x55),
                branch(PBNZ, R3, 2),
                wyde(SETL, R15, 0xaa),     # skipped
                wyde(SETL, R15, 0x55),
                branch(PBNP, R1, 2),
                wyde(SETL, R16, 0xaa),     # skipped
                wyde(SETL, R16, 0x55),
                branch(PBEV, R4, 2),
                wyde(SETL, R17, 0xaa),     # skipped
                wyde(SETL, R17, 0x55),
                halt(),
            ]
        ),
        pc=0x70,
        regs={reg: 0x55 for reg in range(10, 18)},
    ),
    MMIXTest(
        "probable-branches-false-backward",
        b"".join(
            [
                wyde(SETL, R1, 1),
                branch(PBNZ, R0, 2),
                wyde(SETL, R2, 7),
                branch(PBZ, R0, 3),
                wyde(SETL, R3, 9),         # skipped
                halt(),
                branch(PBNZB, R1, 0xffff),
            ]
        ),
        pc=0x14,
        regs={R2: 7, R3: 0},
    ),
    MMIXTest(
        "address-geta",
        b"".join(
            [
                branch(GETA, R1, 2),
                branch(GETAB, R2, 0xffff),
                halt(),
            ]
        ),
        pc=0x08,
        regs={R1: 0x08, R2: 0},
    ),
    MMIXTest(
        "jump-forward-backward",
        b"".join(
            [
                jump(JMP, 3),
                wyde(SETL, R1, 9),         # skipped
                halt(),
                wyde(SETL, R1, 5),
                jump(JMPB, 0xfffffe),
            ]
        ),
        pc=0x08,
        regs={R1: 5},
    ),
    MMIXTest(
        "go-register-immediate",
        b"".join(
            [
                wyde(SETL, R1, 17),
                insn(GO, R2, R1, R0),
                wyde(SETL, R3, 9),         # skipped
                halt(),                   # skipped
                wyde(SETL, R3, 5),
                insn(GOI, R4, R1, 13),
                wyde(SETL, R5, 9),         # skipped
                halt(),
            ]
        ),
        pc=0x1c,
        regs={R2: 0x08, R3: 5, R4: 0x18, R5: 0},
    ),
    MMIXTest(
        "pushj-pop-single-result",
        b"".join(
            [
                branch(PUSHJ, R0, 4),  # subroutine target
                halt(),
                insn(SWYM, 0, 0, 0),
                insn(SWYM, 0, 0, 0),
                wyde(SETL, R0, 42),  # sub
                insn(POP, 1, 0, 0),
            ]
        ),
        pc=0x04,
        regs={R0: 42},
    ),
    MMIXTest(
        "pushjb-pop-single-result",
        b"".join(
            [
                jump(JMP, 3),
                wyde(SETL, R0, 9),  # sub
                insn(POP, 1, 0, 0),
                branch(PUSHJB, R0, 0xfffe),  # caller
                halt(),
            ]
        ),
        pc=0x10,
        regs={R0: 9},
    ),
    MMIXTest(
        "pushgo-pop-single-result",
        b"".join(
            [
                wyde(SETL, R1, 0x10),  # subroutine target
                insn(PUSHGO, R0, R1, R0),
                halt(),
                insn(SWYM, 0, 0, 0),
                wyde(SETL, R0, 33),  # sub
                insn(POP, 1, 0, 0),
            ]
        ),
        pc=0x08,
        regs={R0: 33},
    ),
    MMIXTest(
        "pushgoi-pop-single-result",
        b"".join(
            [
                wyde(SETL, R1, 0x0c),  # subroutine target
                insn(PUSHGOI, R0, R1, 4),
                halt(),
                insn(SWYM, 0, 0, 0),
                wyde(SETL, R0, 44),  # sub
                insn(POP, 1, 0, 0),
            ]
        ),
        pc=0x08,
        regs={R0: 44},
    ),
    MMIXTest(
        "pop-multiple-results",
        b"".join(
            [
                branch(PUSHJ, R0, 4),  # subroutine target
                halt(),
                insn(SWYM, 0, 0, 0),
                insn(SWYM, 0, 0, 0),
                wyde(SETL, R0, 0xaa),  # sub
                wyde(SETL, R1, 0xbb),
                insn(POP, 2, 0, 0),
            ]
        ),
        pc=0x04,
        regs={R0: 0xbb, R1: 0xaa},
    ),
    MMIXTest(
        "nested-pushj-pop",
        b"".join(
            [
                branch(PUSHJ, R0, 4),  # subroutine target
                halt(),
                insn(SWYM, 0, 0, 0),
                insn(SWYM, 0, 0, 0),
                insn(GET, R40, 0, SR_J),  # sub1
                branch(PUSHJ, R0, 4),  # subroutine target
                insn(ADDI, R0, R0, 1),
                insn(PUT, SR_J, 0, R40),
                insn(POP, 1, 0, 0),
                wyde(SETL, R0, 7),  # sub2
                insn(POP, 1, 0, 0),
            ]
        ),
        pc=0x04,
        regs={R0: 8},
    ),
    MMIXTest(
        "register-stack-spill-fill",
        REGISTER_STACK_SPILL_FILL[0],
        pc=REGISTER_STACK_SPILL_FILL[1],
        regs={
            R50: INITIAL_STACK,
            R51: INITIAL_STACK,
            R60: REGISTER_STACK_SPILL_FILL[2],
        },
    ),
    MMIXTest(
        "save-state-after-save",
        SAVE_STATE_AFTER_SAVE[0],
        pc=SAVE_STATE_AFTER_SAVE[1],
        regs={
            R32: INITIAL_STACK + 0x778,
            R33: 0,
            R34: INITIAL_STACK + 0x780,
            R35: INITIAL_STACK + 0x780,
            R36: INITIAL_STACK + 0x778,
        },
    ),
    MMIXTest(
        "save-unsave-roundtrip",
        SAVE_UNSAVE_ROUNDTRIP[0],
        pc=SAVE_UNSAVE_ROUNDTRIP[1],
        regs={
            R50: 0x11,
            R51: 0x22,
            R52: 0x33,
            R53: 0x1234,
            R54: 0x5a,
            R55: 0x6b,
            R56: 0x3ffff,
            R57: 3,
            R58: INITIAL_STACK,
            R59: INITIAL_STACK,
            R60: 0x1111222233334444,
            R61: 0x5555666677778888,
            R62: 0,
            R63: 0,
        },
    ),
    MMIXTest(
        "register-stack-save-unsave-spill-fill",
        REGISTER_STACK_SAVE_UNSAVE[0],
        pc=REGISTER_STACK_SAVE_UNSAVE[1],
        regs={
            R50: INITIAL_STACK,
            R51: INITIAL_STACK,
            R60: REGISTER_STACK_SAVE_UNSAVE[2],
        },
    ),
    MMIXTest(
        "load-store",
        b"".join(
            [
                insn(ADDI, R1, R0, 0x40),
                insn(ADDI, R2, R0, 0x5a),
                insn(STO, R2, R1, R0),
                insn(LDO, R3, R1, R0),
                halt(),
            ]
        ),
        pc=0x10,
        regs={R1: 0x40, R2: 0x5a, R3: 0x5a},
    ),
    MMIXTest(
        "data-segment-runtime-load-store",
        b"".join(
            [
                *set_octa(R1, MMIX_DATA_SEGMENT_BASE + 0x100),
                *set_octa(R2, 0x1122334455667788),
                insn(STOU, R2, R1, R0),
                insn(LDOU, R3, R1, R0),
                halt(),
            ]
        ),
        pc=0x28,
        regs={R2: 0x1122334455667788, R3: 0x1122334455667788},
    ),
    MMIXTest(
        "pool-segment-runtime-load-store",
        b"".join(
            [
                *set_octa(R1, MMIX_POOL_SEGMENT_BASE + 0x100),
                *set_octa(R2, 0xaabbccddeeff0011),
                insn(STOU, R2, R1, R0),
                insn(LDOU, R3, R1, R0),
                halt(),
            ]
        ),
        pc=0x28,
        regs={R2: 0xaabbccddeeff0011, R3: 0xaabbccddeeff0011},
    ),
    MMIXTest(
        "stack-segment-runtime-load-store",
        b"".join(
            [
                *set_octa(R1, MMIX_STACK_SEGMENT_BASE + 0x100),
                *set_octa(R2, 0x1020304050607080),
                insn(STOU, R2, R1, R0),
                insn(LDOU, R3, R1, R0),
                halt(),
            ]
        ),
        pc=0x28,
        regs={R2: 0x1020304050607080, R3: 0x1020304050607080},
    ),
    MMIXTest(
        "stack-segment-runtime-last-octa",
        b"".join(
            [
                *set_octa(R1, MMIX_STACK_SEGMENT_BASE +
                          MMIX_STACK_SEGMENT_SIZE - 8),
                *set_octa(R2, 0xfeedfacecafebeef),
                insn(STOU, R2, R1, R0),
                insn(LDOU, R3, R1, R0),
                halt(),
            ]
        ),
        pc=0x28,
        regs={R2: 0xfeedfacecafebeef, R3: 0xfeedfacecafebeef},
    ),
    MMIXTest(
        "unsupported-high-segment-runtime-trap",
        program_with_handler(
            [
                wyde(SETL, R1, 0x80),  # handler address
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, RQ_PROGRAM_K),
                insn(PUT, SR_K, 0, R2),
                *set_octa(R3, MMIX_UNSUPPORTED_HIGH_SEGMENT_ADDRESS),
                insn(LDOU, R4, R3, R0),
                wyde(SETL, R5, 0x00ff),    # skipped after dynamic trap
            ],
            0x80,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                insn(GET, R43, 0, SR_K),
                halt(),
            ],
        ),
        pc=0x90,
        regs={
            R4: 0,
            R5: 0,
            R40: RQ_PROGRAM_R,
            R41: RQ_PROGRAM_R | int.from_bytes(insn(LDOU, R4, R3, R0),
                                                "big"),
            R42: 0x30,
            R43: 0,
        },
    ),
    MMIXTest(
        "virtual-translation-page0-rwx",
        b"".join(
            [
                *set_octa(R1, VM_PAGE_TABLE),
                wyde(SETL, R2, 7),
                insn(STOU, R2, R1, R0),
                *set_octa(R3, VM_RV_PAGE0),
                insn(PUT, SR_V, 0, R3),
                wyde(SETL, R4, 0x0300),
                *set_octa(R5, 0x1122334455667788),
                insn(STOU, R5, R4, R0),
                insn(LDOU, R6, R4, R0),
                halt(),
            ]
        ),
        pc=0x48,
        regs={R6: 0x1122334455667788},
    ),
    MMIXTest(
        "virtual-translation-store-protection",
        program_with_handler(
            [
                wyde(SETL, R1, 0x80),  # handler address
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R10, VM_PAGE_TABLE),
                wyde(SETL, R11, 5),
                insn(STOU, R11, R10, R0),
                *set_octa(R12, VM_RV_PAGE0),
                insn(PUT, SR_V, 0, R12),
                wyde(SETL, R13, 0x00aa),
                wyde(SETL, R14, 0x0300),
                insn(STOU, R13, R14, R0),
                wyde(SETL, R15, 0x00ff),   # skipped
            ],
            0x80,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                insn(GET, R43, 0, SR_K),
                halt(),
            ],
        ),
        pc=0x90,
        regs={
            R40: RQ_PROGRAM_W,
            R41: RQ_PROGRAM_W | int.from_bytes(insn(STOU, R13, R14, R0),
                                                "big"),
            R42: 0x40,
            R43: 0,
        },
    ),
    MMIXTest(
        "virtual-translation-nonidentity-load",
        b"".join(
            [
                *set_octa(R1, VM_PAGE_TABLE_ROOT2),
                wyde(SETL, R2, 7),
                insn(STOU, R2, R1, R0),
                *set_octa(R3, 0x0000000000006007),
                insn(STOUI, R3, R1, 8),
                *set_octa(R4, 0x0000000000006000),
                *set_octa(R5, 0x0102030405060708),
                insn(STOU, R5, R4, R0),
                *set_octa(R6, VM_RV_ROOT2),
                insn(PUT, SR_V, 0, R6),
                *set_octa(R7, 0x0000000000002000),
                insn(LDOU, R8, R7, R0),
                halt(),
            ]
        ),
        pc=0x78,
        regs={R8: 0x0102030405060708},
    ),
    MMIXTest(
        "virtual-translation-read-protection",
        program_with_handler(
            [
                *set_octa(R1, NEGATIVE_HANDLER),
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, VM_PAGE_TABLE),
                wyde(SETL, R3, 3),
                insn(STOU, R3, R2, R0),
                *set_octa(R4, VM_RV_PAGE0),
                insn(PUT, SR_V, 0, R4),
                wyde(SETL, R5, 0x0300),
                insn(LDOU, R6, R5, R0),
                wyde(SETL, R7, 0x00ff),    # skipped
            ],
            0x80,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                halt(),
            ],
        ),
        pc=0x800000000000008c,
        regs={
            R6: 0,
            R7: 0,
            R40: RQ_PROGRAM_R,
            R41: RQ_PROGRAM_R | int.from_bytes(insn(LDOU, R6, R5, R0),
                                                "big"),
            R42: 0x48,
        },
    ),
    MMIXTest(
        "virtual-translation-execute-protection",
        program_with_handler(
            [
                *set_octa(R1, NEGATIVE_HANDLER),
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, VM_PAGE_TABLE),
                wyde(SETL, R3, 6),
                insn(STOU, R3, R2, R0),
                *set_octa(R4, VM_RV_PAGE0),
                insn(PUT, SR_V, 0, R4),
                wyde(SETL, R5, 0x00ff),    # skipped
            ],
            0x80,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                halt(),
            ],
        ),
        pc=0x800000000000008c,
        regs={
            R5: 0,
            R40: RQ_PROGRAM_X,
            R41: RQ_PROGRAM_X |
                 int.from_bytes(insn(SWYM, 0, 0, 0), "big"),
            R42: 0x44,
        },
    ),
    MMIXTest(
        "virtual-translation-asn-mismatch",
        program_with_handler(
            [
                *set_octa(R1, NEGATIVE_HANDLER),
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, VM_PAGE_TABLE_ROOT2),
                wyde(SETL, R3, 7),
                insn(STOU, R3, R2, R0),
                *set_octa(R4, 0x000000000000600f),
                insn(STOUI, R4, R2, 8),
                *set_octa(R5, VM_RV_ROOT2),
                insn(PUT, SR_V, 0, R5),
                *set_octa(R6, 0x0000000000002000),
                insn(LDOU, R7, R6, R0),
                wyde(SETL, R8, 0x00ff),    # skipped
            ],
            0x80,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                halt(),
            ],
        ),
        pc=0x800000000000008c,
        regs={
            R7: 0,
            R8: 0,
            R40: RQ_PROGRAM_R,
            R41: RQ_PROGRAM_R | int.from_bytes(insn(LDOU, R7, R6, R0),
                                                "big"),
            R42: 0x68,
        },
    ),
    MMIXTest(
        "virtual-translation-invalid-rv",
        program_with_handler(
            [
                *set_octa(R1, NEGATIVE_HANDLER),
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, 0x11110c0000002000),
                insn(PUT, SR_V, 0, R2),
                wyde(SETL, R3, 0x00ff),    # skipped
            ],
            0x80,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                halt(),
            ],
        ),
        pc=0x800000000000008c,
        regs={
            R3: 0,
            R40: RQ_PROGRAM_X,
            R41: RQ_PROGRAM_X |
                 int.from_bytes(insn(SWYM, 0, 0, 0), "big"),
            R42: 0x2c,
        },
    ),
    MMIXTest(
        "virtual-translation-reserved-function",
        program_with_handler(
            [
                *set_octa(R1, NEGATIVE_HANDLER),
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, VM_RV_PAGE0 | 2),
                insn(PUT, SR_V, 0, R2),
                wyde(SETL, R3, 0x00ff),
            ],
            0x80,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                halt(),
            ],
        ),
        pc=0x800000000000008c,
        regs={
            R3: 0,
            R40: RQ_PROGRAM_X,
            R41: RQ_PROGRAM_X |
                 int.from_bytes(insn(SWYM, 0, 0, 0), "big"),
            R42: 0x2c,
        },
    ),
    MMIXTest(
        "negative-address-load-user-trap",
        program_with_handler(
            [
                wyde(SETL, R1, 0x80),  # handler address
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, RQ_PROGRAM_K),
                insn(PUT, SR_K, 0, R2),
                *set_octa(R3, 0x8000000000000300),
                insn(LDOU, R4, R3, R0),
                wyde(SETL, R5, 0x00ff),    # skipped
            ],
            0x80,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                insn(GET, R43, 0, SR_K),
                halt(),
            ],
        ),
        pc=0x90,
        regs={
            R4: 0,
            R5: 0,
            R40: RQ_PROGRAM_N,
            R41: RQ_PROGRAM_N | int.from_bytes(insn(LDOU, R4, R3, R0),
                                                "big"),
            R42: 0x30,
            R43: 0,
        },
    ),
    MMIXTest(
        "negative-address-store-user-trap",
        program_with_handler(
            [
                wyde(SETL, R1, 0x80),  # handler address
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, RQ_PROGRAM_K),
                insn(PUT, SR_K, 0, R2),
                *set_octa(R3, 0x8000000000000300),
                wyde(SETL, R4, 0x00aa),
                insn(STOU, R4, R3, R0),
                wyde(SETL, R5, 0x00ff),    # skipped
            ],
            0x80,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                insn(GET, R43, 0, SR_K),
                halt(),
            ],
        ),
        pc=0x90,
        regs={
            R5: 0,
            R40: RQ_PROGRAM_N,
            R41: RQ_PROGRAM_N | int.from_bytes(insn(STOU, R4, R3, R0),
                                                "big"),
            R42: 0x34,
            R43: 0,
        },
    ),
    MMIXTest(
        "negative-address-fetch-direct",
        program_with_handler(
            [
                *set_octa(R2, RQ_PROGRAM_K),
                insn(PUT, SR_K, 0, R2),
                *set_octa(R3, 0x8000000000000300),
                insn(GO, R4, R3, R0),
                wyde(SETL, R5, 0x00ff),    # skipped
            ],
            0x300,
            [
                wyde(SETL, R5, 0x0055),
                halt(),
            ],
        ),
        pc=0x8000000000000304,
        regs={R4: 0x28, R5: 0x55},
    ),
    MMIXTest(
        "memory-octa-variants",
        b"".join(
            [
                wyde(SETL, R1, 0x0200),
                wyde(SETL, R2, 8),
                wyde(SETH, R3, 0x1122),
                wyde(INCMH, R3, 0x3344),
                wyde(INCML, R3, 0x5566),
                wyde(INCL, R3, 0x7788),
                insn(STO, R3, R1, R2),
                insn(LDO, R4, R1, R2),
                insn(STOI, R3, R1, 16),
                insn(LDOI, R5, R1, 16),
                insn(STOU, R3, R1, R2),
                insn(LDOU, R6, R1, R2),
                insn(STOUI, R3, R1, 24),
                insn(LDOUI, R7, R1, 24),
                halt(),
            ]
        ),
        pc=0x38,
        regs={
            R3: 0x1122334455667788,
            R4: 0x1122334455667788,
            R5: 0x1122334455667788,
            R6: 0x1122334455667788,
            R7: 0x1122334455667788,
        },
    ),
    MMIXTest(
        "memory-store-constant-octa",
        b"".join(
            [
                wyde(SETL, R1, 0x02a0),
                wyde(SETL, R2, 8),
                insn(STCO, 0x5a, R1, R2),
                insn(LDOU, R3, R1, R2),
                insn(STCOI, 0xa5, R1, 16),
                insn(LDOUI, R4, R1, 16),
                halt(),
            ]
        ),
        pc=0x18,
        regs={R3: 0x5a, R4: 0xa5},
    ),
    MMIXTest(
        "memory-load-extension",
        b"".join(
            [
                wyde(SETL, R1, 0x0220),
                wyde(SETL, R2, 1),
                wyde(SETL, R3, 2),
                wyde(SETL, R4, 4),
                wyde(SETL, R10, 0x80),
                insn(STBUI, R10, R1, 1),
                insn(LDB, R11, R1, R2),
                insn(LDBI, R12, R1, 1),
                insn(LDBU, R13, R1, R2),
                insn(LDBUI, R14, R1, 1),
                wyde(SETL, R15, 0x8001),
                insn(STW, R15, R1, R3),
                insn(LDW, R16, R1, R3),
                insn(LDWI, R17, R1, 2),
                insn(LDWU, R18, R1, R3),
                insn(LDWUI, R19, R1, 2),
                wyde(SETML, R20, 0x8000),
                wyde(INCL, R20, 1),
                insn(STTI, R20, R1, 4),
                insn(LDT, R21, R1, R4),
                insn(LDTI, R22, R1, 4),
                insn(LDTU, R23, R1, R4),
                insn(LDTUI, R24, R1, 4),
                halt(),
            ]
        ),
        pc=0x5c,
        regs={
            R11: 0xffffffffffffff80,
            R12: 0xffffffffffffff80,
            R13: 0x80,
            R14: 0x80,
            R16: 0xffffffffffff8001,
            R17: 0xffffffffffff8001,
            R18: 0x8001,
            R19: 0x8001,
            R20: 0x80000001,
            R21: 0xffffffff80000001,
            R22: 0xffffffff80000001,
            R23: 0x80000001,
            R24: 0x80000001,
        },
    ),
    MMIXTest(
        "memory-store-widths",
        b"".join(
            [
                wyde(SETL, R1, 0x0240),
                wyde(SETL, R2, 0x2a),
                insn(STB, R2, R1, R0),
                insn(LDBU, R20, R1, R0),
                wyde(SETL, R3, 0x3b),
                insn(STBI, R3, R1, 1),
                insn(LDBUI, R21, R1, 1),
                wyde(SETL, R4, 0xcc),
                insn(STBU, R4, R1, R2),
                insn(LDBU, R22, R1, R2),
                wyde(SETL, R5, 0xdd),
                insn(STBUI, R5, R1, 3),
                insn(LDBUI, R23, R1, 3),
                wyde(SETL, R6, 0x1234),
                insn(STW, R6, R1, R0),
                insn(LDWU, R24, R1, R0),
                wyde(SETL, R7, 0x5678),
                insn(STWI, R7, R1, 6),
                insn(LDWUI, R25, R1, 6),
                wyde(SETL, R8, 0x9abc),
                insn(STWU, R8, R1, R0),
                insn(LDWU, R26, R1, R0),
                wyde(SETL, R9, 0xdef0),
                insn(STWUI, R9, R1, 10),
                insn(LDWUI, R27, R1, 10),
                wyde(SETML, R10, 0x1122),
                wyde(INCL, R10, 0x3344),
                insn(STT, R10, R1, R0),
                insn(LDTU, R28, R1, R0),
                wyde(SETML, R11, 0x5566),
                wyde(INCL, R11, 0x7788),
                insn(STTI, R11, R1, 16),
                insn(LDTUI, R29, R1, 16),
                wyde(SETML, R12, 0x99aa),
                wyde(INCL, R12, 0xbbcc),
                insn(STTU, R12, R1, R0),
                insn(LDTU, R30, R1, R0),
                wyde(SETML, R13, 0xddee),
                wyde(INCL, R13, 0xff00),
                insn(STTUI, R13, R1, 24),
                insn(LDTUI, R31, R1, 24),
                halt(),
            ]
        ),
        pc=0xa4,
        regs={
            R20: 0x2a,
            R21: 0x3b,
            R22: 0xcc,
            R23: 0xdd,
            R24: 0x1234,
            R25: 0x5678,
            R26: 0x9abc,
            R27: 0xdef0,
            R28: 0x11223344,
            R29: 0x55667788,
            R30: 0x99aabbcc,
            R31: 0xddeeff00,
        },
    ),
    MMIXTest(
        "memory-high-tetra",
        b"".join(
            [
                wyde(SETL, R1, 0x0280),
                wyde(SETH, R2, 0x1234),
                wyde(INCMH, R2, 0x5678),
                wyde(INCML, R2, 0x9abc),
                wyde(INCL, R2, 0xdef0),
                insn(STHT, R2, R1, R0),
                insn(LDHT, R3, R1, R0),
                insn(STHTI, R2, R1, 4),
                insn(LDHTI, R4, R1, 4),
                halt(),
            ]
        ),
        pc=0x24,
        regs={
            R2: 0x123456789abcdef0,
            R3: 0x1234567800000000,
            R4: 0x1234567800000000,
        },
    ),
    MMIXTest(
        "memory-uncached-octa",
        b"".join(
            [
                wyde(SETL, R1, 0x0300),
                *set_octa(R2, 0x1122334455667788),
                insn(STUNC, R2, R1, R0),
                insn(LDOU, R3, R1, R0),
                *set_octa(R4, 0x99aabbccddeeff00),
                insn(STOU, R4, R1, R0),
                insn(LDUNC, R5, R1, R0),
                insn(STUNCI, R2, R1, 8),
                insn(LDUNCI, R6, R1, 8),
                halt(),
            ]
        ),
        pc=0x3c,
        regs={
            R1: 0x0300,
            R2: 0x1122334455667788,
            R3: 0x1122334455667788,
            R4: 0x99aabbccddeeff00,
            R5: 0x99aabbccddeeff00,
            R6: 0x1122334455667788,
        },
    ),
    MMIXTest(
        "memory-prefetch-sync-hints",
        b"".join(
            [
                wyde(SETL, R1, 0x0380),
                *set_octa(R2, 0x0123456789abcdef),
                insn(STOU, R2, R1, R0),
                *set_octa(R3, 0xfffffffffffffff8),
                insn(PRELD, 15, R3, R3),
                insn(PRELDI, 16, R3, 0xff),
                insn(PREST, 17, R3, R3),
                insn(PRESTI, 18, R3, 0xff),
                insn(PREGO, 19, R3, R3),
                insn(PREGOI, 20, R3, 0xff),
                insn(SYNCD, 21, R3, R3),
                insn(SYNCDI, 22, R3, 0xff),
                insn(SYNCID, 23, R3, R3),
                insn(SYNCIDI, 24, R3, 0xff),
                jump(SYNC, 0),
                jump(SYNC, 1),
                jump(SYNC, 2),
                jump(SYNC, 3),
                jump(SYNC, 4),
                jump(SYNC, 5),
                jump(SYNC, 6),
                jump(SYNC, 7),
                insn(LDOU, R4, R1, R0),
                halt(),
            ]
        ),
        pc=0x74,
        regs={
            R1: 0x0380,
            R2: 0x0123456789abcdef,
            R3: 0xfffffffffffffff8,
            R4: 0x0123456789abcdef,
        },
    ),
    MMIXTest(
        "break-rules-sync-masked",
        b"".join(
            [
                wyde(SETL, R8, 0x1234),
                jump(SYNC, 8),
                wyde(SETL, R20, 0x55),
                insn(GET, R21, 0, SR_Q),
                insn(GET, R22, 0, SR_K),
                halt(),
            ]
        ),
        pc=0x14,
        regs={R8: 0x1234, R20: 0x55, R21: RQ_PROGRAM_B, R22: 0},
    ),
    MMIXTest(
        "break-rules-sync-enabled-resume",
        program_with_handler(
            [
                wyde(SETL, R8, 0x1234),
                wyde(SETL, R255, 0x55),
                wyde(SETL, R9, 0x1122),
                insn(PUT, SR_J, 0, R9),
                wyde(SETL, R10, 0x100),
                insn(PUT, SR_TT, 0, R10),
                *set_octa(R2, RQ_PROGRAM_B),
                insn(PUT, SR_K, 0, R2),
                jump(SYNC, 8),
                wyde(SETL, R30, 0xabcd),
                insn(ADDU, R31, R255, R0),
                insn(GET, R32, 0, SR_K),
                insn(GET, R33, 0, SR_Q),
                wyde(SETL, R255, 0),
                halt(),
            ],
            0x100,
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
                insn(ADDU, R255, R2, R0),
                insn(RESUME, 0, 0, 1),
            ],
        ),
        pc=0x44,
        regs={
            R30: 0xabcd,
            R31: 0x55,
            R32: RQ_PROGRAM_B,
            R33: 0,
            R40: RQ_PROGRAM_B,
            R41: 0x30,
            R42: DYNAMIC_TRAP_RESUME_NEXT | RQ_PROGRAM_B |
                 (SYNC << 24) | 8,
            R43: 0,
            R44: 0x1234,
            R45: 0x55,
            R46: 0,
            R47: 0x1122,
        },
    ),
    MMIXTest(
        "break-rules-static-forms-masked",
        RULE_BREAKS_MASKED[0],
        pc=RULE_BREAKS_MASKED[1],
        regs={
            R20: 0x77,
            R30: 13,
            R31: 0x55,
            R32: 0,
            R33: 0,
            R35: RQ_PROGRAM_B,
            R36: 0,
            R37: 0x66,
            R38: 0x55,
        },
    ),
    rule_break_enabled_test(
        "break-rules-get-fields",
        insn(GET, R20, 3, SR_M),
        setup=(wyde(SETL, R20, 0x77),),
        regs={R20: 0x77},
    ),
    rule_break_enabled_test(
        "break-rules-get-register",
        insn(GET, R20, 0, 0xff),
        setup=(wyde(SETL, R20, 0x77),),
        regs={R20: 0x77},
    ),
    rule_break_enabled_test(
        "break-rules-put-fields",
        insn(PUT, SR_M, 3, R4),
        setup=(insn(PUTI, SR_M, 0, 0x55),),
        handler_checks=(insn(GET, R230, 0, SR_M),),
        regs={R230: 0x55},
    ),
    rule_break_enabled_test(
        "break-rules-put-register",
        insn(PUT, 0xff, 0, R4),
    ),
    rule_break_enabled_test(
        "break-rules-puti-fields",
        insn(PUTI, SR_M, 3, 0xaa),
        setup=(insn(PUTI, SR_M, 0, 0x55),),
        z=0xaa,
        handler_checks=(insn(GET, R230, 0, SR_M),),
        regs={R230: 0x55},
    ),
    rule_break_enabled_test(
        "break-rules-puti-register",
        insn(PUTI, 0xff, 0, 0xaa),
        z=0xaa,
    ),
    rule_break_enabled_test(
        "break-rules-save-fields",
        insn(SAVE, R32, 3, 0),
        setup=(
            insn(GET, R220, 0, SR_O),
            insn(GET, R221, 0, SR_S),
        ),
        handler_checks=(
            insn(GET, R223, 0, SR_O),
            insn(GET, R224, 0, SR_S),
            insn(CMP, R230, R220, R223),
            insn(CMP, R231, R221, R224),
        ),
        regs={R230: 0, R231: 0},
    ),
    rule_break_enabled_test(
        "break-rules-unsave-fields",
        insn(UNSAVE, 3, 0, R32),
        setup=(
            insn(GET, R220, 0, SR_O),
            insn(GET, R221, 0, SR_S),
        ),
        handler_checks=(
            insn(GET, R223, 0, SR_O),
            insn(GET, R224, 0, SR_S),
            insn(CMP, R230, R220, R223),
            insn(CMP, R231, R221, R224),
        ),
        regs={R230: 0, R231: 0},
    ),
    rule_break_enabled_test(
        "break-rules-resume-fields",
        insn(RESUME, 3, 0, 0),
    ),
    rule_break_enabled_test(
        "break-rules-resume-z",
        insn(RESUME, 0, 0, 2),
        z=RQ_PROGRAM_B,
    ),
    rule_break_enabled_test(
        "break-rules-resume-ropcode",
        insn(RESUME, 0, 0, 1),
        setup=(
            *set_octa(R40, 0x0400000000000000),
            insn(PUT, SR_XX, 0, R40),
            wyde(SETL, R41, 0x66),
            insn(PUT, SR_BB, 0, R41),
            wyde(SETL, R255, 0x55),
        ),
        old_r255=0x55,
    ),
    rule_break_enabled_test(
        "break-rules-resume0-ropcode3",
        insn(RESUME, 0, 0, 0),
        setup=(
            *set_octa(R40, 0x0300000000000000),
            insn(PUT, SR_X, 0, R40),
        ),
    ),
    rule_break_enabled_test(
        "break-rules-resume1-ropcode3-location",
        insn(RESUME, R0, R0, 1),
        setup=(
            wyde(SETL, R40, 0x20),
            insn(PUT, SR_WW, R0, R40),
            *set_octa(R41, 0x030000008e010200),
            insn(PUT, SR_XX, R0, R41),
            wyde(SETL, R42, FORCED_TRANSLATION_VIRTUAL),
            insn(PUT, SR_YY, R0, R42),
            wyde(SETL, R43, FORCED_TRANSLATION_PHYSICAL | 7),
            insn(PUT, SR_ZZ, R0, R43),
        ),
    ),
    MMIXTest(
        "break-rules-runtime-forms-masked",
        RUNTIME_RULE_BREAKS_MASKED[0],
        pc=RUNTIME_RULE_BREAKS_MASKED[1],
        regs={
            R30: 10,
            R200: 0x1111222233334444,
            R201: 0x5555666677778888,
            R202: 0x9999aaaabbbbcccc,
            R208: 0,
            R218: 0,
            R219: 0,
            R220: 0,
            R221: 0,
            R222: RQ_PROGRAM_B,
            R223: 0,
        },
    ),
    rule_break_enabled_test(
        "break-rules-readonly-special-register",
        insn(PUTI, SR_N, 0, 1),
        setup=(insn(GET, R220, 0, SR_N),),
        z=1,
        handler_checks=(
            insn(GET, R221, 0, SR_N),
            insn(CMP, R230, R220, R221),
        ),
        regs={R230: 0},
    ),
    rule_break_enabled_test(
        "break-rules-rg-value",
        insn(PUTI, SR_G, 0, 31),
        setup=(insn(GET, R220, 0, SR_G),),
        z=31,
        handler_checks=(
            insn(GET, R221, 0, SR_G),
            insn(CMP, R230, R220, R221),
        ),
        regs={R230: 0},
    ),
    rule_break_enabled_test(
        "break-rules-rl-value",
        insn(PUTI, SR_L, 0, 0xff),
        z=0xff,
        handler_checks=(insn(GET, R230, 0, SR_L),),
        regs={R230: 11},
    ),
    rule_break_enabled_test(
        "break-rules-save-destination",
        insn(SAVE, R0, 0, 0),
        setup=(
            insn(GET, R220, 0, SR_O),
            insn(GET, R221, 0, SR_S),
        ),
        handler_checks=(
            insn(GET, R222, 0, SR_O),
            insn(GET, R223, 0, SR_S),
            insn(CMP, R230, R220, R222),
            insn(CMP, R231, R221, R223),
        ),
        regs={R230: 0, R231: 0},
    ),
    rule_break_enabled_test(
        "break-rules-fp-unary-rounding",
        insn(FINT, R200, 5, R5),
        setup=(
            *set_octa(R200, 0x1111222233334444),
            *set_octa(R5, f64(1.5)),
        ),
        y=5,
        z=f64(1.5),
        regs={R200: 0x1111222233334444},
    ),
    rule_break_enabled_test(
        "break-rules-fp-fix-rounding",
        insn(FIX, R201, 6, R5),
        setup=(
            *set_octa(R201, 0x5555666677778888),
            *set_octa(R5, f64(1.5)),
        ),
        y=6,
        z=f64(1.5),
        regs={R201: 0x5555666677778888},
    ),
    rule_break_enabled_test(
        "break-rules-fp-float-rounding",
        insn(FLOT, R202, 7, R6),
        setup=(
            *set_octa(R202, 0x9999aaaabbbbcccc),
            wyde(SETL, R6, 7),
        ),
        y=7,
        z=7,
        regs={R202: 0x9999aaaabbbbcccc},
    ),
    MMIXTest(
        "memory-compare-swap",
        b"".join(
            [
                wyde(SETL, R1, 0x0400),
                *set_octa(R2, 0x1111222233334444),
                *set_octa(R3, 0xaaaabbbbccccdddd),
                insn(STOU, R2, R1, R0),
                insn(PUT, SR_P, 0, R2),
                insn(CSWAP, R3, R1, R0),
                insn(LDOU, R4, R1, R0),
                insn(GET, R5, 0, SR_P),
                *set_octa(R6, 0x5555666677778888),
                *set_octa(R7, 0x9999aaaabbbbcccc),
                insn(PUT, SR_P, 0, R7),
                insn(CSWAP, R6, R1, R0),
                insn(LDOU, R8, R1, R0),
                insn(GET, R9, 0, SR_P),
                *set_octa(R10, 0x0102030405060708),
                insn(STOUI, R10, R1, 16),
                *set_octa(R11, 0x1020304050607080),
                insn(PUT, SR_P, 0, R10),
                insn(CSWAPI, R11, R1, 16),
                insn(LDOUI, R12, R1, 16),
                insn(GET, R13, 0, SR_P),
                *set_octa(R14, 0x0f0e0d0c0b0a0908),
                insn(PUT, SR_P, 0, R14),
                *set_octa(R15, 0x8877665544332211),
                insn(CSWAPI, R15, R1, 16),
                insn(LDOUI, R16, R1, 16),
                insn(GET, R17, 0, SR_P),
                halt(),
            ]
        ),
        pc=0xcc,
        regs={
            R1: 0x0400,
            R3: 1,
            R4: 0xaaaabbbbccccdddd,
            R5: 0x1111222233334444,
            R6: 0,
            R8: 0xaaaabbbbccccdddd,
            R9: 0xaaaabbbbccccdddd,
            R11: 1,
            R12: 0x1020304050607080,
            R13: 0x0102030405060708,
            R15: 0,
            R16: 0x1020304050607080,
            R17: 0x1020304050607080,
        },
    ),
    MMIXTest(
        "special-register-get-reset",
        b"".join(
            [
                insn(GET, R33, 0, SR_K),
                insn(GET, R34, 0, SR_T),
                insn(GET, R35, 0, SR_TT),
                insn(GET, R36, 0, SR_V),
                insn(GET, R37, 0, SR_G),
                insn(GET, R38, 0, SR_L),
                insn(GET, R39, 0, SR_A),
                halt(),
            ]
        ),
        pc=0x1c,
        regs={
            R33: 0,
            R34: 0x8000000500000000,
            R35: 0x8000000600000000,
            R36: 0x369c200400000000,
            R37: 32,
            R38: 0,
            R39: 0,
        },
    ),
    MMIXTest(
        "privileged-register-user-trap",
        program_with_handler(
            [
                wyde(SETL, R1, 0x40),  # handler address
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, RQ_PROGRAM_K),
                insn(PUT, SR_K, 0, R2),
                insn(PUTI, SR_C, 0, 0xaa),
                wyde(SETL, R3, 0xee),      # skipped after dynamic trap
            ],
            0x40,
            [
                insn(GET, R40, 0, SR_C),
                insn(GET, R41, 0, SR_Q),
                insn(GET, R42, 0, SR_XX),
                insn(GET, R43, 0, SR_WW),
                insn(GET, R44, 0, SR_K),
                halt(),
            ],
        ),
        pc=0x54,
        regs={
            R3: 0,
            R40: 0,
            R41: RQ_PROGRAM_K,
            R42: RQ_PROGRAM_K |
                 int.from_bytes(insn(PUTI, SR_C, 0, 0xaa), "big"),
            R43: 0x20,
            R44: 0,
        },
    ),
    MMIXTest(
        "privileged-sync-user-trap",
        program_with_handler(
            [
                wyde(SETL, R1, 0x40),  # handler address
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, RQ_PROGRAM_K),
                insn(PUT, SR_K, 0, R2),
                jump(SYNC, 4),
                wyde(SETL, R3, 0xee),      # skipped after dynamic trap
            ],
            0x40,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                insn(GET, R43, 0, SR_K),
                halt(),
            ],
        ),
        pc=0x50,
        regs={
            R3: 0,
            R40: RQ_PROGRAM_K,
            R41: RQ_PROGRAM_K | int.from_bytes(jump(SYNC, 4), "big"),
            R42: 0x20,
            R43: 0,
        },
    ),
    MMIXTest(
        "privileged-sync7-user-trap",
        program_with_handler(
            [
                wyde(SETL, R1, 0x40),  # handler address
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, RQ_PROGRAM_K),
                insn(PUT, SR_K, 0, R2),
                jump(SYNC, 7),
                wyde(SETL, R3, 0xee),      # skipped after dynamic trap
            ],
            0x40,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                insn(GET, R43, 0, SR_K),
                halt(),
            ],
        ),
        pc=0x50,
        regs={
            R3: 0,
            R40: RQ_PROGRAM_K,
            R41: RQ_PROGRAM_K | int.from_bytes(jump(SYNC, 7), "big"),
            R42: 0x20,
            R43: 0,
        },
    ),
    MMIXTest(
        "ldvts-current-cache-policy",
        b"".join(
            [
                *set_octa(R1, 0x2000000000000005),
                wyde(SETL, R2, 3),
                insn(LDVTS, R3, R1, R2),
                insn(LDVTSI, R4, R1, 7),
                halt(),
            ]
        ),
        pc=0x1c,
        regs={
            R1: 0x2000000000000005,
            R2: 3,
            R3: 0,
            R4: 0,
        },
    ),
    MMIXTest(
        "ldvts-hardware-walk-cache-status",
        b"".join(
            [
                *set_octa(R1, VM_PAGE_TABLE),
                wyde(SETL, R2, 7),
                insn(STOU, R2, R1, R0),
                *set_octa(R3, 0x0000000000006007),
                insn(STOUI, R3, R1, 8),
                *set_octa(R4, 0x0000000000006000),
                *set_octa(R5, 0x1122334455667788),
                insn(STOU, R5, R4, R0),
                *set_octa(R6, VM_RV_PAGE0),
                insn(PUT, SR_V, 0, R6),
                *set_octa(R7, 0x0000000000002000),
                insn(LDOU, R8, R7, R0),
                insn(LDOU, R9, R0, R0),
                wyde(SETL, R10, 7),
                insn(LDVTS, R11, R10, R0),
                wyde(SETL, R12, 0x2007),
                insn(LDVTS, R13, R12, R0),
                halt(),
            ]
        ),
        pc=0x8c,
        regs={
            R8: 0x1122334455667788,
            R11: 3,
            R13: 2,
        },
    ),
    MMIXTest(
        "ldvts-invalidates-stale-hardware-walk",
        b"".join(
            [
                *set_octa(R1, VM_PAGE_TABLE),
                wyde(SETL, R2, 7),
                insn(STOU, R2, R1, R0),
                *set_octa(R3, 0x0000000000006007),
                insn(STOUI, R3, R1, 8),
                *set_octa(R4, 0x0000000000006000),
                *set_octa(R5, 0x1111111111111111),
                insn(STOU, R5, R4, R0),
                *set_octa(R6, 0x0000000000008000),
                *set_octa(R7, 0x2222222222222222),
                insn(STOU, R7, R6, R0),
                *set_octa(R8, VM_RV_PAGE0),
                insn(PUT, SR_V, 0, R8),
                wyde(SETL, R9, 0x2000),
                insn(LDOU, R10, R9, R0),
                *set_octa(R11, 0x8000000000002008),
                *set_octa(R12, 0x0000000000008007),
                insn(STOU, R12, R11, R0),
                insn(LDOU, R13, R9, R0),
                insn(LDVTS, R14, R9, R0),
                insn(LDOU, R15, R9, R0),
                insn(PUT, SR_V, 0, R8),
                wyde(SETL, R16, 0x2007),
                insn(LDVTS, R17, R16, R0),
                halt(),
            ]
        ),
        pc=0xcc,
        regs={
            R10: 0x1111111111111111,
            R13: 0x1111111111111111,
            R14: 2,
            R15: 0x2222222222222222,
            R17: 0,
        },
    ),
    MMIXTest(
        "ldvts-permission-update-flushes-tlb",
        program_with_handler(
            [
                *set_octa(R1, NEGATIVE_HANDLER),
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, VM_PAGE_TABLE),
                wyde(SETL, R3, 7),
                insn(STOU, R3, R2, R0),
                *set_octa(R4, 0x0000000000006007),
                insn(STOUI, R4, R2, 8),
                *set_octa(R5, VM_RV_PAGE0),
                insn(PUT, SR_V, 0, R5),
                wyde(SETL, R6, 0x2000),
                insn(LDOU, R7, R6, R0),
                wyde(SETL, R8, 0x2004),
                insn(LDVTS, R9, R8, R0),
                wyde(SETL, R10, 0x00aa),
                insn(STOU, R10, R6, R0),
                wyde(SETL, R11, 0x00ff),
            ],
            0x80,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                halt(),
            ],
        ),
        pc=0x800000000000008c,
        regs={
            R9: 2,
            R11: 0,
            R40: RQ_PROGRAM_W,
            R41: RQ_PROGRAM_W |
                 int.from_bytes(insn(STOU, R10, R6, R0), "big"),
            R42: 0x6c,
        },
    ),
    MMIXTest(
        "software-translation-load-resume",
        forced_data_translation_program(
            [
                wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL),
                insn(LDOU, R11, R10, R0),
                wyde(SETL, R12, 0x55),
                halt(),
            ],
            [
                insn(GET, R40, R0, SR_WW),
                insn(GET, R41, R0, SR_XX),
                insn(GET, R42, R0, SR_YY),
                insn(GET, R43, R0, SR_ZZ),
                insn(GET, R44, R0, SR_BB),
                insn(GET, R45, R0, SR_K),
                *set_octa(R50, FORCED_TRANSLATION_PHYSICAL | 7),
                insn(PUT, SR_ZZ, R0, R50),
                insn(RESUME, R0, R0, 1),
            ],
            0x1122334455667788,
        ),
        pc=0x8000000000000134,
        regs={
            R11: 0x1122334455667788,
            R12: 0x55,
            R40: 0x8000000000000130,
            R41: 0x030000008e0b0a00,
            R42: FORCED_TRANSLATION_VIRTUAL,
            R43: 0,
            R44: 0,
            R45: 0,
        },
    ),
    MMIXTest(
        "software-translation-store-resume",
        forced_data_translation_program(
            [
                wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL),
                *set_octa(R11, 0x8877665544332211),
                insn(STOU, R11, R10, R0),
                insn(LDOU, R12, R10, R0),
                halt(),
            ],
            [
                insn(GET, R40, R0, SR_WW),
                insn(GET, R41, R0, SR_XX),
                insn(GET, R42, R0, SR_YY),
                *set_octa(R50, 0x8000000000004000),
                insn(LDOU, R51, R50, R0),
                *set_octa(R52, FORCED_TRANSLATION_PHYSICAL | 7),
                insn(PUT, SR_ZZ, R0, R52),
                insn(RESUME, R0, R0, 1),
            ],
            0x0102030405060708,
        ),
        pc=0x8000000000000144,
        regs={
            R12: 0x8877665544332211,
            R40: 0x8000000000000140,
            R41: 0x03000000ae0b0a00,
            R42: FORCED_TRANSLATION_VIRTUAL,
            R51: 0x0102030405060708,
        },
    ),
    MMIXTest(
        "software-translation-store-repeated-miss",
        forced_data_translation_program(
            [
                wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL),
                wyde(SETL, R11, 0x1111),
                insn(STOU, R11, R10, R0),
                wyde(SETL, R13, FORCED_TRANSLATION_VIRTUAL),
                insn(LDVTS, R14, R13, R0),
                wyde(SETL, R11, 0x2222),
                insn(STOU, R11, R10, R0),
                insn(LDOU, R12, R10, R0),
                halt(),
            ],
            [
                insn(ADDI, R60, R60, 1),
                *set_octa(R50, 0x8000000000004000),
                insn(LDOU, R61, R50, R0),
                *set_octa(R51, FORCED_TRANSLATION_PHYSICAL | 7),
                insn(PUT, SR_ZZ, R0, R51),
                insn(RESUME, R0, R0, 1),
            ],
            0x0102030405060708,
        ),
        pc=0x8000000000000148,
        regs={
            R12: 0x2222,
            R14: 2,
            R60: 2,
            R61: 0x1111,
        },
    ),
    MMIXTest(
        "software-translation-put-rv-invalidates-cache",
        forced_data_translation_program(
            [
                wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL),
                insn(LDOU, R11, R10, R0),
                *set_octa(R12, 0x8000000000006000),
                *set_octa(R13, 0x8877665544332211),
                insn(STOU, R13, R12, R0),
                *set_octa(R14, VM_RV_SOFTWARE),
                insn(PUT, SR_V, R0, R14),
                insn(LDOU, R15, R10, R0),
                halt(),
            ],
            [
                insn(ADDI, R60, R60, 1),
                insn(CMPUI, R61, R60, 1),
                branch(BNZ, R61, 7),
                *set_octa(R50, FORCED_TRANSLATION_PHYSICAL | 7),
                insn(PUT, SR_ZZ, R0, R50),
                insn(RESUME, R0, R0, 1),
                *set_octa(R50, 0x6007),
                insn(PUT, SR_ZZ, R0, R50),
                insn(RESUME, R0, R0, 1),
            ],
            0x1122334455667788,
        ),
        pc=0x800000000000016c,
        regs={
            R11: 0x1122334455667788,
            R15: 0x8877665544332211,
            R60: 2,
        },
    ),
    invalid_forced_data_translation_test(
        "software-translation-load-asn-mismatch",
        [
            wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL),
            insn(LDOU, R11, R10, R0),
        ],
        FORCED_TRANSLATION_PHYSICAL | 0xf,
        0x8000000000000130,
        0x030000008e0b0a00,
        0x1122334455667788,
    ),
    invalid_forced_data_translation_test(
        "software-translation-load-permission",
        [
            wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL),
            insn(LDOU, R11, R10, R0),
        ],
        FORCED_TRANSLATION_PHYSICAL | 2,
        0x8000000000000130,
        0x030000008e0b0a00,
        0x1122334455667788,
    ),
    invalid_forced_data_translation_test(
        "software-translation-store-permission",
        [
            wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL),
            wyde(SETL, R11, 0x55),
            insn(STOU, R11, R10, R0),
        ],
        FORCED_TRANSLATION_PHYSICAL | 4,
        0x8000000000000134,
        0x03000000ae0b0a00,
        0x1122334455667788,
    ),
    invalid_forced_data_translation_test(
        "software-translation-load-zero-permissions",
        [
            wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL),
            insn(LDOU, R11, R10, R0),
        ],
        FORCED_TRANSLATION_PHYSICAL,
        0x8000000000000130,
        0x030000008e0b0a00,
        0x1122334455667788,
    ),
    invalid_forced_data_translation_test(
        "software-translation-load-physical-outside-machine",
        [
            wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL),
            insn(LDOU, R11, R10, R0),
        ],
        0x0000001000000007,
        0x8000000000000130,
        0x030000008e0b0a00,
        0x1122334455667788,
    ),
    MMIXTest(
        "software-translation-cached-protection-trap",
        forced_data_translation_program(
            [
                *set_octa(R22, 0x8000000000000300),
                insn(PUT, SR_TT, R0, R22),
                wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL),
                insn(LDOU, R11, R10, R0),
                wyde(SETL, R12, 0x55),
                insn(STOU, R12, R10, R0),
                wyde(SETL, R13, 0xdead),
            ],
            [
                insn(ADDI, R60, R60, 1),
                *set_octa(R50, FORCED_TRANSLATION_PHYSICAL | 4),
                insn(PUT, SR_ZZ, R0, R50),
                insn(RESUME, R0, R0, 1),
            ],
            0x1122334455667788,
            extra_regions=((
                0x300,
                [
                    insn(GET, R70, R0, SR_Q),
                    insn(GET, R71, R0, SR_XX),
                    insn(GET, R72, R0, SR_WW),
                    halt(),
                ],
            ),),
        ),
        pc=0x800000000000030c,
        regs={
            R11: 0x1122334455667788,
            R13: 0,
            R60: 1,
            R70: RQ_PROGRAM_W,
            R71: RQ_PROGRAM_W |
                 int.from_bytes(insn(STOU, R12, R10, R0), "big"),
            R72: 0x800000000000014c,
        },
    ),
    MMIXTest(
        "software-translation-instruction-fetch-resume",
        forced_instruction_translation_program(
            FORCED_TRANSLATION_VIRTUAL,
            ((FORCED_TRANSLATION_PHYSICAL, [
                insn(ADDI, R12, R12, 1),
                wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL | 7),
                insn(LDVTS, R11, R10, R0),
                halt(),
            ]),),
            [
                insn(GET, R40, R0, SR_WW),
                insn(GET, R41, R0, SR_XX),
                insn(GET, R42, R0, SR_YY),
                insn(ADDI, R60, R60, 1),
                *set_octa(R50, FORCED_TRANSLATION_PHYSICAL | 7),
                insn(PUT, SR_ZZ, R0, R50),
                insn(RESUME, R0, R0, 1),
            ],
        ),
        pc=FORCED_TRANSLATION_VIRTUAL + 0xc,
        regs={
            R11: 1,
            R12: 1,
            R40: FORCED_TRANSLATION_VIRTUAL,
            R41: 0x03000000fd000000,
            R42: FORCED_TRANSLATION_VIRTUAL,
            R60: 1,
        },
    ),
    MMIXTest(
        "software-translation-instruction-fetch-permission",
        forced_instruction_translation_program(
            FORCED_TRANSLATION_VIRTUAL,
            ((FORCED_TRANSLATION_PHYSICAL, [
                insn(ADDI, R12, R12, 1),
                halt(),
            ]),),
            [
                insn(GET, R40, R0, SR_WW),
                insn(GET, R41, R0, SR_XX),
                insn(GET, R42, R0, SR_YY),
                *set_octa(R50, FORCED_TRANSLATION_PHYSICAL | 6),
                insn(PUT, SR_ZZ, R0, R50),
                insn(RESUME, R0, R0, 1),
                insn(GET, R46, R0, SR_Q),
                halt(),
            ],
        ),
        pc=NEGATIVE_FORCED_TRANSLATION_HANDLER + 0x28,
        regs={
            R12: 0,
            R40: FORCED_TRANSLATION_VIRTUAL,
            R41: 0x03000000fd000000,
            R42: FORCED_TRANSLATION_VIRTUAL,
            R46: RQ_PROGRAM_B,
        },
    ),
    MMIXTest(
        "software-translation-instruction-fetch-cross-page",
        forced_instruction_translation_program(
            0x3ffc,
            (
                (0x5ffc, [insn(ADDI, R30, R30, 1)]),
                (0x6000, [insn(ADDI, R30, R30, 1), halt()]),
            ),
            [
                insn(GET, R40, R0, SR_YY),
                *set_octa(R50, 0x1fff),
                insn(ANDN, R51, R40, R50),
                wyde(SETL, R52, 0x2000),
                insn(ADDU, R53, R51, R52),
                insn(ORI, R53, R53, 7),
                insn(PUT, SR_ZZ, R0, R53),
                insn(ADDI, R60, R60, 1),
                insn(RESUME, R0, R0, 1),
            ],
        ),
        pc=0x4004,
        regs={
            R30: 2,
            R40: 0x4000,
            R60: 2,
        },
    ),
    MMIXTest(
        "software-translation-register-stack-spill-fill",
        FORCED_STACK_SPILL_FILL[0],
        pc=FORCED_STACK_SPILL_FILL[1],
        regs={
            R151: 2,
            R153: 2,
            R225: 41,
            R226: INITIAL_STACK,
            R227: INITIAL_STACK,
            R228: 32,
            R250: 4,
        },
    ),
    MMIXTest(
        "software-translation-register-stack-save-unsave",
        FORCED_STACK_SAVE_UNSAVE[0],
        pc=FORCED_STACK_SAVE_UNSAVE[1],
        regs={
            R0: 0x11,
            R1: 0x22,
            R40: 0x1122334455667788,
            R62: 0,
            R70: 0x11,
            R71: 0x22,
            R72: 0x1122334455667788,
            R74: 2,
        },
    ),
    MMIXTest(
        "software-translation-dynamic-handler-register-stack",
        FORCED_TRANSLATION_NESTED_HANDLER[0],
        pc=FORCED_TRANSLATION_NESTED_HANDLER[1],
        regs={
            R170: FORCED_TRANSLATION_NESTED_HANDLER[2] + 4,
            R171: DYNAMIC_TRAP_RESUME_NEXT | RQ_PROGRAM_B |
                  int.from_bytes(FORCED_TRANSLATION_NESTED_HANDLER[3], "big"),
            R172: 0,
            R173: 0,
            R174: 0x55,
            R225: 41,
            R226: INITIAL_STACK,
            R227: INITIAL_STACK,
            R228: 32,
            R229: 0x55,
            R230: RQ_PROGRAM_B,
            R250: 4,
        },
    ),
    MMIXTest(
        "software-translation-malformed-saved-opcode",
        forced_data_translation_program(
            [
                wyde(SETL, R10, FORCED_TRANSLATION_VIRTUAL),
                insn(LDOU, R11, R10, R0),
            ],
            [
                *set_octa(R50, 0x0300000021010001),
                insn(PUT, SR_XX, R0, R50),
                *set_octa(R51, FORCED_TRANSLATION_PHYSICAL | 7),
                insn(PUT, SR_ZZ, R0, R51),
                insn(RESUME, R0, R0, 1),
                insn(GET, R52, R0, SR_Q),
                halt(),
            ],
            0x1122334455667788,
        ),
        pc=0x8000000000000230,
        regs={
            R11: 0,
            R52: RQ_PROGRAM_B,
        },
    ),
    MMIXTest(
        "ldvts-user-trap",
        program_with_handler(
            [
                wyde(SETL, R1, 0x40),  # handler address
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, RQ_PROGRAM_K),
                insn(PUT, SR_K, 0, R2),
                insn(LDVTSI, R3, R0, 7),
                wyde(SETL, R4, 0xee),      # skipped after dynamic trap
            ],
            0x40,
            [
                insn(GET, R40, 0, SR_Q),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_WW),
                insn(GET, R43, 0, SR_K),
                halt(),
            ],
        ),
        pc=0x50,
        regs={
            R3: 0,
            R4: 0,
            R40: RQ_PROGRAM_K,
            R41: RQ_PROGRAM_K |
                 int.from_bytes(insn(LDVTSI, R3, R0, 7), "big"),
            R42: 0x20,
            R43: 0,
        },
    ),
    MMIXTest(
        "special-register-get-all",
        b"".join(
            [
                *[
                    insn(GET, 33 + reg, 0, reg)
                    for reg in range(32)
                ],
                halt(),
            ]
        ),
        pc=0x80,
        regs={
            **{33 + reg: 0 for reg in range(32)},
            33 + 10: INITIAL_STACK,
            33 + 11: INITIAL_STACK,
            33 + 13: 0x8000000500000000,
            33 + 14: 0x8000000600000000,
            33 + 15: 0,
            33 + 18: 0x369c200400000000,
            33 + 19: 32,
        },
    ),
    MMIXTest(
        "special-register-put-readback",
        b"".join(
            [
                wyde(SETH, R1, 0xfeed),
                wyde(INCMH, R1, 0xcafe),
                wyde(INCML, R1, 0x1234),
                wyde(INCL, R1, 0x5678),
                insn(PUT, SR_J, 0, R1),
                insn(GET, R2, 0, SR_J),
                insn(PUTI, SR_M, 0, 0x7b),
                insn(GET, R3, 0, SR_M),
                insn(PUT, SR_WW, 0, R1),
                insn(GET, R4, 0, SR_WW),
                insn(PUTI, SR_C, 0, 0x11),
                insn(PUTI, SR_I, 0, 0x12),
                insn(PUTI, SR_K, 0, 0x13),
                insn(PUTI, SR_Q, 0, 0x14),
                insn(PUTI, SR_T, 0, 0x15),
                insn(PUTI, SR_U, 0, 0x16),
                insn(PUTI, SR_TT, 0, 0x17),
                insn(PUTI, SR_P, 0, 0x18),
                insn(GET, R40, 0, SR_C),
                insn(GET, R41, 0, SR_I),
                insn(GET, R42, 0, SR_K),
                insn(GET, R43, 0, SR_Q),
                insn(GET, R44, 0, SR_T),
                insn(GET, R45, 0, SR_U),
                insn(GET, R46, 0, SR_TT),
                insn(GET, R47, 0, SR_P),
                halt(),
            ]
        ),
        pc=0x68,
        regs={
            R1: 0xfeedcafe12345678,
            R2: 0xfeedcafe12345678,
            R3: 0x7b,
            R4: 0xfeedcafe12345678,
            R40: 0x11,
            R41: 0x12,
            R42: 0x13,
            R43: 0x14,
            R44: 0x15,
            R45: 0x16,
            R46: 0x17,
            R47: 0x18,
        },
    ),
    MMIXTest(
        "special-register-ra-mask",
        b"".join(
            [
                *set_octa(R1, 0xffffffff0003ffff),
                insn(PUT, SR_A, 0, R1),
                insn(GET, R33, 0, SR_A),
                halt(),
            ]
        ),
        pc=0x18,
        regs={R33: 0x3ffff},
    ),
    MMIXTest(
        "special-register-rg-rl-policy",
        b"".join(
            [
                wyde(SETL, R1, 64),
                insn(PUT, SR_G, 0, R1),
                wyde(SETL, R40, 0x00aa),
                insn(GET, R70, 0, SR_L),
                wyde(SETL, R2, 40),
                insn(PUT, SR_G, 0, R2),
                insn(GET, R65, 0, SR_G),
                insn(GET, R66, 0, SR_L),
                halt(),
            ]
        ),
        pc=0x20,
        regs={
            R65: 40,
            R66: 40,
            R70: 41,
        },
    ),
    MMIXTest(
        "local-global-registers",
        b"".join(
            [
                insn(ADDI, R2, R1, 5),
                insn(GET, R33, 0, SR_L),
                wyde(SETL, R10, 0x00aa),
                insn(GET, R34, 0, SR_L),
                wyde(SETL, R32, 0x0044),
                insn(GET, R35, 0, SR_L),
                halt(),
            ]
        ),
        pc=0x18,
        regs={
            R1: 0,
            R2: 5,
            R9: 0,
            R10: 0xaa,
            R32: 0x44,
            R33: 3,
            R34: 11,
            R35: 11,
        },
    ),
    MMIXTest(
        "put-rl-narrowing",
        b"".join(
            [
                wyde(SETL, R10, 0x00aa),
                insn(GET, R33, 0, SR_L),
                wyde(SETL, R2, 5),
                insn(PUT, SR_L, 0, R2),
                insn(GET, R34, 0, SR_L),
                insn(ADDI, R11, R10, 0),
                insn(GET, R35, 0, SR_L),
                halt(),
            ]
        ),
        pc=0x1c,
        regs={
            R2: 5,
            R10: 0,
            R11: 0,
            R33: 11,
            R34: 5,
            R35: 12,
        },
    ),
    MMIXTest(
        "existing-integer-logical-variants",
        b"".join(
            [
                insn(ADDI, R1, R0, 0xf0),
                insn(ADDI, R2, R0, 0x0f),
                insn(SWYM, 0, 0, 0),
                insn(SUB, R3, R1, R2),
                insn(ADDU, R4, R3, R2),
                insn(ADDUI, R5, R4, 1),
                insn(SUBU, R6, R5, R2),
                insn(SUBUI, R7, R6, 2),
                insn(OR, R8, R1, R2),
                insn(XOR, R9, R1, R2),
                insn(AND, R10, R1, R2),
                halt(),
            ]
        ),
        pc=0x2c,
        regs={
            R1: 0xf0,
            R2: 0x0f,
            R3: 0xe1,
            R4: 0xf0,
            R5: 0xf1,
            R6: 0xe2,
            R7: 0xe0,
            R8: 0xff,
            R9: 0xff,
            R10: 0,
        },
    ),
    MMIXTest(
        "wyde-constants",
        b"".join(
            [
                wyde(SETH, R1, 0x1234),
                wyde(SETMH, R2, 0x5678),
                wyde(SETML, R3, 0x9abc),
                wyde(SETL, R4, 0xdef0),
                wyde(SETH, R5, 0x1111),
                wyde(INCMH, R5, 0x2222),
                wyde(INCML, R5, 0x3333),
                wyde(INCL, R5, 0x4444),
                wyde(SETL, R6, 0xffff),
                wyde(INCL, R6, 0x0001),
                wyde(SETH, R7, 0xffff),
                wyde(INCH, R7, 0x0001),
                halt(),
            ]
        ),
        pc=0x30,
        regs={
            R1: 0x1234000000000000,
            R2: 0x0000567800000000,
            R3: 0x000000009abc0000,
            R4: 0x000000000000def0,
            R5: 0x1111222233334444,
            R6: 0x0000000000010000,
            R7: 0,
        },
    ),
    MMIXTest(
        "scaled-unsigned-add",
        b"".join(
            [
                wyde(SETL, R1, 7),
                wyde(SETL, R2, 3),
                insn(TWO_ADDU, R3, R1, R2),
                insn(TWO_ADDUI, R4, R1, 5),
                insn(FOUR_ADDU, R5, R1, R2),
                insn(FOUR_ADDUI, R6, R1, 5),
                insn(EIGHT_ADDU, R7, R1, R2),
                insn(EIGHT_ADDUI, R8, R1, 5),
                insn(SIXTEEN_ADDU, R9, R1, R2),
                insn(SIXTEEN_ADDUI, R10, R1, 5),
                wyde(SETH, R11, 0x8000),
                insn(TWO_ADDUI, R12, R11, 0),
                halt(),
            ]
        ),
        pc=0x30,
        regs={
            R3: 17,
            R4: 19,
            R5: 31,
            R6: 33,
            R7: 59,
            R8: 61,
            R9: 115,
            R10: 117,
            R12: 0,
        },
    ),
    MMIXTest(
        "logical-complement",
        b"".join(
            [
                wyde(SETH, R1, 0xf0f0),
                wyde(INCMH, R1, 0xf0f0),
                wyde(INCML, R1, 0xf0f0),
                wyde(INCL, R1, 0xf0f0),
                wyde(SETH, R2, 0x0ff0),
                wyde(INCMH, R2, 0x0ff0),
                wyde(INCML, R2, 0x0ff0),
                wyde(INCL, R2, 0x0ff0),
                insn(ANDN, R3, R1, R2),
                insn(ORN, R4, R1, R2),
                insn(NOR, R5, R1, R2),
                insn(NAND, R6, R1, R2),
                insn(NXOR, R7, R1, R2),
                wyde(SETL, R8, 0x00f0),
                insn(ANDNI, R9, R8, 0x0f),
                insn(ORNI, R10, R8, 0x0f),
                insn(NORI, R11, R8, 0x0f),
                insn(NANDI, R12, R8, 0x0f),
                insn(NXORI, R13, R8, 0x0f),
                halt(),
            ]
        ),
        pc=0x4c,
        regs={
            R1: 0xf0f0f0f0f0f0f0f0,
            R2: 0x0ff00ff00ff00ff0,
            R3: 0xf000f000f000f000,
            R4: 0xf0fff0fff0fff0ff,
            R5: 0x000f000f000f000f,
            R6: 0xff0fff0fff0fff0f,
            R7: 0x00ff00ff00ff00ff,
            R8: 0xf0,
            R9: 0xf0,
            R10: 0xfffffffffffffff0,
            R11: 0xffffffffffffff00,
            R12: MASK64,
            R13: 0xffffffffffffff00,
        },
    ),
    MMIXTest(
        "wyde-logical-immediates",
        b"".join(
            [
                *set_octa(R1, 0x1111222233334444),
                wyde(ORH, R1, 0x8000),
                wyde(ORMH, R1, 0x0800),
                wyde(ORML, R1, 0x0080),
                wyde(ORL, R1, 0x0008),
                *set_octa(R2, MASK64),
                wyde(ANDNH, R2, 0xf0f0),
                wyde(ANDNMH, R2, 0x0f0f),
                wyde(ANDNML, R2, 0xaaaa),
                wyde(ANDNL, R2, 0x5555),
                halt(),
            ]
        ),
        pc=0x40,
        regs={R1: 0x91112a2233b3444c, R2: 0x0f0ff0f05555aaaa},
    ),
    MMIXTest(
        "unsigned-negate",
        b"".join(
            [
                wyde(SETL, R1, 5),
                insn(NEGU, R2, 10, R1),
                insn(NEGUI, R3, 1, 2),
                insn(NEGU, R4, 0, R1),
                insn(NEGUI, R5, 0, 0),
                halt(),
            ]
        ),
        pc=0x14,
        regs={R1: 5, R2: 5, R3: MASK64, R4: MASK64 - 4, R5: 0},
    ),
    MMIXTest(
        "signed-negate",
        b"".join(
            [
                wyde(SETL, R1, 5),
                wyde(SETL, R2, 2),
                insn(NEG, R3, 10, R1),
                insn(NEGI, R4, 1, 2),
                *set_octa(R5, 0x8000000000000000),
                insn(NEG, R6, 0, R5),
                insn(GET, R7, 0, SR_A),
                halt(),
            ]
        ),
        pc=0x28,
        regs={
            R3: 5,
            R4: MASK64,
            R6: 0x8000000000000000,
            R7: RA_EVENT_V,
        },
    ),
    MMIXTest(
        "enabled-signed-negate-overflow-trip",
        program_with_handler(
            [
                *set_octa(R1, 0x8000000000000000),
                wyde(SETL, R2, 1),
                wyde(SETL, R4, RA_EVENT_V << RA_ENABLE_SHIFT),
                insn(PUT, SR_A, 0, R4),
                insn(NEG, R3, 0, R1),
            ],
            32,
            [
                insn(GET, R40, 0, SR_W),
                insn(GET, R41, 0, SR_X),
                insn(GET, R42, 0, SR_Y),
                insn(GET, R43, 0, SR_Z),
                insn(GET, R44, 0, SR_A),
                halt(),
            ],
        ),
        pc=0x34,
        regs={
            R40: 0x20,
            R41: 0x8000000034030001,
            R42: 0,
            R43: 0x8000000000000000,
            R44: RA_EVENT_V << RA_ENABLE_SHIFT,
        },
    ),
    MMIXTest(
        "low-risk-shifts",
        b"".join(
            [
                wyde(SETL, R1, 1),
                wyde(SETL, R2, 64),
                insn(NEGUI, R3, 0, 8),
                insn(SLUI, R4, R1, 63),
                insn(SLU, R5, R1, R2),
                insn(SRI, R6, R3, 1),
                insn(SR, R7, R3, R2),
                insn(SRUI, R8, R3, 1),
                insn(SRU, R9, R3, R2),
                insn(SRUI, R10, R1, 0),
                halt(),
            ]
        ),
        pc=0x28,
        regs={
            R1: 1,
            R2: 64,
            R3: MASK64 - 7,
            R4: 0x8000000000000000,
            R5: 0,
            R6: MASK64 - 3,
            R7: MASK64,
            R8: 0x7ffffffffffffffc,
            R9: 0,
            R10: 1,
        },
    ),
    MMIXTest(
        "signed-shift-left",
        b"".join(
            [
                wyde(SETL, R1, 2),
                wyde(SETL, R2, 4),
                insn(SLI, R3, R1, 4),
                insn(SL, R4, R1, R2),
                insn(SLI, R5, R1, 62),
                wyde(SETL, R6, 64),
                insn(SL, R7, R1, R6),
                insn(SLI, R8, R0, 64),
                insn(GET, R9, 0, SR_A),
                halt(),
            ]
        ),
        pc=0x24,
        regs={
            R3: 32,
            R4: 32,
            R5: 0x8000000000000000,
            R7: 0,
            R8: 0,
            R9: RA_EVENT_V,
        },
    ),
    MMIXTest(
        "enabled-signed-shift-left-overflow-trip",
        program_with_handler(
            [
                wyde(SETL, R1, 2),
                wyde(SETL, R2, 62),
                wyde(SETL, R4, RA_EVENT_V << RA_ENABLE_SHIFT),
                insn(PUT, SR_A, 0, R4),
                insn(SL, R3, R1, R2),
            ],
            32,
            [
                insn(GET, R40, 0, SR_W),
                insn(GET, R41, 0, SR_X),
                insn(GET, R42, 0, SR_Y),
                insn(GET, R43, 0, SR_Z),
                insn(GET, R44, 0, SR_A),
                halt(),
            ],
        ),
        pc=0x34,
        regs={
            R40: 0x14,
            R41: 0x8000000038030102,
            R42: 2,
            R43: 62,
            R44: RA_EVENT_V << RA_ENABLE_SHIFT,
        },
    ),
    MMIXTest(
        "bit-difference",
        b"".join(
            [
                *set_octa(R1, 0x1020304050607080),
                *set_octa(R2, 0x0111223344556677),
                insn(BDIF, R3, R1, R2),
                insn(BDIFI, R4, R1, 0x10),
                insn(WDIF, R5, R1, R2),
                insn(TDIF, R6, R1, R2),
                insn(ODIF, R7, R1, R2),
                insn(ODIFI, R8, R1, 0x80),
                halt(),
            ]
        ),
        pc=0x38,
        regs={
            R3: lane_difference(0x1020304050607080, 0x0111223344556677, 8),
            R4: lane_difference(0x1020304050607080, 0x10, 8),
            R5: lane_difference(0x1020304050607080, 0x0111223344556677, 16),
            R6: lane_difference(0x1020304050607080, 0x0111223344556677, 32),
            R7: 0x0f0f0e0d0c0b0a09,
            R8: 0x1020304050607000,
        },
    ),
    MMIXTest(
        "sideways-add",
        b"".join(
            [
                *set_octa(R1, MASK64),
                *set_octa(R2, 0xf0f0f0f0f0f0f0f0),
                insn(SADD, R3, R1, R0),
                insn(SADD, R4, R1, R2),
                insn(SADDI, R5, R2, 0xf0),
                insn(SADDI, R6, R0, 0xff),
                halt(),
            ]
        ),
        pc=0x30,
        regs={
            R3: sadd(MASK64, 0),
            R4: sadd(MASK64, 0xf0f0f0f0f0f0f0f0),
            R5: sadd(0xf0f0f0f0f0f0f0f0, 0xf0),
            R6: 0,
        },
    ),
    MMIXTest(
        "bit-matrix",
        b"".join(
            [
                *set_octa(R1, 0x1122334455667788),
                *set_octa(R2, 0x8040201008040201),
                *set_octa(R3, 0x0102040810204080),
                insn(MOR, R4, R1, R2),
                insn(MXOR, R5, R1, R2),
                insn(MOR, R6, R1, R3),
                insn(MXOR, R7, R1, R3),
                insn(MORI, R8, R1, 0xff),
                insn(MXORI, R9, R1, 0xff),
                halt(),
            ]
        ),
        pc=0x48,
        regs={
            R4: matrix_multiply(0x1122334455667788, 0x8040201008040201, False),
            R5: matrix_multiply(0x1122334455667788, 0x8040201008040201, True),
            R6: matrix_multiply(0x1122334455667788, 0x0102040810204080, False),
            R7: matrix_multiply(0x1122334455667788, 0x0102040810204080, True),
            R8: matrix_multiply(0x1122334455667788, 0xff, False),
            R9: matrix_multiply(0x1122334455667788, 0xff, True),
        },
    ),
    MMIXTest(
        "integer-multiply",
        b"".join(
            [
                *set_octa(R1, 0xfffffffffffffff0),
                wyde(SETL, R2, 3),
                insn(MUL, R3, R1, R2),
                insn(MULI, R4, R1, 5),
                *set_octa(R5, MASK64),
                insn(MULU, R6, R5, R5),
                insn(GET, R7, 0, SR_H),
                insn(MULUI, R8, R5, 2),
                insn(GET, R9, 0, SR_H),
                halt(),
            ]
        ),
        pc=0x3c,
        regs={
            R3: (-16 * 3) & MASK64,
            R4: (-16 * 5) & MASK64,
            R6: 1,
            R7: MASK64 - 1,
            R8: MASK64 - 1,
            R9: 1,
        },
    ),
    MMIXTest(
        "integer-multiply-overflow-status",
        b"".join(
            [
                *set_octa(R1, 0x7fffffffffffffff),
                wyde(SETL, R2, 2),
                insn(MUL, R3, R1, R2),
                insn(GET, R4, 0, SR_A),
                halt(),
            ]
        ),
        pc=0x1c,
        regs={R3: MASK64 - 1, R4: RA_EVENT_V},
    ),
    MMIXTest(
        "enabled-integer-multiply-overflow-trip",
        program_with_handler(
            [
                *set_octa(R1, 0x7fffffffffffffff),
                wyde(SETL, R2, 2),
                wyde(SETL, R4, RA_EVENT_V << RA_ENABLE_SHIFT),
                insn(PUT, SR_A, 0, R4),
                insn(MUL, R3, R1, R2),
            ],
            32,
            [
                insn(GET, R40, 0, SR_W),
                insn(GET, R41, 0, SR_X),
                insn(GET, R42, 0, SR_Y),
                insn(GET, R43, 0, SR_Z),
                insn(GET, R44, 0, SR_A),
                halt(),
            ],
        ),
        pc=0x34,
        regs={
            R40: 0x20,
            R41: 0x8000000018030102,
            R42: 0x7fffffffffffffff,
            R43: 2,
            R44: RA_EVENT_V << RA_ENABLE_SHIFT,
        },
    ),
    MMIXTest(
        "integer-divide",
        b"".join(
            [
                *set_octa(R1, (-7) & MASK64),
                wyde(SETL, R2, 3),
                insn(DIV, R3, R1, R2),
                insn(GET, R4, 0, SR_R),
                wyde(SETL, R5, 7),
                *set_octa(R6, (-3) & MASK64),
                insn(DIV, R7, R5, R6),
                insn(GET, R8, 0, SR_R),
                insn(DIVI, R9, R1, 3),
                insn(GET, R10, 0, SR_R),
                insn(DIV, R11, R5, R0),
                insn(GET, R12, 0, SR_R),
                insn(GET, R13, 0, SR_A),
                halt(),
            ]
        ),
        pc=0x4c,
        regs={
            R3: signed_div((-7) & MASK64, 3)[0],
            R4: signed_div((-7) & MASK64, 3)[1],
            R7: signed_div(7, (-3) & MASK64)[0],
            R8: signed_div(7, (-3) & MASK64)[1],
            R9: signed_div((-7) & MASK64, 3)[0],
            R10: signed_div((-7) & MASK64, 3)[1],
            R11: 0,
            R12: 7,
            R13: RA_EVENT_D,
        },
    ),
    MMIXTest(
        "integer-divide-overflow-status",
        b"".join(
            [
                *set_octa(R1, 0x8000000000000000),
                *set_octa(R2, MASK64),
                insn(DIV, R3, R1, R2),
                insn(GET, R4, 0, SR_R),
                insn(GET, R5, 0, SR_A),
                halt(),
            ]
        ),
        pc=0x2c,
        regs={R3: 0x8000000000000000, R4: 0, R5: RA_EVENT_V},
    ),
    MMIXTest(
        "integer-unsigned-divide",
        b"".join(
            [
                wyde(SETL, R1, 1),
                insn(PUT, SR_D, 0, R1),
                wyde(SETL, R3, 2),
                insn(DIVU, R4, R0, R3),
                insn(GET, R5, 0, SR_R),
                insn(GET, R6, 0, SR_D),
                wyde(SETL, R7, 5),
                insn(PUT, SR_D, 0, R7),
                wyde(SETL, R8, 0x1234),
                insn(DIVU, R9, R8, R7),
                insn(GET, R10, 0, SR_R),
                wyde(SETL, R11, 1),
                insn(PUT, SR_D, 0, R11),
                insn(DIVUI, R12, R0, 2),
                insn(GET, R13, 0, SR_R),
                halt(),
            ]
        ),
        pc=0x3c,
        regs={
            R4: unsigned_div(1, 0, 2)[0],
            R5: unsigned_div(1, 0, 2)[1],
            R6: 1,
            R9: 5,
            R10: 0x1234,
            R12: unsigned_div(1, 0, 2)[0],
            R13: unsigned_div(1, 0, 2)[1],
        },
    ),
    MMIXTest(
        "bit-mux",
        b"".join(
            [
                *set_octa(R1, 0xff00ff00ff00ff00),
                insn(PUT, SR_M, 0, R1),
                *set_octa(R2, MASK64),
                *set_octa(R3, 0x123456789abcdef0),
                insn(MUX, R4, R2, R3),
                insn(MUXI, R5, R3, 0xaa),
                insn(PUTI, SR_M, 0, 0),
                insn(MUX, R6, R2, R3),
                *set_octa(R7, MASK64),
                insn(PUT, SR_M, 0, R7),
                insn(MUX, R8, R2, R3),
                insn(GET, R9, 0, SR_M),
                halt(),
            ]
        ),
        pc=0x60,
        regs={
            R4: mux(MASK64, 0x123456789abcdef0, 0xff00ff00ff00ff00),
            R5: mux(0x123456789abcdef0, 0xaa, 0xff00ff00ff00ff00),
            R6: 0x123456789abcdef0,
            R8: MASK64,
            R9: MASK64,
        },
    ),
    MMIXTest(
        "integer-overflow-status",
        b"".join(
            [
                *set_octa(R1, 0x7fffffffffffffff),
                wyde(SETL, R2, 1),
                insn(ADD, R3, R1, R2),
                insn(GET, R4, 0, SR_A),
                halt(),
            ]
        ),
        pc=0x1c,
        regs={R3: 0x8000000000000000, R4: RA_EVENT_V},
    ),
    MMIXTest(
        "enabled-integer-overflow-trip",
        program_with_handler(
            [
                *set_octa(R1, 0x7fffffffffffffff),
                wyde(SETL, R2, 1),
                wyde(SETL, R4, RA_EVENT_V << RA_ENABLE_SHIFT),
                insn(PUT, SR_A, 0, R4),
                insn(ADD, R3, R1, R2),
            ],
            32,
            [
                insn(GET, R40, 0, SR_W),
                insn(GET, R41, 0, SR_X),
                insn(GET, R42, 0, SR_Y),
                insn(GET, R43, 0, SR_Z),
                insn(GET, R44, 0, SR_A),
                halt(),
            ],
        ),
        pc=0x34,
        regs={
            R40: 0x20,
            R41: 0x8000000020030102,
            R42: 0x7fffffffffffffff,
            R43: 1,
            R44: RA_EVENT_V << RA_ENABLE_SHIFT,
        },
    ),
    MMIXTest(
        "floating-point-compare",
        b"".join(
            [
                *set_octa(R1, f64(1.0)),
                *set_octa(R2, f64(2.0)),
                *set_octa(R3, 0x8000000000000000),
                *set_octa(R5, 0x7ff8000000000001),
                insn(FCMP, R10, R1, R2),
                insn(FCMP, R11, R2, R1),
                insn(FCMP, R12, R1, R1),
                insn(FEQL, R13, R3, R0),
                insn(FUN, R14, R1, R5),
                insn(FCMP, R15, R1, R5),
                insn(GET, R16, 0, SR_A),
                insn(FCMPE, R17, R1, R2),
                insn(FEQLE, R18, R1, R1),
                insn(FUNE, R19, R1, R5),
                halt(),
            ]
        ),
        pc=0x68,
        regs={
            R10: MASK64,
            R11: 1,
            R12: 0,
            R13: 1,
            R14: 1,
            R15: 0,
            R16: RA_EVENT_I,
            R17: MASK64,
            R18: 1,
            R19: 1,
        },
    ),
    MMIXTest(
        "floating-point-arithmetic",
        b"".join(
            [
                *set_octa(R1, f64(1.0)),
                *set_octa(R2, f64(2.0)),
                *set_octa(R3, f64(3.0)),
                *set_octa(R4, f64(4.0)),
                *set_octa(R5, f64(5.0)),
                *set_octa(R6, f64(1.5)),
                insn(FADD, R10, R1, R2),
                insn(FSUB, R11, R2, R1),
                insn(FMUL, R12, R2, R3),
                insn(FDIV, R13, R4, R2),
                insn(FREM, R14, R5, R2),
                insn(FSQRT, R15, 0, R4),
                insn(FINT, R16, 0, R6),
                halt(),
            ]
        ),
        pc=0x7c,
        regs={
            R10: f64(3.0),
            R11: f64(1.0),
            R12: f64(6.0),
            R13: f64(2.0),
            R14: f64(1.0),
            R15: f64(2.0),
            R16: f64(2.0),
        },
    ),
    MMIXTest(
        "floating-point-default-nan",
        b"".join(
            [
                *set_octa(R1, f64(float("inf"))),
                *set_octa(R2, f64(-1.0)),
                insn(FDIV, R10, R0, R0),
                insn(FSUB, R11, R1, R1),
                insn(FMUL, R12, R1, R0),
                insn(FSQRT, R13, 0, R2),
                insn(GET, R14, 0, SR_A),
                halt(),
            ]
        ),
        pc=0x34,
        regs={
            R10: 0x7ff8000000000000,
            R11: 0x7ff8000000000000,
            R12: 0x7ff8000000000000,
            R13: 0x7ff8000000000000,
            R14: RA_EVENT_I,
        },
    ),
    MMIXTest(
        "floating-point-conversion",
        b"".join(
            [
                wyde(SETL, R1, 42),
                insn(FLOT, R10, 0, R1),
                insn(FLOTI, R11, 0, 42),
                insn(FLOTU, R12, 0, R1),
                insn(SFLOTI, R13, 0, 42),
                *set_octa(R2, f64(42.0)),
                insn(FIX, R14, 0, R2),
                insn(FIXU, R15, 0, R2),
                halt(),
            ]
        ),
        pc=0x2c,
        regs={
            R10: f64(42.0),
            R11: f64(42.0),
            R12: f64(42.0),
            R13: f64(42.0),
            R14: 42,
            R15: 42,
        },
    ),
    MMIXTest(
        "floating-point-status",
        b"".join(
            [
                *set_octa(R1, f64(1.0)),
                *set_octa(R3, f64(3.0)),
                insn(FDIV, R10, R1, R0),
                insn(GET, R11, 0, SR_A),
                insn(FDIV, R12, R1, R3),
                insn(GET, R13, 0, SR_A),
                halt(),
            ]
        ),
        pc=0x30,
        regs={
            R10: f64(float("inf")),
            R11: 0x02,
            R12: f64(1.0 / 3.0),
            R13: 0x03,
        },
    ),
    MMIXTest(
        "enabled-floating-divide-trip",
        program_with_handler(
            [
                *set_octa(R1, f64(1.0)),
                wyde(SETL, R2, RA_EVENT_Z << RA_ENABLE_SHIFT),
                insn(PUT, SR_A, 0, R2),
                insn(FDIV, R3, R1, R0),
            ],
            112,
            [
                insn(GET, R40, 0, SR_W),
                insn(GET, R41, 0, SR_X),
                insn(GET, R42, 0, SR_Y),
                insn(GET, R43, 0, SR_Z),
                insn(GET, R44, 0, SR_A),
                halt(),
            ],
        ),
        pc=0x84,
        regs={
            R40: 0x1c,
            R41: 0x8000000014030100,
            R42: f64(1.0),
            R43: 0,
            R44: RA_EVENT_Z << RA_ENABLE_SHIFT,
        },
    ),
    MMIXTest(
        "arithmetic-trip-priority",
        program_with_handler(
            [
                *set_octa(R1, 0x7fefffffffffffff),
                *set_octa(R2, f64(2.0)),
                wyde(SETL, R3, (RA_EVENT_O | RA_EVENT_X) << RA_ENABLE_SHIFT),
                insn(PUT, SR_A, 0, R3),
                insn(FMUL, R4, R1, R2),
            ],
            80,
            [
                insn(GET, R40, 0, SR_W),
                insn(GET, R41, 0, SR_X),
                insn(GET, R42, 0, SR_A),
                halt(),
            ],
        ),
        pc=0x5c,
        regs={
            R40: 0x2c,
            R41: 0x8000000010040102,
            R42: (RA_EVENT_O | RA_EVENT_X) << RA_ENABLE_SHIFT,
        },
    ),
    MMIXTest(
        "explicit-trip-resume",
        b"".join(
            [
                branch(BZ, R10, 12),  # main branch target
                insn(GET, R40, 0, SR_W),
                insn(GET, R41, 0, SR_X),
                insn(GET, R42, 0, SR_Y),
                insn(GET, R43, 0, SR_Z),
                insn(GET, R44, 0, SR_B),
                insn(RESUME, 0, 0, 0),
                insn(SWYM, 0, 0, 0),      # padding
                insn(SWYM, 0, 0, 0),      # padding
                insn(SWYM, 0, 0, 0),      # padding
                insn(SWYM, 0, 0, 0),      # padding
                insn(SWYM, 0, 0, 0),      # padding
                wyde(SETL, R10, 1),  # main
                wyde(SETL, R1, 0x00aa),
                wyde(SETL, R2, 0x00bb),
                insn(TRIP, 7, R1, R2),
                wyde(SETL, R11, 0x55),
                halt(),
            ]
        ),
        pc=0x44,
        regs={
            R11: 0x55,
            R40: 0x40,
            R41: 0x80000000ff070102,
            R42: 0xaa,
            R43: 0xbb,
            R44: 0,
        },
    ),
    MMIXTest(
        "explicit-trap-state",
        program_with_handler(
            [
                wyde(SETL, R1, 0x40),  # handler address
                insn(PUT, SR_T, 0, R1),
                wyde(SETL, R2, 0x00aa),
                wyde(SETL, R3, 0x00bb),
                wyde(SETL, R4, 0x00dd),
                insn(PUT, SR_J, 0, R4),
                wyde(SETL, R255, 0x00cc),
                insn(TRAP, 1, 2, 3),
            ],
            0x40,
            [
                insn(GET, R40, 0, SR_WW),
                insn(GET, R41, 0, SR_XX),
                insn(GET, R42, 0, SR_YY),
                insn(GET, R43, 0, SR_ZZ),
                insn(GET, R44, 0, SR_BB),
                insn(GET, R45, 0, SR_K),
                insn(ADDI, R46, R255, 0),
                wyde(SETL, R255, 0),
                halt(),
            ],
        ),
        pc=0x60,
        regs={
            R40: 0x20,
            R41: 0x8000000000010203,
            R42: 0xaa,
            R43: 0xbb,
            R44: 0xcc,
            R45: 0,
            R46: 0xdd,
        },
    ),
    MMIXTest(
        "explicit-trap-resume",
        program_with_handler(
            [
                wyde(SETL, R1, 0x40),  # handler address
                insn(PUT, SR_T, 0, R1),
                wyde(SETL, R4, 0x00dd),
                insn(PUT, SR_J, 0, R4),
                wyde(SETL, R255, 0x00cc),
                insn(TRAP, 1, 0, 0),
                wyde(SETL, R10, 0x55),
                insn(GET, R11, 0, SR_K),
                insn(ADDI, R12, R255, 0),
                wyde(SETL, R255, 0),
                halt(),
            ],
            0x40,
            [
                wyde(SETL, R255, 0x0123),
                insn(RESUME, 0, 0, 1),
            ],
        ),
        pc=0x28,
        regs={R10: 0x55, R11: 0x123, R12: 0xcc},
    ),
    MMIXTest(
        "arithmetic-trip-resume",
        program_with_handler(
            [
                *set_octa(R1, f64(1.0)),
                wyde(SETL, R2, RA_EVENT_Z << RA_ENABLE_SHIFT),
                insn(PUT, SR_A, 0, R2),
                insn(FDIV, R3, R1, R0),
                wyde(SETL, R10, 0x55),
                insn(GET, R11, 0, SR_A),
                halt(),
            ],
            112,
            [
                insn(RESUME, 0, 0, 0),
            ],
        ),
        pc=0x24,
        regs={R3: 0, R10: 0x55, R11: RA_EVENT_Z << RA_ENABLE_SHIFT},
    ),
    MMIXTest(
        "resume-ropcode-result",
        program_with_handler(
            [
                wyde(SETL, R1, 0x40),  # target address
                insn(PUT, SR_W, 0, R1),
                *set_octa(R2, 0x0200000021050007),
                insn(PUT, SR_X, 0, R2),
                wyde(SETL, R3, 0x77),
                insn(PUT, SR_Z, 0, R3),
                insn(RESUME, 0, 0, 0),
            ],
            0x40,
            [
                halt(),
            ],
        ),
        pc=0x40,
        regs={R5: 0x77},
    ),
    MMIXTest(
        "resume-ropcode0-integer-replay",
        program_with_regions(
            (
                0,
                [
                    wyde(SETL, R11, 0x20),
                    wyde(SETL, R1, 0x80),
                    insn(PUT, SR_W, 0, R1),
                    *set_octa(
                        R2, int.from_bytes(insn(ADDI, R10, R11, 7), "big")
                    ),
                    insn(PUT, SR_X, 0, R2),
                    wyde(SETL, R3, 0xaa),
                    insn(PUT, SR_Y, 0, R3),
                    wyde(SETL, R4, 0xbb),
                    insn(PUT, SR_Z, 0, R4),
                    insn(RESUME, 0, 0, 0),
                ],
            ),
            (0x7c, [wyde(SETL, R10, 0xdead)]),
            (
                0x80,
                [
                    insn(GET, R40, 0, SR_W),
                    insn(GET, R41, 0, SR_X),
                    insn(GET, R42, 0, SR_Y),
                    insn(GET, R43, 0, SR_Z),
                    halt(),
                ],
            ),
        ),
        pc=0x90,
        regs={
            R10: 0x27,
            R40: 0x80,
            R41: int.from_bytes(insn(ADDI, R10, R11, 7), "big"),
            R42: 0xaa,
            R43: 0xbb,
        },
    ),
    MMIXTest(
        "resume-ropcode0-logical-replay",
        program_with_regions(
            (
                0,
                [
                    *set_octa(R13, 0xf0f0f0f00f0f0f0f),
                    *set_octa(R14, 0xff00ff00ff00ff00),
                    wyde(SETL, R1, 0xa0),
                    insn(PUT, SR_W, 0, R1),
                    *set_octa(
                        R2, int.from_bytes(insn(XOR, R12, R13, R14), "big")
                    ),
                    insn(PUT, SR_X, 0, R2),
                    insn(RESUME, 0, 0, 0),
                ],
            ),
            (0x9c, [wyde(SETL, R12, 0xbeef)]),
            (0xa0, [halt()]),
        ),
        pc=0xa0,
        regs={R12: 0x0ff00ff0f00ff00f},
    ),
    MMIXTest(
        "resume-ropcode0-inserted-resume-rule-break",
        program_with_regions(
            (
                0,
                [
                    wyde(SETL, R1, 0x100),
                    insn(PUT, SR_TT, 0, R1),
                    *set_octa(R2, RQ_PROGRAM_B),
                    insn(PUT, SR_K, 0, R2),
                    wyde(SETL, R3, 0x80),
                    insn(PUT, SR_W, 0, R3),
                    *set_octa(
                        R4,
                        int.from_bytes(insn(RESUME, 0, 0, 0), "big"),
                    ),
                    insn(PUT, SR_X, 0, R4),
                    insn(RESUME, 0, 0, 0),
                ],
            ),
            (0x7c, [wyde(SETL, R10, 0xdead)]),
            (0x80, [wyde(SETL, R11, 0x55), halt()]),
            (
                0x100,
                [
                    insn(GET, R40, 0, SR_Q),
                    insn(GET, R41, 0, SR_WW),
                    insn(GET, R42, 0, SR_XX),
                    insn(PUTI, SR_Q, 0, 0),
                    insn(ADDU, R255, R2, R0),
                    insn(RESUME, 0, 0, 1),
                ],
            ),
        ),
        pc=0x84,
        regs={
            R10: 0,
            R11: 0x55,
            R40: RQ_PROGRAM_B,
            R41: 0x80,
            R42: DYNAMIC_TRAP_RESUME_NEXT | RQ_PROGRAM_B |
                 int.from_bytes(insn(RESUME, 0, 0, 0), "big"),
        },
    ),
    MMIXTest(
        "resume-ropcode0-branch-taken-replay",
        RESUME0_BRANCH_TAKEN_REPLAY,
        pc=0x84,
        regs={R11: 0x33, R12: 0x77, R13: 0x55},
    ),
    MMIXTest(
        "resume-ropcode0-branch-not-taken-replay",
        RESUME0_BRANCH_NOT_TAKEN_REPLAY,
        pc=0x84,
        regs={R11: 0, R12: 0, R13: 0x55},
    ),
    MMIXTest(
        "resume-ropcode0-geta-replay",
        program_with_regions(
            (
                0,
                resume0_replay_setup(
                    branch(GETA, R12, (0xb0 - 0x9c) // 4), 0xa0
                ),
            ),
            (0x9c, [wyde(SETL, R12, 0xdead)]),
            (0xa0, [insn(ADDU, R40, R12, R0), halt()]),
        ),
        pc=0xa4,
        regs={R12: 0xb0, R40: 0xb0},
    ),
    MMIXTest(
        "resume-ropcode0-jump-replay",
        program_with_regions(
            (
                0,
                resume0_replay_setup(
                    jump(JMP, (0xd0 - 0xbc) // 4), 0xc0
                ),
            ),
            (0xbc, [wyde(SETL, R10, 0xdead)]),
            (0xc0, [wyde(SETL, R11, 0xdead), halt()]),
            (0xd0, [wyde(SETL, R12, 0x55), halt()]),
        ),
        pc=0xd4,
        regs={R10: 0, R11: 0, R12: 0x55},
    ),
    MMIXTest(
        "resume-ropcode0-go-replay",
        program_with_regions(
            (
                0,
                resume0_replay_setup(
                    insn(GO, R12, R10, R0),
                    0xa0,
                    setup=set_octa(R10, 0xb0),
                ),
            ),
            (0x9c, [wyde(SETL, R12, 0xdead)]),
            (0xa0, [wyde(SETL, R13, 0xdead), halt()]),
            (0xb0, [insn(ADDU, R40, R12, R0), halt()]),
        ),
        pc=0xb4,
        regs={R12: 0xa0, R13: 0, R40: 0xa0},
    ),
    MMIXTest(
        "resume-ropcode0-pushj-pop-replay",
        program_with_regions(
            (
                0,
                resume0_replay_setup(
                    branch(PUSHJ, R0, (0xa0 - 0x7c) // 4), 0x80
                ),
            ),
            (0x7c, [wyde(SETL, R0, 0xdead)]),
            (0x80, [halt()]),
            (0xa0, [wyde(SETL, R0, 42), insn(POP, 1, 0, 0)]),
        ),
        pc=0x80,
        regs={R0: 42},
    ),
    MMIXTest(
        "resume-ropcode0-arithmetic-trip-replay",
        RESUME0_ARITHMETIC_TRIP_REPLAY,
        pc=0x184,
        regs={
            R12: 0x8000000000000000,
            R40: 0x180,
            R41: DYNAMIC_TRAP_RESUME_NEXT |
                 int.from_bytes(insn(ADD, R12, R10, R11), "big"),
            R42: 0x7fffffffffffffff,
            R43: 1,
            R44: RA_EVENT_V,
            R50: 1,
        },
    ),
    MMIXTest(
        "resume-ropcode0-explicit-trip-replay",
        RESUME0_EXPLICIT_TRIP_REPLAY,
        pc=0x184,
        regs={
            R12: 0,
            R13: 0x55,
            R40: 0x180,
            R41: DYNAMIC_TRAP_RESUME_NEXT |
                 int.from_bytes(insn(TRIP, 7, R10, R11), "big"),
            R42: 0xaa,
            R43: 0xbb,
        },
    ),
    MMIXTest(
        "resume-ropcode0-explicit-trap-replay",
        RESUME0_EXPLICIT_TRAP_REPLAY,
        pc=0x10c,
        regs={
            R12: 0,
            R40: 0x100,
            R41: DYNAMIC_TRAP_RESUME_NEXT |
                 int.from_bytes(insn(TRAP, 1, R10, R11), "big"),
            R42: 0xaa,
            R43: 0xbb,
            R44: 0xcc,
            R45: 0xdd,
            R46: RQ_PROGRAM_B,
            R47: 0xcc,
        },
    ),
    *RECOVERABLE_LOAD_REPLAY_TESTS,
    *RECOVERABLE_STORE_REPLAY_TESTS,
    MMIXTest(
        "resume1-ropcode0-replay",
        RESUME1_ROPCODE0_REPLAY[0],
        pc=RESUME1_ROPCODE0_REPLAY[1],
        regs={
            R10: 0x27,
            R40: RESUME1_ROPCODE0_REPLAY[2],
            R41: RESUME1_ROPCODE0_REPLAY[3],
            R42: 0xaa,
            R43: 0xbb,
            R44: RQ_PROGRAM_B,
            R45: 0x55,
        },
    ),
    MMIXTest(
        "resume1-ropcode0-positive-location",
        program_with_handler(
            [
                wyde(SETL, R1, 0x100),
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, RQ_PROGRAM_B),
                insn(PUT, SR_K, 0, R2),
                wyde(SETL, R10, 0x20),
                jump(SYNC, 8),
            ],
            0x100,
            [
                *set_octa(
                    R3, int.from_bytes(insn(ADDI, R10, R10, 7), "big")
                ),
                insn(PUT, SR_XX, 0, R3),
                insn(ADDU, R255, R2, R0),
                insn(RESUME, 0, 0, 1),
                wyde(SETL, R20, 0x55),
                insn(GET, R21, 0, SR_Q),
                wyde(SETL, R255, 0),
                halt(),
            ],
        ),
        pc=0x128,
        regs={R10: 0x20, R20: 0x55, R21: RQ_PROGRAM_B},
    ),
    MMIXTest(
        "resume1-ropcode0-nested-trap",
        RESUME1_NESTED_REPLAY_TRAP[0],
        pc=RESUME1_NESTED_REPLAY_TRAP[1],
        regs={
            R40: RQ_PROGRAM_B,
            R41: 0x55,
            R50: 2,
            R52: RESUME1_NESTED_REPLAY_TRAP[2],
            R53: DYNAMIC_TRAP_RESUME_NEXT | RQ_PROGRAM_B |
                 RESUME1_NESTED_REPLAY_TRAP[3],
        },
    ),
    MMIXTest(
        "floating-point-exceptions",
        b"".join(
            [
                *set_octa(R1, 0x7ff8000000001234),
                *set_octa(R2, 0x7ff0000000001234),
                *set_octa(R3, f64(1.0)),
                *set_octa(R4, 0x7fefffffffffffff),
                *set_octa(R5, f64(2.0)),
                *set_octa(R6, 0x0010000000000000),
                *set_octa(R7, 0x8000000000000000),
                insn(FADD, R10, R1, R3),
                insn(FADD, R11, R2, R3),
                insn(GET, R12, 0, SR_A),
                insn(FMUL, R13, R4, R5),
                insn(GET, R14, 0, SR_A),
                insn(FMUL, R15, R6, R6),
                insn(GET, R16, 0, SR_A),
                insn(FIX, R17, 0, R1),
                insn(FADD, R18, R7, R7),
                halt(),
            ]
        ),
        pc=0x94,
        regs={
            R10: 0x7ff8000000001234,
            R11: 0x7ff8000000001234,
            R12: RA_EVENT_I,
            R13: f64(float("inf")),
            R14: RA_EVENT_I | RA_EVENT_O | RA_EVENT_X,
            R15: 0,
            R16: RA_EVENT_I | RA_EVENT_O | RA_EVENT_U | RA_EVENT_X,
            R17: 0x7ff8000000001234,
            R18: 0x8000000000000000,
        },
    ),
    MMIXTest(
        "floating-point-rounding",
        b"".join(
            [
                *set_octa(R1, 0xffffffff00030000),
                insn(PUT, SR_A, 0, R1),
                insn(GET, R2, 0, SR_A),
                *set_octa(R5, f64(1.5)),
                insn(FINT, R6, 0, R5),
                insn(FINT, R7, 4, R5),
                halt(),
            ]
        ),
        pc=0x30,
        regs={R2: 0x30000, R6: f64(1.0), R7: f64(2.0)},
    ),
    MMIXTest(
        "short-float-memory",
        b"".join(
            [
                wyde(SETL, R1, 0x0300),
                wyde(SETML, R2, f32(1.5) >> 16),
                insn(STTU, R2, R1, R0),
                insn(LDSF, R3, R1, R0),
                *set_octa(R4, f64(2.0)),
                insn(STSFI, R4, R1, 4),
                insn(LDTUI, R5, R1, 4),
                *set_octa(R6, f64(1.0 / 3.0)),
                insn(STSFI, R6, R1, 8),
                insn(GET, R7, 0, SR_A),
                halt(),
            ]
        ),
        pc=0x40,
        regs={R3: f64(1.5), R5: f32(2.0), R7: 0x01},
    ),
    MMIXTest(
        "conditional-set",
        b"".join(
            [
                wyde(SETH, R1, 0xffff),
                wyde(INCMH, R1, 0xffff),
                wyde(INCML, R1, 0xffff),
                wyde(INCL, R1, 0xffff),
                wyde(SETL, R3, 5),
                wyde(SETL, R4, 4),
                wyde(SETL, R5, 0x55),
                wyde(SETL, R30, 0xaaaa),
                insn(CSN, R10, R1, R5),
                insn(CSZ, R11, R0, R5),
                insn(CSP, R12, R3, R5),
                insn(CSOD, R13, R3, R5),
                insn(CSNN, R14, R0, R5),
                insn(CSNZ, R15, R3, R5),
                insn(CSNP, R16, R1, R5),
                insn(CSEV, R17, R4, R5),
                wyde(SETL, R18, 0xaaaa),
                insn(CSN, R18, R3, R5),  # false preserves r18
                wyde(SETL, R19, 0xaaaa),
                insn(CSZ, R19, R3, R5),  # false preserves r19
                wyde(SETL, R20, 0xaaaa),
                insn(CSP, R20, R1, R5),  # false preserves r20
                wyde(SETL, R21, 0xaaaa),
                insn(CSOD, R21, R4, R5),  # false preserves r21
                wyde(SETL, R22, 0xaaaa),
                insn(CSNN, R22, R1, R5),  # false preserves r22
                wyde(SETL, R23, 0xaaaa),
                insn(CSNZ, R23, R0, R5),  # false preserves r23
                wyde(SETL, R24, 0xaaaa),
                insn(CSNP, R24, R3, R5),  # false preserves r24
                wyde(SETL, R25, 0xaaaa),
                insn(CSEV, R25, R3, R5),  # false preserves r25
                wyde(SETL, R26, 0xaaaa),
                insn(CSZI, R26, R0, 0x77),
                wyde(SETL, R27, 0xaaaa),
                insn(CSNZI, R27, R0, 0x77),  # false preserves r27
                halt(),
            ]
        ),
        pc=0x90,
        regs={
            R10: 0x55,
            R11: 0x55,
            R12: 0x55,
            R13: 0x55,
            R14: 0x55,
            R15: 0x55,
            R16: 0x55,
            R17: 0x55,
            R18: 0xaaaa,
            R19: 0xaaaa,
            R20: 0xaaaa,
            R21: 0xaaaa,
            R22: 0xaaaa,
            R23: 0xaaaa,
            R24: 0xaaaa,
            R25: 0xaaaa,
            R26: 0x77,
            R27: 0xaaaa,
        },
    ),
    MMIXTest(
        "zero-or-set",
        b"".join(
            [
                wyde(SETH, R1, 0xffff),
                wyde(INCMH, R1, 0xffff),
                wyde(INCML, R1, 0xffff),
                wyde(INCL, R1, 0xffff),
                wyde(SETL, R3, 5),
                wyde(SETL, R4, 4),
                wyde(SETL, R5, 0x55),
                insn(ZSN, R10, R1, R5),
                insn(ZSZ, R11, R0, R5),
                insn(ZSP, R12, R3, R5),
                insn(ZSOD, R13, R3, R5),
                insn(ZSNN, R14, R0, R5),
                insn(ZSNZ, R15, R3, R5),
                insn(ZSNP, R16, R1, R5),
                insn(ZSEV, R17, R4, R5),
                insn(ZSN, R18, R3, R5),  # false writes zero
                insn(ZSZ, R19, R3, R5),  # false writes zero
                insn(ZSP, R20, R1, R5),  # false writes zero
                insn(ZSOD, R21, R4, R5),  # false writes zero
                insn(ZSNN, R22, R1, R5),  # false writes zero
                insn(ZSNZ, R23, R0, R5),  # false writes zero
                insn(ZSNP, R24, R3, R5),  # false writes zero
                insn(ZSEV, R25, R3, R5),  # false writes zero
                insn(ZSZI, R26, R0, 0x77),
                insn(ZSNZI, R27, R0, 0x77),  # false writes zero
                halt(),
            ]
        ),
        pc=0x64,
        regs={
            R10: 0x55,
            R11: 0x55,
            R12: 0x55,
            R13: 0x55,
            R14: 0x55,
            R15: 0x55,
            R16: 0x55,
            R17: 0x55,
            R18: 0,
            R19: 0,
            R20: 0,
            R21: 0,
            R22: 0,
            R23: 0,
            R24: 0,
            R25: 0,
            R26: 0x77,
            R27: 0,
        },
    ),
]
