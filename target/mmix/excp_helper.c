/*
 * QEMU MMIX exception helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "mmix-helper.h"
#include "semihosting.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/helper-proto.h"
#include "exec/log.h"
#include "system/runstate.h"

static QemuMutex mmix_trap_restart_mutex;
static gsize mmix_trap_restart_mutex_initialized;
static uint64_t mmix_trap_restart_sequence;

void mmix_trap_restart_lock(void)
{
    if (g_once_init_enter(&mmix_trap_restart_mutex_initialized)) {
        qemu_mutex_init(&mmix_trap_restart_mutex);
        g_once_init_leave(&mmix_trap_restart_mutex_initialized, 1);
    }
    qemu_mutex_lock(&mmix_trap_restart_mutex);
}

void mmix_trap_restart_unlock(void)
{
    qemu_mutex_unlock(&mmix_trap_restart_mutex);
}

static uint32_t mmix_select_arithmetic_event(uint32_t events)
{
    static const uint32_t priority[] = {
        MMIX_RA_EVENT_D,
        MMIX_RA_EVENT_V,
        MMIX_RA_EVENT_W,
        MMIX_RA_EVENT_I,
        MMIX_RA_EVENT_O,
        MMIX_RA_EVENT_U,
        MMIX_RA_EVENT_Z,
        MMIX_RA_EVENT_X,
    };
    unsigned i;

    events &= MMIX_RA_EVENT_MASK;
    for (i = 0; i < ARRAY_SIZE(priority); i++) {
        if (events & priority[i]) {
            return priority[i];
        }
    }
    return 0;
}

static hwaddr mmix_arithmetic_trip_handler(uint32_t event)
{
    switch (event) {
    case MMIX_RA_EVENT_D:
        return 16;
    case MMIX_RA_EVENT_V:
        return 32;
    case MMIX_RA_EVENT_W:
        return 48;
    case MMIX_RA_EVENT_I:
        return 64;
    case MMIX_RA_EVENT_O:
        return 80;
    case MMIX_RA_EVENT_U:
        return 96;
    case MMIX_RA_EVENT_Z:
        return 112;
    case MMIX_RA_EVENT_X:
        return 128;
    default:
        g_assert_not_reached();
    }
}

static void mmix_trap_restart_push(CPUMMIXState *env,
                                   bool forced_translation,
                                   uint64_t where, uint64_t exec,
                                   uint64_t y)
{
    MMIXCPU *cpu = env_archcpu(env);
    MMIXTrapRestartState restart = {
        .stack_access = env->stack_access,
        .interrupted_replay = env->insn_replay,
        .trap_where = where,
        .trap_exec = exec,
        .trap_y = y,
        .trap_rv = env->sregs[MMIX_SREG_RV],
        .forced_translation = forced_translation,
    };

    if (!env->insn_replay.active &&
        env->save_restart.phase == MMIX_SAVE_RESTART_NONE &&
        !env->unsave_restart_active &&
        env->stack_access.kind == MMIX_STACK_ACCESS_NONE) {
        return;
    }

    /* An interrupted SAVE/UNSAVE gives its handler a fresh register stack. */
    if (env->save_restart.phase != MMIX_SAVE_RESTART_NONE ||
        env->unsave_restart_active) {
        MMIXSaveUnsaveTrapState *save_unsave = g_new0(
            MMIXSaveUnsaveTrapState, 1);

        save_unsave->save_restart = env->save_restart;
        save_unsave->unsave_restart_address =
            env->unsave_restart_address;
        save_unsave->unsave_restart_active = env->unsave_restart_active;
        save_unsave->ro = env->sregs[MMIX_SREG_RO];
        save_unsave->rs = env->sregs[MMIX_SREG_RS];
        save_unsave->rl = env->sregs[MMIX_SREG_RL];
        memcpy(save_unsave->local_regs, env->local_regs,
               sizeof(save_unsave->local_regs));
        restart.save_unsave = save_unsave;
    }

    if (forced_translation) {
        restart.forced_translation_access = env->forced_translation_access;
    }
    /* A trap accepted before replay executes suspends it at this depth. */
    mmix_trap_restart_lock();
    g_assert(mmix_trap_restart_sequence != UINT64_MAX);
    restart.sequence = ++mmix_trap_restart_sequence;
    g_array_append_val(cpu->trap_restart_stack, restart);
    mmix_trap_restart_unlock();
    memset(&env->stack_access, 0, sizeof(env->stack_access));
    memset(&env->insn_replay, 0, sizeof(env->insn_replay));
    memset(&env->save_restart, 0, sizeof(env->save_restart));
    env->unsave_restart_address = 0;
    env->unsave_restart_active = false;
    if (restart.save_unsave != NULL) {
        env->sregs[MMIX_SREG_RO] = env->sregs[MMIX_SREG_RS];
        env->sregs[MMIX_SREG_RL] = 0;
    }
}

