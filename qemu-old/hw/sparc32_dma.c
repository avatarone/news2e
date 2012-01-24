/*
 * QEMU Sparc32 DMA controller emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "hw.h"
#include "sparc32_dma.h"
#include "sun4m.h"
#include "sysbus.h"

/* debug DMA */
//#define DEBUG_DMA

/*
 * This is the DMA controller part of chip STP2000 (Master I/O), also
 * produced as NCR89C100. See
 * http://www.ibiblio.org/pub/historic-linux/early-ports/Sparc/NCR/NCR89C100.txt
 * and
 * http://www.ibiblio.org/pub/historic-linux/early-ports/Sparc/NCR/DMA2.txt
 */

#ifdef DEBUG_DMA
#define DPRINTF(fmt, ...)                               \
    do { printf("DMA: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...)
#endif

#define DMA_REGS 4
#define DMA_SIZE (4 * sizeof(uint32_t))
/* We need the mask, because one instance of the device is not page
   aligned (ledma, start address 0x0010) */
#define DMA_MASK (DMA_SIZE - 1)

#define DMA_VER 0xa0000000
#define DMA_INTR 1
#define DMA_INTREN 0x10
#define DMA_WRITE_MEM 0x100
#define DMA_LOADED 0x04000000
#define DMA_DRAIN_FIFO 0x40
#define DMA_RESET 0x80

typedef struct DMAState DMAState;

struct DMAState {
    SysBusDevice busdev;
    uint32_t dmaregs[DMA_REGS];
    qemu_irq irq;
    void *iommu;
    qemu_irq dev_reset;
};

/* Note: on sparc, the lance 16 bit bus is swapped */
void ledma_memory_read(void *opaque, target_phys_addr_t addr,
                       uint8_t *buf, int len, int do_bswap)
{
    DMAState *s = opaque;
    int i;

    DPRINTF("DMA write, direction: %c, addr 0x%8.8x\n",
            s->dmaregs[0] & DMA_WRITE_MEM ? 'w': 'r', s->dmaregs[1]);
    addr |= s->dmaregs[3];
    if (do_bswap) {
        sparc_iommu_memory_read(s->iommu, addr, buf, len);
    } else {
        addr &= ~1;
        len &= ~1;
        sparc_iommu_memory_read(s->iommu, addr, buf, len);
        for(i = 0; i < len; i += 2) {
            bswap16s((uint16_t *)(buf + i));
        }
    }
}

void ledma_memory_write(void *opaque, target_phys_addr_t addr,
                        uint8_t *buf, int len, int do_bswap)
{
    DMAState *s = opaque;
    int l, i;
    uint16_t tmp_buf[32];

    DPRINTF("DMA read, direction: %c, addr 0x%8.8x\n",
            s->dmaregs[0] & DMA_WRITE_MEM ? 'w': 'r', s->dmaregs[1]);
    addr |= s->dmaregs[3];
    if (do_bswap) {
        sparc_iommu_memory_write(s->iommu, addr, buf, len);
    } else {
        addr &= ~1;
        len &= ~1;
        while (len > 0) {
            l = len;
            if (l > sizeof(tmp_buf))
                l = sizeof(tmp_buf);
            for(i = 0; i < l; i += 2) {
                tmp_buf[i >> 1] = bswap16(*(uint16_t *)(buf + i));
            }
            sparc_iommu_memory_write(s->iommu, addr, (uint8_t *)tmp_buf, l);
            len -= l;
            buf += l;
            addr += l;
        }
    }
}

static void dma_set_irq(void *opaque, int irq, int level)
{
    DMAState *s = opaque;
    if (level) {
        DPRINTF("Raise IRQ\n");
        s->dmaregs[0] |= DMA_INTR;
        qemu_irq_raise(s->irq);
    } else {
        s->dmaregs[0] &= ~DMA_INTR;
        DPRINTF("Lower IRQ\n");
        qemu_irq_lower(s->irq);
    }
}

void espdma_memory_read(void *opaque, uint8_t *buf, int len)
{
    DMAState *s = opaque;

    DPRINTF("DMA read, direction: %c, addr 0x%8.8x\n",
            s->dmaregs[0] & DMA_WRITE_MEM ? 'w': 'r', s->dmaregs[1]);
    sparc_iommu_memory_read(s->iommu, s->dmaregs[1], buf, len);
    s->dmaregs[0] |= DMA_INTR;
    s->dmaregs[1] += len;
}

void espdma_memory_write(void *opaque, uint8_t *buf, int len)
{
    DMAState *s = opaque;

    DPRINTF("DMA write, direction: %c, addr 0x%8.8x\n",
            s->dmaregs[0] & DMA_WRITE_MEM ? 'w': 'r', s->dmaregs[1]);
    sparc_iommu_memory_write(s->iommu, s->dmaregs[1], buf, len);
    s->dmaregs[0] |= DMA_INTR;
    s->dmaregs[1] += len;
}

static uint32_t dma_mem_readl(void *opaque, target_phys_addr_t addr)
{
    DMAState *s = opaque;
    uint32_t saddr;

    saddr = (addr & DMA_MASK) >> 2;
    DPRINTF("read dmareg " TARGET_FMT_plx ": 0x%8.8x\n", addr,
            s->dmaregs[saddr]);

    return s->dmaregs[saddr];
}

static void dma_mem_writel(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    DMAState *s = opaque;
    uint32_t saddr;

    saddr = (addr & DMA_MASK) >> 2;
    DPRINTF("write dmareg " TARGET_FMT_plx ": 0x%8.8x -> 0x%8.8x\n", addr,
            s->dmaregs[saddr], val);
    switch (saddr) {
    case 0:
        if (!(val & DMA_INTREN)) {
            DPRINTF("Lower IRQ\n");
            qemu_irq_lower(s->irq);
        }
        if (val & DMA_RESET) {
            qemu_irq_raise(s->dev_reset);
            qemu_irq_lower(s->dev_reset);
        } else if (val & DMA_DRAIN_FIFO) {
            val &= ~DMA_DRAIN_FIFO;
        } else if (val == 0)
            val = DMA_DRAIN_FIFO;
        val &= 0x0fffffff;
        val |= DMA_VER;
        break;
    case 1:
        s->dmaregs[0] |= DMA_LOADED;
        break;
    default:
        break;
    }
    s->dmaregs[saddr] = val;
}

static CPUReadMemoryFunc * const dma_mem_read[3] = {
    NULL,
    NULL,
    dma_mem_readl,
};

static CPUWriteMemoryFunc * const dma_mem_write[3] = {
    NULL,
    NULL,
    dma_mem_writel,
};

static void dma_reset(DeviceState *d)
{
    DMAState *s = container_of(d, DMAState, busdev.qdev);

    memset(s->dmaregs, 0, DMA_SIZE);
    s->dmaregs[0] = DMA_VER;
}

static const VMStateDescription vmstate_dma = {
    .name ="sparc32_dma",
    .version_id = 2,
    .minimum_version_id = 2,
    .minimum_version_id_old = 2,
    .fields      = (VMStateField []) {
        VMSTATE_UINT32_ARRAY(dmaregs, DMAState, DMA_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static int sparc32_dma_init1(SysBusDevice *dev)
{
    DMAState *s = FROM_SYSBUS(DMAState, dev);
    int dma_io_memory;

    sysbus_init_irq(dev, &s->irq);

    dma_io_memory = cpu_register_io_memory(dma_mem_read, dma_mem_write, s);
    sysbus_init_mmio(dev, DMA_SIZE, dma_io_memory);

    qdev_init_gpio_in(&dev->qdev, dma_set_irq, 1);
    qdev_init_gpio_out(&dev->qdev, &s->dev_reset, 1);

    return 0;
}

static SysBusDeviceInfo sparc32_dma_info = {
    .init = sparc32_dma_init1,
    .qdev.name  = "sparc32_dma",
    .qdev.size  = sizeof(DMAState),
    .qdev.vmsd  = &vmstate_dma,
    .qdev.reset = dma_reset,
    .qdev.props = (Property[]) {
        DEFINE_PROP_PTR("iommu_opaque", DMAState, iommu),
        DEFINE_PROP_END_OF_LIST(),
    }
};

static void sparc32_dma_register_devices(void)
{
    sysbus_register_withprop(&sparc32_dma_info);
}

device_init(sparc32_dma_register_devices)
