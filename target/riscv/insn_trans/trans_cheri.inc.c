/*
 * RISC-V translation routines for the CHERI Extension.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2020 Alex Richardson <Alexander.Richardson@cl.cam.ac.uk>
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory (Department of Computer Science and
 * Technology) under DARPA contract HR0011-18-C-0016 ("ECATS"), as part of the
 * DARPA SSITH research programme.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define INSN_CAN_TRAP(ctx) tcg_gen_movi_tl(cpu_pc, ctx->base.pc_next)

#define TRANSLATE_MAYBE_TRAP(name, gen_helper, ...)                            \
    static bool trans_##name(DisasContext *ctx, arg_##name *a)                 \
    {                                                                          \
        INSN_CAN_TRAP(ctx);                                                    \
        return gen_helper(__VA_ARGS__, &gen_helper_##name);                    \
    }
#define TRANSLATE_NO_TRAP(name, gen_helper, ...)                               \
    static bool trans_##name(DisasContext *ctx, arg_##name *a)                 \
    {                                                                          \
        return gen_helper(__VA_ARGS__, &gen_helper_##name);                    \
    }

typedef void(cheri_cget_helper)(TCGv, TCGv_ptr, TCGv_i32);
static inline bool gen_cheri_get(int rd, int cs, cheri_cget_helper *gen_func)
{
    TCGv_i32 source_regnum = tcg_const_i32(cs);
    TCGv result = tcg_temp_new();
    gen_func(result, cpu_env, source_regnum);
    gen_set_gpr(rd, result);
    tcg_temp_free(result);
    tcg_temp_free_i32(source_regnum);
    return true;
}

#define TRANSLATE_CGET(name)                                                   \
    TRANSLATE_NO_TRAP(name, gen_cheri_get, a->rd, a->rs1)

typedef void(cheri_cap_cap_helper)(TCGv_ptr, TCGv_i32, TCGv_i32);
static inline bool gen_cheri_cap_cap(int cd, int cs,
                                     cheri_cap_cap_helper *gen_func)
{
    TCGv_i32 dest_regnum = tcg_const_i32(cd);
    TCGv_i32 source_regnum = tcg_const_i32(cs);
    gen_func(cpu_env, dest_regnum, source_regnum);
    gen_rvfi_dii_set_field_const(rd_addr, cd);
    tcg_temp_free_i32(source_regnum);
    tcg_temp_free_i32(dest_regnum);
    return true;
}
#define TRANSLATE_CAP_CAP(name)                                                \
    TRANSLATE_MAYBE_TRAP(name, gen_cheri_cap_cap, a->rd, a->rs1)
#define TRANSLATE_CAP_CAP_NO_TRAP(name)                                        \
    TRANSLATE_NO_TRAP(name, gen_cheri_cap_cap, a->rd, a->rs1)

typedef void(cheri_cap_int_helper)(TCGv_ptr, TCGv_i32, TCGv);
static inline bool gen_cheri_cap_int(int cd, int rs,
                                     cheri_cap_int_helper *gen_func)
{
    TCGv_i32 dest_regnum = tcg_const_i32(cd);
    TCGv gpr_value = tcg_temp_new();
    gen_get_gpr(gpr_value, rs);
    gen_func(cpu_env, dest_regnum, gpr_value);
    gen_rvfi_dii_set_field_const(rd_addr, cd);
    tcg_temp_free(gpr_value);
    tcg_temp_free_i32(dest_regnum);
    return true;
}

typedef void(cheri_cap_cap_cap_helper)(TCGv_ptr, TCGv_i32, TCGv_i32, TCGv_i32);
static inline bool gen_cheri_cap_cap_cap(int cd, int cs1, int cs2,
                                         cheri_cap_cap_cap_helper *gen_func)
{
    TCGv_i32 dest_regnum = tcg_const_i32(cd);
    TCGv_i32 source_regnum1 = tcg_const_i32(cs1);
    TCGv_i32 source_regnum2 = tcg_const_i32(cs2);
    gen_func(cpu_env, dest_regnum, source_regnum1, source_regnum2);
    gen_rvfi_dii_set_field_const(rd_addr, cd);
    tcg_temp_free_i32(source_regnum2);
    tcg_temp_free_i32(source_regnum1);
    tcg_temp_free_i32(dest_regnum);
    return true;
}
// We assume that all these instructions can trap (e.g. seal violation)
#define TRANSLATE_CAP_CAP_CAP(name)                                            \
    TRANSLATE_MAYBE_TRAP(name, gen_cheri_cap_cap_cap, a->rd, a->rs1, a->rs2)

typedef void(cheri_cap_cap_int_helper)(TCGv_ptr, TCGv_i32, TCGv_i32, TCGv);
static inline bool gen_cheri_cap_cap_int(int cd, int cs1, int rs2,
                                         cheri_cap_cap_int_helper *gen_func)
{
    TCGv_i32 dest_regnum = tcg_const_i32(cd);
    TCGv_i32 source_regnum = tcg_const_i32(cs1);
    TCGv gpr_value = tcg_temp_new();
    gen_get_gpr(gpr_value, rs2);
    gen_func(cpu_env, dest_regnum, source_regnum, gpr_value);
    gen_rvfi_dii_set_field_const(rd_addr, cd);
    tcg_temp_free(gpr_value);
    tcg_temp_free_i32(source_regnum);
    tcg_temp_free_i32(dest_regnum);
    return true;
}
#define TRANSLATE_CAP_CAP_INT(name)                                            \
    TRANSLATE_MAYBE_TRAP(name, gen_cheri_cap_cap_int, a->rd, a->rs1, a->rs2)

typedef void(cheri_int_cap_cap_helper)(TCGv, TCGv_ptr, TCGv_i32, TCGv_i32);
static inline bool gen_cheri_int_cap_cap(int rd, int cs1, int cs2,
                                         cheri_int_cap_cap_helper *gen_func)
{
    TCGv_i32 source_regnum1 = tcg_const_i32(cs1);
    TCGv_i32 source_regnum2 = tcg_const_i32(cs2);
    TCGv result = tcg_temp_new();
    gen_func(result, cpu_env, source_regnum1, source_regnum2);
    gen_set_gpr(rd, result);
    tcg_temp_free(result);
    tcg_temp_free_i32(source_regnum2);
    tcg_temp_free_i32(source_regnum1);
    return true;
}
#define TRANSLATE_INT_CAP_CAP(name)                                            \
    TRANSLATE_MAYBE_TRAP(name, gen_cheri_int_cap_cap, a->rd, a->rs1, a->rs2)

// TODO: all of these could be implemented in TCG without calling a helper
// Two operand (int cap)
TRANSLATE_CGET(cgetaddr)
TRANSLATE_CGET(cgetbase)
TRANSLATE_CGET(cgetflags)
TRANSLATE_CGET(cgetlen)
TRANSLATE_CGET(cgetoffset)
TRANSLATE_CGET(cgetperm)
TRANSLATE_CGET(cgettag)
TRANSLATE_CGET(cgettype)
TRANSLATE_CGET(cgetsealed)

// Two operand (cap int)
TRANSLATE_MAYBE_TRAP(ccheckperm, gen_cheri_cap_int, a->rd, a->rs1)

// Two operand (cap cap)
TRANSLATE_CAP_CAP_NO_TRAP(ccleartag)
TRANSLATE_CAP_CAP(cchecktype)
TRANSLATE_CAP_CAP_NO_TRAP(cmove)

// Three operand (cap cap cap)
TRANSLATE_CAP_CAP_CAP(cbuildcap)
TRANSLATE_CAP_CAP_CAP(ccopytype)
TRANSLATE_CAP_CAP_CAP(ccseal)
TRANSLATE_CAP_CAP_CAP(cseal)
TRANSLATE_CAP_CAP_CAP(cunseal)
// Not quite (cap cap cap) but the index argument can be handled the same way
TRANSLATE_CAP_CAP_CAP(cspecialrw)

// Three operand (cap cap int)
TRANSLATE_CAP_CAP_INT(candperm)
TRANSLATE_CAP_CAP_INT(cfromptr)
TRANSLATE_CAP_CAP_INT(cincoffset)
TRANSLATE_CAP_CAP_INT(csetaddr)
TRANSLATE_CAP_CAP_INT(csetbounds)
TRANSLATE_CAP_CAP_INT(csetboundsexact)
TRANSLATE_CAP_CAP_INT(csetflags)
TRANSLATE_CAP_CAP_INT(csetoffset)

// Three operand (int cap cap)
TRANSLATE_INT_CAP_CAP(csub)
TRANSLATE_INT_CAP_CAP(ctestsubset)
TRANSLATE_INT_CAP_CAP(ctoptr)

// CIncOffsetImm/CSetBoundsImm:
typedef void(cheri_cap_cap_imm_helper)(TCGv_ptr, TCGv_i32, TCGv_i32, TCGv);
static inline bool gen_cheri_cap_cap_imm(int cd, int cs1, target_long imm,
                                         cheri_cap_cap_imm_helper *gen_func)
{
    TCGv_i32 dest_regnum = tcg_const_i32(cd);
    TCGv_i32 source_regnum = tcg_const_i32(cs1);
    TCGv imm_value = tcg_const_tl(imm);
    gen_func(cpu_env, dest_regnum, source_regnum, imm_value);
    gen_rvfi_dii_set_field_const(rd_addr, cd);
    tcg_temp_free(imm_value);
    tcg_temp_free_i32(source_regnum);
    tcg_temp_free_i32(dest_regnum);
    return true;
}
#define TRANSLATE_CAP_CAP_IMM(name)                                            \
    TRANSLATE_MAYBE_TRAP(name, gen_cheri_cap_cap_int, a->rd, a->rs1, a->imm)

static bool trans_cincoffsetimm(DisasContext *ctx, arg_cincoffsetimm *a)
{
    INSN_CAN_TRAP(ctx);
    return gen_cheri_cap_cap_imm(a->rd, a->rs1, a->imm, &gen_helper_cincoffset);
}

static bool trans_csetboundsimm(DisasContext *ctx, arg_cincoffsetimm *a)
{
    INSN_CAN_TRAP(ctx);
    tcg_debug_assert(a->imm >= 0);
    return gen_cheri_cap_cap_imm(a->rd, a->rs1, a->imm, &gen_helper_csetbounds);
}

/// Control-flow instructions
static bool trans_cjalr(DisasContext *ctx, arg_cjalr *a)
{
    INSN_CAN_TRAP(ctx);
    TCGv_i32 dest_regnum = tcg_const_i32(a->rd);
    TCGv_i32 source_regnum = tcg_const_i32(a->rs1);
    TCGv t0 = tcg_const_tl(ctx->pc_succ_insn); // Link addr + resulting $pc
    gen_helper_cjalr(t0, cpu_env, dest_regnum, source_regnum, t0);
    gen_rvfi_dii_set_field_const(rd_addr, a->rd);
    tcg_temp_free(t0);
    tcg_temp_free_i32(source_regnum);
    tcg_temp_free_i32(dest_regnum);
    gen_rvfi_dii_validate_jump(ctx);

    // PC has been updated -> exit translation block
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

// Loads
static bool gen_ddc_load(DisasContext *ctx, int rd, int rs1, MemOp memop)
{
    INSN_CAN_TRAP(ctx);
    TCGv addr = tcg_temp_new();
    TCGv value = tcg_temp_new();
    gen_get_gpr(addr, rs1);
    tcg_gen_qemu_ld_ddc_tl(value, /* Update addr in-place */ NULL, addr,
                           ctx->mem_idx, memop);
    gen_set_gpr(rd, value);
    tcg_temp_free(addr);
    tcg_temp_free(value);
    return true;
}
#define TRANSLATE_DDC_LOAD(name, op)                                           \
    static bool trans_##name(DisasContext *ctx, arg_##name *a)                 \
    {                                                                          \
        return gen_ddc_load(ctx, a->rd, a->rs1, op);                           \
    }
