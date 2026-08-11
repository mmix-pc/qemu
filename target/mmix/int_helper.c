/*
 * QEMU MMIX integer helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "mmix-helper.h"
#include "exec/helper-proto.h"

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
