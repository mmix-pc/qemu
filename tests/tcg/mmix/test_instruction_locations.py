#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pytest

from cases.common import *
from cases.isa import (
    forced_instruction_translation_program,
    resume_unfetchable_insertion_test,
)
from lib.execution import run_one_with_loader, run_semihosting_one


def resume_to(address):
    return [
        *set_octa(R200, address),
        insn(PUT, SR_W, 0, R200),
        *set_octa(R201, 1 << 63),
        insn(PUT, SR_X, 0, R201),
        insn(RESUME, 0, 0, 0),
    ]


@pytest.mark.parametrize("low", range(4))
@pytest.mark.parametrize("negative", (False, True))
def test_containing_tetra_fetch(qemu, workdir, low, negative):
    address = (0x100 + low) | ((1 << 63) if negative else 0)
    enter = [*set_octa(R200, address), insn(GO, R201, R200, R0)] if (
        negative
    ) else resume_to(address)
    test = MMIXTest(
        f"instruction-tetra-{low}-{'negative' if negative else 'positive'}",
        program_with_regions(
            (0, [wyde(SETL, R1, 0x400), insn(PUT, SR_T, 0, R1),
                 *enter]),
            (0x100, [bytes.fromhex("000200000000020000000000")]),
            (0x400, [insn(GET, R40, 0, SR_WW),
                     insn(GET, R41, 0, SR_XX), halt()]),
        ),
        pc=0x408,
        regs={R40: address + 4,
              R41: (1 << 63) | int.from_bytes(insn(TRAP, 2, 0, 0), "big")},
    )
    run_one_with_loader(qemu, workdir, test)


