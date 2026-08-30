#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct
import subprocess

import pytest

from cases.common import elf64_image
from lib.execution import (
    run_firmware_entry_state_test,
    run_no_image_mttcg_test,
    run_paused_machine,
)
from lib.mmix_asm import halt


MMO_PREAMBLE = bytes((0x98, 0x09, 0x01, 0x01))
FLASH_SIZE = 64 * 1024 * 1024


def _pflash_drive(path, unit):
    readonly = ",readonly=on" if unit == 0 else ""
    return (
        "-drive",
        f"if=pflash,unit={unit},format=raw,file={path}{readonly}",
    )


def _firmware_files(workdir, name):
    files = {}

    for role in ("bios", "pflash0", "pflash1", "kernel", "initrd"):
        path = workdir / f"firmware-preflight-{name}-{role}.bin"
        contents = f"{name}-{role}".encode("ascii")
        if role == "bios":
            contents += bytes(-len(contents) % 4)
        path.write_bytes(contents)
        if role in ("pflash0", "pflash1"):
            with path.open("r+b") as stream:
                stream.truncate(FLASH_SIZE)
        files[role] = path
    return files


def _file_states(files):
    states = {}

    for role, path in files.items():
        stat = path.stat()
        with path.open("rb") as stream:
            first = stream.read(64)
            stream.seek(max(0, stat.st_size - 64))
            last = stream.read(64)
        states[role] = (stat.st_size, stat.st_mtime_ns, first, last)
    return states


def _firmware_args(files, *, bios=False, bios_none=False, pflash0=False,
                   pflash1=False, payloads=False):
    args = []

    if bios:
        args.extend(("-bios", str(files["bios"])))
    elif bios_none:
        args.extend(("-bios", "none"))
    if pflash0:
        args.extend(_pflash_drive(files["pflash0"], 0))
    if pflash1:
        args.extend(_pflash_drive(files["pflash1"], 1))
    if payloads:
        args.extend((
            "-kernel", str(files["kernel"]),
            "-initrd", str(files["initrd"]),
            "-append", "opaque firmware command line",
        ))
    return tuple(args)


def _machine_pflash_configuration(files, *, bios_none=False, pflash1=False):
    properties = ["virt", "pflash0=code"]
    args = [
        "-drive", f"if=none,id=code,format=raw,readonly=on,"
                  f"file={files['pflash0']}",
    ]

    if bios_none:
        args.extend(("-bios", "none"))
    if pflash1:
        properties.append("pflash1=vars")
        args.extend((
            "-drive", f"if=none,id=vars,format=raw,file={files['pflash1']}",
        ))
    return ",".join(properties), tuple(args)


def _run_preflight_failure(qemu, args):
    return subprocess.run(
        [qemu, "-machine", "virt", "-display", "none", "-monitor", "none",
         "-serial", "none", *args],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )


def test_no_image_mttcg_startup(qemu):
    run_no_image_mttcg_test(qemu)


@pytest.mark.parametrize("cpu_count", (1, 64))
def test_firmware_cpu_entry_state(qemu, workdir, cpu_count):
    bios = workdir / f"firmware-entry-{cpu_count}.bin"
    bios.write_bytes(halt())

    run_firmware_entry_state_test(qemu, bios, cpu_count)


def test_firmware_executes_from_negative_flash_alias(qemu, workdir):
    bios = workdir / "firmware-negative-alias.bin"
    bios.write_bytes(halt())

    result = subprocess.run(
        [qemu, "-machine", "virt", "-bios", bios, "-semihosting",
         "-display", "none", "-monitor", "none", "-serial", "none"],
        capture_output=True,
        check=False,
        timeout=10,
    )

    assert result.returncode == 0, result.stderr.decode(
        "utf-8", errors="replace"
    )


@pytest.mark.parametrize(
    "name,options",
    (
        ("bios", {"bios": True}),
        ("bios-payloads", {"bios": True, "payloads": True}),
        ("legacy-pflash0", {"pflash0": True}),
        ("legacy-pflash-banks", {"pflash0": True, "pflash1": True}),
    ),
)
def test_firmware_preflight_accepts_valid_inputs(qemu, workdir, name, options):
    files = _firmware_files(workdir, name)
    before = _file_states(files)

    run_paused_machine(qemu, qemu_args=_firmware_args(files, **options))

    assert _file_states(files) == before


@pytest.mark.parametrize(
    "name,options",
    (
        ("pflash0", {}),
        ("bios-none-pflash0", {"bios_none": True}),
        ("pflash0-pflash1", {"pflash1": True}),
    ),
)
def test_firmware_preflight_accepts_machine_pflash_properties(
    qemu, workdir, name, options
):
    files = _firmware_files(workdir, name)
    before = _file_states(files)
    machine, args = _machine_pflash_configuration(files, **options)

    run_paused_machine(qemu, machine=machine, qemu_args=args)

    assert _file_states(files) == before


def test_firmware_preflight_accepts_bios_with_machine_pflash1(qemu, workdir):
    files = _firmware_files(workdir, "bios-pflash1")
    before = _file_states(files)
    args = (
        "-drive", f"if=none,id=vars,format=raw,file={files['pflash1']}",
        "-bios", str(files["bios"]),
    )

    run_paused_machine(qemu, machine="virt,pflash1=vars", qemu_args=args)

    assert _file_states(files) == before


