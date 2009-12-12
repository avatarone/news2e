#include "qemu-common.h"
#include "qemu-option.h"
#include "qemu-config.h"
#include "sysemu.h"
#include "hw/qdev.h"

QemuOptsList qemu_drive_opts = {
    .name = "drive",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_drive_opts.head),
    .desc = {
        {
            .name = "bus",
            .type = QEMU_OPT_NUMBER,
            .help = "bus number",
        },{
            .name = "unit",
            .type = QEMU_OPT_NUMBER,
            .help = "unit number (i.e. lun for scsi)",
        },{
            .name = "if",
            .type = QEMU_OPT_STRING,
            .help = "interface (ide, scsi, sd, mtd, floppy, pflash, virtio)",
        },{
            .name = "index",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "cyls",
            .type = QEMU_OPT_NUMBER,
            .help = "number of cylinders (ide disk geometry)",
        },{
            .name = "heads",
            .type = QEMU_OPT_NUMBER,
            .help = "number of heads (ide disk geometry)",
        },{
            .name = "secs",
            .type = QEMU_OPT_NUMBER,
            .help = "number of sectors (ide disk geometry)",
        },{
            .name = "trans",
            .type = QEMU_OPT_STRING,
            .help = "chs translation (auto, lba. none)",
        },{
            .name = "media",
            .type = QEMU_OPT_STRING,
            .help = "media type (disk, cdrom)",
        },{
            .name = "snapshot",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "file",
            .type = QEMU_OPT_STRING,
            .help = "disk image",
        },{
            .name = "cache",
            .type = QEMU_OPT_STRING,
            .help = "host cache usage (none, writeback, writethrough)",
        },{
            .name = "aio",
            .type = QEMU_OPT_STRING,
            .help = "host AIO implementation (threads, native)",
        },{
            .name = "format",
            .type = QEMU_OPT_STRING,
            .help = "disk format (raw, qcow2, ...)",
        },{
            .name = "serial",
            .type = QEMU_OPT_STRING,
        },{
            .name = "rerror",
            .type = QEMU_OPT_STRING,
        },{
            .name = "werror",
            .type = QEMU_OPT_STRING,
        },{
            .name = "addr",
            .type = QEMU_OPT_STRING,
            .help = "pci address (virtio only)",
        },{
            .name = "readonly",
            .type = QEMU_OPT_BOOL,
        },
        { /* end if list */ }
    },
};

QemuOptsList qemu_chardev_opts = {
    .name = "chardev",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_chardev_opts.head),
    .desc = {
        {
            .name = "backend",
            .type = QEMU_OPT_STRING,
        },{
            .name = "path",
            .type = QEMU_OPT_STRING,
        },{
            .name = "host",
            .type = QEMU_OPT_STRING,
        },{
            .name = "port",
            .type = QEMU_OPT_STRING,
        },{
            .name = "localaddr",
            .type = QEMU_OPT_STRING,
        },{
            .name = "localport",
            .type = QEMU_OPT_STRING,
        },{
            .name = "to",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "ipv4",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "ipv6",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "wait",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "server",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "delay",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "telnet",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "width",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "height",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "cols",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "rows",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "mux",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "signal",
            .type = QEMU_OPT_BOOL,
        },
        { /* end if list */ }
    },
};

QemuOptsList qemu_device_opts = {
    .name = "device",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_device_opts.head),
    .desc = {
        /*
         * no elements => accept any
         * sanity checking will happen later
         * when setting device properties
         */
        { /* end if list */ }
    },
};

QemuOptsList qemu_netdev_opts = {
    .name = "netdev",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_netdev_opts.head),
    .desc = {
        /*
         * no elements => accept any params
         * validation will happen later
         */
        { /* end of list */ }
    },
};

QemuOptsList qemu_net_opts = {
    .name = "net",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_net_opts.head),
    .desc = {
        /*
         * no elements => accept any params
         * validation will happen later
         */
        { /* end of list */ }
    },
};

QemuOptsList qemu_rtc_opts = {
    .name = "rtc",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_rtc_opts.head),
    .desc = {
        {
            .name = "base",
            .type = QEMU_OPT_STRING,
        },{
            .name = "clock",
            .type = QEMU_OPT_STRING,
#ifdef TARGET_I386
        },{
            .name = "driftfix",
            .type = QEMU_OPT_STRING,
#endif
        },
        { /* end if list */ }
    },
};

QemuOptsList qemu_global_opts = {
    .name = "global",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_global_opts.head),
    .desc = {
        {
            .name = "driver",
            .type = QEMU_OPT_STRING,
        },{
            .name = "property",
            .type = QEMU_OPT_STRING,
        },{
            .name = "value",
            .type = QEMU_OPT_STRING,
        },
        { /* end if list */ }
    },
};

QemuOptsList qemu_mon_opts = {
    .name = "mon",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_mon_opts.head),
    .desc = {
        {
            .name = "mode",
            .type = QEMU_OPT_STRING,
        },{
            .name = "chardev",
            .type = QEMU_OPT_STRING,
        },{
            .name = "default",
            .type = QEMU_OPT_BOOL,
        },
        { /* end if list */ }
    },
};

