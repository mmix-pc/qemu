#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses

from .common import *
from .smp import SMPProgram, smp_load, smp_store, smp_sync


LINUX_MACHINE = ("-machine", "elf-startup-abi=linux")


@dataclasses.dataclass(frozen=True)
class MMIXLinuxEntryStateTest:
    name: str
    image: bytes
    entry: int
    cpu_count: int
    qemu_args: tuple[str, ...]
    minimum_fdt: int = 0


@dataclasses.dataclass(frozen=True)
class MMIXLinuxSMPEntryTest:
    name: str
    image: bytes
    success_pc: int
    qemu_args: tuple[str, ...]


LINUX_ENTRY_STATE_TESTS = [
    MMIXLinuxEntryStateTest(
        "elf-linux-one-cpu",
        elf64_image(0, jump(JMP, 0)),
        0,
        1,
        LINUX_MACHINE,
    ),
    MMIXLinuxEntryStateTest(
        "elf-linux-64-cpus",
        elf64_image(0, jump(JMP, 0)),
        0,
        64,
        ("-smp", "64", *LINUX_MACHINE),
    ),
    MMIXLinuxEntryStateTest(
        "elf-linux-above-4g",
        elf64_image(0x100000000, jump(JMP, 0), entry=0x100000000),
        0x100000000,
        1,
        ("-m", "8G", *LINUX_MACHINE),
        minimum_fdt=0x100000000,
    ),
    MMIXLinuxEntryStateTest(
        "elf-linux-command-line-limit",
        elf64_image(0, jump(JMP, 0)),
        0,
        1,
        (*LINUX_MACHINE, "-append", "x" * 4095),
    ),
    MMIXLinuxEntryStateTest(
        "elf-linux-initrd",
        elf64_image(0, jump(JMP, 0)),
        0,
        1,
        (*LINUX_MACHINE, "-initrd", "$IMAGE"),
    ),
    MMIXLinuxEntryStateTest(
        "elf-linux-semihosting",
        elf64_image(0, jump(JMP, 0)),
        0,
        1,
        (*LINUX_MACHINE, "-semihosting"),
    ),
]


LINUX_SMP_MAILBOX_BASE = 0x00200000
LINUX_SMP_MAILBOX_STRIDE = 0x80
LINUX_SMP_CPU_ID = 0x00
LINUX_SMP_FDT = 0x08
LINUX_SMP_ENTRY = 0x10
LINUX_SMP_RO = 0x18
LINUX_SMP_RS = 0x20
LINUX_SMP_READY = 0x28
LINUX_SMP_RELEASE = 0x30
LINUX_SMP_DONE = 0x38
LINUX_SMP_IPI_STATUS = 0x40


def linux_smp_entry_program():
    entry = 0x1000
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        insn(ADDI, R33, R1, 0),
        insn(GET, R34, 0, SR_O),
        insn(GET, R35, 0, SR_S),
        *set_octa(R36, entry),
        *set_octa(R40, LINUX_SMP_MAILBOX_BASE),
        insn(SLUI, R41, R32, 7),
        insn(ADDU, R40, R40, R41),
        smp_store(R32, R40, LINUX_SMP_CPU_ID),
        smp_store(R33, R40, LINUX_SMP_FDT),
        smp_store(R36, R40, LINUX_SMP_ENTRY),
        smp_store(R34, R40, LINUX_SMP_RO),
        smp_store(R35, R40, LINUX_SMP_RS),
    )
    program.emit_branch(BZ, R32, "boot_cpu")

    program.emit(
        wyde(SETL, R42, 1),
        smp_sync(1),
        smp_store(R42, R40, LINUX_SMP_READY),
        *set_octa(
            R60,
            MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0]
            + MMIX_VIRT_IPI_CONTEXT_BASE
            + MMIX_VIRT_IPI_CONTEXT_STRIDE,
        ),
    )
    program.mark("wait_for_ipi")
    program.emit(insn(LDOUI, R43, R60, MMIX_VIRT_IPI_CONTEXT_STATUS))
    program.emit_branch(BZ, R43, "wait_for_ipi")
    program.emit(
        smp_store(R43, R40, LINUX_SMP_IPI_STATUS),
        insn(STOUI, R42, R60, MMIX_VIRT_IPI_CONTEXT_CLEAR),
        *set_octa(R44, LINUX_SMP_MAILBOX_BASE),
    )
    program.mark("wait_for_release")
    program.emit(smp_load(R45, R44, LINUX_SMP_RELEASE))
    program.emit_branch(BZ, R45, "wait_for_release")
    program.emit(
        smp_sync(2),
        smp_store(R42, R40, LINUX_SMP_DONE),
    )
    program.mark("secondary_idle")
    program.emit_branch(BZ, R254, "secondary_idle")

    program.mark("boot_cpu")
    program.emit(*set_octa(R49, LINUX_SMP_MAILBOX_BASE +
                           LINUX_SMP_MAILBOX_STRIDE))
    program.mark("wait_for_secondary")
    program.emit(smp_load(R44, R49, LINUX_SMP_READY))
    program.emit_branch(BZ, R44, "wait_for_secondary")
    program.emit(
        wyde(SETL, R42, 1),
        smp_store(R42, R40, LINUX_SMP_RELEASE),
        smp_sync(),
        *set_octa(R60, MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0] +
                  MMIX_VIRT_IPI_SEND),
        wyde(SETL, R61, 2),
        insn(STOUI, R61, R60, 0),
    )
    program.mark("wait_for_done")
    program.emit(smp_load(R44, R49, LINUX_SMP_DONE))
    program.emit_branch(BZ, R44, "wait_for_done")
    program.emit(
        smp_sync(2),
        smp_load(R50, R49, LINUX_SMP_CPU_ID),
        smp_load(R51, R49, LINUX_SMP_FDT),
        smp_load(R52, R49, LINUX_SMP_ENTRY),
        smp_load(R53, R49, LINUX_SMP_RO),
        smp_load(R54, R49, LINUX_SMP_RS),
        smp_load(R55, R49, LINUX_SMP_DONE),
        smp_load(R56, R49, LINUX_SMP_IPI_STATUS),
        wyde(SETL, R46, 1),
    )
    program.mark("success_halt")
    program.emit(halt())

    return MMIXLinuxSMPEntryTest(
        name="elf-linux-smp-entry-barrier",
        image=elf64_image(entry, program.build(), entry=entry),
        success_pc=program.address("success_halt", base=entry),
        qemu_args=(
            "-smp", "2",
            "-accel", "tcg,thread=multi",
            *LINUX_MACHINE,
        ),
    )


