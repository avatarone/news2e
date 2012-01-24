/*
 *  Host code generation
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * The file was modified for S2E Selective Symbolic Execution Framework
 *
 * Copyright (c) 2010, Dependable Systems Laboratory, EPFL
 *
 * Currently maintained by:
 *    Volodymyr Kuznetsov <vova.kuznetsov@epfl.ch>
 *    Vitaly Chipounov <vitaly.chipounov@epfl.ch>
 *
 * All contributors are listed in S2E-AUTHORS file.
 *
 */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "config.h"

#define NO_CPU_IO_DEFS
#include "cpu.h"
#include "exec-all.h"
#include "disas.h"
#include "tcg.h"

#ifdef CONFIG_LLVM
#include "tcg-llvm.h"
#endif

#ifdef CONFIG_S2E
#include "s2e/s2e_qemu.h"
#endif

/* code generation context */
TCGContext tcg_ctx;

uint16_t gen_opc_buf[OPC_BUF_SIZE];
TCGArg gen_opparam_buf[OPPARAM_BUF_SIZE];

target_ulong gen_opc_pc[OPC_BUF_SIZE];
uint16_t gen_opc_icount[OPC_BUF_SIZE];
uint8_t gen_opc_instr_start[OPC_BUF_SIZE];
#if defined(TARGET_I386)
uint8_t gen_opc_cc_op[OPC_BUF_SIZE];
#elif defined(TARGET_SPARC)
target_ulong gen_opc_npc[OPC_BUF_SIZE];
target_ulong gen_opc_jump_pc[2];
#elif defined(TARGET_MIPS) || defined(TARGET_SH4)
uint32_t gen_opc_hflags[OPC_BUF_SIZE];
#endif

/* XXX: suppress that */
uintptr_t code_gen_max_block_size(void)
{
    static uintptr_t max;

    if (max == 0) {
        max = TCG_MAX_OP_SIZE;
#define DEF(s, n, copy_size) max = copy_size > max? copy_size : max;
#include "tcg-opc.h"
#undef DEF
        max *= OPC_MAX_SIZE;
    }

    return max;
}

void cpu_gen_init(void)
{
    tcg_context_init(&tcg_ctx); 
    tcg_set_frame(&tcg_ctx, TCG_AREG0, offsetof(CPUState, temp_buf),
                  CPU_TEMP_BUF_NLONGS * sizeof(intptr_t));
}

/* return non zero if the very first instruction is invalid so that
   the virtual CPU can trigger an exception.

   '*gen_code_size_ptr' contains the size of the generated code (host
   code).
*/
int cpu_gen_code(CPUState *env, TranslationBlock *tb, int *gen_code_size_ptr)
{
    TCGContext *s = &tcg_ctx;
    uint8_t *gen_code_buf;
    int gen_code_size;
#ifdef CONFIG_PROFILER
    int64_t ti;
#endif

#ifdef CONFIG_PROFILER
    s->tb_count1++; /* includes aborted translations because of
                       exceptions */
    ti = profile_getclock();
#endif
    tcg_func_start(s);

    gen_intermediate_code(env, tb);

    /* generate machine code */
    gen_code_buf = tb->tc_ptr;
    tb->tb_next_offset[0] = 0xffff;
    tb->tb_next_offset[1] = 0xffff;
    s->tb_next_offset = tb->tb_next_offset;
#ifdef USE_DIRECT_JUMP
    s->tb_jmp_offset = tb->tb_jmp_offset;
    s->tb_next = NULL;
    /* the following two entries are optional (only used for string ops) */
    /* XXX: not used ? */
    tb->tb_jmp_offset[2] = 0xffff;
    tb->tb_jmp_offset[3] = 0xffff;
#else
    s->tb_jmp_offset = NULL;
    s->tb_next = tb->tb_next;
#endif

#ifdef CONFIG_PROFILER
    s->tb_count++;
    s->interm_time += profile_getclock() - ti;
    s->code_time -= profile_getclock();
#endif
    gen_code_size = tcg_gen_code(s, gen_code_buf);
    *gen_code_size_ptr = gen_code_size;

#ifdef CONFIG_S2E
    tcg_calc_regmask(s, &tb->reg_rmask, &tb->reg_wmask,
                     &tb->helper_accesses_mem);
#endif

#if defined(CONFIG_LLVM) && !defined(CONFIG_S2E)
    if(generate_llvm)
        tcg_llvm_gen_code(tcg_llvm_ctx, s, tb);
#endif

#ifdef CONFIG_PROFILER
    s->code_time += profile_getclock();
    s->code_in_len += tb->size;
    s->code_out_len += gen_code_size;
#endif

#ifdef DEBUG_DISAS
    if (qemu_loglevel_mask(CPU_LOG_TB_OUT_ASM)) {
        qemu_log("OUT: [size=%d]\n", *gen_code_size_ptr);
        log_disas(tb->tc_ptr, *gen_code_size_ptr);
        qemu_log("\n");
        qemu_log_flush();
    }

#if defined(CONFIG_LLVM) && !defined(CONFIG_S2E)
    if(generate_llvm && qemu_loglevel_mask(CPU_LOG_LLVM_ASM)
            && tb->llvm_tc_ptr) {
        ptrdiff_t size = tb->llvm_tc_end - tb->llvm_tc_ptr;
        qemu_log("OUT (LLVM ASM) [size=%ld] (%s)\n", size,
                    tcg_llvm_get_func_name(tb));
        log_disas((void*) tb->llvm_tc_ptr, size);
        qemu_log("\n");
        qemu_log_flush();
    }
#endif
#endif
    return 0;
}

