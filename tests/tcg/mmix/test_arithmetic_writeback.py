#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import *
from lib.execution import run_one_with_loader


ARITHMETIC_CASES = [
    ("divide-zero", DIV, 17, 0, 0, RA_EVENT_D),
    ("divide-overflow", DIV, 1 << 63, MASK64, 1 << 63, RA_EVENT_V),
    ("add", ADD, (1 << 63) - 1, 1, 1 << 63, RA_EVENT_V),
    ("subtract", SUB, 1 << 63, 1, (1 << 63) - 1, RA_EVENT_V),
    ("multiply", MUL, (1 << 63) - 1, 2, MASK64 - 1, RA_EVENT_V),
    ("shift", SL, 1 << 62, 1, 1 << 63, RA_EVENT_V),
    ("float-divide", FDIV, f64(1.0), 0, f64(float("inf")), RA_EVENT_Z),
    ("float-nan", FADD, 0x7ff0000000000001, f64(1.0),
     0x7ff8000000000001, RA_EVENT_I),
    ("float-compare", FCMP, 0x7ff0000000000001, f64(1.0), 0, RA_EVENT_I),
    ("float-fix", FIX, 4, f64(1.5), 2, RA_EVENT_X),
    ("float-round", FINT, 4, f64(1.5), f64(2.0), RA_EVENT_X),
    ("float-convert", FLOT, 4, (1 << 53) + 1, f64(1 << 53), RA_EVENT_X),
]