static void mmix_trap_restart_restore_register_stack(
    CPUMMIXState *env, const MMIXTrapRestartState *restart)
{
    const MMIXSaveUnsaveTrapState *save_unsave = restart->save_unsave;

    if (save_unsave == NULL) {
        return;
    }

    env->sregs[MMIX_SREG_RO] = save_unsave->ro;
    env->sregs[MMIX_SREG_RS] = save_unsave->rs;
    env->sregs[MMIX_SREG_RL] = save_unsave->rl;
    memcpy(env->local_regs, save_unsave->local_regs,
           sizeof(env->local_regs));
}

static void mmix_trap_restart_restore_helper_state(
    CPUMMIXState *env, const MMIXTrapRestartState *restart)
{
    const MMIXSaveUnsaveTrapState *save_unsave = restart->save_unsave;

    if (save_unsave == NULL) {
        return;
    }

    env->save_restart = save_unsave->save_restart;
    env->unsave_restart_address = save_unsave->unsave_restart_address;
    env->unsave_restart_active = save_unsave->unsave_restart_active;
}

static void mmix_trap_restart_remove(CPUMMIXState *env, uint64_t sequence)
{
    MMIXTrapRestartState removed = { 0 };
    CPUState *cs;
    bool found = false;

    mmix_trap_restart_lock();
    CPU_FOREACH(cs) {
        MMIXCPU *cpu;
        GArray *stack;
        unsigned int i;

        if (!object_dynamic_cast(OBJECT(cs), TYPE_MMIX_CPU)) {
            continue;
        }
        cpu = MMIX_CPU(cs);
        stack = cpu->trap_restart_stack;
        for (i = 0; i < stack->len; i++) {
            MMIXTrapRestartState *restart = &g_array_index(
                stack, MMIXTrapRestartState, i);

            if (restart->sequence != sequence) {
                continue;
            }
            removed = *restart;
            restart->save_unsave = NULL;
            g_array_remove_index(stack, i);
            found = true;
            break;
        }
        if (found) {
            break;
        }
    }
    mmix_trap_restart_unlock();
    g_assert(found);
    env->insn_replay = removed.interrupted_replay;
    g_free(removed.save_unsave);
}

void helper_mmix_consume_insn_replay(CPUMMIXState *env)
{
    uint64_t sequence = env->insn_replay.trap_restart_sequence;

    g_assert(env->insn_replay.active);
    env->insn_replay.active = false;
    if (sequence != 0) {
        /* Restore any replay suspended by a nested trap at consumption. */
        mmix_trap_restart_remove(env, sequence);
    }
}

static void mmix_commit_replay_before_synchronous_trap(CPUMMIXState *env)
{
    /* Forced-translation traps instead suspend replay for an exact retry. */
    if (env->insn_replay.active) {
        helper_mmix_consume_insn_replay(env);
    }
}

static void mmix_cpu_enter_trap(CPUState *cs, hwaddr handler, uint64_t where,
                                uint64_t exec, uint64_t y, uint64_t z)
{
    CPUMMIXState *env = cpu_env(cs);

    mmix_trap_restart_push(env,
                           cs->exception_index == EXCP_MMIX_FORCED_TRANSLATION,
                           where, exec, y);
    env->sregs[MMIX_SREG_RBB] = mmix_cpu_read_reg(env, 255);
    env->sregs[MMIX_SREG_RWW] = where;
    env->sregs[MMIX_SREG_RXX] = exec;
    env->sregs[MMIX_SREG_RYY] = y;
    env->sregs[MMIX_SREG_RZZ] = z;
    mmix_cpu_put_rk(env, 0);
    mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RJ]);
    env->pc = handler;
    env->npc = handler + 4;
    cs->exception_index = -1;
}

static MMUAccessType mmix_resume_translation_access(uint32_t insn)
{
    if (insn == MMIX_SWYM_INSN) {
        return MMU_INST_FETCH;
    }

    return MMU_DATA_LOAD;
}

