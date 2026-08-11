#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses
import pathlib
import shutil
import subprocess

from .common import *

MMIX_TEST_DIR = pathlib.Path(__file__).resolve().parents[1]
MMIXAL_DATA_DIR = MMIX_TEST_DIR / "data" / "mmixal"
MMIXAL_HELLO_SOURCE = MMIXAL_DATA_DIR / "hello.mms"
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
        SET     $255,0
        TRAP    0,Halt,0
"""


@dataclasses.dataclass(frozen=True)
class MMIXALSerialCase:
    name: str
    object_name: str
    pc: int
    output: bytes
    source: str = None
    source_path: pathlib.Path = None
    qemu_args: tuple[str, ...] = ()
    exit_status: int = 0

    def build(self, mmixal, workdir):
        return MMIXSerialTest(
            self.name,
            assemble_mmixal_mmo(mmixal, workdir, self.object_name,
                                self.source, self.source_path),
            pc=self.pc,
            output=self.output,
            qemu_args=self.qemu_args,
            exit_status=self.exit_status,
        )


@dataclasses.dataclass(frozen=True)
class MMIXALMMOCase:
    name: str
    object_name: str
    pc: int
    regs: dict[int, int]
    source: str = None
    source_path: pathlib.Path = None

    def build(self, mmixal, workdir):
        return MMIXMMOTest(
            self.name,
            assemble_mmixal_mmo(mmixal, workdir, self.object_name,
                                self.source, self.source_path),
            pc=self.pc,
            regs=self.regs,
        )


def assemble_mmixal_mmo(mmixal, workdir, name, source=None, source_path=None):
    source_dir = workdir / "mmixal-fixtures"
    source_dir.mkdir(parents=True, exist_ok=True)
    object_path = source_dir / f"{name}.mmo"

    if source_path is None:
        source_path = source_dir / f"{name}.mms"
        source_path.write_text(source, encoding="ascii")
    else:
        original_source_path = pathlib.Path(source_path)
        source_path = source_dir / original_source_path.name
        shutil.copyfile(original_source_path, source_path)

    subprocess.run(
        [mmixal, "-o", object_path.name, source_path.name],
        cwd=source_dir,
        check=True,
        timeout=10,
    )
    return object_path.read_bytes()


MMIXAL_SERIAL_TESTS = [
    MMIXALSerialCase(
        "mmixal-mmo-uart-output",
        "uart",
        pc=0x28,
        output=b"QEMU\n",
        source=MMIXAL_UART_SOURCE,
    ),
]

MMIXAL_SEMIHOSTING_SERIAL_TESTS = [
    MMIXALSerialCase(
        "mmixal-mmo-hosted-fputs-output",
        "hosted",
        pc=0x0c,
        output=b"Hosted MMIXAL\n",
        source=MMIXAL_HOSTED_SOURCE,
    ),
    MMIXALSerialCase(
        "mmixal-mmo-hello-argv",
        "hello",
        pc=0x110,
        output=b"hello, world\n",
        source_path=MMIXAL_HELLO_SOURCE,
        qemu_args=("-semihosting-config", "enable=on,arg=hello"),
        exit_status=8,
    ),
    MMIXALSerialCase(
        "mmixal-mmo-prime-table",
        "prime_table",
        pc=0x1b8,
        output=MMIXAL_PRIME_TABLE_OUTPUT.read_bytes(),
        source_path=MMIXAL_PRIME_TABLE_SOURCE,
    ),
]


MMIXAL_MMO_TESTS = [
    MMIXALMMOCase(
        "mmixal-mmo-data-segment-read-write",
        "data_rw",
        pc=0x10,
        regs={
            R2: 0x1122334455667788,
            R3: 0x2211,
            R4: 0x2211,
            R254: MMIX_DATA_SEGMENT_BASE,
        },
        source=MMIXAL_DATA_RW_SOURCE,
    ),
    MMIXALMMOCase(
        "mmixal-mmo-greg-data-label",
        "greg_data",
        pc=0x08,
        regs={
            R2: MMIX_DATA_SEGMENT_BASE,
            R3: 0x0102030405060708,
            R254: MMIX_DATA_SEGMENT_BASE,
        },
        source=MMIXAL_GREG_DATA_SOURCE,
    ),
]
