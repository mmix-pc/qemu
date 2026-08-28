#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *


def bootinfo_probe_program():
    bootinfo = expected_bootinfo()
    fields = {
        index + R4: field
        for index, field in enumerate(MMIX_BOOTINFO_FIELDS)
    }
    program = [
        insn(ADDI, R2, R0, 0),
        insn(ADDI, R3, R1, 0),
    ]

    for reg, field in fields.items():
        offset = MMIX_BOOTINFO_FIELDS.index(field) * 8
        program.extend([
            wyde(SETL, R50, offset),
            insn(LDOU, reg, R1, R50),
        ])

    program.append(halt())
    regs = {
        R2: 0,
        R3: MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0],
        **{reg: bootinfo[field] for reg, field in fields.items()},
    }
    return b"".join(program), (len(program) - 1) * 4, regs


BOOTINFO_PROBE = bootinfo_probe_program()


def argc_argv_probe_program():
    program = [
        insn(ADDI, R32, R0, 0),
        insn(ADDI, R33, R1, 0),
        insn(GET, R34, 0, SR_L),
        *set_octa(R35, MMIX_POOL_SEGMENT_BASE),
        insn(LDOU, R36, R35, R250),
        insn(LDOUI, R37, R35, 8),
        insn(LDOUI, R38, R35, 16),
        insn(LDBUI, R39, R37, 0),
        insn(LDBUI, R40, R37, 4),
        halt(),
    ]
    regs = {
        R32: 1,
        R33: MMIX_POOL_SEGMENT_BASE + 8,
        R34: 2,
        R36: MMIX_POOL_SEGMENT_BASE + 0x20,
        R37: MMIX_POOL_SEGMENT_BASE + 0x18,
        R38: 0,
        R39: ord("p"),
        R40: 0,
    }
    return b"".join(program), (len(program) - 1) * 4, regs


ARGC_ARGV_PROBE = argc_argv_probe_program()


def explicit_argc_argv_probe_program():
    program = [
        insn(ADDI, R32, R0, 0),
        insn(ADDI, R33, R1, 0),
        insn(GET, R34, 0, SR_L),
        *set_octa(R35, MMIX_POOL_SEGMENT_BASE),
        insn(LDOU, R36, R35, R250),
        insn(LDOUI, R37, R35, 8),
        insn(LDOUI, R38, R35, 16),
        insn(LDOUI, R39, R35, 24),
        insn(LDOUI, R40, R35, 32),
        insn(LDBUI, R41, R37, 0),
        insn(LDBUI, R42, R37, 4),
        insn(LDBUI, R43, R38, 0),
        insn(LDBUI, R44, R38, 3),
        insn(LDBUI, R45, R39, 0),
        insn(LDBUI, R46, R39, 3),
        halt(),
    ]
    regs = {
        R32: 3,
        R33: MMIX_POOL_SEGMENT_BASE + 8,
        R34: 2,
        R36: MMIX_POOL_SEGMENT_BASE + 0x40,
        R37: MMIX_POOL_SEGMENT_BASE + 0x28,
        R38: MMIX_POOL_SEGMENT_BASE + 0x30,
        R39: MMIX_POOL_SEGMENT_BASE + 0x38,
        R40: 0,
        R41: ord("p"),
        R42: 0,
        R43: ord("o"),
        R44: 0,
        R45: ord("t"),
        R46: 0,
    }
    return b"".join(program), (len(program) - 1) * 4, regs


EXPLICIT_ARGC_ARGV_PROBE = explicit_argc_argv_probe_program()


def fallback_argc_argv_probe_program():
    program = [
        insn(ADDI, R32, R0, 0),
        insn(ADDI, R33, R1, 0),
        insn(GET, R34, 0, SR_L),
        *set_octa(R35, MMIX_POOL_SEGMENT_BASE),
        insn(LDOUI, R36, R35, 8),
        insn(LDOUI, R37, R35, 16),
        insn(LDOUI, R38, R35, 24),
        insn(LDOUI, R39, R35, 32),
        insn(LDBUI, R40, R36, 0),
        insn(CMPUI, R40, R40, 0),
        insn(LDBUI, R41, R37, 0),
        insn(LDBUI, R42, R37, 3),
        insn(LDBUI, R43, R38, 0),
        insn(LDBUI, R44, R38, 3),
        halt(),
    ]
    regs = {
        R32: 3,
        R33: MMIX_POOL_SEGMENT_BASE + 8,
        R34: 2,
        R36: MMIX_POOL_SEGMENT_BASE + 0x28,
        R39: 0,
        R40: 1,
        R41: ord("o"),
        R42: 0,
        R43: ord("t"),
        R44: 0,
    }
    return b"".join(program), (len(program) - 1) * 4, regs


