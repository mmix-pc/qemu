#!/usr/bin/env python3
#
# MMIX GDB Remote Serial Protocol tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct
import xml.etree.ElementTree as ET

from cases.common import INITIAL_STACK, MMIX_VIRT_BOOTINFO, MMIX_VIRT_MEMMAP
from cases.debugger import (
    DEBUGGER_ELF_IMAGE,
    DEBUGGER_ENTRY,
    DEBUGGER_WINDOW_BODY,
    DEBUGGER_WINDOW_RETURN,
)
from lib.rsp import QEMURSPServer


MMIX_GDB_GENERAL_REGS = 256
MMIX_GDB_SPECIAL_REGS = (
    "rB", "rD", "rE", "rH", "rJ", "rM", "rR", "rBB",
    "rC", "rN", "rO", "rS", "rI", "rT", "rTT", "rK",
    "rQ", "rU", "rV", "rG", "rL", "rA", "rF", "rP",
    "rW", "rX", "rY", "rZ", "rWW", "rXX", "rYY", "rZZ",
)
MMIX_GDB_DIRECT_SPECIAL_REGS = (
    "rB", "rD", "rE", "rH", "rJ", "rM", "rR", "rBB",
    "rC", "rN", "rI", "rT", "rTT", "rU", "rF", "rP",
    "rW", "rX", "rY", "rZ", "rWW", "rXX", "rYY", "rZZ",
)
MMIX_GDB_PC_REG = MMIX_GDB_GENERAL_REGS + len(MMIX_GDB_SPECIAL_REGS)
MMIX_GDB_REGS = MMIX_GDB_PC_REG + 1


def _write_debugger_fixture(workdir):
    image = workdir / "debugger-fixture.elf"

    image.write_bytes(DEBUGGER_ELF_IMAGE)
    return image


def _read_register(client, number):
    encoded = client.request(f"p{number:x}")

    assert len(encoded) == 16
    return struct.unpack(">Q", bytes.fromhex(encoded.decode("ascii")))[0]


def _write_register(client, number, value):
    encoded = struct.pack(">Q", value).hex()

    assert client.request(f"P{number:x}={encoded}") == b"OK"


def _special_register_number(name):
    return MMIX_GDB_GENERAL_REGS + MMIX_GDB_SPECIAL_REGS.index(name)


def _step(client):
    stop = client.request("s")

    assert stop.startswith((b"S05", b"T05"))


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


def test_rsp_general_register_writes_follow_logical_window(qemu, workdir):
    image = _write_debugger_fixture(workdir)
    values = {
        0: 0x0102030405060708,
        2: 0x1112131415161718,
    }
    values.update({reg: 0x8000000000000000 | reg for reg in range(224, 256)})

    with QEMURSPServer(qemu, image, workdir) as server:
        client = server.client
        ro = MMIX_GDB_GENERAL_REGS + MMIX_GDB_SPECIAL_REGS.index("rO")
        rs = MMIX_GDB_GENERAL_REGS + MMIX_GDB_SPECIAL_REGS.index("rS")
        rg = MMIX_GDB_GENERAL_REGS + MMIX_GDB_SPECIAL_REGS.index("rG")
        rl = MMIX_GDB_GENERAL_REGS + MMIX_GDB_SPECIAL_REGS.index("rL")
        initial_ro = _read_register(client, ro)
        initial_rs = _read_register(client, rs)

        assert _read_register(client, rl) == 2
        assert _read_register(client, rg) == 32
        for number, value in values.items():
            _write_register(client, number, value)
            assert _read_register(client, number) == value
        assert _read_register(client, rl) == 3
        assert _read_register(client, ro) == initial_ro
        assert _read_register(client, rs) == initial_rs

        for _ in range(5):
            _step(client)
        observed_registers = ((39, 0), (40, 2), (41, 224),
                              (42, 253), (43, 254))
        for number, source in observed_registers:
            assert _read_register(client, number) == values[source]

        _step(client)
        assert _read_register(client, MMIX_GDB_PC_REG) == DEBUGGER_WINDOW_BODY
        assert _read_register(client, ro) != initial_ro
        assert _read_register(client, rs) == initial_rs

        nested_value = 0x5152535455565758
        _write_register(client, 0, nested_value)
        assert _read_register(client, 0) == nested_value
        _step(client)
        assert _read_register(client, 44) == nested_value

        _step(client)
        assert _read_register(client, MMIX_GDB_PC_REG) == DEBUGGER_WINDOW_RETURN
        assert _read_register(client, ro) == initial_ro
        assert _read_register(client, 0) == values[0]


