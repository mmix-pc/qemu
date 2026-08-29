#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import contextlib
import json
import socket
import subprocess
import time

import pytest

from cases.common import (
    MMIX_DATA_SEGMENT_BASE,
    MMIX_HOSTED_LIMIT,
    MMIX_POOL_SEGMENT_BASE,
    MMIX_STACK_SEGMENT_BASE,
)
from cases.elf_bare import BARE_ELF_TESTS, BARE_ENTRY, BARE_PROGRAM
from cases.mmo_hosted import (
    mmo_hosted_debug_image,
    mmo_hosted_open_file_debug_image,
)
from cases.raw_loader import RAW_DIRECT_ISA_TESTS, RAW_ENTRY
from lib.gdb_remote import GDBRemote
from lib.mmix_asm import halt
from lib.qemu import build_kernel_command, read_log


MMIX_GDB_SPECIAL_REGISTER_BASE = 32
MMIX_GDB_RO = MMIX_GDB_SPECIAL_REGISTER_BASE + 10
MMIX_GDB_RS = MMIX_GDB_SPECIAL_REGISTER_BASE + 11
MMIX_GDB_RL = MMIX_GDB_SPECIAL_REGISTER_BASE + 20
MMIX_GDB_PC = MMIX_GDB_SPECIAL_REGISTER_BASE + 32


@contextlib.contextmanager
def _gdb_session(qemu, workdir, name, image, *, suffix=".mmo", qemu_args=()):
    image_path = workdir / f"{name}{suffix}"
    socket_path = workdir / f"{name}.gdb"
    log_path = workdir / f"{name}.log"

    image_path.write_bytes(image)
    for path in (socket_path, log_path):
        if path.exists():
            path.unlink()

    debug_args = [
        *qemu_args,
        "-gdb",
        f"unix:path={socket_path},server=on,wait=off",
        "-S",
    ]
    process = subprocess.Popen(
        build_kernel_command(
            qemu,
            image_path,
            trace="int",
            log=log_path,
            qemu_args=debug_args,
        ),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    remote = None
    try:
        remote = GDBRemote.connect(socket_path)
        yield remote, process, log_path
    finally:
        if remote is not None:
            remote.close()
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        if socket_path.exists():
            socket_path.unlink()


def _read_qmp_message(process, timeout=5):
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        line = process.stdout.readline()
        if line:
            return json.loads(line)
        if process.poll() is not None:
            raise AssertionError("QEMU exited while waiting for QMP")
    raise TimeoutError("timed out waiting for QMP")


def _qmp_command(process, command, arguments=None):
    request = {"execute": command}

    if arguments is not None:
        request["arguments"] = arguments
    process.stdin.write(json.dumps(request).encode("utf-8") + b"\n")
    process.stdin.flush()
    while True:
        response = _read_qmp_message(process)
        if "event" in response:
            continue
        if "error" in response:
            raise AssertionError(response["error"])
        return response["return"]


def _connect_unix(path, timeout=5):
    deadline = time.monotonic() + timeout

    while True:
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            connection.connect(str(path))
            connection.settimeout(timeout)
            return connection
        except (FileNotFoundError, ConnectionRefusedError):
            connection.close()
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.01)


def _qtest_command(stream, command):
    stream.write(command.encode("ascii") + b"\n")
    response = stream.readline()

    if not response.startswith(b"OK"):
        raise AssertionError(
            f"QTest command {command!r} failed: {response!r}"
        )
    return response.split()


def test_mmo_hosted_debug_memory(qemu, workdir):
    physical_address = 0x01000000
    qemu_args = (
        "-device",
        f"loader,data=0x8877665544332211,data-len=8,"
        f"addr={physical_address:#x},data-be=on",
    )

    with _gdb_session(
        qemu,
        workdir,
        "mmo-hosted-debug-memory",
        mmo_hosted_debug_image(),
        qemu_args=qemu_args,
    ) as (remote, _process, _log):
        assert remote.read_memory(
            MMIX_DATA_SEGMENT_BASE + 0x100, 8
        ) == bytes.fromhex("1122334455667788")
        assert remote.read_memory(
            MMIX_POOL_SEGMENT_BASE + 0x4000, 8
        ) == bytes(8)

        marker = bytes.fromhex("a1b2c3d4e5f60718")
        remote.write_memory(MMIX_STACK_SEGMENT_BASE + 0x4000, marker)
        assert remote.read_memory(
            MMIX_STACK_SEGMENT_BASE + 0x4000, 8
        ) == marker

        assert remote.read_memory(physical_address, 8) == bytes(8)
        assert remote.memory_error(MMIX_HOSTED_LIMIT, 1) == b"E14"
        assert remote.memory_error(MMIX_POOL_SEGMENT_BASE - 2, 4) == b"E14"
        assert remote.memory_error(
            MMIX_POOL_SEGMENT_BASE - 2, 4, data=bytes(4)
        ) == b"E14"