FALLBACK_ARGC_ARGV_PROBE = fallback_argc_argv_probe_program()


def platform_probe_program():
    entry = 0x1000
    kernel_stack_top = 0x00500000
    kernel_stack_value = 0x1122334455667788
    segment_stack_value = 0x8877665544332211

    def bootinfo_load(reg, field):
        offset = MMIX_BOOTINFO_FIELDS.index(field) * 8
        return insn(LDOUI, reg, R1, offset)

    program = [
        # Preserve the QEMU-provided ELF entry state before setting up a stack.
        insn(ADDI, R20, R0, 0),
        insn(ADDI, R21, R1, 0),
        insn(ADDI, R22, R254, 0),
        *set_octa(R254, kernel_stack_top),
        insn(SUBUI, R254, R254, 8),
        *set_octa(R23, kernel_stack_value),
        insn(STOU, R23, R254, R0),
        insn(LDOU, R24, R254, R0),

        # Discover the logical Stack Segment and exercise one bounded octa.
        bootinfo_load(R25, "stack_logical_base"),
        bootinfo_load(R26, "stack_size"),
        wyde(SETL, R46, 0x100),
        insn(ADDU, R27, R25, R46),
        *set_octa(R28, segment_stack_value),
        insn(STOU, R28, R27, R0),
        insn(LDOU, R29, R27, R0),

        # Read the early platform-discovery fields used by an OS kernel.
        bootinfo_load(R30, "magic"),
        bootinfo_load(R31, "cpu_count"),
        bootinfo_load(R32, "boot_cpu_id"),
        bootinfo_load(R33, "low_ram_base"),
        bootinfo_load(R34, "low_ram_size"),
        bootinfo_load(R35, "uart_base"),
        bootinfo_load(R36, "timer_base"),
        bootinfo_load(R37, "intc_base"),
        bootinfo_load(R38, "timer_irq_base"),
        bootinfo_load(R39, "intc_irq_count"),

        # Inspect deterministic CPU0 timer and interrupt-controller state.
        wyde(SETL, R45, MMIX_VIRT_TIMER_CONTEXT_BASE),
        insn(LDOU, R40, R36, R45),
        insn(ADDUI, R45, R45, MMIX_VIRT_TIMER_CONTEXT_STATUS),
        insn(LDOU, R41, R36, R45),
        insn(LDTUI, R42, R37, MMIX_VIRT_INTC_PENDING),

        # Emit one polling-UART byte through the discovered 16550 base.
        insn(LDBUI, R43, R35, MMIX_VIRT_UART0_LSR),
        insn(ANDI, R43, R43, MMIX_VIRT_UART0_LSR_THRE),
        branch(BZB, R43, 0xfffe),
        wyde(SETL, R44, ord("P")),
        insn(STBI, R44, R35, MMIX_VIRT_UART0_THR),
        halt(),
    ]
    regs = {
        R20: 0,
        R21: MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0],
        R22: 0,
        R24: kernel_stack_value,
        R25: MMIX_STACK_SEGMENT_BASE,
        R26: MMIX_STACK_SEGMENT_SIZE,
        R27: MMIX_STACK_SEGMENT_BASE + 0x100,
        R29: segment_stack_value,
        R30: MMIX_BOOTINFO_MAGIC,
        R31: 1,
        R32: 0,
        R33: MMIX_LOW_RAM_BASE,
        R34: MMIX_LOW_RAM_SIZE,
        R35: MMIX_VIRT_UART0_BASE,
        R36: MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0],
        R37: MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0],
        R38: MMIX_VIRT_TIMER_IRQ_BASE,
        R39: MMIX_VIRT_INTC_IRQ_COUNT,
        R40: 0,
        R41: 0,
        R42: 0,
    }
    return b"".join(program), entry + (len(program) - 1) * 4, regs


PLATFORM_PROBE = platform_probe_program()


KERNEL_CMDLINE = b"console=ttyS0 root=/dev/vda"


