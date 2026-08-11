/*
 * QEMU MMIX floating-point helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "fp.h"
#include "mmix-helper.h"
#include "exec/helper-proto.h"
#include "fpu/softfloat.h"
#include <math.h>

#define MMIX_QNAN_BIT      0x0008000000000000ULL
#define MMIX_DEFAULT_NAN   0x7ff8000000000000ULL
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
