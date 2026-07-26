/*
 * QEMU MMIX helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/log.h"
#include "exec/helper-proto.h"
#include "system/runstate.h"

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

static void mmix_cpu_grow_rl(CPUMMIXState *env, unsigned new_rl)
{
    unsigned old_rl = mmix_cpu_get_rl(env);
    unsigned rg = mmix_cpu_get_rg(env);
    unsigned i;

    new_rl = MIN(new_rl, rg);
    if (new_rl <= old_rl) {
        return;
    }
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

uint64_t helper_mmix_read_reg(CPUMMIXState *env, uint32_t reg)
{
    return mmix_cpu_read_reg(env, reg);
}

void helper_mmix_write_reg(CPUMMIXState *env, uint32_t reg, uint64_t val)
{
    mmix_cpu_write_reg(env, reg, val);
}

void helper_mmix_put_rl(CPUMMIXState *env, uint64_t val)
{
    mmix_cpu_put_rl(env, val);
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

void helper_raise_illegal_instruction(CPUMMIXState *env)
{
    CPUState *cs = env_cpu(env);

    cs->exception_index = EXCP_MMIX_ILLEGAL;
    cpu_loop_exit(cs);
}

void helper_mmix_test_exit(CPUMMIXState *env)
{
    CPUState *cs = env_cpu(env);

    if (!env->test_exit_seen) {
        env->test_exit_seen = true;
        qemu_log_mask(CPU_LOG_INT, "MMIX test exit at 0x%016" PRIx64 "\n",
                      env->pc);
        log_cpu_state_mask(CPU_LOG_INT, cs, 0);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN,
                                               0);
    }
    cs->halted = 1;
    cpu_loop_exit_noexc(cs);
}

void mmix_cpu_do_interrupt(CPUState *cs)
{
    CPUMMIXState *env = cpu_env(cs);

    switch (cs->exception_index) {
    case EXCP_MMIX_ILLEGAL:
        qemu_log_mask(CPU_LOG_INT, "MMIX illegal instruction at 0x%016" PRIx64 "\n",
                      env->pc);
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
    return addr;
}
