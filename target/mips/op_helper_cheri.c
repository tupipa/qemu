/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2015-2016 Stacey Son <sson@FreeBSD.org>
 * Copyright (c) 2016-2018 Alfredo Mazzinghi <am2419@cl.cam.ac.uk>
 * Copyright (c) 2016-2018 Alex Richardson <Alexander.Richardson@cl.cam.ac.uk>
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract FA8750-10-C-0237
 * ("CTSRD"), as part of the DARPA CRASH research programme.
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
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "internal.h"
#include "qemu/host-utils.h"
#include "qemu/error-report.h"
#include "qemu/qemu-print.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "exec/helper-proto.h"

#ifndef TARGET_CHERI
#error "This file should only be compiled for CHERI"
#endif

#include "cheri_tagmem.h"
#include "cheri-lazy-capregs.h"
#include "cheri-bounds-stats.h"

#include "disas/disas.h"
#include "disas/dis-asm.h"

const char *cp2_fault_causestr[] = {
    "None",
    "Length Violation",
    "Tag Violation",
    "Seal Violation",
    "Type Violation",
    "Call Trap",
    "Return Trap",
    "Underflow of Trusted System Stack",
    "User-defined Permission Violation",
    "TLB prohibits Store Capability",
    "Bounds Cannot Be Represented Exactly",
    "Reserved 0x0b",
    "Reserved 0x0c",
    "Reserved 0x0d",
    "Reserved 0x0e",
    "Reserved 0x0f",
    "Global Violation",
    "Permit_Execute Violation",
    "Permit_Load Violation",
    "Permit_Store Violation",
    "Permit_Load_Capability Violation",
    "Permit_Store_Capability Violation",
    "Permit_Store_Local_Capability Violation",
    "Permit_Seal Violation",
    "Access_Sys_Reg Violation",
    "Permit_CCall Violation",
    "Access_EPCC Violation",
    "Access_KDC Violation",
    "Access_KCC Violation",
    "Access_KR1C Violation",
    "Access_KR2C Violation"
};


#ifdef __clang__
#pragma clang diagnostic error "-Wdeprecated-declarations"
#else
#pragma GCC diagnostic error "-Wdeprecated-declarations"
#endif
#define CHERI_HELPER_IMPL(name) \
    __attribute__((deprecated("Do not call the helper directly, it will crash at runtime. Call the _impl variant instead"))) helper_##name

#ifdef DO_CHERI_STATISTICS

DEFINE_CHERI_STAT(cfromptr);
static DEFINE_CHERI_STAT(cgetpccsetoffset);
static DEFINE_CHERI_STAT(cgetpccincoffset);
static DEFINE_CHERI_STAT(cgetpccsetaddr);

#endif

void cheri_cpu_dump_statistics_f(CPUState *cs, FILE* f, int flags)
{
#ifndef DO_CHERI_STATISTICS
    qemu_fprintf(f, "CPUSTATS DISABLED, RECOMPILE WITH -DDO_CHERI_STATISTICS\n");
#else
    dump_out_of_bounds_stats(f, &oob_info_cincoffset);
    dump_out_of_bounds_stats(f, &oob_info_csetoffset);
    dump_out_of_bounds_stats(f, &oob_info_csetaddr);
    dump_out_of_bounds_stats(f, &oob_info_candaddr);
    dump_out_of_bounds_stats(f, &oob_info_cfromptr);
    dump_out_of_bounds_stats(f, &oob_info_cgetpccsetoffset);
    dump_out_of_bounds_stats(f, &oob_info_cgetpccincoffset);
    dump_out_of_bounds_stats(f, &oob_info_cgetpccsetaddr);
#endif
}

void cheri_cpu_dump_statistics(CPUState *cs, int flags) {
    cheri_cpu_dump_statistics_f(cs, NULL, flags);
}


static inline bool
is_cap_sealed(const cap_register_t *cp)
{
    // TODO: remove this function and update all callers to use the correct function
    return cap_is_sealed_with_type(cp) || cap_is_sealed_entry(cp);
}

void qemu_log_capreg(const cap_register_t *cr, const char* prefix, const char* name) {
    qemu_log("%s%s|" PRINT_CAP_FMTSTR_L1 "\n"
             "             |" PRINT_CAP_FMTSTR_L2 "\n",
             prefix, name, PRINT_CAP_ARGS_L1(cr), PRINT_CAP_ARGS_L2(cr));
}

static inline int align_of(int size, uint64_t addr)
{

    switch(size) {
    case 1:
        return 0;
    case 2:
        return (addr & 0x1);
    case 4:
        return (addr & 0x3);
    case 8:
        return (addr & 0x7);
    case 16:
        return (addr & 0xf);
    case 32:
        return (addr & 0x1f);
    case 64:
        return (addr & 0x3f);
    case 128:
        return (addr & 0x7f);
    default:
        return 1;
    }
}

static bool cap_exactly_equal(const cap_register_t *cbp, const cap_register_t *ctp);

static inline void update_ddc(CPUArchState *env, const cap_register_t* new_ddc) {
    if (!cap_exactly_equal(&env->active_tc.CHWR.DDC, new_ddc)) {
        qemu_log_mask(CPU_LOG_INSTR | CPU_LOG_MMU, "Flushing TCG TLB since $ddc is changing to " PRINT_CAP_FMTSTR "\n", PRINT_CAP_ARGS(new_ddc));
        // TODO: in the future we may want to move $ddc to the guest -> host addr
        // translation. This would allow skipping $ddc checks for all pages that
        // are fully covered by $ddc for the second load/store check
        // (QEMU has separate TLBs for both cases already).
        // If we implment this, we will have to flush the entire TLB whenever
        // $ddc changes (or at least flush all pages affected by the $ddc chaged)
        // XXX: tlb_flush(env_cpu(env));
        env->active_tc.CHWR.DDC = *new_ddc;
    } else {
        qemu_log_mask(CPU_LOG_INSTR | CPU_LOG_MMU, "Installing same $ddc again, not flushing TCG TLB: " PRINT_CAP_FMTSTR "\n", PRINT_CAP_ARGS(new_ddc));
    }
}

static inline target_ulong
clear_tag_if_no_loadcap(target_ulong tag, const cap_register_t* cbp, int prot) {
    if (tag && ((prot & PAGE_LC_CLEAR) || !(cbp->cr_perms & CAP_PERM_LOAD_CAP))) {
        if (qemu_loglevel_mask(CPU_LOG_INSTR)) {
            qemu_log("Clearing tag bit due to missing %s\n",
                     prot & PAGE_LC_CLEAR ? "TLB_L" : "CAP_PERM_LOAD_CAP");
        }
        return 0;
    }
    return tag;
}

target_ulong CHERI_HELPER_IMPL(cbez(CPUArchState *env, uint32_t cb, uint32_t offset))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    /*
     * CBEZ: Branch if NULL
     */
    /*
     * Compare the only semantically meaningful fields of int_to_cap(0)
     */
    if (cap_get_base(cbp) == 0 && cbp->cr_tag == 0 && cap_get_cursor(cbp) == 0)
        return (target_ulong)1;
    else
        return (target_ulong)0;
}

target_ulong CHERI_HELPER_IMPL(cbnz(CPUArchState *env, uint32_t cb, uint32_t offset))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    /*
     * CBEZ: Branch if not NULL
     */
    /*
     * Compare the only semantically meaningful fields of int_to_cap(0)
     */
    if (cap_get_base(cbp) == 0 && cbp->cr_tag == 0 && cap_get_cursor(cbp) == 0)
        return (target_ulong)0;
    else
        return (target_ulong)1;
}

target_ulong CHERI_HELPER_IMPL(cbts(CPUArchState *env, uint32_t cb, uint32_t offset))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    /*
     * CBTS: Branch if tag is set
     */
    return (target_ulong)cbp->cr_tag;
}

target_ulong CHERI_HELPER_IMPL(cbtu(CPUArchState *env, uint32_t cb, uint32_t offset))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    /*
     * CBTU: Branch if tag is unset
     */
    return (target_ulong)!cbp->cr_tag;
}

