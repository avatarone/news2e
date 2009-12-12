#include "sysemu.h"
#include "net.h"
#include "qdev.h"

void *qdev_get_prop_ptr(DeviceState *dev, Property *prop)
{
    void *ptr = dev;
    ptr += prop->offset;
    return ptr;
}

/* --- 8bit integer --- */

static int parse_uint8(DeviceState *dev, Property *prop, const char *str)
{
    uint8_t *ptr = qdev_get_prop_ptr(dev, prop);
    const char *fmt;

    /* accept both hex and decimal */
    fmt = strncasecmp(str, "0x",2) == 0 ? "%" PRIx8 : "%" PRIu8;
    if (sscanf(str, fmt, ptr) != 1)
        return -1;
    return 0;
}

static int print_uint8(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    uint8_t *ptr = qdev_get_prop_ptr(dev, prop);
    return snprintf(dest, len, "%" PRIu8, *ptr);
}

PropertyInfo qdev_prop_uint8 = {
    .name  = "uint8",
    .type  = PROP_TYPE_UINT8,
    .size  = sizeof(uint8_t),
    .parse = parse_uint8,
    .print = print_uint8,
};

/* --- 16bit integer --- */

static int parse_uint16(DeviceState *dev, Property *prop, const char *str)
{
    uint16_t *ptr = qdev_get_prop_ptr(dev, prop);
    const char *fmt;

    /* accept both hex and decimal */
    fmt = strncasecmp(str, "0x",2) == 0 ? "%" PRIx16 : "%" PRIu16;
    if (sscanf(str, fmt, ptr) != 1)
        return -1;
    return 0;
}

static int print_uint16(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    uint16_t *ptr = qdev_get_prop_ptr(dev, prop);
    return snprintf(dest, len, "%" PRIu16, *ptr);
}

PropertyInfo qdev_prop_uint16 = {
    .name  = "uint16",
    .type  = PROP_TYPE_UINT16,
    .size  = sizeof(uint16_t),
    .parse = parse_uint16,
    .print = print_uint16,
};

/* --- 32bit integer --- */

static int parse_uint32(DeviceState *dev, Property *prop, const char *str)
{
    uint32_t *ptr = qdev_get_prop_ptr(dev, prop);
    const char *fmt;

    /* accept both hex and decimal */
    fmt = strncasecmp(str, "0x",2) == 0 ? "%" PRIx32 : "%" PRIu32;
    if (sscanf(str, fmt, ptr) != 1)
        return -1;
    return 0;
}

static int print_uint32(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    uint32_t *ptr = qdev_get_prop_ptr(dev, prop);
    return snprintf(dest, len, "%" PRIu32, *ptr);
}

PropertyInfo qdev_prop_uint32 = {
    .name  = "uint32",
    .type  = PROP_TYPE_UINT32,
    .size  = sizeof(uint32_t),
    .parse = parse_uint32,
    .print = print_uint32,
};

static int parse_int32(DeviceState *dev, Property *prop, const char *str)
{
    int32_t *ptr = qdev_get_prop_ptr(dev, prop);

    if (sscanf(str, "%" PRId32, ptr) != 1)
        return -1;
    return 0;
}

static int print_int32(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    int32_t *ptr = qdev_get_prop_ptr(dev, prop);
    return snprintf(dest, len, "%" PRId32, *ptr);
}

PropertyInfo qdev_prop_int32 = {
    .name  = "int32",
    .type  = PROP_TYPE_INT32,
    .size  = sizeof(int32_t),
    .parse = parse_int32,
    .print = print_int32,
};

/* --- 32bit hex value --- */

static int parse_hex32(DeviceState *dev, Property *prop, const char *str)
{
    uint32_t *ptr = qdev_get_prop_ptr(dev, prop);

    if (sscanf(str, "%" PRIx32, ptr) != 1)
        return -1;
    return 0;
}

static int print_hex32(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    uint32_t *ptr = qdev_get_prop_ptr(dev, prop);
    return snprintf(dest, len, "0x%" PRIx32, *ptr);
}

PropertyInfo qdev_prop_hex32 = {
    .name  = "hex32",
    .type  = PROP_TYPE_UINT32,
    .size  = sizeof(uint32_t),
    .parse = parse_hex32,
    .print = print_hex32,
};

/* --- 64bit integer --- */

