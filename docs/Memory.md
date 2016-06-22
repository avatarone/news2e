Memory in S2E
=============

Translation from virtual to physical addresses happens in softmmu_template.h.
This file is instantiated for every memory access size (byte, short, long,
quad).  A memory access function looks up the mapping in the translation
lookaside buffer (TLB). The cache is direct mapped, i.e., every virtual address
maps to only one possible cache location. If a cache entry does not contain the
desired virtual address, the victim TLB is consulted. The victim TLB is a queue
of the most recently evicted cache entries. In case also the victim TLB does
not contain a mapping, the function **tlb_fill** is called. This function in
turn calls into target-architecture specific code which walks the page tables,
and loads the TLB with the correct mapping (or raises a CPU exception if the
page is not mapped).

The function called by **tlb_fill** needs to call back to
**S2EExecutionState_UpdateTlbEntry** to fill information about Klee
**MemoryObject**s in **env->s2etlb**. Once a mapping is found or created from
the page tables, the host address of the storage can be calculated. The address
is stored in form of an __addend__, which is the difference between host and
virtual guest address. By adding
**env->s2etlb[mmu_idx][page_idx][obj_idx].addend** to the virtual address, the
host address of the concrete store in KLEE is calculated. This address is then
passed to **lduw_p** et al (include/exec/cpu-all.h). These functions simply
load the data from memory, and swap bytes if necessary because of an endianness
boundary.


Because calls to the softmmu functions (**helper_le_lduw_mmu** et al) are
deemed expensive, especially if an entry in the TLB exists for the access, host
code generated from TCG optimizes those accesses. Fetching the TLB entry,
checking if it is valid, and actually loading the value are inlined into the
memory access (tcg/i386/tcg-target.c). As this is bad for S2E (we would also
need to add a call to check if the value to be accessed is concrete), S2E does
not support the fast path.  
