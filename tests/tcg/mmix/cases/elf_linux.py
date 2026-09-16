#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses

from .common import *
from .smp import SMPProgram, smp_load, smp_store, smp_sync


LINUX_MACHINE = ("-machine", "elf-startup=platform")
LINUX_NEGATIVE_ALIAS_BIT = 1 << 63


@dataclasses.dataclass(frozen=True)
class MMIXLinuxEntryStateTest:
    name: str
    image: bytes
    entry: int
    cpu_count: int
    qemu_args: tuple[str, ...]
    minimum_fdt: int = 0
    security_checks: bool = True


@dataclasses.dataclass(frozen=True)
class MMIXLinuxSMPEntryTest:
    name: str
    image: bytes
    success_pc: int
    qemu_args: tuple[str, ...]
    security_checks: bool = True


@dataclasses.dataclass(frozen=True)
class MMIXLinuxStateTest:
    name: str
    image: bytes
    initrd: bytes
    entry: int
    load_address: int
    idle_pcs: tuple[int, ...]
    bss: int
    qemu_args: tuple[str, ...]
    security_checks: bool = False


def linux_direct_alias_image():
    bootstrap_address = 0x1000
    bootstrap_virtual_address = LINUX_NEGATIVE_ALIAS_BIT | bootstrap_address
    kernel_address = 0x2000
    kernel_virtual_address = LINUX_NEGATIVE_ALIAS_BIT | kernel_address
    bootstrap = b"".join((
        *set_octa(R32, kernel_virtual_address),
        insn(GO, R33, R32, R0),
    ))
    kernel = b"".join((wyde(SETL, R34, 0x55), halt()))
    bootstrap_offset = 0x200
    kernel_offset = 0x300
    headers = b"".join((
        elf64_phdr(bootstrap_address, bootstrap, offset=bootstrap_offset,
                   virtual_address=bootstrap_virtual_address),
        elf64_phdr(kernel_address, kernel, offset=kernel_offset,
                   virtual_address=kernel_virtual_address),
    ))
    image = bytearray(elf64_header(entry=bootstrap_virtual_address, phnum=2) +
                      headers)

    image.extend(bytes(bootstrap_offset - len(image)))
    image.extend(bootstrap)
    image.extend(bytes(kernel_offset - len(image)))
    image.extend(kernel)
    return bytes(image)


LINUX_DIRECT_ALIAS_IMAGE = linux_direct_alias_image()
LINUX_DIRECT_ALIAS_ADDRESS = LINUX_NEGATIVE_ALIAS_BIT | 0x2000
LINUX_NEGATIVE_ENTRY_IMAGE = elf64_patch_ehdr_field(
    LINUX_DIRECT_ALIAS_IMAGE, "entry", LINUX_DIRECT_ALIAS_ADDRESS
)


def linux_positive_privileged_put_image():
    kernel_address = 0x3000
    kernel_virtual_address = LINUX_NEGATIVE_ALIAS_BIT | kernel_address
    handler_virtual_address = kernel_virtual_address + 0x100
    user_address = 0x4000
    saved_rc = 0x1234
    kernel = b"".join((
        *set_octa(R32, saved_rc),
        insn(PUT, SR_C, R0, R32),
        *set_octa(R33, handler_virtual_address),
        insn(PUT, SR_TT, R0, R33),
        *set_octa(R35, user_address),
        insn(PUT, SR_W, R0, R35),
        *set_octa(R36, LINUX_NEGATIVE_ALIAS_BIT),
        insn(PUT, SR_X, R0, R36),
        *set_octa(
            R34, RQ_PROGRAM_MASK & ~(RQ_PROGRAM_K | RQ_PROGRAM_P)
        ),
        insn(PUT, SR_K, R0, R34),
        insn(RESUME, R0, R0, 0),
    ))
    handler = b"".join((
        insn(GET, R40, R0, SR_C),
        insn(GET, R41, R0, SR_Q),
        insn(GET, R42, R0, SR_K),
        halt(),
    ))
    kernel += bytes(0x100 - len(kernel)) + handler
    user = b"".join((
        insn(PUTI, SR_C, R0, 0xaa),
        insn(SWYM, R0, R0, R0),
        halt(),
    ))
    kernel_offset = 0x200
    user_offset = 0x400
    headers = b"".join((
        elf64_phdr(kernel_address, kernel, offset=kernel_offset,
                   virtual_address=kernel_virtual_address),
        elf64_phdr(
            user_address, user, offset=user_offset,
            virtual_address=LINUX_NEGATIVE_ALIAS_BIT | user_address,
        ),
    ))
    image = bytearray(elf64_header(entry=kernel_virtual_address, phnum=2) +
                      headers)

    image.extend(bytes(kernel_offset - len(image)))
    image.extend(kernel)
    image.extend(bytes(user_offset - len(image)))
    image.extend(user)
    return bytes(image), handler_virtual_address + 12