static target_ulong ccall_common(CPUArchState *env, uint32_t cs, uint32_t cb, uint32_t selector, uintptr_t _host_return_address)
{
    const cap_register_t *csp = get_readonly_capreg(env, cs);
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    /*
     * CCall: Call into a new security domain
     */
    if (!csp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cs);
    } else if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (!cap_is_sealed_with_type(csp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cs);
    } else if (!cap_is_sealed_with_type(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (csp->cr_otype != cbp->cr_otype || csp->cr_otype > CAP_LAST_NONRESERVED_OTYPE) {
        raise_cheri_exception(env, CapEx_TypeViolation, cs);
    } else if (!(csp->cr_perms & CAP_PERM_EXECUTE)) {
        raise_cheri_exception(env, CapEx_PermitExecuteViolation, cs);
    } else if (cbp->cr_perms & CAP_PERM_EXECUTE) {
        raise_cheri_exception(env, CapEx_PermitExecuteViolation, cb);
    } else if (!cap_is_in_bounds(csp, cap_get_cursor(csp), 1)) {
        // TODO: check for at least one instruction worth of data? Like cjr/cjalr?
        raise_cheri_exception(env, CapEx_LengthViolation, cs);
    } else {
        if (selector == CCALL_SELECTOR_0) {
            raise_cheri_exception(env, CapEx_CallTrap, cs);
        } else if (!(csp->cr_perms & CAP_PERM_CCALL)){
            raise_cheri_exception(env, CapEx_PermitCCallViolation, cs);
        } else if (!(cbp->cr_perms & CAP_PERM_CCALL)){
            raise_cheri_exception(env, CapEx_PermitCCallViolation, cb);
        } else {
            cap_register_t idc = *cbp;
            cap_set_unsealed(&idc);
            update_capreg(env, CP2CAP_IDC, &idc);
            // The capability register is loaded into PCC during delay slot
            env->active_tc.CapBranchTarget = *csp;
            // XXXAR: clearing these fields is not strictly needed since they
            // aren't copied from the CapBranchTarget to $pcc but it does make
            // the LOG_INSTR output less confusing.
            cap_set_unsealed(&env->active_tc.CapBranchTarget);
            // Return the branch target address
            return cap_get_cursor(csp);
        }
    }
    return (target_ulong)0;
}

void CHERI_HELPER_IMPL(ccall(CPUArchState *env, uint32_t cs, uint32_t cb))
{
    (void)ccall_common(env, cs, cb, CCALL_SELECTOR_0, GETPC());
}

target_ulong CHERI_HELPER_IMPL(ccall_notrap(CPUArchState *env, uint32_t cs, uint32_t cb))
{
    return ccall_common(env, cs, cb, CCALL_SELECTOR_1, GETPC());
}

void CHERI_HELPER_IMPL(cclearreg(CPUArchState *env, uint32_t mask))
{
    // Register zero means $ddc here since it is useful to clear $ddc on a
    // sandbox switch whereas clearing $NULL is useless
    if (mask & 0x1) {
        update_ddc(env, get_readonly_capreg(env, 0)); // nullify $ddc
    }

    for (int creg = 1; creg < 32; creg++) {
        if (mask & (0x1 << creg)) {
            nullify_capreg(env, creg);
        }
    }
}

void CHERI_HELPER_IMPL(creturn(CPUArchState *env)) {
    do_raise_c2_exception_noreg(env, CapEx_ReturnTrap, GETPC());
}

void CHERI_HELPER_IMPL(cfromptr(CPUArchState *env, uint32_t cd, uint32_t cb,
        target_ulong rt))
{
    GET_HOST_RETPC();
#ifdef DO_CHERI_STATISTICS
    OOB_INFO(cfromptr)->num_uses++;
#endif
    // CFromPtr traps on cbp == NULL so we use reg0 as $ddc to save encoding
    // space (and for backwards compat with old binaries).
    // Note: This is also still required for new binaries since clang assumes it
    // can use zero as $ddc in cfromptr/ctoptr
    const cap_register_t *cbp = get_capreg_0_is_ddc(env, cb);
    /*
     * CFromPtr: Create capability from pointer
     */
    if (rt == (target_ulong)0) {
        cap_register_t result;
        update_capreg(env, cd, null_capability(&result));
    } else if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (is_cap_sealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else {
        cap_register_t result = *cbp;
        uint64_t new_addr = cbp->cr_base + rt;
        if (!is_representable_cap_with_addr(cbp, new_addr)) {
            became_unrepresentable(env, cd, OOB_INFO(cfromptr), _host_return_address);
            cap_mark_unrepresentable(new_addr, &result);
        } else {
            result._cr_cursor = new_addr;
            check_out_of_bounds_stat(env, OOB_INFO(cfromptr), &result);
        }
        update_capreg(env, cd, &result);
    }
}

target_ulong CHERI_HELPER_IMPL(cloadtags(CPUArchState *env, uint32_t cb, uint64_t cbcursor))
{
    GET_HOST_RETPC();
    const cap_register_t *cbp = get_capreg_0_is_ddc(env, cb);

    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (is_cap_sealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_LOAD)) {
        raise_cheri_exception(env, CapEx_PermitLoadViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_LOAD_CAP)) {
        raise_cheri_exception(env, CapEx_PermitLoadCapViolation, cb);
    } else if ((cbcursor & (8 * CHERI_CAP_SIZE - 1)) != 0) {
        do_raise_c0_exception(env, EXCP_AdEL, cbcursor);
    } else {
       return (target_ulong)cheri_tag_get_many(env, cbcursor, cb, NULL, GETPC());
    }
    return 0;
}

target_ulong CHERI_HELPER_IMPL(cgetcause(CPUArchState *env))
{
    uint32_t perms = env->active_tc.PCC.cr_perms;
    /*
     * CGetCause: Move the Capability Exception Cause Register to a
     * General- Purpose Register
     */
    if (!(perms & CAP_ACCESS_SYS_REGS)) {
        do_raise_c2_exception_noreg(env, CapEx_AccessSystemRegsViolation, GETPC());
        return (target_ulong)0;
    } else {
        return (target_ulong)env->CP2_CapCause;
    }
}

void CHERI_HELPER_IMPL(cgetpcc(CPUArchState *env, uint32_t cd))
{
    /*
     * CGetPCC: Move PCC to capability register
     * See Chapter 4 in CHERI Architecture manual.
     */
    update_capreg(env, cd, &env->active_tc.PCC);
    /* Note that the offset(cursor) is updated by ccheck_pcc */
}

static void
derive_from_pcc_impl(CPUArchState *env, uint32_t cd, target_ulong new_addr,
#ifdef DO_CHERI_STATISTICS
                     uintptr_t retpc, struct oob_stats_info* oob_info) {
    oob_info->num_uses++;
#else
                     uintptr_t retpc, void* dummy_arg) {
    (void)dummy_arg;
#endif
#ifdef DO_CHERI_STATISTICS
    oob_info->num_uses++;
#endif
    cap_register_t *pccp = &env->active_tc.PCC;
    /*
     * CGetPCCSetOffset: Get PCC with new offset
     * See Chapter 5 in CHERI Architecture manual.
     */
    cap_register_t result = *pccp;
    if (!is_representable_cap_with_addr(pccp, new_addr)) {
        if (pccp->cr_tag)
            became_unrepresentable(env, cd, oob_info, retpc);
        cap_mark_unrepresentable(new_addr, &result);
    } else {
        result._cr_cursor = new_addr;
        check_out_of_bounds_stat(env, oob_info, &result);
        /* Note that the offset(cursor) is updated by ccheck_pcc */
    }
    update_capreg(env, cd, &result);
}

void CHERI_HELPER_IMPL(cgetpccsetoffset(CPUArchState *env, uint32_t cd, target_ulong rs))
{
    uint64_t new_addr = rs + cap_get_base(&env->active_tc.PCC);
    derive_from_pcc_impl(env, cd, new_addr, GETPC(), OOB_INFO(cgetpccsetoffset));
}

void CHERI_HELPER_IMPL(cgetpccincoffset(CPUArchState *env, uint32_t cd, target_ulong rs))
{
    uint64_t new_addr = rs + cap_get_cursor(&env->active_tc.PCC);
    derive_from_pcc_impl(env, cd, new_addr, GETPC(), OOB_INFO(cgetpccincoffset));
}

void CHERI_HELPER_IMPL(cgetpccsetaddr(CPUArchState *env, uint32_t cd, target_ulong rs))
{
    uint64_t new_addr = rs;
    derive_from_pcc_impl(env, cd, new_addr, GETPC(), OOB_INFO(cgetpccsetaddr));
}

void CHERI_HELPER_IMPL(cincbase(CPUArchState *env, uint32_t cd, uint32_t cb, target_ulong rt))
{
    do_raise_exception(env, EXCP_RI, GETPC());
}

/* Note: not using CHERI_HELPER_IMPL since it cannot trap */
void helper_cmovz(CPUArchState *env, uint32_t cd, uint32_t cs, target_ulong rs)
{
    const cap_register_t *csp = get_readonly_capreg(env, cs);
    /*
     * CMOVZ: conditionally move capability on zero
     */
    if (rs == 0) {
        update_capreg(env, cd, csp);
    }
}

/* Note: not using CHERI_HELPER_IMPL since it cannot trap */
void helper_cmovn(CPUArchState *env, uint32_t cd, uint32_t cs, target_ulong rs)
{
    helper_cmovz(env, cd, cs, rs == 0);
}

target_ulong CHERI_HELPER_IMPL(cjalr(CPUArchState *env, uint32_t cd, uint32_t cb))
{
    GET_HOST_RETPC();
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    /*
     * CJALR: Jump and Link Capability Register
     */
    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (cap_is_sealed_with_type(cbp)) {
        // Note: "sentry" caps can be called using cjalr
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_EXECUTE)) {
        raise_cheri_exception(env, CapEx_PermitExecuteViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_GLOBAL)) {
        raise_cheri_exception(env, CapEx_GlobalViolation, cb);
    } else if (!cap_is_in_bounds(cbp, cap_get_cursor(cbp), 4)) {
        raise_cheri_exception(env, CapEx_LengthViolation, cb);
    } else if (align_of(4, cap_get_cursor(cbp))) {
        do_raise_c0_exception(env, EXCP_AdEL, cap_get_cursor(cbp));
    } else {
        cheri_debug_assert(cap_is_unsealed(cbp) || cap_is_sealed_entry(cbp));
        cap_register_t result = env->active_tc.PCC;
        // can never create an unrepresentable capability since PCC must be in bounds
        result._cr_cursor += 8;
        // The capability register is loaded into PCC during delay slot
        env->active_tc.CapBranchTarget = *cbp;
        if (cap_is_sealed_entry(cbp)) {
            // If we are calling a "sentry" cap, remove the sealed flag
            cap_unseal_entry(&env->active_tc.CapBranchTarget);
            // When calling a sentry capability the return capability is
            // turned into a sentry, too.
            cap_make_sealed_entry(&result);
        }
        update_capreg(env, cd, &result);
         // Return the branch target address
        return cap_get_cursor(cbp);
    }
    return (target_ulong)0;
}

target_ulong CHERI_HELPER_IMPL(cjr(CPUArchState *env, uint32_t cb))
{
    GET_HOST_RETPC();
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    /*
     * CJR: Jump Capability Register
     */
    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (cap_is_sealed_with_type(cbp)) {
        // Note: "sentry" caps can be called using cjalr
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_EXECUTE)) {
        raise_cheri_exception(env, CapEx_PermitExecuteViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_GLOBAL)) {
        raise_cheri_exception(env, CapEx_GlobalViolation, cb);
    } else if (!cap_is_in_bounds(cbp, cap_get_cursor(cbp), 4)) {
        raise_cheri_exception(env, CapEx_LengthViolation, cb);
    } else if (align_of(4, cap_get_cursor(cbp))) {
        do_raise_c0_exception(env, EXCP_AdEL, cap_get_cursor(cbp));
    } else {
        cheri_debug_assert(cap_is_unsealed(cbp) || cap_is_sealed_entry(cbp));
        // The capability register is loaded into PCC during delay slot
        env->active_tc.CapBranchTarget = *cbp;
        // If we are calling a "sentry" cap, remove the sealed flag
        if (cap_is_sealed_entry(cbp))
            cap_unseal_entry(&env->active_tc.CapBranchTarget);
        // Return the branch target address
        return cap_get_cursor(cbp);
    }

    return (target_ulong)0;
}

void CHERI_HELPER_IMPL(csealentry(CPUArchState *env, uint32_t cd, uint32_t cs))
{
    GET_HOST_RETPC();
    /*
     * CSealEntry: Seal a code capability so it is only callable with cjr/cjalr
     * (all other permissions are ignored so it can't be used for loads, etc)
     */
    const cap_register_t *csp = get_readonly_capreg(env, cs);
    if (!csp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cs);
    } else if (!cap_is_unsealed(csp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cs);
    } else if (!(csp->cr_perms & CAP_PERM_EXECUTE)) {
        // Capability must be executable otherwise csealentry doesn't make sense
        raise_cheri_exception(env, CapEx_PermitExecuteViolation, cs);
    } else {
        cap_register_t result = *csp;
        // capability can now only be used in cjr/cjalr
        cap_make_sealed_entry(&result);
        update_capreg(env, cd, &result);
    }
}

