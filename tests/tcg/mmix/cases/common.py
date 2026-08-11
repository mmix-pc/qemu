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
RA_EVENT_X = 0x01
RA_EVENT_Z = 0x02
RA_EVENT_O = 0x08
RA_EVENT_V = 0x40
RA_EVENT_D = 0x80
RA_ENABLE_SHIFT = 8
RQ_PROGRAM_K = 1 << 35
RQ_PROGRAM_R = 1 << 39
RQ_PROGRAM_W = 1 << 38
RQ_PROGRAM_X = 1 << 37
RQ_PROGRAM_N = 1 << 36
VM_PAGE_TABLE = 0x2000
VM_RV_PAGE0 = 0x11110d0000002000
VM_PAGE_TABLE_ROOT2 = 0x4000
VM_RV_ROOT2 = 0x11110d0000004000
NEGATIVE_HANDLER = 0x8000000000000080
MMIX_VIRT_UART_BASE = 0x0000000100000000
MMIX_VIRT_UART_TX = 0x04
MMIX_DATA_SEGMENT_BASE = 0x2000000000000000
MMIX_DATA_SEGMENT_SIZE = 0x0000000004000000
MMIX_POOL_SEGMENT_BASE = 0x4000000000000000
MMIX_POOL_SEGMENT_SIZE = 0x0000000000100000
MMIX_SEMIHOSTING_HALT = 0
MMIX_SEMIHOSTING_FOPEN = 1
MMIX_SEMIHOSTING_FCLOSE = 2
MMIX_SEMIHOSTING_FREAD = 3
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


def serial_tx_program():
    return b"".join(
        [
            *set_octa(R1, MMIX_VIRT_UART_BASE),
            wyde(SETL, R2, ord("M")),
            insn(STBI, R2, R1, MMIX_VIRT_UART_TX),
            wyde(SETL, R2, ord("M")),
            insn(STBI, R2, R1, MMIX_VIRT_UART_TX),
            wyde(SETL, R2, ord("I")),
            insn(STBI, R2, R1, MMIX_VIRT_UART_TX),
            wyde(SETL, R2, ord("X")),
            insn(STBI, R2, R1, MMIX_VIRT_UART_TX),
            wyde(SETL, R2, ord("\n")),
            insn(STBI, R2, R1, MMIX_VIRT_UART_TX),
            halt(),
        ]
    )


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


def program_with_handler(prefix, handler_addr, handler):
    prefix = b"".join(prefix)
    handler = b"".join(handler)
    if len(prefix) > handler_addr:
        raise ValueError("handler address overlaps program prefix")
    if (handler_addr - len(prefix)) % 4 != 0:
        raise ValueError("handler address is not instruction-aligned")
    padding = insn(SWYM, 0, 0, 0) * ((handler_addr - len(prefix)) // 4)
    return prefix + padding + handler


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
    absent: tuple[str, ...] = ("MMIX test exit",)


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


def case_id(test):
    return test.name


REGISTER_STACK_SPILL_FILL = register_stack_spill_fill_program(10)
REGISTER_STACK_SAVE_UNSAVE = register_stack_save_unsave_program(10)
SAVE_STATE_AFTER_SAVE = save_state_after_save_program()
SAVE_UNSAVE_ROUNDTRIP = save_unsave_roundtrip_program()
