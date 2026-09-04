#!/usr/bin/env python3
#
# MMIX softmmu test execution helpers
#
# SPDX-License-Identifier: GPL-2.0-or-later

import dataclasses
import json
import re
import select
import socket
import struct
import subprocess
import time

from lib.asserts import (
    assert_console_output,
    assert_exit_pc,
    assert_exit_status,
    assert_log_patterns,
    assert_output_patterns,
    assert_process_failed,
    assert_regs,
    assert_serial_output,
)
from lib.mmo import MMIX_MMO_ESCAPE, MMIX_MMO_LOP_PRE, mmo_hosted_text_image
from lib.qemu import (
    QEMU_SEMIHOSTING_ARGS,
    QEMU_SEMIHOSTING_STDIN_ARGS,
    build_kernel_command,
    build_smp_elf_loader_command,
    read_log,
    run_kernel,
    run_loader,
)


QEMU_SEMIHOSTING_CONSOLE_CHARDEV = "mmix-semihosting-console"


def _as_hosted_mmo(test):
    if test.program.startswith(bytes((MMIX_MMO_ESCAPE, MMIX_MMO_LOP_PRE))):
        return test
    return dataclasses.replace(
        test, program=mmo_hosted_text_image(test.program)
    )


def _test_stdin_data(test, stdin_data):
    return test.stdin_data if stdin_data is None else stdin_data


def _semihosting_config_args(test, *, chardev=None, chardev_id=None):
    args = list(test.qemu_args)

    if chardev is not None:
        assert chardev_id is not None
        args.extend(("-chardev", chardev))

        for index, arg in enumerate(args[:-1]):
            if arg == "-semihosting-config":
                config = args[index + 1]
                if "chardev=" not in config:
                    args[index + 1] = f"{config},chardev={chardev_id}"
                return tuple(args)

        args.extend(("-semihosting-config", f"enable=on,chardev={chardev_id}"))
        return tuple(args)

    if not any(arg == "-semihosting" or arg == "-semihosting-config"
               for arg in args):
        args.extend(QEMU_SEMIHOSTING_ARGS)

    return tuple(args)


def _run_one(qemu, workdir, test, runner, *, qemu_args=(), stdin_data=None):
    image = workdir / f"{test.name}.bin"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.program)
    if log.exists():
        log.unlink()

    completed = runner(qemu, image, trace="int", log=log,
                       qemu_args=qemu_args, check=False, timeout=10,
                       stdin_data=_test_stdin_data(test, stdin_data))

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)
    assert_regs(test.name, result, test.regs)


def run_one(qemu, workdir, test, *, qemu_args=(), stdin_data=None):
    _run_one(qemu, workdir, test, run_kernel, qemu_args=qemu_args,
             stdin_data=stdin_data)


def run_one_with_loader(qemu, workdir, test, *, qemu_args=(), stdin_data=None):
    _run_one(qemu, workdir, test, run_loader, qemu_args=qemu_args,
             stdin_data=stdin_data)


def run_semihosting_one(qemu, workdir, test):
    test = _as_hosted_mmo(test)
    qemu_args = _semihosting_config_args(test)
    run_one(qemu, workdir, test, qemu_args=qemu_args)


def run_semihosting_stdin_one(qemu, workdir, test):
    test = _as_hosted_mmo(test)
    qemu_args = (
        test.qemu_args if test.qemu_args else QEMU_SEMIHOSTING_STDIN_ARGS
    )

    run_one(qemu, workdir, test, qemu_args=qemu_args)


def run_expected_failure(qemu, workdir, test, *, qemu_args=()):
    image = workdir / f"{test.name}.bin"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.program)
    if log.exists():
        log.unlink()

    command = build_kernel_command(qemu, image, trace="unimp,int", log=log,
                                   qemu_args=qemu_args)
    process = subprocess.Popen(command)
    deadline = time.monotonic() + 2

    try:
        while process.poll() is None and time.monotonic() < deadline:
            if log.exists():
                log_text = log.read_text(encoding="utf-8")
                if all(pattern in log_text for pattern in test.patterns):
                    break
            time.sleep(0.001)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    if not log.exists():
        raise AssertionError(f"{test.name}: missing log")

    log_text = log.read_text(encoding="utf-8")
    assert_log_patterns(test.name, log_text, test.patterns, test.absent)


def run_semihosting_expected_failure(qemu, workdir, test):
    run_expected_failure(qemu, workdir, _as_hosted_mmo(test),
                         qemu_args=QEMU_SEMIHOSTING_ARGS)


def run_semihosting_disabled_expected_failure(qemu, workdir, test):
    run_expected_failure(qemu, workdir, _as_hosted_mmo(test))


def run_process_failure(qemu, workdir, test):
    image = workdir / f"{test.name}.bin"
    empty = workdir / f"{test.name}.empty"
    missing = workdir / f"{test.name}.missing"
    needs_empty = "$EMPTY" in test.qemu_args
    needs_missing = "$MISSING" in test.qemu_args

    image.write_bytes(test.program)
    if needs_empty:
        empty.write_bytes(b"")
    if needs_missing and missing.exists():
        missing.unlink()
    qemu_args = tuple(
        str(image) if arg == "$IMAGE" else
        str(empty) if arg == "$EMPTY" else
        str(missing) if arg == "$MISSING" else arg
        for arg in test.qemu_args
    )

    result = run_kernel(
        qemu,
        image,
        qemu_args=qemu_args,
        check=False,
        timeout=10,
        capture_output=True,
    )
    assert_process_failed(test.name, result)

    output = result.stdout + result.stderr
    text = output.decode("utf-8", errors="replace")
    assert_output_patterns(test.name, text, test.patterns)


def run_serial_test(qemu, workdir, test, *, qemu_args=(), stdin_data=None):
    is_mmo = test.program.startswith(bytes((MMIX_MMO_ESCAPE,
                                            MMIX_MMO_LOP_PRE)))
    suffix = ".mmo" if is_mmo else ".bin"
    image = workdir / f"{test.name}{suffix}"
    log = workdir / f"{test.name}.log"
    serial = workdir / f"{test.name}.serial"

    image.write_bytes(test.program)
    for path in (log, serial):
        if path.exists():
            path.unlink()

    completed = run_kernel(
        qemu,
        image,
        serial=f"file:{serial}",
        trace="int",
        log=log,
        qemu_args=qemu_args,
        check=False,
        timeout=10,
        stdin_data=_test_stdin_data(test, stdin_data),
    )

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)

    actual = serial.read_bytes()
    assert_serial_output(test.name, actual, test.output)


def run_semihosting_console_test(qemu, workdir, test):
    test = _as_hosted_mmo(test)
    is_mmo = test.program.startswith(bytes((MMIX_MMO_ESCAPE,
                                            MMIX_MMO_LOP_PRE)))
    suffix = ".mmo" if is_mmo else ".bin"
    image = workdir / f"{test.name}{suffix}"
    log = workdir / f"{test.name}.log"
    console = workdir / f"{test.name}.console"
    chardev = f"file,id={QEMU_SEMIHOSTING_CONSOLE_CHARDEV},path={console}"
    qemu_args = _semihosting_config_args(
        test,
        chardev=chardev,
        chardev_id=QEMU_SEMIHOSTING_CONSOLE_CHARDEV,
    )

    image.write_bytes(test.program)
    for path in (log, console):
        if path.exists():
            path.unlink()

    completed = run_kernel(
        qemu,
        image,
        trace="int",
        log=log,
        qemu_args=qemu_args,
        check=False,
        timeout=10,
        stdin_data=_test_stdin_data(test, None),
    )

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)

    actual = console.read_bytes()
    assert_console_output(test.name, actual, test.output)


def run_semihosting_stdin_console_test(qemu, workdir, test):
    test = _as_hosted_mmo(test)
    is_mmo = test.program.startswith(bytes((MMIX_MMO_ESCAPE,
                                            MMIX_MMO_LOP_PRE)))
    suffix = ".mmo" if is_mmo else ".bin"
    image = workdir / f"{test.name}{suffix}"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.program)
    if log.exists():
        log.unlink()

    completed = run_kernel(
        qemu,
        image,
        trace="int",
        log=log,
        qemu_args=QEMU_SEMIHOSTING_STDIN_ARGS,
        check=False,
        timeout=10,
        capture_output=True,
        stdin_data=_test_stdin_data(test, None),
    )

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)
    assert_console_output(test.name, completed.stdout, test.output)


def run_loader_failure(qemu, workdir, test):
    image = workdir / f"{test.name}.mmo"

    image.write_bytes(test.image)
    if test.sparse_size is not None:
        with image.open("r+b") as file:
            file.truncate(test.sparse_size)

    result = run_kernel(
        qemu,
        image,
        qemu_args=test.qemu_args,
        check=False,
        timeout=10,
        capture_output=True,
    )
    assert_process_failed(test.name, result)

    output = result.stdout + result.stderr
    text = output.decode("utf-8", errors="replace")
    assert_output_patterns(test.name, text, test.patterns)


def run_mmo_test(qemu, workdir, test):
    image = workdir / f"{test.name}.mmo"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.image)
    if log.exists():
        log.unlink()

    completed = run_kernel(qemu, image, trace="int", log=log,
                           qemu_args=test.qemu_args, check=False, timeout=10)

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)
    assert_regs(test.name, result, test.regs)


def run_elf_test(qemu, workdir, test):
    image = workdir / f"{test.name}.elf"
    log = workdir / f"{test.name}.log"
    serial = workdir / f"{test.name}.serial"

    image.write_bytes(test.image)
    for path in (log, serial):
        if path.exists():
            path.unlink()

    serial_arg = f"file:{serial}" if test.output is not None else "none"
    completed = run_kernel(qemu, image, serial=serial_arg, trace="int",
                           log=log, qemu_args=test.qemu_args, check=False,
                           timeout=10)

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)
    assert_regs(test.name, result, test.regs)
    if test.output is not None:
        assert_serial_output(test.name, serial.read_bytes(), test.output)


def _read_qmp_message(process, timeout=5):
    ready, _, _ = select.select((process.stdout,), (), (), timeout)
    if not ready:
        raise AssertionError("timed out waiting for a QMP response")

    line = process.stdout.readline()
    if not line:
        raise AssertionError("QEMU closed QMP before sending a response")
    return json.loads(line)


def _qmp_command(process, command, arguments=None, timeout=5):
    request_object = {"execute": command}

    if arguments is not None:
        request_object["arguments"] = arguments
    request = json.dumps(request_object).encode("utf-8") + b"\n"

    process.stdin.write(request)
    process.stdin.flush()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        response = _read_qmp_message(process, deadline - time.monotonic())
        if "error" in response:
            raise AssertionError(f"QMP {command} failed: {response['error']}")
        if "return" in response:
            return response["return"]

    raise AssertionError(f"timed out waiting for QMP {command}")


def run_paused_machine(qemu, *, machine="virt", qemu_args=()):
    command = [
        str(qemu),
        "-machine", machine,
        "-display", "none",
        "-monitor", "none",
        "-serial", "none",
        *qemu_args,
        "-S",
        "-qmp", "stdio",
    ]
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    try:
        greeting = _read_qmp_message(process)
        if "QMP" not in greeting:
            raise AssertionError(f"invalid QMP greeting: {greeting}")
        _qmp_command(process, "qmp_capabilities")
        _qmp_command(process, "quit")
        _, stderr = process.communicate(timeout=5)
        if process.returncode != 0:
            raise AssertionError(
                "paused QEMU failed: "
                f"{stderr.decode('utf-8', errors='replace')}"
            )
    except BaseException:
        process.kill()
        _, stderr = process.communicate()
        if stderr:
            print(stderr.decode("utf-8", errors="replace"))
        raise