static inline cap_register_t *
check_writable_cap_hwr_access(CPUArchState *env, enum CP2HWR hwr, target_ulong _host_return_address) {
    cheri_debug_assert((int)hwr >= (int)CP2HWR_BASE_INDEX);
    cheri_debug_assert((int)hwr < (int)(CP2HWR_BASE_INDEX + 32));
    bool access_sysregs = (env->active_tc.PCC.cr_perms & CAP_ACCESS_SYS_REGS) != 0;
    switch (hwr) {
    case CP2HWR_DDC: /* always accessible */
        assert(false && "$ddc should not be handled here");
        break;
    case CP2HWR_USER_TLS:  /* always accessible */
        return &env->active_tc.CHWR.UserTlsCap;
    case CP2HWR_PRIV_TLS:
        if (!access_sysregs) {
            raise_cheri_exception(env, CapEx_AccessSystemRegsViolation, hwr);
        }
        return &env->active_tc.CHWR.PrivTlsCap;
    case CP2HWR_K1RC:
        if (!in_kernel_mode(env) || !access_sysregs) {
            raise_cheri_exception(env, CapEx_AccessSystemRegsViolation, hwr);
        }
        return &env->active_tc.CHWR.KR1C;
    case CP2HWR_K2RC:
        if (!in_kernel_mode(env) || !access_sysregs) {
            raise_cheri_exception(env, CapEx_AccessSystemRegsViolation, hwr);
        }
        return &env->active_tc.CHWR.KR2C;
    case CP2HWR_ErrorEPCC:
        if (!in_kernel_mode(env) || !access_sysregs) {
            raise_cheri_exception(env, CapEx_AccessSystemRegsViolation, hwr);
        }

        return &env->active_tc.CHWR.ErrorEPCC;
    case CP2HWR_KCC:
        if (!in_kernel_mode(env) || !access_sysregs) {
            raise_cheri_exception(env, CapEx_AccessSystemRegsViolation, hwr);
        }
        return &env->active_tc.CHWR.KCC;
    case CP2HWR_KDC:
        if (!in_kernel_mode(env) || !access_sysregs) {
            raise_cheri_exception(env, CapEx_AccessSystemRegsViolation, hwr);
        }
        return &env->active_tc.CHWR.KDC;
    case CP2HWR_EPCC:
        if (!in_kernel_mode(env) || !access_sysregs) {
            raise_cheri_exception(env, CapEx_AccessSystemRegsViolation, hwr);
        }
        return &env->active_tc.CHWR.EPCC;
    }
    /* unknown cap hardware register */
    do_raise_exception(env, EXCP_RI, _host_return_address);
    return NULL;  // silence warning
}

static inline const cap_register_t *
check_readonly_cap_hwr_access(CPUArchState *env, enum CP2HWR hwr, target_ulong pc) {
    // Currently there is no difference for access permissions between read
    // and write access but that may change in the future
    if (hwr == CP2HWR_DDC)
        return &env->active_tc.CHWR.DDC;
    return check_writable_cap_hwr_access(env, hwr, pc);
}

target_ulong CHERI_HELPER_IMPL(mfc0_epc(CPUArchState *env))
{
    return get_CP0_EPC(env);
}

target_ulong CHERI_HELPER_IMPL(mfc0_error_epc(CPUArchState *env))
{
    return get_CP0_ErrorEPC(env);
}

void CHERI_HELPER_IMPL(mtc0_epc(CPUArchState *env, target_ulong arg))
{
    GET_HOST_RETPC();
    // Check that we can write to EPCC (should always be true since we would have got a trap when not in kernel mode)
    if (!in_kernel_mode(env)) {
        do_raise_exception(env, EXCP_RI, GETPC());
    } else if ((env->active_tc.PCC.cr_perms & CAP_ACCESS_SYS_REGS) == 0) {
        raise_cheri_exception(env, CapEx_AccessSystemRegsViolation, CP2HWR_EPCC);
    }
    set_CP0_EPC(env, arg + cap_get_base(&env->active_tc.CHWR.EPCC));
}

void CHERI_HELPER_IMPL(mtc0_error_epc(CPUArchState *env, target_ulong arg))
{
    GET_HOST_RETPC();
    // Check that we can write to ErrorEPCC (should always be true since we would have got a trap when not in kernel mode)
    if (!in_kernel_mode(env)) {
        do_raise_exception(env, EXCP_RI, GETPC());
    } else if ((env->active_tc.PCC.cr_perms & CAP_ACCESS_SYS_REGS) == 0) {
        raise_cheri_exception(env, CapEx_AccessSystemRegsViolation, CP2HWR_ErrorEPCC);
    }
    set_CP0_ErrorEPC(env, arg + cap_get_base(&env->active_tc.CHWR.ErrorEPCC));
}

void CHERI_HELPER_IMPL(creadhwr(CPUArchState *env, uint32_t cd, uint32_t hwr))
{
    cap_register_t result = *check_readonly_cap_hwr_access(env, CP2HWR_BASE_INDEX + hwr, GETPC());
    update_capreg(env, cd, &result);
}

void CHERI_HELPER_IMPL(cwritehwr(CPUArchState *env, uint32_t cs, uint32_t hwr))
{
    const cap_register_t *csp = get_readonly_capreg(env, cs);
    if (hwr == CP2HWR_DDC) {
        // $ddc is always writable
        update_ddc(env, csp);
        return;
    }
    cap_register_t *cdp = check_writable_cap_hwr_access(env, CP2HWR_BASE_INDEX + hwr, GETPC());
    *cdp = *csp;
}

static target_ulong crap_impl(target_ulong len) {
#ifdef CHERI_128
    // In QEMU we do this by performing a csetbounds on a maximum permissions
    // capability and returning the resulting length
    cap_register_t tmpcap;
    set_max_perms_capability(&tmpcap, 0);
    cc128_setbounds(&tmpcap, 0, len);
    // FIXME: should we return 0 for 1 << 64? instead?
    return cap_get_length64(&tmpcap);
#else
  // For MAGIC128 and 256 everything is representable -> we can return len
  return len;
#endif
}

target_ulong CHERI_HELPER_IMPL(crap(CPUArchState *env, target_ulong len))
{
    // CRoundArchitecturalPrecision rt, rs:
    // rt is set to the smallest value greater or equal to rs that can be used
    // by CSetBoundsExact without trapping (assuming a suitably aligned base).
    return crap_impl(len);
}

target_ulong CHERI_HELPER_IMPL(cram(CPUArchState *env, target_ulong len))
{
    // CRepresentableAlignmentMask rt, rs:
    // rt is set to a mask that can be used to align down addresses to a value
    // that is sufficiently aligned to set precise bounds for the nearest
    // representable length of rs (as obtained by CRoundArchitecturalPrecision).
#ifdef CHERI_128
    // The mask used to align down is all ones followed by (required exponent
    // for compressed representation) zeroes
    target_ulong result = cc128_get_alignment_mask(len);
    target_ulong rounded_with_crap = crap_impl(len);
    target_ulong rounded_with_cram = (len + ~result) & result;
    qemu_log_mask(CPU_LOG_INSTR, "cram(" TARGET_FMT_lx ") rounded=" TARGET_FMT_lx " rounded with mask=" TARGET_FMT_lx
                  " mask result=" TARGET_FMT_lx "\n", len, rounded_with_crap, rounded_with_cram, result);
    if (rounded_with_cram != rounded_with_crap) {
       warn_report("CRAM and CRRL disagree for " TARGET_FMT_lx ": crrl=" TARGET_FMT_lx
                   " cram=" TARGET_FMT_lx, len, rounded_with_crap, rounded_with_cram);
       qemu_log_mask(CPU_LOG_INSTR, "WARNING: CRAM and CRRL disagree for " TARGET_FMT_lx ": crrl=" TARGET_FMT_lx
                   " cram=" TARGET_FMT_lx, len, rounded_with_crap, rounded_with_cram);
    }
    return result;
#else
    // For MAGIC128 and 256 everything is representable -> we can return all ones
    return UINT64_MAX;
#endif
}

//static inline bool cap_bounds_are_subset(const cap_register_t *first, const cap_register_t *second) {
//    return cap_get_base(first) <= cap_get_base(second) && cap_get_top(second) <= cap_get_top(first);
//}

target_ulong CHERI_HELPER_IMPL(csub(CPUArchState *env, uint32_t cb, uint32_t ct))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);

#if 0
    // This is very noisy, but may be interesting for C-compatibility analysis

    // If the capabilities are not subsets (i.e. at least one tagged and derived from different caps,
    // emit a warning to see how many subtractions are being performed that are invalid in ISO C
    if (cbp->cr_tag != ctp->cr_tag ||
        (cbp->cr_tag && !cap_bounds_are_subset(cbp, ctp) && !cap_bounds_are_subset(ctp, cbp))) {
        // Don't warn about subtracting NULL:
        if (!is_null_capability(ctp)) {
            warn_report("Subtraction between two capabilities that are not subsets: \r\n"
                    "\tLHS: " PRINT_CAP_FMTSTR "\r\n\tRHS: " PRINT_CAP_FMTSTR "\r",
                    PRINT_CAP_ARGS(cbp), PRINT_CAP_ARGS(ctp));
        }
    }
#endif
    /*
     * CSub: Subtract Capabilities
     */
    return (target_ulong)(cap_get_cursor(cbp) - cap_get_cursor(ctp));
}

void CHERI_HELPER_IMPL(csetcause(CPUArchState *env, target_ulong rt))
{
    uint32_t perms = env->active_tc.PCC.cr_perms;
    /*
     * CSetCause: Set the Capability Exception Cause Register
     */
    if (!(perms & CAP_ACCESS_SYS_REGS)) {
        do_raise_c2_exception_noreg(env, CapEx_AccessSystemRegsViolation, GETPC());
    } else {
        env->CP2_CapCause = (uint16_t)(rt & 0xffffUL);
    }
}

void CHERI_HELPER_IMPL(csetlen(CPUArchState *env, uint32_t cd, uint32_t cb, target_ulong rt))
{
    do_raise_exception(env, EXCP_RI, GETPC());
}

target_ulong CHERI_HELPER_IMPL(ctoptr(CPUArchState *env, uint32_t cb, uint32_t ct))
{
    GET_HOST_RETPC();
    // CToPtr traps on ctp == NULL so we use reg0 as $ddc there. This means we
    // can have a CToPtr relative to $ddc as one instruction instead of two and
    // is required since clang still assumes it can use zero as $ddc in cfromptr/ctoptr
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const cap_register_t *ctp = get_capreg_0_is_ddc(env, ct);
    uint64_t cb_cursor = cap_get_cursor(cbp);
    uint64_t ct_top = cap_get_top(ctp);
    /*
     * CToPtr: Capability to Pointer
     */
    if (!ctp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, ct);
    } else if (!cbp->cr_tag) {
        return (target_ulong)0;
    } else if ((cb_cursor < ctp->cr_base) || (cb_cursor > ct_top)) {
        /* XXX cb can not be wholly represented within ct. */
        return (target_ulong)0;
    } else if (ctp->cr_base > cb_cursor) {
        return (target_ulong)(ctp->cr_base - cb_cursor);
    } else {
        return (target_ulong)(cb_cursor - ctp->cr_base);
    }

    return (target_ulong)0;
}

