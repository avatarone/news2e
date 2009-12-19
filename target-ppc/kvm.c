/*
 * PowerPC implementation of KVM hooks
 *
 * Copyright IBM Corp. 2007
 *
 * Authors:
 *  Jerone Young <jyoung5@us.ibm.com>
 *  Christian Ehrhardt <ehrhardt@linux.vnet.ibm.com>
 *  Hollis Blanchard <hollisb@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/kvm.h>

#include "qemu-common.h"
#include "qemu-timer.h"
#include "sysemu.h"
#include "kvm.h"
#include "kvm_ppc.h"
#include "cpu.h"
#include "device_tree.h"

//#define DEBUG_KVM

#ifdef DEBUG_KVM
#define dprintf(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define dprintf(fmt, ...) \
    do { } while (0)
#endif

int kvm_arch_init(KVMState *s, int smp_cpus)
{
    return 0;
}

int kvm_arch_init_vcpu(CPUState *cenv)
{
    int ret = 0;
    struct kvm_sregs sregs;

    sregs.pvr = cenv->spr[SPR_PVR];
    ret = kvm_vcpu_ioctl(cenv, KVM_SET_SREGS, &sregs);

    return ret;
}

void kvm_arch_reset_vcpu(CPUState *env)
{
}

int kvm_arch_put_registers(CPUState *env)
{
    struct kvm_regs regs;
    int ret;
    int i;

    ret = kvm_vcpu_ioctl(env, KVM_GET_REGS, &regs);
    if (ret < 0)
        return ret;

    regs.ctr = env->ctr;
    regs.lr  = env->lr;
    regs.xer = env->xer;
    regs.msr = env->msr;
    regs.pc = env->nip;

    regs.srr0 = env->spr[SPR_SRR0];
    regs.srr1 = env->spr[SPR_SRR1];

    regs.sprg0 = env->spr[SPR_SPRG0];
    regs.sprg1 = env->spr[SPR_SPRG1];
    regs.sprg2 = env->spr[SPR_SPRG2];
    regs.sprg3 = env->spr[SPR_SPRG3];
    regs.sprg4 = env->spr[SPR_SPRG4];
    regs.sprg5 = env->spr[SPR_SPRG5];
    regs.sprg6 = env->spr[SPR_SPRG6];
    regs.sprg7 = env->spr[SPR_SPRG7];

    for (i = 0;i < 32; i++)
        regs.gpr[i] = env->gpr[i];

    ret = kvm_vcpu_ioctl(env, KVM_SET_REGS, &regs);
    if (ret < 0)
        return ret;

    return ret;
}

int kvm_arch_get_registers(CPUState *env)
{
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    uint32_t i, ret;

    ret = kvm_vcpu_ioctl(env, KVM_GET_REGS, &regs);
    if (ret < 0)
        return ret;

    ret = kvm_vcpu_ioctl(env, KVM_GET_SREGS, &sregs);
    if (ret < 0)
        return ret;

    env->ctr = regs.ctr;
    env->lr = regs.lr;
    env->xer = regs.xer;
    env->msr = regs.msr;
    env->nip = regs.pc;

    env->spr[SPR_SRR0] = regs.srr0;
    env->spr[SPR_SRR1] = regs.srr1;

    env->spr[SPR_SPRG0] = regs.sprg0;
    env->spr[SPR_SPRG1] = regs.sprg1;
    env->spr[SPR_SPRG2] = regs.sprg2;
    env->spr[SPR_SPRG3] = regs.sprg3;
    env->spr[SPR_SPRG4] = regs.sprg4;
    env->spr[SPR_SPRG5] = regs.sprg5;
    env->spr[SPR_SPRG6] = regs.sprg6;
    env->spr[SPR_SPRG7] = regs.sprg7;

    for (i = 0;i < 32; i++)
        env->gpr[i] = regs.gpr[i];

#ifdef KVM_CAP_PPC_SEGSTATE
    if (kvm_check_extension(env->kvm_state, KVM_CAP_PPC_SEGSTATE)) {
        env->sdr1 = sregs.u.s.sdr1;

        /* Sync SLB */
