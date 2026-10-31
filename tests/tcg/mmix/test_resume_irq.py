#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct

import pytest

from cases.common import *
from lib.execution import run_elf_test


def resume_irq_case(mode, where, private, after=False):
    negative = 1 << 63
    handler = negative | 0x3000
    bank = (SR_WW, SR_XX, SR_YY, SR_ZZ) if private else (
        SR_W, SR_X, SR_Y, SR_Z)
    operation = insn(ADDUI, R240, R240, 1)
    y, z = 7, 9
    result = 42
    if mode == "again":
        exec_word = int.from_bytes(operation, "big")
    elif mode == "continue":
        exec_word = (1 << 56) | int.from_bytes(operation, "big")
        result = y + z
    elif mode == "set":
        exec_word = (2 << 56) | int.from_bytes(operation, "big")
        result = z
    elif mode == "branch":
        operation = jump(JMP, 0x400)
        exec_word = int.from_bytes(operation, "big")
        result = 41
    elif mode in ("data", "fault"):
        operation = insn(LDOUI, R240, R241, 0)
        exec_word = int.from_bytes(operation, "big")
        if mode == "data":
            exec_word |= 3 << 56
        y, z = 0x8000, 0x8006
        result = 0xface if mode == "data" else 41
    elif mode == "fetch":
        exec_word = (3 << 56) | int.from_bytes(insn(SWYM, 0, 0, 0), "big")
        y, z = where, 7
        result = 41
    else:
        assert mode == "skip"
        exec_word = negative
        result = 41

    setup = [
        insn(PUTI, SR_G, 0, 224),
        *set_octa(R230, handler), insn(PUT, SR_TT, 0, R230),
        insn(PUT, SR_T, 0, R230),
        wyde(SETL, R240, 41), wyde(SETL, R241, 0x8000),
        *set_octa(R230, where), insn(PUT, bank[0], 0, R230),
        *set_octa(R230, exec_word), insn(PUT, bank[1], 0, R230),
        *set_octa(R230, y), insn(PUT, bank[2], 0, R230),
        *set_octa(R230, z), insn(PUT, bank[3], 0, R230),
    ]
    if mode in ("data", "fetch", "fault"):
        setup += [*set_octa(R230, VM_RV_SOFTWARE),
                  insn(PUT, SR_V, 0, R230)]
    if not private:
        setup += [*set_octa(R230, 0x2000), insn(PUT, SR_WW, 0, R230),
                  *set_octa(R230, negative), insn(PUT, SR_XX, 0, R230)]
    setup += [
        *set_octa(R255, RQ_PROGRAM_MASK | RQ_INTERVAL),
        # PUT and RESUME consume two counts; public insertion has an outer
        # RESUME 1 as well. One extra count interrupts after the insertion.
        insn(PUTI, SR_I, 0, 2 + int(not private) + int(after)),
        insn(RESUME, 0, 0, 1),
    ]
    observe = [
        insn(ORI, R239, R240, 0),
        insn(GET, R224, 0, SR_Q), insn(GET, R225, 0, SR_WW),
        insn(GET, R226, 0, SR_XX), insn(GET, R227, 0, SR_YY),
        wyde(SETL, R255, 0), halt(),
    ]
    payload = program_with_regions(
        (0, [halt()]), (0x1000, setup),
        (0x2000, [insn(RESUME, 0, 0, 0), halt()]),
        (0x3000, observe), (0x4000, [halt(), halt()]),
        (0x8000, [struct.pack(">Q", 0xface)]),
    )
    cause = RQ_PROGRAM_P if where < 4 and mode not in ("skip", "fetch") else 0
    trap_insn = int.from_bytes(
        wyde(SETH, R240, 0) if mode == "set" else operation, "big")
    regs = {R239: result, R240: result, R224: RQ_INTERVAL | cause,
            R225: where - 4 + 0x1000 if mode == "branch" else where,
            R226: negative | (cause | trap_insn if cause else 0)}
    if mode == "fault":
        regs.update({R226: (3 << 56) | trap_insn, R227: 0x8000})
    return MMIXELFTest(
        f"resume-irq-{mode}-{where}-{private}-{after}",
        elf64_image(0, payload, entry=negative | 0x1000,
                    virtual_address=negative),
        pc=handler + (len(observe) - 1) * 4,
        regs=regs,
        qemu_args=("-machine", "elf-startup=platform"),
    )


@pytest.mark.parametrize("mode", ("again", "continue", "set"))
@pytest.mark.parametrize("where", (0, 3, 0x4004, 0x4007))
@pytest.mark.parametrize("private", (False, True), ids=("public", "private"))
@pytest.mark.parametrize("after", (False, True), ids=("before", "after"))
def test_resume_irq_boundary(qemu, workdir, mode, where, private, after):
    run_elf_test(qemu, workdir, resume_irq_case(mode, where, private, after))


@pytest.mark.parametrize("where", (0, 3, 0x4004, 0x4007))
@pytest.mark.parametrize("private", (False, True), ids=("public", "private"))
def test_resume_skip_irq_boundary(qemu, workdir, where, private):
    run_elf_test(qemu, workdir, resume_irq_case("skip", where, private))


@pytest.mark.parametrize("mode", ("data", "fetch"))
@pytest.mark.parametrize("where", (0, 3, 0x4004, 0x4007))
def test_resume_translation_irq_boundary(qemu, workdir, mode, where):
    run_elf_test(qemu, workdir, resume_irq_case(mode, where, True))


@pytest.mark.parametrize("where", (0x4004, 0x4007))
@pytest.mark.parametrize("private", (False, True), ids=("public", "private"))
def test_resume_branch_irq_boundary(qemu, workdir, where, private):
    run_elf_test(qemu, workdir, resume_irq_case("branch", where, private))


@pytest.mark.parametrize("where", (0x4004, 0x4007))
def test_resume_fault_before_irq(qemu, workdir, where):
    run_elf_test(qemu, workdir, resume_irq_case("fault", where, True))
