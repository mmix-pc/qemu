#!/usr/bin/env python3
#
# Minimal GDB remote client for MMIX softmmu memory tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import socket
import time


class GDBRemote:
    def __init__(self, connection):
        self.connection = connection

    @classmethod
    def connect(cls, path, timeout=5):
        deadline = time.monotonic() + timeout
        while True:
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                connection.connect(str(path))
                connection.settimeout(timeout)
                return cls(connection)
            except (FileNotFoundError, ConnectionRefusedError):
                connection.close()
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.01)

    def close(self):
        self.connection.close()

    def _recv_byte(self):
        byte = self.connection.recv(1)
        if not byte:
            raise ConnectionError("GDB remote connection closed")
        return byte

    def _recv_bytes(self, length):
        data = bytearray()
        while len(data) < length:
            data.extend(self._recv_byte())
        return bytes(data)

    def _recv_packet(self):
        while self._recv_byte() != b"$":
            pass

        payload = bytearray()
        while True:
            byte = self._recv_byte()
            if byte == b"#":
                break
            payload.extend(byte)
        received_checksum = int(self._recv_bytes(2), 16)
        expected_checksum = sum(payload) & 0xff
        if received_checksum != expected_checksum:
            self.connection.sendall(b"-")
            raise ValueError("invalid GDB remote packet checksum")
        self.connection.sendall(b"+")
        return bytes(payload)

    def send_packet(self, payload, *, response=True):
        payload = payload.encode("ascii")
        checksum = sum(payload) & 0xff
        packet = b"$" + payload + f"#{checksum:02x}".encode("ascii")
        self.connection.sendall(packet)
        if self._recv_byte() != b"+":
            raise ValueError("GDB remote packet was not acknowledged")
        return self._recv_packet() if response else None

    def read_memory(self, address, length):
        response = self.send_packet(f"m{address:x},{length:x}")
        if response.startswith(b"E"):
            raise RuntimeError(response.decode("ascii"))
        return bytes.fromhex(response.decode("ascii"))

    def write_memory(self, address, data):
        response = self.send_packet(
            f"M{address:x},{len(data):x}:{data.hex()}"
        )
        if response != b"OK":
            raise RuntimeError(response.decode("ascii"))

    def read_register(self, register):
        response = self.send_packet(f"p{register:x}")
        if response.startswith(b"E"):
            raise RuntimeError(response.decode("ascii"))
        return int.from_bytes(bytes.fromhex(response.decode("ascii")), "big")

    def write_register(self, register, value):
        data = value.to_bytes(8, "big")
        response = self.send_packet(f"P{register:x}={data.hex()}")
        if response != b"OK":
            raise RuntimeError(response.decode("ascii"))

    def memory_error(self, address, length, *, data=None):
        if data is None:
            response = self.send_packet(f"m{address:x},{length:x}")
        else:
            response = self.send_packet(
                f"M{address:x},{len(data):x}:{data.hex()}"
            )
        return response

    def continue_execution(self):
        self.send_packet("c", response=False)
