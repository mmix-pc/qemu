#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct

import pytest

from cases.common import *
from lib.execution import run_elf_test


NEGATIVE = 1 << 63
SOURCE = NEGATIVE | 0x4003


def enter_get(opcode, mode):
    if mode == "fetched":
        return [*set_octa(R250, SOURCE), insn(GOI, R250, R250, 0)]
    private = mode == "private"
    return [*set_octa(R250, SOURCE + 4),
            insn(PUT, SR_WW if private else SR_W, 0, R250),
            *set_octa(R250, int.from_bytes(opcode, "big")),
            insn(PUT, SR_XX if private else SR_X, 0, R250),
            wyde(SETL, R255, 0), insn(RESUME, 0, 0, int(private))]


def get_image(name, regions, finish, regs):
    return MMIXELFTest(
        name, elf64_image(0, program_with_regions(*regions),
                          entry=NEGATIVE | 0x100, virtual_address=NEGATIVE),
        pc=SOURCE + len(finish) * 4, regs=regs,
        qemu_args=("-machine", "elf-startup=platform"),
    )


@pytest.mark.parametrize("mode", ("fetched", "public", "private"))
@pytest.mark.parametrize("initial_l", (0, 12, 230))
@pytest.mark.parametrize("x", (R0, R12, R229, R239))
def test_get_rl_destination_growth(qemu, workdir, mode, initial_l, x):
    opcode = insn(GET, x, 0, SR_L)
    setup = [insn(PUTI, SR_G, 0, 230), insn(PUTI, SR_L, 0, 0)]
    if initial_l:
        setup += [wyde(SETL, initial_l - 1, 0x1234)]
    setup += [insn(GET, R240, 0, SR_L), *enter_get(opcode, mode)]
    finish = [insn(GET, R241, 0, SR_L), wyde(SETL, R255, 0), halt()]
    result = max(initial_l, x + 1) if x < 230 else initial_l
    run_elf_test(qemu, workdir, get_image(
        f"get-rl-growth-{mode}-{initial_l}-{x}",
        [(0x100, setup), (0x4000, [opcode, *finish])],
        finish, {x: result, R240: initial_l, R241: result},
    ))


