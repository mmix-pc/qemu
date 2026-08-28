#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import (
    MASK64,
    MMIXProcessFailure,
    PT_INTERP,
    PT_LOAD,
    elf64_header,
    elf64_image,
    elf64_patch_ehdr_field,
    elf64_patch_phdr_field,
    elf64_phdr,
    halt,
)


PREFLIGHT_COMPLETE = (
    "MMIX ELF -kernel preflight succeeded; loading awaits direct-entry "
    "startup implementation"
)


def elf64_segments(segments, entry):
    header = elf64_header(entry=entry, phnum=len(segments))
    headers = b"".join(segment[0] for segment in segments)
    image = bytearray(header + headers)

    for _, offset, data in segments:
        if len(image) < offset + len(data):
            image.extend(bytes(offset + len(data) - len(image)))
        image[offset:offset + len(data)] = data
    return bytes(image)


def segment(address, data, *, offset, mem_size=None, flags=5,
            ph_type=PT_LOAD, virtual_address=None, alignment=1):
    return (
        elf64_phdr(
            address,
            data,
            mem_size=mem_size,
            offset=offset,
            ph_type=ph_type,
            flags=flags,
            virtual_address=virtual_address,
            alignment=alignment,
        ),
        offset,
        data,
    )


SIMPLE = elf64_image(0, halt())
TWO_SEGMENTS_ONE_PAGE = elf64_segments(
    (
        segment(0x2000, halt(), offset=0x200),
        segment(0x2100, b"DATA", offset=0x300, flags=6),
    ),
    entry=0x2000,
)
WITH_INTERPRETER = elf64_segments(
    (
        segment(0, halt(), offset=0x200),
        segment(0, b"/ld.so\0", offset=0x300, ph_type=PT_INTERP, flags=4),
    ),
    entry=0,
)
OVERLAPPING_SEGMENTS = elf64_segments(
    (
        segment(0x2000, halt(), offset=0x200, mem_size=0x100),
        segment(0x2080, b"DATA", offset=0x300, mem_size=0x100, flags=6),
    ),
    entry=0x2000,
)


ELF_PREFLIGHT_TESTS = [
    MMIXProcessFailure(
        "elf-preflight-simple",
        SIMPLE,
        (),
        (PREFLIGHT_COMPLETE,),
    ),
    MMIXProcessFailure(
        "elf-preflight-segments-share-reservation-page",
        TWO_SEGMENTS_ONE_PAGE,
        (),
        (PREFLIGHT_COMPLETE,),
    ),
    MMIXProcessFailure(
        "elf-preflight-ram-endpoint",
        elf64_image(128 * 1024 * 1024 - 4, halt(),
                    entry=128 * 1024 * 1024 - 4),
        ("-m", "128M"),
        (PREFLIGHT_COMPLETE,),
    ),
    MMIXProcessFailure(
        "elf-preflight-above-4g",
        elf64_image(0x100000000, halt(), entry=0x100000000),
        ("-m", "8G"),
        (PREFLIGHT_COMPLETE,),
    ),
    MMIXProcessFailure(
        "elf-preflight-invalid-header-size",
        elf64_patch_ehdr_field(SIMPLE, "header_size", 63),
        (),
        ("invalid MMIX ELF header",),
    ),
    MMIXProcessFailure(
        "elf-preflight-no-program-headers",
        elf64_header(),
        (),
        ("invalid MMIX ELF program header table",),
    ),
    MMIXProcessFailure(
        "elf-preflight-no-nonempty-load-segment",
        elf64_image(0, b"", mem_size=0),
        (),
        ("has no nonempty PT_LOAD segment",),
    ),
    MMIXProcessFailure(
        "elf-preflight-invalid-program-entry-size",
        elf64_patch_ehdr_field(SIMPLE, "program_entry_size", 55),
        (),
        ("invalid MMIX ELF program header table",),
    ),
    MMIXProcessFailure(
        "elf-preflight-extended-program-numbering",
        elf64_patch_ehdr_field(SIMPLE, "program_count", 0xffff),
        (),
        ("unsupported MMIX ELF extended program header numbering",),
    ),
    MMIXProcessFailure(
        "elf-preflight-program-table-offset-overflow",
        elf64_patch_ehdr_field(SIMPLE, "program_offset", MASK64),
        (),
        ("truncated MMIX ELF program header table",),
    ),
    MMIXProcessFailure(
        "elf-preflight-interpreter",
        WITH_INTERPRETER,
        (),
        ("unsupported MMIX ELF interpreter segment",),
    ),
    MMIXProcessFailure(
        "elf-preflight-filesz-exceeds-memsz",
        elf64_patch_phdr_field(SIMPLE, 0, "memory_size", 3),
        (),
        ("invalid MMIX ELF PT_LOAD segment 0",),
    ),
    MMIXProcessFailure(
        "elf-preflight-truncated-segment-data",
        elf64_patch_phdr_field(SIMPLE, 0, "file_size", 8),
        (),
        ("invalid MMIX ELF PT_LOAD segment 0",),
    ),
    MMIXProcessFailure(
        "elf-preflight-non-power-of-two-alignment",
        elf64_patch_phdr_field(SIMPLE, 0, "alignment", 3),
        (),
        ("invalid MMIX ELF PT_LOAD alignment",),
    ),
    MMIXProcessFailure(
        "elf-preflight-incongruent-alignment",
        elf64_patch_phdr_field(SIMPLE, 0, "alignment", 0x1000),
        (),
        ("invalid MMIX ELF PT_LOAD alignment",),
    ),
    MMIXProcessFailure(
        "elf-preflight-nonidentity-address",
        elf64_patch_phdr_field(SIMPLE, 0, "virtual_address", 0x1000),
        (),
        ("does not use identical virtual and physical addresses",),
    ),
    MMIXProcessFailure(
        "elf-preflight-outside-ram",
        elf64_image(128 * 1024 * 1024 - 4, halt(), mem_size=8,
                    entry=128 * 1024 * 1024 - 4),
        ("-m", "128M"),
        ("targets non-RAM physical range",),
    ),
    MMIXProcessFailure(
        "elf-preflight-overlapping-segments",
        OVERLAPPING_SEGMENTS,
        (),
        ("PT_LOAD segments 0 and 1", "overlap"),
    ),
    MMIXProcessFailure(
        "elf-preflight-unaligned-entry",
        elf64_patch_ehdr_field(SIMPLE, "entry", 2),
        (),
        ("is not a complete aligned instruction",),
    ),
    MMIXProcessFailure(
        "elf-preflight-entry-outside-segment",
        elf64_patch_ehdr_field(SIMPLE, "entry", 0x1000),
        (),
        ("is not a complete aligned instruction",),
    ),
    MMIXProcessFailure(
        "elf-preflight-entry-in-nonexecutable-segment",
        elf64_patch_phdr_field(SIMPLE, 0, "flags", 6),
        (),
        ("is not a complete aligned instruction",),
    ),
    MMIXProcessFailure(
        "elf-preflight-incomplete-entry-instruction",
        elf64_image(0, b"\0\0", entry=0),
        (),
        ("is not a complete aligned instruction",),
    ),
]
