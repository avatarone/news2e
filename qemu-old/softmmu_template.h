/*
 *  Software MMU support
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

#define DATA_SIZE (1 << SHIFT)

#if DATA_SIZE == 8
#define SUFFIX q
#define USUFFIX q
#define DATA_TYPE uint64_t
#elif DATA_SIZE == 4
#define SUFFIX l
#define USUFFIX l
#define DATA_TYPE uint32_t
#elif DATA_SIZE == 2
#define SUFFIX w
#define USUFFIX uw
#define DATA_TYPE uint16_t
#elif DATA_SIZE == 1
#define SUFFIX b
#define USUFFIX ub
#define DATA_TYPE uint8_t
#else
#error unsupported data size
#endif

#ifdef SOFTMMU_CODE_ACCESS
#define READ_ACCESS_TYPE 2
#define ADDR_READ addr_code
#else
#define READ_ACCESS_TYPE 0
#define ADDR_READ addr_read
#endif

#define ADDR_MAX 0xffffffff

#ifdef CONFIG_S2E
#include <s2e/s2e_config.h>

#ifdef S2E_LLVM_LIB
#define S2E_TRACE_MEMORY(vaddr, haddr, value, isWrite, isIO) \
    tcg_llvm_trace_memory_access(vaddr, haddr, \
                                 value, 8*sizeof(value), isWrite, isIO);
#define S2E_FORK_AND_CONCRETIZE(val, max) \
    tcg_llvm_fork_and_concretize(val, 0, max)
#else // S2E_LLVM_LIB
#define S2E_TRACE_MEMORY(vaddr, haddr, value, isWrite, isIO) \
    s2e_trace_memory_access(vaddr, haddr, \
                            (uint8_t*) &value, sizeof(value), isWrite, isIO);
#define S2E_FORK_AND_CONCRETIZE(val, max) (val)
#endif // S2E_LLVM_LIB


#define S2E_FORK_AND_CONCRETIZE_ADDR(val, max) \
    (g_s2e_fork_on_symbolic_address ? S2E_FORK_AND_CONCRETIZE(val, max) : val)

#define S2E_RAM_OBJECT_DIFF (TARGET_PAGE_BITS - S2E_RAM_OBJECT_BITS)

#else // CONFIG_S2E

#define S2E_TRACE_MEMORY(...)
#define S2E_FORK_AND_CONCRETIZE(val, max) (val)
#define S2E_FORK_AND_CONCRETIZE_ADDR(val, max) (val)

#define S2E_RAM_OBJECT_BITS TARGET_PAGE_BITS
#define S2E_RAM_OBJECT_SIZE TARGET_PAGE_SIZE
#define S2E_RAM_OBJECT_MASK TARGET_PAGE_MASK
#define S2E_RAM_OBJECT_DIFF 0

#endif // CONFIG_S2E

static DATA_TYPE glue(glue(slow_ld, SUFFIX), MMUSUFFIX)(target_ulong addr,
                                                        int mmu_idx,
                                                        void *retaddr);

#ifndef S2E_LLVM_LIB

inline DATA_TYPE glue(glue(io_read, SUFFIX), MMUSUFFIX)(target_phys_addr_t physaddr,
                                              target_ulong addr,
                                              void *retaddr)
{
    DATA_TYPE res;
    int index;
    index = (physaddr >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
    physaddr = (physaddr & TARGET_PAGE_MASK) + addr;

#ifdef CONFIG_S2E
    if (glue(s2e_is_mmio_symbolic_, SUFFIX)(physaddr)) {
        s2e_switch_to_symbolic(g_s2e, g_s2e_state);
    }
#endif

    env->mem_io_pc = (uintptr_t)retaddr;
    if (index > (IO_MEM_NOTDIRTY >> IO_MEM_SHIFT)
            && !can_do_io(env)) {
        cpu_io_recompile(env, retaddr);
    }

    env->mem_io_vaddr = addr;
#if SHIFT <= 2
    res = io_mem_read[index][SHIFT](io_mem_opaque[index], physaddr);
#else
#ifdef TARGET_WORDS_BIGENDIAN
    res = (uint64_t)io_mem_read[index][2](io_mem_opaque[index], physaddr) << 32;
    res |= io_mem_read[index][2](io_mem_opaque[index], physaddr + 4);
#else
    res = io_mem_read[index][2](io_mem_opaque[index], physaddr);
    res |= (uint64_t)io_mem_read[index][2](io_mem_opaque[index], physaddr + 4) << 32;
#endif
#endif /* SHIFT > 2 */
    return res;
}