def run_mttcg_elf_test(qemu, workdir, test):
    image = workdir / f"{test.name}.elf"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.image)
    if log.exists():
        log.unlink()

    command = build_kernel_command(
        qemu,
        image,
        trace="int",
        log=log,
        qemu_args=(
            *test.qemu_args,
            "-S",
            "-qmp",
            "stdio",
        ),
    )
    process = subprocess.Popen(command, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, bufsize=0)
    try:
        greeting = _read_qmp_message(process)
        if "QMP" not in greeting:
            raise AssertionError(f"invalid QMP greeting: {greeting}")
        _qmp_command(process, "qmp_capabilities")
        cpus = _qmp_command(process, "query-cpus-fast")
        thread_ids = [cpu["thread-id"] for cpu in cpus]
        if len(thread_ids) != test.cpu_count:
            raise AssertionError(
                f"{test.name}: expected {test.cpu_count} QMP CPUs, "
                f"got {len(thread_ids)}"
            )
        if len(set(thread_ids)) != test.cpu_count:
            raise AssertionError(
                f"{test.name}: vCPUs share host thread ids {thread_ids}"
            )
        _qmp_command(process, "cont")
        stdout, stderr = process.communicate(timeout=10)
    except BaseException:
        process.kill()
        process.wait()
        raise

    completed = subprocess.CompletedProcess(command, process.returncode,
                                            stdout, stderr)
    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)
    assert_regs(test.name, result, test.regs)


def run_no_image_mttcg_test(qemu):
    command = [
        str(qemu),
        "-machine", "virt",
        "-smp", "2",
        "-accel", "tcg,thread=multi",
        "-display", "none",
        "-serial", "none",
        "-S",
        "-qmp", "stdio",
    ]
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    try:
        greeting = _read_qmp_message(process)
        if "QMP" not in greeting:
            raise AssertionError(f"invalid QMP greeting: {greeting}")
        _qmp_command(process, "qmp_capabilities")
        cpus = _qmp_command(process, "query-cpus-fast")
        thread_ids = [cpu["thread-id"] for cpu in cpus]
        if len(cpus) != 2 or len(set(thread_ids)) != 2:
            raise AssertionError(
                f"expected two independent MTTCG vCPUs, got {thread_ids}"
            )

        stacks = [
            _qmp_command(
                process,
                "qom-get",
                {
                    "path": f"/machine/cpu[{cpu}]",
                    "property": "initial-stack",
                },
            )
            for cpu in range(2)
        ]
        if stacks[0] == stacks[1] or any(stack % 0x2000 for stack in stacks):
            raise AssertionError(f"invalid per-CPU initial stacks {stacks}")
        for cpu in range(2):
            dump = _hmp_register_dump(process, cpu)
            if _hmp_register_value(dump, "pc=0x") != 0:
                raise AssertionError(f"CPU {cpu} did not reset at PC zero")
            if _hmp_register_value(dump, "rO =0x") != stacks[cpu]:
                raise AssertionError(f"CPU {cpu} rO does not match its stack")
            if _hmp_register_value(dump, "rS =0x") != stacks[cpu]:
                raise AssertionError(f"CPU {cpu} rS does not match its stack")

        _qmp_command(process, "quit")
        process.communicate(timeout=5)
    except BaseException:
        process.kill()
        _, stderr = process.communicate()
        if stderr:
            print(stderr.decode("utf-8", errors="replace"))
        raise


def _hmp_special_registers(dump):
    return {
        name: int(value, 16)
        for name, value in re.findall(
            r"\b(r[A-Z]{1,2})\s*=0x([0-9a-fA-F]+)", dump
        )
    }


def _hmp_general_registers(dump):
    return {
        int(reg): int(value, 16)
        for reg, value in re.findall(
            r"\br(\d+)\s*=0x([0-9a-fA-F]+)", dump
        )
    }


def run_firmware_entry_state_test(qemu, bios, cpu_count):
    command = [
        str(qemu),
        "-machine", "virt",
        "-smp", str(cpu_count),
        "-accel", "tcg,thread=multi",
        "-bios", str(bios),
        "-display", "none",
        "-serial", "none",
        "-S",
        "-qmp", "stdio",
    ]
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    try:
        greeting = _read_qmp_message(process)
        if "QMP" not in greeting:
            raise AssertionError(f"invalid QMP greeting: {greeting}")
        _qmp_command(process, "qmp_capabilities")
        cpus = _qmp_command(process, "query-cpus-fast")
        if len(cpus) != cpu_count:
            raise AssertionError(
                f"expected {cpu_count} firmware CPUs, got {len(cpus)}"
            )

        stacks = [
            _qmp_command(
                process,
                "qom-get",
                {
                    "path": f"/machine/cpu[{cpu}]",
                    "property": "initial-stack",
                },
            )
            for cpu in range(cpu_count)
        ]
        if len(set(stacks)) != cpu_count or any(
            stack % 0x2000 for stack in stacks
        ):
            raise AssertionError(
                f"invalid per-CPU firmware stacks {stacks}"
            )

        special_defaults = {
            "rG": 32,
            "rL": 1,
            "rT": 0x8000000500000000,
            "rTT": 0x8000000600000000,
            "rV": 0x369C200400000000,
        }
        for cpu, stack in enumerate(stacks):
            dump = _hmp_register_dump(process, cpu)
            if _hmp_register_value(dump, "pc=0x") != 0x8001000000000000:
                raise AssertionError(f"CPU {cpu} firmware PC mismatch")
            if _hmp_register_value(dump, "npc=0x") != 0x8001000000000004:
                raise AssertionError(f"CPU {cpu} firmware npc mismatch")

            regs = _hmp_general_registers(dump)
            expected_regs = {reg: 0 for reg in range(256)}
            expected_regs[0] = cpu
            if regs != expected_regs:
                raise AssertionError(
                    f"CPU {cpu} firmware general-register mismatch"
                )

            sregs = _hmp_special_registers(dump)
            expected_sregs = {
                name: 0
                for name in (
                    "rB rD rE rH rJ rM rR rBB rC rN rO rS rI rT rTT "
                    "rK rQ rU rV rG rL rA rF rP rW rX rY rZ rWW rXX "
                    "rYY rZZ"
                ).split()
            }
            expected_sregs.update(special_defaults)
            expected_sregs["rO"] = stack
            expected_sregs["rS"] = stack
            if sregs != expected_sregs:
                raise AssertionError(
                    f"CPU {cpu} firmware special-register mismatch"
                )

        _qmp_command(process, "quit")
        process.communicate(timeout=5)
    except BaseException:
        process.kill()
        _, stderr = process.communicate()
        if stderr:
            print(stderr.decode("utf-8", errors="replace"))
        raise


def run_firmware_reset_and_snapshot_test(qemu, workdir, firmware):
    reset_pc = 0x8001000000000000
    flash0 = 0x0001000000000000
    flash1 = 0x0001000004000000
    fw_cfg = 0x0001000014000000
    timer_context = 0x0001000020010000
    ipi = 0x0001000024000000
    ipi_context1 = ipi + 0x20000
    ram_marker = 0x10000
    fdt_copy = 0x20000
    stack_values = (
        bytes.fromhex("1122334455667788"),
        bytes.fromhex("99aabbccddeeff00"),
    )
    ram_value = bytes.fromhex("0123456789abcdef")
    kernel_data = b"firmware snapshot kernel\n"
    initrd_data = b"firmware snapshot initrd\n"
    command_line = "console=ttyS0 firmware-state"
    bios = workdir / "firmware-reset-snapshot.bin"
    kernel = workdir / "firmware-reset-snapshot-kernel.bin"
    initrd = workdir / "firmware-reset-snapshot-initrd.bin"
    snapshot = workdir / "firmware-reset-snapshot.qcow2"
    qtest_path = workdir / "firmware-reset-snapshot.sock"
    qemu_img = qemu.with_name("qemu-img")

    bios.write_bytes(firmware)
    kernel.write_bytes(kernel_data)
    initrd.write_bytes(initrd_data)
    for path in (snapshot, qtest_path):
        if path.exists():
            path.unlink()
    subprocess.run(
        (qemu_img, "create", "-q", "-f", "qcow2", snapshot, "1M"),
        check=True,
        timeout=10,
    )

    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(qtest_path))
    listener.listen(1)
    listener.settimeout(5)
    command = [
        str(qemu),
        "-machine", "virt",
        "-smp", "2",
        "-accel", "tcg,thread=multi",
        "-bios", str(bios),
        "-kernel", str(kernel),
        "-initrd", str(initrd),
        "-append", command_line,
        "-display", "none",
        "-monitor", "none",
        "-serial", "none",
        "-S",
        "-qmp", "stdio",
        "-qtest", f"unix:{qtest_path}",
        "-qtest-log", "/dev/null",
        "-drive", f"file={snapshot},format=qcow2,if=none",
    ]
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    connection = None
    try:
        connection, _ = listener.accept()
        connection.settimeout(5)
        qtest = connection.makefile("rwb", buffering=0)
        greeting = _read_qmp_message(process)
        if "QMP" not in greeting:
            raise AssertionError(f"invalid QMP greeting: {greeting}")
        _qmp_command(process, "qmp_capabilities")

        def readb(address):
            response = _qtest_command(qtest, f"readb {address:#x}")
            return int(response[1], 0)

        def writeb(address, value):
            _qtest_command(qtest, f"writeb {address:#x} {value:#x}")

        def select_fw_cfg(selector):
            _qtest_command(qtest, f"writew {fw_cfg + 8:#x} {selector:#x}")

        def read_fw_cfg(size):
            return bytes(readb(fw_cfg) for _ in range(size))

        def fw_cfg_files():
            select_fw_cfg(0x19)
            count = int.from_bytes(read_fw_cfg(4), "big")
            files = {}
            for _ in range(count):
                entry = read_fw_cfg(64)
                size = int.from_bytes(entry[0:4], "big")
                selector = int.from_bytes(entry[4:6], "big")
                name = entry[8:].split(b"\0", 1)[0].decode("ascii")
                files[name] = (selector, size)
            return files

        def read_fw_cfg_file(files, name):
            selector, size = files[name]
            select_fw_cfg(selector)
            return read_fw_cfg(size)

        def program_flash1(value):
            writeb(flash1, 0x40)
            writeb(flash1, value)
            writeb(flash1, 0xff)

        stacks = tuple(
            _qmp_command(
                process,
                "qom-get",
                {
                    "path": f"/machine/cpu[{cpu}]",
                    "property": "initial-stack",
                },
            )
            for cpu in range(2)
        )
        assert len(set(stacks)) == 2
        files = fw_cfg_files()
        fdt = read_fw_cfg_file(files, "etc/fdt")
        assert read_fw_cfg_file(files, "opt/mmix/kernel") == kernel_data
        assert read_fw_cfg_file(files, "opt/mmix/initrd") == initrd_data
        assert read_fw_cfg_file(files, "opt/mmix/cmdline") == (
            command_line.encode("ascii") + b"\0"
        )

        original_bios = _qtest_read(qtest, flash0, len(firmware))
        assert original_bios == firmware
        program_flash1(0xa5)
        _qtest_write(qtest, ram_marker, ram_value)
        _qtest_write(qtest, fdt_copy, fdt)
        for stack, value in zip(stacks, stack_values):
            _qtest_write(qtest, stack, value)
        _qtest_writeq(qtest, ipi + 8, 0x2)
        _qtest_writeq(qtest, timer_context, (1 << 64) - 1)
        _qtest_writeq(qtest, timer_context + 8, 0x3)

        bios.write_bytes(bytes(len(firmware)))
        kernel.write_bytes(b"changed kernel\n")
        initrd.write_bytes(b"changed initrd\n")
        _qmp_command(process, "system_reset")

        assert _qtest_read(qtest, flash0, len(firmware)) == original_bios
        assert readb(flash1) == 0xa5
        assert _qtest_read(qtest, ram_marker, len(ram_value)) == ram_value
        assert _qtest_read(qtest, fdt_copy, len(fdt)) == fdt
        for cpu, stack in enumerate(stacks):
            dump = _hmp_register_dump(process, cpu)
            assert _hmp_register_value(dump, "pc=0x") == reset_pc
            assert _hmp_register_value(dump, "npc=0x") == reset_pc + 4
            assert _hmp_register_value(dump, "r0  =0x") == cpu
            assert _hmp_register_value(dump, "r32 =0x") == 0
            assert _hmp_register_value(dump, "rO=0x") == stack
            assert _hmp_register_value(dump, "rS=0x") == stack
            assert _qtest_read(qtest, stack, 8) == bytes(8)
        assert _qtest_readq(qtest, ipi_context1) == 0
        assert _qtest_readq(qtest, timer_context) == 0
        assert _qtest_readq(qtest, timer_context + 8) == 0
        assert _qtest_readq(qtest, timer_context + 0x10) == 0
        assert read_fw_cfg_file(fw_cfg_files(), "opt/mmix/kernel") == kernel_data
        assert read_fw_cfg_file(fw_cfg_files(), "opt/mmix/initrd") == initrd_data

        _qmp_command(process, "cont")
        dumps = _wait_for_cpu_pcs(process, (reset_pc + 4, reset_pc + 4))
        for cpu, dump in enumerate(dumps):
            assert _hmp_register_value(dump, "r32 =0x") == 0x40 + cpu
        for stack, value in zip(stacks, stack_values):
            _qtest_write(qtest, stack, value)
        program_flash1(0xa0)
        _qtest_writeq(qtest, ipi + 8, 0x2)
        _qtest_writeq(qtest, timer_context, (1 << 64) - 1)
        _qtest_writeq(qtest, timer_context + 8, 0x3)
        select_fw_cfg(0)
        assert read_fw_cfg(2) == b"QE"
        result = _qmp_command(
            process,
            "human-monitor-command",
            {"command-line": "savevm firmware-state"},
        )
        assert not result, result

        _qtest_write(qtest, ram_marker, bytes(len(ram_value)))
        _qtest_write(qtest, fdt_copy, bytes(len(fdt)))
        for stack in stacks:
            _qtest_write(qtest, stack, bytes(8))
        program_flash1(0x80)
        _qtest_writeq(qtest, ipi_context1 + 8, 1)
        _qtest_writeq(qtest, timer_context + 8, 0)
        select_fw_cfg(1)
        read_fw_cfg(1)
        _qmp_command(process, "system_reset")

        result = _qmp_command(
            process,
            "human-monitor-command",
            {"command-line": "loadvm firmware-state"},
        )
        assert not result, result
        for cpu, stack in enumerate(stacks):
            dump = _hmp_register_dump(process, cpu)
            assert _hmp_register_value(dump, "pc=0x") == reset_pc + 4
            assert _hmp_register_value(dump, "r32 =0x") == 0x40 + cpu
            assert _qtest_read(qtest, stack, 8) == stack_values[cpu]
        assert _qtest_read(qtest, flash0, len(firmware)) == original_bios
        assert readb(flash1) == 0xa0
        assert _qtest_read(qtest, ram_marker, len(ram_value)) == ram_value
        assert _qtest_read(qtest, fdt_copy, len(fdt)) == fdt
        assert _qtest_readq(qtest, ipi_context1) == 1
        assert _qtest_readq(qtest, timer_context) == (1 << 64) - 1
        assert _qtest_readq(qtest, timer_context + 8) == 0x3
        assert read_fw_cfg(1) == b"M"

        _qmp_command(process, "quit")
        process.communicate(timeout=5)
    except BaseException:
        process.kill()
        _, stderr = process.communicate()
        if stderr:
            print(stderr.decode("utf-8", errors="replace"))
        raise
    finally:
        if connection is not None:
            connection.close()
        listener.close()
        for path in (bios, kernel, initrd, snapshot, qtest_path):
            if path.exists():
                path.unlink()