TRANSLATE_DDC_LOAD(lbddc, MO_SB)
TRANSLATE_DDC_LOAD(lhddc, MO_SW)
TRANSLATE_DDC_LOAD(lwddc, MO_SL)
#ifdef TARGET_RISCV64
TRANSLATE_DDC_LOAD(ldddc, MO_Q)
#endif
TRANSLATE_DDC_LOAD(lbuddc, MO_UB)
TRANSLATE_DDC_LOAD(lhuddc, MO_UW)
#ifdef TARGET_RISCV64
TRANSLATE_DDC_LOAD(lwuddc, MO_UL)
#endif

static inline bool trans_lcddc(DisasContext *ctx, arg_lcddc *a)
{
    // always uses DDC as the base register
    INSN_CAN_TRAP(ctx);
    return gen_cheri_cap_cap_int(a->rd, CHERI_EXC_REGNUM_DDC, a->rs1, &gen_helper_load_cap_via_cap);
}

static inline bool trans_lccap(DisasContext *ctx, arg_lccap *a)
{
    // No immediate available for lccap
    INSN_CAN_TRAP(ctx);
    return gen_cheri_cap_cap_imm(a->rd, a->rs1, 0, &gen_helper_load_cap_via_cap);
}

/* Load Via Capability Register */
static inline bool gen_cap_load(DisasContext *ctx, int32_t rd, int32_t cs,
                                target_long offset, MemOp op)
{
    // FIXME: just do everything in the helper
    INSN_CAN_TRAP(ctx);
    TCGv_i32 tcs = tcg_const_i32(cs);
    TCGv toffset = tcg_const_tl(offset);
    TCGv_cap_checked_ptr vaddr = tcg_temp_new_cap_checked();
    TCGv value = tcg_temp_new();
    TCGv_i32 tsize = tcg_const_i32(memop_size(op));
    gen_helper_cload_check(vaddr, cpu_env, tcs, toffset, tsize);
    tcg_gen_qemu_ld_tl_with_checked_addr(value, vaddr, ctx->mem_idx, op);
    gen_set_gpr(rd, value);
    tcg_temp_free_i32(tsize);
    tcg_temp_free(value);
    tcg_temp_free_cap_checked(vaddr);
    tcg_temp_free(toffset);
    tcg_temp_free_i32(tcs);
    return true;
}
#define TRANSLATE_CAP_LOAD(name, op)                                           \
    static bool trans_##name(DisasContext *ctx, arg_##name *a)                 \
    {                                                                          \
        return gen_cap_load(ctx, a->rd, a->rs1, /*offset=*/0, op);             \
    }