inline DATA_TYPE glue(glue(io_read_chk, SUFFIX), MMUSUFFIX)(target_phys_addr_t physaddr,
                                          target_ulong addr,
                                          void *retaddr)
{
    return glue(glue(io_read, SUFFIX), MMUSUFFIX)(physaddr, addr, retaddr);
}


#else

inline DATA_TYPE glue(glue(io_make_symbolic, SUFFIX), MMUSUFFIX)(const char *name) {
    uint8_t ret;
    klee_make_symbolic(&ret, sizeof(ret), name);
    return ret;
}


inline DATA_TYPE glue(glue(io_read_chk_symb_, SUFFIX), MMUSUFFIX)(const char *label, target_ulong physaddr, uintptr_t pa)
{
    union {
        DATA_TYPE dt;
        uint8_t arr[1<<SHIFT];
    }data;
    unsigned i;

    data.dt = glue(glue(ld, USUFFIX), _raw)((uint8_t *)(intptr_t)(pa));

    for (i = 0; i<(1<<SHIFT); ++i) {
        if (s2e_is_mmio_symbolic_b(physaddr + i)) {
            data.arr[i] = glue(glue(io_make_symbolic, SUFFIX), MMUSUFFIX)(label);
        }
    }
    return data.dt;
}

inline DATA_TYPE glue(glue(io_read_chk, SUFFIX), MMUSUFFIX)(target_phys_addr_t physaddr,
                                          target_ulong addr,
                                          void *retaddr)
{
    target_ulong naddr = (physaddr & TARGET_PAGE_MASK)+addr;
    char label[64];
    int isSymb = 0;
    if ((isSymb = glue(s2e_is_mmio_symbolic_, SUFFIX)(naddr))) {
        //If at least one byte is symbolic, generate a label
        trace_port(label, "iommuread_", naddr, env->eip);
    }

    //If it is not DMA, then check if it is normal memory
    int index;
    target_phys_addr_t oldphysaddr = physaddr;
    index = (physaddr >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
    physaddr = (physaddr & TARGET_PAGE_MASK) + addr;
    env->mem_io_pc = (uintptr_t)retaddr;
    if (index > (IO_MEM_NOTDIRTY >> IO_MEM_SHIFT)
            && !can_do_io(env)) {
        cpu_io_recompile(env, retaddr);
    }

    env->mem_io_vaddr = addr;
#if SHIFT <= 2
    if (s2e_ismemfunc(io_mem_read[index][SHIFT])) {
        uintptr_t pa = s2e_notdirty_mem_write(physaddr);
        if (isSymb) {
            return glue(glue(io_read_chk_symb_, SUFFIX), MMUSUFFIX)(label, naddr, (uintptr_t)(pa));
        }
        return glue(glue(ld, USUFFIX), _raw)((uint8_t *)(intptr_t)(pa));
    }
#else
#ifdef TARGET_WORDS_BIGENDIAN
    if (s2e_ismemfunc(io_mem_read[index][SHIFT])) {
        DATA_TYPE res;
        uintptr_t pa = s2e_notdirty_mem_write(physaddr);

        if (isSymb) {
            res = glue(glue(io_read_chk_symb_, SUFFIX), MMUSUFFIX)(label, naddr, (uintptr_t)(pa)) << 32;
            res |= glue(glue(io_read_chk_symb_, SUFFIX), MMUSUFFIX)(label, naddr,(uintptr_t)(pa+4));
        }else {
            res = glue(glue(ld, USUFFIX), _raw)((uint8_t *)(intptr_t)(pa)) << 32;
            res |= glue(glue(ld, USUFFIX), _raw)((uint8_t *)(intptr_t)(pa+4));
        }

        return res;
    }
#else
    if (s2e_ismemfunc(io_mem_read[index][SHIFT])) {
        DATA_TYPE res;
        uintptr_t pa = s2e_notdirty_mem_write(physaddr);
        if (isSymb) {
            res = glue(glue(io_read_chk_symb_, SUFFIX), MMUSUFFIX)(label, naddr, (uintptr_t)(pa));
            res |= glue(glue(io_read_chk_symb_, SUFFIX), MMUSUFFIX)(label, naddr, (uintptr_t)(pa+4)) << 32;
        }else {
            res = glue(glue(ld, USUFFIX), _raw)((uint8_t *)(intptr_t)(pa));
            res |= glue(glue(ld, USUFFIX), _raw)((uint8_t *)(intptr_t)(pa + 4)) << 32;
        }
        return res;
    }
#endif
#endif /* SHIFT > 2 */

    //By default, call the original io_read function, which is external
    return glue(glue(io_read, SUFFIX), MMUSUFFIX)(oldphysaddr, addr, retaddr);
}


#endif

/* handle all cases except unaligned access which span two pages */
DATA_TYPE REGPARM glue(glue(__ld, SUFFIX), MMUSUFFIX)(target_ulong addr,
                                                      int mmu_idx)
{
    DATA_TYPE res;
    int object_index, index;
    target_ulong tlb_addr;
    target_phys_addr_t addend;
    void *retaddr;

    /* test if there is match for unaligned or IO access */
    /* XXX: could done more in memory macro in a non portable way */
    addr = S2E_FORK_AND_CONCRETIZE_ADDR(addr, ADDR_MAX);
    object_index = S2E_FORK_AND_CONCRETIZE(addr >> S2E_RAM_OBJECT_BITS,
                                           ADDR_MAX >> S2E_RAM_OBJECT_BITS);
    index = (object_index >> S2E_RAM_OBJECT_DIFF) & (CPU_TLB_SIZE - 1);
 redo:
    tlb_addr = env->tlb_table[mmu_idx][index].ADDR_READ;
    if (likely((addr & TARGET_PAGE_MASK) == (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK)))) {
        if (unlikely(tlb_addr & ~TARGET_PAGE_MASK)) {
            /* IO access */
            if ((addr & (DATA_SIZE - 1)) != 0)
                goto do_unaligned_access;
            retaddr = GETPC();
            addend = env->iotlb[mmu_idx][index];
            res = glue(glue(io_read_chk, SUFFIX), MMUSUFFIX)(addend, addr, retaddr);

            S2E_TRACE_MEMORY(addr, addr+addend, res, 0, 1);

        } else if (unlikely(((addr & ~S2E_RAM_OBJECT_MASK) + DATA_SIZE - 1) >= S2E_RAM_OBJECT_SIZE)) {
            /* slow unaligned access (it spans two pages or IO) */
        do_unaligned_access:
            retaddr = GETPC();
#ifdef ALIGNED_ONLY
            do_unaligned_access(addr, READ_ACCESS_TYPE, mmu_idx, retaddr);
#endif
            res = glue(glue(slow_ld, SUFFIX), MMUSUFFIX)(addr,
                                                         mmu_idx, retaddr);
        } else {
            /* unaligned/aligned access in the same page */
#ifdef ALIGNED_ONLY
            if ((addr & (DATA_SIZE - 1)) != 0) {
                retaddr = GETPC();
                do_unaligned_access(addr, READ_ACCESS_TYPE, mmu_idx, retaddr);
            }
#endif
            addend = env->tlb_table[mmu_idx][index].addend;

#if defined(CONFIG_S2E) && defined(S2E_ENABLE_S2E_TLB) && !defined(S2E_LLVM_LIB)
            S2ETLBEntry *e = &env->s2e_tlb_table[mmu_idx][object_index & (CPU_S2E_TLB_SIZE-1)];
            if(likely(_s2e_check_concrete(e->objectState, addr & ~S2E_RAM_OBJECT_MASK, DATA_SIZE)))
                res = glue(glue(ld, USUFFIX), _p)((uint8_t*)(addr + (e->addend&~1)));
            else
#endif
                res = glue(glue(ld, USUFFIX), _raw)((uint8_t *)(intptr_t)(addr+addend));

            S2E_TRACE_MEMORY(addr, addr+addend, res, 0, 0);
        }
    } else {
        /* the page is not in the TLB : fill it */
        retaddr = GETPC();
#ifdef ALIGNED_ONLY
        if ((addr & (DATA_SIZE - 1)) != 0)
            do_unaligned_access(addr, READ_ACCESS_TYPE, mmu_idx, retaddr);
#endif
        tlb_fill(addr, object_index << S2E_RAM_OBJECT_BITS,
                 READ_ACCESS_TYPE, mmu_idx, retaddr);
        goto redo;
    }

    return res;
}

