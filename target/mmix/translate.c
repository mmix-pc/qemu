/*
 * QEMU MMIX TCG translation
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "qemu/log.h"
#include "tcg/tcg-op.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
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
} DisasContext;

typedef enum MMIXALUOp {
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
} MMIXALUOp;

typedef enum MMIXCompareOp {
    MMIX_CMP_SIGNED,
    MMIX_CMP_UNSIGNED,
} MMIXCompareOp;

typedef enum MMIXShiftOp {
    MMIX_SHIFT_SLU,
    MMIX_SHIFT_SR,
    MMIX_SHIFT_SRU,
} MMIXShiftOp;

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

typedef enum MMIXPredicate {
    MMIX_PRED_NEGATIVE,
    MMIX_PRED_ZERO,
    MMIX_PRED_POSITIVE,
    MMIX_PRED_ODD,
    MMIX_PRED_NONNEGATIVE,
    MMIX_PRED_NONZERO,
    MMIX_PRED_NONPOSITIVE,
    MMIX_PRED_EVEN,
} MMIXPredicate;

static TCGv_i64 cpu_pc;
static TCGv_i64 cpu_npc;

/* Include the auto-generated decoder. */
#include "decode-insns.c.inc"

static void gen_raise_illegal(DisasContext *ctx)
{
    gen_helper_raise_illegal_instruction(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_goto_tb(DisasContext *ctx, unsigned tb_slot_idx, vaddr dest)
{
    if (translator_use_goto_tb(&ctx->base, dest)) {
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

static bool gen_mmix_unsupported(DisasContext *ctx, const char *mnemonic,
                                 const arg_xyz *a)
{
    qemu_log_mask(LOG_UNIMP,
                  "MMIX decoded unimplemented %s at 0x%016" VADDR_PRIx
                  " x=%u y=%u z=%u yz=0x%04x xyz=0x%06x\n",
                  mnemonic, ctx->insn_pc, a->x, a->y, a->z, a->yz,
                  a->xyz);
    gen_raise_illegal(ctx);
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
    gen_helper_mmix_resume(tcg_env, tcg_constant_i32(a->x),
                           tcg_constant_i32(a->y), tcg_constant_i32(a->z));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_TRAP(DisasContext *ctx, arg_xyz *a)
{
    if (a->x == 0 && a->y == 0 && a->z == 0) {
        /* QEMU porting test exit, not final MMIX TRAP semantics. */
        gen_helper_mmix_test_exit(tcg_env);
        ctx->base.is_jmp = DISAS_NORETURN;
        return true;
    }

    gen_helper_mmix_trap(tcg_env, tcg_constant_i32(ctx->insn),
                         gen_load_reg(a->y), gen_load_reg(a->z));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_SWYM(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

static bool gen_mmix_invalid_sync(DisasContext *ctx, uint32_t mode)
{
    qemu_log_mask(LOG_UNIMP,
                  "MMIX invalid SYNC %u at 0x%016" VADDR_PRIx "\n",
                  mode, ctx->insn_pc);
    gen_raise_illegal(ctx);
    return true;
}

static bool gen_mmix_privileged_sync(DisasContext *ctx, uint32_t mode)
{
    gen_helper_mmix_sync(tcg_env, tcg_constant_i32(mode));
    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
    return true;
}

static bool gen_fp_binary(DisasContext *ctx, arg_xyz *a, MMIXFPOp op)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_fp_binary(val, tcg_env, tcg_constant_i32(op),
                              tcg_constant_i32(ctx->insn),
                              gen_load_reg(a->y), gen_load_reg(a->z));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_fp_unary(DisasContext *ctx, arg_xyz *a, MMIXFPOp op)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_fp_unary(val, tcg_env, tcg_constant_i32(op),
                             tcg_constant_i32(ctx->insn),
                             tcg_constant_i32(a->y), gen_load_reg(a->z));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_fp_fix(DisasContext *ctx, arg_xyz *a, MMIXFPOp op)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_fp_fix(val, tcg_env, tcg_constant_i32(op),
                           tcg_constant_i32(ctx->insn),
                           tcg_constant_i32(a->y), gen_load_reg(a->z));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_fp_float(DisasContext *ctx, arg_xyz *a, MMIXFPOp op,
                         bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_fp_float(val, tcg_env, tcg_constant_i32(op),
                             tcg_constant_i32(ctx->insn),
                             tcg_constant_i32(a->y),
                             gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

#define TRANS_FP_BINARY(NAME, OP) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_fp_binary(ctx, a, OP); \
    }

#define TRANS_FP_UNARY(NAME, OP) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_fp_unary(ctx, a, OP); \
    }

#define TRANS_FP_FIX(NAME, OP) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_fp_fix(ctx, a, OP); \
    }

#define TRANS_FP_FLOAT(NAME, OP, IMMEDIATE) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_fp_float(ctx, a, OP, IMMEDIATE); \
    }

TRANS_FP_BINARY(FCMP, MMIX_FP_FCMP)
TRANS_FP_BINARY(FUN, MMIX_FP_FUN)
TRANS_FP_BINARY(FEQL, MMIX_FP_FEQL)
TRANS_FP_BINARY(FADD, MMIX_FP_FADD)
TRANS_FP_BINARY(FSUB, MMIX_FP_FSUB)
TRANS_FP_BINARY(FMUL, MMIX_FP_FMUL)
TRANS_FP_BINARY(FDIV, MMIX_FP_FDIV)
TRANS_FP_BINARY(FREM, MMIX_FP_FREM)
TRANS_FP_BINARY(FCMPE, MMIX_FP_FCMPE)
TRANS_FP_BINARY(FUNE, MMIX_FP_FUNE)
TRANS_FP_BINARY(FEQLE, MMIX_FP_FEQLE)
TRANS_FP_UNARY(FSQRT, MMIX_FP_FSQRT)
TRANS_FP_UNARY(FINT, MMIX_FP_FINT)
TRANS_FP_FIX(FIX, MMIX_FP_FIX)
TRANS_FP_FIX(FIXU, MMIX_FP_FIXU)
TRANS_FP_FLOAT(FLOT, MMIX_FP_FLOT, false)
TRANS_FP_FLOAT(FLOTI, MMIX_FP_FLOT, true)
TRANS_FP_FLOAT(FLOTU, MMIX_FP_FLOTU, false)
TRANS_FP_FLOAT(FLOTUI, MMIX_FP_FLOTU, true)
TRANS_FP_FLOAT(SFLOT, MMIX_FP_SFLOT, false)
TRANS_FP_FLOAT(SFLOTI, MMIX_FP_SFLOT, true)
TRANS_FP_FLOAT(SFLOTU, MMIX_FP_SFLOTU, false)
TRANS_FP_FLOAT(SFLOTUI, MMIX_FP_SFLOTU, true)

#undef TRANS_FP_BINARY
#undef TRANS_FP_UNARY
#undef TRANS_FP_FIX
#undef TRANS_FP_FLOAT

static bool gen_alu(DisasContext *ctx, arg_xyz *a, MMIXALUOp op,
                    bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    switch (op) {
    case MMIX_ALU_ADD:
        tcg_gen_add_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_SUB:
        tcg_gen_sub_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_OR:
        tcg_gen_or_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_ORN:
        tcg_gen_orc_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_NOR:
        tcg_gen_or_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        tcg_gen_not_i64(val, val);
        break;
    case MMIX_ALU_XOR:
        tcg_gen_xor_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_AND:
        tcg_gen_and_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_ANDN:
        tcg_gen_andc_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        break;
    case MMIX_ALU_NAND:
        tcg_gen_and_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
        tcg_gen_not_i64(val, val);
        break;
    case MMIX_ALU_NXOR:
        tcg_gen_xor_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
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
    TCGv_i64 y = gen_load_reg(a->y);
    TCGv_i64 z = gen_load_z(a, immediate);

    if (sub) {
        gen_helper_mmix_sub(val, tcg_env, tcg_constant_i32(ctx->insn), y, z);
    } else {
        gen_helper_mmix_add(val, tcg_env, tcg_constant_i32(ctx->insn), y, z);
    }
    gen_store_reg(a->x, val);
    return true;
}

static bool trans_ADD(DisasContext *ctx, arg_xyz *a)
{
    return gen_addsub_checked(ctx, a, false, false);
}

static bool trans_ADDI(DisasContext *ctx, arg_xyz *a)
{
    return gen_addsub_checked(ctx, a, false, true);
}

static bool trans_ADDU(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ADD, false);
}

static bool trans_ADDUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ADD, true);
}

static bool trans_SUB(DisasContext *ctx, arg_xyz *a)
{
    return gen_addsub_checked(ctx, a, true, false);
}

static bool trans_SUBI(DisasContext *ctx, arg_xyz *a)
{
    return gen_addsub_checked(ctx, a, true, true);
}

static bool trans_SUBU(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_SUB, false);
}

static bool trans_SUBUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_SUB, true);
}

