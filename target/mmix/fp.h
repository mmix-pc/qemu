/*
 * MMIX floating-point helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MMIX_FP_H
#define MMIX_FP_H

/*
 * Floating-point helper selectors. These identify shared helper semantics,
 * not architectural opcode values.
 */
typedef enum MMIXFPKind {
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
} MMIXFPKind;

#endif
