#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import (
    ADDI,
    GET,
    MASK64,
    MMIXELFTest,
    MMIXProcessFailure,
    R20,
    R21,
    R22,
    R23,
    R24,
    R25,
    R32,
    R40,
    R41,
    R42,
    R250,
    R251,
    R252,
    R253,
    R254,
    SHN_XINDEX,
    SHT_STRTAB,
    SR_G,
    elf64_duplicate_shdr,
    elf64_image,
    elf64_image_with_reg_contents,
    elf64_patch_ehdr_field,
    elf64_patch_shdr_field,
    halt,
    insn,
)


GLOBAL_BASE = 250
GLOBAL_VALUES = (
    0x0123456789ABCDEF,
    0x1122334455667788,
    0x8877665544332211,
    0xFEDCBA9876543210,
    0xA5A5A5A55A5A5A5A,
)

REGISTER_PROGRAM = b"".join((
    insn(ADDI, R20, R250, 0),
    insn(ADDI, R21, R251, 0),
    insn(ADDI, R22, R252, 0),
    insn(ADDI, R23, R253, 0),
    insn(ADDI, R24, R254, 0),
    insn(GET, R25, 0, SR_G),
    halt(),
))

ABSENT_PROGRAM = b"".join((
    insn(ADDI, R40, R32, 0),
    insn(ADDI, R41, R254, 0),
    insn(GET, R42, 0, SR_G),
    halt(),
))

VALID_IMAGE = elf64_image_with_reg_contents(
    0, REGISTER_PROGRAM, GLOBAL_BASE, GLOBAL_VALUES
)
EMPTY_IMAGE = elf64_image_with_reg_contents(0, halt(), GLOBAL_BASE, ())

ELF_REGISTER_TESTS = [
    MMIXELFTest(
        "elf-register-contents-bare",
        VALID_IMAGE,
        pc=len(REGISTER_PROGRAM) - 4,
        regs={
            R20: GLOBAL_VALUES[0],
            R21: GLOBAL_VALUES[1],
            R22: GLOBAL_VALUES[2],
            R23: GLOBAL_VALUES[3],
            R24: GLOBAL_VALUES[4],
            R25: GLOBAL_BASE,
        },
    ),
    MMIXELFTest(
        "elf-register-contents-absent",
        elf64_image(0, ABSENT_PROGRAM),
        pc=len(ABSENT_PROGRAM) - 4,
        regs={R40: 0, R41: 0, R42: 32},
    ),
]

ELF_REGISTER_FAILURE_TESTS = [
    MMIXProcessFailure(
        "elf-register-contents-empty",
        EMPTY_IMAGE,
        (),
        ("empty .MMIX.reg_contents section",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-duplicate",
        elf64_duplicate_shdr(VALID_IMAGE, 2),
        (),
        ("duplicate .MMIX.reg_contents section",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-truncated",
        elf64_patch_shdr_field(VALID_IMAGE, 2, "offset", MASK64),
        (),
        ("truncated .MMIX.reg_contents section",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-misaligned-address",
        elf64_patch_shdr_field(
            VALID_IMAGE, 2, "address", GLOBAL_BASE * 8 + 1
        ),
        (),
        ("unaligned .MMIX.reg_contents section",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-misaligned-size",
        elf64_patch_shdr_field(VALID_IMAGE, 2, "size", 1),
        (),
        ("unaligned .MMIX.reg_contents section",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-wrong-type",
        elf64_patch_shdr_field(VALID_IMAGE, 2, "type", SHT_STRTAB),
        (),
        ("invalid .MMIX.reg_contents section type",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-below-global-range",
        elf64_patch_shdr_field(VALID_IMAGE, 2, "address", 31 * 8),
        (),
        ("invalid .MMIX.reg_contents register range",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-includes-r255",
        elf64_patch_shdr_field(VALID_IMAGE, 2, "address", 254 * 8),
        (),
        ("invalid .MMIX.reg_contents register range",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-extended-section-count",
        elf64_patch_ehdr_field(VALID_IMAGE, "section_count", 0),
        (),
        ("unsupported MMIX ELF extended section numbering",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-extended-name-index",
        elf64_patch_ehdr_field(
            VALID_IMAGE, "section_names_index", SHN_XINDEX
        ),
        (),
        ("unsupported MMIX ELF extended section-name index",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-invalid-name-index",
        elf64_patch_ehdr_field(VALID_IMAGE, "section_names_index", 3),
        (),
        ("invalid MMIX ELF section-name index",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-invalid-entry-size",
        elf64_patch_ehdr_field(VALID_IMAGE, "section_entry_size", 63),
        (),
        ("invalid MMIX ELF section table",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-truncated-section-table",
        VALID_IMAGE[:-1],
        (),
        ("truncated MMIX ELF section table",),
    ),
    MMIXProcessFailure(
        "elf-register-contents-invalid-name-offset",
        elf64_patch_shdr_field(VALID_IMAGE, 2, "name", 0xFFFFFFFF),
        (),
        ("invalid MMIX ELF section name offset",),
    ),
]
