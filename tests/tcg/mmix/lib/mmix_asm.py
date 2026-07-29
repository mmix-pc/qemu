#!/usr/bin/env python3
#
# MMIX instruction encoding helpers for softmmu tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bitstruct


def insn(op, x=0, y=0, z=0):
    return bitstruct.pack("u8u8u8u8", op, x, y, z)


def branch(op, x, yz):
    return bitstruct.pack("u8u8u16", op, x, yz)


def jump(op, xyz):
    return bitstruct.pack("u8u24", op, xyz)


def wyde(op, x, yz):
    return bitstruct.pack("u8u8u16", op, x, yz)


def halt():
    return insn(0x00, 0, 0, 0)


def set_octa(reg, value):
    return [
        wyde(0xe0, reg, (value >> 48) & 0xffff),  # SETH reg,value[63:48]
        wyde(0xe5, reg, (value >> 32) & 0xffff),  # INCMH reg,value[47:32]
        wyde(0xe6, reg, (value >> 16) & 0xffff),  # INCML reg,value[31:16]
        wyde(0xe7, reg, value & 0xffff),          # INCL reg,value[15:0]
    ]