def run_firmware_handoff_test(qemu, workdir, firmware, kernel, *,
                              cpu_count, memory, initrd=False,
                              command_line=None):
    fdt_address = 0x00100000
    kernel_address = 0x00200000
    record_address = 0x00300000
    release_address = 0x00008000
    success_address = 0x00300800
    success_value = 0x4d4d495846574f4b
    serial = workdir / f"firmware-handoff-{cpu_count}-{memory}.serial"
    qtest_path = workdir / f"firmware-handoff-{cpu_count}-{memory}.sock"
    initrd_path = workdir / f"firmware-handoff-{cpu_count}-{memory}.initrd"

    for path in (serial, qtest_path, initrd_path):
        if path.exists():
            path.unlink()
    args = [
        str(qemu),
        "-machine", "virt",
        "-smp", str(cpu_count),
        "-m", memory,
        "-accel", "tcg,thread=multi",
        "-bios", str(firmware),
        "-kernel", str(kernel),
        "-display", "none",
        "-monitor", "none",
        "-serial", f"file:{serial}",
        "-S",
        "-qmp", "stdio",
        "-qtest", f"unix:{qtest_path}",
        "-qtest-log", "/dev/null",
    ]
    if initrd:
        initrd_path.write_bytes(b"MMIX firmware initrd fixture\n")
        args.extend(("-initrd", str(initrd_path)))
    if command_line is not None:
        args.extend(("-append", command_line))

    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(qtest_path))
    listener.listen(1)
    listener.settimeout(5)
    process = subprocess.Popen(
        args,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    connection = None
    try:
        connection, _ = listener.accept()
        connection.settimeout(5)
        qtest = connection.makefile("rwb", buffering=0)
        greeting = _read_qmp_message(process)
        if "QMP" not in greeting:
            raise AssertionError(f"invalid QMP greeting: {greeting}")
        _qmp_command(process, "qmp_capabilities")
        _qmp_command(process, "cont")

        deadline = time.monotonic() + 10
        while _qtest_readq(qtest, success_address) != success_value:
            if process.poll() is not None:
                raise AssertionError("QEMU exited before firmware handoff")
            if time.monotonic() >= deadline:
                raise AssertionError("timed out waiting for firmware handoff")
            time.sleep(0.01)
        _qmp_command(process, "stop")

        def select_fw_cfg(selector):
            _qtest_command(qtest,
                           f"writew {0x0001000014000008:#x} {selector:#x}")

        def read_fw_cfg(size):
            return bytes(
                int(_qtest_command(
                    qtest, f"readb {0x0001000014000000:#x}"
                )[1], 0)
                for _ in range(size)
            )

        select_fw_cfg(0x19)
        count = int.from_bytes(read_fw_cfg(4), "big")
        files = {}
        for _ in range(count):
            entry = read_fw_cfg(64)
            name = entry[8:].split(b"\0", 1)[0].decode("ascii")
            files[name] = (
                int.from_bytes(entry[4:6], "big"),
                int.from_bytes(entry[0:4], "big"),
            )
        fdt_selector, fdt_size = files["etc/fdt"]
        select_fw_cfg(fdt_selector)
        fdt = read_fw_cfg(fdt_size)

        assert _qtest_read(qtest, fdt_address, fdt_size) == fdt
        assert _qtest_read(qtest, kernel_address,
                           kernel.stat().st_size) == kernel.read_bytes()
        assert _qtest_readq(qtest, release_address) == fdt_address
        stacks = []
        for cpu in range(cpu_count):
            record = record_address + cpu * 32
            assert _qtest_readq(qtest, record) == cpu
            assert _qtest_readq(qtest, record + 8) == fdt_address
            assert _qtest_readq(qtest, record + 16) == 2
            stack = _qtest_readq(qtest, record + 24)
            assert stack != 0
            stacks.append(stack)
        assert len(set(stacks)) == cpu_count
        assert serial.read_bytes() == b"MMIX firmware handoff\n"

        _qmp_command(process, "quit")
        process.communicate(timeout=5)
    except BaseException:
        process.kill()
        _, stderr = process.communicate()
        if stderr:
            print(stderr.decode("utf-8", errors="replace"))
        raise
    finally:
        if connection is not None:
            connection.close()
        listener.close()
        for path in (qtest_path, initrd_path):
            if path.exists():
                path.unlink()


def run_firmware_no_kernel_test(qemu, workdir, firmware, cpu_count):
    serial = workdir / f"firmware-no-kernel-{cpu_count}.serial"

    if serial.exists():
        serial.unlink()
    try:
        subprocess.run(
            [
                qemu,
                "-machine", "virt",
                "-smp", str(cpu_count),
                "-bios", firmware,
                "-display", "none",
                "-monitor", "none",
                "-serial", f"file:{serial}",
            ],
            check=False,
            timeout=0.25,
        )
        raise AssertionError("firmware without a kernel unexpectedly exited")
    except subprocess.TimeoutExpired:
        pass
    assert serial.read_bytes() == b""


def _qmp_start_paused(qemu, image, test):
    command = build_kernel_command(
        qemu,
        image,
        qemu_args=(*test.qemu_args, "-S", "-qmp", "stdio"),
    )
    process = subprocess.Popen(command, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, bufsize=0)
    greeting = _read_qmp_message(process)
    if "QMP" not in greeting:
        process.kill()
        process.wait()
        raise AssertionError(f"invalid QMP greeting: {greeting}")
    _qmp_command(process, "qmp_capabilities")
    return process


def _hmp_register_dump(process, cpu_index):
    return _qmp_command(
        process,
        "human-monitor-command",
        {"command-line": f"info registers {cpu_index}"},
    )


def _hmp_register_value(dump, label):
    match = re.search(rf"{re.escape(label)}([0-9a-fA-F]+)", dump)

    if match is None:
        raise AssertionError(f"missing {label!r} in register dump")
    return int(match.group(1), 16)


def run_linux_entry_state_test(qemu, workdir, test):
    image = workdir / f"{test.name}.elf"

    image.write_bytes(test.image)
    qemu_args = tuple(
        str(image) if arg == "$IMAGE" else arg for arg in test.qemu_args
    )
    command = build_kernel_command(
        qemu,
        image,
        qemu_args=(*qemu_args, "-S", "-qmp", "stdio"),
    )
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    try:
        greeting = _read_qmp_message(process)
        if "QMP" not in greeting:
            raise AssertionError(f"invalid QMP greeting: {greeting}")
        _qmp_command(process, "qmp_capabilities")
        cpus = _qmp_command(process, "query-cpus-fast")
        if len(cpus) != test.cpu_count:
            raise AssertionError(
                f"{test.name}: expected {test.cpu_count} CPUs, got {len(cpus)}"
            )

        fdt = None
        stacks = []
        for cpu in range(test.cpu_count):
            stack = _qmp_command(
                process,
                "qom-get",
                {
                    "path": f"/machine/cpu[{cpu}]",
                    "property": "initial-stack",
                },
            )
            dump = _hmp_register_dump(process, cpu)
            cpu_fdt = _hmp_register_value(dump, "r1  =0x")

            if _hmp_register_value(dump, "pc=0x") != test.entry:
                raise AssertionError(f"{test.name}: CPU {cpu} entry mismatch")
            if _hmp_register_value(dump, "r0  =0x") != cpu:
                raise AssertionError(f"{test.name}: CPU {cpu} ID mismatch")
            if _hmp_register_value(dump, "rL=") != 2:
                raise AssertionError(f"{test.name}: CPU {cpu} rL mismatch")
            ro = _hmp_register_value(dump, "rO=0x")
            rs = _hmp_register_value(dump, "rS=0x")
            if ro != stack or rs != stack:
                raise AssertionError(f"{test.name}: CPU {cpu} stack mismatch")
            rk = _hmp_register_value(dump, "rK =0x")
            rq = _hmp_register_value(dump, "rQ =0x")
            if rk != 0 or rq != 0:
                raise AssertionError(
                    f"{test.name}: CPU {cpu} interrupts are not masked"
                )
            if fdt is None:
                fdt = cpu_fdt
            elif cpu_fdt != fdt:
                raise AssertionError(f"{test.name}: CPU FDT pointers differ")
            stacks.append(stack)

        if fdt is None or fdt == 0 or fdt < test.minimum_fdt or fdt % 8:
            raise AssertionError(f"{test.name}: invalid FDT pointer {fdt!r}")
        if len(set(stacks)) != test.cpu_count:
            raise AssertionError(f"{test.name}: bootstrap stacks overlap")

        _qmp_command(process, "quit")
        process.communicate(timeout=5)
    except BaseException:
        process.kill()
        _, stderr = process.communicate()
        if stderr:
            print(stderr.decode("utf-8", errors="replace"))
        raise


def run_linux_smp_entry_test(qemu, workdir, test):
    image = workdir / f"{test.name}.elf"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.image)
    if log.exists():
        log.unlink()
    completed = run_kernel(
        qemu,
        image,
        trace="int",
        log=log,
        qemu_args=test.qemu_args,
        check=False,
        timeout=10,
    )
    result = read_log(log)
    assert_exit_pc(test.name, result, test.success_pc)
    assert_exit_status(test.name, completed, 0)

    regs = result.regs
    expected = {32: 0, 46: 1, 50: 1, 52: 0x1000, 55: 1, 56: 1}
    assert_regs(test.name, result, expected)
    if regs[33] == 0 or regs[33] % 8 or regs[33] != regs[51]:
        raise AssertionError(f"{test.name}: invalid common FDT pointer")
    if regs[34] != regs[35] or regs[53] != regs[54]:
        raise AssertionError(f"{test.name}: invalid CPU stack pair")
    if regs[34] == regs[53]:
        raise AssertionError(f"{test.name}: CPUs share bootstrap stacks")