@pytest.mark.parametrize(
    "name,options,diagnostic",
    (
        (
            "bios-pflash0",
            {"bios": True, "pflash0": True},
            "executable firmware cannot be supplied by both -bios and "
            "pflash0",
        ),
        (
            "pflash1-only",
            {"pflash1": True},
            "pflash1 requires executable firmware from -bios or pflash0",
        ),
        (
            "bios-none-pflash1",
            {"bios_none": True, "pflash1": True},
            "pflash1 requires executable firmware from -bios or pflash0",
        ),
        (
            "bios-pflash0-pflash1",
            {"bios": True, "pflash0": True, "pflash1": True},
            "executable firmware cannot be supplied by both -bios and "
            "pflash0",
        ),
    ),
)
def test_firmware_preflight_rejects_conflicts(
    qemu, workdir, name, options, diagnostic
):
    files = _firmware_files(workdir, name)
    before = _file_states(files)

    result = _run_preflight_failure(qemu, _firmware_args(files, **options))

    assert result.returncode != 0
    assert diagnostic in result.stderr
    assert _file_states(files) == before


def test_firmware_preflight_rejects_mmo_payload(qemu, workdir):
    files = _firmware_files(workdir, "mmo-payload")
    files["kernel"].write_bytes(MMO_PREAMBLE)
    args = _firmware_args(files, bios=True, payloads=True)

    result = _run_preflight_failure(qemu, args)

    assert result.returncode != 0
    assert "firmware boot does not accept an MMO -kernel payload" in result.stderr


@pytest.mark.parametrize("role", ("kernel", "initrd"))
def test_firmware_preflight_rejects_unreadable_payload(
    qemu, workdir, role
):
    files = _firmware_files(workdir, f"missing-{role}")
    files[role].unlink()
    args = _firmware_args(files, bios=True, payloads=True)

    result = _run_preflight_failure(qemu, args)

    assert result.returncode != 0
    assert str(files[role]) in result.stderr


def test_firmware_preflight_rejects_command_line_overflow(qemu, workdir):
    files = _firmware_files(workdir, "long-command-line")
    args = (
        "-bios", str(files["bios"]),
        "-kernel", str(files["kernel"]),
        "-append", "x" * 4096,
    )

    result = _run_preflight_failure(qemu, args)

    assert result.returncode != 0
    assert "firmware command line is too long" in result.stderr


def test_firmware_preflight_rejects_fw_cfg_size_limit(qemu, workdir):
    files = _firmware_files(workdir, "oversized-initrd")
    with files["initrd"].open("r+b") as stream:
        stream.truncate(1 << 32)
    args = _firmware_args(files, bios=True, payloads=True)

    result = _run_preflight_failure(qemu, args)

    assert result.returncode != 0
    assert "fw_cfg files must be smaller" in result.stderr


def test_user_dtb_is_rejected(qemu, workdir):
    dtb = workdir / "unsupported.dtb"
    dtb.write_bytes(b"not a device tree")

    result = subprocess.run(
        [qemu, "-machine", "virt", "-display", "none", "-monitor", "none",
         "-serial", "none", "-dtb", str(dtb)],
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode != 0
    assert "MMIX virt does not accept a user-supplied DTB" in result.stderr


def test_canonical_dtb_dump_is_deterministic(qemu, workdir):
    blobs = []

    for index in range(2):
        dtb = workdir / f"canonical-{index}.dtb"
        subprocess.run(
            [qemu, "-machine", f"virt,dumpdtb={dtb}", "-display", "none",
             "-monitor", "none", "-serial", "none"],
            capture_output=True,
            text=True,
            check=True,
        )
        blobs.append(dtb.read_bytes())

    assert blobs[0] == blobs[1]
    assert struct.unpack(">I", blobs[0][:4])[0] == 0xD00DFEED
    assert struct.unpack(">I", blobs[0][4:8])[0] == len(blobs[0])
    assert len(blobs[0]) <= 2 * 1024 * 1024


def test_firmware_dtb_matches_other_boot_modes(qemu, workdir):
    bios = workdir / "firmware-fdt.bin"
    kernel = workdir / "direct-fdt.elf"
    bios.write_bytes(halt())
    kernel.write_bytes(elf64_image(0, halt()))
    blobs = []

    for name, machine, args in (
        ("erased", "virt", ()),
        ("direct", "virt,elf-startup-abi=linux",
         ("-kernel", str(kernel))),
        ("firmware", "virt", ("-bios", str(bios))),
    ):
        dtb = workdir / f"canonical-{name}.dtb"
        subprocess.run(
            [qemu, "-machine", f"{machine},dumpdtb={dtb}",
             "-display", "none",
             "-monitor", "none", "-serial", "none", *args],
            capture_output=True,
            text=True,
            check=True,
        )
        blobs.append(dtb.read_bytes())

    assert blobs[0] == blobs[1] == blobs[2]


def test_firmware_dtb_is_preplacement_description(qemu, workdir):
    bios = workdir / "firmware-preplacement-fdt.bin"
    kernel = workdir / "firmware-payload.bin"
    initrd = workdir / "firmware-initrd.bin"
    dtb = workdir / "firmware-preplacement.dtb"
    command_line = b"firmware keeps payload placement"

    bios.write_bytes(halt())
    kernel.write_bytes(b"opaque kernel payload")
    initrd.write_bytes(b"opaque initrd payload")
    subprocess.run(
        [qemu, "-machine", f"virt,dumpdtb={dtb}", "-display", "none",
         "-monitor", "none", "-serial", "none", "-bios", bios,
         "-kernel", kernel, "-initrd", initrd, "-append",
         command_line.decode("ascii")],
        capture_output=True,
        check=True,
    )

    blob = dtb.read_bytes()
    header = struct.unpack_from(">10I", blob)
    reservation_offset = header[4]
    first_reservation = struct.unpack_from(">QQ", blob, reservation_offset)

    assert command_line + b"\0" in blob
    assert b"linux,initrd-start\0" not in blob
    assert b"linux,initrd-end\0" not in blob
    assert first_reservation == (0, 0)
