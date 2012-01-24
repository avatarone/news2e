/*
 * ide bus support for qdev.
 *
 * Copyright (c) 2009 Gerd Hoffmann <kraxel@redhat.com>
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
#include <hw/hw.h>
#include "sysemu.h"
#include "dma.h"

#include <hw/ide/internal.h>

/* --------------------------------- */

static struct BusInfo ide_bus_info = {
    .name  = "IDE",
    .size  = sizeof(IDEBus),
};

void ide_bus_new(IDEBus *idebus, DeviceState *dev)
{
    qbus_create_inplace(&idebus->qbus, &ide_bus_info, dev, NULL);
}

static int ide_qdev_init(DeviceState *qdev, DeviceInfo *base)
{
    IDEDevice *dev = DO_UPCAST(IDEDevice, qdev, qdev);
    IDEDeviceInfo *info = DO_UPCAST(IDEDeviceInfo, qdev, base);
    IDEBus *bus = DO_UPCAST(IDEBus, qbus, qdev->parent_bus);

    if (!dev->dinfo) {
        fprintf(stderr, "%s: no drive specified\n", qdev->info->name);
        goto err;
    }
    if (dev->unit == -1) {
        dev->unit = bus->master ? 1 : 0;
    }
    switch (dev->unit) {
    case 0:
        if (bus->master) {
            fprintf(stderr, "ide: tried to assign master twice\n");
            goto err;
        }
        bus->master = dev;
        break;
    case 1:
        if (bus->slave) {
            fprintf(stderr, "ide: tried to assign slave twice\n");
            goto err;
        }
        bus->slave = dev;
        break;
    default:
        goto err;
    }
    return info->init(dev);

err:
    return -1;
}

static void ide_qdev_register(IDEDeviceInfo *info)
{
    info->qdev.init = ide_qdev_init;
    info->qdev.bus_info = &ide_bus_info;
    qdev_register(&info->qdev);
}

IDEDevice *ide_create_drive(IDEBus *bus, int unit, DriveInfo *drive)
{
    DeviceState *dev;

    dev = qdev_create(&bus->qbus, "ide-drive");
    qdev_prop_set_uint32(dev, "unit", unit);
    qdev_prop_set_drive(dev, "drive", drive);
    if (qdev_init(dev) < 0)
        return NULL;
    return DO_UPCAST(IDEDevice, qdev, dev);
}

/* --------------------------------- */

typedef struct IDEDrive {
    IDEDevice dev;
} IDEDrive;

static int ide_drive_initfn(IDEDevice *dev)
{
    IDEBus *bus = DO_UPCAST(IDEBus, qbus, dev->qdev.parent_bus);
    ide_init_drive(bus->ifs + dev->unit, dev->dinfo, dev->version);
    return 0;
}

static IDEDeviceInfo ide_drive_info = {
    .qdev.name  = "ide-drive",
    .qdev.size  = sizeof(IDEDrive),
    .init       = ide_drive_initfn,
    .qdev.props = (Property[]) {
        DEFINE_PROP_UINT32("unit", IDEDrive, dev.unit, -1),
        DEFINE_PROP_DRIVE("drive", IDEDrive, dev.dinfo),
        DEFINE_PROP_STRING("ver",  IDEDrive, dev.version),
        DEFINE_PROP_END_OF_LIST(),
    }
};

static void ide_drive_register(void)
{
    ide_qdev_register(&ide_drive_info);
}
device_init(ide_drive_register);