/*
 * CPtrCmp Instructions. Capability Pointer Compare.
 */
target_ulong CHERI_HELPER_IMPL(ceq(CPUArchState *env, uint32_t cb, uint32_t ct))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);
    /*
     * CEQ: Capability pointers equal (compares only the cursor)
     */
    return (target_ulong)(cap_get_cursor(cbp) == cap_get_cursor(ctp));
}

target_ulong CHERI_HELPER_IMPL(cne(CPUArchState *env, uint32_t cb, uint32_t ct))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);
    /*
     * CNE: Capability pointers not equal (compares only the cursor)
     */
  return (target_ulong)(cap_get_cursor(cbp) != cap_get_cursor(ctp));

}

target_ulong CHERI_HELPER_IMPL(clt(CPUArchState *env, uint32_t cb, uint32_t ct))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);
    /*
     * CLT: Capability pointers less than (signed)
     */
    int64_t cursor1_signed = (int64_t)cap_get_cursor(cbp);
    int64_t cursor2_signed = (int64_t)cap_get_cursor(ctp);
    return (target_ulong)(cursor1_signed < cursor2_signed);
}

target_ulong CHERI_HELPER_IMPL(cle(CPUArchState *env, uint32_t cb, uint32_t ct))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);
    /*
     * CLE: Capability pointers less than equal (signed)
     */
    int64_t cursor1_signed = (int64_t)cap_get_cursor(cbp);
    int64_t cursor2_signed = (int64_t)cap_get_cursor(ctp);
    return (target_ulong)(cursor1_signed <= cursor2_signed);
}

target_ulong CHERI_HELPER_IMPL(cltu(CPUArchState *env, uint32_t cb, uint32_t ct))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);
    /*
     * CLTU: Capability pointers less than (unsigned)
     */
    uint64_t cursor1_unsigned = cap_get_cursor(cbp);
    uint64_t cursor2_unsigned = cap_get_cursor(ctp);
    return (target_ulong)(cursor1_unsigned < cursor2_unsigned);
}

target_ulong CHERI_HELPER_IMPL(cleu(CPUArchState *env, uint32_t cb, uint32_t ct))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);
    /*
     * CLEU: Capability pointers less than equal (unsigned)
     */
    uint64_t cursor1_unsigned = cap_get_cursor(cbp);
    uint64_t cursor2_unsigned = cap_get_cursor(ctp);
    return (target_ulong)(cursor1_unsigned <= cursor2_unsigned);
}

static bool cap_exactly_equal(const cap_register_t *cbp, const cap_register_t *ctp) {
  if (cbp->cr_tag != ctp->cr_tag) {
    return false;
  } else if (cbp->cr_base != ctp->cr_base) {
    return false;
  } else if (cbp->_cr_cursor != ctp->_cr_cursor) {
    return false;
  } else if (cbp->_cr_top != ctp->_cr_top) {
    return false;
  } else if (cbp->cr_otype != ctp->cr_otype) {
    return false;
  } else if (cbp->cr_perms != ctp->cr_perms) {
    return false;
  }
  return true;
}

target_ulong CHERI_HELPER_IMPL(cexeq(CPUArchState *env, uint32_t cb, uint32_t ct))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);
    return (target_ulong)cap_exactly_equal(cbp, ctp);
}

target_ulong CHERI_HELPER_IMPL(cnexeq(CPUArchState *env, uint32_t cb, uint32_t ct))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);
    return (target_ulong) cap_exactly_equal(cbp, ctp) ? false : true;
}

target_ulong CHERI_HELPER_IMPL(cgetandaddr(CPUArchState *env, uint32_t cb, target_ulong rt))
{
    target_ulong addr = get_capreg_cursor(env, cb);
    return addr & rt;
}

target_ulong CHERI_HELPER_IMPL(ctestsubset(CPUArchState *env, uint32_t cb, uint32_t ct))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);
    gboolean is_subset = FALSE;
    /*
     * CTestSubset: Test if capability is a subset of another
     */
    {
        if (cbp->cr_tag == ctp->cr_tag &&
            /* is_cap_sealed(cbp) == is_cap_sealed(ctp) && */
            cap_get_base(cbp) <= cap_get_base(ctp) &&
            cap_get_top(ctp) <= cap_get_top(cbp) &&
            (ctp->cr_perms & cbp->cr_perms) == ctp->cr_perms &&
            (ctp->cr_uperms & cbp->cr_uperms) == ctp->cr_uperms) {
            is_subset = TRUE;
        }
    }
    return (target_ulong) is_subset;
}

/*
 * Load Via Capability Register
 */
target_ulong CHERI_HELPER_IMPL(cload(CPUArchState *env, uint32_t cb, target_ulong rt,
        uint32_t offset, uint32_t size))
{
    GET_HOST_RETPC();
    // CL[BHWD][U] traps on cbp == NULL so we use reg0 as $ddc to save encoding
    // space and increase code density since loading relative to $ddc is common
    // in the hybrid ABI (and also for backwards compat with old binaries).
    const cap_register_t *cbp = get_capreg_0_is_ddc(env, cb);

    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (is_cap_sealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_LOAD)) {
        raise_cheri_exception(env, CapEx_PermitLoadViolation, cb);
    } else {
        uint64_t cursor = cap_get_cursor(cbp);
        uint64_t addr = cursor + rt + (int32_t)offset;

        if (!cap_is_in_bounds(cbp, addr, size)) {
            raise_cheri_exception(env, CapEx_LengthViolation, cb);
        } else if (align_of(size, addr)) {
#if defined(CHERI_UNALIGNED)
            qemu_log_mask(CPU_LOG_INSTR, "Allowing unaligned %d-byte load of "
                "address 0x%" PRIx64 "\n", size, addr);
            return addr;
#else
            // TODO: is this actually needed? tcg_gen_qemu_st_tl() should
            // check for alignment already.
            do_raise_c0_exception(env, EXCP_AdEL, addr);
#endif
        } else {
            return addr;
        }
    }
    return 0;
}

/*
 * Load Linked Via Capability Register
 */
target_ulong CHERI_HELPER_IMPL(cloadlinked(CPUArchState *env, uint32_t cb, uint32_t size))
{
    GET_HOST_RETPC();
    // CLL[BHWD][U] traps on cbp == NULL so we use reg0 as $ddc to save encoding
    // space and increase code density since loading relative to $ddc is common
    // in the hybrid ABI (and also for backwards compat with old binaries).
    const cap_register_t *cbp = get_capreg_0_is_ddc(env, cb);
    uint64_t addr = cap_get_cursor(cbp);

    env->linkedflag = 0;
    env->lladdr = 1;
    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (is_cap_sealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_LOAD)) {
        raise_cheri_exception(env, CapEx_PermitLoadViolation, cb);
    } else if (!cap_is_in_bounds(cbp, addr, size)) {
        raise_cheri_exception(env, CapEx_LengthViolation, cb);
    } else if (align_of(size, addr)) {
        // TODO: should #if (CHERI_UNALIGNED) also disable this check?
        do_raise_c0_exception(env, EXCP_AdEL, addr);
    } else {
        env->CP0_LLAddr = do_translate_address(env, addr, 0, _host_return_address);
        env->linkedflag = 1;
        env->lladdr = addr;
        // TODO: do the load and return
        return addr;
    }
    return 0;
}

/*
 * Store Conditional Via Capability Register
 */
target_ulong CHERI_HELPER_IMPL(cstorecond(CPUArchState *env, uint32_t cb, uint32_t size))
{
    GET_HOST_RETPC();
    // CSC[BHWD] traps on cbp == NULL so we use reg0 as $ddc to save encoding
    // space and increase code density since storing relative to $ddc is common
    // in the hybrid ABI (and also for backwards compat with old binaries).
    const cap_register_t *cbp = get_capreg_0_is_ddc(env, cb);
    uint64_t addr = cap_get_cursor(cbp);

    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (is_cap_sealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_STORE)) {
        raise_cheri_exception(env, CapEx_PermitStoreViolation, cb);
    } else if (!cap_is_in_bounds(cbp, addr, size)) {
        raise_cheri_exception(env, CapEx_LengthViolation, cb);
    } else if (align_of(size, addr)) {
        // TODO: should #if (CHERI_UNALIGNED) also disable this check?
        do_raise_c0_exception(env, EXCP_AdES, addr);
    } else {
        qemu_log_mask(CPU_LOG_INSTR, "cstorecond: linkedflag = %d,"
                      " addr=" TARGET_FMT_plx "lladdr=" TARGET_FMT_plx
                      " CP0_LLaddr=" TARGET_FMT_plx "\n",
                      (int)env->linkedflag, addr, env->lladdr, env->CP0_LLAddr);
        // Can't do this here.  It might miss in the TLB.
        // cheri_tag_invalidate(env, addr, size);
        // Also, rd is set by the actual store conditional operation.
        if (env->lladdr != addr)
            env->linkedflag = 0; // FIXME: remove the linkedflag hack
        return addr;
    }
    return 0;
}

/*
 * Store Via Capability Register
 */
target_ulong CHERI_HELPER_IMPL(cstore(CPUArchState *env, uint32_t cb, target_ulong rt,
        uint32_t offset, uint32_t size))
{
    GET_HOST_RETPC();
    // CS[BHWD][U] traps on cbp == NULL so we use reg0 as $ddc to save encoding
    // space and increase code density since storing relative to $ddc is common
    // in the hybrid ABI (and also for backwards compat with old binaries).
    const cap_register_t *cbp = get_capreg_0_is_ddc(env, cb);

    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (is_cap_sealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_STORE)) {
        raise_cheri_exception(env, CapEx_PermitStoreViolation, cb);
    } else {
        uint64_t cursor = cap_get_cursor(cbp);
        uint64_t addr = cursor + rt + (int32_t)offset;

        if (!cap_is_in_bounds(cbp, addr, size)) {
            raise_cheri_exception(env, CapEx_LengthViolation, cb);
        } else if (align_of(size, addr)) {
#if defined(CHERI_UNALIGNED)
            qemu_log_mask(CPU_LOG_INSTR, "Allowing unaligned %d-byte store to "
                "address 0x%" PRIx64 "\n", size, addr);
            // Can't do this here.  It might miss in the TLB.
            // cheri_tag_invalidate(env, addr, size);
            return addr;
#else
            // TODO: is this actually needed? tcg_gen_qemu_st_tl() should
            // check for alignment already.
            do_raise_c0_exception(env, EXCP_AdES, addr);
#endif
        } else {
            // Can't do this here.  It might miss in the TLB.
            // cheri_tag_invalidate(env, addr, size);
            return addr;
        }
    }
    return 0;
}