def test_rsp_special_register_writes_preserve_cpu_invariants(qemu, workdir):
    image = _write_debugger_fixture(workdir)

    with QEMURSPServer(qemu, image, workdir) as server:
        client = server.client
        ro = _special_register_number("rO")
        rs = _special_register_number("rS")
        rg = _special_register_number("rG")
        rl = _special_register_number("rL")
        ra = _special_register_number("rA")
        rk = _special_register_number("rK")
        rq = _special_register_number("rQ")
        initial_ro = _read_register(client, ro)
        initial_rs = _read_register(client, rs)

        for index, name in enumerate(MMIX_GDB_DIRECT_SPECIAL_REGS):
            number = _special_register_number(name)
            value = 0x4D4D495800000000 | index

            _write_register(client, number, value)
            assert _read_register(client, number) == value

        _write_register(client, ra, 0x12345)
        assert _read_register(client, ra) == 0x12345
        _write_register(client, ra, 1 << 20)
        assert _read_register(client, ra) == 0x12345

        _write_register(client, rg, 40)
        assert _read_register(client, rg) == 40
        _write_register(client, rg, 31)
        assert _read_register(client, rg) == 40
        _write_register(client, rg, 32)

        old_r1 = _read_register(client, 1)
        _write_register(client, rl, 1)
        assert _read_register(client, rl) == 1
        _write_register(client, rl, 2)
        assert _read_register(client, rl) == 2
        assert _read_register(client, 1) == 0
        _write_register(client, 1, old_r1)
        _write_register(client, rl, 33)
        assert _read_register(client, rl) == 2

        local_values = (0x1011121314151617, 0x2021222324252627)
        for number, value in enumerate(local_values):
            _write_register(client, number, value)
        _write_register(client, ro, initial_ro + 8)
        assert _read_register(client, ro) == initial_ro + 8
        for number, value in enumerate(local_values):
            assert _read_register(client, number) == value

        _write_register(client, ro, initial_ro + 1)
        assert _read_register(client, ro) == initial_ro + 8
        _write_register(client, rs, initial_ro + 16)
        assert _read_register(client, rs) == initial_rs
        _write_register(client, rs, initial_rs + 8)
        assert _read_register(client, rs) == initial_rs + 8
        _write_register(client, rs, initial_rs)
        _write_register(client, ro, initial_ro)

        _write_register(client, rq, 1 << 8)
        assert _read_register(client, rq) == 0
        _write_register(client, rq, 1 << 32)
        assert _read_register(client, rq) == 1 << 32
        _write_register(client, rq, 0)

        _write_register(client, rk, 1 << 32)
        assert _read_register(client, rk) == 1 << 32
        _write_register(client, rk, 0)


def test_rsp_special_register_write_updates_translation_state(qemu, workdir):
    image = _write_debugger_fixture(workdir)

    with QEMURSPServer(qemu, image, workdir) as server:
        client = server.client
        rv = _special_register_number("rV")
        value = _read_register(client, rv) ^ (1 << 3)

        _write_register(client, rv, value)
        assert _read_register(client, rv) == value


def test_rsp_pc_write_preserves_instruction_alignment(qemu, workdir):
    image = _write_debugger_fixture(workdir)

    with QEMURSPServer(qemu, image, workdir) as server:
        client = server.client

        _write_register(client, MMIX_GDB_PC_REG, DEBUGGER_ENTRY + 1)
        assert _read_register(client, MMIX_GDB_PC_REG) == DEBUGGER_ENTRY

        source = 0x3132333435363738
        _write_register(client, 2, source)
        _write_register(client, MMIX_GDB_PC_REG, DEBUGGER_ENTRY + 4)
        assert _read_register(client, MMIX_GDB_PC_REG) == DEBUGGER_ENTRY + 4
        _step(client)
        assert _read_register(client, 40) == source
        assert _read_register(client, MMIX_GDB_PC_REG) == DEBUGGER_ENTRY + 8
