#!/usr/bin/env python3
#
# Shared MMIX softmmu test case helpers and data
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses
import struct
from typing import Optional

from lib.mmix_asm import *
from lib.mmo import (
    MMIX_MMO_LOP_END,
    MMIX_MMO_LOP_FIXO,
    MMIX_MMO_LOP_FIXR,
    MMIX_MMO_LOP_FIXRX,
    MMIX_MMO_LOP_POST,
    MMIX_MMO_LOP_QUOTE,
    MMIX_MMO_LOP_SPEC,
    MMIX_MMO_LOP_STAB,
    mmo_file,
    mmo_fixo,
    mmo_fixr,
    mmo_fixrx,
    mmo_image,
    mmo_line,
    mmo_loc,
    mmo_lop,
    mmo_post,
    mmo_quote,
    mmo_skip,
    mmo_spec,
    mmo_stab_end,
)

MASK64 = (1 << 64) - 1
INITIAL_STACK = 0x00010000
MMIX_VIRT_INITIAL_STACK_SLOT_SIZE = 0x00008000
RA_EVENT_X = 0x01
RA_EVENT_Z = 0x02
RA_EVENT_U = 0x04
RA_EVENT_O = 0x08
RA_EVENT_I = 0x10
RA_EVENT_V = 0x40
RA_EVENT_D = 0x80
RA_ENABLE_SHIFT = 8
RQ_PROGRAM_K = 1 << 35
RQ_PROGRAM_B = 1 << 34
RQ_PROGRAM_R = 1 << 39
RQ_PROGRAM_W = 1 << 38
RQ_PROGRAM_X = 1 << 37
RQ_PROGRAM_N = 1 << 36
RQ_INTERRUPT_CONTROLLER = 1 << 8
RK_INTERRUPT_CONTROLLER = RQ_INTERRUPT_CONTROLLER
RQ_IPI = 1 << 9
RK_IPI = RQ_IPI
DYNAMIC_TRAP_RESUME_NEXT = 1 << 63
VM_PAGE_TABLE = 0x2000
VM_RV_PAGE0 = 0x11110d0000002000
VM_RV_SOFTWARE = VM_RV_PAGE0 | 1
VM_PAGE_TABLE_ROOT2 = 0x4000
VM_RV_ROOT2 = 0x11110d0000004000
NEGATIVE_HANDLER = 0x8000000000000080
MMIX_VIRT_LOW_RAM = "low_ram"
MMIX_VIRT_POOL = "pool"
MMIX_VIRT_DATA = "data"
MMIX_VIRT_STACK = "stack"
MMIX_VIRT_PLATFORM_RAM = "platform_ram"
MMIX_VIRT_BOOTINFO = "bootinfo"
MMIX_VIRT_KERNEL_CMDLINE = "kernel_cmdline"
MMIX_VIRT_FRAMEBUFFER = "framebuffer"
MMIX_VIRT_MMIO = "mmio"
MMIX_VIRT_UART0 = "uart0"
MMIX_VIRT_VIRTIO_MMIO = "virtio_mmio"
MMIX_VIRT_FRAMEBUFFER_CONTROL = "framebuffer_control"
MMIX_VIRT_TIMER = "timer"
MMIX_VIRT_INTC = "intc"
MMIX_VIRT_IPI = "ipi"
MMIX_VIRT_INTC_CONTEXT_COUNT = 16
MMIX_VIRT_INTC_CONTEXT_BASE = 0x1000
MMIX_VIRT_INTC_CONTEXT_STRIDE = 0x100
MMIX_VIRT_INTC_SIZE = (
    MMIX_VIRT_INTC_CONTEXT_BASE +
    MMIX_VIRT_INTC_CONTEXT_COUNT * MMIX_VIRT_INTC_CONTEXT_STRIDE
)
MMIX_VIRT_MEMMAP = {
    MMIX_VIRT_LOW_RAM: (0x00000000, 0x06000000),
    MMIX_VIRT_POOL: (0x06000000, 0x00800000),
    MMIX_VIRT_DATA: (0x06800000, 0x04000000),
    MMIX_VIRT_STACK: (0x0A800000, 0x04000000),
    MMIX_VIRT_PLATFORM_RAM: (0x0E800000, 0x00800000),
    MMIX_VIRT_BOOTINFO: (0x0E800000, 0),
    MMIX_VIRT_KERNEL_CMDLINE: (0, 0x1000),
    MMIX_VIRT_FRAMEBUFFER: (0x0F000000, 0x01000000),
    MMIX_VIRT_MMIO: (0x10000000, 0x10000000),
    MMIX_VIRT_UART0: (0x10000000, 0x100),
    MMIX_VIRT_VIRTIO_MMIO: (0x10001000, 0x1000),
    MMIX_VIRT_FRAMEBUFFER_CONTROL: (0x10002000, 0x1000),
    MMIX_VIRT_TIMER: (0x10003000, 0),
    MMIX_VIRT_INTC: (0x10004000, MMIX_VIRT_INTC_SIZE),
    MMIX_VIRT_IPI: (0x10006000, 0x1000),
}
MMIX_VIRT_SHARED_IRQ_FIRST = 1
MMIX_VIRT_SHARED_IRQ_LAST = 15
MMIX_VIRT_UART0_BASE = MMIX_VIRT_MEMMAP[MMIX_VIRT_UART0][0]
MMIX_VIRT_UART0_THR = 0x00
MMIX_VIRT_UART0_LSR = 0x05
MMIX_VIRT_UART0_LSR_THRE = 0x20
MMIX_VIRT_UART0_IRQ = 1
MMIX_VIRT_VIRTIO_BLOCK0_IRQ = 2
MMIX_VIRT_VIRTIO_MMIO_COUNT = 1
MMIX_VIRT_FRAMEBUFFER_IRQ = 3
MMIX_VIRT_FRAMEBUFFER_WIDTH = 1024
MMIX_VIRT_FRAMEBUFFER_HEIGHT = 768
MMIX_VIRT_FRAMEBUFFER_BPP = 32
MMIX_VIRT_FRAMEBUFFER_STRIDE = 4096
MMIX_VIRT_FRAMEBUFFER_FORMAT_XRGB8888 = 1
MMIX_VIRT_FRAMEBUFFER_REG_WIDTH = 0x00
MMIX_VIRT_FRAMEBUFFER_REG_HEIGHT = 0x08
MMIX_VIRT_FRAMEBUFFER_REG_STRIDE = 0x10
MMIX_VIRT_FRAMEBUFFER_REG_FORMAT = 0x18
MMIX_VIRT_FRAMEBUFFER_REG_BASE = 0x20
MMIX_VIRT_FRAMEBUFFER_REG_SIZE = 0x28
MMIX_VIRT_FRAMEBUFFER_REG_FLUSH = 0x30
MMIX_VIRT_TIMER_IRQ_BASE = 16
MMIX_VIRT_INTC_IRQ_COUNT = (
    MMIX_VIRT_TIMER_IRQ_BASE + MMIX_VIRT_INTC_CONTEXT_COUNT
)
MMIX_VIRT_INTC_PENDING = 0x0000
MMIX_VIRT_INTC_CONTEXT_ENABLE = 0x00
MMIX_VIRT_INTC_CONTEXT_CLAIM = 0x04
MMIX_VIRT_INTC_CONTEXT_COMPLETE = 0x08
MMIX_VIRT_IPI_ACTIVE_TARGETS = 0x0000
MMIX_VIRT_IPI_SEND = 0x0008
MMIX_VIRT_IPI_CONTEXT_BASE = 0x0100
MMIX_VIRT_IPI_CONTEXT_STRIDE = 0x20
MMIX_VIRT_IPI_CONTEXT_STATUS = 0x00
MMIX_VIRT_IPI_CONTEXT_CLEAR = 0x08
MMIX_VIRT_IPI_STATUS_PENDING = 0x01
MMIX_VIRT_TIMER_TIME = 0x0000
MMIX_VIRT_TIMER_CONTEXT_COUNT = MMIX_VIRT_INTC_CONTEXT_COUNT
MMIX_VIRT_TIMER_CONTEXT_BASE = 0x0100
MMIX_VIRT_TIMER_CONTEXT_STRIDE = 0x40
MMIX_VIRT_TIMER_CONTEXT_COMPARE = 0x00
MMIX_VIRT_TIMER_CONTEXT_CONTROL = 0x08
MMIX_VIRT_TIMER_CONTEXT_STATUS = 0x10
MMIX_VIRT_TIMER_CONTROL_ENABLE = 0x01
MMIX_VIRT_TIMER_CONTROL_IRQ_ENABLE = 0x02
MMIX_VIRT_TIMER_STATUS_PENDING = 0x01
MMIX_VIRT_TIMER_SIZE = 0x1000
MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER] = (
    MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0],
    MMIX_VIRT_TIMER_SIZE,
)
MMIX_BOOTINFO_MAGIC = 0x4D4D4958424F4F54
MMIX_BOOTINFO_VERSION = 1
MMIX_BOOTINFO_FLAG_KERNEL_CMDLINE = 1 << 0
MMIX_BOOTINFO_KERNEL_CMDLINE_MAX = 4095
MMIX_POOL_SEGMENT_BASE = 0x4000000000000000
MMIX_POOL_SEGMENT_PHYS_BASE = MMIX_VIRT_MEMMAP[MMIX_VIRT_POOL][0]
MMIX_POOL_SEGMENT_SIZE = 0x0000000000800000
MMIX_DATA_SEGMENT_BASE = 0x2000000000000000
MMIX_DATA_SEGMENT_PHYS_BASE = MMIX_VIRT_MEMMAP[MMIX_VIRT_DATA][0]
MMIX_DATA_SEGMENT_SIZE = 0x0000000004000000
MMIX_STACK_SEGMENT_BASE = 0x6000000000000000
MMIX_STACK_SEGMENT_PHYS_BASE = MMIX_VIRT_MEMMAP[MMIX_VIRT_STACK][0]
MMIX_STACK_SEGMENT_SIZE = 0x0000000004000000
MMIX_LOW_RAM_BASE = MMIX_VIRT_MEMMAP[MMIX_VIRT_LOW_RAM][0]
MMIX_LOW_RAM_SIZE = MMIX_VIRT_MEMMAP[MMIX_VIRT_LOW_RAM][1]
MMIX_UNSUPPORTED_HIGH_SEGMENT_ADDRESS = 0x7000000000000000
MMIX_SEMIHOSTING_HALT = 0
MMIX_SEMIHOSTING_FOPEN = 1
MMIX_SEMIHOSTING_FCLOSE = 2
MMIX_SEMIHOSTING_FREAD = 3
MMIX_SEMIHOSTING_FGETS = 4
MMIX_SEMIHOSTING_FGETWS = 5
MMIX_SEMIHOSTING_FWRITE = 6
MMIX_SEMIHOSTING_FPUTS = 7
MMIX_SEMIHOSTING_FPUTWS = 8
MMIX_SEMIHOSTING_FSEEK = 9
MMIX_SEMIHOSTING_FTELL = 10
MMIX_SEMIHOSTING_STDIN = 0
MMIX_SEMIHOSTING_STDOUT = 1
MMIX_SEMIHOSTING_STDERR = 2
MMIX_SEMIHOSTING_FIRST_FILE_HANDLE = 3
MMIX_SEMIHOSTING_TEXT_READ = 0
MMIX_SEMIHOSTING_TEXT_WRITE = 1
MMIX_SEMIHOSTING_BINARY_READ = 2
MMIX_SEMIHOSTING_BINARY_WRITE = 3
MMIX_SEMIHOSTING_BINARY_READ_WRITE = 4
MMIX_SEMIHOSTING_STRING_MAX = 256
ELFCLASS32 = 1
ELFCLASS64 = 2
ELFDATA2LSB = 1
ELFDATA2MSB = 2
ET_REL = 1
ET_EXEC = 2
EM_X86_64 = 62
EM_MMIX = 80
PT_LOAD = 1
SHT_NULL = 0
SHT_PROGBITS = 1
SHT_STRTAB = 3
SHN_XINDEX = 0xffff
MMIX_REGS = 256
MMIX_GLOBAL_REG_MIN = 32
MMIX_OCTA_SIZE = 8