def kernel_cmdline_probe_program():
    bootinfo_base = MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0]
    platform_base, platform_size = MMIX_VIRT_MEMMAP[MMIX_VIRT_PLATFORM_RAM]
    cmdline_address = MMIX_VIRT_MEMMAP[MMIX_VIRT_KERNEL_CMDLINE][0]

    def bootinfo_load(reg, field):
        offset = MMIX_BOOTINFO_FIELDS.index(field) * 8
        return [
            wyde(SETL, R50, offset),
            insn(LDOU, reg, R1, R50),
        ]

    program = [
        insn(ADDI, R20, R1, 0),
        *bootinfo_load(R21, "flags"),
        *bootinfo_load(R22, "kernel_cmdline_addr"),
        *bootinfo_load(R23, "kernel_cmdline_size"),
        wyde(SETL, R25, 0),
    ]

    for offset, byte in enumerate(KERNEL_CMDLINE):
        program.extend([
            insn(LDBUI, R26, R22, offset),
            insn(XORI, R26, R26, byte),
            insn(OR, R25, R25, R26),
        ])

    program.extend([
        insn(LDBUI, R24, R22, len(KERNEL_CMDLINE)),
        insn(OR, R25, R25, R24),
        insn(SUBU, R27, R22, R20),
        *set_octa(R29, platform_base + platform_size),
        insn(SUBU, R28, R29, R22),
        halt(),
    ])

    regs = {
        R20: bootinfo_base,
        R21: MMIX_BOOTINFO_FLAG_KERNEL_CMDLINE,
        R22: cmdline_address,
        R23: len(KERNEL_CMDLINE),
        R24: 0,
        R25: 0,
        R27: MMIX_BOOTINFO_SIZE,
        R28: platform_base + platform_size - cmdline_address,
    }
    return b"".join(program), (len(program) - 1) * 4, regs


KERNEL_CMDLINE_PROBE = kernel_cmdline_probe_program()


ELF_GLOBAL_BASE = 250
ELF_GLOBAL_VALUES = (
    0x1122334455667788,
    0x8877665544332211,
    0x0123456789abcdef,
    0xfedcba9876543210,
    0xa5a55a5af0f00f0f,
)


def register_contents_probe_program(startup_r0=0, startup_r1=None):
    if startup_r1 is None:
        startup_r1 = MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0]

    program = [
        insn(ADDI, R20, R250, 0),
        insn(ADDI, R21, R251, 0),
        insn(ADDI, R22, R252, 0),
        insn(ADDI, R23, R253, 0),
        insn(ADDI, R24, R254, 0),
        insn(GET, R25, 0, SR_G),
        insn(ADDI, R26, R0, 0),
        insn(ADDI, R27, R1, 0),
        halt(),
    ]
    regs = {
        R20: ELF_GLOBAL_VALUES[0],
        R21: ELF_GLOBAL_VALUES[1],
        R22: ELF_GLOBAL_VALUES[2],
        R23: ELF_GLOBAL_VALUES[3],
        R24: ELF_GLOBAL_VALUES[4],
        R25: ELF_GLOBAL_BASE,
        R26: startup_r0,
        R27: startup_r1,
    }
    return b"".join(program), (len(program) - 1) * 4, regs


REGISTER_CONTENTS_PROBE = register_contents_probe_program()
REGISTER_CONTENTS_ARGC_ARGV_PROBE = register_contents_probe_program(
    1, MMIX_POOL_SEGMENT_BASE + 8
)