@pytest.mark.parametrize("low", range(4))
@pytest.mark.parametrize("entry", ("resume", "go", "goi"))
@pytest.mark.parametrize("hosted", (False, True))
def test_logical_locations_and_links(qemu, workdir, low, entry, hosted):
    enter = resume_to(0x100 + low) if entry == "resume" else [
        *set_octa(R200, 0x100 + low),
        insn(GOI if entry == "goi" else GO, R201, R200, 0),
    ]
    test = MMIXTest(
        f"instruction-links-{entry}-{low}-{'hosted' if hosted else 'raw'}",
        program_with_regions(
            (0, [wyde(SETL, R0, 0), *set_octa(R213, 0x300 + low),
                 *set_octa(R215, 0x400 + low), *enter]),
            (0x100, [branch(GETA, R210, 0),
                     branch(PUSHJ, R0, (0x200 - 0x104) // 4),
                     insn(GO, R212, R213, R0), halt()]),
            (0x200, [insn(GET, R211, 0, SR_J), wyde(POP, 0, 0)]),
            (0x300, [branch(GETA, R214, 0),
                     insn(PUSHGOI if entry == "goi" else PUSHGO,
                          R0, R215, 0),
                     branch(GETA, R217, 0),
                     branch(BZ, R0, (0x320 - 0x30c) // 4), halt()]),
            (0x320, [branch(GETA, R218, 0), halt()]),
            (0x400, [insn(GET, R216, 0, SR_J), wyde(POP, 0, 0)]),
        ),
        pc=0x324 + low,
        regs={R210: 0x100 + low, R211: 0x108 + low,
              R212: 0x10c + low, R214: 0x300 + low,
              R216: 0x308 + low, R217: 0x308 + low, R218: 0x320 + low},
    )
    (run_semihosting_one if hosted else run_one_with_loader)(
        qemu, workdir, test
    )


@pytest.mark.parametrize("low", range(4))
@pytest.mark.parametrize("hosted", (False, True))
def test_logical_pc_crosses_page(qemu, workdir, low, hosted):
    test = MMIXTest(
        f"instruction-page-end-{low}-{'hosted' if hosted else 'raw'}",
        program_with_regions(
            (0, resume_to(0x1ffc + low)),
            (0x1ffc, [branch(GETA, R240, 0),
                      branch(GETA, R241, 0), halt()]),
        ),
        pc=0x2004 + low,
        regs={R240: 0x1ffc + low, R241: 0x2000 + low},
    )
    (run_semihosting_one if hosted else run_one_with_loader)(
        qemu, workdir, test
    )


@pytest.mark.parametrize("low", range(4))
def test_page_end_before_fetch_fault(qemu, workdir, low):
    handler = (1 << 63) | 0x400
    test = MMIXTest(
        f"instruction-page-fault-{low}",
        program_with_regions(
            (0, [*set_octa(R1, handler), insn(PUT, SR_TT, 0, R1),
                 *set_octa(R2, RQ_PROGRAM_X), insn(PUT, SR_K, 0, R2),
                 *set_octa(R3, VM_PAGE_TABLE), wyde(SETL, R4, 7),
                 insn(STOU, R4, R3, R0), wyde(SETL, R4, 0x6006),
                 insn(STOUI, R4, R3, 8),
                 *set_octa(R5, VM_RV_PAGE0), insn(PUT, SR_V, 0, R5),
                 *resume_to(0x1ffc + low)]),
            (0x400, [insn(GET, R241, 0, SR_WW),
                     insn(GET, R242, 0, SR_XX), halt()]),
            (0x1ffc, [branch(GETA, R240, 0)]),
        ),
        pc=handler + 8,
        regs={R240: 0x1ffc + low, R241: 0x2004 + low,
              R242: RQ_PROGRAM_X | int.from_bytes(insn(SWYM, 0, 0, 0), "big")},
    )
    run_one_with_loader(qemu, workdir, test)


@pytest.mark.parametrize("low", range(4))
def test_forced_fetch_translation_preserves_pc(qemu, workdir, low):
    test = MMIXTest(
        f"instruction-forced-fetch-{low}",
        forced_instruction_translation_program(
            0x3ffc + low,
            ((0x5ffc, [branch(GETA, R240, 0)]),
             (0x6000, [branch(GETA, R241, 0), halt()])),
            [insn(GET, R242, 0, SR_WW), insn(GET, R243, 0, SR_YY),
             insn(GET, R244, 0, SR_XX),
             *set_octa(R50, 0x1fff), insn(ANDN, R51, R243, R50),
             wyde(SETL, R52, 0x2000), insn(ADDU, R53, R51, R52),
             insn(ORI, R53, R53, 7), insn(PUT, SR_ZZ, 0, R53),
             insn(ADDI, R245, R245, 1), insn(RESUME, 0, 0, 1)],
        ),
        pc=0x4004 + low,
        regs={R240: 0x3ffc + low, R241: 0x4000 + low,
              R242: 0x4000 + low, R243: 0x4000 + low,
              R244: 0x03000000fd000000, R245: 2},
    )
    run_one_with_loader(qemu, workdir, test)


@pytest.mark.parametrize("low", range(4))
def test_masked_fetch_fault_preserves_pc(qemu, workdir, low):
    test = MMIXTest(
        f"instruction-masked-fetch-{low}",
        program_with_regions(
            (0, [*set_octa(R1, VM_PAGE_TABLE), wyde(SETL, R2, 7),
                 insn(STOU, R2, R1, R0), wyde(SETL, R2, 0x6006),
                 insn(STOUI, R2, R1, 8), wyde(SETL, R2, 0x8007),
                 insn(STOUI, R2, R1, 16),
                 *set_octa(R3, VM_RV_PAGE0), insn(PUT, SR_V, 0, R3),
                 *resume_to(0x3ffc + low)]),
            (0x7ffc, [wyde(SETL, R242, 0xdead)]),
            (0x8000, [branch(GETA, R240, 0),
                      insn(GET, R241, 0, SR_Q), halt()]),
        ),
        pc=0x4008 + low,
        regs={R240: 0x4000 + low, R241: RQ_PROGRAM_X, R242: 0},
    )
    run_one_with_loader(qemu, workdir, test)


@pytest.mark.parametrize("low", range(4))
def test_data_fault_preserves_instruction_pc(qemu, workdir, low):
    handler = (1 << 63) | 0x400
    operation = insn(LDOU, R240, R250, R0)
    test = MMIXTest(
        f"instruction-data-fault-{low}",
        program_with_regions(
            (0, [*set_octa(R1, handler), insn(PUT, SR_TT, 0, R1),
                 *set_octa(R2, RQ_PROGRAM_R), insn(PUT, SR_K, 0, R2),
                 *set_octa(R3, VM_PAGE_TABLE), wyde(SETL, R4, 7),
                 insn(STOU, R4, R3, R0), wyde(SETL, R4, 0x6001),
                 insn(STOUI, R4, R3, 8),
                 *set_octa(R5, VM_RV_PAGE0), insn(PUT, SR_V, 0, R5),
                 wyde(SETL, R250, 0x2000), *resume_to(0x100 + low)]),
            (0x100, [operation, halt()]),
            (0x400, [insn(GET, R241, 0, SR_WW),
                     insn(GET, R242, 0, SR_XX),
                     insn(GET, R243, 0, SR_YY), halt()]),
        ),
        pc=handler + 12,
        regs={R241: 0x104 + low,
              R242: RQ_PROGRAM_R | int.from_bytes(operation, "big"),
              R243: 0x2000},
    )
    run_one_with_loader(qemu, workdir, test)


@pytest.mark.parametrize("low", range(4))
@pytest.mark.parametrize("ropcode", (0, 1))
def test_insertion_does_not_fetch_logical_pc(qemu, workdir, low, ropcode):
    test = resume_unfetchable_insertion_test(
        f"instruction-insertion-{ropcode}-{low}", ropcode,
        (0x4000000 if ropcode == 0 else 0x2000) + low,
        None if ropcode == 0 else 0x6006,
    )
    run_one_with_loader(qemu, workdir, test)


def test_tb_cache_distinguishes_logical_locations(qemu, workdir):
    main = []
    for low in range(4):
        main.extend([
            *set_octa(R200, 0x100 + low), insn(GO, R210, R200, R0),
            insn(ORI, R220 + low, R240, 0),
        ])
    main.append(halt())
    test = MMIXTest(
        "instruction-cache-logical-locations",
        program_with_regions(
            (0, main),
            (0x100, [branch(GETA, R240, 0), insn(GO, R211, R210, R0)]),
        ),
        pc=(len(main) - 1) * 4,
        regs={R220 + low: 0x100 + low for low in range(4)},
    )
    run_one_with_loader(qemu, workdir, test)


@pytest.mark.parametrize("low", range(4))
def test_write_first_byte_invalidates_instruction(qemu, workdir, low):
    main = [
        *set_octa(R200, 0x2000 + low), insn(GO, R210, R200, R0),
        wyde(SETL, R201, SUBI), wyde(SETL, R202, 0x2000),
        insn(STBU, R201, R202, R0), insn(GO, R210, R200, R0), halt(),
    ]
    test = MMIXTest(
        f"instruction-invalidate-first-byte-{low}",
        program_with_regions(
            (0, main),
            (0x2000, [insn(ADDI, R240, R240, 1), insn(GO, R211, R210, R0)]),
        ),
        pc=(len(main) - 1) * 4,
        regs={R240: 0},
    )
    run_one_with_loader(qemu, workdir, test)