static bool gen_mul(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_mul(val, tcg_env, tcg_constant_i32(ctx->insn),
                        gen_load_reg(a->y), gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_mulu(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_mulu(val, tcg_env, gen_load_reg(a->y),
                         gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_div(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_div(val, tcg_env, tcg_constant_i32(ctx->insn),
                        gen_load_reg(a->y), gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_divu(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_divu(val, tcg_env, gen_load_reg(a->y),
                         gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool trans_MUL(DisasContext *ctx, arg_xyz *a)
{
    return gen_mul(ctx, a, false);
}

static bool trans_MULI(DisasContext *ctx, arg_xyz *a)
{
    return gen_mul(ctx, a, true);
}

static bool trans_MULU(DisasContext *ctx, arg_xyz *a)
{
    return gen_mulu(ctx, a, false);
}

static bool trans_MULUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_mulu(ctx, a, true);
}

static bool trans_DIV(DisasContext *ctx, arg_xyz *a)
{
    return gen_div(ctx, a, false);
}

static bool trans_DIVI(DisasContext *ctx, arg_xyz *a)
{
    return gen_div(ctx, a, true);
}

static bool trans_DIVU(DisasContext *ctx, arg_xyz *a)
{
    return gen_divu(ctx, a, false);
}

static bool trans_DIVUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_divu(ctx, a, true);
}

static bool trans_OR(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_OR, false);
}

static bool trans_ORI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_OR, true);
}

static bool trans_ORN(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ORN, false);
}

static bool trans_ORNI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ORN, true);
}

