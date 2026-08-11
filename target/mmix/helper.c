/*
 * QEMU MMIX helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "fp.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/cpu-ldst.h"
#include "exec/log.h"
#include "exec/cputlb.h"
#include "exec/helper-proto.h"
#include "fpu/softfloat.h"
#include "system/memory.h"
#include "system/runstate.h"
#include <math.h>

#define MMIX_QNAN_BIT      0x0008000000000000ULL
#define MMIX_DEFAULT_NAN   0x7ff8000000000000ULL
#define MMIX_SEMIHOSTING_STRING_MAX 256
/*
 * Keep the legacy UART-backed output path behind the MMIX semihosting
 * dispatch boundary. This can later be gated by QEMU semihosting policy and
 * routed through QEMU's semihosting console.
 */
#define MMIX_SEMIHOSTING_STDOUT_TX 0x100000004ULL

typedef enum MMIXSemihostingService {
    MMIX_SEMIHOSTING_SERVICE_HALT = 0,
    MMIX_SEMIHOSTING_SERVICE_FPUTS = 7,
} MMIXSemihostingService;

typedef enum MMIXSemihostingHandle {
    MMIX_SEMIHOSTING_HANDLE_STDOUT = 1,
} MMIXSemihostingHandle;

typedef enum MMIXSemihostingAction {
    MMIX_SEMIHOSTING_ACTION_HALT,
    MMIX_SEMIHOSTING_ACTION_FPUTS_STDOUT,
    MMIX_SEMIHOSTING_ACTION_FPUTS_BAD_HANDLE,
    MMIX_SEMIHOSTING_ACTION_UNSUPPORTED,
} MMIXSemihostingAction;

typedef struct MMIXSemihostingCall {
    MMIXSemihostingAction action;
    uint32_t service;
    uint32_t handle;
    bool requires_semihosting;
} MMIXSemihostingCall;

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

    cpu_stq_be_data_ra(env, addr, env->local_regs[idx], ra);
    env->sregs[MMIX_SREG_RS] = addr + 8;
}

static void mmix_cpu_stack_load(CPUMMIXState *env)
{
    uintptr_t ra = GETPC();
    uint64_t addr = env->sregs[MMIX_SREG_RS] - 8;
    unsigned idx = (addr >> 3) & env->lring_mask;

    env->sregs[MMIX_SREG_RS] = addr;
    env->local_regs[idx] = cpu_ldq_be_data_ra(env, addr, ra);
}

static void mmix_cpu_stack_write_octa(CPUMMIXState *env, uint64_t val)
{
    uintptr_t ra = GETPC();
    uint64_t addr = env->sregs[MMIX_SREG_RS];

    cpu_stq_be_data_ra(env, addr, val, ra);
    env->sregs[MMIX_SREG_RS] = addr + 8;
}

