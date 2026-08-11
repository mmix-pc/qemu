#!/usr/bin/env python3
#
# QEMU execution helpers for MMIX softmmu tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses
import pathlib
import re
import subprocess
from typing import Dict

QEMU_SEMIHOSTING_ARGS = ("-semihosting",)


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
):
    kwargs = {}
    if capture_output:
        kwargs["stdout"] = subprocess.PIPE
        kwargs["stderr"] = subprocess.PIPE

    return subprocess.run(
        build_kernel_command(qemu, kernel, serial=serial, trace=trace, log=log,
                             qemu_args=qemu_args),
        check=check,
        timeout=timeout,
        **kwargs,
    )


def parse_log(log_text):
    if "MMIX test exit" not in log_text:
        raise AssertionError("missing MMIX test exit line")

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
