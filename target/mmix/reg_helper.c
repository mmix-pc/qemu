/*
 * QEMU MMIX register helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "mmix-helper.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/cpu-ldst.h"
#include "exec/cputlb.h"
#include "exec/helper-proto.h"
#include "exec/log.h"

#define MMIX_STACK_NO_RING_INDEX UINT32_MAX

static void mmix_cpu_stack_access_begin(CPUMMIXState *env,
                                        MMIXStackAccessKind kind,
                                        uint64_t address,
                                        uint32_t ring_index,
                                        uint64_t value)
{
    MMIXStackAccessState *access = &env->stack_access;

    g_assert(access->kind == MMIX_STACK_ACCESS_NONE);
    access->kind = kind;
    access->address = address;
    access->ring_index = ring_index;
    access->value = value;
}

static bool mmix_cpu_stack_access_matches(const MMIXStackAccessState *a,
                                          const MMIXStackAccessState *b)
{
    return a->kind == b->kind && a->address == b->address &&
           a->ring_index == b->ring_index && a->value == b->value;
}

static void mmix_cpu_stack_access_commit(CPUMMIXState *env,
                                         MMIXStackAccessState *access)
{
    if (access == &env->stack_access) {
        GArray *stack = env_archcpu(env)->trap_restart_stack;
        unsigned int i;

        for (i = 0; i < stack->len; i++) {
            MMIXTrapRestartState *restart =
                &g_array_index(stack, MMIXTrapRestartState, i);

            if (mmix_cpu_stack_access_matches(&restart->stack_access,
                                              access)) {
                restart->stack_access.completed = true;
            }
        }
    }
    memset(access, 0, sizeof(*access));
}

bool mmix_cpu_prepare_stack_store_retry(CPUMMIXState *env,
                                        MMIXStackAccessState *access)
{
    unsigned idx;

    if (access->completed) {
        g_assert(access->address + 8 == env->sregs[MMIX_SREG_RS]);
        mmix_cpu_stack_access_commit(env, access);
        return true;
    }
    if (access->kind == MMIX_STACK_ACCESS_SAVE) {
        g_assert(access->address == env->sregs[MMIX_SREG_RS]);
        g_assert(access->ring_index == MMIX_STACK_NO_RING_INDEX);
        mmix_cpu_stack_access_commit(env, access);
        return true;
    }
    if (access->kind != MMIX_STACK_ACCESS_SPILL) {
        return false;
    }

    idx = (access->address >> 3) & env->lring_mask;
    g_assert(access->address == env->sregs[MMIX_SREG_RS]);
    g_assert(access->ring_index == idx);
    g_assert(access->value == env->local_regs[idx]);
    mmix_cpu_stack_access_commit(env, access);
    return true;
}

bool mmix_cpu_prepare_stack_load_retry(CPUMMIXState *env,
                                       MMIXStackAccessState *access)
{
    unsigned idx;

    if (access->completed) {
        g_assert(access->address == env->sregs[MMIX_SREG_RS]);
        mmix_cpu_stack_access_commit(env, access);
        return true;
    }
    switch (access->kind) {
    case MMIX_STACK_ACCESS_FILL:
        idx = (access->address >> 3) & env->lring_mask;
        g_assert(access->address == env->sregs[MMIX_SREG_RS] - 8);
        g_assert(access->ring_index == idx);
        break;
    case MMIX_STACK_ACCESS_UNSAVE:
        g_assert(access->address == env->sregs[MMIX_SREG_RS] - 8);
        g_assert(access->ring_index == MMIX_STACK_NO_RING_INDEX);
        break;
    default:
        return false;
    }

    g_assert(access->value == 0);
    mmix_cpu_stack_access_commit(env, access);
    return true;
}

static unsigned mmix_cpu_get_rg(CPUMMIXState *env)
{
    uint64_t rg = env->sregs[MMIX_SREG_RG];

    if (rg < 32) {
        return 32;
    }
    if (rg > 255) {
        return 255;
    }
    return (unsigned)rg;
}

static unsigned mmix_cpu_get_rl(CPUMMIXState *env)
{
    uint64_t rl = env->sregs[MMIX_SREG_RL];
    unsigned rg = mmix_cpu_get_rg(env);

    if (rl > rg) {
        return rg;
    }
    return (unsigned)rl;
}

static unsigned mmix_cpu_local_index(CPUMMIXState *env, unsigned reg)
{
    unsigned base = env->sregs[MMIX_SREG_RO] >> 3;

    return (base + reg) & env->lring_mask;
}

static bool mmix_cpu_local_room(CPUMMIXState *env, unsigned new_rl)
{
    unsigned base = env->sregs[MMIX_SREG_RO] >> 3;
    unsigned stack = env->sregs[MMIX_SREG_RS] >> 3;
    unsigned distance = (stack - base) & env->lring_mask;

    if (distance == 0 && env->sregs[MMIX_SREG_RO] == env->sregs[MMIX_SREG_RS]) {
        distance = env->lring_size;
    }

    return new_rl < distance;
}

static unsigned mmix_cpu_stack_depth(CPUMMIXState *env)
{
    unsigned base = env->sregs[MMIX_SREG_RO] >> 3;
    unsigned stack = env->sregs[MMIX_SREG_RS] >> 3;

    return (base - stack) & env->lring_mask;
}

static void mmix_cpu_stack_store(CPUMMIXState *env)
{
    uintptr_t ra = GETPC();
    uint64_t addr = env->sregs[MMIX_SREG_RS];
    unsigned idx = (addr >> 3) & env->lring_mask;
    uint64_t value = env->local_regs[idx];

    mmix_cpu_stack_access_begin(env, MMIX_STACK_ACCESS_SPILL, addr, idx,
                                value);
    cpu_stq_be_data_ra(env, addr, value, ra);
    env->sregs[MMIX_SREG_RS] = addr + 8;
    mmix_cpu_stack_access_commit(env, &env->stack_access);
}

static void mmix_cpu_stack_load(CPUMMIXState *env)
{
    uintptr_t ra = GETPC();
    uint64_t addr = env->sregs[MMIX_SREG_RS] - 8;
    unsigned idx = (addr >> 3) & env->lring_mask;
    uint64_t value;

    mmix_cpu_stack_access_begin(env, MMIX_STACK_ACCESS_FILL, addr, idx, 0);
    value = cpu_ldq_be_data_ra(env, addr, ra);
    env->local_regs[idx] = value;
    env->sregs[MMIX_SREG_RS] = addr;
    mmix_cpu_stack_access_commit(env, &env->stack_access);
}

static void mmix_cpu_stack_write_octa(CPUMMIXState *env, uint64_t val)
{
    uintptr_t ra = GETPC();
    uint64_t addr = env->sregs[MMIX_SREG_RS];

    mmix_cpu_stack_access_begin(env, MMIX_STACK_ACCESS_SAVE, addr,
                                MMIX_STACK_NO_RING_INDEX, val);
    cpu_stq_be_data_ra(env, addr, val, ra);
    env->sregs[MMIX_SREG_RS] = addr + 8;
    mmix_cpu_stack_access_commit(env, &env->stack_access);
}

static uint64_t mmix_cpu_stack_read_octa(CPUMMIXState *env)
{
    uintptr_t ra = GETPC();
    uint64_t addr = env->sregs[MMIX_SREG_RS] - 8;
    uint64_t value;

    mmix_cpu_stack_access_begin(env, MMIX_STACK_ACCESS_UNSAVE, addr,
                                MMIX_STACK_NO_RING_INDEX, 0);
    value = cpu_ldq_be_data_ra(env, addr, ra);
    env->sregs[MMIX_SREG_RS] = addr;
    mmix_cpu_stack_access_commit(env, &env->stack_access);
    return value;
}

static void mmix_cpu_fill_stack(CPUMMIXState *env)
{
    if (env->sregs[MMIX_SREG_RS] <= MMIX_INITIAL_STACK) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX register stack underflow during POP "
                      "rO=0x%" PRIx64 " rS=0x%" PRIx64 " depth=%u\n",
                      env->sregs[MMIX_SREG_RO],
                      env->sregs[MMIX_SREG_RS],
                      mmix_cpu_stack_depth(env));
        mmix_cpu_raise_emulator_failure(env);
    }
    mmix_cpu_stack_load(env);
}

static void mmix_cpu_ensure_local_room(CPUMMIXState *env, unsigned new_rl)
{
    while (!mmix_cpu_local_room(env, new_rl)) {
        mmix_cpu_stack_store(env);
    }
}

static void mmix_cpu_grow_rl(CPUMMIXState *env, unsigned new_rl)
{
    unsigned old_rl = mmix_cpu_get_rl(env);
    unsigned rg = mmix_cpu_get_rg(env);
    unsigned i;

    new_rl = MIN(new_rl, rg);
    if (new_rl <= old_rl) {
        return;
    }
    mmix_cpu_ensure_local_room(env, new_rl);
    for (i = old_rl; i < new_rl; i++) {
        env->local_regs[mmix_cpu_local_index(env, i)] = 0;
    }
    env->sregs[MMIX_SREG_RL] = new_rl;
}

uint64_t mmix_cpu_read_reg(CPUMMIXState *env, unsigned reg)
{
    unsigned rg = mmix_cpu_get_rg(env);
    unsigned rl = mmix_cpu_get_rl(env);

    if (reg >= MMIX_REGS) {
        return 0;
    }
    if (reg >= rg) {
        return env->regs[reg];
    }
    if (reg < rl) {
        return env->local_regs[mmix_cpu_local_index(env, reg)];
    }
    return 0;
}

void mmix_cpu_write_reg(CPUMMIXState *env, unsigned reg, uint64_t val)
{
    unsigned rg = mmix_cpu_get_rg(env);

    if (reg >= MMIX_REGS) {
        return;
    }
    if (reg >= rg) {
        env->regs[reg] = val;
        return;
    }

    mmix_cpu_grow_rl(env, reg + 1);
    env->local_regs[mmix_cpu_local_index(env, reg)] = val;
}

void mmix_cpu_put_rl(CPUMMIXState *env, uint64_t val)
{
    unsigned rl = mmix_cpu_get_rl(env);
    unsigned new_rl = rl;

    if (val < rl) {
        new_rl = val;
    }

    env->sregs[MMIX_SREG_RL] = new_rl;
}

bool mmix_cpu_is_privileged(CPUMMIXState *env)
{
    return (int64_t)env->pc < 0 ||
           (env->sregs[MMIX_SREG_RK] & MMIX_RQ_PROGRAM_K) == 0;
}

void mmix_cpu_set_rq_bits(CPUMMIXState *env, uint64_t bits)
{
    uint64_t new_bits = bits & ~env->sregs[MMIX_SREG_RQ];

    env->sregs[MMIX_SREG_RQ] |= bits;
    env->rq_new_bits |= new_bits;
}

void mmix_cpu_record_program_exception(CPUMMIXState *env, uint64_t causes)
{
    env->program_exception_causes |= causes & MMIX_RQ_PROGRAM_MASK;
    mmix_cpu_set_rq_bits(env, causes & MMIX_RQ_PROGRAM_MASK);
}

void mmix_cpu_raise_dynamic_trap(CPUMMIXState *env, uint64_t causes)
{
    CPUState *cs = env_cpu(env);

    env->program_exception_insn = env->data_access_insn;
    mmix_cpu_record_program_exception(env, causes);
    cs->exception_index = EXCP_MMIX_DYNAMIC_TRAP;
    cpu_loop_exit(cs);
}

static void mmix_cpu_update_translation_state(CPUMMIXState *env)
{
    tlb_flush(env_cpu(env));
}

void mmix_cpu_put_rk(CPUMMIXState *env, uint64_t val)
{
    env->sregs[MMIX_SREG_RK] = val;
    mmix_cpu_update_translation_state(env);
    mmix_cpu_update_interrupt(env);
}

static void mmix_cpu_put_rv(CPUMMIXState *env, uint64_t val)
{
    env->sregs[MMIX_SREG_RV] = val;
    env->flat_translation = false;
    mmix_cpu_flush_translation_caches(env);
}

uint64_t helper_mmix_read_reg(CPUMMIXState *env, uint32_t reg)
{
    return mmix_cpu_read_reg(env, reg);
}

void helper_mmix_write_reg(CPUMMIXState *env, uint32_t reg, uint64_t val)
{
    mmix_cpu_write_reg(env, reg, val);
}

uint64_t helper_mmix_read_sreg(CPUMMIXState *env, uint32_t reg)
{
    uint64_t val;

    if (reg >= MMIX_SREGS) {
        mmix_cpu_raise_emulator_failure(env);
    }
    val = env->sregs[reg];
    if (reg == MMIX_SREG_RQ) {
        /* MMIXware sections 43 and MMIX-PIPE 148 define GET/PUT handoff. */
        env->rq_new_bits = 0;
    }
    return val;
}

