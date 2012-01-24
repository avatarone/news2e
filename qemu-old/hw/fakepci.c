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

/**
 *  This module enables fake pci device support for native QEMU.
 *  This allows quick creation of snapshots in native mode, ready to be run in S2E.
 */


#include <stdio.h>
#include "qemu-common.h"
#include "hw/hw.h"
#include "hw/pci.h"

#ifndef CONFIG_S2E


static fake_pci_t *s_fake_pci=NULL;
PCIDevice *fake_pci_dev;


/////////////////////////////////////////////////////////////////////
/* Dummy I/O functions for symbolic devices. Unused for now. */
static void symbhw_write8(void *opaque, uint32_t address, uint32_t data) {
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << " 0x" << data << std::endl;
}

static void symbhw_write16(void *opaque, uint32_t address, uint32_t data) {
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << " 0x" << data << std::endl;
}

static void symbhw_write32(void *opaque, uint32_t address, uint32_t data) {
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << " 0x" << data << std::endl;
}

/* These will never be called */
static uint32_t symbhw_read8(void *opaque, uint32_t address)
{
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << std::endl;
    return 0;
}

static uint32_t symbhw_read16(void *opaque, uint32_t address)
{
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << std::endl;
    return 0;
}

static uint32_t symbhw_read32(void *opaque, uint32_t address)
{
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << std::endl;
    return 0;
}

static void symbhw_mmio_writeb(void *opaque, target_phys_addr_t addr, uint32_t val)
{
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << " 0x" << val << std::endl;
}

static void symbhw_mmio_writew(void *opaque, target_phys_addr_t addr, uint32_t val)
{
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << " 0x" << val << std::endl;
}

static void symbhw_mmio_writel(void *opaque, target_phys_addr_t addr, uint32_t val)
{
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << " 0x" << val << std::endl;
}

static uint32_t symbhw_mmio_readb(void *opaque, target_phys_addr_t addr)
{
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << std::endl;
    return 0;
}

static uint32_t symbhw_mmio_readw(void *opaque, target_phys_addr_t addr)
{
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << std::endl;
    return 0;
}

static uint32_t symbhw_mmio_readl(void *opaque, target_phys_addr_t addr)
{
//    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << std::endl;
    return 0;
}

static CPUReadMemoryFunc * const symbhw_mmio_read[3] = {
    symbhw_mmio_readb,
    symbhw_mmio_readw,
    symbhw_mmio_readl,
};

static CPUWriteMemoryFunc * const symbhw_mmio_write[3] = {
    symbhw_mmio_writeb,
    symbhw_mmio_writew,
    symbhw_mmio_writel,
};


typedef struct _PCIFakeState {
    PCIDevice dev;
}PCIFakeState;

static void pci_fake_map(PCIDevice *pci_dev, int region_num,
                       pcibus_t addr, pcibus_t size, int type)
{
    PCIFakeState *s = DO_UPCAST(PCIFakeState, dev, pci_dev);

    if (type & PCI_BASE_ADDRESS_SPACE_IO) {
        register_ioport_write(addr, size, 1, symbhw_write8, s);
        register_ioport_read(addr, size, 1, symbhw_read8, s);

        register_ioport_write(addr, size, 2, symbhw_write16, s);
        register_ioport_read(addr, size, 2, symbhw_read16, s);

        register_ioport_write(addr, size, 4, symbhw_write32, s);
        register_ioport_read(addr, size, 4, symbhw_read32, s);
    }

    if (type & PCI_BASE_ADDRESS_SPACE_MEMORY) {
        cpu_register_physical_memory(addr, size, s_fake_pci->mmioidx);
    }
}


static int pci_fake_init(PCIDevice *pci_dev)
{
    PCIFakeState *d = DO_UPCAST(PCIFakeState, dev, pci_dev);
    uint8_t *pci_conf;
    int i;

    pci_conf = d->dev.config;
    pci_config_set_vendor_id(pci_conf, s_fake_pci->fake_pci_vendor_id);
    pci_config_set_device_id(pci_conf, s_fake_pci->fake_pci_device_id);
    pci_config_set_class(pci_conf, s_fake_pci->fake_pci_class_code);
    pci_conf[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_NORMAL; // header_type
    pci_conf[0x3d] = 1; // interrupt pin 0

    for(i=0; i<s_fake_pci->fake_pci_num_resources; ++i) {
        int type = s_fake_pci->fake_pci_resources[i].type;

        pci_register_bar(&d->dev, i, s_fake_pci->fake_pci_resources[i].size,
                                   type, pci_fake_map);

        ++i;
    }

    /* I/O handler for memory-mapped I/O */
    s_fake_pci->mmioidx =
    cpu_register_io_memory(symbhw_mmio_read, symbhw_mmio_write, d);


    return 0;
}


static int pci_fake_exit(PCIDevice *pci_dev)
{
    return 0;
}

static  VMStateDescription vmstate_pci_fake = {
    .name = "fake",
    .version_id = 3,
    .minimum_version_id = 3,
    .minimum_version_id_old = 3,
    .fields      = (VMStateField []) {
        VMSTATE_PCI_DEVICE(dev, PCIFakeState),
        VMSTATE_END_OF_LIST()
    }
};

static PCIDeviceInfo fake_info = {
    .qdev.name  = "fake", //Should be replaced!
    .qdev.size  = sizeof(PCIFakeState),
    .qdev.vmsd  = &vmstate_pci_fake,
    .init       = pci_fake_init,
    .exit       = pci_fake_exit,
    .qdev.props = (Property[]) {
        DEFINE_PROP_END_OF_LIST(),
    }
};

void fake_register_devices(fake_pci_t *fake);
void fake_activate_devices(struct PCIBus *bus);

void fake_register_devices(fake_pci_t *fake)
{
    s_fake_pci = fake;
    vmstate_pci_fake.name = fake->fake_pci_name;
    fake_info.qdev.name = fake->fake_pci_name;
    pci_qdev_register(&fake_info);
}

void fake_activate_devices(struct PCIBus *bus)
{
    if(s_fake_pci->fake_pci_name)
        pci_create_simple(bus, -1, s_fake_pci->fake_pci_name);
}


#endif