/* handle all unaligned cases */
static DATA_TYPE glue(glue(slow_ld, SUFFIX), MMUSUFFIX)(target_ulong addr,
                                                        int mmu_idx,
                                                        void *retaddr)
{
    DATA_TYPE res, res1, res2;
    int object_index, index, shift;
    target_phys_addr_t addend;
    target_ulong tlb_addr, addr1, addr2;

    addr = S2E_FORK_AND_CONCRETIZE_ADDR(addr, ADDR_MAX);
    object_index = S2E_FORK_AND_CONCRETIZE(addr >> S2E_RAM_OBJECT_BITS,
                                           ADDR_MAX >> S2E_RAM_OBJECT_BITS);
    index = (object_index >> S2E_RAM_OBJECT_DIFF) & (CPU_TLB_SIZE - 1);
 redo:
    tlb_addr = env->tlb_table[mmu_idx][index].ADDR_READ;
    if ((addr & TARGET_PAGE_MASK) == (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        if (tlb_addr & ~TARGET_PAGE_MASK) {
            /* IO access */
            if ((addr & (DATA_SIZE - 1)) != 0)
                goto do_unaligned_access;
            retaddr = GETPC();
            addend = env->iotlb[mmu_idx][index];
            res = glue(glue(io_read_chk, SUFFIX), MMUSUFFIX)(addend, addr, retaddr);

            S2E_TRACE_MEMORY(addr, addr+addend, res, 0, 1);
        } else if (((addr & ~S2E_RAM_OBJECT_MASK) + DATA_SIZE - 1) >= S2E_RAM_OBJECT_SIZE) {
        do_unaligned_access:
            /* slow unaligned access (it spans two pages) */
            addr1 = addr & ~(DATA_SIZE - 1);
            addr2 = addr1 + DATA_SIZE;
            res1 = glue(glue(slow_ld, SUFFIX), MMUSUFFIX)(addr1,
                                                          mmu_idx, retaddr);
            res2 = glue(glue(slow_ld, SUFFIX), MMUSUFFIX)(addr2,
                                                          mmu_idx, retaddr);
            shift = (addr & (DATA_SIZE - 1)) * 8;
#ifdef TARGET_WORDS_BIGENDIAN
            res = (res1 << shift) | (res2 >> ((DATA_SIZE * 8) - shift));
#else
            res = (res1 >> shift) | (res2 << ((DATA_SIZE * 8) - shift));
#endif
            res = (DATA_TYPE)res;
        } else {
            /* unaligned/aligned access in the same page */
            addend = env->tlb_table[mmu_idx][index].addend;

#if defined(CONFIG_S2E) && defined(S2E_ENABLE_S2E_TLB) && !defined(S2E_LLVM_LIB)
            S2ETLBEntry *e = &env->s2e_tlb_table[mmu_idx][object_index & (CPU_S2E_TLB_SIZE-1)];
            if(_s2e_check_concrete(e->objectState, addr & ~S2E_RAM_OBJECT_MASK, DATA_SIZE))
                res = glue(glue(ld, USUFFIX), _p)((uint8_t*)(addr + (e->addend&~1)));
            else
#endif
                res = glue(glue(ld, USUFFIX), _raw)((uint8_t *)(intptr_t)(addr+addend));

            S2E_TRACE_MEMORY(addr, addr+addend, res, 0, 0);
        }
    } else {
        /* the page is not in the TLB : fill it */
        tlb_fill(addr, object_index << S2E_RAM_OBJECT_BITS,
                 READ_ACCESS_TYPE, mmu_idx, retaddr);
        goto redo;
    }
    return res;
}

/*************************************************************************************/

#ifndef SOFTMMU_CODE_ACCESS

static void glue(glue(slow_st, SUFFIX), MMUSUFFIX)(target_ulong addr,
                                                   DATA_TYPE val,
                                                   int mmu_idx,
                                                   void *retaddr);

#ifndef S2E_LLVM_LIB

inline void glue(glue(io_write, SUFFIX), MMUSUFFIX)(target_phys_addr_t physaddr,
                                          DATA_TYPE val,
                                          target_ulong addr,
                                          void *retaddr)
{
    int index;
    index = (physaddr >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
    physaddr = (physaddr & TARGET_PAGE_MASK) + addr;
    if (index > (IO_MEM_NOTDIRTY >> IO_MEM_SHIFT)
            && !can_do_io(env)) {
        cpu_io_recompile(env, retaddr);
    }

    env->mem_io_vaddr = addr;
    env->mem_io_pc = (uintptr_t)retaddr;
#if SHIFT <= 2
    io_mem_write[index][SHIFT](io_mem_opaque[index], physaddr, val);
#else
#ifdef TARGET_WORDS_BIGENDIAN
    io_mem_write[index][2](io_mem_opaque[index], physaddr, val >> 32);
    io_mem_write[index][2](io_mem_opaque[index], physaddr + 4, val);
#else
    io_mem_write[index][2](io_mem_opaque[index], physaddr, val);
    io_mem_write[index][2](io_mem_opaque[index], physaddr + 4, val >> 32);
#endif
#endif /* SHIFT > 2 */
}

inline void glue(glue(io_write_chk, SUFFIX), MMUSUFFIX)(target_phys_addr_t physaddr,
                                          DATA_TYPE val,
                                          target_ulong addr,
                                          void *retaddr)
{
    //XXX: check symbolic memory mapped devices and write log here.
    glue(glue(io_write, SUFFIX), MMUSUFFIX)(physaddr, val, addr, retaddr);
}

#else

/**
  * Only if compiling for LLVM.
  * This function checks whether a write goes to a clean memory page.
  * If yes, does the write directly.
  * This avoids symbolic values flowing outside the LLVM code and killing the states.
  *
  * It also deals with writes to memory-mapped devices that are symbolic
  */
inline void glue(glue(io_write_chk, SUFFIX), MMUSUFFIX)(target_phys_addr_t physaddr,
                                          DATA_TYPE val,
                                          target_ulong addr,
                                          void *retaddr)
{
    int index;
    target_phys_addr_t oldphysaddr = physaddr;
    index = (physaddr >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
    physaddr = (physaddr & TARGET_PAGE_MASK) + addr;
    if (index > (IO_MEM_NOTDIRTY >> IO_MEM_SHIFT)
            && !can_do_io(env)) {
        cpu_io_recompile(env, retaddr);
    }

    env->mem_io_vaddr = addr;
    env->mem_io_pc = (uintptr_t)retaddr;
#if SHIFT <= 2
    if (s2e_ismemfunc(io_mem_write[index][SHIFT])) {
        uintptr_t pa = s2e_notdirty_mem_write(physaddr);
        glue(glue(st, SUFFIX), _raw)((uint8_t *)(intptr_t)(pa), val);
        return;
    }
#else
#ifdef TARGET_WORDS_BIGENDIAN
    if (s2e_ismemfunc(io_mem_write[index][SHIFT])) {
        uintptr_t pa = s2e_notdirty_mem_write(physaddr);
        stl_raw((uint8_t *)(intptr_t)(pa), val>>32);
        stl_raw((uint8_t *)(intptr_t)(pa+4), val);
        return;
    }
#else
    if (s2e_ismemfunc(io_mem_write[index][SHIFT])) {
        uintptr_t pa = s2e_notdirty_mem_write(physaddr);
        stl_raw((uint8_t *)(intptr_t)(pa), val);
        stl_raw((uint8_t *)(intptr_t)(pa+4), val>>32);
        return;
    }
#endif
#endif /* SHIFT > 2 */

    //XXX: Check if MMIO is symbolic, and add corresponding trace entry

    //Since we do not handle symbolic devices for now, we offer the
    //option of concretizing the arguments to I/O helpers.
    if (g_s2e_concretize_io_writes) {
        val = klee_get_value(val);
    }

    if (g_s2e_concretize_io_addresses) {
        addr = klee_get_value(addr);
    }

    //By default, call the original io_write function, which is external
    glue(glue(io_write, SUFFIX), MMUSUFFIX)(oldphysaddr, val, addr, retaddr);
}

#endif

void REGPARM glue(glue(__st, SUFFIX), MMUSUFFIX)(target_ulong addr,
                                                 DATA_TYPE val,
                                                 int mmu_idx)
{
    target_phys_addr_t addend;
    target_ulong tlb_addr;
    void *retaddr;
    int object_index, index;

    addr = S2E_FORK_AND_CONCRETIZE_ADDR(addr, ADDR_MAX);
    object_index = S2E_FORK_AND_CONCRETIZE(addr >> S2E_RAM_OBJECT_BITS,
                                           ADDR_MAX >> S2E_RAM_OBJECT_BITS);
    index = (object_index >> S2E_RAM_OBJECT_DIFF) & (CPU_TLB_SIZE - 1);
 redo:
    tlb_addr = env->tlb_table[mmu_idx][index].addr_write;
    if (likely((addr & TARGET_PAGE_MASK) == (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK)))) {
        if (unlikely(tlb_addr & ~TARGET_PAGE_MASK)) {
            /* IO access */
            if ((addr & (DATA_SIZE - 1)) != 0)
                goto do_unaligned_access;
            retaddr = GETPC();
            addend = env->iotlb[mmu_idx][index];
            glue(glue(io_write_chk, SUFFIX), MMUSUFFIX)(addend, val, addr, retaddr);

            S2E_TRACE_MEMORY(addr, addr+addend, val, 1, 1);
        } else if (unlikely(((addr & ~S2E_RAM_OBJECT_MASK) + DATA_SIZE - 1) >= S2E_RAM_OBJECT_SIZE)) {
        do_unaligned_access:
            retaddr = GETPC();
#ifdef ALIGNED_ONLY
            do_unaligned_access(addr, 1, mmu_idx, retaddr);
#endif
            glue(glue(slow_st, SUFFIX), MMUSUFFIX)(addr, val,
                                                   mmu_idx, retaddr);
        } else {
            /* aligned/unaligned access in the same page */
#ifdef ALIGNED_ONLY
            if ((addr & (DATA_SIZE - 1)) != 0) {
                retaddr = GETPC();
                do_unaligned_access(addr, 1, mmu_idx, retaddr);
            }
#endif
            addend = env->tlb_table[mmu_idx][index].addend;

#if defined(CONFIG_S2E) && defined(S2E_ENABLE_S2E_TLB) && !defined(S2E_LLVM_LIB)
            S2ETLBEntry *e = &env->s2e_tlb_table[mmu_idx][object_index & (CPU_S2E_TLB_SIZE-1)];
            if(likely((e->addend & 1) && _s2e_check_concrete(e->objectState, addr & ~S2E_RAM_OBJECT_MASK, DATA_SIZE)))
                glue(glue(st, SUFFIX), _p)((uint8_t*)(addr + (e->addend&~1)), val);
            else
#endif
                glue(glue(st, SUFFIX), _raw)((uint8_t *)(intptr_t)(addr+addend), val);

            S2E_TRACE_MEMORY(addr, addr+addend, val, 1, 0);
        }
    } else {
        /* the page is not in the TLB : fill it */
        retaddr = GETPC();
#ifdef ALIGNED_ONLY
        if ((addr & (DATA_SIZE - 1)) != 0)
            do_unaligned_access(addr, 1, mmu_idx, retaddr);
#endif
        tlb_fill(addr, object_index << S2E_RAM_OBJECT_BITS,
                 1, mmu_idx, retaddr);
        goto redo;
    }
}

/* handles all unaligned cases */
static void glue(glue(slow_st, SUFFIX), MMUSUFFIX)(target_ulong addr,
                                                   DATA_TYPE val,
                                                   int mmu_idx,
                                                   void *retaddr)
{
    target_phys_addr_t addend;
    target_ulong tlb_addr;
    int object_index, index, i;

    addr = S2E_FORK_AND_CONCRETIZE_ADDR(addr, ADDR_MAX);
    object_index = S2E_FORK_AND_CONCRETIZE(addr >> S2E_RAM_OBJECT_BITS,
                                           ADDR_MAX >> S2E_RAM_OBJECT_BITS);
    index = (object_index >> S2E_RAM_OBJECT_DIFF) & (CPU_TLB_SIZE - 1);
 redo:
    tlb_addr = env->tlb_table[mmu_idx][index].addr_write;
    if ((addr & TARGET_PAGE_MASK) == (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        if (tlb_addr & ~TARGET_PAGE_MASK) {
            /* IO access */
            if ((addr & (DATA_SIZE - 1)) != 0)
                goto do_unaligned_access;
            addend = env->iotlb[mmu_idx][index];
            glue(glue(io_write_chk, SUFFIX), MMUSUFFIX)(addend, val, addr, retaddr);

            S2E_TRACE_MEMORY(addr, addr+addend, val, 1, 1);
        } else if (((addr & ~S2E_RAM_OBJECT_MASK) + DATA_SIZE - 1) >= S2E_RAM_OBJECT_SIZE) {
        do_unaligned_access:
            /* XXX: not efficient, but simple */
            /* Note: relies on the fact that tlb_fill() does not remove the
             * previous page from the TLB cache.  */
            for(i = DATA_SIZE - 1; i >= 0; i--) {
#ifdef TARGET_WORDS_BIGENDIAN
                glue(slow_stb, MMUSUFFIX)(addr + i, val >> (((DATA_SIZE - 1) * 8) - (i * 8)),
                                          mmu_idx, retaddr);
#else
                glue(slow_stb, MMUSUFFIX)(addr + i, val >> (i * 8),
                                          mmu_idx, retaddr);
#endif
            }
        } else {
            /* aligned/unaligned access in the same page */
            addend = env->tlb_table[mmu_idx][index].addend;

#if defined(CONFIG_S2E) && defined(S2E_ENABLE_S2E_TLB) && !defined(S2E_LLVM_LIB)
            S2ETLBEntry *e = &env->s2e_tlb_table[mmu_idx][object_index & (CPU_S2E_TLB_SIZE-1)];
            if((e->addend & 1) && _s2e_check_concrete(e->objectState, addr & ~S2E_RAM_OBJECT_MASK, DATA_SIZE))
                glue(glue(st, SUFFIX), _p)((uint8_t*)(addr + (e->addend&~1)), val);
            else
#endif
                glue(glue(st, SUFFIX), _raw)((uint8_t *)(intptr_t)(addr+addend), val);

            S2E_TRACE_MEMORY(addr, addr+addend, val, 1, 0);
        }
    } else {
        /* the page is not in the TLB : fill it */
        tlb_fill(addr, object_index << S2E_RAM_OBJECT_BITS,
                 1, mmu_idx, retaddr);
        goto redo;
    }
}

#endif /* !defined(SOFTMMU_CODE_ACCESS) */

#ifndef CONFIG_S2E
#undef S2E_RAM_OBJECT_BITS
#undef S2E_RAM_OBJECT_SIZE
#undef S2E_RAM_OBJECT_MASK
#endif
#undef S2E_FORK_AND_CONCRETIZE_ADDR
#undef S2E_FORK_AND_CONCRETIZE
#undef S2E_TRACE_MEMORY
#undef ADDR_MAX
#undef READ_ACCESS_TYPE
#undef SHIFT
#undef DATA_TYPE
#undef SUFFIX
#undef USUFFIX
#undef DATA_SIZE
#undef ADDR_READ
