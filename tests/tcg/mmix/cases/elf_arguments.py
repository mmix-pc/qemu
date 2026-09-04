#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import (
    ADDI,
    CMPU,
    GET,
    LDBU,
    LDBUI,
    LDOU,
    LDOUI,
    MMIXELFTest,
    MMIXProcessFailure,
    R0,
    R1,
    R32,
    R33,
    R34,
    R35,
    R36,
    R37,
    R38,
    R39,
    R40,
    R41,
    R42,
    R43,
    R44,
    R45,
    R46,
    R47,
    R250,
    SR_L,
    SUBUI,
    SUBU,
    elf64_image,
    elf64_image_with_reg_contents,
    halt,
    insn,
    set_octa,
)


def explicit_arguments_program():
    program = [
        insn(ADDI, R32, R0, 0),
        insn(GET, R33, 0, SR_L),
        insn(LDOU, R34, R1, R0),
        insn(LDOUI, R35, R1, 8),
        insn(LDOUI, R36, R1, 16),
        insn(LDOUI, R37, R1, 24),
        insn(SUBU, R38, R34, R1),
        insn(SUBU, R39, R35, R34),
        insn(SUBU, R40, R36, R35),
        insn(SUBUI, R45, R1, 8),
        insn(LDOU, R46, R45, R250),
        insn(SUBU, R47, R46, R45),
        insn(LDBUI, R41, R34, 0),
        insn(LDBUI, R42, R34, 4),
        insn(LDBUI, R43, R35, 0),
        insn(LDBUI, R44, R36, 0),
        halt(),
    ]
    regs = {
        R32: 3,
        R33: 2,
        R37: 0,
        R38: 32,
        R39: 8,
        R40: 8,
        R41: ord("p"),
        R42: 0,
        R43: ord("o"),
        R44: ord("t"),
        R47: 64,
    }
    return b"".join(program), (len(program) - 1) * 4, regs


def fallback_arguments_program():
    program = [
        insn(ADDI, R32, R0, 0),
        insn(GET, R33, 0, SR_L),
        insn(LDOU, R34, R1, R0),
        insn(LDOUI, R35, R1, 8),
        insn(LDOUI, R36, R1, 16),
        insn(LDOUI, R37, R1, 24),
        insn(SUBU, R38, R34, R1),
        insn(CMPU, R39, R35, R34),
        insn(LDBUI, R40, R35, 0),
        insn(LDBUI, R41, R35, 3),
        insn(LDBUI, R42, R36, 0),
        insn(LDBUI, R43, R36, 3),
        insn(SUBU, R44, R36, R35),
        halt(),
    ]
    regs = {
        R32: 3,
        R33: 2,
        R37: 0,
        R38: 32,
        R39: 1,
        R40: ord("o"),
        R41: 0,
        R42: ord("t"),
        R43: 0,
        R44: 8,
    }
    return b"".join(program), (len(program) - 1) * 4, regs


def empty_argument_program():
    program = [
        insn(ADDI, R32, R0, 0),
        insn(GET, R33, 0, SR_L),
        insn(LDOU, R34, R1, R0),
        insn(LDOUI, R35, R1, 8),
        insn(SUBU, R36, R34, R1),
        insn(LDBUI, R37, R34, 0),
        insn(SUBUI, R38, R1, 8),
        insn(LDOU, R39, R38, R250),
        insn(SUBU, R40, R39, R38),
        halt(),
    ]
    regs = {
        R32: 1,
        R33: 2,
        R35: 0,
        R36: 16,
        R37: 0,
        R40: 32,
    }
    return b"".join(program), (len(program) - 1) * 4, regs


def large_argument_program(length):
    program = [
        insn(ADDI, R32, R0, 0),
        insn(LDOU, R33, R1, R0),
        *set_octa(R34, length - 1),
        insn(LDBU, R35, R33, R34),
        insn(ADDI, R34, R34, 1),
        insn(LDBU, R36, R33, R34),
        insn(SUBUI, R37, R1, 8),
        insn(LDOU, R38, R37, R250),
        insn(SUBU, R39, R38, R37),
        halt(),
    ]
    regs = {R32: 1, R35: ord("x"), R36: 0, R39: 5032}
    return b"".join(program), (len(program) - 1) * 4, regs