static int parse_uint64(DeviceState *dev, Property *prop, const char *str)
{
    uint64_t *ptr = qdev_get_prop_ptr(dev, prop);
    const char *fmt;

    /* accept both hex and decimal */
    fmt = strncasecmp(str, "0x",2) == 0 ? "%" PRIx64 : "%" PRIu64;
    if (sscanf(str, fmt, ptr) != 1)
        return -1;
    return 0;
}

static int print_uint64(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    uint64_t *ptr = qdev_get_prop_ptr(dev, prop);
    return snprintf(dest, len, "%" PRIu64, *ptr);
}

PropertyInfo qdev_prop_uint64 = {
    .name  = "uint64",
    .type  = PROP_TYPE_UINT64,
    .size  = sizeof(uint64_t),
    .parse = parse_uint64,
    .print = print_uint64,
};

/* --- 64bit hex value --- */

static int parse_hex64(DeviceState *dev, Property *prop, const char *str)
{
    uint64_t *ptr = qdev_get_prop_ptr(dev, prop);

    if (sscanf(str, "%" PRIx64, ptr) != 1)
        return -1;
    return 0;
}

static int print_hex64(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    uint64_t *ptr = qdev_get_prop_ptr(dev, prop);
    return snprintf(dest, len, "0x%" PRIx64, *ptr);
}

PropertyInfo qdev_prop_hex64 = {
    .name  = "hex64",
    .type  = PROP_TYPE_UINT64,
    .size  = sizeof(uint64_t),
    .parse = parse_hex64,
    .print = print_hex64,
};

/* --- string --- */

static int parse_string(DeviceState *dev, Property *prop, const char *str)
{
    char **ptr = qdev_get_prop_ptr(dev, prop);

    if (*ptr)
        qemu_free(*ptr);
    *ptr = qemu_strdup(str);
    return 0;
}

static int print_string(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    char **ptr = qdev_get_prop_ptr(dev, prop);
    if (!*ptr)
        return snprintf(dest, len, "<null>");
    return snprintf(dest, len, "\"%s\"", *ptr);
}

PropertyInfo qdev_prop_string = {
    .name  = "string",
    .type  = PROP_TYPE_STRING,
    .size  = sizeof(char*),
    .parse = parse_string,
    .print = print_string,
};

/* --- drive --- */

static int parse_drive(DeviceState *dev, Property *prop, const char *str)
{
    DriveInfo **ptr = qdev_get_prop_ptr(dev, prop);

    *ptr = drive_get_by_id(str);
    if (*ptr == NULL)
        return -1;
    return 0;
}

static int print_drive(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    DriveInfo **ptr = qdev_get_prop_ptr(dev, prop);
    return snprintf(dest, len, "%s", (*ptr) ? (*ptr)->id : "<null>");
}

PropertyInfo qdev_prop_drive = {
    .name  = "drive",
    .type  = PROP_TYPE_DRIVE,
    .size  = sizeof(DriveInfo*),
    .parse = parse_drive,
    .print = print_drive,
};

/* --- character device --- */

static int parse_chr(DeviceState *dev, Property *prop, const char *str)
{
    CharDriverState **ptr = qdev_get_prop_ptr(dev, prop);

    *ptr = qemu_chr_find(str);
    if (*ptr == NULL)
        return -1;
    return 0;
}

static int print_chr(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    CharDriverState **ptr = qdev_get_prop_ptr(dev, prop);

    if (*ptr && (*ptr)->label) {
        return snprintf(dest, len, "%s", (*ptr)->label);
    } else {
        return snprintf(dest, len, "<null>");
    }
}

PropertyInfo qdev_prop_chr = {
    .name  = "chr",
    .type  = PROP_TYPE_CHR,
    .size  = sizeof(CharDriverState*),
    .parse = parse_chr,
    .print = print_chr,
};

/* --- netdev device --- */

static int parse_netdev(DeviceState *dev, Property *prop, const char *str)
{
    VLANClientState **ptr = qdev_get_prop_ptr(dev, prop);

    *ptr = qemu_find_netdev(str);
    if (*ptr == NULL)
        return -1;
    return 0;
}

static int print_netdev(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    VLANClientState **ptr = qdev_get_prop_ptr(dev, prop);

    if (*ptr && (*ptr)->name) {
        return snprintf(dest, len, "%s", (*ptr)->name);
    } else {
        return snprintf(dest, len, "<null>");
    }
}

PropertyInfo qdev_prop_netdev = {
    .name  = "netdev",
    .type  = PROP_TYPE_NETDEV,
    .size  = sizeof(VLANClientState*),
    .parse = parse_netdev,
    .print = print_netdev,
};

/* --- vlan --- */

