/*
 * QEMU MMIX TCG translation
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "fp.h"
#include "qemu/log.h"
#include "tcg/tcg-op.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/target_page.h"
#include "exec/translator.h"
#include "exec/translation-block.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

typedef struct DisasContext {
    DisasContextBase base;
    CPUMMIXState *env;
    vaddr insn_pc;
    uint32_t insn;
    TCGv_i64 replay_y;
    TCGv_i64 replay_z;
    bool replay;
    bool substitute_operands;
    bool hosted_memory;
} DisasContext;

typedef enum MMIXALUKind {
    MMIX_ALU_ADD,
    MMIX_ALU_SUB,
    MMIX_ALU_OR,
    MMIX_ALU_ORN,
    MMIX_ALU_NOR,
    MMIX_ALU_XOR,
    MMIX_ALU_AND,
    MMIX_ALU_ANDN,
    MMIX_ALU_NAND,
    MMIX_ALU_NXOR,
} MMIXALUKind;

typedef enum MMIXCompareKind {
    MMIX_CMP_SIGNED,
    MMIX_CMP_UNSIGNED,
} MMIXCompareKind;

typedef enum MMIXShiftKind {
    MMIX_SHIFT_SL,
    MMIX_SHIFT_SLU,
    MMIX_SHIFT_SR,
    MMIX_SHIFT_SRU,
} MMIXShiftKind;

typedef enum MMIXPredicateKind {
    MMIX_PRED_NEGATIVE,
    MMIX_PRED_ZERO,
    MMIX_PRED_POSITIVE,
    MMIX_PRED_ODD,
    MMIX_PRED_NONNEGATIVE,
    MMIX_PRED_NONZERO,
    MMIX_PRED_NONPOSITIVE,
    MMIX_PRED_EVEN,
} MMIXPredicateKind;

static TCGv_i64 cpu_pc;
static TCGv_i64 cpu_npc;

/* Include the auto-generated decoder. */
#include "decode-insns.c.inc"

#define TRANS_GEN1(NAME, GEN, A1) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return GEN(ctx, a, A1); \
    }

#define TRANS_GEN2(NAME, GEN, A1, A2) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return GEN(ctx, a, A1, A2); \
    }

#define TRANS_GEN3(NAME, GEN, A1, A2, A3) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return GEN(ctx, a, A1, A2, A3); \
    }

#define TRANS_NOP(NAME) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return true; \
    }

static void gen_consume_insn_replay(DisasContext *ctx)
{
    if (ctx->replay) {
        gen_helper_mmix_consume_insn_replay(tcg_env);
    }
}

static void gen_goto_tb(DisasContext *ctx, unsigned tb_slot_idx, vaddr dest)
{
    gen_consume_insn_replay(ctx);
    if (!ctx->replay && translator_use_goto_tb(&ctx->base, dest)) {
        tcg_gen_goto_tb(tb_slot_idx);
        tcg_gen_movi_i64(cpu_pc, dest);
        tcg_gen_movi_i64(cpu_npc, dest + 4);
        tcg_gen_exit_tb(ctx->base.tb, tb_slot_idx);
    } else {
        tcg_gen_movi_i64(cpu_pc, dest);
        tcg_gen_movi_i64(cpu_npc, dest + 4);
        tcg_gen_lookup_and_goto_ptr();
    }
}

static TCGv_i64 gen_load_reg(unsigned reg)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_read_reg(val, tcg_env, tcg_constant_i32(reg));
    return val;
}

static TCGv_i64 gen_load_z(const arg_xyz *a, bool immediate)
{
    if (immediate) {
        return tcg_constant_i64(a->z);
    }
    return gen_load_reg(a->z);
}

static TCGv_i64 gen_load_replay_y(DisasContext *ctx, const arg_xyz *a)
{
    return ctx->substitute_operands ? ctx->replay_y : gen_load_reg(a->y);
}

static TCGv_i64 gen_load_replay_y_immediate(DisasContext *ctx,
                                            const arg_xyz *a)
{
    return ctx->substitute_operands ? ctx->replay_y :
           tcg_constant_i64(a->y);
}

static TCGv_i64 gen_load_replay_z(DisasContext *ctx, const arg_xyz *a,
                                  bool immediate)
{
    return ctx->substitute_operands ? ctx->replay_z :
           gen_load_z(a, immediate);
}

static void gen_store_reg(unsigned reg, TCGv_i64 val)
{
    gen_helper_mmix_write_reg(tcg_env, tcg_constant_i32(reg), val);
}

static void gen_effective_address(TCGv_i64 addr, const arg_xyz *a,
                                  bool immediate, uint64_t align_mask)
{
    tcg_gen_add_i64(addr, gen_load_reg(a->y), gen_load_z(a, immediate));
    if (align_mask != 0) {
        tcg_gen_andi_i64(addr, addr, ~align_mask);
    }
}

static vaddr mmix_branch_dest(DisasContext *ctx, const arg_xyz *a,
                              bool backward)
{
    int32_t disp = backward ? (int32_t)a->yz - 0x10000 : a->yz;

    return ctx->insn_pc + ((int64_t)disp << 2);
}

static vaddr mmix_jump_dest(DisasContext *ctx, const arg_xyz *a,
                            bool backward)
{
    int32_t disp = backward ? (int32_t)a->xyz - 0x1000000 : a->xyz;

    return ctx->insn_pc + ((int64_t)disp << 2);
}

static bool gen_mmix_break_rules(DisasContext *ctx, const arg_xyz *a,
                                 bool immediate)
{
    gen_helper_mmix_break_rules(tcg_env, tcg_constant_i32(ctx->insn),
                                gen_load_reg(a->y),
                                gen_load_z(a, immediate));
    return true;
}

