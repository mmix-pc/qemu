#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct

import pytest

from cases.common import *
from lib.execution import run_elf_test


def resume_location_case(mode, where, private, *, masked=False,
                         event=False, destination=R240, insecure=False):
    negative = 1 << 63
    handler = negative | 0x3000
    bank = (SR_WW, SR_XX, SR_YY, SR_ZZ) if private else (
        SR_W, SR_X, SR_Y, SR_Z)
    y, z = 10, 20
    result = 41
    operation = insn(ADDUI, destination, R241, 1)
    if mode == "again":
        exec_word = int.from_bytes(operation, "big")
        result = 11
    elif mode == "continue":
        exec_word = (1 << 56) | int.from_bytes(operation, "big")
        result = y + z
    elif mode == "set":
        z = result = 0xface
        exec_word = (2 << 56) | int.from_bytes(operation, "big")
        if event:
            exec_word |= RA_EVENT_V << 40
    elif mode == "data":
        y, z = 0x8000, 0x8006
        operation = insn(LDOUI, destination, R241, 0)
        exec_word = (3 << 56) | int.from_bytes(operation, "big")
        result = 0xface
    elif mode == "fetch":
        y, z = where, 7
        exec_word = (3 << 56) | int.from_bytes(insn(SWYM, 0, 0, 0), "big")
    else:
        assert mode == "skip"
        exec_word = negative

    rk = 0 if insecure else RQ_PROGRAM_MASK & ~(RQ_PROGRAM_P if masked else 0)
    setup = [
        insn(PUTI, SR_G, 0, 224),
        *set_octa(R230, handler), insn(PUT, SR_TT, 0, R230),
        wyde(SETL, destination, 41),
        *set_octa(R241, 0x8000 if mode == "data" else 10),
        *set_octa(R230, where), insn(PUT, bank[0], 0, R230),
        *set_octa(R230, exec_word), insn(PUT, bank[1], 0, R230),
        *set_octa(R230, y), insn(PUT, bank[2], 0, R230),
        *set_octa(R230, z), insn(PUT, bank[3], 0, R230),
        wyde(SETL, R230, (RA_EVENT_V << RA_ENABLE_SHIFT) if event else 0),
        insn(PUT, SR_A, 0, R230),
        insn(PUTI, SR_BB, 0, 41 if destination == R255 else 0),
    ]
    if mode in ("data", "fetch"):
        setup += [*set_octa(R230, VM_RV_SOFTWARE),
                  insn(PUT, SR_V, 0, R230)]
    if not private:
        # Enter positive code with all program bits set, without executing
        # any negative instruction under that mask before the public RESUME.
        setup += [*set_octa(R230, 0x2000), insn(PUT, SR_WW, 0, R230),
                  *set_octa(R230, negative), insn(PUT, SR_XX, 0, R230)]
    setup += [*set_octa(R255, rk), insn(RESUME, 0, 0, 1)]

    observe = [
        insn(GET, R224, 0, SR_Q), insn(GET, R225, 0, SR_WW),
        insn(GET, R226, 0, SR_XX), insn(GET, R227, 0, SR_YY),
        insn(GET, R228, 0, SR_ZZ), insn(GET, R229, 0, SR_A),
        insn(GET, R232, 0, SR_BB), insn(GET, R233, 0, SR_W),
        insn(GET, R234, 0, SR_X), insn(GET, R235, 0, SR_Y),
        insn(GET, R236, 0, SR_Z),
        wyde(SETL, R255, 0), halt(),
    ]
    payload = program_with_regions(
        (0, [halt(), halt()]), (32, observe), (0x1000, setup),
        (0x2000, [insn(RESUME, 0, 0, 0)]), (0x3000, observe),
        (0x8000, [struct.pack(">Q", 0xface)]),
    )
    cause = (RQ_PROGRAM_S if insecure and where >= 4 else
             RQ_PROGRAM_P if where < 4 and not masked and not insecure else 0)
    if mode in ("skip", "fetch"):
        cause = 0
    trip = event and where >= 4
    pc = (32 + (len(observe) - 1) * 4 if trip else
          handler + (len(observe) - 1) * 4 if cause else where)
    regs = {} if destination == R255 else {destination: result}
    if cause and not trip:
        set_insn = int.from_bytes(wyde(SETH, destination, 0), "big")
        regs.update({
            R224: cause, R225: where,
            R226: negative | cause | (set_insn if mode == "set" else
                                      int.from_bytes(operation, "big")),
            R229: ((RA_EVENT_V << RA_ENABLE_SHIFT) | RA_EVENT_V)
                  if event else 0,
            R232: result if destination == R255 else 0,
        })
        if mode == "set":
            regs.update({R227: y, R228: z})
    if trip:
        regs.update({R224: cause, R233: where,
                     R234: negative | int.from_bytes(
                         wyde(SETH, destination, 0), "big"),
                     R235: y, R236: z,
                     R229: RA_EVENT_V << RA_ENABLE_SHIFT})
    return MMIXELFTest(
        f"resume-location-{mode}-{where}-{private}-{masked}-{event}-"
        f"{destination}-{insecure}",
        elf64_image(0, payload, entry=negative | 0x1000,
                    virtual_address=negative),
        pc=pc, regs=regs,
        qemu_args=("-machine", "elf-startup=platform"),
    )


@pytest.mark.parametrize("mode", ("again", "continue", "set", "skip"))
@pytest.mark.parametrize("where", (0, 1, 2, 3, 4, 7))
@pytest.mark.parametrize("private", (False, True), ids=("public", "private"))
def test_resume_insertion_location(qemu, workdir, mode, where, private):
    run_elf_test(qemu, workdir, resume_location_case(mode, where, private))


@pytest.mark.parametrize("mode", ("data", "fetch"))
@pytest.mark.parametrize("where", (0, 1, 2, 3))
def test_translation_resume_insertion_location(qemu, workdir, mode, where):
    run_elf_test(qemu, workdir, resume_location_case(mode, where, True))


@pytest.mark.parametrize("where", (0, 3))
def test_resume_set_masked_protection(qemu, workdir, where):
    run_elf_test(qemu, workdir, resume_location_case("set", where, True,
                                                  masked=True))


@pytest.mark.parametrize("where", (0, 3, 4, 7))
@pytest.mark.parametrize("private", (False, True), ids=("public", "private"))
def test_resume_set_arithmetic_location(qemu, workdir, where, private):
    run_elf_test(qemu, workdir, resume_location_case("set", where, private,
                                                  event=True))


@pytest.mark.parametrize("where", (0, 3))
def test_resume_set_destination_before_protection(qemu, workdir, where):
    run_elf_test(qemu, workdir, resume_location_case("set", where, True,
                                                  destination=R255))


def test_resume_set_incomplete_program_mask(qemu, workdir):
    run_elf_test(qemu, workdir, resume_location_case("set", 4, True,
                                                  insecure=True))