static bool mmix_trap_restart_matches(
    const MMIXTrapRestartState *restart, uint64_t where, uint64_t exec,
    uint64_t y)
{
    return restart->trap_where == where && restart->trap_exec == exec &&
           restart->trap_y == y;
}

static bool mmix_trap_restart_select(CPUMMIXState *env, uint64_t where,
                                     uint64_t exec, uint64_t y,
                                     bool forced_translation,
                                     MMUAccessType access_type, bool remove,
                                     MMIXTrapRestartState *result)
{
    CPUState *cs;
    uint64_t rv = env->sregs[MMIX_SREG_RV];
    MMIXCPU *selected_cpu = NULL;
    unsigned int selected_index = 0;
    uint64_t selected_sequence = 0;

    mmix_trap_restart_lock();
    CPU_FOREACH(cs) {
        MMIXCPU *cpu;
        GArray *stack;
        unsigned int i;

        if (!object_dynamic_cast(OBJECT(cs), TYPE_MMIX_CPU)) {
            continue;
        }
        cpu = MMIX_CPU(cs);
        stack = cpu->trap_restart_stack;
        for (i = stack->len; i > 0; i--) {
            MMIXTrapRestartState *restart = &g_array_index(
                stack, MMIXTrapRestartState, i - 1);

            if (!mmix_trap_restart_matches(restart, where, exec, y) ||
                restart->forced_translation != forced_translation ||
                (forced_translation && restart->trap_rv != rv) ||
                (forced_translation &&
                 (access_type == MMU_INST_FETCH) !=
                 (restart->forced_translation_access == MMU_INST_FETCH)) ||
                restart->sequence <= selected_sequence) {
                continue;
            }
            selected_cpu = cpu;
            selected_index = i - 1;
            selected_sequence = restart->sequence;
        }
    }
    if (selected_cpu != NULL) {
        MMIXTrapRestartState *restart = &g_array_index(
            selected_cpu->trap_restart_stack, MMIXTrapRestartState,
            selected_index);

        *result = *restart;
        if (remove) {
            restart->save_unsave = NULL;
            g_array_remove_index(selected_cpu->trap_restart_stack,
                                 selected_index);
        }
    }
    mmix_trap_restart_unlock();
    return selected_cpu != NULL;
}

static bool mmix_trap_restart_find(CPUMMIXState *env, uint64_t where,
                                   uint64_t exec, uint64_t y,
                                   MMUAccessType access_type,
                                   MMIXTrapRestartState *result)
{
    return mmix_trap_restart_select(env, where, exec, y, false,
                                    access_type, false, result);
}

static bool mmix_trap_restart_find_current(CPUMMIXState *env,
                                           uint64_t where,
                                           MMIXTrapRestartState *result)
{
    GArray *stack = env_archcpu(env)->trap_restart_stack;
    unsigned int i;
    bool found = false;

    mmix_trap_restart_lock();
    for (i = stack->len; i > 0; i--) {
        MMIXTrapRestartState *restart = &g_array_index(
            stack, MMIXTrapRestartState, i - 1);

        if (restart->forced_translation || restart->trap_where != where) {
            continue;
        }
        *result = *restart;
        found = true;
        break;
    }
    mmix_trap_restart_unlock();
    return found;
}

static bool mmix_trap_restart_take(CPUMMIXState *env, uint64_t where,
                                   uint64_t exec, uint64_t y,
                                   bool forced_translation,
                                   MMUAccessType access_type,
                                   MMIXTrapRestartState *result)
{
    return mmix_trap_restart_select(env, where, exec, y,
                                    forced_translation, access_type, true,
                                    result);
}

static void mmix_trap_restart_finish(CPUMMIXState *env,
                                     MMIXTrapRestartState *restart,
                                     bool detached)
{
    if (!detached) {
        mmix_trap_restart_remove(env, restart->sequence);
        return;
    }

    env->insn_replay = restart->interrupted_replay;
    g_free(restart->save_unsave);
    restart->save_unsave = NULL;
}

static void mmix_raise_arithmetic_trip(CPUMMIXState *env, uint32_t event,
                                       uint32_t insn, uint64_t y, uint64_t z)
{
    CPUState *cs = env_cpu(env);

    env->arithmetic_trip_event = event;
    env->sregs[MMIX_SREG_RW] = env->npc;
    env->sregs[MMIX_SREG_RX] = 0x8000000000000000ULL | insn;
    env->sregs[MMIX_SREG_RY] = y;
    env->sregs[MMIX_SREG_RZ] = z;
    cs->exception_index = EXCP_MMIX_ARITHMETIC_TRIP;
    cpu_loop_exit(cs);
}