static int parse_vlan(DeviceState *dev, Property *prop, const char *str)
{
    VLANState **ptr = qdev_get_prop_ptr(dev, prop);
    int id;

    if (sscanf(str, "%d", &id) != 1)
        return -1;
    *ptr = qemu_find_vlan(id, 1);
    if (*ptr == NULL)
        return -1;
    return 0;
}

static int print_vlan(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    VLANState **ptr = qdev_get_prop_ptr(dev, prop);

    if (*ptr) {
        return snprintf(dest, len, "%d", (*ptr)->id);
    } else {
        return snprintf(dest, len, "<null>");
    }
}

PropertyInfo qdev_prop_vlan = {
    .name  = "vlan",
    .type  = PROP_TYPE_VLAN,
    .size  = sizeof(VLANClientState*),
    .parse = parse_vlan,
    .print = print_vlan,
};

/* --- pointer --- */

static int print_ptr(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    void **ptr = qdev_get_prop_ptr(dev, prop);
    return snprintf(dest, len, "<%p>", *ptr);
}

PropertyInfo qdev_prop_ptr = {
    .name  = "ptr",
    .type  = PROP_TYPE_PTR,
    .size  = sizeof(void*),
    .print = print_ptr,
};

/* --- mac address --- */

/*
 * accepted syntax versions:
 *   01:02:03:04:05:06
 *   01-02-03-04-05-06
 */
static int parse_mac(DeviceState *dev, Property *prop, const char *str)
{
    MACAddr *mac = qdev_get_prop_ptr(dev, prop);
    int i, pos;
    char *p;

    for (i = 0, pos = 0; i < 6; i++, pos += 3) {
        if (!qemu_isxdigit(str[pos]))
            return -1;
        if (!qemu_isxdigit(str[pos+1]))
            return -1;
        if (i == 5) {
            if (str[pos+2] != '\0')
                return -1;
        } else {
            if (str[pos+2] != ':' && str[pos+2] != '-')
                return -1;
        }
        mac->a[i] = strtol(str+pos, &p, 16);
    }
    return 0;
}

static int print_mac(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    MACAddr *mac = qdev_get_prop_ptr(dev, prop);

    return snprintf(dest, len, "%02x:%02x:%02x:%02x:%02x:%02x",
                    mac->a[0], mac->a[1], mac->a[2],
                    mac->a[3], mac->a[4], mac->a[5]);
}

PropertyInfo qdev_prop_macaddr = {
    .name  = "macaddr",
    .type  = PROP_TYPE_MACADDR,
    .size  = sizeof(MACAddr),
    .parse = parse_mac,
    .print = print_mac,
};

/* --- pci address --- */

/*
 * bus-local address, i.e. "$slot" or "$slot.$fn"
 */
static int parse_pci_devfn(DeviceState *dev, Property *prop, const char *str)
{
    uint32_t *ptr = qdev_get_prop_ptr(dev, prop);
    unsigned int slot, fn, n;

    if (sscanf(str, "%x.%x%n", &slot, &fn, &n) != 2) {
        fn = 0;
        if (sscanf(str, "%x%n", &slot, &n) != 1) {
            return -1;
        }
    }
    if (str[n] != '\0')
        return -1;
    if (fn > 7)
        return -1;
    *ptr = slot << 3 | fn;
    return 0;
}

static int print_pci_devfn(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    uint32_t *ptr = qdev_get_prop_ptr(dev, prop);

    if (*ptr == -1) {
        return snprintf(dest, len, "<unset>");
    } else {
        return snprintf(dest, len, "%02x.%x", *ptr >> 3, *ptr & 7);
    }
}

PropertyInfo qdev_prop_pci_devfn = {
    .name  = "pci-devfn",
    .type  = PROP_TYPE_UINT32,
    .size  = sizeof(uint32_t),
    .parse = parse_pci_devfn,
    .print = print_pci_devfn,
};

/* --- public helpers --- */

static Property *qdev_prop_walk(Property *props, const char *name)
{
    if (!props)
        return NULL;
    while (props->name) {
        if (strcmp(props->name, name) == 0)
            return props;
        props++;
    }
    return NULL;
}

static Property *qdev_prop_find(DeviceState *dev, const char *name)
{
    Property *prop;

    /* device properties */
    prop = qdev_prop_walk(dev->info->props, name);
    if (prop)
        return prop;

    /* bus properties */
    prop = qdev_prop_walk(dev->parent_bus->info->props, name);
    if (prop)
        return prop;

    return NULL;
}