static bool trans_NOR(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NOR, false);
}

static bool trans_NORI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NOR, true);
}

static bool trans_XOR(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_XOR, false);
}

static bool trans_XORI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_XOR, true);
}

static bool trans_AND(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_AND, false);
}

static bool trans_ANDI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_AND, true);
}

static bool trans_ANDN(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ANDN, false);
}

static bool trans_ANDNI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_ANDN, true);
}

static bool trans_NAND(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NAND, false);
}

static bool trans_NANDI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NAND, true);
}

static bool trans_NXOR(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NXOR, false);
}

static bool trans_NXORI(DisasContext *ctx, arg_xyz *a)
{
    return gen_alu(ctx, a, MMIX_ALU_NXOR, true);
}

static bool gen_scaled_addu(DisasContext *ctx, arg_xyz *a, unsigned shift,
                            bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    tcg_gen_shli_i64(val, gen_load_reg(a->y), shift);
    tcg_gen_add_i64(val, val, gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_negu(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    tcg_gen_sub_i64(val, tcg_constant_i64(a->y), gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_shift(DisasContext *ctx, arg_xyz *a, MMIXShiftOp op,
                      bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();
    TCGv_i64 count = gen_load_z(a, immediate);
    TCGv_i64 safe_count = tcg_temp_new_i64();
    TCGv_i64 lhs = gen_load_reg(a->y);

    tcg_gen_andi_i64(safe_count, count, 63);

    switch (op) {
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

static bool trans_TWO_ADDU(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 1, false);
}

static bool trans_TWO_ADDUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 1, true);
}

static bool trans_FOUR_ADDU(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 2, false);
}

static bool trans_FOUR_ADDUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 2, true);
}

static bool trans_EIGHT_ADDU(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 3, false);
}

static bool trans_EIGHT_ADDUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 3, true);
}

static bool trans_SIXTEEN_ADDU(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 4, false);
}

static bool trans_SIXTEEN_ADDUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_scaled_addu(ctx, a, 4, true);
}

static bool trans_NEGU(DisasContext *ctx, arg_xyz *a)
{
    return gen_negu(ctx, a, false);
}

static bool trans_NEGUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_negu(ctx, a, true);
}

static bool trans_SLU(DisasContext *ctx, arg_xyz *a)
{
    return gen_shift(ctx, a, MMIX_SHIFT_SLU, false);
}

static bool trans_SLUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_shift(ctx, a, MMIX_SHIFT_SLU, true);
}

static bool trans_SR(DisasContext *ctx, arg_xyz *a)
{
    return gen_shift(ctx, a, MMIX_SHIFT_SR, false);
}

static bool trans_SRI(DisasContext *ctx, arg_xyz *a)
{
    return gen_shift(ctx, a, MMIX_SHIFT_SR, true);
}

static bool trans_SRU(DisasContext *ctx, arg_xyz *a)
{
    return gen_shift(ctx, a, MMIX_SHIFT_SRU, false);
}

static bool trans_SRUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_shift(ctx, a, MMIX_SHIFT_SRU, true);
}

static uint64_t mmix_wyde_value(const arg_xyz *a, unsigned shift)
{
    return (uint64_t)a->yz << shift;
}

static bool gen_set_wyde(DisasContext *ctx, arg_xyz *a, unsigned shift)
{
    gen_store_reg(a->x, tcg_constant_i64(mmix_wyde_value(a, shift)));
    return true;
}