void mmix_update_ra_events(CPUMMIXState *env, uint32_t events,
                                  uint32_t insn, uint64_t y, uint64_t z)
{
    uint32_t enables;
    uint32_t disabled_events;
    uint32_t enabled_events;
    uint32_t selected_event;

    events &= MMIX_RA_EVENT_MASK;
    if (events == 0) {
        return;
    }

    enables = (env->sregs[MMIX_SREG_RA] >> MMIX_RA_ENABLE_SHIFT) &
              MMIX_RA_EVENT_MASK;
    disabled_events = events & ~enables;
    enabled_events = events & enables;

    env->sregs[MMIX_SREG_RA] |= disabled_events;

    selected_event = mmix_select_arithmetic_event(enabled_events);
    if (selected_event != 0) {
        mmix_raise_arithmetic_trip(env, selected_event, insn, y, z);
    }
}

G_NORETURN void mmix_cpu_raise_emulator_failure(CPUMMIXState *env)
{
    CPUState *cs = env_cpu(env);

    cs->exception_index = EXCP_MMIX_EMULATOR_FAILURE;
    cpu_loop_exit(cs);
}

void helper_mmix_break_rules(CPUMMIXState *env, uint32_t insn, uint64_t y,
                             uint64_t z)
{
    CPUState *cs = env_cpu(env);

    mmix_cpu_set_rq_bits(env, MMIX_RQ_PROGRAM_B);
    if ((env->sregs[MMIX_SREG_RK] & MMIX_RQ_PROGRAM_B) == 0) {
        return;
    }

    mmix_cpu_record_program_exception(env, MMIX_RQ_PROGRAM_B);
    env->rule_break_insn = insn;
    env->rule_break_y = y;
    env->rule_break_z = z;
    cs->exception_index = EXCP_MMIX_DYNAMIC_TRAP;
    cpu_loop_exit(cs);
}

void helper_mmix_trip(CPUMMIXState *env, uint32_t insn, uint64_t y,
                      uint64_t z)
{
    CPUState *cs = env_cpu(env);

    mmix_commit_replay_before_synchronous_trap(env);
    env->sregs[MMIX_SREG_RW] = env->npc;
    env->sregs[MMIX_SREG_RX] = 0x8000000000000000ULL | insn;
    env->sregs[MMIX_SREG_RY] = y;
    env->sregs[MMIX_SREG_RZ] = z;
    env->sregs[MMIX_SREG_RB] = mmix_cpu_read_reg(env, 255);
    mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RJ]);
    env->pc = 0;
    env->npc = 4;
    qemu_log_mask(CPU_LOG_INT,
                  "MMIX trip from 0x%016" PRIx64 " to 0x%016x\n",
                  env->sregs[MMIX_SREG_RW] - 4, 0);
    cpu_loop_exit_noexc(cs);
}

void helper_mmix_trap(CPUMMIXState *env, uint32_t insn, uint64_t y,
                      uint64_t z)
{
    CPUState *cs = env_cpu(env);
    uint64_t handler = env->sregs[MMIX_SREG_RT];

    mmix_commit_replay_before_synchronous_trap(env);
    mmix_trap_restart_push(env, false, env->npc,
                           0x8000000000000000ULL | insn, y);
    env->sregs[MMIX_SREG_RWW] = env->npc;
    env->sregs[MMIX_SREG_RXX] = 0x8000000000000000ULL | insn;
    env->sregs[MMIX_SREG_RYY] = y;
    env->sregs[MMIX_SREG_RZZ] = z;
    env->sregs[MMIX_SREG_RBB] = mmix_cpu_read_reg(env, 255);
    mmix_cpu_put_rk(env, 0);
    mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RJ]);
    env->pc = handler;
    env->npc = handler + 4;
    qemu_log_mask(CPU_LOG_INT,
                  "MMIX trap from 0x%016" PRIx64 " to 0x%016" PRIx64 "\n",
                  env->sregs[MMIX_SREG_RWW] - 4, handler);
    cpu_loop_exit_noexc(cs);
}

static void mmix_resume_unsupported(CPUMMIXState *env, const char *why,
                                    uint64_t exec)
{
    qemu_log_mask(LOG_UNIMP,
                  "MMIX unsupported RESUME %s exec=0x%016" PRIx64 "\n",
                  why, exec);
    mmix_cpu_raise_emulator_failure(env);
}