MMIX_BOOTINFO_FIELDS = (
    "magic",
    "version",
    "size",
    "flags",
    "cpu_count",
    "boot_cpu_id",
    "ram_base",
    "ram_size",
    "low_ram_base",
    "low_ram_size",
    "pool_logical_base",
    "pool_phys_base",
    "pool_size",
    "data_logical_base",
    "data_phys_base",
    "data_size",
    "stack_logical_base",
    "stack_phys_base",
    "stack_size",
    "mmio_base",
    "uart_base",
    "uart_irq",
    "timer_base",
    "timer_irq_base",
    "timer_irq_count",
    "intc_base",
    "intc_irq_count",
    "virtio_mmio_base",
    "virtio_mmio_irq",
    "virtio_mmio_count",
    "framebuffer_control_base",
    "framebuffer_base",
    "framebuffer_size",
    "framebuffer_irq",
    "framebuffer_width",
    "framebuffer_height",
    "framebuffer_stride",
    "framebuffer_format",
    "kernel_cmdline_addr",
    "kernel_cmdline_size",
    "ipi_base",
    "ipi_target_count",
    "ipi_request_mask",
    "high_ram_base",
    "high_ram_size",
)
MMIX_BOOTINFO_FORMAT = ">" + ("Q" * len(MMIX_BOOTINFO_FIELDS))
MMIX_BOOTINFO_SIZE = struct.calcsize(MMIX_BOOTINFO_FORMAT)
MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO] = (
    MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0],
    MMIX_BOOTINFO_SIZE,
)
MMIX_VIRT_MEMMAP[MMIX_VIRT_KERNEL_CMDLINE] = (
    MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0] + MMIX_BOOTINFO_SIZE,
    MMIX_BOOTINFO_KERNEL_CMDLINE_MAX + 1,
)