LINUX_SMP_ENTRY_TESTS = [linux_smp_entry_program()]


LINUX_PREFLIGHT_REJECTION_TESTS = [
    MMIXProcessFailure(
        "elf-invalid-startup-abi",
        elf64_image(0, halt()),
        ("-machine", "elf-startup-abi=invalid"),
        (
            "Invalid MMIX ELF startup ABI 'invalid'",
            "Valid values are bare, argc-argv, and linux",
        ),
    ),
    MMIXProcessFailure(
        "raw-linux-startup-abi",
        bytes(0x104),
        LINUX_MACHINE,
        ("raw -kernel loading does not support ELF startup ABI 'linux'",),
    ),
    MMIXProcessFailure(
        "elf-linux-maxcpus",
        elf64_image(0, halt()),
        (
            "-smp",
            "cpus=1,maxcpus=2,sockets=1,cores=2,threads=1",
            *LINUX_MACHINE,
        ),
        ("requires maxcpus to equal the active CPU count",),
    ),
    MMIXProcessFailure(
        "elf-linux-multiple-sockets",
        elf64_image(0, halt()),
        (
            "-smp",
            "cpus=2,sockets=2,cores=1,threads=1",
            *LINUX_MACHINE,
        ),
        ("requires one socket with one single-threaded core per CPU",),
    ),
    MMIXProcessFailure(
        "elf-linux-hardware-threads",
        elf64_image(0, halt()),
        (
            "-smp",
            "cpus=2,sockets=1,cores=1,threads=2",
            *LINUX_MACHINE,
        ),
        ("requires one socket with one single-threaded core per CPU",),
    ),
    MMIXProcessFailure(
        "elf-linux-semihosting-arguments",
        elf64_image(0, halt()),
        (
            *LINUX_MACHINE,
            "-semihosting-config",
            "enable=on,arg=kernel",
        ),
        ("Linux direct boot does not accept semihosting arguments",),
    ),
    MMIXProcessFailure(
        "elf-linux-command-line-too-long",
        elf64_image(0, halt()),
        (*LINUX_MACHINE, "-append", "x" * 4096),
        ("Linux command line exceeds 4095 bytes",),
    ),
    MMIXProcessFailure(
        "elf-linux-missing-initrd",
        elf64_image(0, halt()),
        (*LINUX_MACHINE, "-initrd", "$MISSING"),
        ("Could not open",),
    ),
    MMIXProcessFailure(
        "elf-linux-empty-initrd",
        elf64_image(0, halt()),
        (*LINUX_MACHINE, "-initrd", "$EMPTY"),
        ("Linux initrd", "is empty"),
    ),
    MMIXProcessFailure(
        "elf-linux-initrd-no-free-page",
        elf64_image(
            0,
            halt(),
            mem_size=128 * 1024 * 1024 - 3 * 1024 * 1024 - 32 * 1024,
        ),
        ("-m", "128M", *LINUX_MACHINE, "-initrd", "$IMAGE"),
        ("MMIX RAM reservation 'mmix-kernel/initrd' does not fit",),
    ),
    MMIXProcessFailure(
        "elf-linux-fdt-no-free-page",
        elf64_image(
            0,
            halt(),
            mem_size=128 * 1024 * 1024 - 3 * 1024 * 1024 - 32 * 1024,
        ),
        ("-m", "128M", *LINUX_MACHINE),
        ("MMIX RAM reservation 'mmix-fdt/blob' does not fit",),
    ),
]
