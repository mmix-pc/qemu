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


def _debugger_elf_image():
    program = b"".join([
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