#ifdef CONFIG_S2E
void cpu_restore_icount(CPUState *env)
{
    if(use_icount) {
        /* If we are not executing TB, s2e_icoun equals s2e_icount_after_tb */
        if(env->s2e_current_tb) {
            assert(env->s2e_icount_after_tb - env->s2e_icount +
                            env->icount_decr.u16.low <= 0xffff);
            env->icount_decr.u16.low += (env->s2e_icount_after_tb - env->s2e_icount);
            env->s2e_icount_after_tb = env->s2e_icount;
            env->s2e_icount_before_tb = env->s2e_icount;
        } else {
            assert(env->s2e_icount == env->s2e_icount_after_tb);
        }
        assert(env->s2e_icount == qemu_icount - env->icount_decr.u16.low
                                             - env->icount_extra);
    }
}
#endif

/* The cpu state corresponding to 'searched_pc' is restored.
 */
int cpu_restore_state(TranslationBlock *tb,
                      CPUState *env, uintptr_t searched_pc,
                      void *puc)
{

    //printf("searched_pc=%"PRIx64" pc=%"PRIx64"\n", searched_pc, (uint64_t)env->eip);
/**
 *  The retranslation mechanism interferes with S2E's instrumentation.
 *  We get rid of the retranslation by saving the pc and flags explicitely
 *  before each instruction.
 */
#ifndef CONFIG_S2E
    TCGContext *s = &tcg_ctx;
    int j;
    uintptr_t tc_ptr;
#ifdef CONFIG_PROFILER
    int64_t ti;
#endif

#ifdef CONFIG_PROFILER
    ti = profile_getclock();
#endif
    tcg_func_start(s);

    gen_intermediate_code_pc(env, tb);

    if (use_icount) {
        /* Reset the cycle counter to the start of the block.  */
        env->icount_decr.u16.low += tb->icount;
        /* Clear the IO flag.  */
        env->can_do_io = 0;
    }

    s->tb_next_offset = tb->tb_next_offset;
#ifdef USE_DIRECT_JUMP
    s->tb_jmp_offset = tb->tb_jmp_offset;
    s->tb_next = NULL;
#else
    s->tb_jmp_offset = NULL;
    s->tb_next = tb->tb_next;
#endif

#if defined(CONFIG_LLVM) && !defined(CONFIG_S2E)
    if(execute_llvm) {
        assert(tb->llvm_function != NULL);
        j = tcg_llvm_search_last_pc(tb, searched_pc);
    } else {
#endif
    /* find opc index corresponding to search_pc */
    tc_ptr = (uintptr_t)tb->tc_ptr;
    if (searched_pc < tc_ptr)
        return -1;

    j = tcg_gen_code_search_pc(s, (uint8_t *)tc_ptr, searched_pc - tc_ptr);
    if (j < 0)
        return -1;
    /* now find start of instruction before */
    while (gen_opc_instr_start[j] == 0)
        j--;
#ifdef CONFIG_LLVM
    }
#endif

    env->icount_decr.u16.low -= gen_opc_icount[j];

    gen_pc_load(env, tb, searched_pc, j, puc);

#ifdef CONFIG_PROFILER
    s->restore_time += profile_getclock() - ti;
    s->restore_count++;
#endif

#endif
    return 0;
}

#ifdef CONFIG_S2E

/** Generates LLVM code for already translated TB */
int cpu_gen_llvm(CPUState *env, TranslationBlock *tb)
{
    TCGContext *s = &tcg_ctx;
    assert(tb->llvm_function == NULL);

    tcg_func_start(s);
    gen_intermediate_code_pc(env, tb);
    tcg_llvm_gen_code(tcg_llvm_ctx, s, tb);
    s2e_set_tb_function(g_s2e, tb);

    if(qemu_loglevel_mask(CPU_LOG_LLVM_ASM) && tb->llvm_tc_ptr) {
        ptrdiff_t size = tb->llvm_tc_end - tb->llvm_tc_ptr;
        qemu_log("OUT (LLVM ASM) [size=%ld] (%s)\n", size,
                    tcg_llvm_get_func_name(tb));
        log_disas((void*) tb->llvm_tc_ptr, size);
        qemu_log("\n");
        qemu_log_flush();
    }

    return 0;
}

#endif
