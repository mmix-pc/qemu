#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses

from .common import (
    ADDI,
    GET,
    JMP,
    R0,
    R1,
    R32,
    R33,
    R34,
    R35,
    R250,
    SR_G,
    elf64_image_with_reg_contents,
    elf64_patch_phdr_field,
    insn,
    jump,
)


@dataclasses.dataclass(frozen=True)
class MMIXELFStateTest:
    name: str
    image: bytes
    entry: int
    idle_pc: int
    bss: int
    global_value: int
    qemu_args: tuple[str, ...]


GLOBAL_VALUE = 0x1122334455667788
BSS_ADDRESS = 0x4000
PROGRAM = b"".join((
    insn(ADDI, R32, R250, 0),
    insn(ADDI, R33, R0, 0),
    insn(ADDI, R34, R1, 0),
    insn(GET, R35, 0, SR_G),
    jump(JMP, 0),
))
IMAGE = elf64_image_with_reg_contents(
    0, PROGRAM, 250, (GLOBAL_VALUE,)
)
IMAGE = elf64_patch_phdr_field(IMAGE, 0, "memory_size", BSS_ADDRESS + 8)

ELF_STATE_TEST = MMIXELFStateTest(
    name="elf-reset-and-snapshot-state",
    image=IMAGE,
    entry=0,
    idle_pc=len(PROGRAM) - 4,
    bss=BSS_ADDRESS,
    global_value=GLOBAL_VALUE,
    qemu_args=(
        "-machine",
        "elf-startup-abi=argc-argv",
        "-semihosting-config",
        "enable=on,arg=prog,arg=one",
    ),
)