TRANSLATE_CAP_LOAD(lbcap, MO_SB)
TRANSLATE_CAP_LOAD(lhcap, MO_SW)
TRANSLATE_CAP_LOAD(lwcap, MO_SL)
#ifdef TARGET_RISCV64
TRANSLATE_CAP_LOAD(ldcap, MO_Q)
#endif
TRANSLATE_CAP_LOAD(lbucap, MO_UB)
TRANSLATE_CAP_LOAD(lhucap, MO_UW)
#ifdef TARGET_RISCV64
TRANSLATE_CAP_LOAD(lwucap, MO_UL)
#endif

// Stores
static bool gen_ddc_store(DisasContext *ctx, int rs1, int rs2, MemOp memop)
{
    INSN_CAN_TRAP(ctx);
    TCGv addr = tcg_temp_new();
    TCGv value = tcg_temp_new();
    gen_get_gpr(addr, rs1);
    gen_get_gpr(value, rs2);
    tcg_gen_qemu_st_ddc_tl(value, /* Update addr in-place */ NULL, addr,
                           ctx->mem_idx, memop);
    tcg_temp_free(value);
    tcg_temp_free(addr);
    return true;
}
#define TRANSLATE_DDC_STORE(name, op)                                          \
    static bool trans_##name(DisasContext *ctx, arg_##name *a)                 \
    {                                                                          \
        return gen_ddc_store(ctx, a->rs1, a->rs2, op);                         \
    }

