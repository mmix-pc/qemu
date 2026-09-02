#!/usr/bin/env python3
#
# MMIX GDB Remote Serial Protocol tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

from cases.debugger import DEBUGGER_ELF_IMAGE
from lib.rsp import QEMURSPServer


def test_rsp_initial_stop(qemu, workdir):
    image = workdir / "debugger-fixture.elf"

    image.write_bytes(DEBUGGER_ELF_IMAGE)
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