static uint64_t mmix_cpu_stack_read_octa(CPUMMIXState *env)
{
    uintptr_t ra = GETPC();
    uint64_t addr = env->sregs[MMIX_SREG_RS] - 8;

    env->sregs[MMIX_SREG_RS] = addr;
    return cpu_ldq_be_data_ra(env, addr, ra);
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
        helper_raise_illegal_instruction(env);
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

void mmix_cpu_record_program_exception(CPUMMIXState *env, uint64_t causes)
{
    env->program_exception_causes |= causes & MMIX_RQ_PROGRAM_MASK;
    env->sregs[MMIX_SREG_RQ] |= causes & MMIX_RQ_PROGRAM_MASK;
}

void mmix_cpu_raise_dynamic_trap(CPUMMIXState *env, uint64_t causes)
{
    CPUState *cs = env_cpu(env);

    mmix_cpu_record_program_exception(env, causes);
    cs->exception_index = EXCP_MMIX_DYNAMIC_TRAP;
    cpu_loop_exit(cs);
}

static void mmix_cpu_update_translation_state(CPUMMIXState *env)
{
    tlb_flush(env_cpu(env));
}

static void mmix_cpu_put_rk(CPUMMIXState *env, uint64_t val)
{
    env->sregs[MMIX_SREG_RK] = val;
    mmix_cpu_update_translation_state(env);
}

static void mmix_cpu_put_rv(CPUMMIXState *env, uint64_t val)
{
    env->sregs[MMIX_SREG_RV] = val;
    env->flat_translation = false;
    mmix_cpu_update_translation_state(env);
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
    if (reg >= MMIX_SREGS) {
        helper_raise_illegal_instruction(env);
    }
    return env->sregs[reg];
}

static void mmix_cpu_put_rg(CPUMMIXState *env, uint64_t val)
{
    if (val < 32 || val > 255) {
        helper_raise_illegal_instruction(env);
    }
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

void helper_mmix_put_sreg(CPUMMIXState *env, uint32_t reg, uint64_t val)
{
    if (reg >= MMIX_SREGS || mmix_sreg_read_only(reg)) {
        helper_raise_illegal_instruction(env);
    }
    if (mmix_sreg_privileged(reg) && !mmix_cpu_is_privileged(env)) {
        mmix_cpu_raise_dynamic_trap(env, MMIX_RQ_PROGRAM_K);
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
        /*
         * Hardware interrupt request bits are still not modeled. Privileged
         * PUT[I] can write rQ as stored state; program-exception helpers OR
         * architectural cause bits into rQ when they raise dynamic traps.
         */
        env->sregs[reg] = val;
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

    /*
     * Translation caches are not modeled yet. No key can be present, and the
     * requested low-bit protection-code update has no current-machine target.
     */
    return 0;
}

void helper_mmix_push(CPUMMIXState *env, uint32_t x, uint64_t next_pc)
{
    unsigned rg = mmix_cpu_get_rg(env);
    unsigned old_rl = mmix_cpu_get_rl(env);
    unsigned hole = x;
    unsigned pushed;

    if (x >= rg) {
        hole = old_rl;
        mmix_cpu_ensure_local_room(env, old_rl + 1);
        env->local_regs[mmix_cpu_local_index(env, hole)] = old_rl;
        pushed = old_rl + 1;
    } else {
        if (x >= old_rl) {
            mmix_cpu_grow_rl(env, x + 1);
            old_rl = x + 1;
        }
        env->local_regs[mmix_cpu_local_index(env, x)] = x;
        pushed = x + 1;
    }

    env->sregs[MMIX_SREG_RJ] = next_pc;
    env->sregs[MMIX_SREG_RO] += (uint64_t)pushed * 8;
    env->sregs[MMIX_SREG_RL] = old_rl - pushed;
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

void helper_mmix_save(CPUMMIXState *env, uint32_t x)
{
    unsigned rg = mmix_cpu_get_rg(env);
    unsigned old_rl = mmix_cpu_get_rl(env);
    unsigned i;

    if (x < rg) {
        qemu_log_mask(LOG_UNIMP, "MMIX invalid SAVE local destination %u\n",
                      x);
        helper_raise_illegal_instruction(env);
    }

    mmix_cpu_ensure_local_room(env, old_rl);
    env->local_regs[mmix_cpu_local_index(env, old_rl)] = old_rl;
    env->sregs[MMIX_SREG_RO] += (uint64_t)(old_rl + 1) * 8;
    env->sregs[MMIX_SREG_RL] = 0;

    while (env->sregs[MMIX_SREG_RS] != env->sregs[MMIX_SREG_RO]) {
        mmix_cpu_stack_store(env);
    }

    for (i = rg; i < MMIX_REGS; i++) {
        mmix_cpu_stack_write_octa(env, env->regs[i]);
    }
    for (i = 0; i < ARRAY_SIZE(mmix_save_sregs); i++) {
        mmix_cpu_stack_write_octa(env, env->sregs[mmix_save_sregs[i]]);
    }
    mmix_cpu_stack_write_octa(env, ((uint64_t)rg << 56) |
                                   (env->sregs[MMIX_SREG_RA] &
                                    MMIX_RA_VALID_MASK));

    env->sregs[MMIX_SREG_RO] = env->sregs[MMIX_SREG_RS];
    env->regs[x] = env->sregs[MMIX_SREG_RO] - 8;
}

void helper_mmix_unsave(CPUMMIXState *env, uint32_t z)
{
    uint64_t addr = mmix_cpu_read_reg(env, z) & ~7ULL;
    uint64_t packed;
    unsigned rg;
    unsigned saved_rl;
    unsigned i;

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

static void mmix_update_ra_events(CPUMMIXState *env, uint32_t events,
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

uint64_t helper_mmix_add(CPUMMIXState *env, uint32_t insn, uint64_t y,
                         uint64_t z)
{
    uint64_t result = y + z;

    if (((~(y ^ z) & (y ^ result)) >> 63) != 0) {
        mmix_update_ra_events(env, MMIX_RA_EVENT_V, insn, y, z);
    }
    return result;
}

uint64_t helper_mmix_sub(CPUMMIXState *env, uint32_t insn, uint64_t y,
                         uint64_t z)
{
    uint64_t result = y - z;

    if ((((y ^ z) & (y ^ result)) >> 63) != 0) {
        mmix_update_ra_events(env, MMIX_RA_EVENT_V, insn, y, z);
    }
    return result;
}

uint64_t helper_mmix_mul(CPUMMIXState *env, uint32_t insn, uint64_t y,
                         uint64_t z)
{
    __int128 product = (__int128)(int64_t)y * (int64_t)z;

    if (product < (__int128)INT64_MIN || product > (__int128)INT64_MAX) {
        mmix_update_ra_events(env, MMIX_RA_EVENT_V, insn, y, z);
    }
    return product;
}

uint64_t helper_mmix_mulu(CPUMMIXState *env, uint64_t y, uint64_t z)
{
    __uint128_t product = (__uint128_t)y * z;

    env->sregs[MMIX_SREG_RH] = product >> 64;
    return product;
}

static uint64_t mmix_abs_i64(uint64_t val)
{
    return (int64_t)val < 0 ? -val : val;
}

uint64_t helper_mmix_div(CPUMMIXState *env, uint32_t insn, uint64_t y,
                         uint64_t z)
{
    bool y_neg = (int64_t)y < 0;
    bool z_neg = (int64_t)z < 0;
    uint64_t y_abs;
    uint64_t z_abs;
    uint64_t q_abs;
    uint64_t r_abs;
    uint64_t quotient;
    uint64_t remainder;

    if (z == 0) {
        env->sregs[MMIX_SREG_RR] = y;
        mmix_update_ra_events(env, MMIX_RA_EVENT_D, insn, y, z);
        return 0;
    }

    y_abs = mmix_abs_i64(y);
    z_abs = mmix_abs_i64(z);
    q_abs = y_abs / z_abs;
    r_abs = y_abs % z_abs;

    if (y_neg == z_neg) {
        quotient = q_abs;
        remainder = y_neg && r_abs != 0 ? -r_abs : r_abs;
    } else {
        quotient = r_abs != 0 ? -(q_abs + 1) : -q_abs;
        if (r_abs == 0) {
            remainder = 0;
        } else {
            remainder = z_neg ? -(z_abs - r_abs) : z_abs - r_abs;
        }
    }

    env->sregs[MMIX_SREG_RR] = remainder;
    if (y == 0x8000000000000000ULL && z == UINT64_MAX) {
        mmix_update_ra_events(env, MMIX_RA_EVENT_V, insn, y, z);
    }
    return quotient;
}

uint64_t helper_mmix_divu(CPUMMIXState *env, uint64_t y, uint64_t z)
{
    uint64_t high = env->sregs[MMIX_SREG_RD];
    __uint128_t dividend;
    uint64_t quotient;
    uint64_t remainder;

    if (z == 0 || high >= z) {
        env->sregs[MMIX_SREG_RR] = y;
        return high;
    }

    dividend = ((__uint128_t)high << 64) | y;
    quotient = dividend / z;
    remainder = dividend % z;
    env->sregs[MMIX_SREG_RR] = remainder;
    return quotient;
}

uint64_t helper_mmix_sl(CPUMMIXState *env, uint32_t insn, uint64_t y,
                        uint64_t z)
{
    uint64_t result;
    bool overflow;

    if (z >= 64) {
        result = 0;
        overflow = y != 0;
    } else {
        __int128 product = (__int128)(int64_t)y * ((__int128)1 << z);

        result = y << z;
        overflow = product < (__int128)INT64_MIN ||
                   product > (__int128)INT64_MAX;
    }

    if (overflow) {
        mmix_update_ra_events(env, MMIX_RA_EVENT_V, insn, y, z);
    }
    return result;
}

static uint64_t mmix_lane_difference(uint64_t y, uint64_t z,
                                     unsigned lane_bits)
{
    uint64_t mask = lane_bits == 64 ? UINT64_MAX : (1ULL << lane_bits) - 1;
    uint64_t result = 0;
    unsigned shift;

    for (shift = 0; shift < 64; shift += lane_bits) {
        uint64_t y_lane = (y >> shift) & mask;
        uint64_t z_lane = (z >> shift) & mask;

        if (y_lane > z_lane) {
            result |= (y_lane - z_lane) << shift;
        }
    }

    return result;
}

uint64_t helper_mmix_bdif(uint64_t y, uint64_t z)
{
    return mmix_lane_difference(y, z, 8);
}

uint64_t helper_mmix_wdif(uint64_t y, uint64_t z)
{
    return mmix_lane_difference(y, z, 16);
}

uint64_t helper_mmix_tdif(uint64_t y, uint64_t z)
{
    return mmix_lane_difference(y, z, 32);
}

uint64_t helper_mmix_odif(uint64_t y, uint64_t z)
{
    return y > z ? y - z : 0;
}

uint64_t helper_mmix_mux(CPUMMIXState *env, uint64_t y, uint64_t z)
{
    uint64_t mask = env->sregs[MMIX_SREG_RM];

    return (y & mask) | (z & ~mask);
}

static uint8_t mmix_matrix_byte(uint64_t val, unsigned row)
{
    return val >> ((7 - row) * 8);
}

static uint64_t mmix_matrix_multiply(uint64_t y, uint64_t z, bool exclusive)
{
    uint64_t result = 0;
    unsigned i;

    for (i = 0; i < 8; i++) {
        uint8_t z_row = mmix_matrix_byte(z, i);
        uint8_t x_row = 0;
        unsigned j;

        for (j = 0; j < 8; j++) {
            uint8_t bit = 0;
            unsigned k;

            for (k = 0; k < 8; k++) {
                uint8_t y_bit = mmix_matrix_byte(y, k) & (0x80 >> j);
                uint8_t z_bit = z_row & (0x80 >> k);

                if (exclusive) {
                    bit ^= (y_bit && z_bit);
                } else {
                    bit |= (y_bit && z_bit);
                }
            }

            if (bit) {
                x_row |= 0x80 >> j;
            }
        }

        result |= (uint64_t)x_row << ((7 - i) * 8);
    }

    return result;
}

uint64_t helper_mmix_mor(uint64_t y, uint64_t z)
{
    return mmix_matrix_multiply(y, z, false);
}

uint64_t helper_mmix_mxor(uint64_t y, uint64_t z)
{
    return mmix_matrix_multiply(y, z, true);
}

static bool mmix_fp_is_nan(uint64_t val)
{
    return (val & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL;
}

static bool mmix_fp_is_snan(uint64_t val)
{
    uint64_t abs = val & 0x7fffffffffffffffULL;

    return abs > 0x7ff0000000000000ULL && abs < 0x7ff8000000000000ULL;
}

static uint64_t mmix_fp_quiet_nan(uint64_t val)
{
    return val | MMIX_QNAN_BIT;
}

static FloatRoundMode mmix_round_mode_from_ra(CPUMMIXState *env)
{
    switch ((env->sregs[MMIX_SREG_RA] >> MMIX_RA_ROUND_SHIFT) & 3) {
    case 0:
        return float_round_nearest_even;
    case 1:
        return float_round_to_zero;
    case 2:
        return float_round_up;
    case 3:
        return float_round_down;
    default:
        g_assert_not_reached();
    }
}

static bool mmix_round_mode_from_y(uint32_t y, CPUMMIXState *env,
                                   FloatRoundMode *mode)
{
    switch (y) {
    case 0:
        *mode = mmix_round_mode_from_ra(env);
        return true;
    case 1:
        *mode = float_round_to_zero;
        return true;
    case 2:
        *mode = float_round_up;
        return true;
    case 3:
        *mode = float_round_down;
        return true;
    case 4:
        *mode = float_round_nearest_even;
        return true;
    default:
        return false;
    }
}

static uint32_t mmix_events_from_float_flags(FloatExceptionFlags flags)
{
    uint32_t events = 0;

    if (flags & (float_flag_invalid | float_flag_invalid_snan |
                 float_flag_invalid_cvti)) {
        events |= MMIX_RA_EVENT_I;
    }
    if (flags & float_flag_divbyzero) {
        events |= MMIX_RA_EVENT_Z;
    }
    if (flags & float_flag_overflow) {
        events |= MMIX_RA_EVENT_O | MMIX_RA_EVENT_X;
    }
    if (flags & float_flag_underflow) {
        events |= MMIX_RA_EVENT_U | MMIX_RA_EVENT_X;
    }
    if (flags & float_flag_inexact) {
        events |= MMIX_RA_EVENT_X;
    }

    return events;
}

static void mmix_softfloat_status(float_status *status, FloatRoundMode mode)
{
    memset(status, 0, sizeof(*status));
    set_float_rounding_mode(mode, status);
}

static void mmix_update_ra_from_status(CPUMMIXState *env, uint32_t insn,
                                       uint64_t y, uint64_t z,
                                       float_status *status)
{
    mmix_update_ra_events(env,
                          mmix_events_from_float_flags(
                              get_float_exception_flags(status)),
                          insn, y, z);
}

static uint64_t mmix_binary_nan_result(CPUMMIXState *env, uint32_t insn,
                                       uint64_t y, uint64_t z)
{
    if (mmix_fp_is_snan(y) || mmix_fp_is_snan(z)) {
        mmix_update_ra_events(env, MMIX_RA_EVENT_I, insn, y, z);
        y = mmix_fp_quiet_nan(y);
        z = mmix_fp_quiet_nan(z);
    }
    if (mmix_fp_is_nan(z)) {
        return z;
    }
    if (mmix_fp_is_nan(y)) {
        return y;
    }
    return MMIX_DEFAULT_NAN;
}

static uint64_t mmix_unary_nan_result(CPUMMIXState *env, uint32_t insn,
                                      uint32_t y, uint64_t z)
{
    if (mmix_fp_is_snan(z)) {
        mmix_update_ra_events(env, MMIX_RA_EVENT_I, insn, y, z);
        return mmix_fp_quiet_nan(z);
    }
    return z;
}

static uint64_t mmix_fp_compare(CPUMMIXState *env, MMIXFPKind fp,
                                uint32_t insn,
                                uint64_t y, uint64_t z)
{
    float_status status;
    FloatRelation rel;

    switch (fp) {
    case MMIX_FP_FUN:
        return mmix_fp_is_nan(y) || mmix_fp_is_nan(z);
    case MMIX_FP_FEQL:
        if (mmix_fp_is_nan(y) || mmix_fp_is_nan(z)) {
            return 0;
        }
        mmix_softfloat_status(&status, float_round_nearest_even);
        return float64_compare_quiet(y, z, &status) == float_relation_equal;
    case MMIX_FP_FCMP:
        if (mmix_fp_is_nan(y) || mmix_fp_is_nan(z)) {
            mmix_update_ra_events(env, MMIX_RA_EVENT_I, insn, y, z);
            return 0;
        }
        mmix_softfloat_status(&status, float_round_nearest_even);
        rel = float64_compare_quiet(y, z, &status);
        if (rel == float_relation_less) {
            return UINT64_MAX;
        }
        if (rel == float_relation_greater) {
            return 1;
        }
        return 0;
    default:
        g_assert_not_reached();
    }
}

static long double mmix_fp_to_ld(uint64_t val)
{
    union {
        uint64_t u;
        double d;
    } bits;

    bits.u = val;
    return bits.d;
}

static bool mmix_fp_epsilon_radius(long double epsilon, uint64_t val,
                                   long double *radius)
{
    uint64_t abs = val & 0x7fffffffffffffffULL;
    unsigned exp = (abs >> 52) & 0x7ff;

    if (abs == 0) {
        *radius = 0.0L;
    } else if (exp == 0) {
        *radius = ldexpl(epsilon, -1021);
    } else if (exp == 0x7ff) {
        return false;
    } else {
        *radius = ldexpl(epsilon, (int)exp - 1022);
    }
    return true;
}

static uint64_t mmix_fp_epsilon_compare(CPUMMIXState *env, MMIXFPKind fp,
                                        uint32_t insn, uint64_t y,
                                        uint64_t z)
{
    uint64_t e = env->sregs[MMIX_SREG_RE];
    long double yd, zd, ed, yradius, zradius, diff;
    bool yinf, zinf;

    if (mmix_fp_is_nan(y) || mmix_fp_is_nan(z) || mmix_fp_is_nan(e) ||
        (e >> 63)) {
        if (fp == MMIX_FP_FUNE) {
            return 1;
        }
        mmix_update_ra_events(env, MMIX_RA_EVENT_I, insn, y, z);
        return 0;
    }

    if (fp == MMIX_FP_FUNE) {
        return 0;
    }

    yinf = (y & 0x7fffffffffffffffULL) == 0x7ff0000000000000ULL;
    zinf = (z & 0x7fffffffffffffffULL) == 0x7ff0000000000000ULL;
    ed = mmix_fp_to_ld(e);
    yd = mmix_fp_to_ld(y);
    zd = mmix_fp_to_ld(z);

    if (yinf || zinf) {
        if (e < 0x3ff0000000000000ULL) {
            diff = (y == z) ? 0.0L : INFINITY;
        } else if (e < 0x4000000000000000ULL) {
            diff = ((y ^ z) == 0xfff0000000000000ULL) ? INFINITY : 0.0L;
        } else {
            diff = 0.0L;
        }
        if (fp == MMIX_FP_FEQLE) {
            return diff == 0.0L;
        }
        if (diff == 0.0L) {
            return 0;
        }
        return yd < zd ? UINT64_MAX : 1;
    }

    if (!mmix_fp_epsilon_radius(ed, y, &yradius) ||
        !mmix_fp_epsilon_radius(ed, z, &zradius)) {
        mmix_update_ra_events(env, MMIX_RA_EVENT_I, insn, y, z);
        return 0;
    }

    diff = fabsl(yd - zd);
    if (fp == MMIX_FP_FEQLE) {
        return diff <= yradius && diff <= zradius;
    }
    if (yd < zd && yd + yradius < zd && y < z && yd < zd - zradius) {
        return UINT64_MAX;
    }
    if (yd > zd && yd - yradius > zd && y > z && yd > zd + zradius) {
        return 1;
    }
    return 0;
}

uint64_t helper_mmix_fp_binary(CPUMMIXState *env, uint32_t selector,
                               uint32_t insn, uint64_t y, uint64_t z)
{
    MMIXFPKind fp = (MMIXFPKind)selector;
    float_status status;
    float64 result;

    switch (fp) {
    case MMIX_FP_FCMP:
    case MMIX_FP_FUN:
    case MMIX_FP_FEQL:
        return mmix_fp_compare(env, fp, insn, y, z);
    case MMIX_FP_FCMPE:
    case MMIX_FP_FUNE:
    case MMIX_FP_FEQLE:
        return mmix_fp_epsilon_compare(env, fp, insn, y, z);
    default:
        break;
    }

    if (mmix_fp_is_nan(y) || mmix_fp_is_nan(z)) {
        return mmix_binary_nan_result(env, insn, y, z);
    }

    mmix_softfloat_status(&status, mmix_round_mode_from_ra(env));
    switch (fp) {
    case MMIX_FP_FADD:
        result = float64_add(y, z, &status);
        break;
    case MMIX_FP_FSUB:
        result = float64_sub(y, z, &status);
        break;
    case MMIX_FP_FMUL:
        result = float64_mul(y, z, &status);
        break;
    case MMIX_FP_FDIV:
        result = float64_div(y, z, &status);
        break;
    case MMIX_FP_FREM:
        result = float64_rem(y, z, &status);
        break;
    default:
        g_assert_not_reached();
    }

    mmix_update_ra_from_status(env, insn, y, z, &status);
    return result;
}

uint64_t helper_mmix_fp_unary(CPUMMIXState *env, uint32_t selector,
                              uint32_t insn, uint32_t y, uint64_t z)
{
    MMIXFPKind fp = (MMIXFPKind)selector;
    FloatRoundMode mode;
    float_status status;
    float64 result;

    if (!mmix_round_mode_from_y(y, env, &mode)) {
        helper_raise_illegal_instruction(env);
    }
    if (mmix_fp_is_nan(z)) {
        return mmix_unary_nan_result(env, insn, y, z);
    }

    mmix_softfloat_status(&status, mode);
    switch (fp) {
    case MMIX_FP_FSQRT:
        result = float64_sqrt(z, &status);
        break;
    case MMIX_FP_FINT:
        result = float64_round_to_int(z, &status);
        break;
    default:
        g_assert_not_reached();
    }

    mmix_update_ra_from_status(env, insn, y, z, &status);
    return result;
}

uint64_t helper_mmix_fp_fix(CPUMMIXState *env, uint32_t selector,
                            uint32_t insn, uint32_t y, uint64_t z)
{
    MMIXFPKind fp = (MMIXFPKind)selector;
    FloatRoundMode mode;
    float_status status;
    FloatExceptionFlags flags;
    int64_t result;

    if (!mmix_round_mode_from_y(y, env, &mode)) {
        helper_raise_illegal_instruction(env);
    }
    if (mmix_fp_is_nan(z) || float64_is_infinity(z)) {
        mmix_update_ra_events(env, MMIX_RA_EVENT_I, insn, y, z);
        return z;
    }

    mmix_softfloat_status(&status, mode);
    result = float64_to_int64_modulo(z, mode, &status);
    flags = get_float_exception_flags(&status);
    if (fp == MMIX_FP_FIX) {
        long double d = mmix_fp_to_ld(z);

        if (d < -0x1p63L || d >= 0x1p63L) {
            mmix_update_ra_events(env, MMIX_RA_EVENT_W, insn, y, z);
        }
    }
    mmix_update_ra_events(env, mmix_events_from_float_flags(flags) &
                          ~MMIX_RA_EVENT_I, insn, y, z);
    return result;
}

uint64_t helper_mmix_fp_float(CPUMMIXState *env, uint32_t selector,
                              uint32_t insn, uint32_t y, uint64_t z)
{
    MMIXFPKind fp = (MMIXFPKind)selector;
    FloatRoundMode mode;
    float_status status;
    float64 result;

    if (!mmix_round_mode_from_y(y, env, &mode)) {
        helper_raise_illegal_instruction(env);
    }
    mmix_softfloat_status(&status, mode);

    switch (fp) {
    case MMIX_FP_FLOT:
        result = int64_to_float64(z, &status);
        break;
    case MMIX_FP_FLOTU:
        result = uint64_to_float64(z, &status);
        break;
    case MMIX_FP_SFLOT:
        result = float32_to_float64(int64_to_float32(z, &status), &status);
        break;
    case MMIX_FP_SFLOTU:
        result = float32_to_float64(uint64_to_float32(z, &status), &status);
        break;
    default:
        g_assert_not_reached();
    }

    mmix_update_ra_from_status(env, insn, y, z, &status);
    return result;
}

uint64_t helper_mmix_ldsf(uint64_t raw)
{
    float_status status;

    mmix_softfloat_status(&status, float_round_nearest_even);
    return float32_to_float64(raw, &status);
}

uint64_t helper_mmix_stsf(CPUMMIXState *env, uint32_t insn, uint64_t addr,
                          uint64_t val)
{
    float_status status;
    float32 result;

    if (mmix_fp_is_nan(val)) {
        if (mmix_fp_is_snan(val)) {
            mmix_update_ra_events(env, MMIX_RA_EVENT_I, insn, addr, val);
            val = mmix_fp_quiet_nan(val);
        }
    }

    mmix_softfloat_status(&status, mmix_round_mode_from_ra(env));
    result = float64_to_float32(val, &status);
    mmix_update_ra_from_status(env, insn, addr, val, &status);
    return result;
}

void helper_raise_illegal_instruction(CPUMMIXState *env)
{
    CPUState *cs = env_cpu(env);

    cs->exception_index = EXCP_MMIX_ILLEGAL;
    cpu_loop_exit(cs);
}

void helper_mmix_trip(CPUMMIXState *env, uint32_t insn, uint64_t y,
                      uint64_t z)
{
    CPUState *cs = env_cpu(env);

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
    helper_raise_illegal_instruction(env);
}

static void mmix_resume_state(CPUMMIXState *env, bool trap_state)
{
    CPUState *cs = env_cpu(env);
    uint64_t where;
    uint64_t exec;
    uint64_t y;
    uint64_t z;
    uint8_t ropcode;
    uint32_t insn;

    if (trap_state) {
        where = env->sregs[MMIX_SREG_RWW];
        exec = env->sregs[MMIX_SREG_RXX];
        y = env->sregs[MMIX_SREG_RYY];
        z = env->sregs[MMIX_SREG_RZZ];
        mmix_cpu_put_rk(env, mmix_cpu_read_reg(env, 255));
        mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RBB]);
    } else {
        where = env->sregs[MMIX_SREG_RW];
        exec = env->sregs[MMIX_SREG_RX];
        y = env->sregs[MMIX_SREG_RY];
        z = env->sregs[MMIX_SREG_RZ];
    }

    if ((int64_t)exec < 0) {
        env->pc = where;
        env->npc = where + 4;
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
            mmix_update_ra_events(env, events, insn, y, z);
        }
        env->pc = where;
        env->npc = where + 4;
        cpu_loop_exit_noexc(cs);
    }
    case 0:
        mmix_resume_unsupported(env, "ropcode 0 instruction replay", exec);
        break;
    case 1:
        mmix_resume_unsupported(env, "ropcode 1 operand substitution", exec);
        break;
    case 3:
        mmix_resume_unsupported(env, "ropcode 3 virtual translation", exec);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "MMIX invalid RESUME ropcode %u exec=0x%016" PRIx64
                      "\n",
                      ropcode, exec);
        helper_raise_illegal_instruction(env);
        break;
    }
    g_assert_not_reached();
}

void helper_mmix_resume(CPUMMIXState *env, uint32_t x, uint32_t y, uint32_t z)
{
    if (x != 0 || y != 0) {
        qemu_log_mask(LOG_UNIMP, "MMIX invalid RESUME x=%u y=%u z=%u\n",
                      x, y, z);
        helper_raise_illegal_instruction(env);
    }

    switch (z) {
    case 0:
        mmix_resume_state(env, false);
        break;
    case 1:
        mmix_resume_state(env, true);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "MMIX invalid RESUME z=%u\n", z);
        helper_raise_illegal_instruction(env);
        break;
    }
    g_assert_not_reached();
}

static G_NORETURN void mmix_shutdown_with_cpu_log(CPUMMIXState *env,
                                                  const char *reason)
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
                                               0);
    }
    cs->halted = 1;
    cpu_loop_exit_noexc(cs);
}

