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


def privileged_device_alias_program():
    negative = 1 << 63
    uart = negative | MMIX_VIRT_MEMMAP[MMIX_VIRT_UART0][0]
    framebuffer = (
        negative | MMIX_VIRT_MEMMAP[MMIX_VIRT_FRAMEBUFFER_CONTROL][0]
    )
    timer = negative | MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0]
    ipi = negative | MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0]
    intc = negative | MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0]
    virtio = negative | MMIX_VIRT_MEMMAP[MMIX_VIRT_VIRTIO_MMIO][0]

    program = [
        *set_octa(R1, uart),
        insn(LDBUI, R20, R1, MMIX_VIRT_UART0_LSR),
        insn(ANDI, R20, R20, MMIX_VIRT_UART0_LSR_THRE),
        *set_octa(R2, framebuffer),
        insn(LDOUI, R21, R2, MMIX_VIRT_FRAMEBUFFER_REG_WIDTH),
        *set_octa(R3, timer),
        insn(LDOUI, R22, R3, MMIX_VIRT_TIMER_TIME),
        *set_octa(R4, ipi),
        insn(LDOUI, R23, R4, MMIX_VIRT_IPI_ACTIVE_TARGETS),
        *set_octa(R5, intc),
        insn(LDOUI, R24, R5, 0),
        *set_octa(R6, virtio),
        insn(LDTUI, R25, R6, 0),
        halt(),
    ]
    return MMIXTest(
        "negative-address-privileged-devices",
        b"".join(program),
        pc=(len(program) - 1) * 4,
        regs={
            R20: MMIX_VIRT_UART0_LSR_THRE,
            R21: MMIX_VIRT_FRAMEBUFFER_WIDTH,
            R23: 1,
            R24: MMIX_VIRT_INTC_IRQ_COUNT,
            R25: 0x74726976,
        },
    )


def unprivileged_device_alias_program(name, operation, address):
    prefix = [
        wyde(SETL, R1, 0x80),
        insn(PUT, SR_TT, 0, R1),
        *set_octa(R2, RQ_PROGRAM_K),
        insn(PUT, SR_K, 0, R2),
        *set_octa(R3, (1 << 63) | address),
    ]
    fault_pc = len(b"".join(prefix))
    program = [
        *prefix,
        operation,
        wyde(SETL, R5, 0x00ff),
    ]
    return MMIXTest(
        name,
        program_with_handler(
            program,
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
            R41: RQ_PROGRAM_N | int.from_bytes(operation, "big"),
            R42: fault_pc + 4,
            R43: 0,
        },
    )
_USER_ACCESS_NAMES = {
    "negative-address-load-user-trap",
    "negative-address-store-user-trap",
}

HIGH_ADDRESS_TESTS = tuple(
    test for test in ISA_TESTS if test.name in _USER_ACCESS_NAMES
) + (
    privileged_negative_alias_program(),
    privileged_device_alias_program(),
    unprivileged_device_alias_program(
        "negative-address-uart-user-load-trap",
        insn(LDBU, R4, R3, R0),
        MMIX_VIRT_MEMMAP[MMIX_VIRT_UART0][0],
    ),
    unprivileged_device_alias_program(
        "negative-address-framebuffer-user-store-trap",
        insn(STOU, R4, R3, R0),
        MMIX_VIRT_MEMMAP[MMIX_VIRT_FRAMEBUFFER_CONTROL][0],
    ),
)
