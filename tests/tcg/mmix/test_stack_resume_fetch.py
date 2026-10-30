#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct

import pytest

from cases.common import *
from lib.execution import run_one_with_loader


def partial_stack_program(operation, rebase, inserted, deny_fetch, prefix):
    negative = 1 << 63
    source = 0x4003
    done = 0x4083
    handler = negative | 0x2000
    stack_page = 0x12000
    pop = operation == "pop"
    base = stack_page + prefix * 8 if pop else stack_page - prefix * 8
    fault_address = stack_page - 8 if pop else stack_page
    initial_l = 230
    top = base + (initial_l + 1 + 26 + 12) * 8
    opcode = (wyde(POP, 0, 0) if pop else
              branch(PUSHJ, R229, (done - source) // 4) if
              operation == "push" else wyde(SETL, R229, 0xbeef))
    # UNSAVE seeds a real register window. Subsequent spill/fill is genuine.
    context = [struct.pack(">Q", 0x3000 + i) for i in range(initial_l)]
    context += [struct.pack(">Q", initial_l)] + [bytes(8)] * (26 + 12)
    context += [struct.pack(">Q", 230 << 56)]
    setup = [
        *set_octa(R240, top), insn(UNSAVE, 0, 0, R240),
        *set_octa(R240, handler), insn(PUT, SR_T, 0, R240),
        *set_octa(R238, 0x11110d000000e000),
        *set_octa(R240, VM_RV_SOFTWARE), insn(PUT, SR_V, 0, R240),
        # Cache only the page containing the completed access prefix.
        *set_octa(R240, base), insn(LDOUI, R230, R240, 0),
        *set_octa(R240, done), insn(PUT, SR_J, 0, R240),
    ]
    if not pop:
        # The ancestor occupies 230 ring slots; the next growth spills it.
        setup += [*set_octa(R240, negative | 0x800),
                  insn(PUSHGOI, R229, R240, 0)]
    else:
        setup += [*set_octa(R240, negative | 0x800),
                  insn(GOI, R240, R240, 0)]

    enter = ([*set_octa(R240, source + 4), insn(PUT, SR_W, 0, R240),
              *set_octa(R240, int.from_bytes(opcode, "big")),
              insn(PUT, SR_X, 0, R240), insn(RESUME, 0, 0, 0)] if
             inserted else [*set_octa(R240, source),
                            insn(GOI, R240, R240, 0)])

    # Once armed, any ordinary fetch must see normal translation checks.
    trap = [insn(GET, R240, 0, SR_XX),
            *set_octa(R241, 0xffffffff), insn(AND, R240, R240, R241),
            *set_octa(R241, int.from_bytes(insn(SWYM, 0, 0, 0), "big")),
            insn(CMPU, R240, R240, R241)]
    branch_pc = 0x2000 + len(trap) * 4
    trap += [branch(BNZ, R240, (0x2100 - branch_pc) // 4),
             branch(BNZ, R239, (0x2300 - branch_pc - 4) // 4)]
    trap += [insn(GET, R240, 0, SR_YY),
             *set_octa(R241, 0x1fff), insn(ANDN, R240, R240, R241),
             insn(ORI, R240, R240, 7), insn(PUT, SR_ZZ, 0, R240),
             wyde(SETL, R255, 0), insn(RESUME, 0, 0, 1)]

    data = [insn(GET, R240, 0, SR_YY),
            *set_octa(R241, fault_address), insn(CMPU, R240, R240, R241)]
    branch_pc = 0x2100 + len(data) * 4
    # The initial page preload uses the ordinary translation installer.
    data += [branch(BNZ, R240, (0x2400 - branch_pc) // 4),
             insn(GET, R231, 0, SR_WW), insn(GET, R232, 0, SR_XX),
             insn(GET, R233, 0, SR_YY), insn(GET, R234, 0, SR_O),
             insn(GET, R235, 0, SR_S), insn(GET, R236, 0, SR_L)]
    if rebase:
        # Rebuild the window without faulting inside the observer itself.
        data += [insn(PUT, SR_V, 0, R238), insn(SAVE, R250, 0, 0),
                 insn(UNSAVE, 0, 0, R250),
                 *set_octa(R240, VM_RV_SOFTWARE), insn(PUT, SR_V, 0, R240)]
    data += [wyde(SETL, R239, int(deny_fetch))]
    if deny_fetch:
        data += [*set_octa(R240, source & ~0x1fff),
                 insn(LDVTSI, R240, R240, 0)]
    else:
        data += [*set_octa(R240, negative | (source & ~3)),
                 *set_octa(R241, int.from_bytes(halt(), "big")),
                 insn(STTI, R241, R240, 0)]
    data += [insn(PUT, SR_WW, 0, R231), insn(PUT, SR_XX, 0, R232),
             insn(PUT, SR_YY, 0, R233),
             *set_octa(R240, (fault_address & ~0x1fff) | 6),
             insn(PUT, SR_ZZ, 0, R240), wyde(SETL, R255, 0),
             insn(RESUME, 0, 0, 1)]

    finish = [insn(GET, R244, 0, SR_O), insn(GET, R245, 0, SR_L),
              insn(GET, R246, 0, SR_J), insn(GET, R247, 0, SR_WW),
              insn(ADDI, R248, R229, 0), wyde(SETL, R255, 0), halt()]
    regions = [
        (0, [*set_octa(R240, negative | 0x100), insn(GOI, R240, R240, 0)]),
        (0x100, setup), (0x800, enter), (0x2000, trap), (0x2100, data),
        (0x2300, finish),
        (0x2400, [insn(GET, R240, 0, SR_YY),
                  *set_octa(R241, 0x1fff), insn(ANDN, R240, R240, R241),
                  insn(ORI, R240, R240, 7), insn(PUT, SR_ZZ, 0, R240),
                  wyde(SETL, R255, 0), insn(RESUME, 0, 0, 1)]),
        (source & ~3, [opcode, jump(JMP, (done - source - 4) // 4)]),
        (done & ~3, finish),
        (0xe040, [struct.pack(">Q", 0x10007),
                  struct.pack(">Q", 0x12007)]),
    ]
    if pop:
        regions.append((base - 32 * 8,
                        [struct.pack(">Q", 0x1000 + i) for i in range(31)] +
                        [struct.pack(">Q", 31)]))
    regions.append((base, context))
    expected_o = base - 32 * 8 if pop else base + (460 if
                 operation == "push" else 230) * 8
    expected_l = 31 if pop else 0 if operation == "push" else 230
    regs = {R231: source + 4,
            R232: (3 << 56) | int.from_bytes(opcode, "big"),
            R233: fault_address, R234: base if pop else base + 230 * 8,
            R235: stack_page, R236: min(230, 256 - prefix) if pop else 0,
            R244: expected_o, R245: expected_l,
            R246: source + 4 if operation == "push" else
            done if pop else negative | (0x100 + len(setup) * 4),
            R248: 0xbeef if operation == "growth" else 0}
    if pop:
        regs.update({i: 0x1000 + i for i in range(31)})
    if operation == "growth":
        regs.update({i: 0 for i in range(229)})
        regs[R229] = 0xbeef
    if deny_fetch:
        regs[R247] = source + 4 if operation == "growth" else done
    return MMIXTest(
        f"stack-resume-{operation}-{rebase}-{inserted}-{deny_fetch}-{prefix}",
        program_with_regions(*regions),
        pc=((negative | 0x2300) if deny_fetch else done) + 24,
        regs=regs,
    )


@pytest.mark.parametrize("operation", ("push", "growth", "pop"))
@pytest.mark.parametrize("rebase", (False, True), ids=("direct", "rebuilt"))
@pytest.mark.parametrize("inserted", (False, True), ids=("fetched", "inserted"))
@pytest.mark.parametrize("deny_fetch", (False, True), ids=("replace", "unmap"))
@pytest.mark.parametrize("prefix", (24, 28))
def test_partial_stack_resume_without_refetch(qemu, workdir, operation,
                                            rebase, inserted, deny_fetch,
                                            prefix):
    run_one_with_loader(qemu, workdir, partial_stack_program(
        operation, rebase, inserted, deny_fetch, prefix))