void helper_mmix_test_exit(CPUMMIXState *env)
{
    mmix_shutdown_with_cpu_log(env, "MMIX test exit");
}

static MMIXSemihostingCall mmix_semihosting_decode_call(uint32_t service,
                                                        uint32_t handle)
{
    MMIXSemihostingCall call = {
        .action = MMIX_SEMIHOSTING_ACTION_UNSUPPORTED,
        .service = service,
        .handle = handle,
        .requires_semihosting = true,
    };

    switch (service) {
    case MMIX_SEMIHOSTING_SERVICE_HALT:
        if (handle == 0) {
            call.action = MMIX_SEMIHOSTING_ACTION_HALT;
            call.requires_semihosting = false;
        }
        break;
    case MMIX_SEMIHOSTING_SERVICE_FPUTS:
        if (handle == MMIX_SEMIHOSTING_HANDLE_STDOUT) {
            call.action = MMIX_SEMIHOSTING_ACTION_FPUTS_STDOUT;
        } else {
            call.action = MMIX_SEMIHOSTING_ACTION_FPUTS_BAD_HANDLE;
        }
        break;
    default:
        break;
    }

    return call;
}

static bool mmix_semihosting_read_byte(CPUMMIXState *env, uint64_t address,
                                       uint8_t *byte)
{
    CPUState *cs = env_cpu(env);
    MMIXAddressTranslation translation;
    MemTxResult result;

    if (!mmix_translate_address(env, address, MMU_DATA_LOAD, true, false,
                                &translation)) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted Fputs invalid string address 0x%016"
                      PRIx64 "\n",
                      address);
        return false;
    }

    *byte = address_space_ldub(cs->as, translation.physical,
                               MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted Fputs could not read string address "
                      "0x%016" PRIx64 "\n",
                      address);
        return false;
    }
    return true;
}

