/*
 * QEMU Grackle PCI host (heathrow OldWorld PowerMac)
 *
 * Copyright (c) 2006-2007 Fabrice Bellard
 * Copyright (c) 2007 Jocelyn Mayer
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

#include "sysbus.h"
#include "ppc_mac.h"
#include "pci.h"
#include "pci_host.h"

/* debug Grackle */
//#define DEBUG_GRACKLE

#ifdef DEBUG_GRACKLE
#define GRACKLE_DPRINTF(fmt, ...)                               \
    do { printf("GRACKLE: " fmt , ## __VA_ARGS__); } while (0)
#else
#define GRACKLE_DPRINTF(fmt, ...)
#endif

typedef struct GrackleState {
    SysBusDevice busdev;
    PCIHostState host_state;
} GrackleState;

/* Don't know if this matches real hardware, but it agrees with OHW.  */
static int pci_grackle_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return (irq_num + (pci_dev->devfn >> 3)) & 3;
}

static void pci_grackle_set_irq(void *opaque, int irq_num, int level)
{
    qemu_irq *pic = opaque;

    GRACKLE_DPRINTF("set_irq num %d level %d\n", irq_num, level);
    qemu_set_irq(pic[irq_num + 0x15], level);
}

static void pci_grackle_save(QEMUFile* f, void *opaque)
{
    PCIDevice *d = opaque;

    pci_device_save(d, f);
}

static int pci_grackle_load(QEMUFile* f, void *opaque, int version_id)
{
    PCIDevice *d = opaque;

    if (version_id != 1)
        return -EINVAL;

    return pci_device_load(d, f);
}

static void pci_grackle_reset(void *opaque)
{
}

PCIBus *pci_grackle_init(uint32_t base, qemu_irq *pic)
{
    DeviceState *dev;
    SysBusDevice *s;
    GrackleState *d;

    dev = qdev_create(NULL, "grackle");
    qdev_init_nofail(dev);
    s = sysbus_from_qdev(dev);
    d = FROM_SYSBUS(GrackleState, s);
    d->host_state.bus = pci_register_bus(&d->busdev.qdev, "pci",
                                         pci_grackle_set_irq,
                                         pci_grackle_map_irq,
                                         pic, 0, 4);

    pci_create_simple(d->host_state.bus, 0, "grackle");

    sysbus_mmio_map(s, 0, base);
    sysbus_mmio_map(s, 1, base + 0x00200000);

    return d->host_state.bus;
}

static int pci_grackle_init_device(SysBusDevice *dev)
{
    GrackleState *s;
    int pci_mem_config, pci_mem_data;

    s = FROM_SYSBUS(GrackleState, dev);

    pci_mem_config = pci_host_conf_register_mmio(&s->host_state);
    pci_mem_data = pci_host_data_register_mmio(&s->host_state);
    sysbus_init_mmio(dev, 0x1000, pci_mem_config);
    sysbus_init_mmio(dev, 0x1000, pci_mem_data);

    register_savevm("grackle", 0, 1, pci_grackle_save, pci_grackle_load,
                    &s->host_state);
    qemu_register_reset(pci_grackle_reset, &s->host_state);
    return 0;
}

static int pci_dec_21154_init_device(SysBusDevice *dev)
{
    GrackleState *s;
    int pci_mem_config, pci_mem_data;

    s = FROM_SYSBUS(GrackleState, dev);

    pci_mem_config = pci_host_conf_register_mmio(&s->host_state);
    pci_mem_data = pci_host_data_register_mmio(&s->host_state);
    sysbus_init_mmio(dev, 0x1000, pci_mem_config);
    sysbus_init_mmio(dev, 0x1000, pci_mem_data);
    return 0;
}

static int grackle_pci_host_init(PCIDevice *d)
{
    pci_config_set_vendor_id(d->config, PCI_VENDOR_ID_MOTOROLA);
    pci_config_set_device_id(d->config, PCI_DEVICE_ID_MOTOROLA_MPC106);
    d->config[0x08] = 0x00; // revision
    d->config[0x09] = 0x01;
    pci_config_set_class(d->config, PCI_CLASS_BRIDGE_HOST);
    d->config[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_NORMAL; // header_type
    return 0;
}

static int dec_21154_pci_host_init(PCIDevice *d)
{
    /* PCI2PCI bridge same values as PearPC - check this */
    pci_config_set_vendor_id(d->config, PCI_VENDOR_ID_DEC);
    pci_config_set_device_id(d->config, PCI_DEVICE_ID_DEC_21154);
    d->config[0x08] = 0x02; // revision
    pci_config_set_class(d->config, PCI_CLASS_BRIDGE_PCI);
    d->config[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_BRIDGE; // header_type

    d->config[0x18] = 0x0;  // primary_bus
    d->config[0x19] = 0x1;  // secondary_bus
    d->config[0x1a] = 0x1;  // subordinate_bus
    d->config[0x1c] = 0x10; // io_base
    d->config[0x1d] = 0x20; // io_limit

    d->config[0x20] = 0x80; // memory_base
    d->config[0x21] = 0x80;
    d->config[0x22] = 0x90; // memory_limit
    d->config[0x23] = 0x80;

    d->config[0x24] = 0x00; // prefetchable_memory_base
    d->config[0x25] = 0x84;
    d->config[0x26] = 0x00; // prefetchable_memory_limit
    d->config[0x27] = 0x85;
    return 0;
}

static PCIDeviceInfo grackle_pci_host_info = {
    .qdev.name = "grackle",
    .qdev.size = sizeof(PCIDevice),
    .init      = grackle_pci_host_init,
};

static PCIDeviceInfo dec_21154_pci_host_info = {
    .qdev.name = "dec-21154",
    .qdev.size = sizeof(PCIDevice),
    .init      = dec_21154_pci_host_init,
};

static void grackle_register_devices(void)
{
    sysbus_register_dev("grackle", sizeof(GrackleState),
                        pci_grackle_init_device);
    pci_qdev_register(&grackle_pci_host_info);
    sysbus_register_dev("dec-21154", sizeof(GrackleState),
                        pci_dec_21154_init_device);
    pci_qdev_register(&dec_21154_pci_host_info);
}

device_init(grackle_register_devices)
