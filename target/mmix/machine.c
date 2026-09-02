/*
 * MMIX CPU state serialization for migration and snapshots
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "exec/cputlb.h"
#include "machine.h"

static bool mmix_cpu_pre_save(void *opaque, Error **errp)
{
    MMIXCPU *cpu = opaque;
    CPUMMIXState *env = &cpu->env;
    unsigned int handle;

    if (cpu->trap_restart_stack->len != 0) {
        error_setg(errp,
                   "MMIX nested trap restart state cannot be migrated");
        return false;
    }
    if (env->semihosting_bounce_active ||
        env->semihosting_pending_open_handle ||
        env->semihosting_pending_io_length) {
        error_setg(errp,
                   "MMIX semihosting operation is active during migration");
        return false;
    }
    for (handle = 0; handle < MMIX_SEMIHOSTING_HANDLES; handle++) {
        if (env->semihosting_file_guestfds[handle]) {
            error_setg(errp,
                       "MMIX semihosting file handle %u is open during "
                       "migration", handle);
            return false;
        }
    }
    return true;
}

static int mmix_cpu_post_load(void *opaque, int version_id)
{
    MMIXCPU *cpu = opaque;
    CPUMMIXState *env = &cpu->env;

    if (env->lring_size != MMIX_LOCAL_REGS ||
        env->lring_mask != MMIX_LOCAL_REGS - 1 ||
        env->stack_access.kind > MMIX_STACK_ACCESS_UNSAVE ||
        env->save_restart.phase > MMIX_SAVE_RESTART_PACKED) {
        return -EINVAL;
    }

    mmix_cpu_flush_translation_caches(env);
    tlb_flush(CPU(cpu));
    mmix_cpu_update_interrupt(env);
    return 0;
}

const VMStateDescription vmstate_mmix_cpu = {
    .name = "cpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save_errp = mmix_cpu_pre_save,
    .post_load = mmix_cpu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(env.regs, MMIXCPU, MMIX_REGS),
        VMSTATE_UINT64_ARRAY(env.sregs, MMIXCPU, MMIX_SREGS),
        VMSTATE_UINT64(env.pc, MMIXCPU),
        VMSTATE_UINT64(env.npc, MMIXCPU),
        VMSTATE_BOOL(env.test_exit_seen, MMIXCPU),
        VMSTATE_UINT64_ARRAY(env.local_regs, MMIXCPU, MMIX_LOCAL_REGS),
        VMSTATE_UINT32(env.lring_size, MMIXCPU),
        VMSTATE_UINT32(env.lring_mask, MMIXCPU),
        VMSTATE_SINGLE(env.stack_access.kind, MMIXCPU, 0,
                       vmstate_info_int32, MMIXStackAccessKind),
        VMSTATE_UINT32(env.stack_access.ring_index, MMIXCPU),
        VMSTATE_UINT64(env.stack_access.address, MMIXCPU),
        VMSTATE_UINT64(env.stack_access.value, MMIXCPU),
        VMSTATE_BOOL(env.stack_access.completed, MMIXCPU),
        VMSTATE_SINGLE(env.save_restart.phase, MMIXCPU, 0,
                       vmstate_info_int32, MMIXSaveRestartPhase),
        VMSTATE_UINT32(env.save_restart.x, MMIXCPU),
        VMSTATE_UINT32(env.save_restart.rg, MMIXCPU),
        VMSTATE_UINT32(env.save_restart.old_rl, MMIXCPU),
        VMSTATE_UINT32(env.save_restart.index, MMIXCPU),
        VMSTATE_UINT64_ARRAY(env.save_restart.regs, MMIXCPU, MMIX_REGS),
        VMSTATE_UINT64_ARRAY(env.save_restart.sregs, MMIXCPU, MMIX_SREGS),
        VMSTATE_UINT64(env.save_restart.packed, MMIXCPU),
        VMSTATE_UINT64(env.unsave_restart_address, MMIXCPU),
        VMSTATE_BOOL(env.unsave_restart_active, MMIXCPU),
        VMSTATE_UINT32(env.arithmetic_trip_event, MMIXCPU),
        VMSTATE_UINT64(env.program_exception_causes, MMIXCPU),
        VMSTATE_UINT32(env.program_exception_insn, MMIXCPU),
        VMSTATE_UINT32(env.rule_break_insn, MMIXCPU),
        VMSTATE_UINT64(env.rule_break_y, MMIXCPU),
        VMSTATE_UINT64(env.rule_break_z, MMIXCPU),
        VMSTATE_UINT32(env.data_access_insn, MMIXCPU),
        VMSTATE_UINT64(env.data_access.address, MMIXCPU),
        VMSTATE_UINT64(env.data_access.value, MMIXCPU),
        VMSTATE_BOOL(env.program_exception_data_access, MMIXCPU),
        VMSTATE_UINT64(env.insn_replay.insn_pc, MMIXCPU),
        VMSTATE_UINT64(env.insn_replay.continuation, MMIXCPU),
        VMSTATE_UINT64(env.insn_replay.y, MMIXCPU),
        VMSTATE_UINT64(env.insn_replay.z, MMIXCPU),
        VMSTATE_UINT32(env.insn_replay.insn, MMIXCPU),
        VMSTATE_UINT64(env.insn_replay.trap_restart_sequence, MMIXCPU),
        VMSTATE_BOOL(env.insn_replay.substitute_operands, MMIXCPU),
        VMSTATE_BOOL(env.insn_replay.active, MMIXCPU),
        VMSTATE_UINT32(env.forced_translation_insn, MMIXCPU),
        VMSTATE_UINT64(env.forced_translation_address, MMIXCPU),
        VMSTATE_UINT64(env.forced_translation_where, MMIXCPU),
        VMSTATE_UINT8(env.forced_translation_access, MMIXCPU),
        VMSTATE_UINT64(env.rq_new_bits, MMIXCPU),
        VMSTATE_BOOL(env.interrupt_controller_level, MMIXCPU),
        VMSTATE_BOOL(env.ipi_level, MMIXCPU),
        VMSTATE_BOOL(env.flat_translation, MMIXCPU),
        VMSTATE_END_OF_LIST()
    },
};