static bool gen_inc_wyde(DisasContext *ctx, arg_xyz *a, unsigned shift)
{
    TCGv_i64 val = tcg_temp_new_i64();

    tcg_gen_add_i64(val, gen_load_reg(a->x),
                    tcg_constant_i64(mmix_wyde_value(a, shift)));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_or_wyde(DisasContext *ctx, arg_xyz *a, unsigned shift)
{
    TCGv_i64 val = tcg_temp_new_i64();

    tcg_gen_ori_i64(val, gen_load_reg(a->x), mmix_wyde_value(a, shift));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_andn_wyde(DisasContext *ctx, arg_xyz *a, unsigned shift)
{
    TCGv_i64 val = tcg_temp_new_i64();

    tcg_gen_andi_i64(val, gen_load_reg(a->x), ~mmix_wyde_value(a, shift));
    gen_store_reg(a->x, val);
    return true;
}

static bool trans_SETH(DisasContext *ctx, arg_xyz *a)
{
    return gen_set_wyde(ctx, a, 48);
}

static bool trans_SETMH(DisasContext *ctx, arg_xyz *a)
{
    return gen_set_wyde(ctx, a, 32);
}

static bool trans_SETML(DisasContext *ctx, arg_xyz *a)
{
    return gen_set_wyde(ctx, a, 16);
}

static bool trans_SETL(DisasContext *ctx, arg_xyz *a)
{
    return gen_set_wyde(ctx, a, 0);
}

static bool trans_INCH(DisasContext *ctx, arg_xyz *a)
{
    return gen_inc_wyde(ctx, a, 48);
}

static bool trans_INCMH(DisasContext *ctx, arg_xyz *a)
{
    return gen_inc_wyde(ctx, a, 32);
}

static bool trans_INCML(DisasContext *ctx, arg_xyz *a)
{
    return gen_inc_wyde(ctx, a, 16);
}

static bool trans_INCL(DisasContext *ctx, arg_xyz *a)
{
    return gen_inc_wyde(ctx, a, 0);
}

static bool trans_ORH(DisasContext *ctx, arg_xyz *a)
{
    return gen_or_wyde(ctx, a, 48);
}

static bool trans_ORMH(DisasContext *ctx, arg_xyz *a)
{
    return gen_or_wyde(ctx, a, 32);
}

static bool trans_ORML(DisasContext *ctx, arg_xyz *a)
{
    return gen_or_wyde(ctx, a, 16);
}

static bool trans_ORL(DisasContext *ctx, arg_xyz *a)
{
    return gen_or_wyde(ctx, a, 0);
}

static bool trans_ANDNH(DisasContext *ctx, arg_xyz *a)
{
    return gen_andn_wyde(ctx, a, 48);
}

static bool trans_ANDNMH(DisasContext *ctx, arg_xyz *a)
{
    return gen_andn_wyde(ctx, a, 32);
}

static bool trans_ANDNML(DisasContext *ctx, arg_xyz *a)
{
    return gen_andn_wyde(ctx, a, 16);
}

static bool trans_ANDNL(DisasContext *ctx, arg_xyz *a)
{
    return gen_andn_wyde(ctx, a, 0);
}

static bool gen_bdif(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_bdif(val, gen_load_reg(a->y), gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_wdif(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_wdif(val, gen_load_reg(a->y), gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_tdif(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_tdif(val, gen_load_reg(a->y), gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_odif(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_odif(val, gen_load_reg(a->y), gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_mux(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_mux(val, tcg_env, gen_load_reg(a->y),
                        gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_sadd(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    tcg_gen_andc_i64(val, gen_load_reg(a->y), gen_load_z(a, immediate));
    tcg_gen_ctpop_i64(val, val);
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_mor(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_mor(val, gen_load_reg(a->y), gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_mxor(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_mxor(val, gen_load_reg(a->y), gen_load_z(a, immediate));
    gen_store_reg(a->x, val);
    return true;
}

#define TRANS_BIT_DIFF(NAME, HELPER, IMMEDIATE) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_##HELPER(ctx, a, IMMEDIATE); \
    }

TRANS_BIT_DIFF(BDIF, bdif, false)
TRANS_BIT_DIFF(BDIFI, bdif, true)
TRANS_BIT_DIFF(WDIF, wdif, false)
TRANS_BIT_DIFF(WDIFI, wdif, true)
TRANS_BIT_DIFF(TDIF, tdif, false)
TRANS_BIT_DIFF(TDIFI, tdif, true)
TRANS_BIT_DIFF(ODIF, odif, false)
TRANS_BIT_DIFF(ODIFI, odif, true)
TRANS_BIT_DIFF(MUX, mux, false)
TRANS_BIT_DIFF(MUXI, mux, true)
TRANS_BIT_DIFF(SADD, sadd, false)
TRANS_BIT_DIFF(SADDI, sadd, true)
TRANS_BIT_DIFF(MOR, mor, false)
TRANS_BIT_DIFF(MORI, mor, true)
TRANS_BIT_DIFF(MXOR, mxor, false)
TRANS_BIT_DIFF(MXORI, mxor, true)

#undef TRANS_BIT_DIFF

static bool gen_cmp(DisasContext *ctx, arg_xyz *a, MMIXCompareOp op,
                    bool immediate)
{
    TCGv_i64 gt = tcg_temp_new_i64();
    TCGv_i64 lt = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();
    TCGv_i64 lhs = gen_load_reg(a->y);
    TCGv_i64 rhs = gen_load_z(a, immediate);

    if (op == MMIX_CMP_SIGNED) {
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

static bool trans_CMP(DisasContext *ctx, arg_xyz *a)
{
    return gen_cmp(ctx, a, MMIX_CMP_SIGNED, false);
}

static bool trans_CMPI(DisasContext *ctx, arg_xyz *a)
{
    return gen_cmp(ctx, a, MMIX_CMP_SIGNED, true);
}

static bool trans_CMPU(DisasContext *ctx, arg_xyz *a)
{
    return gen_cmp(ctx, a, MMIX_CMP_UNSIGNED, false);
}

static bool trans_CMPUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_cmp(ctx, a, MMIX_CMP_UNSIGNED, true);
}

static void gen_predicate(TCGv_i64 pred, TCGv_i64 val, MMIXPredicate predicate)
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

static bool gen_cond_result(arg_xyz *a, MMIXPredicate predicate, bool immediate,
                            bool zero_false)
{
    TCGv_i64 pred = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_predicate(pred, gen_load_reg(a->y), predicate);
    tcg_gen_movcond_i64(TCG_COND_NE, val, pred, tcg_constant_i64(0),
                        gen_load_z(a, immediate),
                        zero_false ? tcg_constant_i64(0) : gen_load_reg(a->x));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_cs(DisasContext *ctx, arg_xyz *a, MMIXPredicate predicate,
                   bool immediate)
{
    return gen_cond_result(a, predicate, immediate, false);
}

static bool gen_zs(DisasContext *ctx, arg_xyz *a, MMIXPredicate predicate,
                   bool immediate)
{
    return gen_cond_result(a, predicate, immediate, true);
}

#define TRANS_CS(NAME, PREDICATE, IMMEDIATE) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_cs(ctx, a, PREDICATE, IMMEDIATE); \
    }

#define TRANS_ZS(NAME, PREDICATE, IMMEDIATE) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_zs(ctx, a, PREDICATE, IMMEDIATE); \
    }

TRANS_CS(CSN, MMIX_PRED_NEGATIVE, false)
TRANS_CS(CSNI, MMIX_PRED_NEGATIVE, true)
TRANS_CS(CSZ, MMIX_PRED_ZERO, false)
TRANS_CS(CSZI, MMIX_PRED_ZERO, true)
TRANS_CS(CSP, MMIX_PRED_POSITIVE, false)
TRANS_CS(CSPI, MMIX_PRED_POSITIVE, true)
TRANS_CS(CSOD, MMIX_PRED_ODD, false)
TRANS_CS(CSODI, MMIX_PRED_ODD, true)
TRANS_CS(CSNN, MMIX_PRED_NONNEGATIVE, false)
TRANS_CS(CSNNI, MMIX_PRED_NONNEGATIVE, true)
TRANS_CS(CSNZ, MMIX_PRED_NONZERO, false)
TRANS_CS(CSNZI, MMIX_PRED_NONZERO, true)
TRANS_CS(CSNP, MMIX_PRED_NONPOSITIVE, false)
TRANS_CS(CSNPI, MMIX_PRED_NONPOSITIVE, true)
TRANS_CS(CSEV, MMIX_PRED_EVEN, false)
TRANS_CS(CSEVI, MMIX_PRED_EVEN, true)

TRANS_ZS(ZSN, MMIX_PRED_NEGATIVE, false)
TRANS_ZS(ZSNI, MMIX_PRED_NEGATIVE, true)
TRANS_ZS(ZSZ, MMIX_PRED_ZERO, false)
TRANS_ZS(ZSZI, MMIX_PRED_ZERO, true)
TRANS_ZS(ZSP, MMIX_PRED_POSITIVE, false)
TRANS_ZS(ZSPI, MMIX_PRED_POSITIVE, true)
TRANS_ZS(ZSOD, MMIX_PRED_ODD, false)
TRANS_ZS(ZSODI, MMIX_PRED_ODD, true)
TRANS_ZS(ZSNN, MMIX_PRED_NONNEGATIVE, false)
TRANS_ZS(ZSNNI, MMIX_PRED_NONNEGATIVE, true)
TRANS_ZS(ZSNZ, MMIX_PRED_NONZERO, false)
TRANS_ZS(ZSNZI, MMIX_PRED_NONZERO, true)
TRANS_ZS(ZSNP, MMIX_PRED_NONPOSITIVE, false)
TRANS_ZS(ZSNPI, MMIX_PRED_NONPOSITIVE, true)
TRANS_ZS(ZSEV, MMIX_PRED_EVEN, false)
TRANS_ZS(ZSEVI, MMIX_PRED_EVEN, true)

#undef TRANS_CS
#undef TRANS_ZS

static bool gen_branch(DisasContext *ctx, arg_xyz *a, MMIXPredicate predicate,
                       bool backward)
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
    tcg_gen_mov_i64(cpu_pc, dest);
    tcg_gen_addi_i64(cpu_npc, dest, 4);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

#define TRANS_BRANCH(NAME, PREDICATE, BACKWARD) \
    static bool trans_##NAME(DisasContext *ctx, arg_xyz *a) \
    { \
        return gen_branch(ctx, a, PREDICATE, BACKWARD); \
    }

TRANS_BRANCH(BN, MMIX_PRED_NEGATIVE, false)
TRANS_BRANCH(BNB, MMIX_PRED_NEGATIVE, true)
TRANS_BRANCH(BZ, MMIX_PRED_ZERO, false)
TRANS_BRANCH(BZB, MMIX_PRED_ZERO, true)
TRANS_BRANCH(BP, MMIX_PRED_POSITIVE, false)
TRANS_BRANCH(BPB, MMIX_PRED_POSITIVE, true)
TRANS_BRANCH(BOD, MMIX_PRED_ODD, false)
TRANS_BRANCH(BODB, MMIX_PRED_ODD, true)
TRANS_BRANCH(BNN, MMIX_PRED_NONNEGATIVE, false)
TRANS_BRANCH(BNNB, MMIX_PRED_NONNEGATIVE, true)
TRANS_BRANCH(BNZ, MMIX_PRED_NONZERO, false)
TRANS_BRANCH(BNZB, MMIX_PRED_NONZERO, true)
TRANS_BRANCH(BNP, MMIX_PRED_NONPOSITIVE, false)
TRANS_BRANCH(BNPB, MMIX_PRED_NONPOSITIVE, true)
TRANS_BRANCH(BEV, MMIX_PRED_EVEN, false)
TRANS_BRANCH(BEVB, MMIX_PRED_EVEN, true)
TRANS_BRANCH(PBN, MMIX_PRED_NEGATIVE, false)
TRANS_BRANCH(PBNB, MMIX_PRED_NEGATIVE, true)
TRANS_BRANCH(PBZ, MMIX_PRED_ZERO, false)
TRANS_BRANCH(PBZB, MMIX_PRED_ZERO, true)
TRANS_BRANCH(PBP, MMIX_PRED_POSITIVE, false)
TRANS_BRANCH(PBPB, MMIX_PRED_POSITIVE, true)
TRANS_BRANCH(PBOD, MMIX_PRED_ODD, false)
TRANS_BRANCH(PBODB, MMIX_PRED_ODD, true)
TRANS_BRANCH(PBNN, MMIX_PRED_NONNEGATIVE, false)
TRANS_BRANCH(PBNNB, MMIX_PRED_NONNEGATIVE, true)
TRANS_BRANCH(PBNZ, MMIX_PRED_NONZERO, false)
TRANS_BRANCH(PBNZB, MMIX_PRED_NONZERO, true)
TRANS_BRANCH(PBNP, MMIX_PRED_NONPOSITIVE, false)
TRANS_BRANCH(PBNPB, MMIX_PRED_NONPOSITIVE, true)
TRANS_BRANCH(PBEV, MMIX_PRED_EVEN, false)
TRANS_BRANCH(PBEVB, MMIX_PRED_EVEN, true)

#undef TRANS_BRANCH

static bool trans_GETA(DisasContext *ctx, arg_xyz *a)
{
    return gen_geta(ctx, a, false);
}

static bool trans_GETAB(DisasContext *ctx, arg_xyz *a)
{
    return gen_geta(ctx, a, true);
}

static bool trans_GET(DisasContext *ctx, arg_xyz *a)
{
    if (a->y != 0 || a->z >= MMIX_SREGS) {
        return gen_mmix_unsupported(ctx, "GET", a);
    }

    TCGv_i64 val = tcg_temp_new_i64();

    gen_helper_mmix_read_sreg(val, tcg_env, tcg_constant_i32(a->z));
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_put(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    if (a->y != 0 || a->x >= MMIX_SREGS) {
        return gen_mmix_unsupported(ctx, immediate ? "PUTI" : "PUT", a);
    }

    gen_helper_mmix_put_sreg(tcg_env, tcg_constant_i32(a->x),
                             gen_load_z(a, immediate));
    return true;
}

static bool trans_PUT(DisasContext *ctx, arg_xyz *a)
{
    return gen_put(ctx, a, false);
}

static bool trans_PUTI(DisasContext *ctx, arg_xyz *a)
{
    return gen_put(ctx, a, true);
}

static bool trans_JMP(DisasContext *ctx, arg_xyz *a)
{
    return gen_jmp(ctx, a, false);
}

static bool trans_JMPB(DisasContext *ctx, arg_xyz *a)
{
    return gen_jmp(ctx, a, true);
}

static bool trans_PUSHJ(DisasContext *ctx, arg_xyz *a)
{
    return gen_pushj(ctx, a, false);
}

static bool trans_PUSHJB(DisasContext *ctx, arg_xyz *a)
{
    return gen_pushj(ctx, a, true);
}

static bool trans_GO(DisasContext *ctx, arg_xyz *a)
{
    return gen_go(ctx, a, false);
}

static bool trans_GOI(DisasContext *ctx, arg_xyz *a)
{
    return gen_go(ctx, a, true);
}

static bool trans_PUSHGO(DisasContext *ctx, arg_xyz *a)
{
    return gen_pushgo(ctx, a, false);
}

static bool trans_PUSHGOI(DisasContext *ctx, arg_xyz *a)
{
    return gen_pushgo(ctx, a, true);
}

static bool trans_SAVE(DisasContext *ctx, arg_xyz *a)
{
    if (a->y != 0 || a->z != 0) {
        return gen_mmix_unsupported(ctx, "SAVE", a);
    }

    gen_helper_mmix_save(tcg_env, tcg_constant_i32(a->x));
    return true;
}

static bool trans_UNSAVE(DisasContext *ctx, arg_xyz *a)
{
    if (a->x != 0 || a->y != 0) {
        return gen_mmix_unsupported(ctx, "UNSAVE", a);
    }

    gen_helper_mmix_unsave(tcg_env, tcg_constant_i32(a->z));
    return true;
}

static bool gen_load_mem(DisasContext *ctx, arg_xyz *a, bool immediate,
                         MemOp memop, uint64_t align_mask)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, align_mask);
    tcg_gen_qemu_ld_i64(val, addr, 0, memop);
    gen_store_reg(a->x, val);
    return true;
}

static bool gen_ldht(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, 3);
    tcg_gen_qemu_ld_i64(val, addr, 0, MO_BEUL);
    tcg_gen_shli_i64(val, val, 32);
    gen_store_reg(a->x, val);
    return true;
}

static bool trans_LDB(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_SB, 0);
}

static bool trans_LDBI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_SB, 0);
}

static bool trans_LDBU(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_UB, 0);
}

static bool trans_LDBUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_UB, 0);
}

static bool trans_LDW(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BESW, 1);
}

static bool trans_LDWI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BESW, 1);
}

static bool trans_LDWU(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BEUW, 1);
}

static bool trans_LDWUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BEUW, 1);
}