static void mmix_cpu_put_rq(CPUMMIXState *env, uint64_t val)
{
    /* Software cannot manufacture the virtual interrupt-controller input. */
    val &= ~MMIX_RQ_INTERRUPT_CONTROLLER;
    val |= env->rq_new_bits;
    if (env->interrupt_controller_level) {
        val |= MMIX_RQ_INTERRUPT_CONTROLLER;
    }
    env->sregs[MMIX_SREG_RQ] = val;
    mmix_cpu_update_interrupt(env);
}

static void mmix_cpu_put_rg(CPUMMIXState *env, uint64_t val)
{
    env->sregs[MMIX_SREG_RG] = val;
    if (env->sregs[MMIX_SREG_RL] > val) {
        env->sregs[MMIX_SREG_RL] = val;
    }
}

static bool mmix_sreg_read_only(uint32_t reg)
{
    switch (reg) {
    case MMIX_SREG_RN:
    case MMIX_SREG_RO:
    case MMIX_SREG_RS:
        return true;
    default:
        return false;
    }
}

static bool mmix_sreg_privileged(uint32_t reg)
{
    switch (reg) {
    case MMIX_SREG_RC:
    case MMIX_SREG_RI:
    case MMIX_SREG_RT:
    case MMIX_SREG_RTT:
    case MMIX_SREG_RK:
    case MMIX_SREG_RQ:
    case MMIX_SREG_RU:
    case MMIX_SREG_RV:
        return true;
    default:
        return false;
    }
}

