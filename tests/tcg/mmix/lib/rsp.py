#!/usr/bin/env python3
#
# GDB Remote Serial Protocol helpers for MMIX softmmu tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib
import socket
import subprocess
import time

from lib.qemu import build_kernel_command


_RSP_ESCAPE = ord("}")
_RSP_RESERVED = frozenset(b"$#}*")


def _escape(payload):
    result = bytearray()

    for byte in payload:
        if byte in _RSP_RESERVED:
            result.extend((_RSP_ESCAPE, byte ^ 0x20))
        else:
            result.append(byte)
    return bytes(result)


def _unescape(payload):
    result = bytearray()
    escaped = False

    for byte in payload:
        if escaped:
            result.append(byte ^ 0x20)
            escaped = False
        elif byte == _RSP_ESCAPE:
            escaped = True
        else:
            result.append(byte)
    if escaped:
        raise AssertionError("truncated RSP escape sequence")
    return bytes(result)


class RSPClient:
    def __init__(self, connection, timeout):
        self._connection = connection
        self._connection.settimeout(timeout)

    @classmethod
    def connect(cls, path, *, process=None, timeout=5):
        path = pathlib.Path(path)
        deadline = time.monotonic() + timeout
        last_error = None

        while time.monotonic() < deadline:
            if process is not None and process.poll() is not None:
                raise AssertionError(
                    f"QEMU exited before opening the RSP socket: "
                    f"status {process.returncode}"
                )
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                connection.connect(str(path))
                return cls(connection, timeout)
            except OSError as error:
                last_error = error
                connection.close()
                time.sleep(0.01)

        raise AssertionError(
            f"timed out connecting to RSP socket {path}: {last_error}"
        )

    @staticmethod
    def _frame(payload):
        escaped = _escape(payload)
        checksum = sum(escaped) & 0xff
        return b"$" + escaped + f"#{checksum:02x}".encode("ascii")

    def _read_byte(self):
        byte = self._connection.recv(1)
        if not byte:
            raise AssertionError("QEMU closed the RSP connection")
        return byte[0]

    def _read_ack(self):
        byte = self._read_byte()
        if byte not in (ord("+"), ord("-")):
            raise AssertionError(f"invalid RSP acknowledgement: {byte:#x}")
        return byte == ord("+")

    def _read_packet(self):
        while self._read_byte() != ord("$"):
            pass

        encoded = bytearray()
        escaped = False
        while True:
            byte = self._read_byte()
            if byte == ord("#") and not escaped:
                break
            encoded.append(byte)
            if escaped:
                escaped = False
            elif byte == _RSP_ESCAPE:
                escaped = True

        try:
            received_checksum = int(bytes((self._read_byte(),
                                           self._read_byte())), 16)
        except ValueError as error:
            self._connection.sendall(b"-")
            raise AssertionError("invalid RSP checksum encoding") from error

        expected_checksum = sum(encoded) & 0xff
        if received_checksum != expected_checksum:
            self._connection.sendall(b"-")
            raise AssertionError(
                f"RSP checksum mismatch: expected {expected_checksum:02x}, "
                f"received {received_checksum:02x}"
            )

        self._connection.sendall(b"+")
        return _unescape(encoded)

    def request(self, payload):
        if isinstance(payload, str):
            payload = payload.encode("ascii")
        frame = self._frame(payload)

        for _ in range(3):
            self._connection.sendall(frame)
            if self._read_ack():
                return self._read_packet()
        raise AssertionError("QEMU rejected the RSP packet three times")

    def read_xfer(self, object_name, annex, *, chunk_size=0x400,
                  max_size=1 << 20):
        result = bytearray()

        while True:
            response = self.request(
                f"qXfer:{object_name}:read:{annex}:"
                f"{len(result):x},{chunk_size:x}"
            )
            if not response or response[:1] not in (b"l", b"m"):
                raise AssertionError(
                    f"invalid qXfer response for {object_name}:{annex}: "
                    f"{response!r}"
                )
            result.extend(response[1:])
            if len(result) > max_size:
                raise AssertionError(
                    f"qXfer response for {object_name}:{annex} exceeds "
                    f"{max_size} bytes"
                )
            if response[:1] == b"l":
                return bytes(result)
            if len(response) == 1:
                raise AssertionError(
                    f"empty qXfer continuation for {object_name}:{annex}"
                )

    def interrupt(self):
        self._connection.sendall(b"\x03")
        return self._read_packet()

    def close(self):
        if self._connection is not None:
            self._connection.close()
            self._connection = None


class QEMURSPServer:
    def __init__(self, qemu, kernel, workdir, *, timeout=5):
        self.qemu = qemu
        self.kernel = kernel
        self.workdir = pathlib.Path(workdir)
        self.timeout = timeout
        self.socket_path = self.workdir / "gdb-rsp.sock"
        self.log_path = self.workdir / "gdb-rsp.log"
        self.client = None
        self.process = None
        self._log = None

    def __enter__(self):
        self.workdir.mkdir(parents=True, exist_ok=True)
        for path in (self.socket_path, self.log_path):
            path.unlink(missing_ok=True)

        command = build_kernel_command(
            self.qemu,
            self.kernel,
            qemu_args=(
                "-S",
                "-chardev",
                f"socket,id=mmix-gdb,path={self.socket_path},"
                "server=on,wait=off",
                "-gdb",
                "chardev:mmix-gdb",
            ),
        )
        self._log = self.log_path.open("wb")
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=self._log,
            stderr=subprocess.STDOUT,
        )
        try:
            self.client = RSPClient.connect(
                self.socket_path,
                process=self.process,
                timeout=self.timeout,
            )
        except BaseException as error:
            self.close()
            output = self.log_path.read_text(encoding="utf-8", errors="replace")
            if output:
                raise AssertionError(
                    f"failed to start the QEMU RSP server:\n{output}"
                ) from error
            raise
        return self

    def close(self):
        if self.client is not None:
            self.client.close()
            self.client = None
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=self.timeout)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=self.timeout)
            self.process = None
        if self._log is not None:
            self._log.close()
            self._log = None
        self.socket_path.unlink(missing_ok=True)

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()
