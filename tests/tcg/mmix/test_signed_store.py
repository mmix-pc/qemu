#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import *
from lib.execution import run_one_with_loader, run_semihosting_one


STORES = [(STB, 1), (STW, 2), (STT, 4)]
INITIAL = 0x1122334455667788
ADDRESS = 0x2007


def stored_octa(value, size):
    bits = size * 8
    return (INITIAL & ~((1 << bits) - 1)) | (value & ((1 << bits) - 1))


@pytest.mark.parametrize("opcode,size", STORES)
@pytest.mark.parametrize("immediate", (False, True))
@pytest.mark.parametrize("unsigned", (False, True))
@pytest.mark.parametrize("enabled", (False, True))
@pytest.mark.parametrize("value_kind", (
    "minimum", "maximum", "below", "above", "zero", "minus-one",
))
def test_narrow_store(qemu, workdir, opcode, size, immediate, unsigned,
                      enabled, value_kind):
    limit = 1 << (size * 8 - 1)
    value = {"minimum": -limit, "maximum": limit - 1,
             "below": -limit - 1, "above": limit,
             "zero": 0, "minus-one": -1}[value_kind]
    overflow = not unsigned and not -limit <= value < limit
    trip = enabled and overflow
    opcode += int(immediate) + 2 * int(unsigned)
    operation = insn(opcode, R240, R241, 7 if immediate else R242)
    main = 0x200
    setup = [
        *set_octa(R240, INITIAL),
        wyde(SETL, R241, ADDRESS & ~7),
        insn(STOUI, R240, R241, 0),
        *set_octa(R240, value & MASK64),
        wyde(SETL, R242, 7),
        wyde(SETL, R243, (RA_EVENT_V << RA_ENABLE_SHIFT) if enabled else 0),
        insn(PUT, SR_A, 0, R243),
        operation,
    ]
    continuation = main + len(setup) * 4
    result = [
        insn(LDOUI, R220, R241, 0),
        insn(GET, R221, 0, SR_A),
        insn(GET, R222, 0, SR_W),
        insn(GET, R223, 0, SR_X),
        insn(GET, R224, 0, SR_Y),
        insn(GET, R225, 0, SR_Z),
        wyde(SETL, R255, 0),
        halt(),
    ]
    regs = {R220: stored_octa(value, size),
            R221: ((RA_EVENT_V << RA_ENABLE_SHIFT) if enabled else
                   RA_EVENT_V if overflow else 0)}
    if trip:
        regs.update({R222: continuation,
                     R223: (1 << 63) | int.from_bytes(operation, "big"),
                     R224: ADDRESS, R225: stored_octa(value, size)})
    result_address = 32 if trip else continuation
    run_one_with_loader(qemu, workdir, MMIXTest(
        f"narrow-store-{opcode}-{enabled}-{value_kind}",
        program_with_regions(*sorted((
            (0, [jump(JMP, main // 4)]),
            (main, setup), (result_address, result)))),
        pc=result_address + (len(result) - 1) * 4, regs=regs,
    ))


@pytest.mark.parametrize("opcode,size", STORES)
@pytest.mark.parametrize("insert", (False, True))
@pytest.mark.parametrize("retry", (False, True))
def test_signed_store_write_only_page(qemu, workdir, opcode, size,
                                      insert, retry):
    main = 0x200
    value = 1 << (size * 8 - 1)
    operation = insn(opcode, R240, R241, R242)
    setup = [
        wyde(SETL, R230, 0x6000),
        *set_octa(R231, INITIAL),
        insn(STOUI, R231, R230, 0),
        wyde(SETL, R230, VM_PAGE_TABLE),
        wyde(SETL, R231, 7),
        insn(STOUI, R231, R230, 0),
        wyde(SETL, R231, 0x6004 if retry else 0x6002),
        insn(STOUI, R231, R230, 8),
        *set_octa(R230, (1 << 63) | 0x1000),
        insn(PUT, SR_TT, 0, R230),
        *set_octa(R230, RQ_PROGRAM_W | RQ_PROGRAM_R),
        insn(PUT, SR_K, 0, R230),
        *set_octa(R230, VM_RV_PAGE0),
        insn(PUT, SR_V, 0, R230),
        *set_octa(R240, value),
        wyde(SETL, R241, ADDRESS & ~7),
        wyde(SETL, R242, 7),
        wyde(SETL, R243, RA_EVENT_V << RA_ENABLE_SHIFT),
        insn(PUT, SR_A, 0, R243),
    ]
    if insert:
        continuation = 0x804
        setup.extend([
            wyde(SETL, R230, continuation),
            insn(PUT, SR_W, 0, R230),
            *set_octa(R230, int.from_bytes(operation, "big")),
            insn(PUT, SR_X, 0, R230),
            insn(RESUME, 0, 0, 0),
        ])
    else:
        setup.append(operation)
        continuation = main + len(setup) * 4
    fault = [
        insn(ADDUI, R210, R210, 1),
        insn(GET, R211, 0, SR_A),
        *set_octa(R230, (1 << 63) | (VM_PAGE_TABLE + 8)),
        wyde(SETL, R231, 0x6002),
        insn(STOUI, R231, R230, 0),
        insn(LDVTSI, R232, R241, 0),
        insn(GET, R232, 0, SR_Q),
        insn(PUTI, SR_Q, 0, 0),
        *set_octa(R255, RQ_PROGRAM_W | RQ_PROGRAM_R),
        insn(RESUME, 0, 0, 1),
    ]
    trip = [
        insn(GET, R220, 0, SR_Y),
        insn(GET, R221, 0, SR_Z),
        insn(GET, R222, 0, SR_W),
        insn(GET, R223, 0, SR_A),
        wyde(SETL, R255, 0),
        halt(),
    ]
    run_one_with_loader(qemu, workdir, MMIXTest(
        f"signed-store-write-only-{size}-{insert}-{retry}",
        program_with_regions((0, [jump(JMP, main // 4)]),
                             (32, trip), (main, setup), (0x1000, fault)),
        pc=32 + (len(trip) - 1) * 4,
        regs={R210: int(retry),
              R211: (RA_EVENT_V << RA_ENABLE_SHIFT) if retry else 0,
              R220: ADDRESS, R221: stored_octa(value, size),
              R222: continuation, R223: RA_EVENT_V << RA_ENABLE_SHIFT},
    ))


@pytest.mark.parametrize("opcode,size", STORES)
def test_signed_store_hosted_memory(qemu, workdir, opcode, size):
    main = 0x200
    value = 1 << (size * 8 - 1)
    address = MMIX_DATA_SEGMENT_BASE
    setup = [
        *set_octa(R240, INITIAL),
        *set_octa(R241, address),
        insn(STOUI, R240, R241, 0),
        *set_octa(R240, value),
        wyde(SETL, R242, RA_EVENT_V << RA_ENABLE_SHIFT),
        insn(PUT, SR_A, 0, R242),
        insn(opcode + 1, R240, R241, 7),
    ]
    trip = [
        insn(LDOUI, R220, R241, 0),
        insn(GET, R221, 0, SR_Y),
        insn(GET, R222, 0, SR_Z),
        wyde(SETL, R255, 0),
        halt(),
    ]
    run_semihosting_one(qemu, workdir, MMIXTest(
        f"signed-store-hosted-{size}",
        program_with_regions((0, [jump(JMP, main // 4)]),
                             (32, trip), (main, setup)),
        pc=32 + (len(trip) - 1) * 4,
        regs={R220: stored_octa(value, size), R221: address + 7,
              R222: stored_octa(value, size)},
    ))


@pytest.mark.parametrize("opcode,size", STORES + [(STO, 8), (STOU, 8)])
@pytest.mark.parametrize("immediate", (False, True))
def test_store_at_negative_pc(qemu, workdir, opcode, size, immediate):
    main = 0x200
    entry = [
        *set_octa(R230, (1 << 63) | main),
        insn(GO, R231, R230, R0),
    ]
    value = 1 << (size * 8 - 1)
    body = [
        *set_octa(R240, INITIAL),
        wyde(SETL, R241, ADDRESS & ~7),
        insn(STOUI, R240, R241, 0),
        *set_octa(R240, value),
        wyde(SETL, R242, 7),
        wyde(SETL, R243, RA_EVENT_V << RA_ENABLE_SHIFT),
        insn(PUT, SR_A, 0, R243),
        insn(opcode + int(immediate), R240, R241,
             7 if immediate else R242),
        insn(LDOUI, R220, R241, 0),
        insn(GET, R221, 0, SR_A),
        wyde(SETL, R255, 0),
        halt(),
    ]
    run_one_with_loader(qemu, workdir, MMIXTest(
        f"signed-store-negative-pc-{opcode}-{immediate}",
        program_with_regions((0, entry), (main, body)),
        pc=(1 << 63) | (main + (len(body) - 1) * 4),
        regs={R220: stored_octa(value, size),
              R221: (RA_EVENT_V << RA_ENABLE_SHIFT) |
              (RA_EVENT_V if size < 8 else 0)},
    ))


def test_signed_store_mmio_does_not_read(qemu, workdir):
    main = 0x200
    setup = [
        *set_octa(R241, (1 << 63) | MMIX_VIRT_UART0_BASE),
        # FIFO and loopback make receive-side effects deterministic.
        wyde(SETL, R243, 1),
        insn(STBUI, R243, R241, 2),
        wyde(SETL, R243, 0x10),
        insn(STBUI, R243, R241, 4),
        wyde(SETL, R243, ord("X")),
        insn(STBUI, R243, R241, 0),
        insn(LDBUI, R243, R241, MMIX_VIRT_UART0_LSR),
        insn(ANDI, R243, R243, 1),
        branch(BZB, R243, 0xfffe),
        wyde(SETL, R240, 0x141),
        wyde(SETL, R242, RA_EVENT_V << RA_ENABLE_SHIFT),
        insn(PUT, SR_A, 0, R242),
        insn(STBI, R240, R241, 0),
    ]
    trip = [
        insn(GET, R221, 0, SR_Y),
        insn(GET, R222, 0, SR_Z),
        # Reading the surrounding octa would consume the UART receive byte.
        insn(LDBUI, R220, R241, MMIX_VIRT_UART0_LSR),
        insn(ANDI, R220, R220, 1),
        branch(BZB, R220, 0xfffe),
        insn(LDBUI, R220, R241, 0),
        insn(LDBUI, R223, R241, 0),
        wyde(SETL, R255, 0),
        halt(),
    ]
    run_one_with_loader(qemu, workdir, MMIXTest(
        "signed-store-mmio-no-read",
        program_with_regions((0, [jump(JMP, main // 4)]),
                             (32, trip), (main, setup)),
        pc=32 + (len(trip) - 1) * 4,
        regs={R220: ord("X"), R221: (1 << 63) | MMIX_VIRT_UART0_BASE,
              R222: ord("A") << 56, R223: ord("A")},
    ))