static bool mmix_semihosting_read_cstring(CPUMMIXState *env, uint64_t address,
                                          GByteArray *bytes)
{
    uint8_t byte;
    uint64_t current;
    size_t i;

    for (i = 0; i < MMIX_SEMIHOSTING_STRING_MAX; i++) {
        current = address + i;
        if (current < address) {
            qemu_log_mask(LOG_UNIMP,
                          "MMIX hosted Fputs invalid string address 0x%016"
                          PRIx64 "\n",
                          current);
            return false;
        }
        if (!mmix_semihosting_read_byte(env, current, &byte)) {
            return false;
        }
        if (byte == 0) {
            return true;
        }
        g_byte_array_append(bytes, &byte, 1);
    }

    qemu_log_mask(LOG_UNIMP,
                  "MMIX hosted Fputs string at 0x%016" PRIx64
                  " exceeds %u bytes without NUL\n",
                  address, MMIX_SEMIHOSTING_STRING_MAX);
    return false;
}

static bool mmix_semihosting_write_stdout(CPUMMIXState *env,
                                          const GByteArray *bytes)
{
    CPUState *cs = env_cpu(env);
    MemTxResult result;
    size_t i;

    for (i = 0; i < bytes->len; i++) {
        address_space_stb(cs->as, MMIX_SEMIHOSTING_STDOUT_TX, bytes->data[i],
                          MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_UNIMP,
                          "MMIX hosted Fputs could not write StdOut at "
                          "0x%016" PRIx64 "\n",
                          env->pc);
            return false;
        }
    }

    return true;
}