def _qtest_readq(qtest, address):
    response = _qtest_command(qtest, f"readq {address:#x}")
    return int(response[1], 0)


def _qtest_writeq(qtest, address, value):
    _qtest_command(qtest, f"writeq {address:#x} {value:#x}")


def _qtest_read(qtest, address, size):
    response = _qtest_command(qtest, f"read {address:#x} {size:#x}")
    return bytes.fromhex(response[1][2:])


def _qtest_write(qtest, address, data):
    _qtest_command(
        qtest, f"write {address:#x} {len(data):#x} 0x{data.hex()}"
    )


def _fdt_property(fdt, node_path, property_name):
    header = struct.unpack_from(">10I", fdt)
    structure_offset = header[2]
    strings_offset = header[3]
    strings_size = header[8]
    strings = fdt[strings_offset:strings_offset + strings_size]
    offset = structure_offset
    nodes = []

    while offset + 4 <= len(fdt):
        token = struct.unpack_from(">I", fdt, offset)[0]
        offset += 4
        if token == 1:
            end = fdt.index(0, offset)
            nodes.append(fdt[offset:end].decode("ascii"))
            offset = (end + 4) & ~3
        elif token == 2:
            nodes.pop()
        elif token == 3:
            size, name_offset = struct.unpack_from(">II", fdt, offset)
            offset += 8
            end = strings.index(0, name_offset)
            name = strings[name_offset:end].decode("ascii")
            value = fdt[offset:offset + size]
            offset = (offset + size + 3) & ~3
            path = "/" + "/".join(filter(None, nodes))
            if path == node_path and name == property_name:
                return value
        elif token == 4:
            continue
        elif token == 9:
            break
        else:
            raise AssertionError(f"invalid FDT token {token}")
    raise AssertionError(f"missing FDT property {node_path}:{property_name}")


def _wait_for_cpu_pcs(process, expected):
    deadline = time.monotonic() + 5

    while time.monotonic() < deadline:
        time.sleep(0.01)
        _qmp_command(process, "stop")
        dumps = [_hmp_register_dump(process, cpu)
                 for cpu in range(len(expected))]
        if all(_hmp_register_value(dump, "pc=0x") == pc
               for dump, pc in zip(dumps, expected)):
            return dumps
        _qmp_command(process, "cont")
    raise AssertionError("CPUs did not reach their expected PCs")


def run_linux_state_test(qemu, workdir, test):
    image = workdir / f"{test.name}.elf"
    initrd = workdir / f"{test.name}.initrd"
    snapshot = workdir / f"{test.name}.qcow2"
    qtest_path = workdir / f"{test.name}.sock"
    qemu_img = qemu.with_name("qemu-img")
    saved_code = bytes.fromhex("1020304050607080")
    saved_bss = bytes.fromhex("8877665544332211")
    saved_fdt = bytes.fromhex("a5a55a5af0f00f0f")
    saved_initrd = bytes.fromhex("0123456789abcdef")
    saved_stacks = (
        bytes.fromhex("1122334455667788"),
        bytes.fromhex("99aabbccddeeff00"),
    )

    image.write_bytes(test.image)
    initrd.write_bytes(test.initrd)
    for path in (snapshot, qtest_path):
        if path.exists():
            path.unlink()
    subprocess.run(
        (qemu_img, "create", "-q", "-f", "qcow2", snapshot, "1M"),
        check=True,
        timeout=10,
    )

    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(qtest_path))
    listener.listen(1)
    listener.settimeout(5)
    qemu_args = tuple(
        str(initrd) if arg == "$INITRD" else arg for arg in test.qemu_args
    )
    command = build_kernel_command(
        qemu,
        image,
        qemu_args=(
            *qemu_args,
            "-S",
            "-qmp", "stdio",
            "-qtest", f"unix:{qtest_path}",
            "-qtest-log", "/dev/null",
            "-drive", f"file={snapshot},format=qcow2,if=none",
        ),
    )
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    connection = None
    try:
        connection, _ = listener.accept()
        connection.settimeout(5)
        qtest = connection.makefile("rwb", buffering=0)
        greeting = _read_qmp_message(process)
        if "QMP" not in greeting:
            raise AssertionError(f"invalid QMP greeting: {greeting}")
        _qmp_command(process, "qmp_capabilities")

        stacks = tuple(
            _qmp_command(
                process,
                "qom-get",
                {
                    "path": f"/machine/cpu[{cpu}]",
                    "property": "initial-stack",
                },
            )
            for cpu in range(2)
        )
        dump = _hmp_register_dump(process, 0)
        fdt_address = _hmp_register_value(dump, "r1  =0x")
        fdt_header = _qtest_read(qtest, fdt_address, 8)
        fdt_size = struct.unpack_from(">I", fdt_header, 4)[0]
        original_fdt = _qtest_read(qtest, fdt_address, fdt_size)
        initrd_address = struct.unpack(
            ">Q", _fdt_property(original_fdt, "/chosen",
                                 "linux,initrd-start")
        )[0]
        original_code = _qtest_read(qtest, test.entry, len(saved_code))
        original_initrd = _qtest_read(qtest, initrd_address,
                                      len(test.initrd))
        assert original_initrd == test.initrd
        assert _qtest_read(qtest, test.bss, len(saved_bss)) == bytes(8)
        for stack in stacks:
            assert _qtest_read(qtest, stack, 8) == bytes(8)

        _qmp_command(process, "cont")
        saved_dumps = _wait_for_cpu_pcs(process, test.idle_pcs)
        for cpu, dump in enumerate(saved_dumps):
            assert _hmp_register_value(dump, "r32 =0x") == cpu
            assert _hmp_register_value(dump, "r33 =0x") == 0x40 + cpu

        _qtest_write(qtest, test.entry, saved_code)
        _qtest_write(qtest, test.bss, saved_bss)
        _qtest_write(qtest, fdt_address + 16, saved_fdt)
        _qtest_write(qtest, initrd_address, saved_initrd)
        for stack, value in zip(stacks, saved_stacks):
            _qtest_write(qtest, stack, value)
        result = _qmp_command(
            process,
            "human-monitor-command",
            {"command-line": "savevm linux-loader-state"},
        )
        assert not result, result

        _qmp_command(process, "system_reset")
        for cpu in range(2):
            dump = _hmp_register_dump(process, cpu)
            assert _hmp_register_value(dump, "pc=0x") == test.entry
            assert _hmp_register_value(dump, "r0  =0x") == cpu
            assert _hmp_register_value(dump, "r1  =0x") == fdt_address
            assert _hmp_register_value(dump, "rO=0x") == stacks[cpu]
            assert _hmp_register_value(dump, "rS=0x") == stacks[cpu]
        assert _qtest_read(qtest, test.entry, 8) == original_code
        assert _qtest_read(qtest, test.bss, 8) == bytes(8)
        assert _qtest_read(qtest, fdt_address, fdt_size) == original_fdt
        assert _qtest_read(qtest, initrd_address,
                           len(test.initrd)) == original_initrd
        for stack in stacks:
            assert _qtest_read(qtest, stack, 8) == bytes(8)

        result = _qmp_command(
            process,
            "human-monitor-command",
            {"command-line": "loadvm linux-loader-state"},
        )
        assert not result, result
        for cpu, expected_pc in enumerate(test.idle_pcs):
            dump = _hmp_register_dump(process, cpu)
            assert _hmp_register_value(dump, "pc=0x") == expected_pc
            assert _hmp_register_value(dump, "r32 =0x") == cpu
            assert _hmp_register_value(dump, "r33 =0x") == 0x40 + cpu
            assert _hmp_register_value(dump, "r1  =0x") == fdt_address
        assert _qtest_read(qtest, test.entry, 8) == saved_code
        assert _qtest_read(qtest, test.bss, 8) == saved_bss
        assert _qtest_read(qtest, fdt_address + 16, 8) == saved_fdt
        assert _qtest_read(qtest, initrd_address, 8) == saved_initrd
        for stack, value in zip(stacks, saved_stacks):
            assert _qtest_read(qtest, stack, 8) == value

        _qmp_command(process, "quit")
        process.communicate(timeout=5)
    except BaseException:
        process.kill()
        _, stderr = process.communicate()
        if stderr:
            print(stderr.decode("utf-8", errors="replace"))
        raise
    finally:
        if connection is not None:
            connection.close()
        listener.close()
        for path in (snapshot, qtest_path):
            if path.exists():
                path.unlink()


def _wait_for_pc(process, expected):
    deadline = time.monotonic() + 5

    while time.monotonic() < deadline:
        time.sleep(0.01)
        _qmp_command(process, "stop")
        dump = _hmp_register_dump(process, 0)
        if _hmp_register_value(dump, "pc=0x") == expected:
            return dump
        _qmp_command(process, "cont")
    raise AssertionError(f"CPU did not reach PC 0x{expected:x}")