void helper_mmix_put_sreg(CPUMMIXState *env, uint32_t insn, uint32_t reg,
                          uint64_t val)
{
    if (reg >= MMIX_SREGS || mmix_sreg_read_only(reg)) {
        helper_mmix_break_rules(env, insn, 0, val);
        return;
    }
    if (mmix_sreg_privileged(reg) && !mmix_cpu_is_privileged(env)) {
        mmix_cpu_raise_dynamic_trap(env, MMIX_RQ_PROGRAM_K);
    }
    if (reg == MMIX_SREG_RG && (val < 32 || val > 255)) {
        helper_mmix_break_rules(env, insn, 0, val);
        return;
    }
    if (reg == MMIX_SREG_RL && val > mmix_cpu_get_rl(env)) {
        helper_mmix_break_rules(env, insn, 0, val);
        return;
    }

    switch (reg) {
    case MMIX_SREG_RA:
        env->sregs[MMIX_SREG_RA] = val & MMIX_RA_VALID_MASK;
        break;
    case MMIX_SREG_RG:
        mmix_cpu_put_rg(env, val);
        break;
    case MMIX_SREG_RL:
        mmix_cpu_put_rl(env, val);
        break;
    case MMIX_SREG_RQ:
        mmix_cpu_put_rq(env, val);
        break;
    case MMIX_SREG_RK:
        mmix_cpu_put_rk(env, val);
        break;
    case MMIX_SREG_RV:
        mmix_cpu_put_rv(env, val);
        break;
    default:
        env->sregs[reg] = val;
        break;
    }
}