def parse_bootinfo(data):
    values = struct.unpack(MMIX_BOOTINFO_FORMAT, data[:MMIX_BOOTINFO_SIZE])
    return dict(zip(MMIX_BOOTINFO_FIELDS, values))


def expected_bootinfo(ram_size=512 * 1024 * 1024):
    high_ram_size = max(0, ram_size - 256 * 1024 * 1024)

    return {
        "magic": MMIX_BOOTINFO_MAGIC,
        "version": MMIX_BOOTINFO_VERSION,
        "size": MMIX_BOOTINFO_SIZE,
        "flags": 0,
        "cpu_count": 1,
        "boot_cpu_id": 0,
        "ram_base": 0,
        "ram_size": ram_size,
        "low_ram_base": MMIX_LOW_RAM_BASE,
        "low_ram_size": MMIX_LOW_RAM_SIZE,
        "pool_logical_base": MMIX_POOL_SEGMENT_BASE,
        "pool_phys_base": MMIX_POOL_SEGMENT_PHYS_BASE,
        "pool_size": MMIX_POOL_SEGMENT_SIZE,
        "data_logical_base": MMIX_DATA_SEGMENT_BASE,
        "data_phys_base": MMIX_DATA_SEGMENT_PHYS_BASE,
        "data_size": MMIX_DATA_SEGMENT_SIZE,
        "stack_logical_base": MMIX_STACK_SEGMENT_BASE,
        "stack_phys_base": MMIX_STACK_SEGMENT_PHYS_BASE,
        "stack_size": MMIX_STACK_SEGMENT_SIZE,
        "mmio_base": MMIX_VIRT_MEMMAP[MMIX_VIRT_MMIO][0],
        "uart_base": MMIX_VIRT_MEMMAP[MMIX_VIRT_UART0][0],
        "uart_irq": MMIX_VIRT_UART0_IRQ,
        "timer_base": MMIX_VIRT_MEMMAP[MMIX_VIRT_TIMER][0],
        "timer_irq_base": MMIX_VIRT_TIMER_IRQ_BASE,
        "timer_irq_count": 1,
        "intc_base": MMIX_VIRT_MEMMAP[MMIX_VIRT_INTC][0],
        "intc_irq_count": MMIX_VIRT_INTC_IRQ_COUNT,
        "virtio_mmio_base": MMIX_VIRT_MEMMAP[MMIX_VIRT_VIRTIO_MMIO][0],
        "virtio_mmio_irq": MMIX_VIRT_VIRTIO_BLOCK0_IRQ,
        "virtio_mmio_count": MMIX_VIRT_VIRTIO_MMIO_COUNT,
        "framebuffer_control_base": MMIX_VIRT_MEMMAP[
            MMIX_VIRT_FRAMEBUFFER_CONTROL
        ][0],
        "framebuffer_base": MMIX_VIRT_MEMMAP[MMIX_VIRT_FRAMEBUFFER][0],
        "framebuffer_size": MMIX_VIRT_MEMMAP[MMIX_VIRT_FRAMEBUFFER][1],
        "framebuffer_irq": MMIX_VIRT_FRAMEBUFFER_IRQ,
        "framebuffer_width": MMIX_VIRT_FRAMEBUFFER_WIDTH,
        "framebuffer_height": MMIX_VIRT_FRAMEBUFFER_HEIGHT,
        "framebuffer_stride": MMIX_VIRT_FRAMEBUFFER_STRIDE,
        "framebuffer_format": MMIX_VIRT_FRAMEBUFFER_FORMAT_XRGB8888,
        "kernel_cmdline_addr": 0,
        "kernel_cmdline_size": 0,
        "ipi_base": MMIX_VIRT_MEMMAP[MMIX_VIRT_IPI][0],
        "ipi_target_count": 1,
        "ipi_request_mask": RQ_IPI,
        "high_ram_base": 0x20000000 if high_ram_size else 0,
        "high_ram_size": high_ram_size,
    }


