/*
 *  Samsung exynos4210 SoC emulation
 *
 *  Copyright (c) 2011 Samsung Electronics Co., Ltd. All rights reserved.
 *    Maksim Kozlov <m.kozlov@samsung.com>
 *    Evgeny Voevodin <e.voevodin@samsung.com>
 *    Igor Mitsyanko <i.mitsyanko@samsung.com>
 *
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */


#ifndef EXYNOS4210_H_
#define EXYNOS4210_H_

#include "qemu-common.h"
#include "memory.h"

#define EXYNOS4210_NCPUS                    2

/*
 * exynos4210 IRQ subsystem stub definitions.
 */
#define EXYNOS4210_IRQ_GATE_NINPUTS 8

#define EXYNOS4210_MAX_INT_COMBINER_OUT_IRQ  64
#define EXYNOS4210_MAX_EXT_COMBINER_OUT_IRQ  16
#define EXYNOS4210_MAX_INT_COMBINER_IN_IRQ   \
    (EXYNOS4210_MAX_INT_COMBINER_OUT_IRQ * 8)
#define EXYNOS4210_MAX_EXT_COMBINER_IN_IRQ   \
    (EXYNOS4210_MAX_EXT_COMBINER_OUT_IRQ * 8)

#define EXYNOS4210_COMBINER_GET_IRQ_NUM(grp, bit)  ((grp)*8 + (bit))
#define EXYNOS4210_COMBINER_GET_GRP_NUM(irq)       ((irq) / 8)
#define EXYNOS4210_COMBINER_GET_BIT_NUM(irq) \
    ((irq) - 8 * EXYNOS4210_COMBINER_GET_GRP_NUM(irq))

/* IRQs number for external and internal GIC */
#define EXYNOS4210_EXT_GIC_NIRQ     (160-32)
#define EXYNOS4210_INT_GIC_NIRQ     64

typedef struct Exynos4210Irq {
    qemu_irq int_combiner_irq[EXYNOS4210_MAX_INT_COMBINER_IN_IRQ];
    qemu_irq ext_combiner_irq[EXYNOS4210_MAX_EXT_COMBINER_IN_IRQ];
    qemu_irq int_gic_irq[EXYNOS4210_INT_GIC_NIRQ];
    qemu_irq ext_gic_irq[EXYNOS4210_EXT_GIC_NIRQ];
    qemu_irq board_irqs[EXYNOS4210_MAX_INT_COMBINER_IN_IRQ];
} Exynos4210Irq;

/* Initialize exynos4210 IRQ subsystem stub */
qemu_irq *exynos4210_init_irq(Exynos4210Irq *env);

/* Initialize board IRQs.
 * These IRQs contain splitted Int/External Combiner and External Gic IRQs */
void exynos4210_init_board_irqs(Exynos4210Irq *s);

/* Get IRQ number from exynos4210 IRQ subsystem stub.
 * To identify IRQ source use internal combiner group and bit number
 *  grp - group number
 *  bit - bit number inside group */
uint32_t exynos4210_get_irq(uint32_t grp, uint32_t bit);

/*
 * Get Combiner input GPIO into irqs structure
 */
void exynos4210_combiner_get_gpioin(Exynos4210Irq *irqs, DeviceState *dev,
        int ext);

#endif /* EXYNOS4210_H_ */