void helper_mmix_sync(CPUMMIXState *env, uint32_t mode)
{
    if (mode >= 4 && mode <= 7 && !mmix_cpu_is_privileged(env)) {
        mmix_cpu_raise_dynamic_trap(env, MMIX_RQ_PROGRAM_K);
    }
}

uint64_t helper_mmix_ldvts(CPUMMIXState *env, uint64_t key)
{
    if (!mmix_cpu_is_privileged(env)) {
        mmix_cpu_raise_dynamic_trap(env, MMIX_RQ_PROGRAM_K);
    }

    return mmix_cpu_ldvts(env, key);
}

void helper_mmix_push(CPUMMIXState *env, uint32_t x, uint64_t next_pc)
{
    unsigned rg = mmix_cpu_get_rg(env);
    unsigned old_rl = mmix_cpu_get_rl(env);
    unsigned hole = x;
    unsigned pushed;
    unsigned new_rl;

    if (x >= rg) {
        hole = old_rl;
        mmix_cpu_ensure_local_room(env, old_rl + 1);
        env->local_regs[mmix_cpu_local_index(env, hole)] = old_rl;
        pushed = old_rl + 1;
        new_rl = 0;
    } else {
        if (x >= old_rl) {
            mmix_cpu_grow_rl(env, x + 1);
            old_rl = x + 1;
        }
        env->local_regs[mmix_cpu_local_index(env, x)] = x;
        pushed = x + 1;
        new_rl = old_rl - pushed;
    }

    env->sregs[MMIX_SREG_RJ] = next_pc;
    env->sregs[MMIX_SREG_RO] += (uint64_t)pushed * 8;
    env->sregs[MMIX_SREG_RL] = new_rl;
}

