#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct
import subprocess

from lib.execution import run_no_image_mttcg_test


def test_no_image_mttcg_startup(qemu):
    run_no_image_mttcg_test(qemu)


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
