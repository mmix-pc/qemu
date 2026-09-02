#!/usr/bin/env python3
#
# MMIX debugger fixture
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct

from .common import *


DEBUGGER_ENTRY = 0x1000
DEBUGGER_DATA_ADDRESS = 0x1100
DEBUGGER_STACK_ADDRESS = INITIAL_STACK + 0x100
DEBUGGER_DATA_VALUE = 0x1122334455667788
DEBUGGER_WINDOW_CALL = DEBUGGER_ENTRY + 5 * 4
DEBUGGER_WINDOW_RETURN = DEBUGGER_ENTRY + 6 * 4
DEBUGGER_WINDOW_BODY = DEBUGGER_ENTRY + 7 * 4
DEBUGGER_DIRECT_JUMP = DEBUGGER_ENTRY + 6 * 4
DEBUGGER_NOT_TAKEN_BRANCH = DEBUGGER_ENTRY + 19 * 4
DEBUGGER_TAKEN_BRANCH = DEBUGGER_ENTRY + 21 * 4
DEBUGGER_LOOP = DEBUGGER_ENTRY + 23 * 4


def _debugger_elf_image():
    program = b"".join([
        insn(ADDI, R39, R0, 0),
        insn(ADDI, R40, R2, 0),
        insn(ADDI, R41, R224, 0),
        insn(ADDI, R42, R253, 0),
        insn(ADDI, R43, R254, 0),
        branch(PUSHJ, R8, 2),
        jump(JMP, 3),
        insn(ADDI, R44, R0, 0),
        insn(POP, 0, 0, 0),
        *set_octa(R254, DEBUGGER_STACK_ADDRESS),
        *set_octa(R32, DEBUGGER_DATA_ADDRESS),
        insn(LDOU, R33, R32, R0),
        insn(ADDI, R34, R33, 1),
        branch(BZ, R34, 2),
        insn(ADDI, R35, R35, 1),
        branch(BNZ, R34, 2),
        insn(ADDI, R35, R35, 1),
        jump(JMP, 0),
    ])
    data_offset = DEBUGGER_DATA_ADDRESS - DEBUGGER_ENTRY
    segment = (
        program + bytes(data_offset - len(program)) +
        struct.pack(">Q", DEBUGGER_DATA_VALUE)
    )
    return elf64_image(
        DEBUGGER_ENTRY,
        segment,
        entry=DEBUGGER_ENTRY,
        offset=0x1000,
    )


DEBUGGER_ELF_IMAGE = _debugger_elf_image()