def test_mmo_hosted_debug_exhaustion(qemu, workdir):
    with _gdb_session(
        qemu,
        workdir,
        "mmo-hosted-debug-exhaustion",
        mmo_hosted_debug_image(fill_budget=True),
        qemu_args=("-m", "128M"),
    ) as (remote, _process, _log):
        address = MMIX_STACK_SEGMENT_BASE + 0x4000

        assert remote.read_memory(address, 8) == bytes(8)
        assert remote.memory_error(
            address, 8, data=bytes.fromhex("0102030405060708")
        ) == b"E14"
        assert remote.read_memory(address, 8) == bytes(8)


def test_mmo_hosted_debug_text_invalidation(qemu, workdir):
    with _gdb_session(
        qemu,
        workdir,
        "mmo-hosted-debug-text-invalidation",
        mmo_hosted_debug_image(),
    ) as (remote, process, log_path):
        stop = remote.send_packet("s")
        assert stop.startswith((b"S", b"T"))
        remote.write_memory(0, halt())
        remote.continue_execution()
        assert process.wait(timeout=5) == 0

        result = read_log(log_path)
        assert result.pc == 0


def test_mmo_hosted_cold_reset(qemu, workdir):
    name = "mmo-hosted-cold-reset"
    qtest_path = workdir / f"{name}.qtest"
    data_address = MMIX_DATA_SEGMENT_BASE + 0x100
    runtime_address = MMIX_STACK_SEGMENT_BASE + 0x4000
    physical_address = 0x10000000
    mutation = bytes.fromhex("a1b2c3d4e5f60718")
    physical_marker = 0x0123456789abcdef

    if qtest_path.exists():
        qtest_path.unlink()
    qemu_args = (
        "-qmp",
        "stdio",
        "-qtest",
        f"unix:path={qtest_path},server=on,wait=off",
        "-qtest-log",
        "/dev/null",
    )
    qtest_connection = None
    try:
        with _gdb_session(
            qemu,
            workdir,
            name,
            mmo_hosted_debug_image(),
            qemu_args=qemu_args,
        ) as (remote, process, _log):
            greeting = _read_qmp_message(process)
            assert "QMP" in greeting
            _qmp_command(process, "qmp_capabilities")

            qtest_connection = _connect_unix(qtest_path)
            qtest = qtest_connection.makefile("rwb", buffering=0)
            _qtest_command(
                qtest, f"writeq {physical_address:#x} {physical_marker:#x}"
            )
            assert int(
                _qtest_command(qtest, f"readq {physical_address:#x}")[1],
                0,
            ) == physical_marker

            initial_data = remote.read_memory(data_address, 8)
            initial_arguments = remote.read_memory(
                MMIX_POOL_SEGMENT_BASE, 32
            )
            initial_r0 = remote.read_register(0)
            initial_r1 = remote.read_register(1)
            initial_rl = remote.read_register(MMIX_GDB_RL)

            for iteration in range(2):
                remote.write_memory(data_address, mutation)
                remote.write_memory(MMIX_POOL_SEGMENT_BASE, bytes(32))
                remote.write_memory(runtime_address, mutation)
                remote.write_register(0, 0x80 + iteration)
                remote.write_register(MMIX_GDB_RO, runtime_address)
                remote.write_register(MMIX_GDB_RS, runtime_address + 8)
                remote.write_register(MMIX_GDB_RL, 7)
                remote.write_register(MMIX_GDB_PC, data_address)

                _qmp_command(process, "system_reset")

                assert remote.read_memory(data_address, 8) == initial_data
                assert remote.read_memory(
                    MMIX_POOL_SEGMENT_BASE, 32
                ) == initial_arguments
                assert remote.read_memory(runtime_address, 8) == bytes(8)
                assert remote.read_register(0) == initial_r0
                assert remote.read_register(1) == initial_r1
                assert remote.read_register(MMIX_GDB_RO) == (
                    MMIX_STACK_SEGMENT_BASE
                )
                assert remote.read_register(MMIX_GDB_RS) == (
                    MMIX_STACK_SEGMENT_BASE
                )
                assert remote.read_register(MMIX_GDB_RL) == initial_rl
                assert remote.read_register(MMIX_GDB_PC) == 0
                assert int(
                    _qtest_command(qtest, f"readq {physical_address:#x}")[1],
                    0,
                ) == physical_marker
    finally:
        if qtest_connection is not None:
            qtest_connection.close()
        if qtest_path.exists():
            qtest_path.unlink()