static inline target_ulong get_csc_addr(CPUArchState *env, uint32_t cs, uint32_t cb,
        target_ulong rt, uint32_t offset, uintptr_t _host_return_address)
{
    // CSC traps on cbp == NULL so we use reg0 as $ddc to save encoding
    // space and increase code density since storing relative to $ddc is common
    // in the hybrid ABI (and also for backwards compat with old binaries).
    const cap_register_t *cbp = get_capreg_0_is_ddc(env, cb);

    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
        return (target_ulong)0;
    } else if (is_cap_sealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
        return (target_ulong)0;
    } else if (!(cbp->cr_perms & CAP_PERM_STORE)) {
        raise_cheri_exception(env, CapEx_PermitStoreViolation, cb);
        return (target_ulong)0;
    } else if (!(cbp->cr_perms & CAP_PERM_STORE_CAP)) {
        raise_cheri_exception(env, CapEx_PermitStoreCapViolation, cb);
        return (target_ulong)0;
    } else if (!(cbp->cr_perms & CAP_PERM_STORE_LOCAL) &&
               get_capreg_tag(env, cs) &&
               !(get_capreg_hwperms(env, cs) & CAP_PERM_GLOBAL)) {
        raise_cheri_exception(env, CapEx_PermitStoreLocalCapViolation, cb);
        return (target_ulong)0;
    } else {
        uint64_t cursor = cap_get_cursor(cbp);
        uint64_t addr = (uint64_t)((int64_t)(cursor + rt) + (int32_t)offset);

        if (!cap_is_in_bounds(cbp, addr, CHERI_CAP_SIZE)) {
            raise_cheri_exception(env, CapEx_LengthViolation, cb);
            return (target_ulong)0;
        } else if (align_of(CHERI_CAP_SIZE, addr)) {
            do_raise_c0_exception(env, EXCP_AdES, addr);
            return (target_ulong)0;
        }

        return (target_ulong)addr;
    }
}

static inline target_ulong get_cscc_addr(CPUArchState *env, uint32_t cs, uint32_t cb, uintptr_t _host_return_address)
{
    // CSCC traps on cbp == NULL so we use reg0 as $ddc to save encoding
    // space and increase code density since storing relative to $ddc is common
    // in the hybrid ABI (and also for backwards compat with old binaries).
    const cap_register_t *cbp = get_capreg_0_is_ddc(env, cb);
    const cap_register_t *csp = get_readonly_capreg(env, cs);
    uint64_t addr = cap_get_cursor(cbp);

    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
        return (target_ulong)0;
    } else if (is_cap_sealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
        return (target_ulong)0;
    } else if (!(cbp->cr_perms & CAP_PERM_STORE)) {
        raise_cheri_exception(env, CapEx_PermitStoreViolation, cb);
        return (target_ulong)0;
    } else if (!(cbp->cr_perms & CAP_PERM_STORE_CAP)) {
        raise_cheri_exception(env, CapEx_PermitStoreCapViolation, cb);
        return (target_ulong)0;
    } else if (!(cbp->cr_perms & CAP_PERM_STORE_LOCAL) && csp->cr_tag &&
            !(csp->cr_perms & CAP_PERM_GLOBAL)) {
        raise_cheri_exception(env, CapEx_PermitStoreLocalCapViolation, cb);
        return (target_ulong)0;
    } else if (!cap_is_in_bounds(cbp, addr, CHERI_CAP_SIZE)) {
        raise_cheri_exception(env, CapEx_LengthViolation, cb);
        return (target_ulong)0;
    } else if (align_of(CHERI_CAP_SIZE, addr)) {
        do_raise_c0_exception(env, EXCP_AdES, addr);
        return (target_ulong)0;
    }

    return (target_ulong)addr;
}

static void store_cap_to_memory(CPUArchState *env, uint32_t cs, target_ulong vaddr, target_ulong retpc);
static void load_cap_from_memory(CPUArchState *env, uint32_t cd, uint32_t cb,
                                 const cap_register_t *source,
                                 target_ulong offset, target_ulong retpc,
                                 hwaddr *physaddr);

target_ulong CHERI_HELPER_IMPL(cscc_without_tcg(CPUArchState *env, uint32_t cs, uint32_t cb))
{
    target_ulong retpc = GETPC();
    target_ulong vaddr = get_cscc_addr(env, cs, cb, retpc);
    qemu_log_mask(CPU_LOG_INSTR,
                  "cscc: linkedflag = %d, addr=" TARGET_FMT_plx
                  " lladdr=" TARGET_FMT_plx " CP0_LLaddr=" TARGET_FMT_plx "\n",
                  (int)env->linkedflag, vaddr, env->lladdr, env->CP0_LLAddr);
    /* If linkedflag is zero then don't store capability. */
    if (!env->linkedflag || env->lladdr != vaddr)
        return 0;
    store_cap_to_memory(env, cs, vaddr, retpc);
    return 1;
}

void CHERI_HELPER_IMPL(csc_without_tcg(CPUArchState *env, uint32_t cs, uint32_t cb,
        target_ulong rt, uint32_t offset))
{
    target_ulong retpc = GETPC();
    target_ulong vaddr = get_csc_addr(env, cs, cb, rt, offset, retpc);
    // helper_csc_addr should check for alignment
    cheri_debug_assert(align_of(CHERI_CAP_SIZE, vaddr) == 0);
    store_cap_to_memory(env, cs, vaddr, retpc);
}

void CHERI_HELPER_IMPL(clc_without_tcg(CPUArchState *env, uint32_t cd, uint32_t cb,
        target_ulong rt, uint32_t offset))
{
    target_ulong _host_return_address = GETPC();
    // CLC traps on cbp == NULL so we use reg0 as $ddc to save encoding
    // space and increase code density since loading relative to $ddc is common
    // in the hybrid ABI (and also for backwards compat with old binaries).
    const cap_register_t *cbp = get_capreg_0_is_ddc(env, cb);

    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (is_cap_sealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_LOAD)) {
        raise_cheri_exception(env, CapEx_PermitLoadViolation, cb);
    }
    uint64_t addr = (uint64_t)((cap_get_cursor(cbp) + rt) + (int32_t)offset);
    /* uint32_t tag = cheri_tag_get(env, addr, cd, NULL); */
    if (!cap_is_in_bounds(cbp, addr, CHERI_CAP_SIZE)) {
        raise_cheri_exception(env, CapEx_LengthViolation, cb);
    } else if (align_of(CHERI_CAP_SIZE, addr)) {
        do_raise_c0_exception(env, EXCP_AdEL, addr);
    }
    load_cap_from_memory(env, cd, cb, cbp, addr, _host_return_address, /*physaddr_out=*/NULL);
}

