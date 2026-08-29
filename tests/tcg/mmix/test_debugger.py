#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import contextlib
import subprocess

import pytest

from cases.common import (
    MMIX_DATA_SEGMENT_BASE,
    MMIX_HOSTED_LIMIT,
    MMIX_POOL_SEGMENT_BASE,
    MMIX_STACK_SEGMENT_BASE,
)
from cases.elf_bare import BARE_ELF_TESTS, BARE_ENTRY, BARE_PROGRAM
from cases.mmo_hosted import mmo_hosted_debug_image
from cases.raw_loader import RAW_DIRECT_ISA_TESTS, RAW_ENTRY
from lib.gdb_remote import GDBRemote
from lib.mmix_asm import halt
from lib.qemu import build_kernel_command, read_log


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
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
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