#ifdef TARGET_PPC64
        for (i = 0; i < 64; i++) {
            ppc_store_slb(env, sregs.u.s.ppc64.slb[i].slbe,
                               sregs.u.s.ppc64.slb[i].slbv);
        }
#endif

        /* Sync SRs */
        for (i = 0; i < 16; i++) {
            env->sr[i] = sregs.u.s.ppc32.sr[i];
        }

        /* Sync BATs */
        for (i = 0; i < 8; i++) {
            env->DBAT[0][i] = sregs.u.s.ppc32.dbat[i] & 0xffffffff;
            env->DBAT[1][i] = sregs.u.s.ppc32.dbat[i] >> 32;
            env->IBAT[0][i] = sregs.u.s.ppc32.ibat[i] & 0xffffffff;
            env->IBAT[1][i] = sregs.u.s.ppc32.ibat[i] >> 32;
        }
    }
#endif

    return 0;
}

#if defined(TARGET_PPCEMB)
#define PPC_INPUT_INT PPC40x_INPUT_INT
#elif defined(TARGET_PPC64)
#define PPC_INPUT_INT PPC970_INPUT_INT
#else
#define PPC_INPUT_INT PPC6xx_INPUT_INT
#endif

int kvm_arch_pre_run(CPUState *env, struct kvm_run *run)
{
    int r;
    unsigned irq;

    /* PowerPC Qemu tracks the various core input pins (interrupt, critical
     * interrupt, reset, etc) in PPC-specific env->irq_input_state. */
    if (run->ready_for_interrupt_injection &&
        (env->interrupt_request & CPU_INTERRUPT_HARD) &&
        (env->irq_input_state & (1<<PPC_INPUT_INT)))
    {
        /* For now KVM disregards the 'irq' argument. However, in the
         * future KVM could cache it in-kernel to avoid a heavyweight exit
         * when reading the UIC.
         */
        irq = -1U;

        dprintf("injected interrupt %d\n", irq);
        r = kvm_vcpu_ioctl(env, KVM_INTERRUPT, &irq);
        if (r < 0)
            printf("cpu %d fail inject %x\n", env->cpu_index, irq);
    }

    /* We don't know if there are more interrupts pending after this. However,
     * the guest will return to userspace in the course of handling this one
     * anyways, so we will get a chance to deliver the rest. */
    return 0;
}

int kvm_arch_post_run(CPUState *env, struct kvm_run *run)
{
    return 0;
}

static int kvmppc_handle_halt(CPUState *env)
{
    if (!(env->interrupt_request & CPU_INTERRUPT_HARD) && (msr_ee)) {
        env->halted = 1;
        env->exception_index = EXCP_HLT;
    }

    return 1;
}

/* map dcr access to existing qemu dcr emulation */
static int kvmppc_handle_dcr_read(CPUState *env, uint32_t dcrn, uint32_t *data)
{
    if (ppc_dcr_read(env->dcr_env, dcrn, data) < 0)
        fprintf(stderr, "Read to unhandled DCR (0x%x)\n", dcrn);

    return 1;
}

static int kvmppc_handle_dcr_write(CPUState *env, uint32_t dcrn, uint32_t data)
{
    if (ppc_dcr_write(env->dcr_env, dcrn, data) < 0)
        fprintf(stderr, "Write to unhandled DCR (0x%x)\n", dcrn);

    return 1;
}

int kvm_arch_handle_exit(CPUState *env, struct kvm_run *run)
{
    int ret = 0;

    switch (run->exit_reason) {
    case KVM_EXIT_DCR:
        if (run->dcr.is_write) {
            dprintf("handle dcr write\n");
            ret = kvmppc_handle_dcr_write(env, run->dcr.dcrn, run->dcr.data);
        } else {
            dprintf("handle dcr read\n");
            ret = kvmppc_handle_dcr_read(env, run->dcr.dcrn, &run->dcr.data);
        }
        break;
    case KVM_EXIT_HLT:
        dprintf("handle halt\n");
        ret = kvmppc_handle_halt(env);
        break;
    }

    return ret;
}