def linux_negative_fetch_without_kernel_access_image():
    kernel_address = 0x5000
    kernel_virtual_address = LINUX_NEGATIVE_ALIAS_BIT | kernel_address
    handler_virtual_address = kernel_virtual_address + 0x100
    saved_rc = 0x1234
    kernel = b"".join((
        *set_octa(R32, saved_rc),
        insn(PUT, SR_C, R0, R32),
        *set_octa(R33, handler_virtual_address),
        insn(PUT, SR_TT, R0, R33),
        *set_octa(R34, RQ_PROGRAM_MASK & ~RQ_PROGRAM_P),
        insn(PUT, SR_K, R0, R34),
        insn(PUTI, SR_C, R0, 0xaa),
        halt(),
    ))
    handler = b"".join((
        insn(GET, R40, R0, SR_C),
        insn(GET, R41, R0, SR_Q),
        insn(GET, R42, R0, SR_K),
        halt(),
    ))
    kernel += bytes(0x100 - len(kernel)) + handler

    return (
        elf64_image(
            kernel_address, kernel, entry=kernel_virtual_address,
            offset=0x200, virtual_address=kernel_virtual_address
        ),
        handler_virtual_address + 12,
        saved_rc,
    )


LINUX_POSITIVE_PRIVILEGED_PUT = linux_positive_privileged_put_image()
LINUX_NEGATIVE_FETCH_WITHOUT_KERNEL_ACCESS = (
    linux_negative_fetch_without_kernel_access_image()
)


def linux_privilege_qualification_test(name, fixture, rc, rq):
    image, pc = fixture[:2]

    return MMIXELFTest(
        name,
        image,
        pc=pc,
        regs={
            R40: rc,
            R41: rq,
            R42: 0,
        },
        qemu_args=LINUX_MACHINE,
        security_checks=True,
    )


LINUX_DIRECT_ALIAS_TESTS = [
    MMIXELFTest(
        "elf-linux-negative-direct-alias",
        LINUX_DIRECT_ALIAS_IMAGE,
        pc=LINUX_DIRECT_ALIAS_ADDRESS + 4,
        regs={R34: 0x55},
        qemu_args=LINUX_MACHINE,
    ),
    MMIXELFTest(
        "elf-linux-negative-entry",
        LINUX_NEGATIVE_ENTRY_IMAGE,
        pc=LINUX_DIRECT_ALIAS_ADDRESS + 4,
        regs={R34: 0x55},
        qemu_args=LINUX_MACHINE,
        security_checks=True,
    ),
    linux_privilege_qualification_test(
        "elf-linux-positive-put-k-disabled",
        LINUX_POSITIVE_PRIVILEGED_PUT,
        0xaa,
        RQ_PROGRAM_S,
    ),
    linux_privilege_qualification_test(
        "elf-linux-negative-fetch-k-enabled",
        LINUX_NEGATIVE_FETCH_WITHOUT_KERNEL_ACCESS,
        LINUX_NEGATIVE_FETCH_WITHOUT_KERNEL_ACCESS[2],
        RQ_PROGRAM_N,
    ),
]