static bool trans_LDT(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BESL, 3);
}

static bool trans_LDTI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BESL, 3);
}

static bool trans_LDTU(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BEUL, 3);
}

static bool trans_LDTUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BEUL, 3);
}

static bool trans_LDO(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BEUQ, 7);
}

static bool trans_LDOI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BEUQ, 7);
}

static bool trans_LDOU(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BEUQ, 7);
}

static bool trans_LDOUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BEUQ, 7);
}

static bool trans_LDUNC(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, false, MO_BEUQ, 7);
}

static bool trans_LDUNCI(DisasContext *ctx, arg_xyz *a)
{
    return gen_load_mem(ctx, a, true, MO_BEUQ, 7);
}

static bool trans_LDHT(DisasContext *ctx, arg_xyz *a)
{
    return gen_ldht(ctx, a, false);
}

static bool trans_LDHTI(DisasContext *ctx, arg_xyz *a)
{
    return gen_ldht(ctx, a, true);
}

static bool gen_ldsf(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, 3);
    tcg_gen_qemu_ld_i64(val, addr, 0, MO_BEUL);
    gen_helper_mmix_ldsf(val, val);
    gen_store_reg(a->x, val);
    return true;
}

static bool trans_LDSF(DisasContext *ctx, arg_xyz *a)
{
    return gen_ldsf(ctx, a, false);
}