def serial_tx_program(message=b"MMIX\n"):
    program = [*set_octa(R1, MMIX_VIRT_UART0_BASE)]

    for ch in message:
        program.extend([
            insn(LDBUI, R3, R1, MMIX_VIRT_UART0_LSR),
            insn(ANDI, R3, R3, MMIX_VIRT_UART0_LSR_THRE),
            branch(BZB, R3, 0xfffe),
            wyde(SETL, R2, ch),
            insn(STBI, R2, R1, MMIX_VIRT_UART0_THR),
        ])

    program.append(halt())
    return b"".join(program), (len(program) - 1) * 4


def hosted_fputs_program(handle=MMIX_SEMIHOSTING_STDOUT,
                         message=b"Hosted MMIX\n"):
    message_address = 0x40
    prefix = b"".join(
        [
            *set_octa(R255, message_address),
            insn(TRAP, 0, MMIX_SEMIHOSTING_FPUTS, handle),
            wyde(SETL, R255, 0),
            halt(),
        ]
    )
    padding = insn(SWYM, 0, 0, 0) * ((message_address - len(prefix)) // 4)
    return prefix + padding + message + b"\0"


def hosted_llvm_smoke_program(message=b"LLVM smoke\n"):
    arg_address = 0x100
    buffer_address = 0x140
    prefix = b"".join(
        [
            *set_octa(R1, arg_address),
            *set_octa(R2, buffer_address),
            *set_octa(R3, len(message)),
            insn(STOUI, R2, R1, 0),
            insn(STOUI, R3, R1, 8),
            *set_octa(R255, arg_address),
            insn(TRAP, 0, MMIX_SEMIHOSTING_FWRITE,
                 MMIX_SEMIHOSTING_STDOUT),
            wyde(SETL, R255, 0),
            halt(),
        ]
    )
    padding = insn(SWYM, 0, 0, 0) * ((buffer_address - len(prefix)) // 4)
    return prefix + padding + message, len(prefix) - 4


def program_with_handler(prefix, handler_addr, handler):
    prefix = b"".join(prefix)
    handler = b"".join(handler)
    if len(prefix) > handler_addr:
        raise ValueError("handler address overlaps program prefix")
    if (handler_addr - len(prefix)) % 4 != 0:
        raise ValueError("handler address is not instruction-aligned")
    padding = insn(SWYM, 0, 0, 0) * ((handler_addr - len(prefix)) // 4)
    return prefix + padding + handler


def program_with_regions(*regions):
    image = bytearray()

    for address, instructions in regions:
        data = b"".join(instructions)
        if len(image) > address:
            raise ValueError("program regions overlap")
        if (address - len(image)) % 4 != 0:
            raise ValueError("program region is not instruction-aligned")
        image.extend(insn(SWYM, 0, 0, 0) * ((address - len(image)) // 4))
        image.extend(data)
    return bytes(image)


def register_stack_spill_fill_program(depth):
    sub_base = 0x20
    body_size = 6 * 4
    program = [
        branch(PUSHJ, R31, sub_base // 4),  # subroutine target
        insn(ADDI, R60, R31, 0),
        insn(GET, R50, 0, SR_O),
        insn(GET, R51, 0, SR_S),
        halt(),
    ]

    program.extend([insn(SWYM, 0, 0, 0)] * ((sub_base - len(program) * 4) // 4))

    for level in range(depth):
        program.extend(
            [
                insn(GET, 40 + level, 0, SR_J),  # global register
                wyde(SETL, R31, level + 1),
                branch(PUSHJ, R31, 4),  # next-call target
                insn(ADDI, R0, R31, 1),
                insn(PUT, SR_J, 0, 40 + level),  # global register
                insn(POP, 1, 0, 0),
            ]
        )

    program.extend(
        [
            wyde(SETL, R0, 1),
            insn(POP, 1, 0, 0),
        ]
    )

    image = b"".join(program)
    expected_image_len = sub_base + depth * body_size + 2 * 4
    if len(image) != expected_image_len:
        raise AssertionError("register-stack spill/fill image layout changed")
    return image, 4 * 4, depth + 1


def register_stack_save_unsave_program(depth):
    sub_base = 0x20
    body_size = 6 * 4
    program = [
        branch(PUSHJ, R31, sub_base // 4),  # subroutine target
        insn(ADDI, R60, R31, 0),
        insn(GET, R50, 0, SR_O),
        insn(GET, R51, 0, SR_S),
        halt(),
    ]

    program.extend([insn(SWYM, 0, 0, 0)] * ((sub_base - len(program) * 4) // 4))

    for level in range(depth):
        program.extend(
            [
                insn(GET, 40 + level, 0, SR_J),  # global register
                wyde(SETL, R31, level + 1),
                branch(PUSHJ, R31, 4),  # next-call target
                insn(ADDI, R0, R31, 1),
                insn(PUT, SR_J, 0, 40 + level),  # global register
                insn(POP, 1, 0, 0),
            ]
        )

    program.extend(
        [
            wyde(SETL, R0, 0x55),
            insn(SAVE, R32, 0, 0),
            wyde(SETL, R0, 0xaa),
            insn(UNSAVE, 0, 0, R32),
            insn(POP, 1, 0, 0),
        ]
    )

    image = b"".join(program)
    expected_image_len = sub_base + depth * body_size + 5 * 4
    if len(image) != expected_image_len:
        raise AssertionError("register-stack save/unsave image layout changed")
    return image, 4 * 4, 0x55 + depth


def save_state_after_save_program():
    program = [
        wyde(SETL, R0, 0x11),
        wyde(SETL, R1, 0x22),
        insn(SAVE, R32, 0, 0),
        insn(GET, R33, 0, SR_L),
        insn(GET, R34, 0, SR_O),
        insn(GET, R35, 0, SR_S),
        insn(ADDI, R36, R32, 0),
        halt(),
    ]
    return b"".join(program), (len(program) - 1) * 4


def save_unsave_roundtrip_program():
    program = [
        wyde(SETL, R0, 0x11),
        wyde(SETL, R1, 0x22),
        wyde(SETL, R2, 0x33),
        *set_octa(R40, 0x1111222233334444),
        *set_octa(R41, 0x5555666677778888),
        *set_octa(R42, 0x0000000000001234),
        insn(PUT, SR_J, 0, R42),
        insn(PUTI, SR_M, 0, 0x5a),
        insn(PUTI, SR_P, 0, 0x6b),
        *set_octa(R43, 0x000000000003ffff),
        insn(PUT, SR_A, 0, R43),
        insn(SAVE, R32, 0, 0),
        insn(ADDI, R33, R32, 0),
        wyde(SETL, R40, 0),
        wyde(SETL, R41, 0),
        insn(PUTI, SR_J, 0, 0),
        insn(PUTI, SR_M, 0, 0),
        insn(PUTI, SR_P, 0, 0),
        insn(PUTI, SR_A, 0, 0),
        wyde(SETL, R0, 0xee),
        wyde(SETL, R1, 0xff),
        wyde(SETL, R2, 0xaa),
        insn(UNSAVE, 0, 0, R33),
        insn(ADDI, R50, R0, 0),
        insn(ADDI, R51, R1, 0),
        insn(ADDI, R52, R2, 0),
        insn(GET, R53, 0, SR_J),
        insn(GET, R54, 0, SR_M),
        insn(GET, R55, 0, SR_P),
        insn(GET, R56, 0, SR_A),
        insn(GET, R57, 0, SR_L),
        insn(GET, R58, 0, SR_O),
        insn(GET, R59, 0, SR_S),
        insn(ADDI, R60, R40, 0),
        insn(ADDI, R61, R41, 0),
        insn(ADDI, R62, R32, 0),
        insn(ADDI, R63, R33, 0),
        halt(),
    ]
    return b"".join(program), (len(program) - 1) * 4


def lane_difference(y, z, lane_bits):
    result = 0
    mask = (1 << lane_bits) - 1
    for shift in range(0, 64, lane_bits):
        y_lane = (y >> shift) & mask
        z_lane = (z >> shift) & mask
        if y_lane > z_lane:
            result |= (y_lane - z_lane) << shift
    return result


def sadd(y, z):
    return (y & (~z & MASK64)).bit_count()


def matrix_byte(value, row):
    return (value >> ((7 - row) * 8)) & 0xff


def matrix_multiply(y, z, exclusive):
    result = 0
    for i in range(8):
        z_row = matrix_byte(z, i)
        x_row = 0
        for j in range(8):
            bit = 0
            for k in range(8):
                y_bit = matrix_byte(y, k) & (0x80 >> j)
                z_bit = z_row & (0x80 >> k)
                if exclusive:
                    bit ^= bool(y_bit and z_bit)
                else:
                    bit |= bool(y_bit and z_bit)
            if bit:
                x_row |= 0x80 >> j
        result |= x_row << ((7 - i) * 8)
    return result


def mux(y, z, mask):
    return ((y & mask) | (z & ~mask)) & MASK64


def signed_div(y, z):
    y = s64(y)
    z = s64(z)
    if z == 0:
        return 0, y & MASK64
    quotient = y // z
    remainder = y - quotient * z
    return quotient & MASK64, remainder & MASK64


def unsigned_div(high, low, divisor):
    if divisor == 0 or high >= divisor:
        return high & MASK64, low & MASK64
    dividend = (high << 64) | low
    return (dividend // divisor) & MASK64, (dividend % divisor) & MASK64


def s64(value):
    value &= MASK64
    return value - (1 << 64) if value & (1 << 63) else value


def f64(value):
    return struct.unpack(">Q", struct.pack(">d", value))[0]


def f32(value):
    return struct.unpack(">I", struct.pack(">f", value))[0]


def elf64_header(elf_class=ELFCLASS64, elf_data=ELFDATA2MSB,
                 elf_type=ET_EXEC, machine=EM_MMIX, entry=0, phnum=0,
                 phoff=64, shoff=0, shnum=0, shstrndx=0):
    e_ident = bytes((
        0x7f, ord("E"), ord("L"), ord("F"),
        elf_class, elf_data, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    ))
    return e_ident + struct.pack(
        ">HHIQQQIHHHHHH",
        elf_type,
        machine,
        1,
        entry,
        phoff if phnum else 0,
        shoff if shnum else 0,
        0,
        64,
        56,
        phnum,
        64,
        shnum,
        shstrndx,
    )


def elf64_phdr(load_address, data, mem_size=None, offset=0x100,
               ph_type=PT_LOAD, flags=5, virtual_address=None):
    if mem_size is None:
        mem_size = len(data)
    if virtual_address is None:
        virtual_address = load_address
    return struct.pack(
        ">IIQQQQQQ",
        ph_type,
        flags,
        offset,
        virtual_address,
        load_address,
        len(data),
        mem_size,
        0x1000,
    )


def elf64_image(load_address, data, mem_size=None, entry=0, offset=0x100):
    phdr = elf64_phdr(load_address, data, mem_size=mem_size, offset=offset)
    prefix = elf64_header(entry=entry, phnum=1) + phdr
    if offset < len(prefix):
        raise ValueError("ELF segment offset overlaps the program header table")
    return prefix + bytes(offset - len(prefix)) + data


def elf64_shdr(name=0, section_type=SHT_NULL, flags=0, address=0, offset=0,
               size=0, link=0, info=0, alignment=0, entry_size=0):
    return struct.pack(
        ">IIQQQQIIQQ",
        name,
        section_type,
        flags,
        address,
        offset,
        size,
        link,
        info,
        alignment,
        entry_size,
    )


_ELF64_EHDR_FIELDS = {
    "section_offset": (40, "Q"),
    "section_entry_size": (58, "H"),
    "section_count": (60, "H"),
    "section_names_index": (62, "H"),
}

_ELF64_SHDR_FIELDS = {
    "name": (0, "I"),
    "type": (4, "I"),
    "flags": (8, "Q"),
    "address": (16, "Q"),
    "offset": (24, "Q"),
    "size": (32, "Q"),
    "link": (40, "I"),
    "info": (44, "I"),
    "alignment": (48, "Q"),
    "entry_size": (56, "Q"),
}


def _elf64_read_field(image, base, fields, field):
    offset, format_ = fields[field]
    return struct.unpack_from(">" + format_, image, base + offset)[0]


def _elf64_patch_field(image, base, fields, field, value):
    offset, format_ = fields[field]
    result = bytearray(image)
    struct.pack_into(">" + format_, result, base + offset, value)
    return bytes(result)


def elf64_read_ehdr_field(image, field):
    return _elf64_read_field(image, 0, _ELF64_EHDR_FIELDS, field)


def elf64_patch_ehdr_field(image, field, value):
    return _elf64_patch_field(image, 0, _ELF64_EHDR_FIELDS, field, value)


def elf64_read_shdr_field(image, index, field):
    table_offset = elf64_read_ehdr_field(image, "section_offset")
    entry_size = elf64_read_ehdr_field(image, "section_entry_size")
    return _elf64_read_field(
        image, table_offset + index * entry_size, _ELF64_SHDR_FIELDS, field
    )


def elf64_patch_shdr_field(image, index, field, value):
    table_offset = elf64_read_ehdr_field(image, "section_offset")
    entry_size = elf64_read_ehdr_field(image, "section_entry_size")
    return _elf64_patch_field(
        image, table_offset + index * entry_size,
        _ELF64_SHDR_FIELDS, field, value
    )


def elf64_patch_byte(image, offset, value):
    result = bytearray(image)
    result[offset] = value
    return bytes(result)


def elf64_duplicate_shdr(image, index):
    table_offset = elf64_read_ehdr_field(image, "section_offset")
    entry_size = elf64_read_ehdr_field(image, "section_entry_size")
    section_count = elf64_read_ehdr_field(image, "section_count")
    table_end = table_offset + section_count * entry_size
    if table_end != len(image):
        raise ValueError("ELF section table must end at the end of the image")

    duplicate = image[
        table_offset + index * entry_size:
        table_offset + (index + 1) * entry_size
    ]
    return elf64_patch_ehdr_field(
        image + duplicate, "section_count", section_count + 1
    )


def _align_up(value, alignment):
    return (value + alignment - 1) & -alignment


def elf64_image_with_reg_contents(load_address, data, global_base, values,
                                  entry=0, segment_offset=0x100):
    if not MMIX_GLOBAL_REG_MIN <= global_base < MMIX_REGS:
        raise ValueError("ELF global-register base must be in 32..255")
    if len(values) > MMIX_REGS - 1 - global_base:
        raise ValueError("ELF register contents must not include register 255")

    register_data = b"".join(struct.pack(">Q", value) for value in values)
    section_names = b"\0.shstrtab\0.MMIX.reg_contents\0"
    shstrtab_name = section_names.index(b".shstrtab")
    reg_contents_name = section_names.index(b".MMIX.reg_contents")
    register_offset = _align_up(segment_offset + len(data), 8)
    names_offset = register_offset + len(register_data)
    section_offset = _align_up(names_offset + len(section_names), 8)
    section_count = 3

    image = bytearray(section_offset + section_count * 64)
    header = elf64_header(
        entry=entry,
        phnum=1,
        shoff=section_offset,
        shnum=section_count,
        shstrndx=1,
    )
    phdr = elf64_phdr(load_address, data, offset=segment_offset)
    prefix = header + phdr
    if segment_offset < len(prefix):
        raise ValueError("ELF segment offset overlaps the program header table")

    image[:len(prefix)] = prefix
    image[segment_offset:segment_offset + len(data)] = data
    image[register_offset:register_offset + len(register_data)] = register_data
    image[names_offset:names_offset + len(section_names)] = section_names

    section_headers = b"".join([
        elf64_shdr(),
        elf64_shdr(
            name=shstrtab_name,
            section_type=SHT_STRTAB,
            offset=names_offset,
            size=len(section_names),
            alignment=1,
        ),
        elf64_shdr(
            name=reg_contents_name,
            section_type=SHT_PROGBITS,
            address=global_base * 8,
            offset=register_offset,
            size=len(register_data),
            alignment=8,
        ),
    ])
    image[
        section_offset:section_offset + len(section_headers)
    ] = section_headers
    return bytes(image)


@dataclasses.dataclass(frozen=True)
class MMIXTest:
    name: str
    program: bytes
    pc: int
    regs: dict[int, int]
    exit_status: int = 0
    qemu_args: tuple[str, ...] = ()
    stdin_data: Optional[bytes] = None


@dataclasses.dataclass(frozen=True)
class MMIXExpectedFailure:
    name: str
    program: bytes
    patterns: tuple[str, ...]
    absent: tuple[str, ...] = ("MMIX test exit", "MMIX dynamic trap causes=")
    qemu_args: tuple[str, ...] = ()


@dataclasses.dataclass(frozen=True)
class MMIXSerialTest:
    name: str
    program: bytes
    pc: int
    output: bytes
    exit_status: int = 0
    qemu_args: tuple[str, ...] = ()
    stdin_data: Optional[bytes] = None


@dataclasses.dataclass(frozen=True)
class MMIXLoaderFailure:
    name: str
    image: bytes
    patterns: tuple[str, ...]
    qemu_args: tuple[str, ...] = ()
    sparse_size: Optional[int] = None


@dataclasses.dataclass(frozen=True)
class MMIXProcessFailure:
    name: str
    program: bytes
    qemu_args: tuple[str, ...]
    patterns: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class MMIXMMOTest:
    name: str
    image: bytes
    pc: int
    regs: dict[int, int]
    exit_status: int = 0
    qemu_args: tuple[str, ...] = ()


@dataclasses.dataclass(frozen=True)
class MMIXELFTest:
    name: str
    image: bytes
    pc: int
    regs: dict[int, int]
    output: Optional[bytes] = None
    exit_status: int = 0
    qemu_args: tuple[str, ...] = ()


def case_id(test):
    return test.name


REGISTER_STACK_SPILL_FILL = register_stack_spill_fill_program(10)
REGISTER_STACK_SAVE_UNSAVE = register_stack_save_unsave_program(10)
SAVE_STATE_AFTER_SAVE = save_state_after_save_program()
SAVE_UNSAVE_ROUNDTRIP = save_unsave_roundtrip_program()