LINUX_ENTRY_STATE_TESTS = [
    MMIXLinuxEntryStateTest(
        "elf-linux-one-cpu",
        elf64_image(
            0, jump(JMP, 0), entry=LINUX_NEGATIVE_ALIAS_BIT,
            virtual_address=LINUX_NEGATIVE_ALIAS_BIT,
        ),
        LINUX_NEGATIVE_ALIAS_BIT,
        1,
        LINUX_MACHINE,
    ),
    MMIXLinuxEntryStateTest(
        "elf-linux-64-cpus",
        elf64_image(
            0, jump(JMP, 0), entry=LINUX_NEGATIVE_ALIAS_BIT,
            virtual_address=LINUX_NEGATIVE_ALIAS_BIT,
        ),
        LINUX_NEGATIVE_ALIAS_BIT,
        64,
        ("-smp", "64", *LINUX_MACHINE),
    ),
    MMIXLinuxEntryStateTest(
        "elf-linux-above-4g",
        elf64_image(
            0x100000000, jump(JMP, 0),
            entry=LINUX_NEGATIVE_ALIAS_BIT | 0x100000000,
            virtual_address=LINUX_NEGATIVE_ALIAS_BIT | 0x100000000,
        ),
        LINUX_NEGATIVE_ALIAS_BIT | 0x100000000,
        1,
        ("-m", "8G", *LINUX_MACHINE),
        minimum_fdt=0x100000000,
    ),
    MMIXLinuxEntryStateTest(
        "elf-linux-negative-entry-smp",
        LINUX_NEGATIVE_ENTRY_IMAGE,
        LINUX_DIRECT_ALIAS_ADDRESS,
        2,
        ("-smp", "2", *LINUX_MACHINE),
    ),
    MMIXLinuxEntryStateTest(
        "elf-linux-command-line-limit",
        elf64_image(
            0, jump(JMP, 0), entry=LINUX_NEGATIVE_ALIAS_BIT,
            virtual_address=LINUX_NEGATIVE_ALIAS_BIT,
        ),
        LINUX_NEGATIVE_ALIAS_BIT,
        1,
        (*LINUX_MACHINE, "-append", "x" * 4095),
    ),
    MMIXLinuxEntryStateTest(
        "elf-linux-initrd",
        elf64_image(
            0, jump(JMP, 0), entry=LINUX_NEGATIVE_ALIAS_BIT,
            virtual_address=LINUX_NEGATIVE_ALIAS_BIT,
        ),
        LINUX_NEGATIVE_ALIAS_BIT,
        1,
        (*LINUX_MACHINE, "-initrd", "$IMAGE"),
    ),
    MMIXLinuxEntryStateTest(
        "elf-linux-semihosting",
        elf64_image(
            0, jump(JMP, 0), entry=LINUX_NEGATIVE_ALIAS_BIT,
            virtual_address=LINUX_NEGATIVE_ALIAS_BIT,
        ),
        LINUX_NEGATIVE_ALIAS_BIT,
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
    load_address = 0x1000
    entry = LINUX_NEGATIVE_ALIAS_BIT | load_address
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
        image=elf64_image(
            load_address, program.build(), entry=entry,
            virtual_address=entry,
        ),
        success_pc=program.address("success_halt", base=entry),
        qemu_args=(
            "-smp", "2",
            "-accel", "tcg,thread=multi",
            *LINUX_MACHINE,
        ),
    )


LINUX_SMP_ENTRY_TESTS = [linux_smp_entry_program()]


def linux_state_program():
    load_address = 0x1000
    entry = LINUX_NEGATIVE_ALIAS_BIT | load_address
    bss = 0x4000
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        insn(ADDI, R33, R32, 0x40),
    )
    program.emit_branch(BZ, R32, "boot_idle")
    program.mark("secondary_idle")
    program.emit_branch(BZ, R254, "secondary_idle")
    program.mark("boot_idle")
    program.emit_branch(BZ, R254, "boot_idle")

    return MMIXLinuxStateTest(
        name="elf-linux-reset-and-snapshot-state",
        image=elf64_image(load_address, program.build(), entry=entry,
                          mem_size=bss - load_address + 8,
                          virtual_address=entry),
        initrd=b"MMIX Linux reset and snapshot initrd\n",
        entry=entry,
        load_address=load_address,
        idle_pcs=(
            program.address("boot_idle", base=entry),
            program.address("secondary_idle", base=entry),
        ),
        bss=bss,
        qemu_args=(
            "-smp", "2",
            "-accel", "tcg,thread=multi",
            *LINUX_MACHINE,
            "-initrd", "$INITRD",
        ),
        security_checks=True,
    )


