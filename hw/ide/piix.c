/*
 * QEMU IDE Emulation: PCI PIIX3/4 support.
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2006 Openedhand Ltd.
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
#include <hw/hw.h>
#include <hw/pc.h>
#include <hw/pci.h>
#include <hw/isa.h>
#include "block.h"
#include "block_int.h"
#include "sysemu.h"
#include "dma.h"

#include <hw/ide/pci.h>

static uint32_t bmdma_readb(void *opaque, uint32_t addr)
{
    BMDMAState *bm = opaque;
    uint32_t val;

    switch(addr & 3) {
    case 0:
        val = bm->cmd;
        break;
    case 2:
        val = bm->status;
        break;
    default:
        val = 0xff;
        break;
    }
#ifdef DEBUG_IDE
    printf("bmdma: readb 0x%02x : 0x%02x\n", addr, val);
#endif
    return val;
}

static void bmdma_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    BMDMAState *bm = opaque;
#ifdef DEBUG_IDE
    printf("bmdma: writeb 0x%02x : 0x%02x\n", addr, val);
#endif
    switch(addr & 3) {
    case 2:
        bm->status = (val & 0x60) | (bm->status & 1) | (bm->status & ~val & 0x06);
        break;
    }
}

static void bmdma_map(PCIDevice *pci_dev, int region_num,
                    pcibus_t addr, pcibus_t size, int type)
{
    PCIIDEState *d = DO_UPCAST(PCIIDEState, dev, pci_dev);
    int i;

    for(i = 0;i < 2; i++) {
        BMDMAState *bm = &d->bmdma[i];
        d->bus[i].bmdma = bm;
        bm->bus = d->bus+i;
        qemu_add_vm_change_state_handler(ide_dma_restart_cb, bm);

        register_ioport_write(addr, 1, 1, bmdma_cmd_writeb, bm);

        register_ioport_write(addr + 1, 3, 1, bmdma_writeb, bm);
        register_ioport_read(addr, 4, 1, bmdma_readb, bm);

        register_ioport_write(addr + 4, 4, 1, bmdma_addr_writeb, bm);
        register_ioport_read(addr + 4, 4, 1, bmdma_addr_readb, bm);
        register_ioport_write(addr + 4, 4, 2, bmdma_addr_writew, bm);
        register_ioport_read(addr + 4, 4, 2, bmdma_addr_readw, bm);
        register_ioport_write(addr + 4, 4, 4, bmdma_addr_writel, bm);
        register_ioport_read(addr + 4, 4, 4, bmdma_addr_readl, bm);
        addr += 8;
    }
}

static void piix3_reset(void *opaque)
{
    PCIIDEState *d = opaque;
    uint8_t *pci_conf = d->dev.config;
    int i;

    for (i = 0; i < 2; i++) {
        ide_bus_reset(&d->bus[i]);
        ide_dma_reset(&d->bmdma[i]);
    }

    pci_conf[0x04] = 0x00;
    pci_conf[0x05] = 0x00;
    pci_conf[0x06] = 0x80; /* FBC */
    pci_conf[0x07] = 0x02; // PCI_status_devsel_medium
    pci_conf[0x20] = 0x01; /* BMIBA: 20-23h */
}

static int pci_piix_ide_initfn(PCIIDEState *d)
{
    uint8_t *pci_conf = d->dev.config;

    pci_conf[0x09] = 0x80; // legacy ATA mode
    pci_config_set_class(pci_conf, PCI_CLASS_STORAGE_IDE);
    pci_conf[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_NORMAL; // header_type

    qemu_register_reset(piix3_reset, d);

    pci_register_bar(&d->dev, 4, 0x10, PCI_BASE_ADDRESS_SPACE_IO, bmdma_map);

    vmstate_register(0, &vmstate_ide_pci, d);

    ide_bus_new(&d->bus[0], &d->dev.qdev);
    ide_bus_new(&d->bus[1], &d->dev.qdev);
    ide_init_ioport(&d->bus[0], 0x1f0, 0x3f6);
    ide_init_ioport(&d->bus[1], 0x170, 0x376);

    ide_init2(&d->bus[0], NULL, NULL, isa_reserve_irq(14));
    ide_init2(&d->bus[1], NULL, NULL, isa_reserve_irq(15));
    return 0;
}

static int pci_piix3_ide_initfn(PCIDevice *dev)
{
    PCIIDEState *d = DO_UPCAST(PCIIDEState, dev, dev);

    pci_config_set_vendor_id(d->dev.config, PCI_VENDOR_ID_INTEL);
    pci_config_set_device_id(d->dev.config, PCI_DEVICE_ID_INTEL_82371SB_1);
    return pci_piix_ide_initfn(d);
}

static int pci_piix4_ide_initfn(PCIDevice *dev)
{
    PCIIDEState *d = DO_UPCAST(PCIIDEState, dev, dev);

    pci_config_set_vendor_id(d->dev.config, PCI_VENDOR_ID_INTEL);
    pci_config_set_device_id(d->dev.config, PCI_DEVICE_ID_INTEL_82371AB);
    return pci_piix_ide_initfn(d);
}

/* hd_table must contain 4 block drivers */
/* NOTE: for the PIIX3, the IRQs and IOports are hardcoded */
void pci_piix3_ide_init(PCIBus *bus, DriveInfo **hd_table, int devfn)
{
    PCIDevice *dev;

    dev = pci_create_simple(bus, devfn, "piix3-ide");
    pci_ide_create_devs(dev, hd_table);
}

/* hd_table must contain 4 block drivers */
/* NOTE: for the PIIX4, the IRQs and IOports are hardcoded */
void pci_piix4_ide_init(PCIBus *bus, DriveInfo **hd_table, int devfn)
{
    PCIDevice *dev;

    dev = pci_create_simple(bus, devfn, "piix4-ide");
    pci_ide_create_devs(dev, hd_table);
}

static PCIDeviceInfo piix_ide_info[] = {
    {
        .qdev.name    = "piix3-ide",
        .qdev.size    = sizeof(PCIIDEState),
        .qdev.no_user = 1,
        .init         = pci_piix3_ide_initfn,
    },{
        .qdev.name    = "piix4-ide",
        .qdev.size    = sizeof(PCIIDEState),
        .qdev.no_user = 1,
        .init         = pci_piix4_ide_initfn,
    },{
        /* end of list */
    }
};

static void piix_ide_register(void)
{
    pci_qdev_register_many(piix_ide_info);
}
device_init(piix_ide_register);
