#!/usr/bin/env python3
#
# MMIX softmmu test execution helpers
#
# SPDX-License-Identifier: GPL-2.0-or-later

import json
import re
import select
import socket
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
from lib.mmo import MMIX_MMO_ESCAPE, MMIX_MMO_LOP_PRE
from lib.qemu import (
    QEMU_SEMIHOSTING_ARGS,
    QEMU_SEMIHOSTING_STDIN_ARGS,
    build_kernel_command,
    read_log,
    run_kernel,
)


QEMU_SEMIHOSTING_CONSOLE_CHARDEV = "mmix-semihosting-console"


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


def run_one(qemu, workdir, test, *, qemu_args=(), stdin_data=None):
    image = workdir / f"{test.name}.bin"
    log = workdir / f"{test.name}.log"

    image.write_bytes(test.program)
    if log.exists():
        log.unlink()

    completed = run_kernel(qemu, image, trace="int", log=log,
                           qemu_args=qemu_args, check=False, timeout=10,
                           stdin_data=_test_stdin_data(test, stdin_data))

    result = read_log(log)
    assert_exit_pc(test.name, result, test.pc)
    assert_exit_status(test.name, completed, test.exit_status)
    assert_regs(test.name, result, test.regs)


def run_semihosting_one(qemu, workdir, test):
    qemu_args = _semihosting_config_args(test)
    run_one(qemu, workdir, test, qemu_args=qemu_args)


def run_semihosting_stdin_one(qemu, workdir, test):
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

    try:
        run_kernel(qemu, image, trace="unimp,int", log=log,
                   qemu_args=qemu_args,
                   check=False, timeout=2)
    except subprocess.TimeoutExpired:
        pass

    if not log.exists():
        raise AssertionError(f"{test.name}: missing log")

    log_text = log.read_text(encoding="utf-8")
    assert_log_patterns(test.name, log_text, test.patterns, test.absent)


def run_semihosting_expected_failure(qemu, workdir, test):
    run_expected_failure(qemu, workdir, test,
                         qemu_args=QEMU_SEMIHOSTING_ARGS)


def run_process_failure(qemu, workdir, test):
    image = workdir / f"{test.name}.bin"

    image.write_bytes(test.program)

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
        expected_stack = test.initial_stack + cpu * test.initial_stack_slot_size
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
        assert snapshot["rO"] == expected_stack
        assert snapshot["rS"] == expected_stack
        assert read(cpu, test.ro_final_offset) == expected_stack
        assert read(cpu, test.rs_final_offset) == expected_stack
        assert read(cpu, test.rq_final_offset) & test.interrupt_request == 0
        assert read(cpu, test.rk_final_offset) == test.interrupt_request
        assert read(cpu, test.sentinel_final_offset) == test.sentinels[cpu]

    assert snapshots[0] != snapshots[1]
    write(0, test.command_offset, test.command_halt)


def run_mttcg_interrupt_test(qemu, workdir, test):
    image = workdir / f"{test.name}.elf"
    qtest_path = workdir / "m45-qtest.sock"

    image.write_bytes(test.image)
    if qtest_path.exists():
        qtest_path.unlink()

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
        _run_smp_interrupt_protocol(qtest, test)
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