static QemuOptsList *lists[] = {
    &qemu_drive_opts,
    &qemu_chardev_opts,
    &qemu_device_opts,
    &qemu_netdev_opts,
    &qemu_net_opts,
    &qemu_rtc_opts,
    &qemu_global_opts,
    &qemu_mon_opts,
    NULL,
};

static QemuOptsList *find_list(const char *group)
{
    int i;

    for (i = 0; lists[i] != NULL; i++) {
        if (strcmp(lists[i]->name, group) == 0)
            break;
    }
    if (lists[i] == NULL) {
        qemu_error("there is no option group \"%s\"\n", group);
    }
    return lists[i];
}

int qemu_set_option(const char *str)
{
    char group[64], id[64], arg[64];
    QemuOptsList *list;
    QemuOpts *opts;
    int rc, offset;

    rc = sscanf(str, "%63[^.].%63[^.].%63[^=]%n", group, id, arg, &offset);
    if (rc < 3 || str[offset] != '=') {
        qemu_error("can't parse: \"%s\"\n", str);
        return -1;
    }

    list = find_list(group);
    if (list == NULL) {
        return -1;
    }

    opts = qemu_opts_find(list, id);
    if (!opts) {
        qemu_error("there is no %s \"%s\" defined\n",
                   list->name, id);
        return -1;
    }

    if (qemu_opt_set(opts, arg, str+offset+1) == -1) {
        return -1;
    }
    return 0;
}

int qemu_global_option(const char *str)
{
    char driver[64], property[64];
    QemuOpts *opts;
    int rc, offset;

    rc = sscanf(str, "%63[^.].%63[^=]%n", driver, property, &offset);
    if (rc < 2 || str[offset] != '=') {
        qemu_error("can't parse: \"%s\"\n", str);
        return -1;
    }

    opts = qemu_opts_create(&qemu_global_opts, NULL, 0);
    qemu_opt_set(opts, "driver", driver);
    qemu_opt_set(opts, "property", property);
    qemu_opt_set(opts, "value", str+offset+1);
    return 0;
}

static int qemu_add_one_global(QemuOpts *opts, void *opaque)
{
    GlobalProperty *g;

    g = qemu_mallocz(sizeof(*g));
    g->driver   = qemu_opt_get(opts, "driver");
    g->property = qemu_opt_get(opts, "property");
    g->value    = qemu_opt_get(opts, "value");
    qdev_prop_register_global(g);
    return 0;
}

void qemu_add_globals(void)
{
    qemu_opts_foreach(&qemu_global_opts, qemu_add_one_global, NULL, 0);
}

struct ConfigWriteData {
    QemuOptsList *list;
    FILE *fp;
};

static int config_write_opt(const char *name, const char *value, void *opaque)
{
    struct ConfigWriteData *data = opaque;

    fprintf(data->fp, "  %s = \"%s\"\n", name, value);
    return 0;
}

static int config_write_opts(QemuOpts *opts, void *opaque)
{
    struct ConfigWriteData *data = opaque;
    const char *id = qemu_opts_id(opts);

    if (id) {
        fprintf(data->fp, "[%s \"%s\"]\n", data->list->name, id);
    } else {
        fprintf(data->fp, "[%s]\n", data->list->name);
    }
    qemu_opt_foreach(opts, config_write_opt, data, 0);
    fprintf(data->fp, "\n");
    return 0;
}

void qemu_config_write(FILE *fp)
{
    struct ConfigWriteData data = { .fp = fp };
    int i;

    fprintf(fp, "# qemu config file\n\n");
    for (i = 0; lists[i] != NULL; i++) {
        data.list = lists[i];
        qemu_opts_foreach(data.list, config_write_opts, &data, 0);
    }
}

int qemu_config_parse(FILE *fp)
{
    char line[1024], group[64], id[64], arg[64], value[1024];
    QemuOptsList *list = NULL;
    QemuOpts *opts = NULL;

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (line[0] == '\n') {
            /* skip empty lines */
            continue;
        }
        if (line[0] == '#') {
            /* comment */
            continue;
        }
        if (sscanf(line, "[%63s \"%63[^\"]\"]", group, id) == 2) {
            /* group with id */
            list = find_list(group);
            if (list == NULL)
                return -1;
            opts = qemu_opts_create(list, id, 1);
            continue;
        }
        if (sscanf(line, "[%63[^]]]", group) == 1) {
            /* group without id */
            list = find_list(group);
            if (list == NULL)
                return -1;
            opts = qemu_opts_create(list, NULL, 0);
            continue;
        }
        if (sscanf(line, " %63s = \"%1023[^\"]\"", arg, value) == 2) {
            /* arg = value */
            if (opts == NULL) {
                fprintf(stderr, "no group defined\n");
                return -1;
            }
            if (qemu_opt_set(opts, arg, value) != 0) {
                fprintf(stderr, "failed to set \"%s\" for %s\n",
                        arg, group);
                return -1;
            }
            continue;
        }
        fprintf(stderr, "parse error: %s\n", line);
        return -1;
    }
    return 0;
}
