#!/usr/bin/env python3
#
# QEMU execution helpers for MMIX softmmu tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses
import pathlib
import re
import subprocess
from typing import Dict, Optional

from .mmix_asm import GOI, R255, insn, set_octa

QEMU_SEMIHOSTING_ARGS = (
    "-semihosting-config",
    "enable=on,userspace=on",
)
QEMU_SEMIHOSTING_STDIN_CHARDEV = "mmix-semihosting-stdin"
QEMU_SEMIHOSTING_STDIN_ARGS = (
    "-chardev",
    f"stdio,id={QEMU_SEMIHOSTING_STDIN_CHARDEV},signal=off",
    "-semihosting-config",
    f"enable=on,userspace=on,chardev={QEMU_SEMIHOSTING_STDIN_CHARDEV}",
)

# White-box fixtures may bypass architectural entry or return state.
QEMU_DISABLE_SECURITY_CHECKS_ARGS = (
    "-global",
    "mmix-cpu.x-security-checks=off",
)


@dataclasses.dataclass(frozen=True)
class QemuLog:
    pc: int
    npc: int
    regs: Dict[int, int]


def build_kernel_command(qemu, kernel, *, serial="none", trace=None, log=None,
                         qemu_args=()):
    cmd = [
        str(qemu),
        "-machine",
        "virt",
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        str(serial),
        *qemu_args,
        "-kernel",
        str(kernel),
    ]
    if trace is not None:
        cmd.extend(["-d", trace])
    if log is not None:
        cmd.extend(["-D", str(log)])
    return cmd


def build_loader_command(qemu, image, *, serial="none", trace=None, log=None,
                         qemu_args=(), disable_security_checks=True):
    security_args = (QEMU_DISABLE_SECURITY_CHECKS_ARGS
                     if disable_security_checks else ())
    cmd = [
        str(qemu),
        "-machine",
        "virt",
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        str(serial),
        *security_args,
        *qemu_args,
        "-device",
        f"loader,file={image},addr=0,cpu-num=0",
    ]
    if trace is not None:
        cmd.extend(["-d", trace])
    if log is not None:
        cmd.extend(["-D", str(log)])
    return cmd


def build_smp_elf_loader_command(qemu, image, entry, *, trace=None, log=None,
                                 qemu_args=(),
                                 disable_security_checks=True):
    if entry & 3:
        raise ValueError(f"MMIX SMP test entry is not aligned: {entry:#x}")
    if entry < 1 << 26:
        trampoline = (((0xf0 << 24) | (entry >> 2)) << 32).to_bytes(8, "big")
        trampoline = trampoline[:4]
    else:
        trampoline = b"".join((
            *set_octa(R255, entry),
            insn(GOI, R255, R255, 0),
        ))
    security_args = (QEMU_DISABLE_SECURITY_CHECKS_ARGS
                     if disable_security_checks else ())
    cmd = [
        str(qemu),
        "-machine",
        "virt",
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "none",
        *security_args,
        *qemu_args,
        "-device",
        f"loader,file={image}",
    ]
    for offset in range(0, len(trampoline), 8):
        chunk = trampoline[offset:offset + 8]
        data = int.from_bytes(chunk.ljust(8, b"\0"), "big")
        cmd.extend((
            "-device",
            f"loader,data={data:#x},data-len={len(chunk)},"
            f"addr={offset:#x},data-be=on",
        ))
    if trace is not None:
        cmd.extend(["-d", trace])
    if log is not None:
        cmd.extend(["-D", str(log)])
    return cmd


def _run_command(cmd, *, check, timeout, capture_output, stdin_data):
    kwargs = {}
    if capture_output:
        kwargs["stdout"] = subprocess.PIPE
        kwargs["stderr"] = subprocess.PIPE
    if stdin_data is not None:
        kwargs["input"] = stdin_data

    return subprocess.run(cmd, check=check, timeout=timeout, **kwargs)


def run_kernel(
    qemu,
    kernel,
    *,
    serial="none",
    trace=None,
    log=None,
    qemu_args=(),
    check=True,
    timeout=10,
    capture_output=False,
    stdin_data: Optional[bytes] = None,
):
    return _run_command(
        build_kernel_command(qemu, kernel, serial=serial, trace=trace, log=log,
                             qemu_args=qemu_args),
        check=check, timeout=timeout, capture_output=capture_output,
        stdin_data=stdin_data,
    )


def run_loader(
    qemu,
    image,
    *,
    serial="none",
    trace=None,
    log=None,
    qemu_args=(),
    check=True,
    timeout=10,
    capture_output=False,
    stdin_data: Optional[bytes] = None,
    disable_security_checks=True,
):
    return _run_command(
        build_loader_command(qemu, image, serial=serial, trace=trace, log=log,
                             qemu_args=qemu_args,
                             disable_security_checks=disable_security_checks),
        check=check, timeout=timeout, capture_output=capture_output,
        stdin_data=stdin_data,
    )


def parse_log(log_text):
    if "MMIX test exit" not in log_text:
        raise AssertionError("missing MMIX test exit line")
    if "register-stack-access=none" not in log_text:
        raise AssertionError("incomplete MMIX register-stack access")

    pc_match = re.search(r"pc=0x([0-9a-fA-F]+)\s+npc=0x([0-9a-fA-F]+)", log_text)
    if pc_match is None:
        raise AssertionError("missing pc/npc line")

    regs = {}
    for reg, value in re.findall(r"\br(\d+)\s*=0x([0-9a-fA-F]+)", log_text):
        regs[int(reg)] = int(value, 16)

    return QemuLog(
        pc=int(pc_match.group(1), 16),
        npc=int(pc_match.group(2), 16),
        regs=regs,
    )


def read_log(log):
    return parse_log(pathlib.Path(log).read_text(encoding="utf-8"))