EXPLICIT_ARGUMENTS = explicit_arguments_program()
FALLBACK_ARGUMENTS = fallback_arguments_program()
EMPTY_ARGUMENT = empty_argument_program()
LARGE_ARGUMENT_LENGTH = 5000
LARGE_ARGUMENT = "x" * LARGE_ARGUMENT_LENGTH
LARGE_ARGUMENT_PROBE = large_argument_program(LARGE_ARGUMENT_LENGTH)

GLOBAL_ARGUMENT_PROGRAM = b"".join((
    insn(ADDI, R32, R0, 0),
    insn(ADDI, R33, R250, 0),
    insn(LDOU, R34, R1, R0),
    insn(SUBU, R35, R34, R1),
    halt(),
))


HOSTED_ELF_TESTS = [
    MMIXELFTest(
        "elf-arguments-explicit",
        elf64_image(0, EXPLICIT_ARGUMENTS[0]),
        pc=EXPLICIT_ARGUMENTS[1],
        regs=EXPLICIT_ARGUMENTS[2],
        qemu_args=(
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=prog,arg=one,arg=two",
        ),
    ),
    MMIXELFTest(
        "elf-arguments-fallback",
        elf64_image(0, FALLBACK_ARGUMENTS[0]),
        pc=FALLBACK_ARGUMENTS[1],
        regs=FALLBACK_ARGUMENTS[2],
        qemu_args=(
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting",
            "-append",
            "one two",
        ),
    ),
    MMIXELFTest(
        "elf-arguments-empty",
        elf64_image(0, EMPTY_ARGUMENT[0]),
        pc=EMPTY_ARGUMENT[1],
        regs=EMPTY_ARGUMENT[2],
        qemu_args=(
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=",
        ),
    ),
    MMIXELFTest(
        "elf-arguments-large",
        elf64_image(0, LARGE_ARGUMENT_PROBE[0]),
        pc=LARGE_ARGUMENT_PROBE[1],
        regs=LARGE_ARGUMENT_PROBE[2],
        qemu_args=(
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            f"enable=on,arg={LARGE_ARGUMENT}",
        ),
    ),
    MMIXELFTest(
        "elf-arguments-with-global-registers",
        elf64_image_with_reg_contents(
            0,
            GLOBAL_ARGUMENT_PROGRAM,
            250,
            (0x1122334455667788,),
        ),
        pc=len(GLOBAL_ARGUMENT_PROGRAM) - 4,
        regs={
            R32: 1,
            R33: 0x1122334455667788,
            R35: 16,
        },
        qemu_args=(
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=prog",
        ),
    ),
]


HOSTED_ELF_REJECTION_TESTS = [
    MMIXProcessFailure(
        "elf-arguments-semihosting-disabled",
        elf64_image(0, halt()),
        ("-machine", "elf-startup-abi=argc-argv"),
        ("startup ABI 'argc-argv' requires semihosting",),
    ),
    MMIXProcessFailure(
        "elf-arguments-smp",
        elf64_image(0, halt()),
        (
            "-smp",
            "2",
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=prog",
        ),
        ("startup ABI 'argc-argv' requires exactly one CPU",),
    ),
    MMIXProcessFailure(
        "elf-arguments-explicit-with-append",
        elf64_image(0, halt()),
        (
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=prog",
            "-append",
            "one",
        ),
        ("does not allow explicit semihosting arguments with -append",),
    ),
    MMIXProcessFailure(
        "elf-arguments-initrd",
        elf64_image(0, halt()),
        (
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=prog",
            "-initrd",
            "$IMAGE",
        ),
        ("startup ABI 'argc-argv' does not accept -initrd",),
    ),
    MMIXProcessFailure(
        "elf-arguments-no-free-page",
        elf64_image(
            0,
            halt(),
            mem_size=128 * 1024 * 1024 - 3 * 1024 * 1024 - 32 * 1024,
        ),
        (
            "-m",
            "128M",
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=prog",
        ),
        ("MMIX RAM reservation 'mmix-semihosting/arguments' does not fit",),
    ),
]
