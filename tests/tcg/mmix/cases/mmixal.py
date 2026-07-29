#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib
import shutil
import subprocess

from .common import *

MMIX_TEST_DIR = pathlib.Path(__file__).resolve().parents[1]
MMIXAL_DATA_DIR = MMIX_TEST_DIR / "data" / "mmixal"
MMIXAL_PRIME_TABLE_SOURCE = MMIXAL_DATA_DIR / "prime_table.mms"
MMIXAL_PRIME_TABLE_OUTPUT = MMIXAL_DATA_DIR / "prime_table.out"

MMIXAL_DATA_RW_SOURCE = """\
        LOC     Data_Segment
Base    GREG    @
Data    OCTA    #1122334455667788
Out     OCTA    0
        LOC     #0
Main    LDOU    $2,Data
        SETL    $3,#2211
        STOU    $3,Out
        LDOU    $4,Out
        TRAP    0,0,0
"""

MMIXAL_GREG_DATA_SOURCE = """\
        LOC     Data_Segment
Data    OCTA    #0102030405060708
Ptr     GREG    Data
        LOC     #0
Main    ADDU    $2,Ptr,0
        LDOU    $3,Ptr,0
        TRAP    0,0,0
"""

MMIXAL_UART_SOURCE = """\
Uart    GREG    #100000000
        LOC     #0
Main    SETL    $2,'Q'
        STBU    $2,Uart,#4
        SETL    $2,'E'
        STBU    $2,Uart,#4
        SETL    $2,'M'
        STBU    $2,Uart,#4
        SETL    $2,'U'
        STBU    $2,Uart,#4
        SETL    $2,#a
        STBU    $2,Uart,#4
        TRAP    0,0,0
"""

MMIXAL_HOSTED_SOURCE = """\
        LOC     Data_Segment
Msg     BYTE    "Hosted MMIXAL",#a,0
Ptr     GREG    Msg
        LOC     #0
Main    ADDU    $255,Ptr,0
        TRAP    0,Fputs,StdOut
        TRAP    0,Halt,0
"""


def assemble_mmixal_mmo(mmixal, workdir, name, source):
    source_dir = workdir / "mmixal-fixtures"
    source_dir.mkdir(parents=True, exist_ok=True)
    source_path = source_dir / f"{name}.mms"
    object_path = source_dir / f"{name}.mmo"

    source_path.write_text(source, encoding="ascii")
    subprocess.run(
        [mmixal, "-o", object_path.name, source_path.name],
        cwd=source_dir,
        check=True,
        timeout=10,
    )
    return object_path.read_bytes()


def assemble_mmixal_mmo_from_file(mmixal, workdir, name, source_path):
    source_dir = workdir / "mmixal-fixtures"
    source_dir.mkdir(parents=True, exist_ok=True)
    source_path = pathlib.Path(source_path)
    fixture_source = source_dir / source_path.name
    object_path = source_dir / f"{name}.mmo"

    shutil.copyfile(source_path, fixture_source)
    subprocess.run(
        [mmixal, "-o", object_path.name, fixture_source.name],
        cwd=source_dir,
        check=True,
        timeout=10,
    )
    return object_path.read_bytes()


def mmixal_tests(mmixal, workdir):
    return [
        MMIXSerialTest(
            "mmixal-mmo-uart-output",
            assemble_mmixal_mmo(mmixal, workdir, "uart", MMIXAL_UART_SOURCE),
            pc=0x28,
            output=b"QEMU\n",
        ),
        MMIXSerialTest(
            "mmixal-mmo-hosted-fputs-output",
            assemble_mmixal_mmo(mmixal, workdir, "hosted",
                                MMIXAL_HOSTED_SOURCE),
            pc=0x08,
            output=b"Hosted MMIXAL\n",
        ),
        MMIXSerialTest(
            "mmixal-mmo-prime-table",
            assemble_mmixal_mmo_from_file(mmixal, workdir, "prime_table",
                                          MMIXAL_PRIME_TABLE_SOURCE),
            pc=0x1b8,
            output=MMIXAL_PRIME_TABLE_OUTPUT.read_bytes(),
        ),
    ], [
        MMIXMMOTest(
            "mmixal-mmo-data-segment-read-write",
            assemble_mmixal_mmo(mmixal, workdir, "data_rw",
                                MMIXAL_DATA_RW_SOURCE),
            pc=0x10,
            regs={
                2: 0x1122334455667788,
                3: 0x2211,
                4: 0x2211,
                254: MMIX_DATA_SEGMENT_BASE,
            },
        ),
        MMIXMMOTest(
            "mmixal-mmo-greg-data-label",
            assemble_mmixal_mmo(mmixal, workdir, "greg_data",
                                MMIXAL_GREG_DATA_SOURCE),
            pc=0x08,
            regs={
                2: MMIX_DATA_SEGMENT_BASE,
                3: 0x0102030405060708,
                254: MMIX_DATA_SEGMENT_BASE,
            },
        ),
    ]