static bool trans_LDSFI(DisasContext *ctx, arg_xyz *a)
{
    return gen_ldsf(ctx, a, true);
}

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
    tcg_gen_atomic_cmpxchg_i64(old, addr, rp, new, 0, MO_BEUQ);

    tcg_gen_movcond_i64(TCG_COND_EQ, next_rp, old, rp, rp, old);
    gen_helper_mmix_put_sreg(tcg_env, tcg_constant_i32(MMIX_SREG_RP), next_rp);

    tcg_gen_movcond_i64(TCG_COND_EQ, success, old, rp,
                        tcg_constant_i64(1), tcg_constant_i64(0));
    gen_store_reg(a->x, success);
    return true;
}

static bool trans_CSWAP(DisasContext *ctx, arg_xyz *a)
{
    return gen_cswap(ctx, a, false);
}

static bool trans_CSWAPI(DisasContext *ctx, arg_xyz *a)
{
    return gen_cswap(ctx, a, true);
}

static bool trans_PRELD(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

static bool trans_PRELDI(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

static bool trans_PREGO(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

static bool trans_PREGOI(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

static bool gen_store_mem(DisasContext *ctx, arg_xyz *a, bool immediate,
                          MemOp memop, uint64_t align_mask)
{
    TCGv_i64 addr = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, align_mask);
    tcg_gen_qemu_st_i64(gen_load_reg(a->x), addr, 0, memop);
    return true;
}

static bool gen_stht(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, 3);
    tcg_gen_shri_i64(val, gen_load_reg(a->x), 32);
    tcg_gen_qemu_st_i64(val, addr, 0, MO_BEUL);
    return true;
}

static bool trans_STB(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_UB, 0);
}

static bool trans_STBI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_UB, 0);
}

