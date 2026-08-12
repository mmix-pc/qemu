#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *

LOADER_FAILURE_TESTS = [
    MMIXLoaderFailure(
        "mmo-truncated-preamble",
        bytes((0x98, 0x09)),
        ("truncated MMIX .mmo preamble",),
    ),
    MMIXLoaderFailure(
        "mmo-unsupported-preamble-version",
        bytes((0x98, 0x09, 0x02, 0x00)),
        ("unsupported MMIX .mmo preamble version 2",),
    ),
    MMIXLoaderFailure(
        "elf-truncated-header",
        b"\x7fELF",
        ("truncated MMIX ELF header",),
    ),
    MMIXLoaderFailure(
        "elf-unsupported-class",
        elf64_header(elf_class=ELFCLASS32),
        ("unsupported MMIX ELF class 1",),
    ),
    MMIXLoaderFailure(
        "elf-unsupported-data-encoding",
        elf64_header(elf_data=ELFDATA2LSB),
        ("unsupported MMIX ELF data encoding 1",),
    ),
    MMIXLoaderFailure(
        "elf-unsupported-type",
        elf64_header(elf_type=ET_REL),
        ("unsupported MMIX ELF type 1",),
    ),
    MMIXLoaderFailure(
        "elf-unsupported-machine",
        elf64_header(machine=EM_X86_64),
        ("unsupported MMIX ELF machine 62",),
    ),
    MMIXLoaderFailure(
        "elf-no-loadable-segments",
        elf64_header(),
        ("could not load MMIX ELF kernel",),
    ),
    MMIXLoaderFailure(
        "elf-truncated-program-header-table",
        elf64_header(phnum=1),
        ("could not load MMIX ELF kernel",),
    ),
    MMIXLoaderFailure(
        "elf-load-segment-outside-low-ram",
        elf64_image(0x06000000, halt()),
        ("MMIX ELF kernel", "loads outside Low RAM"),
    ),
    MMIXLoaderFailure(
        "mmo-fixo-invalid-z",
        mmo_image([mmo_lop(MMIX_MMO_LOP_FIXO, 0, y=0, z=0)]),
        ("invalid MMIX .mmo lop_fixo z=0", "tetra 2"),
    ),
    MMIXLoaderFailure(
        "mmo-unsupported-data-segment-range",
        mmo_image([mmo_loc(MMIX_DATA_SEGMENT_BASE + MMIX_DATA_SEGMENT_SIZE), halt()]),
        ("unsupported MMIX .mmo high-segment tetrabyte address "
         "0x2000000004000000",),
    ),
    MMIXLoaderFailure(
        "mmo-unsupported-pool-segment-range",
        mmo_image([mmo_loc(MMIX_POOL_SEGMENT_BASE + MMIX_POOL_SEGMENT_SIZE),
                   halt()]),
        (f"unsupported MMIX .mmo high-segment tetrabyte address "
         f"0x{MMIX_POOL_SEGMENT_BASE + MMIX_POOL_SEGMENT_SIZE:016x}",),
    ),
    MMIXLoaderFailure(
        "mmo-unsupported-stack-segment-range",
        mmo_image([mmo_loc(MMIX_STACK_SEGMENT_BASE + MMIX_STACK_SEGMENT_SIZE),
                   halt()]),
        (f"unsupported MMIX .mmo high-segment tetrabyte address "
         f"0x{MMIX_STACK_SEGMENT_BASE + MMIX_STACK_SEGMENT_SIZE:016x}",),
    ),
    MMIXLoaderFailure(
        "mmo-fixr-target-before-zero",
        mmo_image([mmo_lop(MMIX_MMO_LOP_FIXR, 4)]),
        ("MMIX .mmo lop_fixr target before address 0", "tetra 2"),
    ),
    MMIXLoaderFailure(
        "mmo-fixrx-invalid-yz",
        mmo_image([mmo_lop(MMIX_MMO_LOP_FIXRX, 8)]),
        ("invalid MMIX .mmo lop_fixrx yz=8", "tetra 2"),
    ),
    MMIXLoaderFailure(
        "mmo-fixrx-truncated-delta",
        mmo_image([mmo_lop(MMIX_MMO_LOP_FIXRX, 16)]),
        ("truncated MMIX .mmo object", "tetra 2"),
    ),
    MMIXLoaderFailure(
        "mmo-fixrx-delta-too-large",
        mmo_image([mmo_lop(MMIX_MMO_LOP_FIXRX, 24), struct.pack(">I", 0x02000000)]),
        ("invalid MMIX .mmo lop_fixrx delta 0x02000000", "tetra 2"),
    ),
    MMIXLoaderFailure(
        "mmo-fixrx-invalid-16-bit-delta",
        mmo_image([mmo_lop(MMIX_MMO_LOP_FIXRX, 16), struct.pack(">I", 0x00010000)]),
        ("invalid MMIX .mmo lop_fixrx delta 0x00010000 for 16-bit relative fixup",
         "tetra 2"),
    ),
    MMIXLoaderFailure(
        "mmo-fixrx-target-before-zero",
        mmo_image([mmo_lop(MMIX_MMO_LOP_FIXRX, 16), struct.pack(">I", 1)]),
        ("MMIX .mmo lop_fixrx target before address 0", "tetra 2"),
    ),
    MMIXLoaderFailure(
        "mmo-post-invalid-y",
        mmo_image([mmo_lop(MMIX_MMO_LOP_POST, 0, y=1, z=32)]),
        ("invalid MMIX .mmo lop_post y=1", "tetra 2"),
    ),
    MMIXLoaderFailure(
        "mmo-post-invalid-z",
        mmo_image([mmo_lop(MMIX_MMO_LOP_POST, 0, y=0, z=31)]),
        ("invalid MMIX .mmo lop_post z=31", "tetra 2"),
    ),
    MMIXLoaderFailure(
        "mmo-post-truncated-global",
        mmo_image([mmo_lop(MMIX_MMO_LOP_POST, 0, y=0, z=255), struct.pack(">I", 0)]),
        ("truncated MMIX .mmo object", "tetra 3"),
    ),
    MMIXLoaderFailure(
        "mmo-post-trailing-records",
        mmo_image(
            [
                mmo_post(R255, {R255: 0}),
                halt(),
            ]
        ),
        ("expected MMIX .mmo lop_stab after postamble", "tetra 5"),
    ),
    MMIXLoaderFailure(
        "mmo-spec-unterminated",
        mmo_image([mmo_lop(MMIX_MMO_LOP_SPEC, 1)]),
        ("unterminated MMIX .mmo lop_spec", "tetra 2"),
    ),
    MMIXLoaderFailure(
        "mmo-spec-invalid-quote",
        mmo_image([mmo_spec(1), mmo_lop(MMIX_MMO_LOP_QUOTE, 2)]),
        ("invalid MMIX .mmo lop_quote yz=2 in lop_spec", "tetra 3"),
    ),
    MMIXLoaderFailure(
        "mmo-spec-truncated-quote-payload",
        mmo_image([mmo_spec(1), mmo_lop(MMIX_MMO_LOP_QUOTE, 1)]),
        ("truncated MMIX .mmo object", "tetra 3"),
    ),
    MMIXLoaderFailure(
        "mmo-stab-invalid-yz",
        mmo_image([mmo_post(R255, {R255: 0}), mmo_lop(MMIX_MMO_LOP_STAB, 1)]),
        ("invalid MMIX .mmo lop_stab yz=1", "tetra 5"),
    ),
    MMIXLoaderFailure(
        "mmo-end-invalid-count",
        mmo_image(
            [
                mmo_post(R255, {R255: 0}),
                mmo_lop(MMIX_MMO_LOP_STAB, 0),
                struct.pack(">I", 0),
                mmo_lop(MMIX_MMO_LOP_END, 0),
            ]
        ),
        ("invalid MMIX .mmo lop_end yz=0", "expected 1"),
    ),
    MMIXLoaderFailure(
        "mmo-records-after-end",
        mmo_image([mmo_post(R255, {R255: 0}), mmo_stab_end(), halt()]),
        ("unsupported MMIX .mmo records after lop_end", "tetra 7"),
    ),
]
