#!/usr/bin/env python3
#
# MMIX GDB Remote Serial Protocol tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct
import xml.etree.ElementTree as ET

from cases.common import INITIAL_STACK, MMIX_VIRT_BOOTINFO, MMIX_VIRT_MEMMAP
from cases.debugger import DEBUGGER_ELF_IMAGE, DEBUGGER_ENTRY
from lib.rsp import QEMURSPServer


MMIX_GDB_GENERAL_REGS = 256
MMIX_GDB_SPECIAL_REGS = (
    "rB", "rD", "rE", "rH", "rJ", "rM", "rR", "rBB",
    "rC", "rN", "rO", "rS", "rI", "rT", "rTT", "rK",
    "rQ", "rU", "rV", "rG", "rL", "rA", "rF", "rP",
    "rW", "rX", "rY", "rZ", "rWW", "rXX", "rYY", "rZZ",
)
MMIX_GDB_PC_REG = MMIX_GDB_GENERAL_REGS + len(MMIX_GDB_SPECIAL_REGS)
MMIX_GDB_REGS = MMIX_GDB_PC_REG + 1


def _write_debugger_fixture(workdir):
    image = workdir / "debugger-fixture.elf"

    image.write_bytes(DEBUGGER_ELF_IMAGE)
    return image


def test_rsp_initial_stop(qemu, workdir):
    image = _write_debugger_fixture(workdir)

    with QEMURSPServer(qemu, image, workdir) as server:
        process = server.process
        socket_path = server.socket_path
        features = server.client.request(
            "qSupported:multiprocess+;qXfer:features:read+"
        )
        assert b"PacketSize=" in features
        assert b"qXfer:features:read+" in features

        stop = server.client.request("?")
        assert stop.startswith((b"S05", b"T05"))

    assert process.poll() is not None
    assert not socket_path.exists()


def test_rsp_register_description_and_reads(qemu, workdir):
    image = _write_debugger_fixture(workdir)

    with QEMURSPServer(qemu, image, workdir) as server:
        target_xml = server.client.read_xfer("features", "target.xml")
        assert b'mmix-core.xml' in target_xml

        core_xml = server.client.read_xfer("features", "mmix-core.xml")
        feature = ET.fromstring(core_xml)
        registers = feature.findall("reg")
        assert len(registers) == MMIX_GDB_REGS

        expected_names = [
            *(f"r{reg}" for reg in range(MMIX_GDB_GENERAL_REGS)),
            *MMIX_GDB_SPECIAL_REGS,
            "pc",
        ]
        for number, (register, name) in enumerate(
            zip(registers, expected_names, strict=True)
        ):
            assert register.get("name") == name
            assert register.get("regnum") == str(number)
            assert register.get("bitsize") == "64"
            expected_type = "code_ptr" if name == "pc" else "uint64"
            assert register.get("type") == expected_type

        assert registers[253].get("altname") == "fp"
        assert registers[253].get("generic") == "fp"
        assert registers[254].get("altname") == "sp"
        assert registers[254].get("generic") == "sp"
        assert registers[MMIX_GDB_PC_REG].get("generic") == "pc"

        encoded_registers = server.client.request("g")
        assert len(encoded_registers) == MMIX_GDB_REGS * 16
        register_data = bytes.fromhex(encoded_registers.decode("ascii"))

        for number in range(MMIX_GDB_REGS):
            encoded = server.client.request(f"p{number:x}")
            assert len(encoded) == 16
            value = bytes.fromhex(encoded.decode("ascii"))
            offset = number * 8
            assert value == register_data[offset:offset + 8]

        ro = MMIX_GDB_GENERAL_REGS + MMIX_GDB_SPECIAL_REGS.index("rO")
        assert register_data[8:16] == struct.pack(
            ">Q", MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0]
        )
        assert register_data[ro * 8:(ro + 1) * 8] == struct.pack(
            ">Q", INITIAL_STACK
        )
        assert register_data[-8:] == struct.pack(">Q", DEBUGGER_ENTRY)