static bool trans_STBU(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_UB, 0);
}

static bool trans_STBUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_UB, 0);
}

static bool trans_STW(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUW, 1);
}

static bool trans_STWI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUW, 1);
}

static bool trans_STWU(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUW, 1);
}

static bool trans_STWUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUW, 1);
}

static bool trans_STT(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUL, 3);
}

static bool trans_STTI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUL, 3);
}

static bool trans_STTU(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUL, 3);
}

static bool trans_STTUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUL, 3);
}

static bool trans_STO(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUQ, 7);
}

static bool trans_STOI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUQ, 7);
}

static bool trans_STOU(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUQ, 7);
}

static bool trans_STOUI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUQ, 7);
}

static bool trans_STUNC(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, false, MO_BEUQ, 7);
}

static bool trans_STUNCI(DisasContext *ctx, arg_xyz *a)
{
    return gen_store_mem(ctx, a, true, MO_BEUQ, 7);
}

static bool trans_STHT(DisasContext *ctx, arg_xyz *a)
{
    return gen_stht(ctx, a, false);
}

static bool trans_STHTI(DisasContext *ctx, arg_xyz *a)
{
    return gen_stht(ctx, a, true);
}

static bool gen_stsf(DisasContext *ctx, arg_xyz *a, bool immediate)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 val = tcg_temp_new_i64();

    gen_effective_address(addr, a, immediate, 3);
    gen_helper_mmix_stsf(val, tcg_env, tcg_constant_i32(ctx->insn), addr,
                         gen_load_reg(a->x));
    tcg_gen_qemu_st_i64(val, addr, 0, MO_BEUL);
    return true;
}