uint64_t helper_mmix_pop(CPUMMIXState *env, uint32_t x, uint32_t yz)
{
    unsigned old_rl = mmix_cpu_get_rl(env);
    unsigned base = env->sregs[MMIX_SREG_RO] >> 3;
    unsigned saved;
    unsigned preserved;
    uint64_t output = 0;
    uint64_t dest;

    if (mmix_cpu_stack_depth(env) == 0) {
        mmix_cpu_fill_stack(env);
    }

    if (x != 0 && x <= old_rl) {
        output = env->local_regs[mmix_cpu_local_index(env, x - 1)];
    }

    saved = env->local_regs[(base - 1) & env->lring_mask] & 0xff;
    while (mmix_cpu_stack_depth(env) <= saved) {
        mmix_cpu_fill_stack(env);
    }

    if (x != 0) {
        env->local_regs[(base - 1) & env->lring_mask] = output;
    }

    preserved = x <= old_rl ? x : old_rl + 1;
    env->sregs[MMIX_SREG_RO] -= (uint64_t)(saved + 1) * 8;
    env->sregs[MMIX_SREG_RL] = MIN(saved + preserved, mmix_cpu_get_rg(env));

    dest = env->sregs[MMIX_SREG_RJ] + ((uint64_t)yz << 2);
    return dest & ~3ULL;
}

static const uint32_t mmix_save_sregs[] = {
    MMIX_SREG_RB,
    MMIX_SREG_RD,
    MMIX_SREG_RE,
    MMIX_SREG_RH,
    MMIX_SREG_RJ,
    MMIX_SREG_RM,
    MMIX_SREG_RR,
    MMIX_SREG_RP,
    MMIX_SREG_RW,
    MMIX_SREG_RX,
    MMIX_SREG_RY,
    MMIX_SREG_RZ,
};