static bool mmix_resume_ropcode1_eligible(uint32_t insn)
{
    uint8_t op = insn >> 24;

    /*
     * MMIXware mmix-doc section 38 permits normal opcodes in groups 0-3,
     * 6-7, and C-E, except TRAP. Optional SYNCD[I] and SYNCID[I]
     * substitution remains deferred.
     */
    return (op >= 0x01 && op <= 0x3f) ||
           (op >= 0x60 && op <= 0x7f) ||
           (op >= 0xc0 && op <= 0xef);
}

static bool mmix_resume_destination_marginal(CPUMMIXState *env,
                                             uint32_t insn)
{
    uint8_t x = insn >> 16;

    return x >= env->sregs[MMIX_SREG_RL] &&
           x < env->sregs[MMIX_SREG_RG];
}

G_NORETURN void mmix_cpu_break_rules_and_continue(CPUMMIXState *env,
                                                  uint32_t insn, uint64_t y,
                                                  uint64_t z)
{
    CPUState *cs = env_cpu(env);

    helper_mmix_break_rules(env, insn, y, z);
    env->pc = env->npc;
    env->npc += 4;
    cpu_loop_exit_noexc(cs);
}

static void mmix_resume_state(CPUMMIXState *env, bool trap_state,
                              uint32_t resume_insn, uint32_t resume_z)
{
    CPUState *cs = env_cpu(env);
    uint64_t where;
    uint64_t exec;
    uint64_t y;
    uint64_t z;
    uint8_t ropcode = 0;
    uint32_t insn;
    MMUAccessType translation_access = MMU_DATA_LOAD;
    MMIXTrapRestartState *restart;
    MMIXTrapRestartState untracked_restart = { 0 };
    MMIXTrapRestartState matched_restart = { 0 };
    MMIXTrapRestartState detached_restart = { 0 };
    bool tracked_restart = false;
    bool restart_detached = false;

    restart = &untracked_restart;

    if (trap_state) {
        where = env->sregs[MMIX_SREG_RWW];
        exec = env->sregs[MMIX_SREG_RXX];
        y = env->sregs[MMIX_SREG_RYY];
        z = env->sregs[MMIX_SREG_RZZ];
    } else {
        where = env->sregs[MMIX_SREG_RW];
        exec = env->sregs[MMIX_SREG_RX];
        y = env->sregs[MMIX_SREG_RY];
        z = env->sregs[MMIX_SREG_RZ];
    }

    if (trap_state && (int64_t)exec < 0) {
        if (mmix_trap_restart_take(env, where, exec, y, false,
                                   MMU_DATA_LOAD, &detached_restart)) {
            restart = &detached_restart;
            tracked_restart = true;
            restart_detached = true;
        } else {
            restart = &untracked_restart;
            tracked_restart = false;
        }
    }

    if ((int64_t)exec >= 0) {
        ropcode = exec >> 56;
        if (ropcode > 3 || (ropcode == 3 && !trap_state)) {
            mmix_cpu_break_rules_and_continue(
                env, resume_insn, mmix_cpu_read_reg(env, 0),
                mmix_cpu_read_reg(env, resume_z));
        }
        if (trap_state && ropcode != 3) {
            if (mmix_trap_restart_find_current(env, where,
                                               &matched_restart) ||
                mmix_trap_restart_find(env, where, exec, y,
                                       MMU_DATA_LOAD, &matched_restart)) {
                restart = &matched_restart;
                tracked_restart = true;
            }
        }
        if ((ropcode == 0 || ropcode == 1) && trap_state &&
            restart->stack_access.kind == MMIX_STACK_ACCESS_NONE &&
            (int64_t)env->pc >= 0) {
            mmix_cpu_break_rules_and_continue(
                env, resume_insn, mmix_cpu_read_reg(env, 0),
                mmix_cpu_read_reg(env, resume_z));
        }
        if ((ropcode == 1 &&
             !mmix_resume_ropcode1_eligible((uint32_t)exec)) ||
            ((ropcode == 1 || ropcode == 2) &&
             mmix_resume_destination_marginal(env, (uint32_t)exec))) {
            mmix_cpu_break_rules_and_continue(
                env, resume_insn, mmix_cpu_read_reg(env, 0),
                mmix_cpu_read_reg(env, resume_z));
        }
        if (ropcode == 3) {
            bool valid;

            translation_access = mmix_resume_translation_access(
                (uint32_t)exec);
            valid = (int64_t)env->pc < 0 &&
                    (exec & 0xffffffff00000000ULL) ==
                    MMIX_FORCED_TRANSLATION_EXEC_PREFIX &&
                    (int64_t)y >= 0 &&
                    ((uint32_t)exec >> 24) != MMIX_RESUME_OPCODE;

            if (!valid ||
                (translation_access != MMU_INST_FETCH && where < 4) ||
                !mmix_cpu_install_translation(env, y, z,
                                              translation_access)) {
                mmix_cpu_break_rules_and_continue(
                    env, resume_insn, mmix_cpu_read_reg(env, 0),
                    mmix_cpu_read_reg(env, resume_z));
            }

            /*
             * SAVE/UNSAVE can move an interrupted context to another CPU.
             * Recover only the partial helper state whose architectural trap
             * identity and address space match the restored context.
             */
            if (mmix_trap_restart_take(
                    env, where, exec, y, true, translation_access,
                    &detached_restart)) {
                restart = &detached_restart;
                tracked_restart = true;
                restart_detached = true;
            } else {
                restart = &untracked_restart;
                tracked_restart = false;
            }
        }
    }

    if (trap_state) {
        mmix_cpu_put_rk(env, mmix_cpu_read_reg(env, 255));
        mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RBB]);
    }

    if (trap_state) {
        bool pending_stack_access = true;
        uint64_t cause;

        mmix_trap_restart_restore_register_stack(env, restart);
        mmix_trap_restart_restore_helper_state(env, restart);

        switch (restart->stack_access.kind) {
        case MMIX_STACK_ACCESS_SPILL:
        case MMIX_STACK_ACCESS_SAVE:
            cause = MMIX_RQ_PROGRAM_W;
            break;
        case MMIX_STACK_ACCESS_FILL:
        case MMIX_STACK_ACCESS_UNSAVE:
            cause = MMIX_RQ_PROGRAM_R;
            break;
        default:
            pending_stack_access = false;
            cause = 0;
            break;
        }

        if (pending_stack_access) {
            if ((ropcode != 3 && (exec & cause) == 0) || where < 4) {
                mmix_resume_unsupported(env, "invalid pending stack access",
                                        exec);
            }

            if (restart->register_stack_rebased) {
                /* Retry the instruction against the UNSAVE-rebuilt stack. */
                memset(&restart->stack_access, 0,
                       sizeof(restart->stack_access));
                env->pc = where - 4;
                env->npc = where;
                if (tracked_restart) {
                    mmix_trap_restart_finish(env, restart,
                                             restart_detached);
                }
                cpu_loop_exit_noexc(cs);
            }

            if (cause == MMIX_RQ_PROGRAM_W) {
                g_assert(mmix_cpu_prepare_stack_store_retry(
                    env, &restart->stack_access));
            } else {
                g_assert(mmix_cpu_prepare_stack_load_retry(
                    env, &restart->stack_access));
            }

            if ((int64_t)exec < 0 || ropcode == 1 || ropcode == 2) {
                /*
                 * rWW points past the instruction whose helper faulted.
                 * Re-execution continues at the first access that did not
                 * commit.
                 */
                env->pc = where - 4;
                env->npc = where;
                if (tracked_restart) {
                    mmix_trap_restart_finish(env, restart,
                                             restart_detached);
                }
                cpu_loop_exit_noexc(cs);
            }
        }
    }

    if ((int64_t)exec < 0) {
        env->pc = where;
        env->npc = where + 4;
        if (tracked_restart) {
            mmix_trap_restart_finish(env, restart, restart_detached);
        }
        cpu_loop_exit_noexc(cs);
    }

    ropcode = exec >> 56;
    insn = exec;
    switch (ropcode) {
    case 2:
    {
        uint32_t events = (exec >> 40) & MMIX_RA_EVENT_MASK;
        uint32_t reg = (insn >> 16) & 0xff;

        mmix_cpu_write_reg(env, reg, z);
        if (events != 0) {
            env->pc = where - 4;
            env->npc = where;
            if (tracked_restart) {
                mmix_trap_restart_finish(env, restart, restart_detached);
            }
            mmix_update_ra_events(env, events, insn, y, z);
        }
        env->pc = where;
        env->npc = where + 4;
        if (tracked_restart) {
            mmix_trap_restart_finish(env, restart, restart_detached);
        }
        cpu_loop_exit_noexc(cs);
    }
    case 0:
        if (where < 4) {
            mmix_cpu_break_rules_and_continue(
                env, resume_insn, mmix_cpu_read_reg(env, 0),
                mmix_cpu_read_reg(env, resume_z));
        }
        env->insn_replay = (MMIXInsnReplayState) {
            .insn_pc = where - 4,
            .continuation = where,
            .insn = insn,
            .trap_restart_sequence = tracked_restart ?
                                     restart->sequence : 0,
            .active = true,
        };
        env->pc = env->insn_replay.insn_pc;
        env->npc = env->insn_replay.continuation;
        cpu_loop_exit_noexc(cs);
    case 1:
        if (where < 4) {
            mmix_cpu_break_rules_and_continue(
                env, resume_insn, mmix_cpu_read_reg(env, 0),
                mmix_cpu_read_reg(env, resume_z));
        }
        env->insn_replay = (MMIXInsnReplayState) {
            .insn_pc = where - 4,
            .continuation = where,
            .y = y,
            .z = z,
            .insn = insn,
            .trap_restart_sequence = tracked_restart ?
                                     restart->sequence : 0,
            .substitute_operands = true,
            .active = true,
        };
        env->pc = env->insn_replay.insn_pc;
        env->npc = env->insn_replay.continuation;
        cpu_loop_exit_noexc(cs);
    case 3:
        env->forced_translation_insn = 0;
        env->forced_translation_address = 0;
        env->forced_translation_where = 0;
        env->forced_translation_access = 0;
        if (translation_access == MMU_INST_FETCH) {
            env->pc = where;
            env->npc = where + 4;
            if (tracked_restart) {
                mmix_trap_restart_finish(env, restart, restart_detached);
            }
            cpu_loop_exit_noexc(cs);
        }

        if (tracked_restart) {
            env->pc = where - 4;
            env->npc = where;
            mmix_trap_restart_finish(env, restart, restart_detached);
            cpu_loop_exit_noexc(cs);
        }

        env->insn_replay = (MMIXInsnReplayState) {
            .insn_pc = where - 4,
            .continuation = where,
            .insn = insn,
            .trap_restart_sequence = 0,
            .active = true,
        };
        env->pc = env->insn_replay.insn_pc;
        env->npc = env->insn_replay.continuation;
        cpu_loop_exit_noexc(cs);
    default:
        g_assert_not_reached();
    }
    g_assert_not_reached();
}

