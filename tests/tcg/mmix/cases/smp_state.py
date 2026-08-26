#!/usr/bin/env python3
#
# MMIX concurrent CPU-local state test cases
#
# SPDX-License-Identifier: GPL-2.0-or-later

from .common import *
from .smp import (
    MMIXSMPResetTest,
    MMIXSMPTest,
    SMPProgram,
    SMPProgramImage,
    SMP_ENTRY,
    SMP_WAIT_LIMIT,
    TCG_THREAD_MULTI,
    smp_elf_image,
    smp_emit_unconditional_branch,
    smp_emit_wait_equal,
    smp_load,
    smp_store,
    smp_sync,
)


SMP_STATE_BASE = 0x00200200
SMP_STATE_SLOT_SIZE = 0x80
SMP_STATE_LOCAL = 0x00
SMP_STATE_GLOBAL = 0x08
SMP_STATE_RM = 0x10
SMP_STATE_RA = 0x18
SMP_STATE_RO = 0x20
SMP_STATE_RS = 0x28
SMP_STATE_TRAPS = 0x30
SMP_STATE_RL = 0x38
SMP_STATE_READY = 0x40
SMP_STATE_HANDLER = 0x1800
SMP_STATE_CALL_DEPTH = 40


def emit_state_slot_address(program, dst, cpu_id, scratch):
    program.emit(
        *set_octa(dst, SMP_STATE_BASE),
        insn(SLUI, scratch, cpu_id, 7),
        insn(ADDU, dst, dst, scratch),
    )


def emit_deep_register_stack_calls(program):
    program.emit_branch(PUSHJ, R31, "stack_calls")
    program.emit_branch(BZ, R254, "stack_returned")
    program.mark("stack_calls")
    for level in range(SMP_STATE_CALL_DEPTH):
        program.emit(
            insn(GET, R130 + level, 0, SR_J),
            wyde(SETL, R31, level + 1),
            branch(PUSHJ, R31, 4),
            insn(ADDI, R0, R31, 1),
            insn(PUT, SR_J, 0, R130 + level),
            insn(POP, 1, 0, 0),
        )
    program.emit(
        wyde(SETL, R0, 1),
        insn(POP, 1, 0, 0),
    )
    program.mark("stack_returned")


def cpu_local_state_program():
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        wyde(SETL, R100, 0x100),
        insn(ADDU, R100, R100, R32),
        wyde(SETL, R110, 0x200),
        insn(ADDU, R110, R110, R32),
        insn(PUT, SR_M, 0, R110),
        *set_octa(R123, (1 << 63) | SMP_STATE_HANDLER),
        insn(PUT, SR_TT, 0, R123),
        *set_octa(R124, RQ_PROGRAM_B),
        insn(PUT, SR_K, 0, R124),
    )
    emit_deep_register_stack_calls(program)
    program.emit(
        insn(GET, R111, 0, SR_L),
        insn(GET, R112, 0, SR_O),
        insn(GET, R113, 0, SR_S),
    )

    program.emit_branch(BNZ, R32, "cpu1_arithmetic")
    program.emit(
        wyde(SETL, R70, 1),
        wyde(SETL, R71, 2),
        insn(ADDU, R72, R70, R71),
    )
    smp_emit_unconditional_branch(program, "arithmetic_done")
    program.mark("cpu1_arithmetic")
    program.emit(
        *set_octa(R70, 0x7fffffffffffffff),
        wyde(SETL, R71, 1),
        insn(ADD, R72, R70, R71),
    )
    program.mark("arithmetic_done")
    program.emit(insn(GET, R73, 0, SR_A))

    program.emit_branch(BZ, R32, "trap_done")
    program.emit(insn(GET, R20, 3, SR_M))
    program.mark("trap_done")
    program.emit(
        insn(GET, R114, 0, SR_M),
        insn(ADDI, R0, R32, 0x10),
    )
    emit_state_slot_address(program, R40, R32, R41)
    program.emit(
        smp_store(R0, R40, SMP_STATE_LOCAL),
        smp_store(R100, R40, SMP_STATE_GLOBAL),
        smp_store(R114, R40, SMP_STATE_RM),
        smp_store(R73, R40, SMP_STATE_RA),
        smp_store(R112, R40, SMP_STATE_RO),
        smp_store(R113, R40, SMP_STATE_RS),
        smp_store(R120, R40, SMP_STATE_TRAPS),
        smp_store(R111, R40, SMP_STATE_RL),
        wyde(SETL, R42, 1),
        smp_sync(1),
        smp_store(R42, R40, SMP_STATE_READY),
    )
    program.emit_branch(BNZ, R32, "secondary_idle")

    program.emit(*set_octa(R49, SMP_STATE_BASE + SMP_STATE_SLOT_SIZE))
    smp_emit_wait_equal(
        program,
        address=R49,
        field=SMP_STATE_READY,
        expected=R42,
        value=R43,
        compare=R44,
        counter=R45,
        label="state_peer_ready",
        timeout_label="failure",
    )
    program.emit(
        smp_sync(2),
        smp_load(R150, R49, SMP_STATE_LOCAL),
        smp_load(R151, R49, SMP_STATE_GLOBAL),
        smp_load(R152, R49, SMP_STATE_RM),
        smp_load(R153, R49, SMP_STATE_RA),
        smp_load(R154, R49, SMP_STATE_RO),
        smp_load(R155, R49, SMP_STATE_RS),
        smp_load(R156, R49, SMP_STATE_TRAPS),
        smp_load(R157, R49, SMP_STATE_RL),
        wyde(SETL, R90, 1),
    )
    program.mark("success")
    program.emit(halt())

    program.mark("secondary_idle")
    smp_emit_unconditional_branch(program, "secondary_idle")
    program.mark("failure")
    program.emit(wyde(SETL, R90, 0xdead))
    program.mark("failure_halt")
    program.emit(halt())

    handler = b"".join([
        insn(ADDUI, R120, R120, 1),
        insn(GET, R121, 0, SR_K),
        insn(ADDU, R255, R121, R254),
        insn(RESUME, 0, 0, 1),
    ])
    return SMPProgramImage(
        code=smp_elf_image(
            program.build(),
            (SMP_STATE_HANDLER, handler),
        ),
        success_pc=program.address("success"),
        timeout_pc=program.address("failure_halt"),
        success_regs={
            R0: 0x10,
            R32: 0,
            R100: 0x100,
            R111: 32,
            R112: INITIAL_STACK,
            R113: INITIAL_STACK,
            R114: 0x200,
            R120: 0,
            R73: 0,
            R150: 0x11,
            R151: 0x101,
            R152: 0x201,
            R153: RA_EVENT_V,
            R154: INITIAL_STACK + MMIX_VIRT_INITIAL_STACK_SLOT_SIZE,
            R155: INITIAL_STACK + MMIX_VIRT_INITIAL_STACK_SLOT_SIZE,
            R156: 1,
            R157: 32,
            R90: 1,
        },
    )