static bool trans_TRIP(DisasContext *ctx, arg_xyz *a)
{
    gen_helper_mmix_trip(tcg_env, tcg_constant_i32(ctx->insn),
                         gen_load_reg(a->y), gen_load_reg(a->z));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_RESUME(DisasContext *ctx, arg_xyz *a)
{
    if (ctx->replay || a->x != 0 || a->y != 0 || a->z > 1) {
        return gen_mmix_break_rules(ctx, a, false);
    }

    gen_helper_mmix_resume(tcg_env, tcg_constant_i32(ctx->insn),
                           tcg_constant_i32(a->z));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_TRAP(DisasContext *ctx, arg_xyz *a)
{
    if (a->x == 0) {
        gen_helper_mmix_semihosting_trap(tcg_env, tcg_constant_i32(a->y),
                                          tcg_constant_i32(a->z));
        return true;
    }

    gen_helper_mmix_trap(tcg_env, tcg_constant_i32(ctx->insn),
                         gen_load_reg(a->y), gen_load_reg(a->z));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

TRANS_NOP(SWYM)

static bool gen_mmix_invalid_sync(DisasContext *ctx, arg_xyz *a)
{
    return gen_mmix_break_rules(ctx, a, false);
}

static bool gen_mmix_privileged_sync(DisasContext *ctx, uint32_t mode)
{
    gen_helper_mmix_sync(tcg_env, tcg_constant_i32(ctx->insn),
                         tcg_constant_i32(mode));
    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
    return true;
}

static bool gen_fp_binary(DisasContext *ctx, arg_xyz *a, MMIXFPKind fp)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_fp_binary(val, tcg_env, tcg_constant_i32(fp),
                              tcg_constant_i32(ctx->insn),
                              gen_load_replay_y(ctx, a),
                              gen_load_replay_z(ctx, a, false));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_fp_unary(DisasContext *ctx, arg_xyz *a, MMIXFPKind fp)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_fp_unary(val, tcg_env, tcg_constant_i32(fp),
                             tcg_constant_i32(ctx->insn),
                             gen_load_replay_y_immediate(ctx, a),
                             gen_load_replay_z(ctx, a, false));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_fp_fix(DisasContext *ctx, arg_xyz *a, MMIXFPKind fp)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_fp_fix(val, tcg_env, tcg_constant_i32(fp),
                           tcg_constant_i32(ctx->insn),
                           gen_load_replay_y_immediate(ctx, a),
                           gen_load_replay_z(ctx, a, false));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_fp_float(DisasContext *ctx, arg_xyz *a, MMIXFPKind fp,
                         bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_fp_float(val, tcg_env, tcg_constant_i32(fp),
                             tcg_constant_i32(ctx->insn),
                             gen_load_replay_y_immediate(ctx, a),
                             gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

TRANS_GEN1(FCMP, gen_fp_binary, MMIX_FP_FCMP)
TRANS_GEN1(FUN, gen_fp_binary, MMIX_FP_FUN)
TRANS_GEN1(FEQL, gen_fp_binary, MMIX_FP_FEQL)
TRANS_GEN1(FADD, gen_fp_binary, MMIX_FP_FADD)
TRANS_GEN1(FSUB, gen_fp_binary, MMIX_FP_FSUB)
TRANS_GEN1(FMUL, gen_fp_binary, MMIX_FP_FMUL)
TRANS_GEN1(FDIV, gen_fp_binary, MMIX_FP_FDIV)
TRANS_GEN1(FREM, gen_fp_binary, MMIX_FP_FREM)
TRANS_GEN1(FCMPE, gen_fp_binary, MMIX_FP_FCMPE)
TRANS_GEN1(FUNE, gen_fp_binary, MMIX_FP_FUNE)
TRANS_GEN1(FEQLE, gen_fp_binary, MMIX_FP_FEQLE)
TRANS_GEN1(FSQRT, gen_fp_unary, MMIX_FP_FSQRT)
TRANS_GEN1(FINT, gen_fp_unary, MMIX_FP_FINT)
TRANS_GEN1(FIX, gen_fp_fix, MMIX_FP_FIX)
TRANS_GEN1(FIXU, gen_fp_fix, MMIX_FP_FIXU)
TRANS_GEN2(FLOT, gen_fp_float, MMIX_FP_FLOT, false)
TRANS_GEN2(FLOTI, gen_fp_float, MMIX_FP_FLOT, true)
TRANS_GEN2(FLOTU, gen_fp_float, MMIX_FP_FLOTU, false)
TRANS_GEN2(FLOTUI, gen_fp_float, MMIX_FP_FLOTU, true)
TRANS_GEN2(SFLOT, gen_fp_float, MMIX_FP_SFLOT, false)
TRANS_GEN2(SFLOTI, gen_fp_float, MMIX_FP_SFLOT, true)
TRANS_GEN2(SFLOTU, gen_fp_float, MMIX_FP_SFLOTU, false)
TRANS_GEN2(SFLOTUI, gen_fp_float, MMIX_FP_SFLOTU, true)

static bool gen_alu(DisasContext *ctx, arg_xyz *a, MMIXALUKind kind,
                    bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    switch (kind) {
    case MMIX_ALU_ADD:
        tcg_gen_add_i64(val, gen_load_replay_y(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
        break;
    case MMIX_ALU_SUB:
        tcg_gen_sub_i64(val, gen_load_replay_y(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
        break;
    case MMIX_ALU_OR:
        tcg_gen_or_i64(val, gen_load_replay_y(ctx, a),
                       gen_load_replay_z(ctx, a, immediate));
        break;
    case MMIX_ALU_ORN:
        tcg_gen_orc_i64(val, gen_load_replay_y(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
        break;
    case MMIX_ALU_NOR:
        tcg_gen_or_i64(val, gen_load_replay_y(ctx, a),
                       gen_load_replay_z(ctx, a, immediate));
        tcg_gen_not_i64(val, val);
        break;
    case MMIX_ALU_XOR:
        tcg_gen_xor_i64(val, gen_load_replay_y(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
        break;
    case MMIX_ALU_AND:
        tcg_gen_and_i64(val, gen_load_replay_y(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
        break;
    case MMIX_ALU_ANDN:
        tcg_gen_andc_i64(val, gen_load_replay_y(ctx, a),
                         gen_load_replay_z(ctx, a, immediate));
        break;
    case MMIX_ALU_NAND:
        tcg_gen_and_i64(val, gen_load_replay_y(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
        tcg_gen_not_i64(val, val);
        break;
    case MMIX_ALU_NXOR:
        tcg_gen_xor_i64(val, gen_load_replay_y(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
        tcg_gen_not_i64(val, val);
        break;
    default:
        g_assert_not_reached();
    }

    gen_store_reg(a->x, val);
    return true;
}

static bool gen_addsub_checked(DisasContext *ctx, arg_xyz *a, bool sub,
                               bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();
    TCGv_i64 y = gen_load_replay_y(ctx, a);
    TCGv_i64 z = gen_load_replay_z(ctx, a, immediate);

    if (sub) {
        gen_helper_mmix_sub(val, tcg_env, tcg_constant_i32(ctx->insn), y, z);
    } else {
        gen_helper_mmix_add(val, tcg_env, tcg_constant_i32(ctx->insn), y, z);
    }
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_neg_checked(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_sub(val, tcg_env, tcg_constant_i32(ctx->insn),
                        gen_load_replay_y_immediate(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

TRANS_GEN2(ADD, gen_addsub_checked, false, false)
TRANS_GEN2(ADDI, gen_addsub_checked, false, true)
TRANS_GEN2(ADDU, gen_alu, MMIX_ALU_ADD, false)
TRANS_GEN2(ADDUI, gen_alu, MMIX_ALU_ADD, true)
TRANS_GEN2(SUB, gen_addsub_checked, true, false)
TRANS_GEN2(SUBI, gen_addsub_checked, true, true)
TRANS_GEN2(SUBU, gen_alu, MMIX_ALU_SUB, false)
TRANS_GEN2(SUBUI, gen_alu, MMIX_ALU_SUB, true)

static bool gen_mul(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_mul(val, tcg_env, tcg_constant_i32(ctx->insn),
                        gen_load_replay_y(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_mulu(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_mulu(val, tcg_env, gen_load_replay_y(ctx, a),
                         gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_div(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_div(val, tcg_env, tcg_constant_i32(ctx->insn),
                        gen_load_replay_y(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_divu(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_divu(val, tcg_env, gen_load_replay_y(ctx, a),
                         gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

TRANS_GEN1(MUL, gen_mul, false)
TRANS_GEN1(MULI, gen_mul, true)
TRANS_GEN1(MULU, gen_mulu, false)
TRANS_GEN1(MULUI, gen_mulu, true)
TRANS_GEN1(DIV, gen_div, false)
TRANS_GEN1(DIVI, gen_div, true)
TRANS_GEN1(DIVU, gen_divu, false)
TRANS_GEN1(DIVUI, gen_divu, true)
TRANS_GEN2(OR, gen_alu, MMIX_ALU_OR, false)
TRANS_GEN2(ORI, gen_alu, MMIX_ALU_OR, true)
TRANS_GEN2(ORN, gen_alu, MMIX_ALU_ORN, false)
TRANS_GEN2(ORNI, gen_alu, MMIX_ALU_ORN, true)
TRANS_GEN2(NOR, gen_alu, MMIX_ALU_NOR, false)
TRANS_GEN2(NORI, gen_alu, MMIX_ALU_NOR, true)
TRANS_GEN2(XOR, gen_alu, MMIX_ALU_XOR, false)
TRANS_GEN2(XORI, gen_alu, MMIX_ALU_XOR, true)
TRANS_GEN2(AND, gen_alu, MMIX_ALU_AND, false)
TRANS_GEN2(ANDI, gen_alu, MMIX_ALU_AND, true)
TRANS_GEN2(ANDN, gen_alu, MMIX_ALU_ANDN, false)
TRANS_GEN2(ANDNI, gen_alu, MMIX_ALU_ANDN, true)
TRANS_GEN2(NAND, gen_alu, MMIX_ALU_NAND, false)
TRANS_GEN2(NANDI, gen_alu, MMIX_ALU_NAND, true)
TRANS_GEN2(NXOR, gen_alu, MMIX_ALU_NXOR, false)
TRANS_GEN2(NXORI, gen_alu, MMIX_ALU_NXOR, true)

static bool gen_scaled_addu(DisasContext *ctx, arg_xyz *a, unsigned shift,
                            bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    tcg_gen_shli_i64(val, gen_load_replay_y(ctx, a), shift);
    tcg_gen_add_i64(val, val, gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_negu(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    tcg_gen_sub_i64(val, gen_load_replay_y_immediate(ctx, a),
                    gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_shift(DisasContext *ctx, arg_xyz *a, MMIXShiftKind kind,
                      bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();
    TCGv_i64 count = gen_load_replay_z(ctx, a, immediate);
    TCGv_i64 safe_count = tcg_temp_new_i64();
    TCGv_i64 lhs = gen_load_replay_y(ctx, a);

    if (kind != MMIX_SHIFT_SL) {
        tcg_gen_andi_i64(safe_count, count, 63);
    }

    switch (kind) {
    case MMIX_SHIFT_SL:
        gen_helper_mmix_sl(val, tcg_env, tcg_constant_i32(ctx->insn),
                           lhs, count);
        break;
    case MMIX_SHIFT_SLU:
        tcg_gen_shl_i64(val, lhs, safe_count);
        tcg_gen_movcond_i64(TCG_COND_LTU, val, count, tcg_constant_i64(64),
                            val, tcg_constant_i64(0));
        break;
    case MMIX_SHIFT_SR:
    {
        TCGv_i64 extreme = tcg_temp_new_i64();

        tcg_gen_sar_i64(val, lhs, safe_count);
        tcg_gen_sari_i64(extreme, lhs, 63);
        tcg_gen_movcond_i64(TCG_COND_LTU, val, count, tcg_constant_i64(64),
                            val, extreme);
        break;
    }
    case MMIX_SHIFT_SRU:
        tcg_gen_shr_i64(val, lhs, safe_count);
        tcg_gen_movcond_i64(TCG_COND_LTU, val, count, tcg_constant_i64(64),
                            val, tcg_constant_i64(0));
        break;
    default:
        g_assert_not_reached();
    }

    gen_store_reg(a->x, val);
    return true;
}

TRANS_GEN2(TWO_ADDU, gen_scaled_addu, 1, false)
TRANS_GEN2(TWO_ADDUI, gen_scaled_addu, 1, true)
TRANS_GEN2(FOUR_ADDU, gen_scaled_addu, 2, false)
TRANS_GEN2(FOUR_ADDUI, gen_scaled_addu, 2, true)
TRANS_GEN2(EIGHT_ADDU, gen_scaled_addu, 3, false)
TRANS_GEN2(EIGHT_ADDUI, gen_scaled_addu, 3, true)
TRANS_GEN2(SIXTEEN_ADDU, gen_scaled_addu, 4, false)
TRANS_GEN2(SIXTEEN_ADDUI, gen_scaled_addu, 4, true)
TRANS_GEN1(NEGU, gen_negu, false)
TRANS_GEN1(NEG, gen_neg_checked, false)
TRANS_GEN1(NEGUI, gen_negu, true)
TRANS_GEN1(NEGI, gen_neg_checked, true)
TRANS_GEN2(SL, gen_shift, MMIX_SHIFT_SL, false)
TRANS_GEN2(SLI, gen_shift, MMIX_SHIFT_SL, true)
TRANS_GEN2(SLU, gen_shift, MMIX_SHIFT_SLU, false)
TRANS_GEN2(SLUI, gen_shift, MMIX_SHIFT_SLU, true)
TRANS_GEN2(SR, gen_shift, MMIX_SHIFT_SR, false)
TRANS_GEN2(SRI, gen_shift, MMIX_SHIFT_SR, true)
TRANS_GEN2(SRU, gen_shift, MMIX_SHIFT_SRU, false)
TRANS_GEN2(SRUI, gen_shift, MMIX_SHIFT_SRU, true)

static uint64_t mmix_wyde_value(const arg_xyz *a, unsigned shift)
{
    return (uint64_t)a->yz << shift;
}

static bool gen_set_wyde(DisasContext *ctx, arg_xyz *a, unsigned shift)
{
    TCGv_i64 val = ctx->substitute_operands ?
                   ctx->replay_z :
                   tcg_constant_i64(mmix_wyde_value(a, shift));

    gen_store_reg(a->x, val);
    return true;
}

static bool gen_inc_wyde(DisasContext *ctx, arg_xyz *a, unsigned shift)
{
    TCGv_i64 val = tcg_temp_new_i64();

    if (ctx->substitute_operands) {
        tcg_gen_add_i64(val, ctx->replay_y, ctx->replay_z);
    } else {
        tcg_gen_add_i64(val, gen_load_reg(a->x),
                        tcg_constant_i64(mmix_wyde_value(a, shift)));
    }
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_or_wyde(DisasContext *ctx, arg_xyz *a, unsigned shift)
{
    TCGv_i64 val = tcg_temp_new_i64();

    if (ctx->substitute_operands) {
        tcg_gen_or_i64(val, ctx->replay_y, ctx->replay_z);
    } else {
        tcg_gen_ori_i64(val, gen_load_reg(a->x),
                        mmix_wyde_value(a, shift));
    }
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_andn_wyde(DisasContext *ctx, arg_xyz *a, unsigned shift)
{
    TCGv_i64 val = tcg_temp_new_i64();

    if (ctx->substitute_operands) {
        tcg_gen_andc_i64(val, ctx->replay_y, ctx->replay_z);
    } else {
        tcg_gen_andi_i64(val, gen_load_reg(a->x),
                         ~mmix_wyde_value(a, shift));
    }
    gen_store_reg(a->x, val);
    return true;
}

TRANS_GEN1(SETH, gen_set_wyde, 48)
TRANS_GEN1(SETMH, gen_set_wyde, 32)
TRANS_GEN1(SETML, gen_set_wyde, 16)
TRANS_GEN1(SETL, gen_set_wyde, 0)
TRANS_GEN1(INCH, gen_inc_wyde, 48)
TRANS_GEN1(INCMH, gen_inc_wyde, 32)
TRANS_GEN1(INCML, gen_inc_wyde, 16)
TRANS_GEN1(INCL, gen_inc_wyde, 0)
TRANS_GEN1(ORH, gen_or_wyde, 48)
TRANS_GEN1(ORMH, gen_or_wyde, 32)
TRANS_GEN1(ORML, gen_or_wyde, 16)
TRANS_GEN1(ORL, gen_or_wyde, 0)
TRANS_GEN1(ANDNH, gen_andn_wyde, 48)
TRANS_GEN1(ANDNMH, gen_andn_wyde, 32)
TRANS_GEN1(ANDNML, gen_andn_wyde, 16)
TRANS_GEN1(ANDNL, gen_andn_wyde, 0)

static bool gen_bdif(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_bdif(val, gen_load_replay_y(ctx, a),
                         gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_wdif(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_wdif(val, gen_load_replay_y(ctx, a),
                         gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_tdif(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_tdif(val, gen_load_replay_y(ctx, a),
                         gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_odif(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_odif(val, gen_load_replay_y(ctx, a),
                         gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_mux(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_mux(val, tcg_env, gen_load_replay_y(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_sadd(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    tcg_gen_andc_i64(val, gen_load_replay_y(ctx, a),
                     gen_load_replay_z(ctx, a, immediate));
    tcg_gen_ctpop_i64(val, val);
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_mor(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_mor(val, gen_load_replay_y(ctx, a),
                        gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_mxor(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_mxor(val, gen_load_replay_y(ctx, a),
                         gen_load_replay_z(ctx, a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

TRANS_GEN1(BDIF, gen_bdif, false)
TRANS_GEN1(BDIFI, gen_bdif, true)
TRANS_GEN1(WDIF, gen_wdif, false)
TRANS_GEN1(WDIFI, gen_wdif, true)
TRANS_GEN1(TDIF, gen_tdif, false)
TRANS_GEN1(TDIFI, gen_tdif, true)
TRANS_GEN1(ODIF, gen_odif, false)
TRANS_GEN1(ODIFI, gen_odif, true)
TRANS_GEN1(MUX, gen_mux, false)
TRANS_GEN1(MUXI, gen_mux, true)
TRANS_GEN1(SADD, gen_sadd, false)
TRANS_GEN1(SADDI, gen_sadd, true)
TRANS_GEN1(MOR, gen_mor, false)
TRANS_GEN1(MORI, gen_mor, true)
TRANS_GEN1(MXOR, gen_mxor, false)
TRANS_GEN1(MXORI, gen_mxor, true)

static bool gen_cmp(DisasContext *ctx, arg_xyz *a, MMIXCompareKind kind,
                    bool immediate)
{
    TCGv_i64 gt = tcg_temp_new_i64();
    TCGv_i64 lt = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();
    TCGv_i64 lhs = gen_load_replay_y(ctx, a);
    TCGv_i64 rhs = gen_load_replay_z(ctx, a, immediate);

    if (kind == MMIX_CMP_SIGNED) {
        tcg_gen_setcond_i64(TCG_COND_GT, gt, lhs, rhs);
        tcg_gen_setcond_i64(TCG_COND_LT, lt, lhs, rhs);
    } else {
        tcg_gen_setcond_i64(TCG_COND_GTU, gt, lhs, rhs);
        tcg_gen_setcond_i64(TCG_COND_LTU, lt, lhs, rhs);
    }
    tcg_gen_sub_i64(val, gt, lt);
    gen_store_reg(a->x, val);
    return true;
}

TRANS_GEN2(CMP, gen_cmp, MMIX_CMP_SIGNED, false)
TRANS_GEN2(CMPI, gen_cmp, MMIX_CMP_SIGNED, true)
TRANS_GEN2(CMPU, gen_cmp, MMIX_CMP_UNSIGNED, false)
TRANS_GEN2(CMPUI, gen_cmp, MMIX_CMP_UNSIGNED, true)

static void gen_predicate(TCGv_i64 pred, TCGv_i64 val,
                          MMIXPredicateKind predicate)
{
    TCGv_i64 tmp;

    switch (predicate) {
    case MMIX_PRED_NEGATIVE:
        tcg_gen_setcondi_i64(TCG_COND_LT, pred, val, 0);
        break;
    case MMIX_PRED_ZERO:
        tcg_gen_setcondi_i64(TCG_COND_EQ, pred, val, 0);
        break;
    case MMIX_PRED_POSITIVE:
        tcg_gen_setcondi_i64(TCG_COND_GT, pred, val, 0);
        break;
    case MMIX_PRED_ODD:
        tmp = tcg_temp_new_i64();
        tcg_gen_andi_i64(tmp, val, 1);
        tcg_gen_setcondi_i64(TCG_COND_NE, pred, tmp, 0);
        break;
    case MMIX_PRED_NONNEGATIVE:
        tcg_gen_setcondi_i64(TCG_COND_GE, pred, val, 0);
        break;
    case MMIX_PRED_NONZERO:
        tcg_gen_setcondi_i64(TCG_COND_NE, pred, val, 0);
        break;
    case MMIX_PRED_NONPOSITIVE:
        tcg_gen_setcondi_i64(TCG_COND_LE, pred, val, 0);
        break;
    case MMIX_PRED_EVEN:
        tmp = tcg_temp_new_i64();
        tcg_gen_andi_i64(tmp, val, 1);
        tcg_gen_setcondi_i64(TCG_COND_EQ, pred, tmp, 0);
        break;
    default:
        g_assert_not_reached();
    }
}

static bool gen_cond_result(DisasContext *ctx, arg_xyz *a,
                            MMIXPredicateKind predicate,
                            bool immediate, bool zero_false)
{
    TCGv_i64 pred = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_predicate(pred, gen_load_replay_y(ctx, a), predicate);
    tcg_gen_movcond_i64(TCG_COND_NE, val, pred, tcg_constant_i64(0),
                        gen_load_replay_z(ctx, a, immediate),
                        zero_false ? tcg_constant_i64(0) : gen_load_reg(a->x));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_cs(DisasContext *ctx, arg_xyz *a, MMIXPredicateKind predicate,
                   bool immediate)
{
    return gen_cond_result(ctx, a, predicate, immediate, false);
}

static bool gen_zs(DisasContext *ctx, arg_xyz *a, MMIXPredicateKind predicate,
                   bool immediate)
{
    return gen_cond_result(ctx, a, predicate, immediate, true);
}

TRANS_GEN2(CSN, gen_cs, MMIX_PRED_NEGATIVE, false)
TRANS_GEN2(CSNI, gen_cs, MMIX_PRED_NEGATIVE, true)
TRANS_GEN2(CSZ, gen_cs, MMIX_PRED_ZERO, false)
TRANS_GEN2(CSZI, gen_cs, MMIX_PRED_ZERO, true)
TRANS_GEN2(CSP, gen_cs, MMIX_PRED_POSITIVE, false)
TRANS_GEN2(CSPI, gen_cs, MMIX_PRED_POSITIVE, true)
TRANS_GEN2(CSOD, gen_cs, MMIX_PRED_ODD, false)
TRANS_GEN2(CSODI, gen_cs, MMIX_PRED_ODD, true)
TRANS_GEN2(CSNN, gen_cs, MMIX_PRED_NONNEGATIVE, false)
TRANS_GEN2(CSNNI, gen_cs, MMIX_PRED_NONNEGATIVE, true)
TRANS_GEN2(CSNZ, gen_cs, MMIX_PRED_NONZERO, false)
TRANS_GEN2(CSNZI, gen_cs, MMIX_PRED_NONZERO, true)
TRANS_GEN2(CSNP, gen_cs, MMIX_PRED_NONPOSITIVE, false)
TRANS_GEN2(CSNPI, gen_cs, MMIX_PRED_NONPOSITIVE, true)
TRANS_GEN2(CSEV, gen_cs, MMIX_PRED_EVEN, false)
TRANS_GEN2(CSEVI, gen_cs, MMIX_PRED_EVEN, true)

TRANS_GEN2(ZSN, gen_zs, MMIX_PRED_NEGATIVE, false)
TRANS_GEN2(ZSNI, gen_zs, MMIX_PRED_NEGATIVE, true)
TRANS_GEN2(ZSZ, gen_zs, MMIX_PRED_ZERO, false)
TRANS_GEN2(ZSZI, gen_zs, MMIX_PRED_ZERO, true)
TRANS_GEN2(ZSP, gen_zs, MMIX_PRED_POSITIVE, false)
TRANS_GEN2(ZSPI, gen_zs, MMIX_PRED_POSITIVE, true)
TRANS_GEN2(ZSOD, gen_zs, MMIX_PRED_ODD, false)
TRANS_GEN2(ZSODI, gen_zs, MMIX_PRED_ODD, true)
TRANS_GEN2(ZSNN, gen_zs, MMIX_PRED_NONNEGATIVE, false)
TRANS_GEN2(ZSNNI, gen_zs, MMIX_PRED_NONNEGATIVE, true)
TRANS_GEN2(ZSNZ, gen_zs, MMIX_PRED_NONZERO, false)
TRANS_GEN2(ZSNZI, gen_zs, MMIX_PRED_NONZERO, true)
TRANS_GEN2(ZSNP, gen_zs, MMIX_PRED_NONPOSITIVE, false)
TRANS_GEN2(ZSNPI, gen_zs, MMIX_PRED_NONPOSITIVE, true)
TRANS_GEN2(ZSEV, gen_zs, MMIX_PRED_EVEN, false)
TRANS_GEN2(ZSEVI, gen_zs, MMIX_PRED_EVEN, true)

static bool gen_branch(DisasContext *ctx, arg_xyz *a,
                       MMIXPredicateKind predicate, bool backward)
{
    TCGLabel *not_taken = gen_new_label();
    TCGv_i64 pred = tcg_temp_new_i64();
    vaddr dest = mmix_branch_dest(ctx, a, backward);
    vaddr next = ctx->base.pc_next;

    gen_predicate(pred, gen_load_reg(a->x), predicate);
    tcg_gen_brcondi_i64(TCG_COND_EQ, pred, 0, not_taken);
    gen_goto_tb(ctx, 0, dest);
    gen_set_label(not_taken);
    gen_goto_tb(ctx, 1, next);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool gen_geta(DisasContext *ctx, arg_xyz *a, bool backward)
{
    gen_store_reg(a->x, tcg_constant_i64(mmix_branch_dest(ctx, a, backward)));
    return true;
}

static bool gen_jmp(DisasContext *ctx, arg_xyz *a, bool backward)
{
    gen_goto_tb(ctx, 0, mmix_jump_dest(ctx, a, backward));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool gen_go(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 dest = tcg_temp_new_i64();

    tcg_gen_add_i64(dest, gen_load_reg(a->y), gen_load_z(a, immediate));
    tcg_gen_andi_i64(dest, dest, ~3ULL);
    gen_store_reg(a->x, tcg_constant_i64(ctx->base.pc_next));
    gen_consume_insn_replay(ctx);
    tcg_gen_mov_i64(cpu_pc, dest);
    tcg_gen_addi_i64(cpu_npc, dest, 4);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool gen_pushj(DisasContext *ctx, arg_xyz *a, bool backward)
{
    gen_helper_mmix_push(tcg_env, tcg_constant_i32(a->x),
                         tcg_constant_i64(ctx->base.pc_next));
    gen_goto_tb(ctx, 0, mmix_branch_dest(ctx, a, backward));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool gen_pushgo(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 dest = tcg_temp_new_i64();

    tcg_gen_add_i64(dest, gen_load_reg(a->y), gen_load_z(a, immediate));
    tcg_gen_andi_i64(dest, dest, ~3ULL);
    gen_helper_mmix_push(tcg_env, tcg_constant_i32(a->x),
                         tcg_constant_i64(ctx->base.pc_next));
    gen_consume_insn_replay(ctx);
    tcg_gen_mov_i64(cpu_pc, dest);
    tcg_gen_addi_i64(cpu_npc, dest, 4);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_POP(DisasContext *ctx, arg_xyz *a)
{
    TCGv_i64 dest = tcg_temp_new_i64();

    gen_helper_mmix_pop(dest, tcg_env, tcg_constant_i32(a->x),
                        tcg_constant_i32(a->yz));
    gen_consume_insn_replay(ctx);
    tcg_gen_mov_i64(cpu_pc, dest);
    tcg_gen_addi_i64(cpu_npc, dest, 4);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

TRANS_GEN2(BN, gen_branch, MMIX_PRED_NEGATIVE, false)
TRANS_GEN2(BNB, gen_branch, MMIX_PRED_NEGATIVE, true)
TRANS_GEN2(BZ, gen_branch, MMIX_PRED_ZERO, false)
TRANS_GEN2(BZB, gen_branch, MMIX_PRED_ZERO, true)
TRANS_GEN2(BP, gen_branch, MMIX_PRED_POSITIVE, false)
TRANS_GEN2(BPB, gen_branch, MMIX_PRED_POSITIVE, true)
TRANS_GEN2(BOD, gen_branch, MMIX_PRED_ODD, false)
TRANS_GEN2(BODB, gen_branch, MMIX_PRED_ODD, true)
TRANS_GEN2(BNN, gen_branch, MMIX_PRED_NONNEGATIVE, false)
TRANS_GEN2(BNNB, gen_branch, MMIX_PRED_NONNEGATIVE, true)
TRANS_GEN2(BNZ, gen_branch, MMIX_PRED_NONZERO, false)
TRANS_GEN2(BNZB, gen_branch, MMIX_PRED_NONZERO, true)
TRANS_GEN2(BNP, gen_branch, MMIX_PRED_NONPOSITIVE, false)
TRANS_GEN2(BNPB, gen_branch, MMIX_PRED_NONPOSITIVE, true)
TRANS_GEN2(BEV, gen_branch, MMIX_PRED_EVEN, false)
TRANS_GEN2(BEVB, gen_branch, MMIX_PRED_EVEN, true)
TRANS_GEN2(PBN, gen_branch, MMIX_PRED_NEGATIVE, false)
TRANS_GEN2(PBNB, gen_branch, MMIX_PRED_NEGATIVE, true)
TRANS_GEN2(PBZ, gen_branch, MMIX_PRED_ZERO, false)
TRANS_GEN2(PBZB, gen_branch, MMIX_PRED_ZERO, true)
TRANS_GEN2(PBP, gen_branch, MMIX_PRED_POSITIVE, false)
TRANS_GEN2(PBPB, gen_branch, MMIX_PRED_POSITIVE, true)
TRANS_GEN2(PBOD, gen_branch, MMIX_PRED_ODD, false)
TRANS_GEN2(PBODB, gen_branch, MMIX_PRED_ODD, true)
TRANS_GEN2(PBNN, gen_branch, MMIX_PRED_NONNEGATIVE, false)
TRANS_GEN2(PBNNB, gen_branch, MMIX_PRED_NONNEGATIVE, true)
TRANS_GEN2(PBNZ, gen_branch, MMIX_PRED_NONZERO, false)
TRANS_GEN2(PBNZB, gen_branch, MMIX_PRED_NONZERO, true)
TRANS_GEN2(PBNP, gen_branch, MMIX_PRED_NONPOSITIVE, false)
TRANS_GEN2(PBNPB, gen_branch, MMIX_PRED_NONPOSITIVE, true)
TRANS_GEN2(PBEV, gen_branch, MMIX_PRED_EVEN, false)
TRANS_GEN2(PBEVB, gen_branch, MMIX_PRED_EVEN, true)

TRANS_GEN1(GETA, gen_geta, false)
TRANS_GEN1(GETAB, gen_geta, true)

static bool trans_GET(DisasContext *ctx, arg_xyz *a)
{
    if (a->y != 0 || a->z >= MMIX_SREGS) {
        return gen_mmix_break_rules(ctx, a, false);
    }

    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_read_sreg(val, tcg_env, tcg_constant_i32(a->z));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_put(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    if (a->y != 0 || a->x >= MMIX_SREGS) {
        return gen_mmix_break_rules(ctx, a, immediate);
    }

    gen_helper_mmix_put_sreg(tcg_env, tcg_constant_i32(ctx->insn),
                             tcg_constant_i32(a->x),
                             gen_load_z(a, immediate));
    if (a->x == MMIX_SREG_RK || a->x == MMIX_SREG_RV) {
        ctx->base.is_jmp = DISAS_TOO_MANY;
    }
    return true;
}

TRANS_GEN1(PUT, gen_put, false)
TRANS_GEN1(PUTI, gen_put, true)
TRANS_GEN1(JMP, gen_jmp, false)
TRANS_GEN1(JMPB, gen_jmp, true)
TRANS_GEN1(PUSHJ, gen_pushj, false)
TRANS_GEN1(PUSHJB, gen_pushj, true)
TRANS_GEN1(GO, gen_go, false)
TRANS_GEN1(GOI, gen_go, true)
TRANS_GEN1(PUSHGO, gen_pushgo, false)
TRANS_GEN1(PUSHGOI, gen_pushgo, true)

static bool trans_SAVE(DisasContext *ctx, arg_xyz *a)
{
    if (a->y != 0 || a->z != 0) {
        return gen_mmix_break_rules(ctx, a, false);
    }

    gen_helper_mmix_save(tcg_env, tcg_constant_i32(ctx->insn),
                         tcg_constant_i32(a->x));
    return true;
}

static bool trans_UNSAVE(DisasContext *ctx, arg_xyz *a)
{
    if (a->x != 0 || a->y != 0) {
        return gen_mmix_break_rules(ctx, a, false);
    }

    gen_helper_mmix_unsave(tcg_env, tcg_constant_i32(a->z));
    return true;
}

static void gen_data_access_value(TCGv_i64 value)
{
    tcg_gen_st_i64(value, tcg_env,
                   offsetof(CPUMMIXState, data_access.value));
}

static bool gen_load_mem(DisasContext *ctx, arg_xyz *a, bool immediate,
                         MemOp memop, uint64_t align_mask)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, align_mask);
    if (ctx->hosted_memory) {
        gen_helper_mmix_hosted_load(val, tcg_env, addr,
                                    tcg_constant_i32(memop));
    } else {
        tcg_gen_qemu_ld_i64(val, addr, 0, memop);
    }
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_ldht(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, 3);
    if (ctx->hosted_memory) {
        gen_helper_mmix_hosted_load(val, tcg_env, addr,
                                    tcg_constant_i32(MO_BEUL));
    } else {
        tcg_gen_qemu_ld_i64(val, addr, 0, MO_BEUL);
    }
    tcg_gen_shli_i64(val, val, 32);
    gen_store_reg(a->x, val);
    return true;
}

TRANS_GEN3(LDB, gen_load_mem, false, MO_SB, 0)
TRANS_GEN3(LDBI, gen_load_mem, true, MO_SB, 0)
TRANS_GEN3(LDBU, gen_load_mem, false, MO_UB, 0)
TRANS_GEN3(LDBUI, gen_load_mem, true, MO_UB, 0)
TRANS_GEN3(LDW, gen_load_mem, false, MO_BESW, 1)
TRANS_GEN3(LDWI, gen_load_mem, true, MO_BESW, 1)
TRANS_GEN3(LDWU, gen_load_mem, false, MO_BEUW, 1)
TRANS_GEN3(LDWUI, gen_load_mem, true, MO_BEUW, 1)
TRANS_GEN3(LDT, gen_load_mem, false, MO_BESL, 3)
TRANS_GEN3(LDTI, gen_load_mem, true, MO_BESL, 3)
TRANS_GEN3(LDTU, gen_load_mem, false, MO_BEUL, 3)
TRANS_GEN3(LDTUI, gen_load_mem, true, MO_BEUL, 3)
TRANS_GEN3(LDO, gen_load_mem, false, MO_BEUQ, 7)
TRANS_GEN3(LDOI, gen_load_mem, true, MO_BEUQ, 7)
TRANS_GEN3(LDOU, gen_load_mem, false, MO_BEUQ, 7)
TRANS_GEN3(LDOUI, gen_load_mem, true, MO_BEUQ, 7)
TRANS_GEN3(LDUNC, gen_load_mem, false, MO_BEUQ, 7)
TRANS_GEN3(LDUNCI, gen_load_mem, true, MO_BEUQ, 7)

static bool gen_ldvts(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 key = tcg_temp_new_i64();
    TCGv_i64 status = tcg_temp_new_i64();

    gen_effective_address(key, a, immediate, 0);
    gen_helper_mmix_ldvts(status, tcg_env, tcg_constant_i32(ctx->insn), key);
    gen_store_reg(a->x, status);
    return true;
}

TRANS_GEN1(LDVTS, gen_ldvts, false)
TRANS_GEN1(LDVTSI, gen_ldvts, true)
TRANS_GEN1(LDHT, gen_ldht, false)
TRANS_GEN1(LDHTI, gen_ldht, true)

static bool gen_ldsf(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, 3);
    if (ctx->hosted_memory) {
        gen_helper_mmix_hosted_load(val, tcg_env, addr,
                                    tcg_constant_i32(MO_BEUL));
    } else {
        tcg_gen_qemu_ld_i64(val, addr, 0, MO_BEUL);
    }
    gen_helper_mmix_ldsf(val, val);
    gen_store_reg(a->x, val);
    return true;
}

TRANS_GEN1(LDSF, gen_ldsf, false)
TRANS_GEN1(LDSFI, gen_ldsf, true)

static bool gen_cswap(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i64 rp = tcg_temp_new_i64();
    TCGv_i64 new = gen_load_reg(a->x);
    TCGv_i64 next_rp = tcg_temp_new_i64();
    TCGv_i64 success = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, 7);
    gen_helper_mmix_read_sreg(rp, tcg_env, tcg_constant_i32(MMIX_SREG_RP));
    if (ctx->hosted_memory) {
        gen_helper_mmix_hosted_cmpxchg(old, tcg_env, addr, rp, new);
        ctx->base.is_jmp = DISAS_TOO_MANY;
    } else {
        tcg_gen_atomic_cmpxchg_i64(old, addr, rp, new, 0, MO_BEUQ);
    }

    tcg_gen_movcond_i64(TCG_COND_EQ, next_rp, old, rp, rp, old);
    gen_helper_mmix_put_sreg(tcg_env, tcg_constant_i32(ctx->insn),
                             tcg_constant_i32(MMIX_SREG_RP), next_rp);

    tcg_gen_movcond_i64(TCG_COND_EQ, success, old, rp,
                        tcg_constant_i64(1), tcg_constant_i64(0));
    gen_store_reg(a->x, success);
    return true;
}

TRANS_GEN1(CSWAP, gen_cswap, false)
TRANS_GEN1(CSWAPI, gen_cswap, true)
TRANS_NOP(PRELD)
TRANS_NOP(PRELDI)
TRANS_NOP(PREGO)
TRANS_NOP(PREGOI)

static bool gen_store_value(DisasContext *ctx, arg_xyz *a, bool immediate,
                            MemOp memop, uint64_t align_mask, TCGv_i64 val)
{
    TCGv_i64 addr = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, align_mask);
    if (ctx->hosted_memory) {
        gen_helper_mmix_hosted_store(tcg_env, addr, val,
                                     tcg_constant_i32(memop));
        ctx->base.is_jmp = DISAS_TOO_MANY;
    } else {
        tcg_gen_qemu_st_i64(val, addr, 0, memop);
    }
    return true;
}

static bool gen_store_mem(DisasContext *ctx, arg_xyz *a, bool immediate,
                          MemOp memop, uint64_t align_mask)
{
    return gen_store_value(ctx, a, immediate, memop, align_mask,
                           gen_load_reg(a->x));
}

static bool gen_store_const_mem(DisasContext *ctx, arg_xyz *a, bool immediate,
                                MemOp memop, uint64_t align_mask)
{
    return gen_store_value(ctx, a, immediate, memop, align_mask,
                           tcg_constant_i64(a->x));
}

static bool gen_stht(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, 3);
    tcg_gen_shri_i64(val, gen_load_reg(a->x), 32);
    if (ctx->hosted_memory) {
        gen_helper_mmix_hosted_store(tcg_env, addr, val,
                                     tcg_constant_i32(MO_BEUL));
        ctx->base.is_jmp = DISAS_TOO_MANY;
    } else {
        tcg_gen_qemu_st_i64(val, addr, 0, MO_BEUL);
    }
    return true;
}

TRANS_GEN3(STB, gen_store_mem, false, MO_UB, 0)
TRANS_GEN3(STBI, gen_store_mem, true, MO_UB, 0)
TRANS_GEN3(STBU, gen_store_mem, false, MO_UB, 0)
TRANS_GEN3(STBUI, gen_store_mem, true, MO_UB, 0)
TRANS_GEN3(STW, gen_store_mem, false, MO_BEUW, 1)
TRANS_GEN3(STWI, gen_store_mem, true, MO_BEUW, 1)
TRANS_GEN3(STWU, gen_store_mem, false, MO_BEUW, 1)
TRANS_GEN3(STWUI, gen_store_mem, true, MO_BEUW, 1)
TRANS_GEN3(STT, gen_store_mem, false, MO_BEUL, 3)
TRANS_GEN3(STTI, gen_store_mem, true, MO_BEUL, 3)
TRANS_GEN3(STTU, gen_store_mem, false, MO_BEUL, 3)
TRANS_GEN3(STTUI, gen_store_mem, true, MO_BEUL, 3)
TRANS_GEN3(STO, gen_store_mem, false, MO_BEUQ, 7)
TRANS_GEN3(STOI, gen_store_mem, true, MO_BEUQ, 7)
TRANS_GEN3(STOU, gen_store_mem, false, MO_BEUQ, 7)
TRANS_GEN3(STOUI, gen_store_mem, true, MO_BEUQ, 7)
TRANS_GEN3(STCO, gen_store_const_mem, false, MO_BEUQ, 7)
TRANS_GEN3(STCOI, gen_store_const_mem, true, MO_BEUQ, 7)
TRANS_GEN3(STUNC, gen_store_mem, false, MO_BEUQ, 7)
TRANS_GEN3(STUNCI, gen_store_mem, true, MO_BEUQ, 7)
TRANS_GEN1(STHT, gen_stht, false)
TRANS_GEN1(STHTI, gen_stht, true)

static bool gen_stsf(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, 3);
    gen_helper_mmix_stsf(val, tcg_env, tcg_constant_i32(ctx->insn), addr,
                         gen_load_reg(a->x));
    gen_data_access_value(val);
    if (ctx->hosted_memory) {
        gen_helper_mmix_hosted_store(tcg_env, addr, val,
                                     tcg_constant_i32(MO_BEUL));
        ctx->base.is_jmp = DISAS_TOO_MANY;
    } else {
        tcg_gen_qemu_st_i64(val, addr, 0, MO_BEUL);
    }
    return true;
}

TRANS_GEN1(STSF, gen_stsf, false)
TRANS_GEN1(STSFI, gen_stsf, true)
TRANS_NOP(SYNCD)
TRANS_NOP(SYNCDI)
TRANS_NOP(PREST)
TRANS_NOP(PRESTI)
TRANS_NOP(SYNCID)
TRANS_NOP(SYNCIDI)

static bool trans_SYNC(DisasContext *ctx, arg_xyz *a)
{
    switch (a->xyz) {
    case 0:
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
        return true;
    case 1:
        tcg_gen_mb(TCG_MO_ST_ST | TCG_BAR_SC);
        return true;
    case 2:
        tcg_gen_mb(TCG_MO_LD_LD | TCG_BAR_SC);
        return true;
    case 3:
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
        return true;
    case 4:
    case 5:
    case 6:
    case 7:
        return gen_mmix_privileged_sync(ctx, a->xyz);
    default:
        return gen_mmix_invalid_sync(ctx, a);
    }
}

#undef TRANS_GEN1
#undef TRANS_GEN2
#undef TRANS_GEN3
#undef TRANS_NOP

static void mmix_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    target_ulong page_insns;

    ctx->env = cpu_env(cs);
    ctx->hosted_memory = mmix_cpu_hosted_memory_enabled(ctx->env);
    ctx->replay = dcbase->tb->cs_base & MMIX_TB_REPLAY_FLAG;
    ctx->substitute_operands =
        dcbase->tb->cs_base & MMIX_TB_REPLAY_SUBSTITUTE_FLAG;
    g_assert(!ctx->substitute_operands || ctx->replay);
    if (ctx->replay) {
        dcbase->max_insns = 1;
    }

    /* Keep instruction-fetch traps precise at translated page boundaries. */
    page_insns = -(dcbase->pc_first | TARGET_PAGE_MASK) / 4;
    dcbase->max_insns = MIN(dcbase->max_insns, page_insns);
}

static void mmix_tr_tb_start(DisasContextBase *dcbase, CPUState *cs)
{
}

static void mmix_tr_insn_start(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    vaddr pc = dcbase->pc_next;

    ctx->insn_pc = pc;
    ctx->insn = ctx->replay ? dcbase->tb->cs_base :
                ctx->hosted_memory ? mmix_cpu_hosted_fetch(ctx->env, pc) :
                translator_ldl_end(ctx->env, dcbase, pc, MO_BE);
    tcg_gen_insn_start(pc, ctx->insn, 0);
}

static void mmix_tr_translate_insn(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    vaddr pc = ctx->base.pc_next;
    uint32_t insn = ctx->insn;

    if (ctx->replay) {
        g_assert(ctx->env->insn_replay.active);
        g_assert(pc == ctx->env->insn_replay.insn_pc);
        ctx->base.pc_next = ctx->env->insn_replay.continuation;
        if (ctx->substitute_operands) {
            ctx->replay_y = tcg_temp_new_i64();
            ctx->replay_z = tcg_temp_new_i64();
            tcg_gen_ld_i64(ctx->replay_y, tcg_env,
                           offsetof(CPUMMIXState, insn_replay.y));
            tcg_gen_ld_i64(ctx->replay_z, tcg_env,
                           offsetof(CPUMMIXState, insn_replay.z));
        }
    } else {
        ctx->base.pc_next += 4;
    }

    tcg_gen_movi_i64(cpu_pc, pc);
    tcg_gen_movi_i64(cpu_npc, ctx->base.pc_next);
    if (!decode(ctx, insn)) {
        arg_xyz a = {
            .y = extract32(insn, 8, 8),
            .z = extract32(insn, 0, 8),
        };

        gen_mmix_break_rules(ctx, &a, false);
    }
    /* Faultable helpers must complete before replay ownership is released. */
    if (ctx->base.is_jmp != DISAS_NORETURN) {
        gen_consume_insn_replay(ctx);
    }
}

static void mmix_tr_tb_stop(DisasContextBase *dcbase, CPUState *cs)
{
    switch (dcbase->is_jmp) {
    case DISAS_NEXT:
    case DISAS_TOO_MANY:
        tcg_gen_movi_i64(cpu_pc, dcbase->pc_next);
        tcg_gen_movi_i64(cpu_npc, dcbase->pc_next + 4);
        tcg_gen_exit_tb(NULL, 0);
        break;
    case DISAS_NORETURN:
        break;
    default:
        g_assert_not_reached();
    }
}

static const TranslatorOps mmix_tr_ops = {
    .init_disas_context = mmix_tr_init_disas_context,
    .tb_start           = mmix_tr_tb_start,
    .insn_start         = mmix_tr_insn_start,
    .translate_insn     = mmix_tr_translate_insn,
    .tb_stop            = mmix_tr_tb_stop,
};

void mmix_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc = { };

    translator_loop(cs, tb, max_insns, pc, host_pc, &mmix_tr_ops, &dc.base,
                    TCG_TYPE_VA);
}

void mmix_translate_init(void)
{
    cpu_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPUMMIXState, pc), "pc");
    cpu_npc = tcg_global_mem_new_i64(tcg_env, offsetof(CPUMMIXState, npc),
                                     "npc");
}