void helper_mmix_resume(CPUMMIXState *env, uint32_t insn, uint32_t z)
{
    switch (z) {
    case 0:
        mmix_resume_state(env, false, insn, z);
        break;
    case 1:
        mmix_resume_state(env, true, insn, z);
        break;
    default:
        g_assert_not_reached();
    }
    g_assert_not_reached();
}

G_NORETURN void mmix_cpu_shutdown_with_log(CPUMMIXState *env,
                                                  const char *reason,
                                                  int exit_code)
{
    CPUState *cs = env_cpu(env);

    if (!env->test_exit_seen) {
        env->test_exit_seen = true;
        if (g_strcmp0(reason, "MMIX test exit") != 0) {
            qemu_log_mask(CPU_LOG_INT, "%s at 0x%016" PRIx64 "\n", reason,
                          env->pc);
        }
        qemu_log_mask(CPU_LOG_INT, "MMIX test exit at 0x%016" PRIx64 "\n",
                      env->pc);
        log_cpu_state_mask(CPU_LOG_INT, cs, 0);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN,
                                               exit_code);
    }
    cs->halted = 1;
    cpu_loop_exit_noexc(cs);
}

void helper_mmix_test_exit(CPUMMIXState *env)
{
    mmix_cpu_shutdown_with_log(env, "MMIX test exit", 0);
}


