#!/usr/bin/env python3
#
# MMIX high-address execution test cases
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *
from .isa import ISA_TESTS


def privileged_negative_alias_program():
    first = 0x1122334455667788
    second = 0x8877665544332211
    physical_data = 0x300
    physical_target = 0x380
    alias_data = 0x8000000000000000 | physical_data
    alias_target = 0x8000000000000000 | physical_target

    return MMIXTest(
        "negative-address-privileged-direct",
        program_with_handler(
            [
                *set_octa(R1, physical_data),
                *set_octa(R2, first),
                insn(STOU, R2, R1, R0),
                *set_octa(R3, alias_data),
                insn(LDOU, R4, R3, R0),
                *set_octa(R5, second),
                insn(STOU, R5, R3, R0),
                insn(LDOU, R6, R1, R0),
                *set_octa(R7, alias_target),
                insn(GO, R8, R7, R0),
                wyde(SETL, R9, 0x00ff),
            ],
            physical_target,
            [
                wyde(SETL, R9, 0x0055),
                halt(),
            ],
        ),
        pc=alias_target + 4,
        regs={R4: first, R6: second, R8: 0x64, R9: 0x55},
    )


_USER_ACCESS_NAMES = {
    "negative-address-load-user-trap",
    "negative-address-store-user-trap",
}

HIGH_ADDRESS_TESTS = tuple(
    test for test in ISA_TESTS if test.name in _USER_ACCESS_NAMES
) + (privileged_negative_alias_program(),)