void helper_mmix_save(CPUMMIXState *env, uint32_t insn, uint32_t x)
{
    MMIXSaveRestartState *restart = &env->save_restart;
    unsigned rg;

    if (restart->phase == MMIX_SAVE_RESTART_NONE) {
        rg = mmix_cpu_get_rg(env);
        if (x < rg) {
            helper_mmix_break_rules(env, insn, 0, 0);
            return;
        }

        restart->phase = MMIX_SAVE_RESTART_PREPARE;
        restart->x = x;
        restart->rg = rg;
        restart->old_rl = mmix_cpu_get_rl(env);
        memcpy(restart->regs, env->regs, sizeof(restart->regs));
        memcpy(restart->sregs, env->sregs, sizeof(restart->sregs));
        restart->packed = ((uint64_t)rg << 56) |
                          (env->sregs[MMIX_SREG_RA] & MMIX_RA_VALID_MASK);
    } else {
        g_assert(restart->x == x);
        rg = restart->rg;
    }

    if (restart->phase == MMIX_SAVE_RESTART_PREPARE) {
        mmix_cpu_ensure_local_room(env, restart->old_rl);
        env->local_regs[mmix_cpu_local_index(env, restart->old_rl)] =
            restart->old_rl;
        env->sregs[MMIX_SREG_RO] += (uint64_t)(restart->old_rl + 1) * 8;
        env->sregs[MMIX_SREG_RL] = 0;
        restart->phase = MMIX_SAVE_RESTART_SPILL;
    }

    if (restart->phase == MMIX_SAVE_RESTART_SPILL) {
        while (env->sregs[MMIX_SREG_RS] != env->sregs[MMIX_SREG_RO]) {
            mmix_cpu_stack_store(env);
        }
        restart->phase = MMIX_SAVE_RESTART_GLOBALS;
        restart->index = rg;
    }

    if (restart->phase == MMIX_SAVE_RESTART_GLOBALS) {
        while (restart->index < MMIX_REGS) {
            mmix_cpu_stack_write_octa(env, restart->regs[restart->index]);
            restart->index++;
        }
        restart->phase = MMIX_SAVE_RESTART_SPECIALS;
        restart->index = 0;
    }

    if (restart->phase == MMIX_SAVE_RESTART_SPECIALS) {
        while (restart->index < ARRAY_SIZE(mmix_save_sregs)) {
            mmix_cpu_stack_write_octa(
                env, restart->sregs[mmix_save_sregs[restart->index]]);
            restart->index++;
        }
        restart->phase = MMIX_SAVE_RESTART_PACKED;
    }

    mmix_cpu_stack_write_octa(env, restart->packed);

    env->sregs[MMIX_SREG_RO] = env->sregs[MMIX_SREG_RS];
    env->regs[x] = env->sregs[MMIX_SREG_RO] - 8;
    memset(restart, 0, sizeof(*restart));
}

void helper_mmix_unsave(CPUMMIXState *env, uint32_t z)
{
    uint64_t addr;
    uint64_t packed;
    unsigned rg;
    unsigned saved_rl;
    unsigned i;

    if (env->unsave_restart_active) {
        addr = env->unsave_restart_address;
    } else {
        addr = mmix_cpu_read_reg(env, z) & ~7ULL;
        env->unsave_restart_address = addr;
        env->unsave_restart_active = true;
    }
    env->sregs[MMIX_SREG_RS] = addr + 8;

    packed = mmix_cpu_stack_read_octa(env);
    rg = packed >> 56;
    if (rg < 32) {
        rg = 32;
    }
    env->sregs[MMIX_SREG_RG] = rg;
    env->sregs[MMIX_SREG_RA] = packed & MMIX_RA_VALID_MASK;

    for (i = ARRAY_SIZE(mmix_save_sregs); i > 0; i--) {
        env->sregs[mmix_save_sregs[i - 1]] = mmix_cpu_stack_read_octa(env);
    }
    for (i = MMIX_REGS; i > rg; i--) {
        env->regs[i - 1] = mmix_cpu_stack_read_octa(env);
    }

    mmix_cpu_stack_load(env);
    saved_rl = env->local_regs[(env->sregs[MMIX_SREG_RS] >> 3) &
                               env->lring_mask] & 0xff;
    for (i = 0; i < saved_rl; i++) {
        mmix_cpu_stack_load(env);
    }

    env->sregs[MMIX_SREG_RO] = env->sregs[MMIX_SREG_RS];
    env->sregs[MMIX_SREG_RL] = MIN(saved_rl, rg);
    env->unsave_restart_active = false;
}