void mmix_cpu_do_interrupt(CPUState *cs)
{
    CPUMMIXState *env = cpu_env(cs);
    uint32_t event;
    uint64_t causes;
    uint64_t requests;
    uint64_t where;
    hwaddr handler;

    switch (cs->exception_index) {
    case EXCP_MMIX_EMULATOR_FAILURE:
        qemu_log_mask(CPU_LOG_INT,
                      "MMIX emulator failure at 0x%016" PRIx64 "\n",
                      env->pc);
        break;
    case EXCP_MMIX_ARITHMETIC_TRIP:
        event = env->arithmetic_trip_event;
        handler = mmix_arithmetic_trip_handler(event);
        qemu_log_mask(CPU_LOG_INT,
                      "MMIX arithmetic trip event=0x%02x from 0x%016" PRIx64
                      " to 0x%016" HWADDR_PRIx "\n",
                      event, env->pc, handler);
        mmix_commit_replay_before_synchronous_trap(env);
        env->sregs[MMIX_SREG_RB] = mmix_cpu_read_reg(env, 255);
        mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RJ]);
        env->pc = handler;
        env->npc = handler + 4;
        env->arithmetic_trip_event = 0;
        cs->exception_index = -1;
        break;
    case EXCP_MMIX_DYNAMIC_TRAP: {
        uint64_t exec;
        uint64_t y;
        uint64_t z;

        causes = env->program_exception_causes & MMIX_RQ_PROGRAM_MASK;
        handler = env->sregs[MMIX_SREG_RTT];
        qemu_log_mask(CPU_LOG_INT,
                      "MMIX dynamic trap causes=0x%016" PRIx64
                      " from 0x%016" PRIx64 " to 0x%016" HWADDR_PRIx "\n",
                      causes, env->pc, handler);
        mmix_commit_replay_before_synchronous_trap(env);
        if (causes & MMIX_RQ_PROGRAM_B) {
            mmix_cpu_enter_trap(
                cs, handler, env->npc,
                MMIX_DYNAMIC_TRAP_RESUME_NEXT | causes |
                env->rule_break_insn,
                env->rule_break_y, env->rule_break_z);
            env->rule_break_insn = 0;
            env->rule_break_y = 0;
            env->rule_break_z = 0;
        } else {
            exec = causes | env->program_exception_insn;
            y = 0;
            z = 0;
            if (env->program_exception_data_access) {
                y = env->data_access.address;
                z = env->data_access.value;
            }
            mmix_cpu_enter_trap(cs, handler, env->npc, exec, y, z);
        }
        env->program_exception_causes = 0;
        env->program_exception_insn = 0;
        env->program_exception_data_access = false;
        break;
    }
    case EXCP_MMIX_FORCED_TRANSLATION:
        handler = env->sregs[MMIX_SREG_RT];
        where = env->forced_translation_access == MMU_INST_FETCH ?
                env->forced_translation_address : env->npc;
        env->forced_translation_where = where;
        qemu_log_mask(CPU_LOG_INT,
                      "MMIX forced translation trap address=0x%016" PRIx64
                      " from 0x%016" PRIx64 " to 0x%016" HWADDR_PRIx "\n",
                      env->forced_translation_address, env->pc, handler);
        mmix_cpu_enter_trap(
            cs, handler, where,
            MMIX_FORCED_TRANSLATION_EXEC_PREFIX |
            env->forced_translation_insn,
            env->forced_translation_address, 0);
        break;
    case EXCP_MMIX_INTERRUPT:
        requests = env->sregs[MMIX_SREG_RQ] & env->sregs[MMIX_SREG_RK];
        handler = env->sregs[MMIX_SREG_RTT];
        qemu_log_mask(CPU_LOG_INT,
                      "MMIX external dynamic trap requests=0x%016" PRIx64
                      " from 0x%016" PRIx64 " to 0x%016" HWADDR_PRIx "\n",
                      requests, env->pc, handler);
        mmix_cpu_enter_trap(cs, handler, env->pc,
                            MMIX_DYNAMIC_TRAP_RESUME_NEXT, 0, 0);
        break;
    default:
        qemu_log_mask(CPU_LOG_INT, "MMIX exception %d at 0x%016" PRIx64 "\n",
                      cs->exception_index, env->pc);
        break;
    }
}

bool mmix_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    CPUMMIXState *env = cpu_env(cs);

    if (interrupt_request & CPU_INTERRUPT_HARD) {
        if (!mmix_cpu_interrupt_enabled(env)) {
            return false;
        }
        cs->exception_index = EXCP_MMIX_INTERRUPT;
        mmix_cpu_do_interrupt(cs);
        return true;
    }

    return false;
}

hwaddr mmix_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    CPUMMIXState *env = cpu_env(cs);
    MMIXAddressTranslation translation;
    hwaddr physical;

    if (mmix_semihosting_get_phys_addr_debug(env, addr, &physical)) {
        return physical;
    }

    if (mmix_translate_address(env, addr, MMU_DATA_LOAD, true, false,
                               &translation)) {
        return translation.physical;
    }
    return -1;
}