TRANSLATE_DDC_STORE(sbddc, MO_UB)
TRANSLATE_DDC_STORE(shddc, MO_UW)
TRANSLATE_DDC_STORE(swddc, MO_UL)
#ifdef TARGET_RISCV64
TRANSLATE_DDC_STORE(sdddc, MO_Q)
#endif

/* Load Via Capability Register */
static inline bool gen_cap_store(DisasContext *ctx, int32_t addr_regnum,
                                 int32_t val_regnum, target_long offset,
                                 MemOp op)
{
    INSN_CAN_TRAP(ctx);
    // FIXME: just do everything in the helper
    TCGv_i32 tcs = tcg_const_i32(addr_regnum);
    TCGv toffset = tcg_const_tl(offset);
    TCGv_cap_checked_ptr vaddr = tcg_temp_new_cap_checked();

    TCGv_i32 tsize = tcg_const_i32(memop_size(op));
    gen_helper_cstore_check(vaddr, cpu_env, tcs, toffset, tsize);
    tcg_temp_free_i32(tsize);
    tcg_temp_free(toffset);
    tcg_temp_free_i32(tcs);

    TCGv value = tcg_temp_new();
    gen_get_gpr(value, val_regnum);
    tcg_gen_qemu_st_tl_with_checked_addr(value, vaddr, ctx->mem_idx, op);
    tcg_temp_free(value);
    tcg_temp_free_cap_checked(vaddr);
    return true;
}
#define TRANSLATE_CAP_STORE(name, op)                                          \
    static bool trans_##name(DisasContext *ctx, arg_##name *a)                 \
    {                                                                          \
        return gen_cap_store(ctx, a->rs1, a->rs2, /*offset=*/0, op);           \
    }