static bool trans_STSF(DisasContext *ctx, arg_xyz *a)
{
    return gen_stsf(ctx, a, false);
}

static bool trans_STSFI(DisasContext *ctx, arg_xyz *a)
{
    return gen_stsf(ctx, a, true);
}

static bool trans_SYNCD(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

static bool trans_SYNCDI(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

static bool trans_PREST(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

static bool trans_PRESTI(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

static bool trans_SYNCID(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

static bool trans_SYNCIDI(DisasContext *ctx, arg_xyz *a)
{
    return true;
}

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
        return gen_mmix_invalid_sync(ctx, a->xyz);
    }
}

static void mmix_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    ctx->env = cpu_env(cs);
}

static void mmix_tr_tb_start(DisasContextBase *dcbase, CPUState *cs)
{
}

static void mmix_tr_insn_start(DisasContextBase *dcbase, CPUState *cs)
{
    tcg_gen_insn_start(dcbase->pc_next, 0, 0);
}

static void mmix_tr_translate_insn(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    vaddr pc = ctx->base.pc_next;
    uint32_t insn;

    insn = translator_ldl_end(ctx->env, &ctx->base, pc, MO_BE);
    ctx->insn_pc = pc;
    ctx->insn = insn;
    ctx->base.pc_next += 4;

    tcg_gen_movi_i64(cpu_pc, pc);
    tcg_gen_movi_i64(cpu_npc, ctx->base.pc_next);
    if (!decode(ctx, insn)) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX unknown opcode 0x%02x at 0x%016" VADDR_PRIx
                      " insn=0x%08x\n",
                      insn >> 24, pc, insn);
        gen_raise_illegal(ctx);
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
