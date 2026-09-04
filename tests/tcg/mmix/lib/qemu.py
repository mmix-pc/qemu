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

QEMU_SEMIHOSTING_ARGS = ("-semihosting",)
QEMU_SEMIHOSTING_STDIN_CHARDEV = "mmix-semihosting-stdin"
QEMU_SEMIHOSTING_STDIN_ARGS = (
    "-chardev",
    f"stdio,id={QEMU_SEMIHOSTING_STDIN_CHARDEV},signal=off",
    "-semihosting-config",
    f"enable=on,chardev={QEMU_SEMIHOSTING_STDIN_CHARDEV}",
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
        "-device",
        f"loader,file={image},addr=0,cpu-num=0",
    ]
    if trace is not None:
        cmd.extend(["-d", trace])
    if log is not None:
        cmd.extend(["-D", str(log)])
    return cmd


def build_smp_elf_loader_command(qemu, image, entry, *, trace=None, log=None,
                                 qemu_args=()):
    if entry & 3 or entry >= 1 << 26:
        raise ValueError(
            f"MMIX SMP test entry is not JMP-reachable: {entry:#x}"
        )
    trampoline = ((0xf0 << 24) | (entry >> 2)) << 32
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
        *qemu_args,
        "-device",
        f"loader,file={image}",
        "-device",
        f"loader,data={trampoline:#x},data-len=4,addr=0,data-be=on",
    ]
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
):
    return _run_command(
        build_loader_command(qemu, image, serial=serial, trace=trace, log=log,
                             qemu_args=qemu_args),
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