def run_elf_state_test(qemu, workdir, test):
    image = workdir / f"{test.name}.elf"
    snapshot = workdir / f"{test.name}.qcow2"
    qtest_path = workdir / f"{test.name}.sock"
    qemu_img = qemu.with_name("qemu-img")
    saved_code = 0x1020304050607080
    saved_bss = 0x8877665544332211
    saved_argument = 0xa5a55a5af0f00f0f
    saved_argument_end = 0x5a5aa5a50f0ff0f0
    saved_stack = 0x0123456789abcdef

    image.write_bytes(test.image)
    for path in (snapshot, qtest_path):
        if path.exists():
            path.unlink()
    subprocess.run(
        (qemu_img, "create", "-q", "-f", "qcow2", snapshot, "1M"),
        check=True,
        timeout=10,
    )

    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(qtest_path))
    listener.listen(1)
    listener.settimeout(5)
    command = build_kernel_command(
        qemu,
        image,
        qemu_args=(
            *test.qemu_args,
            "-S",
            "-qmp",
            "stdio",
            "-qtest",
            f"unix:{qtest_path}",
            "-qtest-log",
            "/dev/null",
            "-drive",
            f"file={snapshot},format=qcow2,if=none",
        ),
    )
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    connection = None
    try:
        connection, _ = listener.accept()
        connection.settimeout(5)
        qtest = connection.makefile("rwb", buffering=0)
        greeting = _read_qmp_message(process)
        if "QMP" not in greeting:
            raise AssertionError(f"invalid QMP greeting: {greeting}")
        _qmp_command(process, "qmp_capabilities")
        stack = _qmp_command(
            process,
            "qom-get",
            {"path": "/machine/cpu[0]", "property": "initial-stack"},
        )

        _qmp_command(process, "cont")
        dump = _wait_for_pc(process, test.idle_pc)
        argument_argv = _hmp_register_value(dump, "r34 =0x")
        argument_base = argument_argv - 8
        argument_end = argument_argv + 40
        expected_argument = argument_argv + 24
        assert _hmp_register_value(dump, "r32 =0x") == test.global_value
        assert _hmp_register_value(dump, "r33 =0x") == 2
        assert _hmp_register_value(dump, "r35 =0x") == 250
        assert _qtest_readq(qtest, test.bss) == 0
        assert _qtest_readq(qtest, argument_base) == argument_end
        assert _qtest_readq(qtest, argument_argv) == expected_argument
        assert _qtest_readq(qtest, stack) == 0

        _qtest_writeq(qtest, test.entry, saved_code)
        _qtest_writeq(qtest, test.bss, saved_bss)
        _qtest_writeq(qtest, argument_base, saved_argument_end)
        _qtest_writeq(qtest, argument_argv, saved_argument)
        _qtest_writeq(qtest, stack, saved_stack)
        result = _qmp_command(
            process,
            "human-monitor-command",
            {"command-line": "savevm loader-state"},
        )
        assert not result, result

        _qmp_command(process, "system_reset")
        dump = _hmp_register_dump(process, 0)
        assert _hmp_register_value(dump, "pc=0x") == test.entry
        assert _hmp_register_value(dump, "r0  =0x") == 2
        assert _hmp_register_value(dump, "r1  =0x") == argument_argv
        assert _hmp_register_value(dump, "rG =0x") == 250
        assert _hmp_register_value(dump, "r250=0x") == test.global_value
        assert _qtest_readq(qtest, test.entry) != saved_code
        assert _qtest_readq(qtest, test.bss) == 0
        assert _qtest_readq(qtest, argument_base) == argument_end
        assert _qtest_readq(qtest, argument_argv) == expected_argument
        assert _qtest_readq(qtest, stack) == 0

        result = _qmp_command(
            process,
            "human-monitor-command",
            {"command-line": "loadvm loader-state"},
        )
        assert not result, result
        dump = _hmp_register_dump(process, 0)
        assert _hmp_register_value(dump, "pc=0x") == test.idle_pc, dump
        assert _hmp_register_value(dump, "r32 =0x") == test.global_value
        assert _hmp_register_value(dump, "r33 =0x") == 2
        assert _hmp_register_value(dump, "r34 =0x") == argument_argv
        assert _hmp_register_value(dump, "r35 =0x") == 250
        assert _qtest_readq(qtest, test.entry) == saved_code
        assert _qtest_readq(qtest, test.bss) == saved_bss
        assert _qtest_readq(qtest, argument_base) == saved_argument_end
        assert _qtest_readq(qtest, argument_argv) == saved_argument
        assert _qtest_readq(qtest, stack) == saved_stack

        _qmp_command(process, "quit")
        process.communicate(timeout=5)
    except BaseException:
        process.kill()
        _, stderr = process.communicate()
        if stderr:
            print(stderr.decode("utf-8", errors="replace"))
        raise
    finally:
        if connection is not None:
            connection.close()
        listener.close()
        for path in (snapshot, qtest_path):
            if path.exists():
                path.unlink()


def run_mttcg_reset_test(qemu, workdir, test):
    image = workdir / f"{test.name}.elf"

    image.write_bytes(test.image)
    process = _qmp_start_paused(qemu, image, test)
    try:
        _qmp_command(process, "cont")
        for _ in range(500):
            time.sleep(0.01)
            _qmp_command(process, "stop")
            dumps = [
                _hmp_register_dump(process, cpu)
                for cpu in range(test.cpu_count)
            ]
            if all(_hmp_register_value(dump, "pc=0x") == test.idle_pc
                   for dump in dumps):
                break
            _qmp_command(process, "cont")
        else:
            raise AssertionError(
                f"{test.name}: CPUs did not reach the reset rendezvous"
            )

        _qmp_command(process, "system_reset")
        for _ in range(500):
            time.sleep(0.01)
            _qmp_command(process, "stop")
            reset_dumps = [
                _hmp_register_dump(process, cpu)
                for cpu in range(test.cpu_count)
            ]
            if all(_hmp_register_value(dump, "pc=0x") == test.reset_idle_pc
                   for dump in reset_dumps):
                break
            _qmp_command(process, "cont")
        else:
            raise AssertionError(
                f"{test.name}: CPUs did not reach the post-reset rendezvous"
            )

        for cpu, dump in enumerate(reset_dumps):
            for label, value in test.reset_regs[cpu].items():
                actual = _hmp_register_value(dump, label)
                if actual != value:
                    raise AssertionError(
                        f"{test.name}: CPU {cpu} {label} expected "
                        f"0x{value:x}, got 0x{actual:x}"
                    )

        _qmp_command(process, "quit")
        process.communicate(timeout=5)
    except BaseException:
        process.kill()
        process.wait()
        raise


def _qtest_command(stream, command):
    stream.write(command.encode("ascii") + b"\n")
    response = stream.readline()
    if not response:
        raise AssertionError("QEMU closed the QTest connection")
    if not response.startswith(b"OK"):
        raise AssertionError(
            f"QTest command {command!r} failed: "
            f"{response.decode('utf-8', errors='replace').rstrip()}"
        )
    return response.decode("ascii").split()


def _run_smp_interrupt_protocol(qtest, test):
    def address(cpu, offset):
        return test.mailbox_base + cpu * test.mailbox_slot_size + offset

    def read(cpu, offset):
        response = _qtest_command(qtest, f"readq {address(cpu, offset):#x}")
        return int(response[1], 0)

    def write(cpu, offset, value):
        _qtest_command(
            qtest,
            f"writeq {address(cpu, offset):#x} {value:#x}",
        )

    def set_irq(irq, level):
        _qtest_command(
            qtest,
            f"set_irq_in /machine/intc unnamed-gpio-in {irq} {level}",
        )

    def wait(cpu, offset, expected, description):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            actual = read(cpu, offset)
            stage = read(cpu, test.stage_offset)
            if stage == test.stage_failure:
                raise AssertionError(
                    f"{test.name}: CPU {cpu} failed while {description}"
                )
            if actual == expected:
                return
            time.sleep(0.001)
        raise AssertionError(
            f"{test.name}: CPU {cpu} timed out while {description}; "
            f"expected {expected:#x}, got {actual:#x}"
        )

    def command(cpu, value, expected_stage, description):
        write(cpu, test.command_offset, value)
        wait(cpu, test.command_offset, 0,
             f"consuming command for {description}")
        wait(cpu, test.stage_offset, expected_stage, description)

    for cpu in range(test.cpu_count):
        wait(cpu, test.stage_offset, test.stage_ready, "waiting for startup")

    for cpu in range(test.cpu_count):
        assert read(cpu, test.rk_initial_offset) == test.initial_rk[cpu]

    set_irq(test.timer_irq_base, 1)
    set_irq(test.timer_irq_base + 1, 1)
    for cpu in range(test.cpu_count):
        command(cpu, test.command_wait_rq, test.stage_rq_latched,
                "waiting for a masked request")
        assert read(cpu, test.rq_initial_offset) & test.interrupt_request
        assert read(cpu, test.handler_count_offset) == 0

    command(0, test.command_claim_ack, test.stage_claimed,
            "claiming CPU0's request")
    assert read(0, test.claim_offset) == test.timer_irq_base

    command(1, test.command_snapshot, test.stage_snapshot,
            "checking CPU1's independent request")
    assert read(1, test.rq_after_offset) & test.interrupt_request

    set_irq(test.timer_irq_base, 0)
    command(0, test.command_complete, test.stage_completed,
            "completing CPU0's claimed request")
    assert read(0, test.rq_after_offset) & test.interrupt_request == 0
    assert read(1, test.rq_after_offset) & test.interrupt_request

    write(1, test.command_offset, test.command_enable)
    wait(1, test.handler_count_offset, 1, "entering CPU1's first handler")
    assert read(0, test.handler_count_offset) == 0
    assert read(1, test.claim_offset) == test.timer_irq_base + 1
    set_irq(test.timer_irq_base + 1, 0)
    write(1, test.handler_ack_offset, 1)
    wait(1, test.handler_done_offset, 1, "resuming CPU1's first request")
    wait(1, test.stage_offset, test.stage_enabled,
         "returning to CPU1's interrupted command")

    command(0, test.command_enable, test.stage_enabled,
            "enabling CPU0 delivery")
    assert read(0, test.handler_count_offset) == 0

    for cpu, count in ((0, 1), (1, 2)):
        irq = test.timer_irq_base + cpu
        set_irq(irq, 1)
        wait(cpu, test.handler_count_offset, count,
             f"entering CPU{cpu}'s independent handler")
        claim = read(cpu, test.claim_offset)
        assert claim == irq, claim
        set_irq(irq, 0)
        write(cpu, test.handler_ack_offset, count)
        wait(cpu, test.handler_done_offset, count,
             f"resuming CPU{cpu}'s independent request")
        command(cpu, test.command_snapshot, test.stage_snapshot,
                f"confirming CPU{cpu} resumed its instruction stream")

    set_irq(test.timer_irq_base, 1)
    set_irq(test.timer_irq_base + 1, 1)
    wait(0, test.handler_count_offset, 2,
         "entering CPU0's simultaneous handler")
    wait(1, test.handler_count_offset, 3,
         "entering CPU1's simultaneous handler")
    cpu0_claim = read(0, test.claim_offset)
    cpu1_claim = read(1, test.claim_offset)
    assert cpu0_claim == test.timer_irq_base, cpu0_claim
    assert cpu1_claim == test.timer_irq_base + 1, cpu1_claim
    set_irq(test.timer_irq_base, 0)
    set_irq(test.timer_irq_base + 1, 0)
    write(0, test.handler_ack_offset, 2)
    write(1, test.handler_ack_offset, 3)
    for cpu, count in ((0, 2), (1, 3)):
        wait(cpu, test.handler_done_offset, count,
             f"resuming CPU{cpu}'s simultaneous request")

    for cpu in range(test.cpu_count):
        command(cpu, test.command_finalize, test.stage_final,
                "recording final CPU-local state")

    snapshots = []
    for cpu, expected_count in enumerate((2, 3)):
        snapshot = {
            "rWW": read(cpu, test.rww_offset),
            "rXX": read(cpu, test.rxx_offset),
            "rYY": read(cpu, test.ryy_offset),
            "rZZ": read(cpu, test.rzz_offset),
            "rBB": read(cpu, test.rbb_offset),
            "rO": read(cpu, test.ro_entry_offset),
            "rS": read(cpu, test.rs_entry_offset),
        }
        snapshots.append(snapshot)

        assert read(cpu, test.handler_count_offset) == expected_count
        assert read(cpu, test.handler_done_offset) == expected_count
        assert read(cpu, test.claim_offset) == test.timer_irq_base + cpu
        assert snapshot["rWW"] % 4 == 0
        assert test.main_start <= snapshot["rWW"] < test.main_end
        assert snapshot["rXX"] & test.dynamic_trap_resume_next
        assert read(cpu, test.handler_rq_offset) & test.interrupt_request
        assert snapshot["rBB"] == test.sentinels[cpu]
        assert snapshot["rO"] == snapshot["rS"]
        assert snapshot["rO"] % 0x2000 == 0
        assert read(cpu, test.ro_final_offset) == snapshot["rO"]
        assert read(cpu, test.rs_final_offset) == snapshot["rS"]
        assert read(cpu, test.rq_final_offset) & test.interrupt_request == 0
        assert read(cpu, test.rk_final_offset) == test.interrupt_request
        assert read(cpu, test.sentinel_final_offset) == test.sentinels[cpu]

    assert snapshots[0] != snapshots[1]
    write(0, test.command_offset, test.command_halt)


