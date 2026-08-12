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
Uart    GREG    #10000000
        LOC     #0
Main    SETL    $2,'Q'
        STBU    $2,Uart,#0
        SETL    $2,'E'
        STBU    $2,Uart,#0
        SETL    $2,'M'
        STBU    $2,Uart,#0
        SETL    $2,'U'
        STBU    $2,Uart,#0
        SETL    $2,#a
        STBU    $2,Uart,#0
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

MMIXAL_STDIN_READ_SOURCE = """\
        LOC     Data_Segment
Read    OCTA    Buffer,0
Write   OCTA    Buffer,0
Buffer  OCTA    0,0
ReadPtr GREG    Read
WritePtr GREG   Write
        LOC     #100
Main    SET     $2,11
        STOU    $2,ReadPtr,8
        STOU    $2,WritePtr,8
        ADDU    $255,ReadPtr,0
        TRAP    0,Fread,StdIn
        ADDU    $255,WritePtr,0
        TRAP    0,Fwrite,StdOut
        SET     $255,0
        TRAP    0,Halt,0
"""

MMIXAL_STDIN_FGETS_SOURCE = """\
        LOC     Data_Segment
Read    OCTA    Buffer,16
Buffer  OCTA    0,0
ReadPtr GREG    Read
BufPtr  GREG    Buffer
        LOC     #100
Main    ADDU    $255,ReadPtr,0
        TRAP    0,Fgets,StdIn
        ADDU    $255,BufPtr,0
        TRAP    0,Fputs,StdOut
        SET     $255,0
        TRAP    0,Halt,0
"""


def mmixal_file_read_source(size):
    return f"""\
argv    IS      $1
File    IS      3
        LOC     Data_Segment
Open    OCTA    0,TextRead
Read    OCTA    Buffer,{size}
Buffer  OCTA    0,0
OpenPtr GREG    Open
ReadPtr GREG    Read
BufPtr  GREG    Buffer
        LOC     #100
Main    LDOU    $2,argv,0
        STOU    $2,OpenPtr,0
        ADDU    $255,OpenPtr,0
        TRAP    0,Fopen,File
        ADDU    $255,ReadPtr,0
        TRAP    0,Fread,File
        ADDU    $255,BufPtr,0
        TRAP    0,Fputs,StdOut
        TRAP    0,Fclose,File
        SET     $255,0
        TRAP    0,Halt,0
"""


def _byte_operands(data):
    return ",".join(f"#{byte:02x}" for byte in data)


def _byte_directives(label, data):
    chunks = [data[i:i + 8] for i in range(0, len(data), 8)]
    lines = []

    for i, chunk in enumerate(chunks):
        prefix = f"{label:<8}BYTE    " if i == 0 else "        BYTE    "
        lines.append(prefix + _byte_operands(chunk))
    return "\n".join(lines)


def mmixal_file_write_source(data):
    buffer = _byte_directives("Buffer", data)

    return f"""\
argv    IS      $1
File    IS      3
        LOC     Data_Segment
Open    OCTA    0,TextWrite
Write   OCTA    Buffer,{len(data)}
{buffer}
OpenPtr GREG    Open
WritePtr GREG   Write
        LOC     #100
Main    LDOU    $2,argv,0
        STOU    $2,OpenPtr,0
        ADDU    $255,OpenPtr,0
        TRAP    0,Fopen,File
        ADDU    $255,WritePtr,0
        TRAP    0,Fwrite,File
        TRAP    0,Fclose,File
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
    stdin_data: bytes = None

    def build(self, mmixal, workdir):
        return MMIXSerialTest(
            self.name,
            assemble_mmixal_mmo(mmixal, workdir, self.object_name,
                                self.source, self.source_path),
            pc=self.pc,
            output=self.output,
            qemu_args=self.qemu_args,
            exit_status=self.exit_status,
            stdin_data=self.stdin_data,
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


def mmixal_file_read_case(pathname, data):
    return MMIXALSerialCase(
        "mmixal-mmo-file-read",
        "file_read",
        pc=0x128,
        output=data,
        source=mmixal_file_read_source(len(data)),
        qemu_args=("-semihosting-config", f"enable=on,arg={pathname}"),
    )


def mmixal_file_write_case(pathname, data):
    return MMIXALSerialCase(
        "mmixal-mmo-file-write",
        "file_write",
        pc=0x120,
        output=b"",
        source=mmixal_file_write_source(data),
        qemu_args=("-semihosting-config", f"enable=on,arg={pathname}"),
    )


MMIXAL_SERIAL_TESTS = [
    MMIXALSerialCase(
        "mmixal-mmo-uart-output",
        "uart",
        pc=0x28,
        output=b"QEMU\n",
        source=MMIXAL_UART_SOURCE,
    ),
]

MMIXAL_SEMIHOSTING_CONSOLE_TESTS = [
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
    MMIXALSerialCase(
        "mmixal-mmo-stdin-fread",
        "stdin_fread",
        pc=0x120,
        output=b"MMIX stdin\n",
        source=MMIXAL_STDIN_READ_SOURCE,
        stdin_data=b"MMIX stdin\n",
    ),
    MMIXALSerialCase(
        "mmixal-mmo-stdin-fgets",
        "stdin_fgets",
        pc=0x114,
        output=b"MMIX fgets\n",
        source=MMIXAL_STDIN_FGETS_SOURCE,
        stdin_data=b"MMIX fgets\nignored",
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