LINUX_STATE_TESTS = [linux_state_program()]


LINUX_PREFLIGHT_REJECTION_TESTS = [
    MMIXProcessFailure(
        "elf-bare-identity-mapping",
        elf64_image(0x2000, halt(), entry=0x2000),
        (),
        ("does not use a negative direct-alias mapping",),
    ),
    MMIXProcessFailure(
        "elf-hosted-negative-direct-alias",
        LINUX_DIRECT_ALIAS_IMAGE,
        (
            "-machine", "elf-startup=hosted",
            "-semihosting-config", "enable=on,userspace=on",
        ),
        ("does not use identical virtual and physical addresses",),
    ),
    MMIXProcessFailure(
        "elf-linux-identity-mapping",
        elf64_image(0x2000, halt(), entry=0x2000),
        LINUX_MACHINE,
        ("does not use a negative direct-alias mapping",),
    ),
    MMIXProcessFailure(
        "elf-linux-arbitrary-virtual-address",
        elf64_patch_phdr_field(
            LINUX_DIRECT_ALIAS_IMAGE, 1, "virtual_address", 0x4000
        ),
        LINUX_MACHINE,
        ("does not use a negative direct-alias mapping",),
    ),
    MMIXProcessFailure(
        "elf-linux-negative-entry-outside-segment",
        elf64_patch_ehdr_field(
            LINUX_DIRECT_ALIAS_IMAGE, "entry",
            LINUX_NEGATIVE_ALIAS_BIT | 0x4000
        ),
        LINUX_MACHINE,
        ("negative direct-alias executable PT_LOAD segment",),
    ),
    MMIXProcessFailure(
        "elf-linux-negative-entry-unaligned",
        elf64_patch_ehdr_field(
            LINUX_DIRECT_ALIAS_IMAGE, "entry", LINUX_DIRECT_ALIAS_ADDRESS + 2
        ),
        LINUX_MACHINE,
        ("negative direct-alias executable PT_LOAD segment",),
    ),
    MMIXProcessFailure(
        "elf-retired-bootinfo-startup-abi",
        elf64_image(0, halt()),
        ("-machine", "elf-startup=bootinfo"),
        (
            "Invalid MMIX ELF startup profile 'bootinfo'",
            "Valid values are platform and hosted",
        ),
    ),
    MMIXProcessFailure(
        "elf-invalid-startup-abi",
        elf64_image(0, halt()),
        ("-machine", "elf-startup=invalid"),
        (
            "Invalid MMIX ELF startup profile 'invalid'",
            "Valid values are platform and hosted",
        ),
    ),
    MMIXProcessFailure(
        "raw-hosted-startup",
        bytes(0x104),
        ("-machine", "elf-startup=hosted"),
        ("raw -kernel loading does not support ELF startup profile "
         "'hosted'",),
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
        ("platform ELF startup does not accept semihosting arguments",),
    ),
    MMIXProcessFailure(
        "elf-linux-command-line-too-long",
        elf64_image(0, halt()),
        (*LINUX_MACHINE, "-append", "x" * 4096),
        ("platform command line exceeds 4095 bytes",),
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
        ("platform initrd", "is empty"),
    ),
    MMIXProcessFailure(
        "elf-linux-initrd-no-free-page",
        elf64_image(
            0,
            halt(),
            mem_size=128 * 1024 * 1024 - 3 * 1024 * 1024 - 32 * 1024,
            entry=LINUX_NEGATIVE_ALIAS_BIT,
            virtual_address=LINUX_NEGATIVE_ALIAS_BIT,
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
            entry=LINUX_NEGATIVE_ALIAS_BIT,
            virtual_address=LINUX_NEGATIVE_ALIAS_BIT,
        ),
        ("-m", "128M", *LINUX_MACHINE),
        ("MMIX RAM reservation 'mmix-fdt/blob' does not fit",),
    ),
]