def _run_smp_timer_protocol(qtest, test):
    def address(cpu, offset):
        return test.mailbox_base + cpu * test.mailbox_slot_size + offset

    def read(cpu, offset):
        response = _qtest_command(qtest, f"readq {address(cpu, offset):#x}")
        return int(response[1], 0)

    def write(cpu, offset, value):
        _qtest_command(
            qtest,
            f"writeq {address(cpu, offset):#x} {value:#x}",
        )

    def read_mmio(address):
        response = _qtest_command(qtest, f"readq {address:#x}")
        return int(response[1], 0)

    def wait(cpu, offset, expected, description):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            actual = read(cpu, offset)
            stage = read(cpu, test.stage_offset)
            if stage == test.stage_failure:
                raise AssertionError(
                    f"{test.name}: CPU {cpu} failed while {description}"
                )
            if actual == expected:
                return
            time.sleep(0.001)
        raise AssertionError(
            f"{test.name}: CPU {cpu} timed out while {description}; "
            f"expected {expected:#x}, got {actual:#x}"
        )

    def command(cpu, value, expected_stage, description):
        write(cpu, test.command_offset, value)
        wait(cpu, test.command_offset, 0,
             f"consuming command for {description}")
        wait(cpu, test.stage_offset, expected_stage, description)

    def program(cpu, deadline):
        write(cpu, test.deadline_offset, deadline)
        command(cpu, test.command_program, test.stage_programmed,
                f"programming CPU{cpu}'s timer")

    def enable(cpu):
        command(cpu, test.command_enable, test.stage_enabled,
                f"enabling CPU{cpu}'s timer")

    def wait_for_delivery(cpu, count):
        wait(cpu, test.handler_count_offset, count,
             f"entering CPU{cpu}'s timer handler {count}")
        wait(cpu, test.handler_done_offset, count,
             f"resuming CPU{cpu}'s timer handler {count}")
        command(cpu, test.command_snapshot, test.stage_resumed,
                f"confirming CPU{cpu} resumed after timer {count}")
        assert read(cpu, test.claim_offset) == test.timer_irq_base + cpu
        assert read(cpu, test.timer_status_offset) == test.timer_status_pending
        status_address = test.timer_context_address(
            cpu, test.timer_context_status
        )
        assert read_mmio(status_address) == 0

    for cpu in range(test.cpu_count):
        wait(cpu, test.stage_offset, test.stage_ready, "waiting for startup")

    deadline = read_mmio(test.timer_base + test.timer_time) + 100_000_000
    program(0, deadline)
    enable(0)
    wait_for_delivery(0, 1)
    assert read(1, test.handler_count_offset) == 0
    assert read_mmio(test.timer_context_address(
        1, test.timer_context_status)) == 0

    deadline = read_mmio(test.timer_base + test.timer_time) + 100_000_000
    program(1, deadline)
    enable(1)
    wait_for_delivery(1, 1)
    assert read(0, test.handler_count_offset) == 1

    deadline = read_mmio(test.timer_base + test.timer_time) + 250_000_000
    for cpu in range(test.cpu_count):
        program(cpu, deadline)
    for cpu in range(test.cpu_count):
        write(cpu, test.command_offset, test.command_enable)
    for cpu in range(test.cpu_count):
        wait(cpu, test.command_offset, 0,
             f"consuming simultaneous enable for CPU{cpu}")
        wait(cpu, test.stage_offset, test.stage_enabled,
             f"enabling CPU{cpu}'s simultaneous timer")
    for cpu in range(test.cpu_count):
        wait_for_delivery(cpu, 2)

    for cpu in range(test.cpu_count):
        command(cpu, test.command_finalize, test.stage_final,
                "recording final CPU-local timer state")

    snapshots = []
    for cpu in range(test.cpu_count):
        snapshot = {
            "rWW": read(cpu, test.rww_offset),
            "rXX": read(cpu, test.rxx_offset),
            "rYY": read(cpu, test.ryy_offset),
            "rZZ": read(cpu, test.rzz_offset),
            "rBB": read(cpu, test.rbb_offset),
            "rO": read(cpu, test.ro_entry_offset),
            "rS": read(cpu, test.rs_entry_offset),
        }
        snapshots.append(snapshot)

        assert read(cpu, test.handler_count_offset) == 2
        assert read(cpu, test.handler_done_offset) == 2
        assert read(cpu, test.claim_offset) == test.timer_irq_base + cpu
        assert snapshot["rWW"] % 4 == 0
        assert test.main_start <= snapshot["rWW"] < test.main_end
        assert snapshot["rXX"] & test.dynamic_trap_resume_next
        assert read(cpu, test.handler_rq_offset) & test.interrupt_request
        assert snapshot["rBB"] == test.sentinels[cpu]
        assert snapshot["rO"] == snapshot["rS"]
        assert snapshot["rO"] % 0x2000 == 0
        assert read(cpu, test.ro_final_offset) == snapshot["rO"]
        assert read(cpu, test.rs_final_offset) == snapshot["rS"]
        assert read(cpu, test.rq_final_offset) & test.interrupt_request == 0
        assert read(cpu, test.rk_final_offset) == test.interrupt_request
        assert read(cpu, test.sentinel_final_offset) == test.sentinels[cpu]

    assert snapshots[0] != snapshots[1]
    write(0, test.command_offset, test.command_halt)


def _run_smp_shared_interrupt_protocol(qtest, test):
    def address(cpu, offset):
        return test.mailbox_base + cpu * test.mailbox_slot_size + offset

    def read(cpu, offset):
        response = _qtest_command(qtest, f"readq {address(cpu, offset):#x}")
        return int(response[1], 0)

    def write(cpu, offset, value):
        _qtest_command(
            qtest,
            f"writeq {address(cpu, offset):#x} {value:#x}",
        )

    def read_mmio(address):
        response = _qtest_command(qtest, f"readq {address:#x}")
        return int(response[1], 0)

    def write_mmio(address, value, width="q"):
        _qtest_command(qtest, f"write{width} {address:#x} {value:#x}")

    def set_irq(level):
        _qtest_command(
            qtest,
            f"set_irq_in /machine/intc unnamed-gpio-in "
            f"{test.shared_irq} {level}",
        )

    def wait(cpu, offset, expected, description):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            actual = read(cpu, offset)
            stage = read(cpu, test.stage_offset)
            if stage == test.stage_failure:
                raise AssertionError(
                    f"{test.name}: CPU {cpu} failed while {description}"
                )
            if actual == expected:
                return
            time.sleep(0.001)
        raise AssertionError(
            f"{test.name}: CPU {cpu} timed out while {description}; "
            f"expected {expected:#x}, got {actual:#x}"
        )

    def wait_at_least(cpu, offset, expected, description):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            actual = read(cpu, offset)
            stage = read(cpu, test.stage_offset)
            if stage == test.stage_failure:
                raise AssertionError(
                    f"{test.name}: CPU {cpu} failed while {description}"
                )
            if actual >= expected:
                return
            time.sleep(0.001)
        raise AssertionError(
            f"{test.name}: CPU {cpu} timed out while {description}; "
            f"expected at least {expected:#x}, got {actual:#x}"
        )

    def wait_for_progress(cpu, previous, description):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            actual = read(cpu, test.progress_offset)
            if actual != previous:
                return
            time.sleep(0.001)
        raise AssertionError(
            f"{test.name}: CPU {cpu} made no progress while {description}"
        )

    def wait_at_least(cpu, offset, expected, description):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            actual = read(cpu, offset)
            stage = read(cpu, test.stage_offset)
            if stage == test.stage_failure:
                raise AssertionError(
                    f"{test.name}: CPU {cpu} failed while {description}"
                )
            if actual >= expected:
                return
            time.sleep(0.001)
        raise AssertionError(
            f"{test.name}: CPU {cpu} timed out while {description}; "
            f"expected at least {expected:#x}, got {actual:#x}"
        )

    def intc_enable_address(cpu):
        return (
            test.intc_base + test.intc_context_base +
            cpu * test.intc_context_stride + test.intc_enable
        )

    def timer_context_address(cpu, register):
        return (
            test.timer_base + test.timer_context_base +
            cpu * test.timer_context_stride + register
        )

    def program_expired_timer(cpu):
        write_mmio(timer_context_address(cpu, test.timer_compare), 0)
        write_mmio(
            timer_context_address(cpu, test.timer_control),
            test.timer_control_enabled,
        )

    for cpu in range(test.cpu_count):
        wait(cpu, test.stage_offset, test.stage_ready, "waiting for startup")

    cpu1_progress = read(1, test.progress_offset)
    set_irq(1)
    wait(0, test.handler_count_offset, 1,
         "entering CPU0's first shared handler")
    assert read(0, test.claim_offset) == test.shared_irq
    assert read(1, test.handler_count_offset) == 0
    wait_for_progress(1, cpu1_progress,
                      "CPU0 owns the shared interrupt")

    write(0, test.handler_ack_offset, 1)
    wait(0, test.handler_done_offset, 1,
         "completing CPU0's still-high shared source")
    wait(0, test.handler_count_offset, 2,
         "reentering CPU0 for the shared-source retrigger")
    assert read(0, test.claim_offset) == test.shared_irq
    assert read(1, test.handler_count_offset) == 0
    set_irq(0)
    write(0, test.handler_ack_offset, 2)
    wait(0, test.handler_done_offset, 2,
         "completing CPU0's withdrawn shared source")
    wait(0, test.resume_count_offset, 2,
         "resuming CPU0 after shared-source retrigger")

    cpu0_timer_mask = 1 << test.timer_irq_base
    write_mmio(intc_enable_address(0), cpu0_timer_mask)
    write_mmio(
        intc_enable_address(1),
        (1 << test.shared_irq) | (1 << (test.timer_irq_base + 1)),
    )
    set_irq(1)
    wait(1, test.handler_count_offset, 1,
         "retargeting the shared source to CPU1")
    assert read(1, test.claim_offset) == test.shared_irq
    assert read(0, test.handler_count_offset) == 2
    set_irq(0)
    write(1, test.handler_ack_offset, 1)
    wait(1, test.handler_done_offset, 1,
         "completing CPU1's first shared source")
    wait(1, test.resume_count_offset, 1,
         "resuming CPU1 after retargeting")

    set_irq(1)
    wait(1, test.handler_count_offset, 2,
         "entering CPU1's combined shared handler")
    assert read(1, test.claim_offset) == test.shared_irq
    for cpu in range(test.cpu_count):
        program_expired_timer(cpu)

    wait(0, test.handler_count_offset, 3,
         "entering CPU0's fixed timer handler")
    wait(0, test.handler_done_offset, 3,
         "completing CPU0's fixed timer handler")
    wait(0, test.resume_count_offset, 3,
         "resuming CPU0 after its fixed timer")
    assert read(0, test.claim_offset) == test.timer_irq_base

    set_irq(0)
    write(1, test.handler_ack_offset, 2)
    wait_at_least(1, test.handler_done_offset, 2,
                  "completing CPU1's combined shared source")
    wait(1, test.handler_count_offset, 3,
         "entering CPU1's fixed timer handler")
    wait(1, test.handler_done_offset, 3,
         "completing CPU1's fixed timer handler")
    wait(1, test.resume_count_offset, 3,
         "resuming CPU1 after its fixed timer")
    assert read(1, test.claim_offset) == test.timer_irq_base + 1

    snapshots = []
    for cpu in range(test.cpu_count):
        snapshot = {
            "rWW": read(cpu, test.rww_offset),
            "rXX": read(cpu, test.rxx_offset),
            "rYY": read(cpu, test.ryy_offset),
            "rZZ": read(cpu, test.rzz_offset),
            "rBB": read(cpu, test.rbb_offset),
            "rO": read(cpu, test.ro_entry_offset),
            "rS": read(cpu, test.rs_entry_offset),
        }
        snapshots.append(snapshot)

        assert read(cpu, test.handler_count_offset) == 3
        assert read(cpu, test.handler_done_offset) == 3
        assert read(cpu, test.resume_count_offset) == 3
        assert read(cpu, test.shared_count_offset) == 2
        assert read(cpu, test.timer_count_offset) == 1
        assert snapshot["rWW"] % 4 == 0
        assert test.main_start <= snapshot["rWW"] < test.main_end
        assert snapshot["rXX"] & test.dynamic_trap_resume_next
        assert read(cpu, test.handler_rq_offset) & test.interrupt_request
        assert snapshot["rBB"] == test.sentinels[cpu]
        assert snapshot["rO"] == snapshot["rS"]
        assert snapshot["rO"] % 0x2000 == 0
        assert read(cpu, test.ro_final_offset) == snapshot["rO"]
        assert read(cpu, test.rs_final_offset) == snapshot["rS"]
        assert read(cpu, test.rq_final_offset) & test.interrupt_request == 0
        assert read(cpu, test.rk_final_offset) == test.interrupt_request
        assert read(cpu, test.sentinel_final_offset) == test.sentinels[cpu]
        assert read_mmio(timer_context_address(cpu, test.timer_status)) == 0

    assert snapshots[0] != snapshots[1]
    write(0, test.halt_offset, 1)