ELF_LOADER_TESTS = [
    MMIXELFTest(
        "elf-load-low-ram-segment",
        elf64_image(
            0,
            b"".join([
                wyde(SETL, R1, 0x42),
                halt(),
            ]),
        ),
        pc=0x04,
        regs={R1: 0x42},
    ),
    MMIXELFTest(
        "elf-load-bss-zero-fill",
        elf64_image(
            0,
            b"".join([
                *set_octa(R1, 0x100),
                insn(LDOU, R2, R1, R0),
                halt(),
            ]),
            mem_size=0x108,
        ),
        pc=0x14,
        regs={R2: 0},
    ),
    MMIXELFTest(
        "elf-entry-state",
        elf64_image(
            0x100,
            b"".join([
                insn(ADDI, R2, R0, 0),
                insn(ADDI, R3, R1, 0),
                wyde(SETL, R4, 0x7e),
                halt(),
            ]),
            entry=0x100,
        ),
        pc=0x10c,
        regs={R2: 0, R3: MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0], R4: 0x7e},
        qemu_args=("-machine", "elf-startup-abi=bootinfo"),
    ),
    MMIXELFTest(
        "elf-argc-argv-entry-state",
        elf64_image(
            0,
            ARGC_ARGV_PROBE[0],
        ),
        pc=ARGC_ARGV_PROBE[1],
        regs=ARGC_ARGV_PROBE[2],
        qemu_args=(
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=prog",
        ),
    ),
    MMIXELFTest(
        "elf-argc-argv-multiple-explicit-arguments",
        elf64_image(
            0,
            EXPLICIT_ARGC_ARGV_PROBE[0],
        ),
        pc=EXPLICIT_ARGC_ARGV_PROBE[1],
        regs=EXPLICIT_ARGC_ARGV_PROBE[2],
        qemu_args=(
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=prog,arg=one,arg=two",
        ),
    ),
    MMIXELFTest(
        "elf-argc-argv-kernel-append-fallback",
        elf64_image(
            0,
            FALLBACK_ARGC_ARGV_PROBE[0],
        ),
        pc=FALLBACK_ARGC_ARGV_PROBE[1],
        regs=FALLBACK_ARGC_ARGV_PROBE[2],
        qemu_args=(
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting",
            "-append",
            "one two",
        ),
    ),
    MMIXELFTest(
        "elf-bootinfo",
        elf64_image(
            0,
            BOOTINFO_PROBE[0],
        ),
        pc=BOOTINFO_PROBE[1],
        regs=BOOTINFO_PROBE[2],
    ),
    MMIXELFTest(
        "elf-kernel-command-line",
        elf64_image(
            0,
            KERNEL_CMDLINE_PROBE[0],
        ),
        pc=KERNEL_CMDLINE_PROBE[1],
        regs=KERNEL_CMDLINE_PROBE[2],
        qemu_args=("-append", KERNEL_CMDLINE.decode("ascii")),
    ),
    MMIXELFTest(
        "elf-bootinfo-with-semihosting",
        elf64_image(
            0,
            KERNEL_CMDLINE_PROBE[0],
        ),
        pc=KERNEL_CMDLINE_PROBE[1],
        regs=KERNEL_CMDLINE_PROBE[2],
        qemu_args=(
            "-machine",
            "elf-startup-abi=bootinfo",
            "-semihosting-config",
            "enable=on,arg=ignored",
            "-append",
            KERNEL_CMDLINE.decode("ascii"),
        ),
    ),
    MMIXELFTest(
        "elf-register-contents",
        elf64_image_with_reg_contents(
            0,
            REGISTER_CONTENTS_PROBE[0],
            ELF_GLOBAL_BASE,
            ELF_GLOBAL_VALUES,
        ),
        pc=REGISTER_CONTENTS_PROBE[1],
        regs=REGISTER_CONTENTS_PROBE[2],
    ),
    MMIXELFTest(
        "elf-register-contents-with-argc-argv",
        elf64_image_with_reg_contents(
            0,
            REGISTER_CONTENTS_ARGC_ARGV_PROBE[0],
            ELF_GLOBAL_BASE,
            ELF_GLOBAL_VALUES,
        ),
        pc=REGISTER_CONTENTS_ARGC_ARGV_PROBE[1],
        regs=REGISTER_CONTENTS_ARGC_ARGV_PROBE[2],
        qemu_args=(
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=prog",
        ),
    ),
    MMIXELFTest(
        "elf-single-core-platform",
        elf64_image(
            0x1000,
            PLATFORM_PROBE[0],
            entry=0x1000,
        ),
        pc=PLATFORM_PROBE[1],
        regs=PLATFORM_PROBE[2],
        output=b"P",
    ),
]


ELF_STARTUP_PROCESS_FAILURE_TESTS = [
    MMIXProcessFailure(
        "elf-argc-argv-semihosting-disabled",
        elf64_image(0, halt()),
        ("-machine", "elf-startup-abi=argc-argv"),
        ("MMIX ELF startup ABI 'argc-argv' requires semihosting",),
    ),
    MMIXProcessFailure(
        "elf-argc-argv-multicore",
        elf64_image(0, halt()),
        (
            "-smp",
            "2",
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=prog",
        ),
        ("MMIX ELF startup ABI 'argc-argv' supports only one CPU",),
    ),
    MMIXProcessFailure(
        "elf-invalid-startup-abi",
        elf64_image(0, halt()),
        ("-machine", "elf-startup-abi=invalid"),
        (
            "Invalid MMIX ELF startup ABI 'invalid'",
            "Valid values are bootinfo and argc-argv",
        ),
    ),
    MMIXProcessFailure(
        "elf-argc-argv-below-minimum-ram",
        elf64_image(0, halt()),
        (
            "-m",
            "127M",
            "-machine",
            "elf-startup-abi=argc-argv",
            "-semihosting-config",
            "enable=on,arg=prog",
        ),
        ("MMIX virt RAM size 0x7f00000 is below the minimum 0x8000000",),
    ),
]


NON_ELF_STARTUP_ABI_TESTS = [
    MMIXTest(
        "raw-elf-startup-abi-ignored",
        b"".join([
            insn(ADDI, R32, R0, 0),
            insn(ADDI, R33, R1, 0),
            halt(),
        ]),
        pc=0x08,
        regs={R32: 1, R33: MMIX_POOL_SEGMENT_BASE + 8},
        qemu_args=(
            "-machine",
            "elf-startup-abi=bootinfo",
            "-semihosting-config",
            "enable=on,arg=prog",
        ),
    ),
]
