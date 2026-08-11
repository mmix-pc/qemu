#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

ISA_TESTS = [
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
        "unsupported-high-segment-runtime-trap",
        program_with_handler(
            [
                wyde(SETL, R1, 0x80),  # handler address
                insn(PUT, SR_TT, 0, R1),
                *set_octa(R2, RQ_PROGRAM_K),
                insn(PUT, SR_K, 0, R2),
                *set_octa(R3, 0x6000000000000000),
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
        regs={R4: 0, R5: 0, R40: RQ_PROGRAM_R, R41: RQ_PROGRAM_R, R42: 0x30, R43: 0},
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
        regs={R40: RQ_PROGRAM_W, R41: RQ_PROGRAM_W, R42: 0x40, R43: 0},
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
        regs={R6: 0, R7: 0, R40: RQ_PROGRAM_R, R41: RQ_PROGRAM_R, R42: 0x48},
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
        regs={R5: 0, R40: RQ_PROGRAM_X, R41: RQ_PROGRAM_X, R42: 0x44},
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
        regs={R7: 0, R8: 0, R40: RQ_PROGRAM_R, R41: RQ_PROGRAM_R, R42: 0x68},
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
        regs={R3: 0, R40: RQ_PROGRAM_X, R41: RQ_PROGRAM_X, R42: 0x2c},
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
        regs={R4: 0, R5: 0, R40: RQ_PROGRAM_N, R41: RQ_PROGRAM_N, R42: 0x30, R43: 0},
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
        regs={R5: 0, R40: RQ_PROGRAM_N, R41: RQ_PROGRAM_N, R42: 0x34, R43: 0},
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
            R42: RQ_PROGRAM_K,
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
            R41: RQ_PROGRAM_K,
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
            R41: RQ_PROGRAM_K,
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
            R41: RQ_PROGRAM_K,
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
            R16: 0x10,
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
            R12: 0x10,
            R13: f64(float("inf")),
            R14: 0x19,
            R15: 0,
            R16: 0x1d,
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