def get_spill_case(sreg, mode, recovery, prefix):
    stack_page = 0x12000
    base = stack_page - prefix * 8
    top = base + (230 + 1 + 26 + 12) * 8
    opcode = insn(GET, R229, 0, sreg)
    context = [struct.pack(">Q", 0x3000 + i) for i in range(230)]
    context += [struct.pack(">Q", 230)] + [bytes(8)] * (26 + 12)
    context += [struct.pack(">Q", 230 << 56)]
    setup = [
        *set_octa(R250, top), insn(UNSAVE, 0, 0, R250),
        *set_octa(R250, NEGATIVE | 0x2000), insn(PUT, SR_T, 0, R250),
        *set_octa(R250, VM_RV_SOFTWARE), insn(PUT, SR_V, 0, R250),
        *set_octa(R250, base), insn(LDOUI, R243, R250, 0),
    ]
    if recovery == "mapped":
        setup += [*set_octa(R250, stack_page), insn(LDOUI, R243, R250, 0)]
    setup += [*set_octa(R250, NEGATIVE | 0x800),
              insn(PUSHGOI, R229, R250, 0)]
    # Raise a masked b request; PUT rQ alone does not record a new event.
    enter = [insn(GET, R250, 0, SR_Q), insn(PUTI, SR_Q, 0, 0),
             insn(PUTI, SR_M, 1, 0),
             *enter_get(opcode, mode)]

    trap = [insn(GET, R250, 0, SR_YY),
            *set_octa(R251, stack_page), insn(CMPU, R251, R250, R251)]
    branch_pc = 0x2000 + len(trap) * 4
    trap += [branch(BNZ, R251, (0x2600 - branch_pc) // 4)]
    if recovery == "mapped":
        # This initial preload is not an interrupted GET.
        trap += [jump(JMP, (0x2600 - branch_pc - 4) // 4)]
    trap += [
        insn(GET, R230, 0, SR_WW), insn(GET, R231, 0, SR_XX),
        insn(GET, R232, 0, SR_YY), insn(GET, R233, 0, SR_O),
        insn(GET, R234, 0, SR_S), insn(GET, R235, 0, SR_L),
        insn(PUTI, SR_Q, 0, 0), insn(GET, R236, 0, SR_Q),
        insn(ADDUI, R238, R238, 1),
    ]
    if recovery == "rebuilt":
        trap += [*set_octa(R250, 0x11110d000000e000),
                 insn(PUT, SR_V, 0, R250), insn(SAVE, R249, 0, 0),
                 insn(UNSAVE, 0, 0, R249),
                 *set_octa(R250, VM_RV_SOFTWARE), insn(PUT, SR_V, 0, R250)]
    # Restore a new request, and require replay of rXX rather than a refetch.
    trap += [insn(PUTI, SR_Q, 0, 0),
             insn(PUTI, SR_M, 1, 0),
             *set_octa(R250, SOURCE & ~3),
             *set_octa(R251, int.from_bytes(halt(), "big")),
             insn(STTI, R251, R250, 0),
             insn(PUT, SR_WW, 0, R230), insn(PUT, SR_XX, 0, R231),
             insn(PUT, SR_YY, 0, R232),
             *set_octa(R250, stack_page | 6), insn(PUT, SR_ZZ, 0, R250),
             wyde(SETL, R255, 0), insn(RESUME, 0, 0, 1)]
    refill = [insn(GET, R250, 0, SR_YY),
              *set_octa(R251, 0x1fff), insn(ANDN, R250, R250, R251),
              insn(ORI, R250, R250, 7), insn(PUT, SR_ZZ, 0, R250),
              wyde(SETL, R255, 0), insn(RESUME, 0, 0, 1)]
    finish = [insn(PUTI, SR_Q, 0, 0), insn(GET, R244, 0, SR_Q),
              insn(GET, R245, 0, SR_S), insn(GET, R246, 0, SR_L),
              insn(GET, R247, 0, SR_O), wyde(SETL, R255, 0), halt()]
    # SAVE/UNSAVE rebuilds an empty window above the saved ancestor.
    final_s = base + (230 if recovery == "rebuilt" else 205) * 8
    result = {SR_L: 230, SR_S: final_s, SR_Q: RQ_PROGRAM_B}[sreg]
    regs = {R229: result, R238: int(recovery != "mapped"),
            R244: 0 if sreg == SR_Q else RQ_PROGRAM_B,
            R245: final_s, R246: 230, R247: base + 230 * 8}
    regs.update({i: 0 for i in range(229)})
    if recovery != "mapped":
        regs.update({R230: SOURCE + 4,
                     R231: (3 << 56) | int.from_bytes(opcode, "big"),
                     R232: stack_page, R233: base + 230 * 8,
                     R234: stack_page, R235: 0, R236: RQ_PROGRAM_B})
    return get_image(
        f"get-spill-{sreg}-{mode}-{recovery}-{prefix}",
        [(0x100, setup), (0x800, enter), (0x2000, trap), (0x2600, refill),
         (0x4000, [opcode, *finish]),
         (0xe040, [struct.pack(">Q", 0x10007), struct.pack(">Q", 0x12007)]),
         (base, context)], finish, regs,
    )


@pytest.mark.parametrize("sreg", (SR_L, SR_S, SR_Q), ids=("rL", "rS", "rQ"))
@pytest.mark.parametrize("mode", ("fetched", "public", "private"))
@pytest.mark.parametrize("recovery", ("mapped", "direct", "rebuilt"))
@pytest.mark.parametrize("prefix", (24, 28))
def test_get_after_spill(qemu, workdir, sreg, mode, recovery, prefix):
    run_elf_test(qemu, workdir, get_spill_case(sreg, mode, recovery, prefix))


@pytest.mark.parametrize("y,z", ((1, SR_L), (0, 32)))
@pytest.mark.parametrize("mode", ("fetched", "public", "private"))
def test_invalid_get_does_not_grow(qemu, workdir, y, z, mode):
    opcode = insn(GET, R229, y, z)
    setup = [insn(PUTI, SR_G, 0, 230), insn(PUTI, SR_L, 0, 0),
             *set_octa(R250, NEGATIVE | 0x2000), insn(PUT, SR_TT, 0, R250),
             *set_octa(R250, RQ_PROGRAM_B), insn(PUT, SR_K, 0, R250)]
    # Private RESUME takes the inserted operation's mask from $255.
    if mode == "private":
        setup += [*set_octa(R250, SOURCE + 4), insn(PUT, SR_WW, 0, R250),
                  *set_octa(R250, int.from_bytes(opcode, "big")),
                  insn(PUT, SR_XX, 0, R250),
                  *set_octa(R255, RQ_PROGRAM_B), insn(RESUME, 0, 0, 1)]
    else:
        setup += enter_get(opcode, mode)
    handler = [insn(GET, R240, 0, SR_L), insn(GET, R241, 0, SR_Q),
               wyde(SETL, R255, 0), halt()]
    run_elf_test(qemu, workdir, MMIXELFTest(
        f"get-invalid-growth-{y}-{z}-{mode}",
        elf64_image(0, program_with_regions(
            (0x100, setup), (0x2000, handler), (0x4000, [opcode, halt()])),
            entry=NEGATIVE | 0x100, virtual_address=NEGATIVE),
        pc=(NEGATIVE | 0x2000) + 12,
        regs={R229: 0, R240: 0, R241: RQ_PROGRAM_B},
        qemu_args=("-machine", "elf-startup=platform"),
    ))