TRANSLATE_CAP_STORE(sbcap, MO_UB)
TRANSLATE_CAP_STORE(shcap, MO_UW)
TRANSLATE_CAP_STORE(swcap, MO_UL)
#ifdef TARGET_RISCV64
TRANSLATE_CAP_STORE(sdcap, MO_Q)
#endif

// RS2 is the value, RS1 is the capability/ddc offset
static inline bool trans_scddc(DisasContext *ctx, arg_scddc *a)
{
    // always uses DDC as the base register
    INSN_CAN_TRAP(ctx);
    return gen_cheri_cap_cap_int(a->rs2, CHERI_EXC_REGNUM_DDC, a->rs1, &gen_helper_load_cap_via_cap);
}

static inline bool trans_sccap(DisasContext *ctx, arg_sccap *a)
{
    // No immediate available for sccap
    INSN_CAN_TRAP(ctx);
    return gen_cheri_cap_cap_imm(a->rs2, a->rs1, /*offset=*/0, &gen_helper_store_cap_via_cap);
}

static inline bool trans_clc(DisasContext *ctx, arg_clc *a)
{
    INSN_CAN_TRAP(ctx);
    return gen_cheri_cap_cap_imm(a->rd, a->rs1, /*offset=*/a->imm, &gen_helper_load_cap_via_cap);
}

static inline bool trans_csc(DisasContext *ctx, arg_csc *a)
{
    INSN_CAN_TRAP(ctx);
    // RS2 is the value, RS1 is the capability
    return gen_cheri_cap_cap_imm(a->rs2, a->rs1, /*offset=*/a->imm, &gen_helper_store_cap_via_cap);
}
