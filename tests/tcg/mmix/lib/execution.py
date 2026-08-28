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
    build_smp_elf_loader_command,
    read_log,
    run_kernel,
    run_loader,
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

        assert read(cpu, test.handler_count_offset) == 2
        assert read(cpu, test.handler_done_offset) == 2
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
    wait(1, test.handler_done_offset, 2,
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
        assert snapshot["rO"] == expected_stack
        assert snapshot["rS"] == expected_stack
        assert read(cpu, test.ro_final_offset) == expected_stack
        assert read(cpu, test.rs_final_offset) == expected_stack
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

    for cpu in range(test.cpu_count):
        expected_stack = test.initial_stack + cpu * test.initial_stack_slot_size
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
        assert read(cpu, test.ro_entry_offset) == expected_stack
        assert read(cpu, test.rs_entry_offset) == expected_stack
        assert read(cpu, test.ro_final_offset) == expected_stack
        assert read(cpu, test.rs_final_offset) == expected_stack
        assert read(cpu, test.rq_final_offset) & test.request_mask == 0
        assert read(cpu, test.rk_final_offset) == test.request_mask
        assert read(cpu, test.sentinel_final_offset) == test.sentinels[cpu]
        assert read(cpu, test.progress_offset) > 0
        assert read(cpu, test.claim_offset) == test.timer_irq_base + cpu
        assert read_mmio(ipi_status_address(cpu)) == 0
        assert read_mmio(timer_status_address(cpu)) == 0

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
                          *, use_loader=False):
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
        "m45-qtest.sock",
    )


def run_mttcg_timer_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_smp_timer_protocol,
        "m46-qtest.sock",
    )


def run_mttcg_shared_interrupt_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_smp_shared_interrupt_protocol,
        "m47-qtest.sock",
    )


def run_mttcg_ipi_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_smp_ipi_protocol,
        "m48-qtest.sock",
    )


def run_mttcg_shootdown_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_smp_shootdown_protocol,
        "m49-qtest.sock",
    )


def run_l3_mttcg_shared_interrupt_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_l3_shared_interrupt_protocol,
        "l3-shared-interrupt-qtest.sock", use_loader=True,
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
        response = _qtest_command(
            qtest, f"readq {test.mailbox(cpu, offset):#x}"
        )
        return int(response[1], 0)

    def write(address, value):
        _qtest_command(qtest, f"writeq {address:#x} {value:#x}")

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

    targets = sum(1 << cpu for cpu in test.cpu_ids)
    write(test.ipi_base + 0x8, targets)
    for cpu in test.cpu_ids:
        wait(cpu, test.ipi_count_offset, 1, "handling its IPI")
        rq = read(cpu, test.ipi_rq_offset)
        assert rq & test.ipi_request
        assert rq & test.timer_request == 0

    for cpu in test.cpu_ids:
        write(test.timer_context(cpu, 0x00), 0)
        write(test.timer_context(cpu, 0x08), 0x3)
    for cpu in test.cpu_ids:
        wait(cpu, test.timer_count_offset, 1, "handling its timer")
        rq = read(cpu, test.timer_rq_offset)
        assert rq & test.timer_request
        assert rq & test.ipi_request == 0
        assert read(cpu, test.timer_claim_offset) == 16 + cpu
        expected_stack = (
            test.initial_stack + cpu * test.initial_stack_slot_size
        )
        assert read(cpu, test.handler_ro_offset) == expected_stack
        assert read(cpu, test.handler_rs_offset) == expected_stack

    assert read(0, test.handler_ro_offset) != read(
        63, test.handler_ro_offset
    )
    write(test.mailbox(0, test.halt_offset), 1)


def run_l3_mttcg_cpu_isolation_test(qemu, workdir, test):
    _run_mttcg_qtest_test(
        qemu, workdir, test, _run_l3_cpu_isolation_protocol,
        "l3-cpu-isolation-qtest.sock", use_loader=True,
    )