void CHERI_HELPER_IMPL(cllc_without_tcg(CPUArchState *env, uint32_t cd, uint32_t cb))
{
    GET_HOST_RETPC();

    // CLLC traps on cbp == NULL so we use reg0 as $ddc to save encoding
    // space and increase code density since loading relative to $ddc is common
    // in the hybrid ABI (and also for backwards compat with old binaries).
    const cap_register_t *cbp = get_capreg_0_is_ddc(env, cb);
    uint64_t addr = cap_get_cursor(cbp);

    /* Clear linked state */
    env->linkedflag = 0;
    env->lladdr = 1;
    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (is_cap_sealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (!(cbp->cr_perms & CAP_PERM_LOAD)) {
        raise_cheri_exception(env, CapEx_PermitLoadViolation, cb);
    } else if (!cap_is_in_bounds(cbp, addr, CHERI_CAP_SIZE)) {
        raise_cheri_exception(env, CapEx_LengthViolation, cb);
    } else if (align_of(CHERI_CAP_SIZE, addr)) {
        do_raise_c0_exception(env, EXCP_AdEL, addr);
    }
    cheri_debug_assert(align_of(CHERI_CAP_SIZE, addr) == 0);
    hwaddr physaddr;
    load_cap_from_memory(env, cd, cb, cbp, /*addr=*/cap_get_cursor(cbp),
                         _host_return_address, &physaddr);
    env->lladdr = addr;
    env->CP0_LLAddr = physaddr;
    env->linkedflag = 1; // FIXME: remove
}

#ifdef CONFIG_MIPS_LOG_INSTR
/*
 * Dump cap tag, otype, permissions and seal bit to cvtrace entry
 */
static inline void
cvtrace_dump_cap_perms(cvtrace_t *cvtrace, const cap_register_t *cr)
{
    if (unlikely(qemu_loglevel_mask(CPU_LOG_CVTRACE))) {
        cvtrace->val2 = tswap64(((uint64_t)cr->cr_tag << 63) |
            ((uint64_t)(cr->cr_otype & CAP_MAX_REPRESENTABLE_OTYPE)<< 32) |
            ((((cr->cr_uperms & CAP_UPERMS_ALL) << CAP_UPERMS_SHFT) |
              (cr->cr_perms & CAP_PERMS_ALL)) << 1) |
            (uint64_t)(is_cap_sealed(cr) ? 1 : 0));
    }
}

/*
 * Dump capability cursor, base and length to cvtrace entry
 */
static inline void cvtrace_dump_cap_cbl(cvtrace_t *cvtrace, const cap_register_t *cr)
{
    if (unlikely(qemu_loglevel_mask(CPU_LOG_CVTRACE))) {
        cvtrace->val3 = tswap64(cr->_cr_cursor);
        cvtrace->val4 = tswap64(cr->cr_base);
        cvtrace->val5 = tswap64(cap_get_length64(cr)); // write UINT64_MAX for 1 << 64
    }
}

void dump_changed_capreg(CPUArchState *env, const cap_register_t *cr,
        cap_register_t *old_reg, const char* name)
{
    if (memcmp(cr, old_reg, sizeof(cap_register_t))) {
        if (qemu_loglevel_mask(CPU_LOG_CVTRACE)) {
            if (env->cvtrace.version == CVT_NO_REG ||
                env->cvtrace.version == CVT_GPR)
                env->cvtrace.version = CVT_CAP;
            if (env->cvtrace.version == CVT_ST_GPR)
                env->cvtrace.version = CVT_ST_CAP;
            cvtrace_dump_cap_perms(&env->cvtrace, cr);
            cvtrace_dump_cap_cbl(&env->cvtrace, cr);
        }
        if (qemu_loglevel_mask(CPU_LOG_INSTR)) {
            qemu_log_capreg(cr, "    Write ", name);
        }
        *old_reg = *cr;
    }
}

void dump_changed_cop2(CPUArchState *env, TCState *cur) {
    static const char * const capreg_name[] = {
        "C00", "C01", "C02", "C03", "C04", "C05", "C06", "C07",
        "C08", "C09", "C10", "C11", "C12", "C13", "C14", "C15",
        "C16", "C17", "C18", "C19", "C20", "C21", "C22", "C23",
        "C24", "C25", "C26", "C27", "C28", "C29", "C30", "C31",
    };

    dump_changed_capreg(env, &cur->CapBranchTarget, &env->last_CapBranchTarget, "CapBranchTarget");
    for (int i=0; i<32; i++) {
        dump_changed_capreg(env, get_readonly_capreg(env, i), &env->last_C[i], capreg_name[i]);
    }
    dump_changed_capreg(env, &cur->CHWR.DDC, &env->last_CHWR.DDC, "DDC");
    dump_changed_capreg(env, &cur->CHWR.UserTlsCap, &env->last_CHWR.UserTlsCap, "UserTlsCap");
    dump_changed_capreg(env, &cur->CHWR.PrivTlsCap, &env->last_CHWR.PrivTlsCap, "PrivTlsCap");
    dump_changed_capreg(env, &cur->CHWR.KR1C, &env->last_CHWR.KR1C, "ChwrKR1C");
    dump_changed_capreg(env, &cur->CHWR.KR2C, &env->last_CHWR.KR2C, "ChwrKR1C");
    dump_changed_capreg(env, &cur->CHWR.ErrorEPCC, &env->last_CHWR.ErrorEPCC, "ErrorEPCC");
    dump_changed_capreg(env, &cur->CHWR.KCC, &env->last_CHWR.KCC, "KCC");
    dump_changed_capreg(env, &cur->CHWR.KDC, &env->last_CHWR.KDC, "KDC");
    /*
     * The binary trace format only allows a single register to be changed by
     * an instruction so if there is an exception where another register
     * was also changed, do not overwrite that value with EPCC, otherwise
     * the original result of the instruction is lost.
     */
    if (!qemu_loglevel_mask(CPU_LOG_CVTRACE) || env->cvtrace.exception == 31) {
        dump_changed_capreg(env, &cur->CHWR.EPCC, &env->last_CHWR.EPCC, "EPCC");
    }
}

/*
 * Dump cap load or store to cvtrace
 */
static inline void cvtrace_dump_cap_ldst(cvtrace_t *cvtrace, uint8_t version,
        uint64_t addr, const cap_register_t *cr)
{
    if (unlikely(qemu_loglevel_mask(CPU_LOG_CVTRACE))) {
        cvtrace->version = version;
        cvtrace->val1 = tswap64(addr);
        cvtrace->val2 = tswap64(((uint64_t)cr->cr_tag << 63) |
            ((uint64_t)(cr->cr_otype & CAP_MAX_REPRESENTABLE_OTYPE) << 32) |
            ((((cr->cr_uperms & CAP_UPERMS_ALL) << CAP_UPERMS_SHFT) |
              (cr->cr_perms & CAP_PERMS_ALL)) << 1) |
            (uint64_t)(is_cap_sealed(cr) ? 1 : 0));
    }
}
#define cvtrace_dump_cap_load(trace, addr, cr)          \
    cvtrace_dump_cap_ldst(trace, CVT_LD_CAP, addr, cr)
#define cvtrace_dump_cap_store(trace, addr, cr)         \
    cvtrace_dump_cap_ldst(trace, CVT_ST_CAP, addr, cr)

#endif // CONFIG_MIPS_LOG_INSTR

#ifdef CHERI_128

#ifdef CONFIG_MIPS_LOG_INSTR
/*
 * Print capability load from memory to log file.
 */
static inline void dump_cap_load(uint64_t addr, uint64_t pesbt,
        uint64_t cursor, uint8_t tag)
{

    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        qemu_log("    Cap Memory Read [" TARGET_FMT_lx
                "] = v:%d PESBT:" TARGET_FMT_lx " Cursor:" TARGET_FMT_lx "\n",
                addr, tag, pesbt, cursor);
    }
}

/*
 * Print capability store to memory to log file.
 */
static inline void dump_cap_store(uint64_t addr, uint64_t pesbt,
        uint64_t cursor, uint8_t tag)
{

    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        qemu_log("    Cap Memory Write [" TARGET_FMT_lx
                "] = v:%d PESBT:" TARGET_FMT_lx " Cursor:" TARGET_FMT_lx "\n",
                addr, tag, pesbt, cursor);
    }
}
#endif // CONFIG_MIPS_LOG_INSTR

static void load_cap_from_memory(CPUArchState *env, uint32_t cd, uint32_t cb,
                                 const cap_register_t *source,
                                 target_ulong vaddr, target_ulong retpc,
                                 hwaddr *physaddr)
{
    cheri_debug_assert(align_of(CHERI_CAP_SIZE, vaddr) == 0);
    /*
     * Load otype and perms from memory (might trap on load)
     *
     * Note: In-memory capabilities pesbt is xored with a mask to ensure that
     * NULL capabilities have an all zeroes representation.
     */
    /* No TLB fault possible, should be safe to get a host pointer now */
    void* host = probe_read(env, vaddr, CHERI_CAP_SIZE, cpu_mmu_index(env, false), retpc);
    // When writing back pesbt we have to XOR with the NULL mask to ensure that
    // NULL capabilities have an all-zeroes representation.
    uint64_t pesbt;
    uint64_t cursor;
    if (likely(host)) {
        // Fast path, host address in TLB
        pesbt = ldq_p(host) ^ CC128_NULL_XOR_MASK;
        cursor = ldq_p((char*)host + 8);
#ifdef CONFIG_MIPS_LOG_INSTR
        // cpu_ldq_data_ra() performs the read logging, with raw memory
        // accesses we have to do it manually
        if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
            helper_dump_load(env, vaddr, pesbt ^ CC128_NULL_XOR_MASK, MO_64);
            helper_dump_load(env, vaddr + 8, cursor, MO_64);
        }
#endif
    } else {
        // Slow path for e.g. IO regions.
        qemu_log_mask(CPU_LOG_INSTR, "Using slow path for load from guest address " TARGET_FMT_plx "\n", vaddr);
        pesbt = cpu_ldq_data_ra(env, vaddr + 0, retpc) ^ CC128_NULL_XOR_MASK;
        cursor = cpu_ldq_data_ra(env, vaddr + 8, retpc);
    }
    int prot;
    target_ulong tag = cheri_tag_get(env, vaddr, cb, physaddr, &prot, retpc);
    // TODO: qemu_ram_addr_from_host()
    tag = clear_tag_if_no_loadcap(tag, source, prot);

    env->statcounters_cap_read++;
    if (tag)
        env->statcounters_cap_read_tagged++;
#ifdef CONFIG_MIPS_LOG_INSTR
    /* Log memory read, if needed. */
    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        // Decompress to log all fields
        cap_register_t ncd;
        decompress_128cap_already_xored(pesbt, cursor, &ncd);
        ncd.cr_tag = tag;
        dump_cap_load(vaddr, compress_128cap(&ncd), cursor, tag);
        cvtrace_dump_cap_load(&env->cvtrace, vaddr, &ncd);
        cvtrace_dump_cap_cbl(&env->cvtrace, &ncd);
    }
#endif
    update_compressed_capreg(env, cd, pesbt, tag, cursor);
}

static void store_cap_to_memory(CPUArchState *env, uint32_t cs,
    target_ulong vaddr, target_ulong retpc)
{
    uint64_t cursor = get_capreg_cursor(env, cs);
    uint64_t pesbt = get_capreg_pesbt(env, cs);
    bool tag = get_capreg_tag(env, cs);
    /*
     * Touching the tags will take both the data write TLB fault and
     * capability write TLB fault before updating anything.  Thereafter, the
     * data stores will not take additional faults, so there is no risk of
     * accidentally tagging a shorn data write.  This, like the rest of the
     * tag logic, is not multi-TCG-thread safe.
     */

    env->statcounters_cap_write++;
    // TODO: qemu_ram_addr_from_host()
    if (tag) {
        env->statcounters_cap_write_tagged++;
        cheri_tag_set(env, vaddr, cs, retpc);
    } else {
        cheri_tag_invalidate(env, vaddr, CHERI_CAP_SIZE, retpc);
    }
    /* No TLB fault possible, should be safe to get a host pointer now */
    void* host = probe_write(env, vaddr, CHERI_CAP_SIZE, cpu_mmu_index(env, false), retpc);
    // When writing back pesbt we have to XOR with the NULL mask to ensure that
    // NULL capabilities have an all-zeroes representation.
    if (likely(host)) {
        // Fast path, host address in TLB
        stq_p(host, pesbt ^ CC128_NULL_XOR_MASK);
        stq_p((char*)host + 8, cursor);
#ifdef CONFIG_MIPS_LOG_INSTR
        // cpu_stq_data_ra() performs the write logging, with raw memory
        // accesses we have to do it manually
        if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
            helper_dump_store(env, vaddr, pesbt ^ CC128_NULL_XOR_MASK, MO_64);
            helper_dump_store(env, vaddr + 8, cursor, MO_64);
        }
#endif
    } else {
        // Slow path for e.g. IO regions.
        qemu_log_mask(CPU_LOG_INSTR, "Using slow path for store to guest address " TARGET_FMT_plx "\n", vaddr);
        cpu_stq_data_ra(env, vaddr, pesbt ^ CC128_NULL_XOR_MASK, retpc);
        cpu_stq_data_ra(env, vaddr + 8, cursor, retpc);
    }

#ifdef CONFIG_MIPS_LOG_INSTR
    /* Log memory cap write, if needed. */
    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        /* Log memory cap write, if needed. */
        // Decompress to log all fields
        cap_register_t stored_cap;
        decompress_128cap_already_xored(pesbt, cursor, &stored_cap);
        stored_cap.cr_tag = tag;
        cheri_debug_assert(cursor == cap_get_cursor(&stored_cap));
        dump_cap_store(vaddr, pesbt, cursor, tag);
        cvtrace_dump_cap_store(&env->cvtrace, vaddr, &stored_cap);
        cvtrace_dump_cap_cbl(&env->cvtrace, &stored_cap);
    }
#endif
}

#elif defined(CHERI_MAGIC128)
#ifdef CONFIG_MIPS_LOG_INSTR
/*
 * Print capability load from memory to log file.
 */
static inline void dump_cap_load(uint64_t addr, uint64_t cursor,
        uint64_t base, uint8_t tag)
{

    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
       qemu_log("    Cap Memory Read [" TARGET_FMT_lx "] = v:%d c:"
               TARGET_FMT_lx " b:" TARGET_FMT_lx "\n", addr, tag, cursor, base);
    }
}