def concurrent_reset_program():
    program = SMPProgram()

    program.emit(
        insn(ADDI, R32, R0, 0),
        wyde(SETL, R254, 0),
        *set_octa(R200, SMP_STATE_BASE + 0x100),
        insn(SLUI, R201, R32, 3),
        insn(ADDU, R200, R200, R201),
        smp_load(R202, R200, 0),
    )
    program.emit_branch(BNZ, R202, "reset_idle")
    program.emit(
        wyde(SETL, R202, 1),
        smp_store(R202, R200, 0),
        wyde(SETL, R100, 0x500),
        insn(ADDU, R100, R100, R32),
        wyde(SETL, R110, 0x600),
        insn(ADDU, R110, R110, R32),
        insn(PUT, SR_M, 0, R110),
    )
    emit_deep_register_stack_calls(program)
    program.mark("idle")
    smp_emit_unconditional_branch(program, "idle")
    program.mark("reset_idle")
    smp_emit_unconditional_branch(program, "reset_idle")

    stack0 = INITIAL_STACK
    stack1 = INITIAL_STACK + MMIX_VIRT_INITIAL_STACK_SLOT_SIZE
    reset_regs = (
        {
            "pc=0x": program.address("reset_idle"),
            "rO=0x": stack0,
            "rS=0x": stack0,
            "r0  =0x": 0,
            "r1  =0x": MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0],
            "r100=0x": 0,
        },
        {
            "pc=0x": program.address("reset_idle"),
            "rO=0x": stack1,
            "rS=0x": stack1,
            "r0  =0x": 1,
            "r1  =0x": MMIX_VIRT_MEMMAP[MMIX_VIRT_BOOTINFO][0],
            "r100=0x": 0,
        },
    )
    return MMIXSMPResetTest(
        name="smp-multi-thread-cold-reset",
        image=smp_elf_image(program.build()),
        idle_pc=program.address("idle"),
        reset_idle_pc=program.address("reset_idle"),
        reset_regs=reset_regs,
    )


SMP_CPU_LOCAL_STATE = cpu_local_state_program()

SMP_STATE_TESTS = [
    MMIXSMPTest(
        "smp-multi-thread-cpu-local-state",
        SMP_CPU_LOCAL_STATE.code,
        pc=SMP_CPU_LOCAL_STATE.success_pc,
        regs=SMP_CPU_LOCAL_STATE.success_regs,
        thread_mode=TCG_THREAD_MULTI,
    ),
]

SMP_RESET_TESTS = [concurrent_reset_program()]
