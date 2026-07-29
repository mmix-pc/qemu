#!/usr/bin/env python3
#
# Shared MMIX softmmu test case helpers and data
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses
import struct

from lib.mmix_asm import branch, halt, insn, jump, set_octa, wyde
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
MMIX_HOSTED_STDOUT = 1
MMIX_HOSTED_FPUTS = 7
MMIX_HOSTED_FPUTWS = 8
MMIX_HOSTED_STRING_MAX = 256


def serial_tx_program():
    return b"".join(
        [
            *set_octa(1, MMIX_VIRT_UART_BASE),
            wyde(0xe3, 2, ord("M")),                  # SETL r2,'M'
            insn(0xa1, 2, 1, MMIX_VIRT_UART_TX),      # STBI r2,r1,TX
            wyde(0xe3, 2, ord("M")),                  # SETL r2,'M'
            insn(0xa1, 2, 1, MMIX_VIRT_UART_TX),      # STBI r2,r1,TX
            wyde(0xe3, 2, ord("I")),                  # SETL r2,'I'
            insn(0xa1, 2, 1, MMIX_VIRT_UART_TX),      # STBI r2,r1,TX
            wyde(0xe3, 2, ord("X")),                  # SETL r2,'X'
            insn(0xa1, 2, 1, MMIX_VIRT_UART_TX),      # STBI r2,r1,TX
            wyde(0xe3, 2, ord("\n")),                 # SETL r2,'\n'
            insn(0xa1, 2, 1, MMIX_VIRT_UART_TX),      # STBI r2,r1,TX
            halt(),
        ]
    )