def _run_smp_ipi_protocol(qtest, test):
    def address(cpu, offset):
        return test.mailbox_base + cpu * test.mailbox_slot_size + offset

    def read(cpu, offset):
        response = _qtest_command(qtest, f"readq {address(cpu, offset):#x}")
        return int(response[1], 0)

    def write(cpu, offset, value):
        _qtest_command(
            qtest,
            f"writeq {address(cpu, offset):#x} {value:#x}",
        )

    def read_mmio(mmio_address):
        response = _qtest_command(qtest, f"readq {mmio_address:#x}")
        return int(response[1], 0)

    def set_shared_irq(level):
        _qtest_command(
            qtest,
            f"set_irq_in /machine/intc unnamed-gpio-in "
            f"{test.shared_irq} {level}",
        )

    def wait(cpu, offset, expected, description):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            actual = read(cpu, offset)
            stage = read(cpu, test.stage_offset)
            if stage == test.stage_failure:
                raise AssertionError(
                    f"{test.name}: CPU {cpu} failed with reason "
                    f"{read(cpu, test.failure_offset)} while {description}"
                )
            if actual == expected:
                return
            time.sleep(0.001)
        raise AssertionError(
            f"{test.name}: CPU {cpu} timed out while {description}; "
            f"expected {expected:#x}, got {actual:#x}"
        )

    def command(cpu, value, expected_stage, description):
        write(cpu, test.command_offset, value)
        wait(cpu, test.command_offset, 0,
             f"consuming command for {description}")
        wait(cpu, test.stage_offset, expected_stage, description)

    def prepare_send(cpu, targets, generation, twice=False):
        write(cpu, test.send_mask_offset, targets)
        write(cpu, test.generation_offset, generation)
        write(cpu, test.command_offset,
              test.command_send_twice if twice else test.command_send)

    def wait_send(cpu, description):
        wait(cpu, test.command_offset, 0,
             f"consuming send command for {description}")
        wait(cpu, test.stage_offset, test.stage_sent, description)

    def send(sender, targets, generation, counts, extra_handlers,
             twice=False):
        prepare_send(sender, targets, generation, twice=twice)
        wait_send(sender, f"CPU{sender} sending {targets:#x}")
        for target in range(test.cpu_count):
            if targets & (1 << target):
                counts[target] += 1
                wait(target, test.ack_offset, generation,
                     f"CPU{target} acknowledging generation {generation}")
                wait(target, test.ipi_count_offset, counts[target],
                     f"CPU{target} recording IPI {counts[target]}")
                wait(target, test.handler_done_offset,
                     counts[target] + extra_handlers[target],
                     f"CPU{target} resuming from IPI {counts[target]}")
        command(sender, test.command_observe_ack, test.stage_acked,
                f"CPU{sender} observing generation {generation}")
        assert read(sender, test.sender_ack_offset) == generation

    def ipi_status_address(cpu):
        return (
            test.ipi_base + test.ipi_context_base +
            cpu * test.ipi_context_stride + test.ipi_context_status
        )

    def timer_status_address(cpu):
        return (
            test.timer_base + test.timer_context_base +
            cpu * test.timer_context_stride + test.timer_status
        )

    for cpu in range(test.cpu_count):
        wait(cpu, test.stage_offset, test.stage_ready, "waiting for startup")

    ipi_counts = [0, 0]
    extra_handlers = [0, 0]
    send(0, 0x1, 0x101, ipi_counts, extra_handlers)
    assert read(1, test.handler_count_offset) == 0
    send(0, 0x2, 0x102, ipi_counts, extra_handlers)
    send(1, 0x1, 0x103, ipi_counts, extra_handlers)

    prepare_send(0, 0x2, 0x104)
    prepare_send(1, 0x1, 0x105)
    wait_send(0, "CPU0 reciprocal send")
    wait_send(1, "CPU1 reciprocal send")
    ipi_counts[0] += 1
    ipi_counts[1] += 1
    wait(0, test.ack_offset, 0x105, "CPU0 reciprocal acknowledgement")
    wait(1, test.ack_offset, 0x104, "CPU1 reciprocal acknowledgement")
    for cpu in range(test.cpu_count):
        wait(cpu, test.ipi_count_offset, ipi_counts[cpu],
             f"CPU{cpu} reciprocal delivery")
        wait(cpu, test.handler_done_offset, ipi_counts[cpu],
             f"CPU{cpu} resuming from reciprocal delivery")
    for cpu, generation in ((0, 0x104), (1, 0x105)):
        command(cpu, test.command_observe_ack, test.stage_acked,
                f"CPU{cpu} observing its reciprocal acknowledgement")
        assert read(cpu, test.sender_ack_offset) == generation

    send(0, 0x3, 0x106, ipi_counts, extra_handlers)

    command(1, test.command_mask, test.stage_masked,
            "masking CPU1 IPI delivery")
    prepare_send(0, 0x2, 0x107, twice=True)
    wait_send(0, "sending duplicate masked IPIs")
    assert read(1, test.ipi_count_offset) == ipi_counts[1]
    assert read_mmio(ipi_status_address(1)) == test.ipi_status_pending
    command(1, test.command_enable, test.stage_enabled,
            "enabling CPU1 IPI delivery")
    ipi_counts[1] += 1
    wait(1, test.ack_offset, 0x107,
         "CPU1 acknowledging the coalesced IPI")
    wait(1, test.ipi_count_offset, ipi_counts[1],
         "CPU1 recording one coalesced IPI")
    wait(1, test.handler_done_offset, ipi_counts[1],
         "CPU1 resuming from the coalesced IPI")
    command(0, test.command_observe_ack, test.stage_acked,
            "CPU0 observing the coalesced acknowledgement")
    assert read(0, test.sender_ack_offset) == 0x107

    send(0, 0x2, 0x108, ipi_counts, extra_handlers)

    set_shared_irq(1)
    wait(0, test.shared_count_offset, 1,
         "CPU0 claiming the shared source")
    prepare_send(1, 0x1, 0x109)
    wait_send(1, "CPU1 sending while CPU0 handles the shared source")
    assert read_mmio(ipi_status_address(0)) == test.ipi_status_pending

    command(1, test.command_timer, test.stage_timer,
            "programming CPU1's timer")
    wait(1, test.timer_count_offset, 1,
         "CPU1 handling its timer")
    extra_handlers[1] += 1
    wait(1, test.handler_done_offset,
         ipi_counts[1] + extra_handlers[1],
         "CPU1 resuming from its timer")

    set_shared_irq(0)
    write(0, test.shared_release_offset, 1)
    extra_handlers[0] += 1
    ipi_counts[0] += 1
    wait(0, test.ack_offset, 0x109,
         "CPU0 handling the IPI queued behind the shared source")
    wait(0, test.ipi_count_offset, ipi_counts[0],
         "CPU0 recording the queued IPI")
    wait(0, test.handler_done_offset,
         ipi_counts[0] + extra_handlers[0],
         "CPU0 resuming from its shared source and queued IPI")
    command(1, test.command_observe_ack, test.stage_acked,
            "CPU1 observing the queued IPI acknowledgement")
    assert read(1, test.sender_ack_offset) == 0x109

    command(0, test.command_timer, test.stage_timer,
            "programming CPU0's timer")
    wait(0, test.timer_count_offset, 1,
         "CPU0 handling its timer")
    extra_handlers[0] += 1
    wait(0, test.handler_done_offset,
         ipi_counts[0] + extra_handlers[0],
         "CPU0 resuming from its timer")
    send(1, 0x2, 0x10a, ipi_counts, extra_handlers)

    for cpu in range(test.cpu_count):
        wait(cpu, test.handler_count_offset,
             ipi_counts[cpu] + extra_handlers[cpu],
             f"CPU{cpu} completing every interrupt source")
        wait(cpu, test.handler_done_offset,
             read(cpu, test.handler_count_offset),
             f"CPU{cpu} resuming from every dynamic trap")
        command(cpu, test.command_finalize, test.stage_final,
                f"recording CPU{cpu} final state")

    stacks = []
    for cpu in range(test.cpu_count):
        expected_handlers = ipi_counts[cpu] + extra_handlers[cpu]

        assert read(cpu, test.handler_count_offset) == expected_handlers
        assert read(cpu, test.handler_done_offset) == expected_handlers
        assert read(cpu, test.ipi_count_offset) == ipi_counts[cpu]
        assert read(cpu, test.shared_count_offset) == (1 if cpu == 0 else 0)
        assert read(cpu, test.timer_count_offset) == 1
        assert read(cpu, test.target_id_offset) == cpu
        assert read(cpu, test.ipi_status_offset) == test.ipi_status_pending
        assert read(cpu, test.handler_rq_offset) & test.request_mask
        assert read(cpu, test.handler_rk_offset) == 0
        assert read(cpu, test.rww_offset) % 4 == 0
        assert test.main_start <= read(cpu, test.rww_offset) < test.main_end
        assert read(cpu, test.rxx_offset) & test.dynamic_trap_resume_next
        assert read(cpu, test.ryy_offset) == 0
        assert read(cpu, test.rzz_offset) == 0
        assert read(cpu, test.rbb_offset) == test.sentinels[cpu]
        stack = read(cpu, test.ro_entry_offset)
        stacks.append(stack)
        assert stack == read(cpu, test.rs_entry_offset)
        assert stack % 0x2000 == 0
        assert read(cpu, test.ro_final_offset) == stack
        assert read(cpu, test.rs_final_offset) == stack
        assert read(cpu, test.rq_final_offset) & test.request_mask == 0
        assert read(cpu, test.rk_final_offset) == test.request_mask
        assert read(cpu, test.sentinel_final_offset) == test.sentinels[cpu]
        assert read(cpu, test.progress_offset) > 0
        assert read(cpu, test.claim_offset) == test.timer_irq_base + cpu
        assert read_mmio(ipi_status_address(cpu)) == 0
        assert read_mmio(timer_status_address(cpu)) == 0

    assert len(set(stacks)) == test.cpu_count

    write(0, test.command_offset, test.command_halt)