/*
 * Print capability store to memory to log file.
 */
static inline void dump_cap_store(uint64_t addr, uint64_t cursor,
        uint64_t base, uint8_t tag)
{

    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
      qemu_log("    Cap Memory Write [" TARGET_FMT_lx "] = v:%d c:"
              TARGET_FMT_lx " b:" TARGET_FMT_lx "\n", addr, tag, cursor, base);
    }
}
#endif // CONFIG_MIPS_LOG_INSTR

static void load_cap_from_memory(CPUArchState *env, uint32_t cd, uint32_t cb,
                                 const cap_register_t *source,
                                 target_ulong vaddr, target_ulong retpc,
                                 hwaddr *physaddr)
{
    int prot;
    cap_register_t ncd;

    // TODO: do one physical translation and then use that to speed up tag read
    // and use address_space_read to read the full 16 byte buffer
    /* Load base and perms from memory (might trap on load) */
    inmemory_chericap256 mem_buffer;
    mem_buffer.u64s[2] = cpu_ldq_data_ra(env, vaddr + 0, retpc); /* base */
    mem_buffer.u64s[1] = cpu_ldq_data_ra(env, vaddr + 8, retpc); /* cursor */
    /* Load the two magic values */
    target_ulong tag = cheri_tag_get_m128(
        env, vaddr, cd, &mem_buffer.u64s[0] /* tps */,
        &mem_buffer.u64s[3] /* length */, physaddr, &prot, retpc);

    tag = clear_tag_if_no_loadcap(tag, source, prot);
    env->statcounters_cap_read++;
    if (tag)
        env->statcounters_cap_read_tagged++;

    // XOR with -1 so that NULL is zero in memory, etc.
    decompress_256cap(mem_buffer, &ncd, tag);

#ifdef CONFIG_MIPS_LOG_INSTR
    /* Log memory read, if needed. */
    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        dump_cap_load(vaddr, cap_get_cursor(&ncd), ncd.cr_base, tag);
        cvtrace_dump_cap_load(&env->cvtrace, vaddr, &ncd);
        cvtrace_dump_cap_cbl(&env->cvtrace, &ncd);
    }
#endif

    update_capreg(env, cd, &ncd);
}

static void store_cap_to_memory(CPUArchState *env, uint32_t cs,
                                target_ulong vaddr, target_ulong retpc)
{
    const cap_register_t *csp = get_readonly_capreg(env, cs);
    inmemory_chericap256 mem_buffer;
    compress_256cap(&mem_buffer, csp);
    /*
     * Touching the tags will take both the data write TLB fault and
     * capability write TLB fault before updating anything.  Thereafter, the
     * data stores will not take additional faults, so there is no risk of
     * accidentally tagging a shorn data write.  This, like the rest of the
     * tag logic, is not multi-TCG-thread safe.
     */
    /* Store the "magic" data with the tags */
    cheri_tag_set_m128(env, vaddr, cs, csp->cr_tag,
                       mem_buffer.u64s[0] /* tps */,
                       mem_buffer.u64s[3] /* length */, NULL, retpc);
    env->statcounters_cap_write++;
    if (csp->cr_tag) {
        env->statcounters_cap_write_tagged++;
    }
    cpu_stq_data_ra(env, vaddr, mem_buffer.u64s[2], retpc); /* base */
    cpu_stq_data_ra(env, vaddr + 8, mem_buffer.u64s[1], retpc);

#ifdef CONFIG_MIPS_LOG_INSTR
    /* Log memory cap write, if needed. */
    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        /* Log memory cap write, if needed. */
        cvtrace_dump_cap_store(&env->cvtrace, vaddr, csp);
        cvtrace_dump_cap_cbl(&env->cvtrace, csp);
        dump_cap_store(vaddr, cap_get_cursor(csp), csp->cr_base, csp->cr_tag);
    }
#endif
}

#else /* ! CHERI_MAGIC128 */

#ifdef CONFIG_MIPS_LOG_INSTR
/*
 * Print capability load from memory to log file.
 */
static inline void dump_cap_load_op(uint64_t addr, uint64_t perm_type,
        uint8_t tag)
{

    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        qemu_log("    Cap Memory Read [" TARGET_FMT_lx
             "] = v:%d tps:" TARGET_FMT_lx "\n", addr, tag, perm_type);
    }
}

static inline void dump_cap_load_cbl(uint64_t cursor, uint64_t base,
        uint64_t length)
{

    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        qemu_log("    c:" TARGET_FMT_lx " b:" TARGET_FMT_lx " l:"
                TARGET_FMT_lx "\n", cursor, base, length);
    }
}

/*
 * Print capability store to memory to log file.
 */
static inline void dump_cap_store_op(uint64_t addr, uint64_t perm_type,
        uint8_t tag)
{

    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        qemu_log("    Cap Memory Write [" TARGET_FMT_lx
                "] = v:%d tps:" TARGET_FMT_lx "\n", addr, tag, perm_type);
    }
}

static inline void dump_cap_store_cursor(uint64_t cursor)
{

    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        qemu_log("    c:" TARGET_FMT_lx, cursor);
    }
}

static inline void dump_cap_store_base(uint64_t base)
{

    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        qemu_log(" b:" TARGET_FMT_lx, base);
    }
}

static inline void dump_cap_store_length(uint64_t length)
{

    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        qemu_log(" l:" TARGET_FMT_lx "\n", length);
    }
}

#endif // CONFIG_MIPS_LOG_INSTR

static void load_cap_from_memory(CPUArchState *env, uint32_t cd, uint32_t cb,
                                 const cap_register_t *source,
                                 target_ulong vaddr, target_ulong retpc,
                                 hwaddr *physaddr)
{
    cap_register_t ncd;
    int prot;

    // TODO: do one physical translation and then use that to speed up tag read
    /* Load otype and perms from memory (might trap on load) */
    inmemory_chericap256 mem_buffer;
    mem_buffer.u64s[0] =
        cpu_ldq_data_ra(env, vaddr + 0, retpc); /* perms+otype */
    mem_buffer.u64s[1] = cpu_ldq_data_ra(env, vaddr + 8, retpc);  /* cursor */
    mem_buffer.u64s[2] = cpu_ldq_data_ra(env, vaddr + 16, retpc); /* base */
    mem_buffer.u64s[3] = cpu_ldq_data_ra(env, vaddr + 24, retpc); /* length */

    target_ulong tag = cheri_tag_get(env, vaddr, cd, physaddr, &prot, retpc);
    tag = clear_tag_if_no_loadcap(tag, source, prot);
    env->statcounters_cap_read++;
    if (tag)
        env->statcounters_cap_read_tagged++;

    // XOR with -1 so that NULL is zero in memory, etc.
    decompress_256cap(mem_buffer, &ncd, tag);

#ifdef CONFIG_MIPS_LOG_INSTR
    /* Log memory reads, if needed. */
    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        dump_cap_load_op(vaddr, mem_buffer.u64s[0], tag);
        cvtrace_dump_cap_load(&env->cvtrace, vaddr, &ncd);
        dump_cap_load_cbl(cap_get_cursor(&ncd), cap_get_base(&ncd),
                          cap_get_length64(&ncd));
        cvtrace_dump_cap_cbl(&env->cvtrace, &ncd);
    }
#endif

    update_capreg(env, cd, &ncd);
}

#ifdef CONFIG_MIPS_LOG_INSTR
static inline void cvtrace_dump_cap_cursor(cvtrace_t *cvtrace, uint64_t cursor)
{
    if (unlikely(qemu_loglevel_mask(CPU_LOG_CVTRACE))) {
        cvtrace->val3 = tswap64(cursor);
    }
}

static inline void cvtrace_dump_cap_base(cvtrace_t *cvtrace, uint64_t base)
{
    if (unlikely(qemu_loglevel_mask(CPU_LOG_CVTRACE))) {
        cvtrace->val4 = tswap64(base);
    }
}

static inline void cvtrace_dump_cap_length(cvtrace_t *cvtrace, uint64_t length)
{
    if (unlikely(qemu_loglevel_mask(CPU_LOG_CVTRACE))) {
        cvtrace->val5 = tswap64(length);
    }
}
#endif // CONFIG_MIPS_LOG_INSTR

static void store_cap_to_memory(CPUArchState *env, uint32_t cs,
                                target_ulong vaddr, target_ulong retpc)
{
    const cap_register_t *csp = get_readonly_capreg(env, cs);
    inmemory_chericap256 mem_buffer;
    compress_256cap(&mem_buffer, csp);

    /*
     * Touching the tags will take both the data write TLB fault and
     * capability write TLB fault before updating anything.  Thereafter, the
     * data stores will not take additional faults, so there is no risk of
     * accidentally tagging a shorn data write.  This, like the rest of the
     * tag logic, is not multi-TCG-thread safe.
     */

    env->statcounters_cap_write++;
    if (csp->cr_tag) {
        env->statcounters_cap_write_tagged++;
        cheri_tag_set(env, vaddr, cs, retpc);
    } else {
        cheri_tag_invalidate(env, vaddr, CHERI_CAP_SIZE, retpc);
    }

    cpu_stq_data_ra(env, vaddr + 0, mem_buffer.u64s[0], retpc);
    cpu_stq_data_ra(env, vaddr + 8, mem_buffer.u64s[1], retpc);
    cpu_stq_data_ra(env, vaddr + 16, mem_buffer.u64s[2], retpc);
    cpu_stq_data_ra(env, vaddr + 24, mem_buffer.u64s[3], retpc);

#ifdef CONFIG_MIPS_LOG_INSTR
    /* Log memory cap write, if needed. */
    if (unlikely(qemu_loglevel_mask(CPU_LOG_INSTR))) {
        uint64_t otype_and_perms = mem_buffer.u64s[0];
        dump_cap_store_op(vaddr, otype_and_perms, csp->cr_tag);
        cvtrace_dump_cap_store(&env->cvtrace, vaddr, csp);
        dump_cap_store_cursor(cap_get_cursor(csp));
        cvtrace_dump_cap_cursor(&env->cvtrace, cap_get_cursor(csp));
        dump_cap_store_base(csp->cr_base);
        cvtrace_dump_cap_base(&env->cvtrace, cap_get_base(csp));
        dump_cap_store_length(cap_get_length64(csp));
        cvtrace_dump_cap_length(&env->cvtrace, cap_get_length64(csp));
    }
#endif
}

#endif /* ! CHERI_MAGIC128 */

void CHERI_HELPER_IMPL(ccheck_btarget(CPUArchState *env))
{
    // Check whether the branch target is within $pcc and if not raise an exception
    // qemu_log_mask(CPU_LOG_INSTR, "%s: env->pc=0x" TARGET_FMT_lx " hflags=0x%x, btarget=0x" TARGET_FMT_lx "\n",
    //    __func__, env->active_tc.PC, env->hflags, env->btarget);
    check_cap(env, &env->active_tc.PCC, CAP_PERM_EXECUTE, env->btarget, 0xff, 4, /*instavail=*/false, GETPC());
}

