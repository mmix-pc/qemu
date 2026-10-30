#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct

import pytest

from cases.common import *
from lib.execution import run_elf_test, run_one_with_loader


def cswap_spill_case(immediate, equal, rebase, inserted, prefix):
    negative = 1 << 63
    source = 0x4003
    stack_page = 0x12000
    base = stack_page - (24 if prefix is None else prefix) * 8
    initial_l = 230
    top = base + (initial_l + 1 + 26 + 12) * 8
    prediction = 41 if equal else 42
    opcode = insn(CSWAPI if immediate else CSWAP, R229, R241,
                  0 if immediate else R242)
    context = [struct.pack(">Q", 0x3000 + i) for i in range(initial_l)]
    context += [struct.pack(">Q", initial_l)] + [bytes(8)] * (26 + 12)
    context += [struct.pack(">Q", 230 << 56)]
    setup = [
        *set_octa(R250, top), insn(UNSAVE, 0, 0, R250),
        wyde(SETL, R229, 0),
        *set_octa(R250, negative | 0x2000), insn(PUT, SR_T, 0, R250),
        *set_octa(R250, negative | 0x2800), insn(PUT, SR_TT, 0, R250),
        *set_octa(R250, VM_RV_SOFTWARE), insn(PUT, SR_V, 0, R250),
        # Preload only the stack page holding the completed spill prefix.
        *set_octa(R250, base), insn(LDOUI, R243, R250, 0),
        *set_octa(R241, 0x8000), wyde(SETL, R242, 0),
        insn(LDOUI, R243, R241, 0), insn(PUTI, SR_P, 0, prediction),
        *set_octa(R250, negative | 0x800),
        insn(PUSHGOI if prefix is not None else GOI,
             R229 if prefix is not None else R250, R250, 0),
    ]
    enter = []
    if inserted:
        enter += [*set_octa(R250, source + 4), insn(PUT, SR_W, 0, R250),
                  *set_octa(R250, int.from_bytes(opcode, "big")),
                  insn(PUT, SR_X, 0, R250)]
    enter += [*set_octa(R250, 0x4200 if inserted else source),
              insn(PUT, SR_WW, 0, R250),
              *set_octa(R250, negative), insn(PUT, SR_XX, 0, R250),
              insn(PUTI, SR_BB, 0, 0), *set_octa(R255, RQ_PROGRAM_MASK),
              insn(RESUME, 0, 0, 1)]

    trap = [insn(GET, R250, 0, SR_YY),
            *set_octa(R251, stack_page), insn(CMPU, R251, R250, R251)]
    branch_pc = 0x2000 + len(trap) * 4
    trap += [branch(BNZ, R251, (0x2600 - branch_pc) // 4)]
    trap += [
        insn(GET, R230, 0, SR_WW), insn(GET, R231, 0, SR_XX),
        insn(GET, R232, 0, SR_YY), insn(GET, R233, 0, SR_O),
        insn(GET, R234, 0, SR_S), insn(GET, R235, 0, SR_L),
        insn(GET, R236, 0, SR_P),
        *set_octa(R250, negative | 0x8000), insn(LDOUI, R237, R250, 0),
        insn(ADDUI, R238, R238, 1),
    ]
    if rebase:
        trap += [*set_octa(R250, 0x11110d000000e000),
                 insn(PUT, SR_V, 0, R250), insn(SAVE, R249, 0, 0),
                 insn(UNSAVE, 0, 0, R249),
                 *set_octa(R250, VM_RV_SOFTWARE),
                 insn(PUT, SR_V, 0, R250)]
    # A retry must use rXX, not refetch the replaced instruction.
    trap += [*set_octa(R250, negative | (source & ~3)),
             *set_octa(R251, int.from_bytes(halt(), "big")),
             insn(STTI, R251, R250, 0),
             insn(PUT, SR_WW, 0, R230), insn(PUT, SR_XX, 0, R231),
             insn(PUT, SR_YY, 0, R232),
             *set_octa(R250, stack_page | 6), insn(PUT, SR_ZZ, 0, R250),
             *set_octa(R255, RQ_PROGRAM_MASK), insn(RESUME, 0, 0, 1)]

    refill = [insn(GET, R250, 0, SR_YY),
              *set_octa(R251, 0x1fff), insn(ANDN, R250, R250, R251),
              insn(ORI, R250, R250, 7), insn(PUT, SR_ZZ, 0, R250),
              insn(GET, R252, 0, SR_WW)]
    branch_pc = 0x2600 + len(refill) * 4
    refill += [branch(BNN, R252, (0x2700 - branch_pc) // 4),
               wyde(SETL, R255, 0), insn(RESUME, 0, 0, 1)]
    finish = [insn(GET, R244, 0, SR_P), insn(LDOUI, R245, R241, 0),
              insn(GET, R246, 0, SR_L), insn(GET, R247, 0, SR_O),
              wyde(SETL, R255, 0), halt()]
    payload = program_with_regions(
        (0x100, setup), (0x800, enter), (0x2000, trap), (0x2600, refill),
        (0x2700, [*set_octa(R255, RQ_PROGRAM_MASK), insn(RESUME, 0, 0, 1)]),
        (0x2800, [wyde(SETL, R255, 0), halt()]),
        (source & ~3, [opcode, *finish]),
        (0x4200, [insn(RESUME, 0, 0, 0)]),
        (0x8000, [struct.pack(">Q", 41)]),
        (0xe040, [struct.pack(">Q", 0x10007), struct.pack(">Q", 0x12007)]),
        (base, context),
    )
    regs = {R229: int(equal), R244: 41, R245: 0 if equal else 41,
            R246: 230, R247: base + (230 * 8 if prefix is not None else 0),
            R238: int(prefix is not None)}
    if prefix is not None:
        regs.update({R230: source + 4,
                     R231: (3 << 56) | int.from_bytes(opcode, "big"),
                     R232: stack_page, R233: base + 230 * 8,
                     R234: stack_page, R235: 0, R236: prediction, R237: 41})
        regs.update({i: 0 for i in range(229)})
    return MMIXELFTest(
        f"cswap-spill-{immediate}-{equal}-{rebase}-{inserted}-{prefix}",
        elf64_image(0, payload, entry=negative | 0x100,
                    virtual_address=negative),
        pc=source + len(finish) * 4, regs=regs,
        qemu_args=("-machine", "elf-startup=platform"),
    )


@pytest.mark.parametrize("immediate", (False, True),
                         ids=("register", "immediate"))
@pytest.mark.parametrize("equal", (False, True), ids=("mismatch", "match"))
@pytest.mark.parametrize("rebase", (False, True), ids=("direct", "rebuilt"))
@pytest.mark.parametrize("inserted", (False, True),
                         ids=("fetched", "inserted"))
@pytest.mark.parametrize("prefix", (24, 28))
def test_cswap_before_result_spill(qemu, workdir, immediate, equal, rebase,
                                  inserted, prefix):
    run_elf_test(qemu, workdir, cswap_spill_case(
        immediate, equal, rebase, inserted, prefix))


@pytest.mark.parametrize("immediate", (False, True),
                         ids=("register", "immediate"))
@pytest.mark.parametrize("equal", (False, True), ids=("mismatch", "match"))
@pytest.mark.parametrize("inserted", (False, True),
                         ids=("fetched", "inserted"))
def test_cswap_resident_destination(qemu, workdir, immediate, equal, inserted):
    run_elf_test(qemu, workdir, cswap_spill_case(
        immediate, equal, False, inserted, None))


@pytest.mark.parametrize("x", (R10, R240), ids=("local", "global"))
@pytest.mark.parametrize("alias", ("y", "z", "immediate"))
@pytest.mark.parametrize("equal", (False, True), ids=("mismatch", "match"))
def test_cswap_aliased_destination(qemu, workdir, x, alias, equal):
    operation = (insn(CSWAPI, x, x, 0) if alias == "immediate" else
                 insn(CSWAP, x, x, R0) if alias == "y" else
                 insn(CSWAP, x, R0, x))
    program = [*set_octa(x, 0x8000), insn(PUTI, SR_P, 0, 41 if equal else 42),
               operation, *set_octa(R241, 0x8000), insn(LDOUI, R242, R241, 0),
               insn(GET, R243, 0, SR_P), halt()]
    run_one_with_loader(qemu, workdir, MMIXTest(
        f"cswap-alias-{x}-{alias}-{equal}",
        program_with_regions((0, program), (0x8000, [struct.pack(">Q", 41)])),
        pc=(len(program) - 1) * 4,
        regs={x: int(equal), R242: 0x8000 if equal else 41, R243: 41},
    ))
