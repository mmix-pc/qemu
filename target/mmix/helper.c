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
#include "fpu/softfloat.h"
#include "system/runstate.h"
#include <math.h>

#define MMIX_QNAN_BIT      0x0008000000000000ULL
#define MMIX_DEFAULT_NAN   0x7ff8000000000000ULL

typedef enum MMIXFPOp {
    MMIX_FP_FCMP,
    MMIX_FP_FUN,
    MMIX_FP_FEQL,
    MMIX_FP_FADD,
    MMIX_FP_FSUB,
    MMIX_FP_FMUL,
    MMIX_FP_FDIV,
    MMIX_FP_FREM,
    MMIX_FP_FCMPE,
    MMIX_FP_FUNE,
    MMIX_FP_FEQLE,
    MMIX_FP_FSQRT,
    MMIX_FP_FINT,
    MMIX_FP_FIX,
    MMIX_FP_FIXU,
    MMIX_FP_FLOT,
    MMIX_FP_FLOTU,
    MMIX_FP_SFLOT,
    MMIX_FP_SFLOTU,
} MMIXFPOp;

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

void helper_mmix_put_ra(CPUMMIXState *env, uint64_t val)
{
    env->sregs[MMIX_SREG_RA] = val & MMIX_RA_VALID_MASK;
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

static uint64_t mmix_fp_compare(CPUMMIXState *env, MMIXFPOp op, uint32_t insn,
                                uint64_t y, uint64_t z)
{
    float_status status;
    FloatRelation rel;

    switch (op) {
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

static uint64_t mmix_fp_epsilon_compare(CPUMMIXState *env, MMIXFPOp op,
                                        uint32_t insn, uint64_t y,
                                        uint64_t z)
{
    uint64_t e = env->sregs[MMIX_SREG_RE];
    long double yd, zd, ed, yradius, zradius, diff;
    bool yinf, zinf;

    if (mmix_fp_is_nan(y) || mmix_fp_is_nan(z) || mmix_fp_is_nan(e) ||
        (e >> 63)) {
        if (op == MMIX_FP_FUNE) {
            return 1;
        }
        mmix_update_ra_events(env, MMIX_RA_EVENT_I, insn, y, z);
        return 0;
    }

    if (op == MMIX_FP_FUNE) {
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
        if (op == MMIX_FP_FEQLE) {
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
    if (op == MMIX_FP_FEQLE) {
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

uint64_t helper_mmix_fp_binary(CPUMMIXState *env, uint32_t op, uint32_t insn,
                               uint64_t y, uint64_t z)
{
    float_status status;
    float64 result;

    switch (op) {
    case MMIX_FP_FCMP:
    case MMIX_FP_FUN:
    case MMIX_FP_FEQL:
        return mmix_fp_compare(env, op, insn, y, z);
    case MMIX_FP_FCMPE:
    case MMIX_FP_FUNE:
    case MMIX_FP_FEQLE:
        return mmix_fp_epsilon_compare(env, op, insn, y, z);
    default:
        break;
    }

    if (mmix_fp_is_nan(y) || mmix_fp_is_nan(z)) {
        return mmix_binary_nan_result(env, insn, y, z);
    }

    mmix_softfloat_status(&status, mmix_round_mode_from_ra(env));
    switch (op) {
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

uint64_t helper_mmix_fp_unary(CPUMMIXState *env, uint32_t op, uint32_t insn,
                              uint32_t y, uint64_t z)
{
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
    switch (op) {
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

uint64_t helper_mmix_fp_fix(CPUMMIXState *env, uint32_t op, uint32_t insn,
                            uint32_t y, uint64_t z)
{
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
    if (op == MMIX_FP_FIX) {
        long double d = mmix_fp_to_ld(z);

        if (d < -0x1p63L || d >= 0x1p63L) {
            mmix_update_ra_events(env, MMIX_RA_EVENT_W, insn, y, z);
        }
    }
    mmix_update_ra_events(env, mmix_events_from_float_flags(flags) &
                          ~MMIX_RA_EVENT_I, insn, y, z);
    return result;
}

uint64_t helper_mmix_fp_float(CPUMMIXState *env, uint32_t op, uint32_t insn,
                              uint32_t y, uint64_t z)
{
    FloatRoundMode mode;
    float_status status;
    float64 result;

    if (!mmix_round_mode_from_y(y, env, &mode)) {
        helper_raise_illegal_instruction(env);
    }
    mmix_softfloat_status(&status, mode);

    switch (op) {
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
    env->sregs[MMIX_SREG_RK] = 0;
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
        env->sregs[MMIX_SREG_RK] = mmix_cpu_read_reg(env, 255);
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
    uint32_t event;
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