static void mmix_semihosting_fputs_stdout(CPUMMIXState *env)
{
    GByteArray *bytes = g_byte_array_new();
    uint64_t address = mmix_cpu_read_reg(env, 255);

    if (!mmix_semihosting_read_cstring(env, address, bytes)) {
        g_byte_array_free(bytes, true);
        helper_raise_illegal_instruction(env);
    }

    if (!mmix_semihosting_write_stdout(env, bytes)) {
        g_byte_array_free(bytes, true);
        helper_raise_illegal_instruction(env);
    }

    g_byte_array_free(bytes, true);
}

void helper_mmix_semihosting_trap(CPUMMIXState *env, uint32_t service,
                                  uint32_t handle)
{
    MMIXSemihostingCall call = mmix_semihosting_decode_call(service, handle);

    switch (call.action) {
    case MMIX_SEMIHOSTING_ACTION_HALT:
        mmix_shutdown_with_cpu_log(env, "MMIX hosted Halt");
        break;
    case MMIX_SEMIHOSTING_ACTION_FPUTS_STDOUT:
        mmix_semihosting_fputs_stdout(env);
        return;
    case MMIX_SEMIHOSTING_ACTION_FPUTS_BAD_HANDLE:
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted Fputs unsupported handle %u at 0x%016"
                      PRIx64 "\n",
                      call.handle, env->pc);
        helper_raise_illegal_instruction(env);
        break;
    case MMIX_SEMIHOSTING_ACTION_UNSUPPORTED:
        qemu_log_mask(LOG_UNIMP,
                      "MMIX unsupported hosted TRAP service %u handle %u at "
                      "0x%016" PRIx64 "\n",
                      call.service, call.handle, env->pc);
        helper_raise_illegal_instruction(env);
        break;
    default:
        g_assert_not_reached();
    }
}

