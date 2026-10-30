#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import *
from lib.execution import run_elf_test


def resume_trap_case(private, ropcode, x, marginal, low_bits,
                     operand_255=False):
    negative = 1 << 63
    forced = negative | 0x3000
    dynamic = negative | 0x3100
    where = 0x4004 + low_bits
    bank = (SR_WW, SR_XX, SR_YY, SR_ZZ) if private else (
        SR_W, SR_X, SR_Y, SR_Z)
    operation = insn(TRAP, x, R255 if operand_255 else R241,
                     R255 if operand_255 else R242)
    exec_word = (ropcode << 56) | int.from_bytes(operation, "big")
    local_count = 32 if x == R31 else 1
    if marginal and x == R0:
        local_count = 0
    setup = [
        insn(PUTI, SR_G, 0, 230),
        wyde(SETL, R0, 0xbeef),
        *([wyde(SETL, R31, 0xbeef)] if x == R31 else []),
        insn(PUTI, SR_L, 0, local_count),
        *set_octa(R250, forced), insn(PUT, SR_T, 0, R250),
        *set_octa(R250, dynamic), insn(PUT, SR_TT, 0, R250),
        wyde(SETL, R240, 41),
        wyde(SETL, R241, 0x5566), wyde(SETL, R242, 0x7788),
        *set_octa(R250, where), insn(PUT, bank[0], 0, R250),
        *set_octa(R250, exec_word), insn(PUT, bank[1], 0, R250),
        wyde(SETL, R250, 0x1122), insn(PUT, bank[2], 0, R250),
        wyde(SETL, R250, 0x3344), insn(PUT, bank[3], 0, R250),
        wyde(SETL, R250, 0xabcd), insn(PUT, SR_BB, 0, R250),
        wyde(SETL, R250, 0x1234), insn(PUT, SR_J, 0, R250),
    ]
    if not private:
        # Use RESUME 1 to enter user code without executing negative
        # instructions under the user program-protection mask.
        setup += [*set_octa(R250, 0x2000), insn(PUT, SR_WW, 0, R250),
                  *set_octa(R250, negative), insn(PUT, SR_XX, 0, R250)]
    else:
        # Invalid private insertions must be rejected before restoring rK.
        setup += [*set_octa(R250, RQ_PROGRAM_B),
                  insn(PUT, SR_K, 0, R250)]
    setup += [*set_octa(R255, RQ_PROGRAM_MASK), insn(RESUME, 0, 0, 1)]
    origin_next = negative | (0x1000 + len(setup) * 4) if private else 0x2004
    observe = [
        insn(GET, R230, 0, SR_Q), insn(GET, R231, 0, SR_WW),
        insn(GET, R232, 0, SR_XX), insn(GET, R233, 0, SR_YY),
        insn(GET, R234, 0, SR_ZZ), insn(GET, R235, 0, SR_BB),
        insn(GET, R236, 0, SR_K), insn(ORI, R237, R255, 0),
        insn(GET, R238, 0, SR_L), insn(GET, R239, 0, SR_G),
        wyde(SETL, R255, 0), halt(),
    ]
    rejected = ropcode == 1 and marginal
    y, z = ((0x1122, 0x3344) if ropcode else
            (0xabcd, 0xabcd) if operand_255 else (0x5566, 0x7788))
    regs = {
        R230: RQ_PROGRAM_B if rejected else 0,
        R231: origin_next if rejected else where,
        R232: negative | (RQ_PROGRAM_B | int.from_bytes(
            insn(RESUME, 0, 0, int(private)), "big") if rejected else
            int.from_bytes(operation, "big")),
        R235: RQ_PROGRAM_MASK if private and rejected else 0xabcd,
        R236: 0, R237: 0x1234, R238: local_count, R239: 230, R240: 41,
    }
    if not rejected:
        regs.update({R233: y, R234: z})
    payload = program_with_regions(
        (0x1000, setup), (0x2000, [insn(RESUME, 0, 0, 0), halt()]),
        (0x3000, observe), (0x3100, observe), (0x4000, [halt(), halt()]),
    )
    return MMIXELFTest(
        f"resume-trap-{private}-{ropcode}-{x}-{marginal}-{low_bits}-"
        f"{operand_255}",
        elf64_image(0, payload, entry=negative | 0x1000,
                    virtual_address=negative),
        pc=(dynamic if rejected else forced) + (len(observe) - 1) * 4,
        regs=regs,
        # The positive X=0 insertion must take a forced trap, not a hosted
        # call. Only the negative observer's final Halt uses semihosting.
        qemu_args=("-machine", "elf-startup=platform",
                   "-semihosting-config", "enable=on,userspace=off"),
    )


@pytest.mark.parametrize("private", (False, True), ids=("public", "private"))
@pytest.mark.parametrize("ropcode", (0, 1), ids=("again", "continue"))
@pytest.mark.parametrize("x,marginal", (
    (R0, False), (R31, False), (R240, False), (R255, False),
    (R0, True), (R100, True),
))
@pytest.mark.parametrize("low_bits", (0, 3))
def test_resume_trap_operands(qemu, workdir, private, ropcode, x, marginal,
                              low_bits):
    run_elf_test(qemu, workdir, resume_trap_case(
        private, ropcode, x, marginal, low_bits))


@pytest.mark.parametrize("private", (False, True), ids=("public", "private"))
@pytest.mark.parametrize("ropcode", (0, 1), ids=("again", "continue"))
def test_resume_trap_restored_register_operand(qemu, workdir, private,
                                              ropcode):
    run_elf_test(qemu, workdir, resume_trap_case(
        private, ropcode, R240, False, 0, operand_255=True))