def _run_smp_shootdown_protocol(qtest, test):
    def read_at(base, offset):
        response = _qtest_command(
            qtest, f"readq {base + offset:#x}"
        )
        return int(response[1], 0)

    def read(offset):
        return read_at(test.state_base, offset)

    def history(offset):
        return read_at(test.history_base, offset)

    def write(offset, value):
        _qtest_command(
            qtest, f"writeq {test.state_base + offset:#x} {value:#x}"
        )

    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        failure = read(test.failure_offset)
        if failure:
            detail = f"{test.name}: guest failed with reason {failure:#x}"
            if hasattr(test, "handler_rq_offset"):
                detail += f", rQ={read(test.handler_rq_offset):#x}"
            elif hasattr(test, "expected_memory"):
                fields = [read(offset) for offset in range(0, 0xe1, 8)]
                detail += f", state={fields}"
            raise AssertionError(detail)
        if read(test.complete_offset) == 1:
            break
        time.sleep(0.001)
    else:
        raise AssertionError(f"{test.name}: timed out waiting for completion")

    if hasattr(test, "expected_memory"):
        for address, expected in test.expected_memory:
            actual = read_at(0, address)
            assert actual == expected, (
                f"{test.name}: memory {address:#x}: expected "
                f"{expected:#x}, got {actual:#x}"
            )
        for address, mask, expected in test.masked_memory:
            actual = read_at(0, address)
            assert actual & mask == expected, (
                f"{test.name}: memory {address:#x} masked by {mask:#x}: "
                f"expected {expected:#x}, got {actual:#x}"
            )
        for address, minimum, maximum in test.ranged_memory:
            actual = read_at(0, address)
            assert minimum <= actual < maximum, (
                f"{test.name}: memory {address:#x}: expected in "
                f"[{minimum:#x}, {maximum:#x}), got {actual:#x}"
            )
        write(test.halt_offset, 1)
        return

    assert read(test.generation_offset) == test.generation_empty
    assert read(test.target_mask_offset) == test.target_cpu1
    assert read(test.key_offset) == test.uncached_key
    assert read(test.operation_offset) == test.operation_invalidate_only
    assert read(test.ack0_offset) == test.generation_all
    assert read(test.ack1_offset) == test.generation_empty
    assert read(test.result0_offset) == 0
    assert read(test.result1_offset) == 0
    assert read(test.post0_offset) == 0
    assert read(test.post1_offset) == 0
    assert read(test.cpu1_ready_offset) == 1
    assert read(test.pte_published_offset) == 3
    assert read(test.cpu1_observed_offset) == 2
    assert read(test.observed_value_offset) == test.value_b
    assert read(test.handler_count_offset) == 4
    assert read(test.handler_done_offset) == 4
    assert read(test.handler_rq_offset) & test.ipi_request
    assert read(test.cpu1_resumed_offset) == 5
    assert read(test.sender_ack_offset) == test.generation_empty
    assert read(test.lock_offset) == 0
    assert read(test.lock_result_offset) != 0
    assert read(test.lock_blocked_offset) == 1
    assert read(test.lock_observed_offset) == 1
    assert read(test.invalidation_count_offset) == 3
    assert read(test.duplicate_count_offset) == 1
    assert read(test.failure_offset) == 0

    assert history(test.g1_ack0_offset) == 0
    assert history(test.g1_ack1_offset) == test.generation_remote
    assert history(test.g1_result1_offset) == test.ldvts_data
    assert history(test.g1_post1_offset) == test.value_b
    assert history(test.g1_stale_offset) == test.value_a
    assert history(test.g2_ack0_offset) == test.generation_local
    assert history(test.g2_ack1_offset) == test.generation_remote
    assert history(test.g2_result0_offset) == test.ldvts_data
    assert history(test.g2_post0_offset) == test.value_a
    assert history(test.g2_remote_before_offset) == test.value_b
    assert history(test.g2_remote_after_offset) == test.value_b
    assert history(test.g2_handler_count_offset) == 1
    assert history(test.g3_ack0_offset) == test.generation_all
    assert history(test.g3_ack1_offset) == test.generation_all
    assert history(test.g3_result0_offset) == test.ldvts_data
    assert history(test.g3_result1_offset) == test.ldvts_data
    assert history(test.g3_post0_offset) == test.value_b
    assert history(test.g3_post1_offset) == test.value_b
    assert history(test.g4_ack0_offset) == test.generation_all
    assert history(test.g4_ack1_offset) == test.generation_empty
    assert history(test.g4_result1_offset) == 0
    assert history(test.duplicate_ack1_offset) == test.generation_empty
    assert history(test.final_handler_count_offset) == 4
    assert history(test.final_invalidation_count_offset) == 3
    assert history(test.final_duplicate_count_offset) == 1

    write(test.halt_offset, 1)


def _run_mttcg_qtest_test(qemu, workdir, test, protocol, socket_name,
                          *, use_loader=False, publish_initial_stack=False,
                          validate_initial_stacks=False):
    image = workdir / f"{test.name}.elf"
    qtest_path = workdir / socket_name

    image.write_bytes(test.image)
    if qtest_path.exists():
        qtest_path.unlink()

    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(qtest_path))
    listener.listen(1)
    listener.settimeout(5)
    qemu_args = (
        *test.qemu_args,
        "-S",
        "-qmp",
        "stdio",
        "-qtest",
        f"unix:{qtest_path}",
        "-qtest-log",
        "/dev/null",
    )
    if use_loader:
        command = build_smp_elf_loader_command(
            qemu, image, test.main_start, trace="int",
            log=workdir / f"{test.name}.log", qemu_args=qemu_args,
        )
    else:
        command = build_kernel_command(qemu, image, qemu_args=qemu_args)
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    connection = None
    try:
        connection, _ = listener.accept()
        connection.settimeout(5)
        qtest = connection.makefile("rwb", buffering=0)
        greeting = _read_qmp_message(process)
        if "QMP" not in greeting:
            raise AssertionError(f"invalid QMP greeting: {greeting}")
        _qmp_command(process, "qmp_capabilities")
        cpus = _qmp_command(process, "query-cpus-fast")
        thread_ids = [cpu["thread-id"] for cpu in cpus]
        if len(thread_ids) != test.cpu_count:
            raise AssertionError(
                f"{test.name}: expected {test.cpu_count} QMP CPUs, "
                f"got {len(thread_ids)}"
            )
        if len(set(thread_ids)) != test.cpu_count:
            raise AssertionError(
                f"{test.name}: vCPUs share host thread ids {thread_ids}"
            )
        if publish_initial_stack:
            stack = _qmp_command(
                process,
                "qom-get",
                {
                    "path": "/machine/cpu[0]",
                    "property": "initial-stack",
                },
            )
            _qtest_command(
                qtest, f"writeq {test.cpu_id_stack_phys:#x} {stack:#x}"
            )
        if validate_initial_stacks:
            for cpu in test.cpu_ids:
                stack = _qmp_command(
                    process,
                    "qom-get",
                    {
                        "path": f"/machine/cpu[{cpu}]",
                        "property": "initial-stack",
                    },
                )
                expected = test.initial_stack - cpu * test.initial_stack_slot_size
                if stack != expected:
                    raise AssertionError(
                        f"{test.name}: CPU {cpu} initial stack is "
                        f"{stack:#x}, expected {expected:#x}"
                    )
        _qmp_command(process, "cont")
        protocol(qtest, test)
        stdout, stderr = process.communicate(timeout=5)
        if process.returncode != 0:
            raise AssertionError(
                f"{test.name}: QEMU exited with {process.returncode}: "
                f"{stderr.decode('utf-8', errors='replace')}"
            )
    except BaseException:
        process.kill()
        stdout, stderr = process.communicate()
        if stderr:
            print(stderr.decode("utf-8", errors="replace"))
        raise
    finally:
        if connection is not None:
            connection.close()
        listener.close()
        if qtest_path.exists():
            qtest_path.unlink()


def run_mttcg_interrupt_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_smp_interrupt_protocol,
        "m45-qtest.sock", use_loader=True, publish_initial_stack=True,
    )


def run_mttcg_timer_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_smp_timer_protocol,
        "m46-qtest.sock", use_loader=True, publish_initial_stack=True,
    )


def run_mttcg_shared_interrupt_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_smp_shared_interrupt_protocol,
        "m47-qtest.sock", use_loader=True, publish_initial_stack=True,
    )


def run_mttcg_ipi_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_smp_ipi_protocol,
        "m48-qtest.sock", use_loader=True, publish_initial_stack=True,
    )


def run_mttcg_shootdown_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_smp_shootdown_protocol,
        "m49-qtest.sock", use_loader=True, publish_initial_stack=True,
    )


def run_l3_mttcg_shared_interrupt_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_l3_shared_interrupt_protocol,
        "l3-shared-interrupt-qtest.sock", use_loader=True,
        publish_initial_stack=True,
    )


def _run_l3_shared_interrupt_protocol(qtest, test):
    def read(cpu, offset):
        address = test.mailbox_base + cpu * test.mailbox_slot_size + offset
        response = _qtest_command(qtest, f"readq {address:#x}")
        return int(response[1], 0)

    def write(cpu, offset, value):
        address = test.mailbox_base + cpu * test.mailbox_slot_size + offset
        _qtest_command(qtest, f"writeq {address:#x} {value:#x}")

    def set_irq(level):
        _qtest_command(
            qtest,
            f"set_irq_in /machine/intc unnamed-gpio-in "
            f"{test.shared_irq} {level}",
        )

    def wait_until(predicate, description):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.001)
        raise AssertionError(f"{test.name}: timed out while {description}")

    for cpu in range(test.cpu_count):
        wait_until(
            lambda cpu=cpu: read(cpu, test.stage_offset) == test.stage_ready,
            f"waiting for CPU{cpu} startup",
        )

    set_irq(1)
    wait_until(
        lambda: sum(read(cpu, test.shared_count_offset)
                    for cpu in range(test.cpu_count)) == 1,
        "selecting the first shared-source owner",
    )
    first_counts = [read(cpu, test.shared_count_offset)
                    for cpu in range(test.cpu_count)]
    assert sorted(first_counts) == [0, 1]
    owner = first_counts.index(1)
    assert read(owner, test.claim_offset) == test.shared_irq
    assert read(owner, test.handler_rq_offset) & test.interrupt_request

    write(owner, test.handler_ack_offset, 1)
    wait_until(
        lambda: sum(read(cpu, test.shared_count_offset)
                    for cpu in range(test.cpu_count)) == 2,
        "retriggering the still-asserted shared source",
    )
    counts = [read(cpu, test.shared_count_offset)
              for cpu in range(test.cpu_count)]
    retrigger_owner = next(
        cpu for cpu in range(test.cpu_count)
        if counts[cpu] > first_counts[cpu]
    )
    assert read(retrigger_owner, test.claim_offset) == test.shared_irq
    set_irq(0)
    write(retrigger_owner, test.handler_ack_offset,
          counts[retrigger_owner])
    wait_until(
        lambda: sum(read(cpu, test.handler_done_offset)
                    for cpu in range(test.cpu_count)) == 2,
        "completing the retriggered shared source",
    )

    for cpu, count in enumerate(counts):
        assert read(cpu, test.handler_done_offset) == count
        if count:
            expected_stack = (
                test.initial_stack + cpu * test.initial_stack_slot_size
            )
            assert read(cpu, test.ro_entry_offset) == expected_stack
            assert read(cpu, test.rs_entry_offset) == expected_stack
    write(0, test.halt_offset, 1)


def _run_l3_cpu_isolation_protocol(qtest, test):
    def read(cpu, offset):
        return int.from_bytes(
            _qtest_read(qtest, test.mailbox(cpu, offset), 8), "big"
        )

    def write(address, value):
        _qtest_write(qtest, address, value.to_bytes(8, "big"))

    def read_phys(address):
        return int.from_bytes(_qtest_read(qtest, address, 8), "big")

    def wait(cpu, offset, expected, description):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            actual = read(cpu, offset)
            if actual == expected:
                return
            time.sleep(0.001)
        raise AssertionError(
            f"{test.name}: CPU {cpu} timed out while {description}; "
            f"expected {expected:#x}, got {actual:#x}"
        )

    for cpu in test.cpu_ids:
        wait(cpu, test.ready_offset, 1, "waiting for startup")

    for cpu in test.cpu_ids:
        irq = 16 + cpu
        word = irq // 64
        bit = 1 << (irq % 64)
        write(test.intc_enable(cpu, word), bit)
        enable = read_phys(test.intc_enable(cpu, word))
        assert enable & bit, (
            f"{test.name}: CPU {cpu} INTC enable word {word} is "
            f"{enable:#x}"
        )
        write(test.timer_context(cpu, 0x00), 0)
        write(test.timer_context(cpu, 0x08), 0x3)
    for cpu in test.cpu_ids:
        wait(cpu, test.timer_count_offset, 1, "handling its timer")
        rq = read(cpu, test.timer_rq_offset)
        assert rq & test.timer_request
        claim = read(cpu, test.timer_claim_offset)
        assert claim == 16 + cpu, (
            f"{test.name}: CPU {cpu} claimed {claim}, expected {16 + cpu}"
        )
        expected_stack = (
            test.initial_stack - cpu * test.initial_stack_slot_size
        )
        assert read(cpu, test.handler_ro_offset) == expected_stack
        assert read(cpu, test.handler_rs_offset) == expected_stack

    targets = sum(1 << cpu for cpu in test.cpu_ids)
    write(test.ipi_base + 0x8, targets)
    for cpu in test.cpu_ids:
        wait(cpu, test.ipi_count_offset, 1, "handling its IPI")
        rq = read(cpu, test.ipi_rq_offset)
        assert rq & test.ipi_request

    assert read(0, test.handler_ro_offset) != read(
        63, test.handler_ro_offset
    )
    write(test.mailbox(0, test.halt_offset), 1)


def run_l3_mttcg_cpu_isolation_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_l3_cpu_isolation_protocol,
        "l3-cpu-isolation-qtest.sock", validate_initial_stacks=True,
    )
