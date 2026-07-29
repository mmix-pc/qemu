#!/usr/bin/env python3
#
# MMIX .mmo fixture construction helpers for softmmu tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import construct

MMIX_MMO_ESCAPE = 0x98
MMIX_MMO_LOP_QUOTE = 0x00
MMIX_MMO_LOP_LOC = 0x01
MMIX_MMO_LOP_SKIP = 0x02
MMIX_MMO_LOP_FIXO = 0x03
MMIX_MMO_LOP_FIXR = 0x04
MMIX_MMO_LOP_FIXRX = 0x05
MMIX_MMO_LOP_FILE = 0x06
MMIX_MMO_LOP_LINE = 0x07
MMIX_MMO_LOP_SPEC = 0x08
MMIX_MMO_LOP_PRE = 0x09
MMIX_MMO_LOP_POST = 0x0A
MMIX_MMO_LOP_STAB = 0x0B
MMIX_MMO_LOP_END = 0x0C

_LOP_RECORD = construct.Struct(
    "escape" / construct.Int8ub,
    "lop" / construct.Int8ub,
    "y" / construct.Int8ub,
    "z" / construct.Int8ub,
)


def _u32(value):
    return construct.Int32ub.build(value)


def _u64(value):
    return construct.Int64ub.build(value)


def mmo_lop(lop, yz, y=None, z=None):
    if y is None:
        y = (yz >> 8) & 0xff
    if z is None:
        z = yz & 0xff
    return _LOP_RECORD.build(
        {
            "escape": MMIX_MMO_ESCAPE,
            "lop": lop,
            "y": y,
            "z": z,
        }
    )


def mmo_image(items):
    return b"".join([mmo_lop(MMIX_MMO_LOP_PRE, 0, y=1, z=0), *items])


def mmo_quote(tetra):
    return mmo_lop(MMIX_MMO_LOP_QUOTE, 1) + tetra


def mmo_address_lop(lop, address):
    high = (address >> 32) & 0xffffffff
    low = address & 0xffffffff
    if high & 0x00ffffff:
        return (
            mmo_lop(lop, 0, y=(high >> 24) & 0xff, z=2)
            + _u32(high & 0x00ffffff)
            + _u32(low)
        )
    return mmo_lop(lop, 0, y=(high >> 24) & 0xff, z=1) + _u32(low)


def mmo_loc(address):
    return mmo_address_lop(MMIX_MMO_LOP_LOC, address)


def mmo_fixo(address):
    return mmo_address_lop(MMIX_MMO_LOP_FIXO, address)


def mmo_fixr(delta):
    if not 0 <= delta <= 0xffff:
        raise ValueError("mmo_fixr only supports a 16-bit delta")
    return mmo_lop(MMIX_MMO_LOP_FIXR, delta)


def mmo_fixrx(width, delta):
    if width not in (16, 24):
        raise ValueError("mmo_fixrx width must be 16 or 24")
    if not 0 <= delta <= 0x01ffffff:
        raise ValueError("mmo_fixrx delta must fit the accepted range")
    return mmo_lop(MMIX_MMO_LOP_FIXRX, width) + _u32(delta)


def mmo_skip(bytes_):
    if not 0 <= bytes_ <= 0xffff:
        raise ValueError("mmo_skip only supports a 16-bit byte count")
    return mmo_lop(MMIX_MMO_LOP_SKIP, bytes_)


def mmo_file(file_number, name):
    encoded = name.encode("ascii")
    encoded += b"\0" * ((4 - len(encoded) % 4) % 4)
    tetra_count = len(encoded) // 4
    if not 0 <= file_number <= 255 or not 0 <= tetra_count <= 255:
        raise ValueError("mmo_file fields must fit in one byte")
    return mmo_lop(MMIX_MMO_LOP_FILE, 0, y=file_number, z=tetra_count) + encoded


def mmo_line(line):
    if not 0 <= line <= 0xffff:
        raise ValueError("mmo_line line must fit in 16 bits")
    return mmo_lop(MMIX_MMO_LOP_LINE, line)


def mmo_spec(kind):
    if not 0 <= kind <= 0xffff:
        raise ValueError("mmo_spec kind must fit in 16 bits")
    return mmo_lop(MMIX_MMO_LOP_SPEC, kind)


def mmo_post(global_base, globals_):
    if not 32 <= global_base <= 255:
        raise ValueError("mmo_post global base must be in 32..255")
    records = [mmo_lop(MMIX_MMO_LOP_POST, 0, y=0, z=global_base)]
    for reg in range(global_base, 256):
        records.append(_u64(globals_.get(reg, 0)))
    return b"".join(records)


def mmo_stab_end(symbol_tetras=()):
    symbol_data = b"".join(symbol_tetras)
    if len(symbol_data) % 4:
        raise ValueError("mmo_stab_end symbol data must be tetrabyte-aligned")
    tetra_count = len(symbol_data) // 4
    if tetra_count > 0xffff:
        raise ValueError("mmo_stab_end symbol data is too large")
    return mmo_lop(MMIX_MMO_LOP_STAB, 0) + symbol_data + mmo_lop(
        MMIX_MMO_LOP_END, tetra_count
    )
