/*
 * S2E Selective Symbolic Execution Framework
 *
 * Copyright (c) 2010, Dependable Systems Laboratory, EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Dependable Systems Laboratory, EPFL nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE DEPENDABLE SYSTEMS LABORATORY, EPFL BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Currently maintained by:
 *    Volodymyr Kuznetsov <vova.kuznetsov@epfl.ch>
 *    Vitaly Chipounov <vitaly.chipounov@epfl.ch>
 *
 * All contributors are listed in S2E-AUTHORS file.
 *
 */

#ifndef TCG_LLVM_H
#define TCG_LLVM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tcg.h"

/*****************************/
/* Functions for QEMU c code */

struct TranslationBlock;
struct TCGLLVMContext;

extern struct TCGLLVMContext* tcg_llvm_ctx;

struct TCGLLVMRuntime {
    // NOTE: The order of these are fixed !
    uint64_t helper_ret_addr;
    uint64_t helper_call_addr;
    uint64_t helper_regs[3];
    // END of fixed block

#ifdef CONFIG_S2E
    /* run-time tb linking mechanism */
    uint8_t goto_tb;
#endif

#ifndef CONFIG_S2E
    TranslationBlock *last_tb;
    uint64_t last_opc_index;
    uint64_t last_pc;
#endif
};

extern struct TCGLLVMRuntime tcg_llvm_runtime;

struct TCGLLVMContext* tcg_llvm_initialize(void);
void tcg_llvm_close(struct TCGLLVMContext *l);

void tcg_llvm_tb_alloc(struct TranslationBlock *tb);
void tcg_llvm_tb_free(struct TranslationBlock *tb);

void tcg_llvm_gen_code(struct TCGLLVMContext *l, struct TCGContext *s,
                       struct TranslationBlock *tb);
const char* tcg_llvm_get_func_name(struct TranslationBlock *tb);

uintptr_t tcg_llvm_qemu_tb_exec(struct TranslationBlock *tb,
                            void* volatile* saved_AREGs);

#ifndef CONFIG_S2E
int tcg_llvm_search_last_pc(struct TranslationBlock *tb, uintptr_t searched_pc);
#endif

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

/***********************************/
/* External interface for C++ code */

namespace llvm {
    class Function;
    class LLVMContext;
    class Module;
    class ModuleProvider;
    class ExecutionEngine;
    class FunctionPassManager;
}

class TCGLLVMContextPrivate;
class TCGLLVMContext
{
private:
    TCGLLVMContextPrivate* m_private;

public:
    TCGLLVMContext();
    ~TCGLLVMContext();

    llvm::LLVMContext& getLLVMContext();

    llvm::Module* getModule();
    llvm::ModuleProvider* getModuleProvider();

    llvm::ExecutionEngine* getExecutionEngine();

    void deleteExecutionEngine();
    llvm::FunctionPassManager* getFunctionPassManager() const;

#ifdef CONFIG_S2E
    /** Called after linking all helper libraries */
    void initializeHelpers();
#endif

    void generateCode(struct TCGContext *s,
                      struct TranslationBlock *tb);
};

#endif

#endif

