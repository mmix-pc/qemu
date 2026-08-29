#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import MMIXProcessFailure, elf64_image, halt


LINUX_MACHINE = ("-machine", "elf-startup-abi=linux")
FDT_REQUIRED = ("preflight completed; execution requires FDT support",)


LINUX_PREFLIGHT_TESTS = [
    MMIXProcessFailure(
        "elf-linux-one-cpu",
        elf64_image(0, halt()),
        LINUX_MACHINE,
        FDT_REQUIRED,
    ),
    MMIXProcessFailure(
        "elf-linux-64-cpus",
        elf64_image(0, halt()),
        ("-smp", "64", *LINUX_MACHINE),
        FDT_REQUIRED,
    ),
    MMIXProcessFailure(
        "elf-linux-above-4g",
        elf64_image(0x100000000, halt(), entry=0x100000000),
        ("-m", "8G", *LINUX_MACHINE),
        FDT_REQUIRED,
    ),
    MMIXProcessFailure(
        "elf-linux-command-line-limit",
        elf64_image(0, halt()),
        (*LINUX_MACHINE, "-append", "x" * 4095),
        FDT_REQUIRED,
    ),
    MMIXProcessFailure(
        "elf-linux-initrd",
        elf64_image(0, halt()),
        (*LINUX_MACHINE, "-initrd", "$IMAGE"),
        FDT_REQUIRED,
    ),
    MMIXProcessFailure(
        "elf-linux-semihosting",
        elf64_image(0, halt()),
        (*LINUX_MACHINE, "-semihosting"),
        FDT_REQUIRED,
    ),
]


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
]