def test_mmo_hosted_snapshot(qemu, workdir):
    name = "mmo-hosted-snapshot"
    snapshot_path = workdir / f"{name}.qcow2"
    qemu_img = qemu.with_name("qemu-img")
    data_address = MMIX_DATA_SEGMENT_BASE + 0x100
    stack_address = MMIX_STACK_SEGMENT_BASE + 0x6000
    hole_address = MMIX_POOL_SEGMENT_BASE + 0x8000
    saved_data = bytes.fromhex("1020304050607080")
    saved_stack = bytes.fromhex("8877665544332211")

    if snapshot_path.exists():
        snapshot_path.unlink()
    subprocess.run(
        (qemu_img, "create", "-q", "-f", "qcow2", snapshot_path, "1M"),
        check=True,
        timeout=10,
    )
    qemu_args = (
        "-qmp",
        "stdio",
        "-drive",
        f"file={snapshot_path},format=qcow2,if=none",
    )
    try:
        with _gdb_session(
            qemu,
            workdir,
            name,
            mmo_hosted_debug_image(),
            qemu_args=qemu_args,
        ) as (remote, process, log_path):
            greeting = _read_qmp_message(process)
            assert "QMP" in greeting
            _qmp_command(process, "qmp_capabilities")

            stop = remote.send_packet("s")
            assert stop.startswith((b"S", b"T"))
            remote.write_memory(0, halt())
            remote.write_memory(data_address, saved_data)
            remote.write_memory(stack_address, saved_stack)
            remote.write_register(0, 0x55)
            remote.write_register(MMIX_GDB_RO, stack_address)
            remote.write_register(MMIX_GDB_RS, stack_address + 8)
            remote.write_register(MMIX_GDB_RL, 7)
            remote.write_register(MMIX_GDB_PC, 0)
            assert remote.read_memory(hole_address, 8) == bytes(8)

            result = _qmp_command(
                process,
                "human-monitor-command",
                {"command-line": "savevm hosted-state"},
            )
            assert not result, result

            remote.write_memory(0, bytes.fromhex("f0ffffff"))
            remote.write_memory(data_address, bytes(8))
            remote.write_memory(stack_address, bytes(8))
            remote.write_memory(hole_address, bytes.fromhex("a5" * 8))
            remote.write_register(0, 0xaa)
            remote.write_register(MMIX_GDB_RO, MMIX_STACK_SEGMENT_BASE)
            remote.write_register(MMIX_GDB_RS, MMIX_STACK_SEGMENT_BASE)
            remote.write_register(MMIX_GDB_RL, 2)

            result = _qmp_command(
                process,
                "human-monitor-command",
                {"command-line": "loadvm hosted-state"},
            )
            assert not result, result
            assert remote.read_memory(0, 4) == halt()
            assert remote.read_memory(data_address, 8) == saved_data
            assert remote.read_memory(stack_address, 8) == saved_stack
            assert remote.read_memory(hole_address, 8) == bytes(8)
            assert remote.read_register(0) == 0x55
            assert remote.read_register(MMIX_GDB_RO) == stack_address
            assert remote.read_register(MMIX_GDB_RS) == stack_address + 8
            assert remote.read_register(MMIX_GDB_RL) == 7
            assert remote.read_register(MMIX_GDB_PC) == 0

            remote.continue_execution()
            assert process.wait(timeout=5) == 0
            assert read_log(log_path).pc == 0
    finally:
        if snapshot_path.exists():
            snapshot_path.unlink()


def test_mmo_hosted_snapshot_rejects_open_file(qemu, workdir):
    name = "mmo-hosted-snapshot-open-file"
    snapshot_path = workdir / f"{name}.qcow2"
    host_path = workdir / f"{name}.txt"
    qemu_img = qemu.with_name("qemu-img")

    if snapshot_path.exists():
        snapshot_path.unlink()
    subprocess.run(
        (qemu_img, "create", "-q", "-f", "qcow2", snapshot_path, "1M"),
        check=True,
        timeout=10,
    )
    qemu_args = (
        "-qmp",
        "stdio",
        "-semihosting",
        "-drive",
        f"file={snapshot_path},format=qcow2,if=none",
    )
    try:
        with _gdb_session(
            qemu,
            workdir,
            name,
            mmo_hosted_open_file_debug_image(host_path),
            qemu_args=qemu_args,
        ) as (remote, process, _log):
            greeting = _read_qmp_message(process)
            assert "QMP" in greeting
            _qmp_command(process, "qmp_capabilities")

            for _ in range(5):
                stop = remote.send_packet("s")
                assert stop.startswith((b"S", b"T"))
            assert remote.read_register(MMIX_GDB_PC) == 0x14

            result = _qmp_command(
                process,
                "human-monitor-command",
                {"command-line": "savevm hosted-open-file"},
            )
            assert "file handle 3 is open" in result
    finally:
        for path in (snapshot_path, host_path):
            if path.exists():
                path.unlink()


@pytest.mark.parametrize(
    "name,suffix,image,address,expected",
    (
        (
            "raw-debug-memory",
            ".bin",
            RAW_DIRECT_ISA_TESTS[0].program,
            RAW_ENTRY,
            RAW_DIRECT_ISA_TESTS[0].program[RAW_ENTRY:RAW_ENTRY + 4],
        ),
        (
            "elf-debug-memory",
            ".elf",
            BARE_ELF_TESTS[0].image,
            BARE_ENTRY,
            BARE_PROGRAM[:4],
        ),
    ),
)
def test_non_hosted_debug_memory(qemu, workdir, name, suffix, image, address,
                                 expected):
    with _gdb_session(
        qemu, workdir, name, image, suffix=suffix
    ) as (remote, _process, _log):
        assert remote.read_memory(address, len(expected)) == expected