@pytest.mark.parametrize("case", ARITHMETIC_CASES, ids=lambda case: case[0])
@pytest.mark.parametrize("mode", ("ordinary", "insert", "substitute"))
def test_arithmetic_result_before_trip(qemu, workdir, case, mode):
    name, opcode, y, z, expected, event = case
    vector = (8 - (event.bit_length() - 1)) * 16
    main = 0x200
    unary = opcode in (FIX, FINT, FLOT)
    operation = insn(opcode, R240, y if unary else R241, R242)
    setup = [
        wyde(SETL, R240, 41),
        *set_octa(R241, y),
        *set_octa(R242, z),
        wyde(SETL, R200, event << RA_ENABLE_SHIFT),
        insn(PUT, SR_A, 0, R200),
        *set_octa(R200, (opcode << 56) | (0xff << 48)),
        insn(PUT, SR_U, 0, R200),
    ]
    if mode == "ordinary":
        setup.append(operation)
        continuation = main + len(setup) * 4
    else:
        continuation = 0x804
        setup.extend([
            *set_octa(R200, continuation),
            insn(PUT, SR_W, 0, R200),
            *set_octa(R200, int.from_bytes(operation, "big") |
                      ((1 << 56) if mode == "substitute" else 0)),
            insn(PUT, SR_X, 0, R200),
            insn(PUT, SR_Y, 0, R241),
            insn(PUT, SR_Z, 0, R242),
            insn(RESUME, 0, 0, 0),
        ])
    handler = [
        insn(GET, R220, 0, SR_R),
        insn(GET, R221, 0, SR_W),
        insn(GET, R222, 0, SR_X),
        insn(GET, R223, 0, SR_Y),
        insn(GET, R224, 0, SR_Z),
        insn(GET, R225, 0, SR_A),
        insn(GET, R226, 0, SR_U),
        halt(),
    ]
    regs = {
        R240: expected,
        R221: continuation,
        R222: (1 << 63) | int.from_bytes(operation, "big"),
        R223: y,
        R224: z,
        R225: event << RA_ENABLE_SHIFT,
        R226: (opcode << 56) | (0xff << 48) | 1,
    }
    if opcode == DIV:
        regs[R220] = y if z == 0 else 0
    run_one_with_loader(qemu, workdir, MMIXTest(
        f"arithmetic-writeback-{name}-{mode}",
        program_with_regions((0, [jump(JMP, main // 4)]),
                             (vector, handler), (main, setup)),
        pc=vector + (len(handler) - 1) * 4, regs=regs,
    ))


@pytest.mark.parametrize("destination", (R31, R255))
def test_arithmetic_destination_before_trip(qemu, workdir, destination):
    main = 0x100
    setup = [
        *set_octa(R240, (1 << 63) - 1),
        wyde(SETL, R241, 1),
        wyde(SETL, R242, RA_EVENT_V << RA_ENABLE_SHIFT),
        insn(PUT, SR_A, 0, R242),
        insn(ADD, destination, R240, R241),
    ]
    handler = [
        insn(GET, R220, 0, SR_B),
        insn(GET, R221, 0, SR_L),
        halt(),
    ]
    regs = ({R31: 1 << 63, R221: 32} if destination == R31 else
            {R220: 1 << 63, R255: 0})
    run_one_with_loader(qemu, workdir, MMIXTest(
        f"arithmetic-writeback-destination-{destination}",
        program_with_regions((0, [jump(JMP, main // 4)]),
                             (32, handler), (main, setup)),
        pc=40, regs=regs,
    ))


@pytest.mark.parametrize("insert", (False, True))
@pytest.mark.parametrize("retry", (False, True))
def test_stsf_writeback_before_trip(qemu, workdir, insert, retry):
    main = 0x200
    address = 0x2000
    operation = insn(STSFI, R240, R241, 0)
    setup = [
        *set_octa(R240, 0x7ff0000000000001),
        wyde(SETL, R241, address),
        wyde(SETL, R242, RA_EVENT_I << RA_ENABLE_SHIFT),
        insn(PUT, SR_A, 0, R242),
    ]
    if retry:
        setup.extend([
            wyde(SETL, R230, VM_PAGE_TABLE),
            wyde(SETL, R231, 7),
            insn(STOUI, R231, R230, 0),
            wyde(SETL, R231, 0x6004),
            insn(STOUI, R231, R230, 8),
            *set_octa(R230, (1 << 63) | 0x1000),
            insn(PUT, SR_TT, 0, R230),
            *set_octa(R230, RQ_PROGRAM_W),
            insn(PUT, SR_K, 0, R230),
            *set_octa(R230, VM_RV_PAGE0),
            insn(PUT, SR_V, 0, R230),
        ])
    if insert:
        setup.extend([
            wyde(SETL, R242, 0x804),
            insn(PUT, SR_W, 0, R242),
            *set_octa(R242, int.from_bytes(operation, "big")),
            insn(PUT, SR_X, 0, R242),
            insn(RESUME, 0, 0, 0),
        ])
    else:
        setup.append(operation)
    handler = [
        insn(LDTUI, R220, R241, 0),
        insn(GET, R221, 0, SR_Y),
        insn(GET, R222, 0, SR_Z),
        halt(),
    ]
    fault = [
        insn(ADDUI, R210, R210, 1),
        insn(GET, R211, 0, SR_A),
        insn(ADD, R212, R241, R241),
        *set_octa(R230, (1 << 63) | (VM_PAGE_TABLE + 8)),
        wyde(SETL, R231, 0x6006),
        insn(STOUI, R231, R230, 0),
        insn(LDVTSI, R232, R241, 0),
        insn(GET, R232, 0, SR_Q),
        insn(PUTI, SR_Q, 0, 0),
        *set_octa(R255, RQ_PROGRAM_W),
        insn(RESUME, 0, 0, 1),
    ]
    handler.insert(-1, wyde(SETL, R255, 0))
    run_one_with_loader(qemu, workdir, MMIXTest(
        f"arithmetic-writeback-stsf-{insert}-{retry}",
        program_with_regions((0, [jump(JMP, main // 4)]),
                             (64, handler), (main, setup), (0x1000, fault)),
        pc=80, regs={R220: 0x7fc00000, R221: address,
                     R222: 0x7fc0000000000000, R210: int(retry),
                     R211: (RA_EVENT_I << RA_ENABLE_SHIFT) if retry else 0},
    ))


def test_arithmetic_local_spill_retry(qemu, workdir):
    main = 0x200
    fault_handler = 0x800
    setup = [
        *set_octa(R230, VM_PAGE_TABLE),
        wyde(SETL, R231, 7),
        insn(STOUI, R231, R230, 0),
        *set_octa(R230, VM_STACK_PTP_ADDRESS),
        *set_octa(R231, VM_STACK_PTP),
        insn(STOUI, R231, R230, 0),
        *set_octa(R230, VM_STACK_PTE),
        *set_octa(R231, INITIAL_STACK | 5),
        insn(STOUI, R231, R230, 0),
        *set_octa(R230, (1 << 63) | fault_handler),
        insn(PUT, SR_TT, 0, R230),
        *set_octa(R230, RQ_PROGRAM_W),
        insn(PUT, SR_K, 0, R230),
        *set_octa(R230, VM_RV_STACK),
        insn(PUT, SR_V, 0, R230),
        wyde(SETL, R241, 17),
        wyde(SETL, R242, 0),
        wyde(SETL, R230, RA_EVENT_D << RA_ENABLE_SHIFT),
        insn(PUT, SR_A, 0, R230),
        *[branch(PUSHJ, R31, 1) for _ in range(7)],
        insn(DIV, R31, R241, R242),
    ]
    handler = [
        insn(ADDUI, R210, R210, 1),
        insn(GET, R211, 0, SR_A),
        # Exercise another arithmetic helper while the writeback is suspended.
        insn(ADD, R212, R241, R241),
        *set_octa(R230, (1 << 63) | VM_STACK_PTE),
        *set_octa(R231, INITIAL_STACK | 7),
        insn(STOUI, R231, R230, 0),
        *set_octa(R230, INITIAL_STACK),
        insn(LDVTSI, R232, R230, 0),
        insn(GET, R232, 0, SR_Q),
        insn(PUTI, SR_Q, 0, 0),
        *set_octa(R255, RQ_PROGRAM_W),
        insn(RESUME, 0, 0, 1),
    ]
    trip = [
        insn(GET, R220, 0, SR_R),
        insn(GET, R221, 0, SR_L),
        insn(GET, R222, 0, SR_W),
        wyde(SETL, R255, 0),
        halt(),
    ]
    run_one_with_loader(qemu, workdir, MMIXTest(
        "arithmetic-writeback-local-spill-retry",
        program_with_regions((0, [jump(JMP, main // 4)]),
                             (16, trip), (main, setup),
                             (fault_handler, handler)),
        pc=32, regs={R31: 0, R210: 1, R211: RA_EVENT_D << RA_ENABLE_SHIFT,
                     R212: 34, R220: 17, R221: 32,
                     R222: main + len(setup) * 4},
    ))