int qdev_prop_exists(DeviceState *dev, const char *name)
{
    return qdev_prop_find(dev, name) ? true : false;
}

int qdev_prop_parse(DeviceState *dev, const char *name, const char *value)
{
    Property *prop;

    prop = qdev_prop_find(dev, name);
    if (!prop) {
        fprintf(stderr, "property \"%s.%s\" not found\n",
                dev->info->name, name);
        return -1;
    }
    if (!prop->info->parse) {
        fprintf(stderr, "property \"%s.%s\" has no parser\n",
                dev->info->name, name);
        return -1;
    }
    return prop->info->parse(dev, prop, value);
}

void qdev_prop_set(DeviceState *dev, const char *name, void *src, enum PropertyType type)
{
    Property *prop;
    void *dst;

    prop = qdev_prop_find(dev, name);
    if (!prop) {
        fprintf(stderr, "%s: property \"%s.%s\" not found\n",
                __FUNCTION__, dev->info->name, name);
        abort();
    }
    if (prop->info->type != type) {
        fprintf(stderr, "%s: property \"%s.%s\" type mismatch\n",
                __FUNCTION__, dev->info->name, name);
        abort();
    }
    dst = qdev_get_prop_ptr(dev, prop);
    memcpy(dst, src, prop->info->size);
}

void qdev_prop_set_uint8(DeviceState *dev, const char *name, uint8_t value)
{
    qdev_prop_set(dev, name, &value, PROP_TYPE_UINT8);
}

void qdev_prop_set_uint16(DeviceState *dev, const char *name, uint16_t value)
{
    qdev_prop_set(dev, name, &value, PROP_TYPE_UINT16);
}

void qdev_prop_set_uint32(DeviceState *dev, const char *name, uint32_t value)
{
    qdev_prop_set(dev, name, &value, PROP_TYPE_UINT32);
}

void qdev_prop_set_int32(DeviceState *dev, const char *name, int32_t value)
{
    qdev_prop_set(dev, name, &value, PROP_TYPE_INT32);
}

void qdev_prop_set_uint64(DeviceState *dev, const char *name, uint64_t value)
{
    qdev_prop_set(dev, name, &value, PROP_TYPE_UINT64);
}

void qdev_prop_set_drive(DeviceState *dev, const char *name, DriveInfo *value)
{
    qdev_prop_set(dev, name, &value, PROP_TYPE_DRIVE);
}

void qdev_prop_set_chr(DeviceState *dev, const char *name, CharDriverState *value)
{
    qdev_prop_set(dev, name, &value, PROP_TYPE_CHR);
}

void qdev_prop_set_netdev(DeviceState *dev, const char *name, VLANClientState *value)
{
    qdev_prop_set(dev, name, &value, PROP_TYPE_NETDEV);
}

void qdev_prop_set_vlan(DeviceState *dev, const char *name, VLANState *value)
{
    qdev_prop_set(dev, name, &value, PROP_TYPE_VLAN);
}

void qdev_prop_set_macaddr(DeviceState *dev, const char *name, uint8_t *value)
{
    qdev_prop_set(dev, name, value, PROP_TYPE_MACADDR);
}

void qdev_prop_set_ptr(DeviceState *dev, const char *name, void *value)
{
    qdev_prop_set(dev, name, &value, PROP_TYPE_PTR);
}

void qdev_prop_set_defaults(DeviceState *dev, Property *props)
{
    char *dst;

    if (!props)
        return;
    while (props->name) {
        if (props->defval) {
            dst = qdev_get_prop_ptr(dev, props);
            memcpy(dst, props->defval, props->info->size);
        }
        props++;
    }
}

static QTAILQ_HEAD(, GlobalProperty) global_props = QTAILQ_HEAD_INITIALIZER(global_props);

void qdev_prop_register_global(GlobalProperty *prop)
{
    QTAILQ_INSERT_TAIL(&global_props, prop, next);
}

void qdev_prop_register_global_list(GlobalProperty *props)
{
    int i;

    for (i = 0; props[i].driver != NULL; i++) {
        qdev_prop_register_global(props+i);
    }
}

void qdev_prop_set_globals(DeviceState *dev)
{
    GlobalProperty *prop;

    QTAILQ_FOREACH(prop, &global_props, next) {
        if (strcmp(dev->info->name, prop->driver) != 0 &&
            strcmp(dev->info->bus_info->name, prop->driver) != 0) {
            continue;
        }
        if (qdev_prop_parse(dev, prop->property, prop->value) != 0) {
            abort();
        }
    }
}