void CHERI_HELPER_IMPL(copy_cap_btarget_to_pcc(CPUArchState *env))
{
    env->active_tc.PCC = env->active_tc.CapBranchTarget;
    // Restore or clear MIPS_HFLAG_CP0 depending on Access_System_Registers permission
    update_cp0_access_for_pc(env);
}

void CHERI_HELPER_IMPL(ccheck_pc(CPUArchState *env, uint64_t next_pc))
{
    cap_register_t *pcc = &env->active_tc.PCC;

#ifdef CONFIG_MIPS_LOG_INSTR
    /* Print changed state before advancing to the next instruction: GPR, HI/LO, COP0. */
    const bool should_log_instr =
        qemu_loglevel_mask(CPU_LOG_CVTRACE | CPU_LOG_INSTR | CPU_LOG_USER_ONLY) || env->user_only_tracing_enabled;
    if (unlikely(should_log_instr))
        helper_dump_changed_state(env);
#endif

    /* Update statcounters icount */
    env->statcounters_icount++;
    if (in_kernel_mode(env)) {
        assert(((env->hflags & MIPS_HFLAG_CP0) == MIPS_HFLAG_CP0) == can_access_cp0(env));
        env->statcounters_icount_kernel++;
    } else {
        assert((env->hflags & MIPS_HFLAG_CP0) == 0);
        env->statcounters_icount_user++;
    }

    // branch instructions have already checked the validity of the target,
    // but we still need to check if the next instruction is accessible.
    // In order to ensure that EPC is set correctly we must set the offset
    // before checking the bounds.
    pcc->_cr_cursor = next_pc;
    check_cap(env, pcc, CAP_PERM_EXECUTE, next_pc, 0xff, 4, /*instavail=*/false, GETPC());
    // qemu_log("PC:%016lx\n", pc);

#ifdef CONFIG_MIPS_LOG_INSTR
    // Finally, log the instruction that will be executed next
    if (unlikely(should_log_instr)) {
        helper_log_instruction(env, next_pc);
    }
#endif
}

target_ulong CHERI_HELPER_IMPL(ccheck_load_right(CPUArchState *env, target_ulong offset, uint32_t len))
{
#ifndef TARGET_WORDS_BIGENDIAN
#error "This check is only valid for big endian targets, for little endian the load/store left instructions need to be checked"
#endif
    // For lwr/ldr we load all bytes if offset & 3/7 == 0 we load only the first byte, if all low bits are set we load the full amount
    uint32_t low_bits = (uint32_t)offset & (len - 1);
    uint32_t loaded_bytes = low_bits + 1;
    // From spec:
    //if BigEndianMem = 1 then
    //  pAddr <- pAddr(PSIZE-1)..3 || 000 (for ldr), 00 for lwr
    //endif
    // clear the low bits in offset to perform the length check
    target_ulong read_offset = offset & ~((target_ulong)len - 1);
    // fprintf(stderr, "%s: len=%d, offset=%zd, read_offset=%zd: will touch %d bytes\n",
    //      __func__, len, (size_t)offset, (size_t)read_offset, loaded_bytes);
    // return the actual address by adding the low bits (this is expected by translate.c
    return check_ddc(env, CAP_PERM_LOAD, read_offset, loaded_bytes, GETPC()) + low_bits;
}

target_ulong CHERI_HELPER_IMPL(ccheck_store(CPUArchState *env, target_ulong offset, uint32_t len))
{
    return check_ddc(env, CAP_PERM_STORE, offset, len, GETPC());
}

target_ulong CHERI_HELPER_IMPL(ccheck_load(CPUArchState *env, target_ulong offset, uint32_t len))
{
    return check_ddc(env, CAP_PERM_LOAD, offset, len, GETPC());
}

void CHERI_HELPER_IMPL(ccheck_load_pcrel(CPUArchState *env, target_ulong addr,
                                         uint32_t len)) {
    // Note: the address is already absolute, no need to add pcc.base
    check_cap(env, &env->active_tc.PCC, CAP_PERM_LOAD, addr, /*regnum=*/0, len,
              /*instavail=*/true, GETPC());
}

static const char *cheri_cap_reg[] = {
  "DDC",  "",   "",      "",     "",    "",    "",    "",  /* C00 - C07 */
     "",  "",   "",      "",     "",    "",    "",    "",  /* C08 - C15 */
     "",  "",   "",      "",     "",    "",    "",    "",  /* C16 - C23 */
  "RCC",  "", "IDC", "KR1C", "KR2C", "KCC", "KDC", "EPCC"  /* C24 - C31 */
};


static void cheri_dump_creg(const cap_register_t *crp, const char *name,
        const char *alias, FILE *f, fprintf_function cpu_fprintf)
{

#if 0
    if (crp->cr_tag) {
        cpu_fprintf(f, "%s: bas=%016lx len=%016lx cur=%016lx\n", name,
            // crp->cr_base, crp->cr_length, crp->cr_cursor);
            crp->cr_base, crp->cr_length, cap_get_cursor(crp));
        cpu_fprintf(f, "%-4s off=%016lx otype=%06x seal=%d "
		    "perms=%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c\n",
            // alias, (crp->cr_cursor - crp->cr_base), crp->cr_otype,
            alias, (uint64_t)cap_get_offset(crp), crp->cr_otype,
            is_cap_sealed(crp) ? 1 : 0,
            (crp->cr_perms & CAP_PERM_GLOBAL) ? 'G' : '-',
            (crp->cr_perms & CAP_PERM_EXECUTE) ? 'e' : '-',
            (crp->cr_perms & CAP_PERM_LOAD) ? 'l' : '-',
            (crp->cr_perms & CAP_PERM_STORE) ? 's' : '-',
            (crp->cr_perms & CAP_PERM_LOAD_CAP) ? 'L' : '-',
            (crp->cr_perms & CAP_PERM_STORE_CAP) ? 'S' : '-',
            (crp->cr_perms & CAP_PERM_STORE_LOCAL) ? '&' : '-',
            (crp->cr_perms & CAP_PERM_SEAL) ? '$' : '-',
            (crp->cr_perms & CAP_RESERVED1) ? 'R' : '-',
            (crp->cr_perms & CAP_RESERVED2) ? 'R' : '-',
            (crp->cr_perms & CAP_ACCESS_SYS_REGS) ? 'r' : '-');
    } else {
        cpu_fprintf(f, "%s: (not valid - tag not set)\n");
        cpu_fprintf(f, "%-4s\n", alias);
    }
#else
    /*
    cpu_fprintf(f, "DEBUG %s: v:%d s:%d p:%08x b:%016lx l:%016lx o:%016lx t:%08x\n",
            name, crp->cr_tag, is_cap_sealed(crp) ? 1 : 0,
            ((crp->cr_uperms & CAP_UPERMS_ALL) << CAP_UPERMS_SHFT) |
            (crp->cr_perms & CAP_PERMS_ALL), crp->cr_base, crp->cr_length,
            crp->cr_offset, crp->cr_otype);
    */

/* #define OLD_DEBUG_CAP */

#ifdef OLD_DEBUG_CAP
    cpu_fprintf(f, "DEBUG CAP %s u:%d perms:0x%08x type:0x%06x "
            "offset:0x%016lx base:0x%016lx length:0x%016lx\n",
            name, is_cap_sealed(crp),
#else
    cpu_fprintf(f, "DEBUG CAP %s t:%d s:%d perms:0x%08x type:0x%016" PRIx64 " "
            "offset:0x%016lx base:0x%016lx length:0x%016lx\n",
            name, crp->cr_tag, is_cap_sealed(crp),
#endif
            ((crp->cr_uperms & CAP_UPERMS_ALL) << CAP_UPERMS_SHFT) |
            (crp->cr_perms & CAP_PERMS_ALL),
            (uint64_t)cap_get_otype(crp), /* testsuite wants -1 for unsealed */
            (uint64_t)cap_get_offset(crp), cap_get_base(crp),
            cap_get_length64(crp) /* testsuite expects UINT64_MAX for 1 << 64) */);
#endif
}

void cheri_dump_state(CPUState *cs, FILE *f, fprintf_function cpu_fprintf, int flags)
{
    MIPSCPU *cpu = MIPS_CPU(cs);
    CPUArchState *env = &cpu->env;
    int i;
    char name[8];

    cpu_fprintf(f, "DEBUG CAP COREID 0\n");
    cheri_dump_creg(&env->active_tc.PCC, "PCC", "", f, cpu_fprintf);
    for (i = 0; i < 32; i++) {
        // snprintf(name, sizeof(name), "C%02d", i);
        snprintf(name, sizeof(name), "REG %02d", i);
        cheri_dump_creg(get_readonly_capreg(env, i), name, cheri_cap_reg[i], f,
                        cpu_fprintf);
    }
    cheri_dump_creg(&env->active_tc.CHWR.DDC,        "HWREG 00 (DDC)", "", f, cpu_fprintf);
    cheri_dump_creg(&env->active_tc.CHWR.UserTlsCap, "HWREG 01 (CTLSU)", "", f, cpu_fprintf);
    cheri_dump_creg(&env->active_tc.CHWR.PrivTlsCap, "HWREG 08 (CTLSP)", "", f, cpu_fprintf);
    cheri_dump_creg(&env->active_tc.CHWR.KR1C,       "HWREG 22 (KR1C)", "", f, cpu_fprintf);
    cheri_dump_creg(&env->active_tc.CHWR.KR2C,       "HWREG 23 (KR2C)", "", f, cpu_fprintf);
    cheri_dump_creg(&env->active_tc.CHWR.ErrorEPCC,  "HWREG 28 (ErrorEPCC)", "", f, cpu_fprintf);
    cheri_dump_creg(&env->active_tc.CHWR.KCC,        "HWREG 29 (KCC)", "", f, cpu_fprintf);
    cheri_dump_creg(&env->active_tc.CHWR.KDC,        "HWREG 30 (KDC)", "", f, cpu_fprintf);
    cheri_dump_creg(&env->active_tc.CHWR.EPCC,       "HWREG 31 (EPCC)", "", f, cpu_fprintf);

    cpu_fprintf(f, "\n");
}

void CHERI_HELPER_IMPL(mtc2_dumpcstate(CPUArchState *env, target_ulong arg1))
{
    FILE* logfile = qemu_log_enabled() ? qemu_log_lock() : stderr;
    cheri_dump_state(env_cpu(env), logfile, fprintf, CPU_DUMP_CODE);
    if (logfile != stderr)
        qemu_log_unlock(logfile);
}