def hosted_fputs_program():
    message_address = 0x40
    message = b"Hosted MMIX\n"
    prefix = b"".join(
        [
            *set_octa(255, message_address),
            insn(0x00, 0, MMIX_HOSTED_FPUTS, MMIX_HOSTED_STDOUT),
            halt(),
        ]
    )
    padding = insn(0xfd, 0, 0, 0) * ((message_address - len(prefix)) // 4)
    return prefix + padding + message + b"\0"


def program_with_handler(prefix, handler_addr, handler):
    prefix = b"".join(prefix)
    handler = b"".join(handler)
    if len(prefix) > handler_addr:
        raise ValueError("handler address overlaps program prefix")
    if (handler_addr - len(prefix)) % 4 != 0:
        raise ValueError("handler address is not instruction-aligned")
    padding = insn(0xfd, 0, 0, 0) * ((handler_addr - len(prefix)) // 4)
    return prefix + padding + handler


def register_stack_spill_fill_program(depth):
    sub_base = 0x20
    body_size = 6 * 4
    program = [
        branch(0xf2, 31, sub_base // 4),   # PUSHJ 31,sub0
        insn(0x21, 60, 31, 0),             # ADDI r60,r31,0
        insn(0xfe, 50, 0, 10),             # GET r50,rO
        insn(0xfe, 51, 0, 11),             # GET r51,rS
        halt(),
    ]

    program.extend([insn(0xfd, 0, 0, 0)] * ((sub_base - len(program) * 4) // 4))

    for level in range(depth):
        program.extend(
            [
                insn(0xfe, 40 + level, 0, 4),  # GET global,rJ
                wyde(0xe3, 31, level + 1),     # SETL r31,level+1
                branch(0xf2, 31, 4),           # PUSHJ 31,next
                insn(0x21, 0, 31, 1),          # ADDI r0,r31,1
                insn(0xf6, 4, 0, 40 + level),  # PUT rJ,global
                insn(0xf8, 1, 0, 0),           # POP 1,0
            ]
        )

    program.extend(
        [
            wyde(0xe3, 0, 1),                  # SETL r0,1
            insn(0xf8, 1, 0, 0),               # POP 1,0
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
        branch(0xf2, 31, sub_base // 4),   # PUSHJ 31,sub0
        insn(0x21, 60, 31, 0),             # ADDI r60,r31,0
        insn(0xfe, 50, 0, 10),             # GET r50,rO
        insn(0xfe, 51, 0, 11),             # GET r51,rS
        halt(),
    ]

    program.extend([insn(0xfd, 0, 0, 0)] * ((sub_base - len(program) * 4) // 4))

    for level in range(depth):
        program.extend(
            [
                insn(0xfe, 40 + level, 0, 4),  # GET global,rJ
                wyde(0xe3, 31, level + 1),     # SETL r31,level+1
                branch(0xf2, 31, 4),           # PUSHJ 31,next
                insn(0x21, 0, 31, 1),          # ADDI r0,r31,1
                insn(0xf6, 4, 0, 40 + level),  # PUT rJ,global
                insn(0xf8, 1, 0, 0),           # POP 1,0
            ]
        )

    program.extend(
        [
            wyde(0xe3, 0, 0x55),               # SETL r0,0x55
            insn(0xfa, 32, 0, 0),              # SAVE r32,0
            wyde(0xe3, 0, 0xaa),               # SETL r0,0xaa
            insn(0xfb, 0, 0, 32),              # UNSAVE 0,r32
            insn(0xf8, 1, 0, 0),               # POP 1,0
        ]
    )

    image = b"".join(program)
    expected_image_len = sub_base + depth * body_size + 5 * 4
    if len(image) != expected_image_len:
        raise AssertionError("register-stack save/unsave image layout changed")
    return image, 4 * 4, 0x55 + depth


def save_state_after_save_program():
    program = [
        wyde(0xe3, 0, 0x11),               # SETL r0,0x11
        wyde(0xe3, 1, 0x22),               # SETL r1,0x22
        insn(0xfa, 32, 0, 0),              # SAVE r32,0
        insn(0xfe, 33, 0, 20),             # GET r33,rL
        insn(0xfe, 34, 0, 10),             # GET r34,rO
        insn(0xfe, 35, 0, 11),             # GET r35,rS
        insn(0x21, 36, 32, 0),             # ADDI r36,r32,0
        halt(),
    ]
    return b"".join(program), (len(program) - 1) * 4


def save_unsave_roundtrip_program():
    program = [
        wyde(0xe3, 0, 0x11),               # SETL r0,0x11
        wyde(0xe3, 1, 0x22),               # SETL r1,0x22
        wyde(0xe3, 2, 0x33),               # SETL r2,0x33
        *set_octa(40, 0x1111222233334444),
        *set_octa(41, 0x5555666677778888),
        *set_octa(42, 0x0000000000001234),
        insn(0xf6, 4, 0, 42),              # PUT rJ,r42
        insn(0xf7, 5, 0, 0x5a),            # PUTI rM,0x5a
        insn(0xf7, 23, 0, 0x6b),           # PUTI rP,0x6b
        *set_octa(43, 0x000000000003ffff),
        insn(0xf6, 21, 0, 43),             # PUT rA,r43
        insn(0xfa, 32, 0, 0),              # SAVE r32,0
        insn(0x21, 33, 32, 0),             # ADDI r33,r32,0
        wyde(0xe3, 40, 0),                 # SETL r40,0
        wyde(0xe3, 41, 0),                 # SETL r41,0
        insn(0xf7, 4, 0, 0),               # PUTI rJ,0
        insn(0xf7, 5, 0, 0),               # PUTI rM,0
        insn(0xf7, 23, 0, 0),              # PUTI rP,0
        insn(0xf7, 21, 0, 0),              # PUTI rA,0
        wyde(0xe3, 0, 0xee),               # SETL r0,0xee
        wyde(0xe3, 1, 0xff),               # SETL r1,0xff
        wyde(0xe3, 2, 0xaa),               # SETL r2,0xaa
        insn(0xfb, 0, 0, 33),              # UNSAVE 0,r33
        insn(0x21, 50, 0, 0),              # ADDI r50,r0,0
        insn(0x21, 51, 1, 0),              # ADDI r51,r1,0
        insn(0x21, 52, 2, 0),              # ADDI r52,r2,0
        insn(0xfe, 53, 0, 4),              # GET r53,rJ
        insn(0xfe, 54, 0, 5),              # GET r54,rM
        insn(0xfe, 55, 0, 23),             # GET r55,rP
        insn(0xfe, 56, 0, 21),             # GET r56,rA
        insn(0xfe, 57, 0, 20),             # GET r57,rL
        insn(0xfe, 58, 0, 10),             # GET r58,rO
        insn(0xfe, 59, 0, 11),             # GET r59,rS
        insn(0x21, 60, 40, 0),             # ADDI r60,r40,0
        insn(0x21, 61, 41, 0),             # ADDI r61,r41,0
        insn(0x21, 62, 32, 0),             # ADDI r62,r32,0
        insn(0x21, 63, 33, 0),             # ADDI r63,r33,0
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


@dataclasses.dataclass(frozen=True)
class MMIXLoaderFailure:
    name: str
    image: bytes
    patterns: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class MMIXMMOTest:
    name: str
    image: bytes
    pc: int
    regs: dict[int, int]


REGISTER_STACK_SPILL_FILL = register_stack_spill_fill_program(10)
REGISTER_STACK_SAVE_UNSAVE = register_stack_save_unsave_program(10)
SAVE_STATE_AFTER_SAVE = save_state_after_save_program()
SAVE_UNSAVE_ROUNDTRIP = save_unsave_roundtrip_program()