void mmix_cpu_do_interrupt(CPUState *cs)
{
    CPUMMIXState *env = cpu_env(cs);
    uint32_t event;
    uint64_t causes;
    hwaddr handler;

    switch (cs->exception_index) {
    case EXCP_MMIX_ILLEGAL:
        qemu_log_mask(CPU_LOG_INT, "MMIX illegal instruction at 0x%016" PRIx64 "\n",
                      env->pc);
        break;
    case EXCP_MMIX_ARITHMETIC_TRIP:
        event = env->arithmetic_trip_event;
        handler = mmix_arithmetic_trip_handler(event);
        qemu_log_mask(CPU_LOG_INT,
                      "MMIX arithmetic trip event=0x%02x from 0x%016" PRIx64
                      " to 0x%016" HWADDR_PRIx "\n",
                      event, env->pc, handler);
        env->sregs[MMIX_SREG_RB] = mmix_cpu_read_reg(env, 255);
        mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RJ]);
        env->pc = handler;
        env->npc = handler + 4;
        env->arithmetic_trip_event = 0;
        cs->exception_index = -1;
        break;
    case EXCP_MMIX_DYNAMIC_TRAP:
        causes = env->program_exception_causes & MMIX_RQ_PROGRAM_MASK;
        handler = env->sregs[MMIX_SREG_RTT];
        qemu_log_mask(CPU_LOG_INT,
                      "MMIX dynamic trap causes=0x%016" PRIx64
                      " from 0x%016" PRIx64 " to 0x%016" HWADDR_PRIx "\n",
                      causes, env->pc, handler);
        env->sregs[MMIX_SREG_RBB] = mmix_cpu_read_reg(env, 255);
        env->sregs[MMIX_SREG_RWW] = env->npc;
        env->sregs[MMIX_SREG_RXX] = causes;
        env->sregs[MMIX_SREG_RYY] = 0;
        env->sregs[MMIX_SREG_RZZ] = 0;
        mmix_cpu_put_rk(env, 0);
        mmix_cpu_write_reg(env, 255, env->sregs[MMIX_SREG_RJ]);
        env->pc = handler;
        env->npc = handler + 4;
        env->program_exception_causes = 0;
        cs->exception_index = -1;
        break;
    default:
        qemu_log_mask(CPU_LOG_INT, "MMIX exception %d at 0x%016" PRIx64 "\n",
                      cs->exception_index, env->pc);
        break;
    }
}

bool mmix_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        cs->exception_index = EXCP_MMIX_INTERRUPT;
        mmix_cpu_do_interrupt(cs);
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        return true;
    }

    return false;
}

hwaddr mmix_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    CPUMMIXState *env = cpu_env(cs);
    MMIXAddressTranslation translation;

    if (mmix_translate_address(env, addr, MMU_DATA_LOAD, true, false,
                               &translation)) {
        return translation.physical;
    }
    return -1;
}
