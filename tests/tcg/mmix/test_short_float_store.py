#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import *
from lib.execution import run_one_with_loader, run_semihosting_one


INITIAL = 0x1122334455667788
CASES = [
    ("exact", f64(1.0), 0x3f800000, 0),
    ("snan", 0x7ff0000000000001, 0x7fc00000, RA_EVENT_I),
    ("qnan", 0x7ff8000000000000, 0x7fc00000, 0),
    ("overflow", f64(2.0 ** 128), 0x7f800000, RA_EVENT_O | RA_EVENT_X),
    ("underflow", f64(2.0 ** -150), 0, RA_EVENT_U | RA_EVENT_X),
    ("inexact", f64(1.0 + 2.0 ** -24), 0x3f800000, RA_EVENT_X),
]


def short_float_store_case(case, immediate, offset, enabled, insert,
                           *, memory="ram", negative=False, rounding=0):
    name, source, short, events = case
    event = 1 << (events.bit_length() - 1) if events else 0
    enables = event if enabled else 0
    initial_ra = (rounding << 16) | (enables << RA_ENABLE_SHIFT)
    trip = bool(enables) and not negative and memory != "masked"
    vector = (8 - (event.bit_length() - 1)) * 16 if trip else None
    main = 0x200
    high = (1 << 63) if negative else 0
    address = MMIX_DATA_SEGMENT_BASE if memory == "hosted" else 0x2000
    mapped = memory in ("write-only", "retry", "masked")
    physical = 0x6000 if mapped else address
    opcode = STSFI if immediate else STSF
    operation = insn(opcode, R240, R241, offset if immediate else R242)
    setup = [
        *set_octa(R230, physical),
        *set_octa(R231, INITIAL),
        insn(STOUI, R231, R230, 0),
        *set_octa(R245, ((1 << 63) | physical) if mapped else address),
    ]
    if mapped:
        setup.extend([
            wyde(SETL, R230, VM_PAGE_TABLE),
            wyde(SETL, R231, 7),
            insn(STOUI, R231, R230, 0),
            wyde(SETL, R231,
                 0x6004 if memory in ("retry", "masked") else 0x6002),
            insn(STOUI, R231, R230, 8),
            *set_octa(R230, (1 << 63) | 0x1000),
            insn(PUT, SR_TT, 0, R230),
            *set_octa(R230, RQ_PROGRAM_R |
                      (0 if memory == "masked" else RQ_PROGRAM_W)),
            insn(PUT, SR_K, 0, R230),
            *set_octa(R230, VM_RV_PAGE0),
            insn(PUT, SR_V, 0, R230),
        ])
    setup.extend([
        *set_octa(R240, source),
        *set_octa(R241, address),
        wyde(SETL, R242, offset),
        *set_octa(R243, initial_ra),
        insn(PUT, SR_A, 0, R243),
        wyde(SETL, R230, 0x1234),
        insn(PUT, SR_Y, 0, R230),
        insn(PUT, SR_Z, 0, R230),
    ])
    if insert:
        continuation = high | 0x804
        setup.extend([
            *set_octa(R230, continuation),
            insn(PUT, SR_W, 0, R230),
            *set_octa(R230, int.from_bytes(operation, "big")),
            insn(PUT, SR_X, 0, R230),
            insn(RESUME, 0, 0, 0),
        ])
    else:
        setup.append(operation)
        continuation = high | (main + len(setup) * 4)
    result = [
        insn(LDOUI, R220, R245, 0),
        insn(GET, R221, 0, SR_A),
        insn(GET, R222, 0, SR_W),
        insn(GET, R223, 0, SR_X),
        insn(GET, R224, 0, SR_Y),
        insn(GET, R225, 0, SR_Z),
        insn(GET, R226, 0, SR_Q),
        wyde(SETL, R255, 0),
        halt(),
    ]
    fault = [
        insn(ADDUI, R210, R210, 1),
        insn(GET, R211, 0, SR_A),
        # The failed store must not have changed any bytes or published rA.
        insn(LDOUI, R212, R245, 0),
        *set_octa(R230, (1 << 63) | (VM_PAGE_TABLE + 8)),
        wyde(SETL, R231, 0x6002),
        insn(STOUI, R231, R230, 0),
        insn(LDVTSI, R232, R241, 0),
        insn(GET, R232, 0, SR_Q),
        insn(PUTI, SR_Q, 0, 0),
        *set_octa(R255, RQ_PROGRAM_W | RQ_PROGRAM_R),
        insn(RESUME, 0, 0, 1),
    ]
    entry = ([*set_octa(R230, high | main), insn(GO, R231, R230, R0)]
             if negative else [jump(JMP, main // 4)])
    result_pc = vector if trip else continuation
    regions = [(0, entry), (main, setup),
               (result_pc & ~(1 << 63), result)]
    if memory == "retry":
        regions.append((0x1000, fault))
    merged = ((short << 32) | (INITIAL & 0xffffffff) if offset < 4 else
              (INITIAL & 0xffffffff00000000) | short)
    regs = {R220: merged,
            R221: initial_ra | (events & ~enables if trip else events),
            R224: address + offset if trip else 0x1234,
            R225: merged if trip else 0x1234,
            R210: int(memory == "retry")}
    if trip:
        regs.update({R222: continuation,
                     R223: (1 << 63) | int.from_bytes(operation, "big")})
    if memory == "retry":
        regs.update({R211: initial_ra, R212: INITIAL})
    if memory == "masked":
        regs.update({R220: INITIAL, R221: initial_ra, R226: RQ_PROGRAM_W})
    return MMIXTest(
        f"short-float-store-{name}-{immediate}-{offset}-{enabled}-{insert}"
        f"-{memory}-{negative}-{rounding}",
        program_with_regions(*sorted(regions)),
        pc=result_pc + (len(result) - 1) * 4, regs=regs,
    )


@pytest.mark.parametrize("case", CASES, ids=lambda case: case[0])
@pytest.mark.parametrize("immediate", (False, True))
@pytest.mark.parametrize("offset", (0, 3, 4, 7))
@pytest.mark.parametrize("enabled", (False, True))
@pytest.mark.parametrize("insert", (False, True))
def test_short_float_store(qemu, workdir, case, immediate, offset,
                           enabled, insert):
    run_one_with_loader(qemu, workdir, short_float_store_case(
        case, immediate, offset, enabled, insert,
    ))


@pytest.mark.parametrize("case", [case for case in CASES if case[3]],
                         ids=lambda case: case[0])
@pytest.mark.parametrize("immediate", (False, True))
@pytest.mark.parametrize("memory", ("write-only", "retry"))
@pytest.mark.parametrize("insert", (False, True))
def test_short_float_store_write_only(qemu, workdir, case, immediate,
                                      memory, insert):
    run_one_with_loader(qemu, workdir, short_float_store_case(
        case, immediate, 7, True, insert, memory=memory,
    ))


@pytest.mark.parametrize("case", CASES, ids=lambda case: case[0])
@pytest.mark.parametrize("immediate", (False, True))
def test_short_float_store_hosted(qemu, workdir, case, immediate):
    run_semihosting_one(qemu, workdir, short_float_store_case(
        case, immediate, 3, True, False, memory="hosted",
    ))


@pytest.mark.parametrize("case", CASES, ids=lambda case: case[0])
@pytest.mark.parametrize("immediate", (False, True))
def test_short_float_store_negative_pc(qemu, workdir, case, immediate):
    run_one_with_loader(qemu, workdir, short_float_store_case(
        case, immediate, 3, True, False, negative=True,
    ))


@pytest.mark.parametrize("rounding", (0, 1, 2, 3))
@pytest.mark.parametrize("immediate", (False, True))
def test_short_float_store_rounding(qemu, workdir, rounding, immediate):
    case = ("rounded", f64(1.0 + 2.0 ** -24),
            0x3f800001 if rounding == 2 else 0x3f800000, RA_EVENT_X)
    run_one_with_loader(qemu, workdir, short_float_store_case(
        case, immediate, 3, True, False, rounding=rounding,
    ))


@pytest.mark.parametrize("immediate", (False, True))
@pytest.mark.parametrize("insert", (False, True))
def test_short_float_store_masked_fault(qemu, workdir, immediate, insert):
    run_one_with_loader(qemu, workdir, short_float_store_case(
        CASES[1], immediate, 3, True, insert, memory="masked",
    ))


@pytest.mark.parametrize("immediate", (False, True))
def test_short_float_store_mmio(qemu, workdir, immediate):
    main = 0x200
    address = (1 << 63) | MMIX_VIRT_UART0_BASE
    setup = [
        *set_octa(R241, address),
        # Populate the UART receive FIFO through loopback before the STSF.
        wyde(SETL, R243, 1),
        insn(STBUI, R243, R241, 2),
        wyde(SETL, R243, 0x10),
        insn(STBUI, R243, R241, 4),
        wyde(SETL, R243, ord("X")),
        insn(STBUI, R243, R241, 0),
        insn(LDBUI, R243, R241, MMIX_VIRT_UART0_LSR),
        insn(ANDI, R243, R243, 1),
        branch(BZB, R243, 0xfffe),
        *set_octa(R240, 0x7ff0000000000001),
        wyde(SETL, R242, 7),
        wyde(SETL, R243, RA_EVENT_I << RA_ENABLE_SHIFT),
        insn(PUT, SR_A, 0, R243),
        # The upper UART registers have no receive-data read side effects.
        insn(STSFI if immediate else STSF, R240, R241,
             7 if immediate else R242),
    ]
    trip = [
        insn(GET, R221, 0, SR_Y),
        insn(GET, R222, 0, SR_Z),
        insn(LDBUI, R220, R241, 0),
        wyde(SETL, R255, 0),
        halt(),
    ]
    run_one_with_loader(qemu, workdir, MMIXTest(
        f"short-float-store-mmio-{immediate}",
        program_with_regions((0, [jump(JMP, main // 4)]),
                             (64, trip), (main, setup)),
        pc=64 + (len(trip) - 1) * 4,
        regs={R220: ord("X"), R221: address + 7, R222: 0x7fc00000},
    ))
