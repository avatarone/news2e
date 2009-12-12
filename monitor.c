/*
 * QEMU monitor
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
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
#include <dirent.h>
#include "hw/hw.h"
#include "hw/qdev.h"
#include "hw/usb.h"
#include "hw/pcmcia.h"
#include "hw/pc.h"
#include "hw/pci.h"
#include "hw/watchdog.h"
#include "hw/loader.h"
#include "gdbstub.h"
#include "net.h"
#include "net/slirp.h"
#include "qemu-char.h"
#include "sysemu.h"
#include "monitor.h"
#include "readline.h"
#include "console.h"
#include "block.h"
#include "audio/audio.h"
#include "disas.h"
#include "balloon.h"
#include "qemu-timer.h"
#include "migration.h"
#include "kvm.h"
#include "acl.h"
#include "qint.h"
#include "qlist.h"
#include "qdict.h"
#include "qbool.h"
#include "qstring.h"
#include "qerror.h"
#include "qjson.h"
#include "json-streamer.h"
#include "json-parser.h"
#include "osdep.h"

//#define DEBUG
//#define DEBUG_COMPLETION

/*
 * Supported types:
 *
 * 'F'          filename
 * 'B'          block device name
 * 's'          string (accept optional quote)
 * 'i'          32 bit integer
 * 'l'          target long (32 or 64 bit)
 * '/'          optional gdb-like print format (like "/10x")
 *
 * '?'          optional type (for all types, except '/')
 * '.'          other form of optional type (for 'i' and 'l')
 * '-'          optional parameter (eg. '-f')
 *
 */

typedef struct mon_cmd_t {
    const char *name;
    const char *args_type;
    const char *params;
    const char *help;
    void (*user_print)(Monitor *mon, const QObject *data);
    union {
        void (*info)(Monitor *mon);
        void (*info_new)(Monitor *mon, QObject **ret_data);
        void (*cmd)(Monitor *mon, const QDict *qdict);
        void (*cmd_new)(Monitor *mon, const QDict *params, QObject **ret_data);
    } mhandler;
} mon_cmd_t;

/* file descriptors passed via SCM_RIGHTS */
typedef struct mon_fd_t mon_fd_t;
struct mon_fd_t {
    char *name;
    int fd;
    QLIST_ENTRY(mon_fd_t) next;
};

typedef struct MonitorControl {
    QObject *id;
    int print_enabled;
    JSONMessageParser parser;
} MonitorControl;

struct Monitor {
    CharDriverState *chr;
    int mux_out;
    int reset_seen;
    int flags;
    int suspend_cnt;
    uint8_t outbuf[1024];
    int outbuf_index;
    ReadLineState *rs;
    MonitorControl *mc;
    CPUState *mon_cpu;
    BlockDriverCompletionFunc *password_completion_cb;
    void *password_opaque;
    QError *error;
    QLIST_HEAD(,mon_fd_t) fds;
    QLIST_ENTRY(Monitor) entry;
};

static QLIST_HEAD(mon_list, Monitor) mon_list;

static const mon_cmd_t mon_cmds[];
static const mon_cmd_t info_cmds[];

Monitor *cur_mon = NULL;

static void monitor_command_cb(Monitor *mon, const char *cmdline,
                               void *opaque);

/* Return true if in control mode, false otherwise */
static inline int monitor_ctrl_mode(const Monitor *mon)
{
    return (mon->flags & MONITOR_USE_CONTROL);
}

static void monitor_read_command(Monitor *mon, int show_prompt)
{
    readline_start(mon->rs, "(qemu) ", 0, monitor_command_cb, NULL);
    if (show_prompt)
        readline_show_prompt(mon->rs);
}

static int monitor_read_password(Monitor *mon, ReadLineFunc *readline_func,
                                 void *opaque)
{
    if (monitor_ctrl_mode(mon)) {
        qemu_error_new(QERR_MISSING_PARAMETER, "password");
        return -EINVAL;
    } else if (mon->rs) {
        readline_start(mon->rs, "Password: ", 1, readline_func, opaque);
        /* prompt is printed on return from the command handler */
        return 0;
    } else {
        monitor_printf(mon, "terminal does not support password prompting\n");
        return -ENOTTY;
    }
}

void monitor_flush(Monitor *mon)
{
    if (mon && mon->outbuf_index != 0 && !mon->mux_out) {
        qemu_chr_write(mon->chr, mon->outbuf, mon->outbuf_index);
        mon->outbuf_index = 0;
    }
}

/* flush at every end of line or if the buffer is full */
static void monitor_puts(Monitor *mon, const char *str)
{
    char c;

    if (!mon)
        return;

    for(;;) {
        c = *str++;
        if (c == '\0')
            break;
        if (c == '\n')
            mon->outbuf[mon->outbuf_index++] = '\r';
        mon->outbuf[mon->outbuf_index++] = c;
        if (mon->outbuf_index >= (sizeof(mon->outbuf) - 1)
            || c == '\n')
            monitor_flush(mon);
    }
}

void monitor_vprintf(Monitor *mon, const char *fmt, va_list ap)
{
    if (mon->mc && !mon->mc->print_enabled) {
        qemu_error_new(QERR_UNDEFINED_ERROR);
    } else {
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        monitor_puts(mon, buf);
    }
}

void monitor_printf(Monitor *mon, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    monitor_vprintf(mon, fmt, ap);
    va_end(ap);
}

void monitor_print_filename(Monitor *mon, const char *filename)
{
    int i;

    for (i = 0; filename[i]; i++) {
        switch (filename[i]) {
        case ' ':
        case '"':
        case '\\':
            monitor_printf(mon, "\\%c", filename[i]);
            break;
        case '\t':
            monitor_printf(mon, "\\t");
            break;
        case '\r':
            monitor_printf(mon, "\\r");
            break;
        case '\n':
            monitor_printf(mon, "\\n");
            break;
        default:
            monitor_printf(mon, "%c", filename[i]);
            break;
        }
    }
}

static int monitor_fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    monitor_vprintf((Monitor *)stream, fmt, ap);
    va_end(ap);
    return 0;
}

static void monitor_user_noop(Monitor *mon, const QObject *data) { }

static inline int monitor_handler_ported(const mon_cmd_t *cmd)
{
    return cmd->user_print != NULL;
}

static inline int monitor_has_error(const Monitor *mon)
{
    return mon->error != NULL;
}

static void monitor_json_emitter(Monitor *mon, const QObject *data)
{
    QString *json;

    json = qobject_to_json(data);
    assert(json != NULL);

    mon->mc->print_enabled = 1;
    monitor_printf(mon, "%s\n", qstring_get_str(json));
    mon->mc->print_enabled = 0;

    QDECREF(json);
}

static void monitor_protocol_emitter(Monitor *mon, QObject *data)
{
    QDict *qmp;

    qmp = qdict_new();

    if (!monitor_has_error(mon)) {
        /* success response */
        if (data) {
            qobject_incref(data);
            qdict_put_obj(qmp, "return", data);
        } else {
            qdict_put(qmp, "return", qstring_from_str("OK"));
        }
    } else {
        /* error response */
        qdict_put(mon->error->error, "desc", qerror_human(mon->error));
        qdict_put(qmp, "error", mon->error->error);
        QINCREF(mon->error->error);
        QDECREF(mon->error);
        mon->error = NULL;
    }

    if (mon->mc->id) {
        qdict_put_obj(qmp, "id", mon->mc->id);
        mon->mc->id = NULL;
    }

    monitor_json_emitter(mon, QOBJECT(qmp));
    QDECREF(qmp);
}

static void timestamp_put(QDict *qdict)
{
    int err;
    QObject *obj;
    qemu_timeval tv;

    err = qemu_gettimeofday(&tv);
    if (err < 0)
        return;

    obj = qobject_from_jsonf("{ 'seconds': %" PRId64 ", "
                                "'microseconds': %" PRId64 " }",
                                (int64_t) tv.tv_sec, (int64_t) tv.tv_usec);
    assert(obj != NULL);

    qdict_put_obj(qdict, "timestamp", obj);
}

/**
 * monitor_protocol_event(): Generate a Monitor event
 *
 * Event-specific data can be emitted through the (optional) 'data' parameter.
 */
void monitor_protocol_event(MonitorEvent event, QObject *data)
{
    QDict *qmp;
    const char *event_name;
    Monitor *mon = cur_mon;

    assert(event < QEVENT_MAX);

    if (!monitor_ctrl_mode(mon))
        return;

    switch (event) {
        case QEVENT_DEBUG:
            event_name = "DEBUG";
            break;
        case QEVENT_SHUTDOWN:
            event_name = "SHUTDOWN";
            break;
        case QEVENT_RESET:
            event_name = "RESET";
            break;
        case QEVENT_POWERDOWN:
            event_name = "POWERDOWN";
            break;
        case QEVENT_STOP:
            event_name = "STOP";
            break;
        default:
            abort();
            break;
    }

    qmp = qdict_new();
    timestamp_put(qmp);
    qdict_put(qmp, "event", qstring_from_str(event_name));
    if (data)
        qdict_put_obj(qmp, "data", data);

    monitor_json_emitter(mon, QOBJECT(qmp));
    QDECREF(qmp);
}

static int compare_cmd(const char *name, const char *list)
{
    const char *p, *pstart;
    int len;
    len = strlen(name);
    p = list;
    for(;;) {
        pstart = p;
        p = strchr(p, '|');
        if (!p)
            p = pstart + strlen(pstart);
        if ((p - pstart) == len && !memcmp(pstart, name, len))
            return 1;
        if (*p == '\0')
            break;
        p++;
    }
    return 0;
}

static void help_cmd_dump(Monitor *mon, const mon_cmd_t *cmds,
                          const char *prefix, const char *name)
{
    const mon_cmd_t *cmd;

    for(cmd = cmds; cmd->name != NULL; cmd++) {
        if (!name || !strcmp(name, cmd->name))
            monitor_printf(mon, "%s%s %s -- %s\n", prefix, cmd->name,
                           cmd->params, cmd->help);
    }
}

static void help_cmd(Monitor *mon, const char *name)
{
    if (name && !strcmp(name, "info")) {
        help_cmd_dump(mon, info_cmds, "info ", NULL);
    } else {
        help_cmd_dump(mon, mon_cmds, "", name);
        if (name && !strcmp(name, "log")) {
            const CPULogItem *item;
            monitor_printf(mon, "Log items (comma separated):\n");
            monitor_printf(mon, "%-10s %s\n", "none", "remove all logs");
            for(item = cpu_log_items; item->mask != 0; item++) {
                monitor_printf(mon, "%-10s %s\n", item->name, item->help);
            }
        }
    }
}

static void do_help_cmd(Monitor *mon, const QDict *qdict)
{
    help_cmd(mon, qdict_get_try_str(qdict, "name"));
}

static void do_commit(Monitor *mon, const QDict *qdict)
{
    int all_devices;
    DriveInfo *dinfo;
    const char *device = qdict_get_str(qdict, "device");

    all_devices = !strcmp(device, "all");
    QTAILQ_FOREACH(dinfo, &drives, next) {
        if (!all_devices)
            if (strcmp(bdrv_get_device_name(dinfo->bdrv), device))
                continue;
        bdrv_commit(dinfo->bdrv);
    }
}

static void do_info(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    const mon_cmd_t *cmd;
    const char *item = qdict_get_try_str(qdict, "item");

    if (!item) {
        assert(monitor_ctrl_mode(mon) == 0);
        goto help;
    }

    for (cmd = info_cmds; cmd->name != NULL; cmd++) {
        if (compare_cmd(item, cmd->name))
            break;
    }

    if (cmd->name == NULL) {
        if (monitor_ctrl_mode(mon)) {
            qemu_error_new(QERR_COMMAND_NOT_FOUND, item);
            return;
        }
        goto help;
    }

    if (monitor_handler_ported(cmd)) {
        cmd->mhandler.info_new(mon, ret_data);

        if (!monitor_ctrl_mode(mon)) {
            /*
             * User Protocol function is called here, Monitor Protocol is
             * handled by monitor_call_handler()
             */
            if (*ret_data)
                cmd->user_print(mon, *ret_data);
        }
    } else {
        if (monitor_ctrl_mode(mon)) {
            /* handler not converted yet */
            qemu_error_new(QERR_COMMAND_NOT_FOUND, item);
        } else {
            cmd->mhandler.info(mon);
        }
    }

    return;

help:
    help_cmd(mon, "info");
}

static void do_info_version_print(Monitor *mon, const QObject *data)
{
    QDict *qdict;

    qdict = qobject_to_qdict(data);

    monitor_printf(mon, "%s%s\n", qdict_get_str(qdict, "qemu"),
                                  qdict_get_str(qdict, "package"));
}

/**
 * do_info_version(): Show QEMU version
 *
 * Return a QDict with the following information:
 *
 * - "qemu": QEMU's version
 * - "package": package's version
 *
 * Example:
 *
 * { "qemu": "0.11.50", "package": "" }
 */
static void do_info_version(Monitor *mon, QObject **ret_data)
{
    *ret_data = qobject_from_jsonf("{ 'qemu': %s, 'package': %s }",
                                   QEMU_VERSION, QEMU_PKGVERSION);
}

static void do_info_name_print(Monitor *mon, const QObject *data)
{
    QDict *qdict;

    qdict = qobject_to_qdict(data);
    if (qdict_size(qdict) == 0) {
        return;
    }

    monitor_printf(mon, "%s\n", qdict_get_str(qdict, "name"));
}

/**
 * do_info_name(): Show VM name
 *
 * Return a QDict with the following information:
 *
 * - "name": VM's name (optional)
 *
 * Example:
 *
 * { "name": "qemu-name" }
 */
static void do_info_name(Monitor *mon, QObject **ret_data)
{
    *ret_data = qemu_name ? qobject_from_jsonf("{'name': %s }", qemu_name) :
                            qobject_from_jsonf("{}");
}

static QObject *get_cmd_dict(const char *name)
{
    const char *p;

    /* Remove '|' from some commands */
    p = strchr(name, '|');
    if (p) {
        p++;
    } else {
        p = name;
    }

    return qobject_from_jsonf("{ 'name': %s }", p);
}

/**
 * do_info_commands(): List QMP available commands
 *
 * Each command is represented by a QDict, the returned QObject is a QList
 * of all commands.
 *
 * The QDict contains:
 *
 * - "name": command's name
 *
 * Example:
 *
 * { [ { "name": "query-balloon" }, { "name": "system_powerdown" } ] }
 */
static void do_info_commands(Monitor *mon, QObject **ret_data)
{
    QList *cmd_list;
    const mon_cmd_t *cmd;

    cmd_list = qlist_new();

    for (cmd = mon_cmds; cmd->name != NULL; cmd++) {
        if (monitor_handler_ported(cmd) && !compare_cmd(cmd->name, "info")) {
            qlist_append_obj(cmd_list, get_cmd_dict(cmd->name));
        }
    }

    for (cmd = info_cmds; cmd->name != NULL; cmd++) {
        if (monitor_handler_ported(cmd)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "query-%s", cmd->name);
            qlist_append_obj(cmd_list, get_cmd_dict(buf));
        }
    }

    *ret_data = QOBJECT(cmd_list);
}

#if defined(TARGET_I386)
static void do_info_hpet_print(Monitor *mon, const QObject *data)
{
    monitor_printf(mon, "HPET is %s by QEMU\n",
                   qdict_get_bool(qobject_to_qdict(data), "enabled") ?
                   "enabled" : "disabled");
}

/**
 * do_info_hpet(): Show HPET state
 *
 * Return a QDict with the following information:
 *
 * - "enabled": true if hpet if enabled, false otherwise
 *
 * Example:
 *
 * { "enabled": true }
 */
static void do_info_hpet(Monitor *mon, QObject **ret_data)
{
    *ret_data = qobject_from_jsonf("{ 'enabled': %i }", !no_hpet);
}
#endif

static void do_info_uuid_print(Monitor *mon, const QObject *data)
{
    monitor_printf(mon, "%s\n", qdict_get_str(qobject_to_qdict(data), "UUID"));
}

/**
 * do_info_uuid(): Show VM UUID
 *
 * Return a QDict with the following information:
 *
 * - "UUID": Universally Unique Identifier
 *
 * Example:
 *
 * { "UUID": "550e8400-e29b-41d4-a716-446655440000" }
 */
static void do_info_uuid(Monitor *mon, QObject **ret_data)
{
    char uuid[64];

    snprintf(uuid, sizeof(uuid), UUID_FMT, qemu_uuid[0], qemu_uuid[1],
                   qemu_uuid[2], qemu_uuid[3], qemu_uuid[4], qemu_uuid[5],
                   qemu_uuid[6], qemu_uuid[7], qemu_uuid[8], qemu_uuid[9],
                   qemu_uuid[10], qemu_uuid[11], qemu_uuid[12], qemu_uuid[13],
                   qemu_uuid[14], qemu_uuid[15]);
    *ret_data = qobject_from_jsonf("{ 'UUID': %s }", uuid);
}

/* get the current CPU defined by the user */
static int mon_set_cpu(int cpu_index)
{
    CPUState *env;

    for(env = first_cpu; env != NULL; env = env->next_cpu) {
        if (env->cpu_index == cpu_index) {
            cur_mon->mon_cpu = env;
            return 0;
        }
    }
    return -1;
}

static CPUState *mon_get_cpu(void)
{
    if (!cur_mon->mon_cpu) {
        mon_set_cpu(0);
    }
    cpu_synchronize_state(cur_mon->mon_cpu);
    return cur_mon->mon_cpu;
}

static void do_info_registers(Monitor *mon)
{
    CPUState *env;
    env = mon_get_cpu();
    if (!env)
        return;
#ifdef TARGET_I386
    cpu_dump_state(env, (FILE *)mon, monitor_fprintf,
                   X86_DUMP_FPU);
#else
    cpu_dump_state(env, (FILE *)mon, monitor_fprintf,
                   0);
#endif
}

static void print_cpu_iter(QObject *obj, void *opaque)
{
    QDict *cpu;
    int active = ' ';
    Monitor *mon = opaque;

    assert(qobject_type(obj) == QTYPE_QDICT);
    cpu = qobject_to_qdict(obj);

    if (qdict_get_bool(cpu, "current")) {
        active = '*';
    }

    monitor_printf(mon, "%c CPU #%d: ", active, (int)qdict_get_int(cpu, "CPU"));

#if defined(TARGET_I386)
    monitor_printf(mon, "pc=0x" TARGET_FMT_lx,
                   (target_ulong) qdict_get_int(cpu, "pc"));
#elif defined(TARGET_PPC)
    monitor_printf(mon, "nip=0x" TARGET_FMT_lx,
                   (target_long) qdict_get_int(cpu, "nip"));
#elif defined(TARGET_SPARC)
    monitor_printf(mon, "pc=0x " TARGET_FMT_lx,
                   (target_long) qdict_get_int(cpu, "pc"));
    monitor_printf(mon, "npc=0x" TARGET_FMT_lx,
                   (target_long) qdict_get_int(cpu, "npc"));
#elif defined(TARGET_MIPS)
    monitor_printf(mon, "PC=0x" TARGET_FMT_lx,
                   (target_long) qdict_get_int(cpu, "PC"));
#endif

    if (qdict_get_bool(cpu, "halted")) {
        monitor_printf(mon, " (halted)");
    }

    monitor_printf(mon, "\n");
}

static void monitor_print_cpus(Monitor *mon, const QObject *data)
{
    QList *cpu_list;

    assert(qobject_type(data) == QTYPE_QLIST);
    cpu_list = qobject_to_qlist(data);
    qlist_iter(cpu_list, print_cpu_iter, mon);
}

/**
 * do_info_cpus(): Show CPU information
 *
 * Return a QList. Each CPU is represented by a QDict, which contains:
 *
 * - "cpu": CPU index
 * - "current": true if this is the current CPU, false otherwise
 * - "halted": true if the cpu is halted, false otherwise
 * - Current program counter. The key's name depends on the architecture:
 *      "pc": i386/x86)64
 *      "nip": PPC
 *      "pc" and "npc": sparc
 *      "PC": mips
 *
 * Example:
 *
 * [ { "CPU": 0, "current": true, "halted": false, "pc": 3227107138 },
 *   { "CPU": 1, "current": false, "halted": true, "pc": 7108165 } ]
 */
static void do_info_cpus(Monitor *mon, QObject **ret_data)
{
    CPUState *env;
    QList *cpu_list;

    cpu_list = qlist_new();

    /* just to set the default cpu if not already done */
    mon_get_cpu();

    for(env = first_cpu; env != NULL; env = env->next_cpu) {
        QDict *cpu;
        QObject *obj;

        cpu_synchronize_state(env);

        obj = qobject_from_jsonf("{ 'CPU': %d, 'current': %i, 'halted': %i }",
                                 env->cpu_index, env == mon->mon_cpu,
                                 env->halted);
        assert(obj != NULL);

        cpu = qobject_to_qdict(obj);

#if defined(TARGET_I386)
        qdict_put(cpu, "pc", qint_from_int(env->eip + env->segs[R_CS].base));
#elif defined(TARGET_PPC)
        qdict_put(cpu, "nip", qint_from_int(env->nip));
#elif defined(TARGET_SPARC)
        qdict_put(cpu, "pc", qint_from_int(env->pc));
        qdict_put(cpu, "npc", qint_from_int(env->npc));
#elif defined(TARGET_MIPS)
        qdict_put(cpu, "PC", qint_from_int(env->active_tc.PC));
#endif

        qlist_append(cpu_list, cpu);
    }

    *ret_data = QOBJECT(cpu_list);
}

static void do_cpu_set(Monitor *mon, const QDict *qdict)
{
    int index = qdict_get_int(qdict, "index");
    if (mon_set_cpu(index) < 0)
        monitor_printf(mon, "Invalid CPU index\n");
}

static void do_info_jit(Monitor *mon)
{
    dump_exec_info((FILE *)mon, monitor_fprintf);
}

static void do_info_history(Monitor *mon)
{
    int i;
    const char *str;

    if (!mon->rs)
        return;
    i = 0;
    for(;;) {
        str = readline_get_history(mon->rs, i);
        if (!str)
            break;
        monitor_printf(mon, "%d: '%s'\n", i, str);
        i++;
    }
}

#if defined(TARGET_PPC)
/* XXX: not implemented in other targets */
static void do_info_cpu_stats(Monitor *mon)
{
    CPUState *env;

    env = mon_get_cpu();
    cpu_dump_statistics(env, (FILE *)mon, &monitor_fprintf, 0);
}
#endif

/**
 * do_quit(): Quit QEMU execution
 */
static void do_quit(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    exit(0);
}

static int eject_device(Monitor *mon, BlockDriverState *bs, int force)
{
    if (bdrv_is_inserted(bs)) {
        if (!force) {
            if (!bdrv_is_removable(bs)) {
                qemu_error_new(QERR_DEVICE_NOT_REMOVABLE,
                               bdrv_get_device_name(bs));
                return -1;
            }
            if (bdrv_is_locked(bs)) {
                qemu_error_new(QERR_DEVICE_LOCKED, bdrv_get_device_name(bs));
                return -1;
            }
        }
        bdrv_close(bs);
    }
    return 0;
}

static void do_eject(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    BlockDriverState *bs;
    int force = qdict_get_int(qdict, "force");
    const char *filename = qdict_get_str(qdict, "filename");

    bs = bdrv_find(filename);
    if (!bs) {
        qemu_error_new(QERR_DEVICE_NOT_FOUND, filename);
        return;
    }
    eject_device(mon, bs, force);
}

static void do_block_set_passwd(Monitor *mon, const QDict *qdict,
                                QObject **ret_data)
{
    BlockDriverState *bs;

    bs = bdrv_find(qdict_get_str(qdict, "device"));
    if (!bs) {
        qemu_error_new(QERR_DEVICE_NOT_FOUND, qdict_get_str(qdict, "device"));
        return;
    }

    if (bdrv_set_key(bs, qdict_get_str(qdict, "password")) < 0) {
        qemu_error_new(QERR_INVALID_PASSWORD);
    }
}

static void do_change_block(Monitor *mon, const char *device,
                            const char *filename, const char *fmt)
{
    BlockDriverState *bs;
    BlockDriver *drv = NULL;

    bs = bdrv_find(device);
    if (!bs) {
        qemu_error_new(QERR_DEVICE_NOT_FOUND, device);
        return;
    }
    if (fmt) {
        drv = bdrv_find_whitelisted_format(fmt);
        if (!drv) {
            qemu_error_new(QERR_INVALID_BLOCK_FORMAT, fmt);
            return;
        }
    }
    if (eject_device(mon, bs, 0) < 0)
        return;
    bdrv_open2(bs, filename, 0, drv);
    monitor_read_bdrv_key_start(mon, bs, NULL, NULL);
}

static void change_vnc_password(const char *password)
{
    if (vnc_display_password(NULL, password) < 0)
        qemu_error_new(QERR_SET_PASSWD_FAILED);

}

static void change_vnc_password_cb(Monitor *mon, const char *password,
                                   void *opaque)
{
    change_vnc_password(password);
    monitor_read_command(mon, 1);
}

static void do_change_vnc(Monitor *mon, const char *target, const char *arg)
{
    if (strcmp(target, "passwd") == 0 ||
        strcmp(target, "password") == 0) {
        if (arg) {
            char password[9];
            strncpy(password, arg, sizeof(password));
            password[sizeof(password) - 1] = '\0';
            change_vnc_password(password);
        } else {
            monitor_read_password(mon, change_vnc_password_cb, NULL);
        }
    } else {
        if (vnc_display_open(NULL, target) < 0)
            qemu_error_new(QERR_VNC_SERVER_FAILED, target);
    }
}

/**
 * do_change(): Change a removable medium, or VNC configuration
 */
static void do_change(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    const char *device = qdict_get_str(qdict, "device");
    const char *target = qdict_get_str(qdict, "target");
    const char *arg = qdict_get_try_str(qdict, "arg");
    if (strcmp(device, "vnc") == 0) {
        do_change_vnc(mon, target, arg);
    } else {
        do_change_block(mon, device, target, arg);
    }
}

static void do_screen_dump(Monitor *mon, const QDict *qdict)
{
    vga_hw_screen_dump(qdict_get_str(qdict, "filename"));
}

static void do_logfile(Monitor *mon, const QDict *qdict)
{
    cpu_set_log_filename(qdict_get_str(qdict, "filename"));
}

static void do_log(Monitor *mon, const QDict *qdict)
{
    int mask;
    const char *items = qdict_get_str(qdict, "items");

    if (!strcmp(items, "none")) {
        mask = 0;
    } else {
        mask = cpu_str_to_log_mask(items);
        if (!mask) {
            help_cmd(mon, "log");
            return;
        }
    }
    cpu_set_log(mask);
}

static void do_singlestep(Monitor *mon, const QDict *qdict)
{
    const char *option = qdict_get_try_str(qdict, "option");
    if (!option || !strcmp(option, "on")) {
        singlestep = 1;
    } else if (!strcmp(option, "off")) {
        singlestep = 0;
    } else {
        monitor_printf(mon, "unexpected option %s\n", option);
    }
}

/**
 * do_stop(): Stop VM execution
 */
static void do_stop(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    vm_stop(EXCP_INTERRUPT);
}

static void encrypted_bdrv_it(void *opaque, BlockDriverState *bs);

struct bdrv_iterate_context {
    Monitor *mon;
    int err;
};

/**
 * do_cont(): Resume emulation.
 */
static void do_cont(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    struct bdrv_iterate_context context = { mon, 0 };

    bdrv_iterate(encrypted_bdrv_it, &context);
    /* only resume the vm if all keys are set and valid */
    if (!context.err)
        vm_start();
}

static void bdrv_key_cb(void *opaque, int err)
{
    Monitor *mon = opaque;

    /* another key was set successfully, retry to continue */
    if (!err)
        do_cont(mon, NULL, NULL);
}

static void encrypted_bdrv_it(void *opaque, BlockDriverState *bs)
{
    struct bdrv_iterate_context *context = opaque;

    if (!context->err && bdrv_key_required(bs)) {
        context->err = -EBUSY;
        monitor_read_bdrv_key_start(context->mon, bs, bdrv_key_cb,
                                    context->mon);
    }
}

static void do_gdbserver(Monitor *mon, const QDict *qdict)
{
    const char *device = qdict_get_try_str(qdict, "device");
    if (!device)
        device = "tcp::" DEFAULT_GDBSTUB_PORT;
    if (gdbserver_start(device) < 0) {
        monitor_printf(mon, "Could not open gdbserver on device '%s'\n",
                       device);
    } else if (strcmp(device, "none") == 0) {
        monitor_printf(mon, "Disabled gdbserver\n");
    } else {
        monitor_printf(mon, "Waiting for gdb connection on device '%s'\n",
                       device);
    }
}

static void do_watchdog_action(Monitor *mon, const QDict *qdict)
{
    const char *action = qdict_get_str(qdict, "action");
    if (select_watchdog_action(action) == -1) {
        monitor_printf(mon, "Unknown watchdog action '%s'\n", action);
    }
}

static void monitor_printc(Monitor *mon, int c)
{
    monitor_printf(mon, "'");
    switch(c) {
    case '\'':
        monitor_printf(mon, "\\'");
        break;
    case '\\':
        monitor_printf(mon, "\\\\");
        break;
    case '\n':
        monitor_printf(mon, "\\n");
        break;
    case '\r':
        monitor_printf(mon, "\\r");
        break;
    default:
        if (c >= 32 && c <= 126) {
            monitor_printf(mon, "%c", c);
        } else {
            monitor_printf(mon, "\\x%02x", c);
        }
        break;
    }
    monitor_printf(mon, "'");
}

static void memory_dump(Monitor *mon, int count, int format, int wsize,
                        target_phys_addr_t addr, int is_physical)
{
    CPUState *env;
    int nb_per_line, l, line_size, i, max_digits, len;
    uint8_t buf[16];
    uint64_t v;

    if (format == 'i') {
        int flags;
        flags = 0;
        env = mon_get_cpu();
        if (!env && !is_physical)
            return;
#ifdef TARGET_I386
        if (wsize == 2) {
            flags = 1;
        } else if (wsize == 4) {
            flags = 0;
        } else {
            /* as default we use the current CS size */
            flags = 0;
            if (env) {
#ifdef TARGET_X86_64
                if ((env->efer & MSR_EFER_LMA) &&
                    (env->segs[R_CS].flags & DESC_L_MASK))
                    flags = 2;
                else
#endif
                if (!(env->segs[R_CS].flags & DESC_B_MASK))
                    flags = 1;
            }
        }
#endif
        monitor_disas(mon, env, addr, count, is_physical, flags);
        return;
    }

    len = wsize * count;
    if (wsize == 1)
        line_size = 8;
    else
        line_size = 16;
    nb_per_line = line_size / wsize;
    max_digits = 0;

    switch(format) {
    case 'o':
        max_digits = (wsize * 8 + 2) / 3;
        break;
    default:
    case 'x':
        max_digits = (wsize * 8) / 4;
        break;
    case 'u':
    case 'd':
        max_digits = (wsize * 8 * 10 + 32) / 33;
        break;
    case 'c':
        wsize = 1;
        break;
    }

    while (len > 0) {
        if (is_physical)
            monitor_printf(mon, TARGET_FMT_plx ":", addr);
        else
            monitor_printf(mon, TARGET_FMT_lx ":", (target_ulong)addr);
        l = len;
        if (l > line_size)
            l = line_size;
        if (is_physical) {
            cpu_physical_memory_rw(addr, buf, l, 0);
        } else {
            env = mon_get_cpu();
            if (!env)
                break;
            if (cpu_memory_rw_debug(env, addr, buf, l, 0) < 0) {
                monitor_printf(mon, " Cannot access memory\n");
                break;
            }
        }
        i = 0;
        while (i < l) {
            switch(wsize) {
            default:
            case 1:
                v = ldub_raw(buf + i);
                break;
            case 2:
                v = lduw_raw(buf + i);
                break;
            case 4:
                v = (uint32_t)ldl_raw(buf + i);
                break;
            case 8:
                v = ldq_raw(buf + i);
                break;
            }
            monitor_printf(mon, " ");
            switch(format) {
            case 'o':
                monitor_printf(mon, "%#*" PRIo64, max_digits, v);
                break;
            case 'x':
                monitor_printf(mon, "0x%0*" PRIx64, max_digits, v);
                break;
            case 'u':
                monitor_printf(mon, "%*" PRIu64, max_digits, v);
                break;
            case 'd':
                monitor_printf(mon, "%*" PRId64, max_digits, v);
                break;
            case 'c':
                monitor_printc(mon, v);
                break;
            }
            i += wsize;
        }
        monitor_printf(mon, "\n");
        addr += l;
        len -= l;
    }
}

static void do_memory_dump(Monitor *mon, const QDict *qdict)
{
    int count = qdict_get_int(qdict, "count");
    int format = qdict_get_int(qdict, "format");
    int size = qdict_get_int(qdict, "size");
    target_long addr = qdict_get_int(qdict, "addr");

    memory_dump(mon, count, format, size, addr, 0);
}

static void do_physical_memory_dump(Monitor *mon, const QDict *qdict)
{
    int count = qdict_get_int(qdict, "count");
    int format = qdict_get_int(qdict, "format");
    int size = qdict_get_int(qdict, "size");
    target_phys_addr_t addr = qdict_get_int(qdict, "addr");

    memory_dump(mon, count, format, size, addr, 1);
}

static void do_print(Monitor *mon, const QDict *qdict)
{
    int format = qdict_get_int(qdict, "format");
    target_phys_addr_t val = qdict_get_int(qdict, "val");

#if TARGET_PHYS_ADDR_BITS == 32
    switch(format) {
    case 'o':
        monitor_printf(mon, "%#o", val);
        break;
    case 'x':
        monitor_printf(mon, "%#x", val);
        break;
    case 'u':
        monitor_printf(mon, "%u", val);
        break;
    default:
    case 'd':
        monitor_printf(mon, "%d", val);
        break;
    case 'c':
        monitor_printc(mon, val);
        break;
    }
#else
    switch(format) {
    case 'o':
        monitor_printf(mon, "%#" PRIo64, val);
        break;
    case 'x':
        monitor_printf(mon, "%#" PRIx64, val);
        break;
    case 'u':
        monitor_printf(mon, "%" PRIu64, val);
        break;
    default:
    case 'd':
        monitor_printf(mon, "%" PRId64, val);
        break;
    case 'c':
        monitor_printc(mon, val);
        break;
    }
#endif
    monitor_printf(mon, "\n");
}

static void do_memory_save(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    FILE *f;
    uint32_t size = qdict_get_int(qdict, "size");
    const char *filename = qdict_get_str(qdict, "filename");
    target_long addr = qdict_get_int(qdict, "val");
    uint32_t l;
    CPUState *env;
    uint8_t buf[1024];

    env = mon_get_cpu();
    if (!env)
        return;

    f = fopen(filename, "wb");
    if (!f) {
        monitor_printf(mon, "could not open '%s'\n", filename);
        return;
    }
    while (size != 0) {
        l = sizeof(buf);
        if (l > size)
            l = size;
        cpu_memory_rw_debug(env, addr, buf, l, 0);
        fwrite(buf, 1, l, f);
        addr += l;
        size -= l;
    }
    fclose(f);
}

static void do_physical_memory_save(Monitor *mon, const QDict *qdict,
                                    QObject **ret_data)
{
    FILE *f;
    uint32_t l;
    uint8_t buf[1024];
    uint32_t size = qdict_get_int(qdict, "size");
    const char *filename = qdict_get_str(qdict, "filename");
    target_phys_addr_t addr = qdict_get_int(qdict, "val");

    f = fopen(filename, "wb");
    if (!f) {
        monitor_printf(mon, "could not open '%s'\n", filename);
        return;
    }
    while (size != 0) {
        l = sizeof(buf);
        if (l > size)
            l = size;
        cpu_physical_memory_rw(addr, buf, l, 0);
        fwrite(buf, 1, l, f);
        fflush(f);
        addr += l;
        size -= l;
    }
    fclose(f);
}

static void do_sum(Monitor *mon, const QDict *qdict)
{
    uint32_t addr;
    uint8_t buf[1];
    uint16_t sum;
    uint32_t start = qdict_get_int(qdict, "start");
    uint32_t size = qdict_get_int(qdict, "size");

    sum = 0;
    for(addr = start; addr < (start + size); addr++) {
        cpu_physical_memory_rw(addr, buf, 1, 0);
        /* BSD sum algorithm ('sum' Unix command) */
        sum = (sum >> 1) | (sum << 15);
        sum += buf[0];
    }
    monitor_printf(mon, "%05d\n", sum);
}

typedef struct {
    int keycode;
    const char *name;
} KeyDef;

static const KeyDef key_defs[] = {
    { 0x2a, "shift" },
    { 0x36, "shift_r" },

    { 0x38, "alt" },
    { 0xb8, "alt_r" },
    { 0x64, "altgr" },
    { 0xe4, "altgr_r" },
    { 0x1d, "ctrl" },
    { 0x9d, "ctrl_r" },

    { 0xdd, "menu" },

    { 0x01, "esc" },

    { 0x02, "1" },
    { 0x03, "2" },
    { 0x04, "3" },
    { 0x05, "4" },
    { 0x06, "5" },
    { 0x07, "6" },
    { 0x08, "7" },
    { 0x09, "8" },
    { 0x0a, "9" },
    { 0x0b, "0" },
    { 0x0c, "minus" },
    { 0x0d, "equal" },
    { 0x0e, "backspace" },

    { 0x0f, "tab" },
    { 0x10, "q" },
    { 0x11, "w" },
    { 0x12, "e" },
    { 0x13, "r" },
    { 0x14, "t" },
    { 0x15, "y" },
    { 0x16, "u" },
    { 0x17, "i" },
    { 0x18, "o" },
    { 0x19, "p" },

    { 0x1c, "ret" },

    { 0x1e, "a" },
    { 0x1f, "s" },
    { 0x20, "d" },
    { 0x21, "f" },
    { 0x22, "g" },
    { 0x23, "h" },
    { 0x24, "j" },
    { 0x25, "k" },
    { 0x26, "l" },

    { 0x2c, "z" },
    { 0x2d, "x" },
    { 0x2e, "c" },
    { 0x2f, "v" },
    { 0x30, "b" },
    { 0x31, "n" },
    { 0x32, "m" },
    { 0x33, "comma" },
    { 0x34, "dot" },
    { 0x35, "slash" },

    { 0x37, "asterisk" },

    { 0x39, "spc" },
    { 0x3a, "caps_lock" },
    { 0x3b, "f1" },
    { 0x3c, "f2" },
    { 0x3d, "f3" },
    { 0x3e, "f4" },
    { 0x3f, "f5" },
    { 0x40, "f6" },
    { 0x41, "f7" },
    { 0x42, "f8" },
    { 0x43, "f9" },
    { 0x44, "f10" },
    { 0x45, "num_lock" },
    { 0x46, "scroll_lock" },

    { 0xb5, "kp_divide" },
    { 0x37, "kp_multiply" },
    { 0x4a, "kp_subtract" },
    { 0x4e, "kp_add" },
    { 0x9c, "kp_enter" },
    { 0x53, "kp_decimal" },
    { 0x54, "sysrq" },

    { 0x52, "kp_0" },
    { 0x4f, "kp_1" },
    { 0x50, "kp_2" },
    { 0x51, "kp_3" },
    { 0x4b, "kp_4" },
    { 0x4c, "kp_5" },
    { 0x4d, "kp_6" },
    { 0x47, "kp_7" },
    { 0x48, "kp_8" },
    { 0x49, "kp_9" },

    { 0x56, "<" },

    { 0x57, "f11" },
    { 0x58, "f12" },

    { 0xb7, "print" },

    { 0xc7, "home" },
    { 0xc9, "pgup" },
    { 0xd1, "pgdn" },
    { 0xcf, "end" },

    { 0xcb, "left" },
    { 0xc8, "up" },
    { 0xd0, "down" },
    { 0xcd, "right" },

    { 0xd2, "insert" },
    { 0xd3, "delete" },
#if defined(TARGET_SPARC) && !defined(TARGET_SPARC64)
    { 0xf0, "stop" },
    { 0xf1, "again" },
    { 0xf2, "props" },
    { 0xf3, "undo" },
    { 0xf4, "front" },
    { 0xf5, "copy" },
    { 0xf6, "open" },
    { 0xf7, "paste" },
    { 0xf8, "find" },
    { 0xf9, "cut" },
    { 0xfa, "lf" },
    { 0xfb, "help" },
    { 0xfc, "meta_l" },
    { 0xfd, "meta_r" },
    { 0xfe, "compose" },
#endif
    { 0, NULL },
};

static int get_keycode(const char *key)
{
    const KeyDef *p;
    char *endp;
    int ret;

    for(p = key_defs; p->name != NULL; p++) {
        if (!strcmp(key, p->name))
            return p->keycode;
    }
    if (strstart(key, "0x", NULL)) {
        ret = strtoul(key, &endp, 0);
        if (*endp == '\0' && ret >= 0x01 && ret <= 0xff)
            return ret;
    }
    return -1;
}

#define MAX_KEYCODES 16
static uint8_t keycodes[MAX_KEYCODES];
static int nb_pending_keycodes;
static QEMUTimer *key_timer;

static void release_keys(void *opaque)
{
    int keycode;

    while (nb_pending_keycodes > 0) {
        nb_pending_keycodes--;
        keycode = keycodes[nb_pending_keycodes];
        if (keycode & 0x80)
            kbd_put_keycode(0xe0);
        kbd_put_keycode(keycode | 0x80);
    }
}

static void do_sendkey(Monitor *mon, const QDict *qdict)
{
    char keyname_buf[16];
    char *separator;
    int keyname_len, keycode, i;
    const char *string = qdict_get_str(qdict, "string");
    int has_hold_time = qdict_haskey(qdict, "hold_time");
    int hold_time = qdict_get_try_int(qdict, "hold_time", -1);

    if (nb_pending_keycodes > 0) {
        qemu_del_timer(key_timer);
        release_keys(NULL);
    }
    if (!has_hold_time)
        hold_time = 100;
    i = 0;
    while (1) {
        separator = strchr(string, '-');
        keyname_len = separator ? separator - string : strlen(string);
        if (keyname_len > 0) {
            pstrcpy(keyname_buf, sizeof(keyname_buf), string);
            if (keyname_len > sizeof(keyname_buf) - 1) {
                monitor_printf(mon, "invalid key: '%s...'\n", keyname_buf);
                return;
            }
            if (i == MAX_KEYCODES) {
                monitor_printf(mon, "too many keys\n");
                return;
            }
            keyname_buf[keyname_len] = 0;
            keycode = get_keycode(keyname_buf);
            if (keycode < 0) {
                monitor_printf(mon, "unknown key: '%s'\n", keyname_buf);
                return;
            }
            keycodes[i++] = keycode;
        }
        if (!separator)
            break;
        string = separator + 1;
    }
    nb_pending_keycodes = i;
    /* key down events */
    for (i = 0; i < nb_pending_keycodes; i++) {
        keycode = keycodes[i];
        if (keycode & 0x80)
            kbd_put_keycode(0xe0);
        kbd_put_keycode(keycode & 0x7f);
    }
    /* delayed key up events */
    qemu_mod_timer(key_timer, qemu_get_clock(vm_clock) +
                   muldiv64(get_ticks_per_sec(), hold_time, 1000));
}

static int mouse_button_state;

static void do_mouse_move(Monitor *mon, const QDict *qdict)
{
    int dx, dy, dz;
    const char *dx_str = qdict_get_str(qdict, "dx_str");
    const char *dy_str = qdict_get_str(qdict, "dy_str");
    const char *dz_str = qdict_get_try_str(qdict, "dz_str");
    dx = strtol(dx_str, NULL, 0);
    dy = strtol(dy_str, NULL, 0);
    dz = 0;
    if (dz_str)
        dz = strtol(dz_str, NULL, 0);
    kbd_mouse_event(dx, dy, dz, mouse_button_state);
}

static void do_mouse_button(Monitor *mon, const QDict *qdict)
{
    int button_state = qdict_get_int(qdict, "button_state");
    mouse_button_state = button_state;
    kbd_mouse_event(0, 0, 0, mouse_button_state);
}

static void do_ioport_read(Monitor *mon, const QDict *qdict)
{
    int size = qdict_get_int(qdict, "size");
    int addr = qdict_get_int(qdict, "addr");
    int has_index = qdict_haskey(qdict, "index");
    uint32_t val;
    int suffix;

    if (has_index) {
        int index = qdict_get_int(qdict, "index");
        cpu_outb(addr & IOPORTS_MASK, index & 0xff);
        addr++;
    }
    addr &= 0xffff;

    switch(size) {
    default:
    case 1:
        val = cpu_inb(addr);
        suffix = 'b';
        break;
    case 2:
        val = cpu_inw(addr);
        suffix = 'w';
        break;
    case 4:
        val = cpu_inl(addr);
        suffix = 'l';
        break;
    }
    monitor_printf(mon, "port%c[0x%04x] = %#0*x\n",
                   suffix, addr, size * 2, val);
}

static void do_ioport_write(Monitor *mon, const QDict *qdict)
{
    int size = qdict_get_int(qdict, "size");
    int addr = qdict_get_int(qdict, "addr");
    int val = qdict_get_int(qdict, "val");

    addr &= IOPORTS_MASK;

    switch (size) {
    default:
    case 1:
        cpu_outb(addr, val);
        break;
    case 2:
        cpu_outw(addr, val);
        break;
    case 4:
        cpu_outl(addr, val);
        break;
    }
}

static void do_boot_set(Monitor *mon, const QDict *qdict)
{
    int res;
    const char *bootdevice = qdict_get_str(qdict, "bootdevice");

    res = qemu_boot_set(bootdevice);
    if (res == 0) {
        monitor_printf(mon, "boot device list now set to %s\n", bootdevice);
    } else if (res > 0) {
        monitor_printf(mon, "setting boot device list failed\n");
    } else {
        monitor_printf(mon, "no function defined to set boot device list for "
                       "this architecture\n");
    }
}

/**
 * do_system_reset(): Issue a machine reset
 */
static void do_system_reset(Monitor *mon, const QDict *qdict,
                            QObject **ret_data)
{
    qemu_system_reset_request();
}

/**
 * do_system_powerdown(): Issue a machine powerdown
 */
static void do_system_powerdown(Monitor *mon, const QDict *qdict,
                                QObject **ret_data)
{
    qemu_system_powerdown_request();
}

#if defined(TARGET_I386)
static void print_pte(Monitor *mon, uint32_t addr, uint32_t pte, uint32_t mask)
{
    monitor_printf(mon, "%08x: %08x %c%c%c%c%c%c%c%c\n",
                   addr,
                   pte & mask,
                   pte & PG_GLOBAL_MASK ? 'G' : '-',
                   pte & PG_PSE_MASK ? 'P' : '-',
                   pte & PG_DIRTY_MASK ? 'D' : '-',
                   pte & PG_ACCESSED_MASK ? 'A' : '-',
                   pte & PG_PCD_MASK ? 'C' : '-',
                   pte & PG_PWT_MASK ? 'T' : '-',
                   pte & PG_USER_MASK ? 'U' : '-',
                   pte & PG_RW_MASK ? 'W' : '-');
}

static void tlb_info(Monitor *mon)
{
    CPUState *env;
    int l1, l2;
    uint32_t pgd, pde, pte;

    env = mon_get_cpu();
    if (!env)
        return;

    if (!(env->cr[0] & CR0_PG_MASK)) {
        monitor_printf(mon, "PG disabled\n");
        return;
    }
    pgd = env->cr[3] & ~0xfff;
    for(l1 = 0; l1 < 1024; l1++) {
        cpu_physical_memory_read(pgd + l1 * 4, (uint8_t *)&pde, 4);
        pde = le32_to_cpu(pde);
        if (pde & PG_PRESENT_MASK) {
            if ((pde & PG_PSE_MASK) && (env->cr[4] & CR4_PSE_MASK)) {
                print_pte(mon, (l1 << 22), pde, ~((1 << 20) - 1));
            } else {
                for(l2 = 0; l2 < 1024; l2++) {
                    cpu_physical_memory_read((pde & ~0xfff) + l2 * 4,
                                             (uint8_t *)&pte, 4);
                    pte = le32_to_cpu(pte);
                    if (pte & PG_PRESENT_MASK) {
                        print_pte(mon, (l1 << 22) + (l2 << 12),
                                  pte & ~PG_PSE_MASK,
                                  ~0xfff);
                    }
                }
            }
        }
    }
}

static void mem_print(Monitor *mon, uint32_t *pstart, int *plast_prot,
                      uint32_t end, int prot)
{
    int prot1;
    prot1 = *plast_prot;
    if (prot != prot1) {
        if (*pstart != -1) {
            monitor_printf(mon, "%08x-%08x %08x %c%c%c\n",
                           *pstart, end, end - *pstart,
                           prot1 & PG_USER_MASK ? 'u' : '-',
                           'r',
                           prot1 & PG_RW_MASK ? 'w' : '-');
        }
        if (prot != 0)
            *pstart = end;
        else
            *pstart = -1;
        *plast_prot = prot;
    }
}

static void mem_info(Monitor *mon)
{
    CPUState *env;
    int l1, l2, prot, last_prot;
    uint32_t pgd, pde, pte, start, end;

    env = mon_get_cpu();
    if (!env)
        return;

    if (!(env->cr[0] & CR0_PG_MASK)) {
        monitor_printf(mon, "PG disabled\n");
        return;
    }
    pgd = env->cr[3] & ~0xfff;
    last_prot = 0;
    start = -1;
    for(l1 = 0; l1 < 1024; l1++) {
        cpu_physical_memory_read(pgd + l1 * 4, (uint8_t *)&pde, 4);
        pde = le32_to_cpu(pde);
        end = l1 << 22;
        if (pde & PG_PRESENT_MASK) {
            if ((pde & PG_PSE_MASK) && (env->cr[4] & CR4_PSE_MASK)) {
                prot = pde & (PG_USER_MASK | PG_RW_MASK | PG_PRESENT_MASK);
                mem_print(mon, &start, &last_prot, end, prot);
            } else {
                for(l2 = 0; l2 < 1024; l2++) {
                    cpu_physical_memory_read((pde & ~0xfff) + l2 * 4,
                                             (uint8_t *)&pte, 4);
                    pte = le32_to_cpu(pte);
                    end = (l1 << 22) + (l2 << 12);
                    if (pte & PG_PRESENT_MASK) {
                        prot = pte & (PG_USER_MASK | PG_RW_MASK | PG_PRESENT_MASK);
                    } else {
                        prot = 0;
                    }
                    mem_print(mon, &start, &last_prot, end, prot);
                }
            }
        } else {
            prot = 0;
            mem_print(mon, &start, &last_prot, end, prot);
        }
    }
}
#endif

#if defined(TARGET_SH4)

static void print_tlb(Monitor *mon, int idx, tlb_t *tlb)
{
    monitor_printf(mon, " tlb%i:\t"
                   "asid=%hhu vpn=%x\tppn=%x\tsz=%hhu size=%u\t"
                   "v=%hhu shared=%hhu cached=%hhu prot=%hhu "
                   "dirty=%hhu writethrough=%hhu\n",
                   idx,
                   tlb->asid, tlb->vpn, tlb->ppn, tlb->sz, tlb->size,
                   tlb->v, tlb->sh, tlb->c, tlb->pr,
                   tlb->d, tlb->wt);
}

static void tlb_info(Monitor *mon)
{
    CPUState *env = mon_get_cpu();
    int i;

    monitor_printf (mon, "ITLB:\n");
    for (i = 0 ; i < ITLB_SIZE ; i++)
        print_tlb (mon, i, &env->itlb[i]);
    monitor_printf (mon, "UTLB:\n");
    for (i = 0 ; i < UTLB_SIZE ; i++)
        print_tlb (mon, i, &env->utlb[i]);
}

#endif

static void do_info_kvm_print(Monitor *mon, const QObject *data)
{
    QDict *qdict;

    qdict = qobject_to_qdict(data);

    monitor_printf(mon, "kvm support: ");
    if (qdict_get_bool(qdict, "present")) {
        monitor_printf(mon, "%s\n", qdict_get_bool(qdict, "enabled") ?
                                    "enabled" : "disabled");
    } else {
        monitor_printf(mon, "not compiled\n");
    }
}

/**
 * do_info_kvm(): Show KVM information
 *
 * Return a QDict with the following information:
 *
 * - "enabled": true if KVM support is enabled, false otherwise
 * - "present": true if QEMU has KVM support, false otherwise
 *
 * Example:
 *
 * { "enabled": true, "present": true }
 */
static void do_info_kvm(Monitor *mon, QObject **ret_data)
{
#ifdef CONFIG_KVM
    *ret_data = qobject_from_jsonf("{ 'enabled': %i, 'present': true }",
                                   kvm_enabled());
#else
    *ret_data = qobject_from_jsonf("{ 'enabled': false, 'present': false }");
#endif
}

static void do_info_numa(Monitor *mon)
{
    int i;
    CPUState *env;

    monitor_printf(mon, "%d nodes\n", nb_numa_nodes);
    for (i = 0; i < nb_numa_nodes; i++) {
        monitor_printf(mon, "node %d cpus:", i);
        for (env = first_cpu; env != NULL; env = env->next_cpu) {
            if (env->numa_node == i) {
                monitor_printf(mon, " %d", env->cpu_index);
            }
        }
        monitor_printf(mon, "\n");
        monitor_printf(mon, "node %d size: %" PRId64 " MB\n", i,
            node_mem[i] >> 20);
    }
}

#ifdef CONFIG_PROFILER

int64_t qemu_time;
int64_t dev_time;

static void do_info_profile(Monitor *mon)
{
    int64_t total;
    total = qemu_time;
    if (total == 0)
        total = 1;
    monitor_printf(mon, "async time  %" PRId64 " (%0.3f)\n",
                   dev_time, dev_time / (double)get_ticks_per_sec());
    monitor_printf(mon, "qemu time   %" PRId64 " (%0.3f)\n",
                   qemu_time, qemu_time / (double)get_ticks_per_sec());
    qemu_time = 0;
    dev_time = 0;
}
#else
static void do_info_profile(Monitor *mon)
{
    monitor_printf(mon, "Internal profiler not compiled\n");
}
#endif

/* Capture support */
static QLIST_HEAD (capture_list_head, CaptureState) capture_head;

static void do_info_capture(Monitor *mon)
{
    int i;
    CaptureState *s;

    for (s = capture_head.lh_first, i = 0; s; s = s->entries.le_next, ++i) {
        monitor_printf(mon, "[%d]: ", i);
        s->ops.info (s->opaque);
    }
}

#ifdef HAS_AUDIO
static void do_stop_capture(Monitor *mon, const QDict *qdict)
{
    int i;
    int n = qdict_get_int(qdict, "n");
    CaptureState *s;

    for (s = capture_head.lh_first, i = 0; s; s = s->entries.le_next, ++i) {
        if (i == n) {
            s->ops.destroy (s->opaque);
            QLIST_REMOVE (s, entries);
            qemu_free (s);
            return;
        }
    }
}

static void do_wav_capture(Monitor *mon, const QDict *qdict)
{
    const char *path = qdict_get_str(qdict, "path");
    int has_freq = qdict_haskey(qdict, "freq");
    int freq = qdict_get_try_int(qdict, "freq", -1);
    int has_bits = qdict_haskey(qdict, "bits");
    int bits = qdict_get_try_int(qdict, "bits", -1);
    int has_channels = qdict_haskey(qdict, "nchannels");
    int nchannels = qdict_get_try_int(qdict, "nchannels", -1);
    CaptureState *s;

    s = qemu_mallocz (sizeof (*s));

    freq = has_freq ? freq : 44100;
    bits = has_bits ? bits : 16;
    nchannels = has_channels ? nchannels : 2;

    if (wav_start_capture (s, path, freq, bits, nchannels)) {
        monitor_printf(mon, "Faied to add wave capture\n");
        qemu_free (s);
    }
    QLIST_INSERT_HEAD (&capture_head, s, entries);
}
#endif

#if defined(TARGET_I386)
static void do_inject_nmi(Monitor *mon, const QDict *qdict)
{
    CPUState *env;
    int cpu_index = qdict_get_int(qdict, "cpu_index");

    for (env = first_cpu; env != NULL; env = env->next_cpu)
        if (env->cpu_index == cpu_index) {
            cpu_interrupt(env, CPU_INTERRUPT_NMI);
            break;
        }
}
#endif

static void do_info_status_print(Monitor *mon, const QObject *data)
{
    QDict *qdict;

    qdict = qobject_to_qdict(data);

    monitor_printf(mon, "VM status: ");
    if (qdict_get_bool(qdict, "running")) {
        monitor_printf(mon, "running");
        if (qdict_get_bool(qdict, "singlestep")) {
            monitor_printf(mon, " (single step mode)");
        }
    } else {
        monitor_printf(mon, "paused");
    }

    monitor_printf(mon, "\n");
}

/**
 * do_info_status(): VM status
 *
 * Return a QDict with the following information:
 *
 * - "running": true if the VM is running, or false if it is paused
 * - "singlestep": true if the VM is in single step mode, false otherwise
 *
 * Example:
 *
 * { "running": true, "singlestep": false }
 */
static void do_info_status(Monitor *mon, QObject **ret_data)
{
    *ret_data = qobject_from_jsonf("{ 'running': %i, 'singlestep': %i }",
                                    vm_running, singlestep);
}

/**
 * do_balloon(): Request VM to change its memory allocation
 */
static void do_balloon(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    int value = qdict_get_int(qdict, "value");
    ram_addr_t target = value;
    qemu_balloon(target << 20);
}

static void monitor_print_balloon(Monitor *mon, const QObject *data)
{
    QDict *qdict;

    qdict = qobject_to_qdict(data);

    monitor_printf(mon, "balloon: actual=%" PRId64 "\n",
                        qdict_get_int(qdict, "balloon") >> 20);
}

/**
 * do_info_balloon(): Balloon information
 *
 * Return a QDict with the following information:
 *
 * - "balloon": current balloon value in bytes
 *
 * Example:
 *
 * { "balloon": 1073741824 }
 */
static void do_info_balloon(Monitor *mon, QObject **ret_data)
{
    ram_addr_t actual;

    actual = qemu_balloon_status();
    if (kvm_enabled() && !kvm_has_sync_mmu())
        qemu_error_new(QERR_KVM_MISSING_CAP, "synchronous MMU", "balloon");
    else if (actual == 0)
        qemu_error_new(QERR_DEVICE_NOT_ACTIVE, "balloon");
    else
        *ret_data = qobject_from_jsonf("{ 'balloon': %" PRId64 "}",
                                       (int64_t) actual);
}

static qemu_acl *find_acl(Monitor *mon, const char *name)
{
    qemu_acl *acl = qemu_acl_find(name);

    if (!acl) {
        monitor_printf(mon, "acl: unknown list '%s'\n", name);
    }
    return acl;
}

static void do_acl_show(Monitor *mon, const QDict *qdict)
{
    const char *aclname = qdict_get_str(qdict, "aclname");
    qemu_acl *acl = find_acl(mon, aclname);
    qemu_acl_entry *entry;
    int i = 0;

    if (acl) {
        monitor_printf(mon, "policy: %s\n",
                       acl->defaultDeny ? "deny" : "allow");
        QTAILQ_FOREACH(entry, &acl->entries, next) {
            i++;
            monitor_printf(mon, "%d: %s %s\n", i,
                           entry->deny ? "deny" : "allow", entry->match);
        }
    }
}

static void do_acl_reset(Monitor *mon, const QDict *qdict)
{
    const char *aclname = qdict_get_str(qdict, "aclname");
    qemu_acl *acl = find_acl(mon, aclname);

    if (acl) {
        qemu_acl_reset(acl);
        monitor_printf(mon, "acl: removed all rules\n");
    }
}

static void do_acl_policy(Monitor *mon, const QDict *qdict)
{
    const char *aclname = qdict_get_str(qdict, "aclname");
    const char *policy = qdict_get_str(qdict, "policy");
    qemu_acl *acl = find_acl(mon, aclname);

    if (acl) {
        if (strcmp(policy, "allow") == 0) {
            acl->defaultDeny = 0;
            monitor_printf(mon, "acl: policy set to 'allow'\n");
        } else if (strcmp(policy, "deny") == 0) {
            acl->defaultDeny = 1;
            monitor_printf(mon, "acl: policy set to 'deny'\n");
        } else {
            monitor_printf(mon, "acl: unknown policy '%s', "
                           "expected 'deny' or 'allow'\n", policy);
        }
    }
}

static void do_acl_add(Monitor *mon, const QDict *qdict)
{
    const char *aclname = qdict_get_str(qdict, "aclname");
    const char *match = qdict_get_str(qdict, "match");
    const char *policy = qdict_get_str(qdict, "policy");
    int has_index = qdict_haskey(qdict, "index");
    int index = qdict_get_try_int(qdict, "index", -1);
    qemu_acl *acl = find_acl(mon, aclname);
    int deny, ret;

    if (acl) {
        if (strcmp(policy, "allow") == 0) {
            deny = 0;
        } else if (strcmp(policy, "deny") == 0) {
            deny = 1;
        } else {
            monitor_printf(mon, "acl: unknown policy '%s', "
                           "expected 'deny' or 'allow'\n", policy);
            return;
        }
        if (has_index)
            ret = qemu_acl_insert(acl, deny, match, index);
        else
            ret = qemu_acl_append(acl, deny, match);
        if (ret < 0)
            monitor_printf(mon, "acl: unable to add acl entry\n");
        else
            monitor_printf(mon, "acl: added rule at position %d\n", ret);
    }
}

static void do_acl_remove(Monitor *mon, const QDict *qdict)
{
    const char *aclname = qdict_get_str(qdict, "aclname");
    const char *match = qdict_get_str(qdict, "match");
    qemu_acl *acl = find_acl(mon, aclname);
    int ret;

    if (acl) {
        ret = qemu_acl_remove(acl, match);
        if (ret < 0)
            monitor_printf(mon, "acl: no matching acl entry\n");
        else
            monitor_printf(mon, "acl: removed rule at position %d\n", ret);
    }
}

#if defined(TARGET_I386)
static void do_inject_mce(Monitor *mon, const QDict *qdict)
{
    CPUState *cenv;
    int cpu_index = qdict_get_int(qdict, "cpu_index");
    int bank = qdict_get_int(qdict, "bank");
    uint64_t status = qdict_get_int(qdict, "status");
    uint64_t mcg_status = qdict_get_int(qdict, "mcg_status");
    uint64_t addr = qdict_get_int(qdict, "addr");
    uint64_t misc = qdict_get_int(qdict, "misc");

    for (cenv = first_cpu; cenv != NULL; cenv = cenv->next_cpu)
        if (cenv->cpu_index == cpu_index && cenv->mcg_cap) {
            cpu_inject_x86_mce(cenv, bank, status, mcg_status, addr, misc);
            break;
        }
}
#endif

static void do_getfd(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    const char *fdname = qdict_get_str(qdict, "fdname");
    mon_fd_t *monfd;
    int fd;

    fd = qemu_chr_get_msgfd(mon->chr);
    if (fd == -1) {
        qemu_error_new(QERR_FD_NOT_SUPPLIED);
        return;
    }

    if (qemu_isdigit(fdname[0])) {
        qemu_error_new(QERR_INVALID_PARAMETER, "fdname");
        return;
    }

    fd = dup(fd);
    if (fd == -1) {
        if (errno == EMFILE)
            qemu_error_new(QERR_TOO_MANY_FILES);
        else
            qemu_error_new(QERR_UNDEFINED_ERROR);
        return;
    }

    QLIST_FOREACH(monfd, &mon->fds, next) {
        if (strcmp(monfd->name, fdname) != 0) {
            continue;
        }

        close(monfd->fd);
        monfd->fd = fd;
        return;
    }

    monfd = qemu_mallocz(sizeof(mon_fd_t));
    monfd->name = qemu_strdup(fdname);
    monfd->fd = fd;

    QLIST_INSERT_HEAD(&mon->fds, monfd, next);
}

static void do_closefd(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    const char *fdname = qdict_get_str(qdict, "fdname");
    mon_fd_t *monfd;

    QLIST_FOREACH(monfd, &mon->fds, next) {
        if (strcmp(monfd->name, fdname) != 0) {
            continue;
        }

        QLIST_REMOVE(monfd, next);
        close(monfd->fd);
        qemu_free(monfd->name);
        qemu_free(monfd);
        return;
    }

    qemu_error_new(QERR_FD_NOT_FOUND, fdname);
}

static void do_loadvm(Monitor *mon, const QDict *qdict)
{
    int saved_vm_running  = vm_running;
    const char *name = qdict_get_str(qdict, "name");

    vm_stop(0);

    if (load_vmstate(mon, name) >= 0 && saved_vm_running)
        vm_start();
}

int monitor_get_fd(Monitor *mon, const char *fdname)
{
    mon_fd_t *monfd;

    QLIST_FOREACH(monfd, &mon->fds, next) {
        int fd;

        if (strcmp(monfd->name, fdname) != 0) {
            continue;
        }

        fd = monfd->fd;

        /* caller takes ownership of fd */
        QLIST_REMOVE(monfd, next);
        qemu_free(monfd->name);
        qemu_free(monfd);

        return fd;
    }

    return -1;
}

static const mon_cmd_t mon_cmds[] = {
#include "qemu-monitor.h"
    { NULL, NULL, },
};

/* Please update qemu-monitor.hx when adding or changing commands */
static const mon_cmd_t info_cmds[] = {
    {
        .name       = "version",
        .args_type  = "",
        .params     = "",
        .help       = "show the version of QEMU",
        .user_print = do_info_version_print,
        .mhandler.info_new = do_info_version,
    },
    {
        .name       = "commands",
        .args_type  = "",
        .params     = "",
        .help       = "list QMP available commands",
        .user_print = monitor_user_noop,
        .mhandler.info_new = do_info_commands,
    },
    {
        .name       = "network",
        .args_type  = "",
        .params     = "",
        .help       = "show the network state",
        .mhandler.info = do_info_network,
    },
    {
        .name       = "chardev",
        .args_type  = "",
        .params     = "",
        .help       = "show the character devices",
        .mhandler.info = qemu_chr_info,
    },
    {
        .name       = "block",
        .args_type  = "",
        .params     = "",
        .help       = "show the block devices",
        .mhandler.info = bdrv_info,
    },
    {
        .name       = "blockstats",
        .args_type  = "",
        .params     = "",
        .help       = "show block device statistics",
        .mhandler.info = bdrv_info_stats,
    },
    {
        .name       = "registers",
        .args_type  = "",
        .params     = "",
        .help       = "show the cpu registers",
        .mhandler.info = do_info_registers,
    },
    {
        .name       = "cpus",
        .args_type  = "",
        .params     = "",
        .help       = "show infos for each CPU",
        .user_print = monitor_print_cpus,
        .mhandler.info_new = do_info_cpus,
    },
    {
        .name       = "history",
        .args_type  = "",
        .params     = "",
        .help       = "show the command line history",
        .mhandler.info = do_info_history,
    },
    {
        .name       = "irq",
        .args_type  = "",
        .params     = "",
        .help       = "show the interrupts statistics (if available)",
        .mhandler.info = irq_info,
    },
    {
        .name       = "pic",
        .args_type  = "",
        .params     = "",
        .help       = "show i8259 (PIC) state",
        .mhandler.info = pic_info,
    },
    {
        .name       = "pci",
        .args_type  = "",
        .params     = "",
        .help       = "show PCI info",
        .mhandler.info = pci_info,
    },
#if defined(TARGET_I386) || defined(TARGET_SH4)
    {
        .name       = "tlb",
        .args_type  = "",
        .params     = "",
        .help       = "show virtual to physical memory mappings",
        .mhandler.info = tlb_info,
    },
#endif
#if defined(TARGET_I386)
    {
        .name       = "mem",
        .args_type  = "",
        .params     = "",
        .help       = "show the active virtual memory mappings",
        .mhandler.info = mem_info,
    },
    {
        .name       = "hpet",
        .args_type  = "",
        .params     = "",
        .help       = "show state of HPET",
        .user_print = do_info_hpet_print,
        .mhandler.info_new = do_info_hpet,
    },
#endif
    {
        .name       = "jit",
        .args_type  = "",
        .params     = "",
        .help       = "show dynamic compiler info",
        .mhandler.info = do_info_jit,
    },
    {
        .name       = "kvm",
        .args_type  = "",
        .params     = "",
        .help       = "show KVM information",
        .user_print = do_info_kvm_print,
        .mhandler.info_new = do_info_kvm,
    },
    {
        .name       = "numa",
        .args_type  = "",
        .params     = "",
        .help       = "show NUMA information",
        .mhandler.info = do_info_numa,
    },
    {
        .name       = "usb",
        .args_type  = "",
        .params     = "",
        .help       = "show guest USB devices",
        .mhandler.info = usb_info,
    },
    {
        .name       = "usbhost",
        .args_type  = "",
        .params     = "",
        .help       = "show host USB devices",
        .mhandler.info = usb_host_info,
    },
    {
        .name       = "profile",
        .args_type  = "",
        .params     = "",
        .help       = "show profiling information",
        .mhandler.info = do_info_profile,
    },
    {
        .name       = "capture",
        .args_type  = "",
        .params     = "",
        .help       = "show capture information",
        .mhandler.info = do_info_capture,
    },
    {
        .name       = "snapshots",
        .args_type  = "",
        .params     = "",
        .help       = "show the currently saved VM snapshots",
        .mhandler.info = do_info_snapshots,
    },
    {
        .name       = "status",
        .args_type  = "",
        .params     = "",
        .help       = "show the current VM status (running|paused)",
        .user_print = do_info_status_print,
        .mhandler.info_new = do_info_status,
    },
    {
        .name       = "pcmcia",
        .args_type  = "",
        .params     = "",
        .help       = "show guest PCMCIA status",
        .mhandler.info = pcmcia_info,
    },
    {
        .name       = "mice",
        .args_type  = "",
        .params     = "",
        .help       = "show which guest mouse is receiving events",
        .mhandler.info = do_info_mice,
    },
    {
        .name       = "vnc",
        .args_type  = "",
        .params     = "",
        .help       = "show the vnc server status",
        .mhandler.info = do_info_vnc,
    },
    {
        .name       = "name",
        .args_type  = "",
        .params     = "",
        .help       = "show the current VM name",
        .user_print = do_info_name_print,
        .mhandler.info_new = do_info_name,
    },
    {
        .name       = "uuid",
        .args_type  = "",
        .params     = "",
        .help       = "show the current VM UUID",
        .user_print = do_info_uuid_print,
        .mhandler.info_new = do_info_uuid,
    },
#if defined(TARGET_PPC)
    {
        .name       = "cpustats",
        .args_type  = "",
        .params     = "",
        .help       = "show CPU statistics",
        .mhandler.info = do_info_cpu_stats,
    },
#endif
#if defined(CONFIG_SLIRP)
    {
        .name       = "usernet",
        .args_type  = "",
        .params     = "",
        .help       = "show user network stack connection states",
        .mhandler.info = do_info_usernet,
    },
#endif
    {
        .name       = "migrate",
        .args_type  = "",
        .params     = "",
        .help       = "show migration status",
        .mhandler.info = do_info_migrate,
    },
    {
        .name       = "balloon",
        .args_type  = "",
        .params     = "",
        .help       = "show balloon information",
        .user_print = monitor_print_balloon,
        .mhandler.info_new = do_info_balloon,
    },
    {
        .name       = "qtree",
        .args_type  = "",
        .params     = "",
        .help       = "show device tree",
        .mhandler.info = do_info_qtree,
    },
    {
        .name       = "qdm",
        .args_type  = "",
        .params     = "",
        .help       = "show qdev device model list",
        .mhandler.info = do_info_qdm,
    },
    {
        .name       = "roms",
        .args_type  = "",
        .params     = "",
        .help       = "show roms",
        .mhandler.info = do_info_roms,
    },
    {
        .name       = NULL,
    },
};

/*******************************************************************/

static const char *pch;
static jmp_buf expr_env;

#define MD_TLONG 0
#define MD_I32   1

typedef struct MonitorDef {
    const char *name;
    int offset;
    target_long (*get_value)(const struct MonitorDef *md, int val);
    int type;
} MonitorDef;

#if defined(TARGET_I386)
static target_long monitor_get_pc (const struct MonitorDef *md, int val)
{
    CPUState *env = mon_get_cpu();
    if (!env)
        return 0;
    return env->eip + env->segs[R_CS].base;
}
#endif

#if defined(TARGET_PPC)
static target_long monitor_get_ccr (const struct MonitorDef *md, int val)
{
    CPUState *env = mon_get_cpu();
    unsigned int u;
    int i;

    if (!env)
        return 0;

    u = 0;
    for (i = 0; i < 8; i++)
        u |= env->crf[i] << (32 - (4 * i));

    return u;
}

static target_long monitor_get_msr (const struct MonitorDef *md, int val)
{
    CPUState *env = mon_get_cpu();
    if (!env)
        return 0;
    return env->msr;
}

static target_long monitor_get_xer (const struct MonitorDef *md, int val)
{
    CPUState *env = mon_get_cpu();
    if (!env)
        return 0;
    return env->xer;
}

static target_long monitor_get_decr (const struct MonitorDef *md, int val)
{
    CPUState *env = mon_get_cpu();
    if (!env)
        return 0;
    return cpu_ppc_load_decr(env);
}

static target_long monitor_get_tbu (const struct MonitorDef *md, int val)
{
    CPUState *env = mon_get_cpu();
    if (!env)
        return 0;
    return cpu_ppc_load_tbu(env);
}

static target_long monitor_get_tbl (const struct MonitorDef *md, int val)
{
    CPUState *env = mon_get_cpu();
    if (!env)
        return 0;
    return cpu_ppc_load_tbl(env);
}
#endif

#if defined(TARGET_SPARC)
#ifndef TARGET_SPARC64
static target_long monitor_get_psr (const struct MonitorDef *md, int val)
{
    CPUState *env = mon_get_cpu();
    if (!env)
        return 0;
    return GET_PSR(env);
}
#endif

static target_long monitor_get_reg(const struct MonitorDef *md, int val)
{
    CPUState *env = mon_get_cpu();
    if (!env)
        return 0;
    return env->regwptr[val];
}
#endif

static const MonitorDef monitor_defs[] = {
#ifdef TARGET_I386

#define SEG(name, seg) \
    { name, offsetof(CPUState, segs[seg].selector), NULL, MD_I32 },\
    { name ".base", offsetof(CPUState, segs[seg].base) },\
    { name ".limit", offsetof(CPUState, segs[seg].limit), NULL, MD_I32 },

    { "eax", offsetof(CPUState, regs[0]) },
    { "ecx", offsetof(CPUState, regs[1]) },
    { "edx", offsetof(CPUState, regs[2]) },
    { "ebx", offsetof(CPUState, regs[3]) },
    { "esp|sp", offsetof(CPUState, regs[4]) },
    { "ebp|fp", offsetof(CPUState, regs[5]) },
    { "esi", offsetof(CPUState, regs[6]) },
    { "edi", offsetof(CPUState, regs[7]) },
#ifdef TARGET_X86_64
    { "r8", offsetof(CPUState, regs[8]) },
    { "r9", offsetof(CPUState, regs[9]) },
    { "r10", offsetof(CPUState, regs[10]) },
    { "r11", offsetof(CPUState, regs[11]) },
    { "r12", offsetof(CPUState, regs[12]) },
    { "r13", offsetof(CPUState, regs[13]) },
    { "r14", offsetof(CPUState, regs[14]) },
    { "r15", offsetof(CPUState, regs[15]) },
#endif
    { "eflags", offsetof(CPUState, eflags) },
    { "eip", offsetof(CPUState, eip) },
    SEG("cs", R_CS)
    SEG("ds", R_DS)
    SEG("es", R_ES)
    SEG("ss", R_SS)
    SEG("fs", R_FS)
    SEG("gs", R_GS)
    { "pc", 0, monitor_get_pc, },
#elif defined(TARGET_PPC)
    /* General purpose registers */
    { "r0", offsetof(CPUState, gpr[0]) },
    { "r1", offsetof(CPUState, gpr[1]) },
    { "r2", offsetof(CPUState, gpr[2]) },
    { "r3", offsetof(CPUState, gpr[3]) },
    { "r4", offsetof(CPUState, gpr[4]) },
    { "r5", offsetof(CPUState, gpr[5]) },
    { "r6", offsetof(CPUState, gpr[6]) },
    { "r7", offsetof(CPUState, gpr[7]) },
    { "r8", offsetof(CPUState, gpr[8]) },
    { "r9", offsetof(CPUState, gpr[9]) },
    { "r10", offsetof(CPUState, gpr[10]) },
    { "r11", offsetof(CPUState, gpr[11]) },
    { "r12", offsetof(CPUState, gpr[12]) },
    { "r13", offsetof(CPUState, gpr[13]) },
    { "r14", offsetof(CPUState, gpr[14]) },
    { "r15", offsetof(CPUState, gpr[15]) },
    { "r16", offsetof(CPUState, gpr[16]) },
    { "r17", offsetof(CPUState, gpr[17]) },
    { "r18", offsetof(CPUState, gpr[18]) },
    { "r19", offsetof(CPUState, gpr[19]) },
    { "r20", offsetof(CPUState, gpr[20]) },
    { "r21", offsetof(CPUState, gpr[21]) },
    { "r22", offsetof(CPUState, gpr[22]) },
    { "r23", offsetof(CPUState, gpr[23]) },
    { "r24", offsetof(CPUState, gpr[24]) },
    { "r25", offsetof(CPUState, gpr[25]) },
    { "r26", offsetof(CPUState, gpr[26]) },
    { "r27", offsetof(CPUState, gpr[27]) },
    { "r28", offsetof(CPUState, gpr[28]) },
    { "r29", offsetof(CPUState, gpr[29]) },
    { "r30", offsetof(CPUState, gpr[30]) },
    { "r31", offsetof(CPUState, gpr[31]) },
    /* Floating point registers */
    { "f0", offsetof(CPUState, fpr[0]) },
    { "f1", offsetof(CPUState, fpr[1]) },
    { "f2", offsetof(CPUState, fpr[2]) },
    { "f3", offsetof(CPUState, fpr[3]) },
    { "f4", offsetof(CPUState, fpr[4]) },
    { "f5", offsetof(CPUState, fpr[5]) },
    { "f6", offsetof(CPUState, fpr[6]) },
    { "f7", offsetof(CPUState, fpr[7]) },
    { "f8", offsetof(CPUState, fpr[8]) },
    { "f9", offsetof(CPUState, fpr[9]) },
    { "f10", offsetof(CPUState, fpr[10]) },
    { "f11", offsetof(CPUState, fpr[11]) },
    { "f12", offsetof(CPUState, fpr[12]) },
    { "f13", offsetof(CPUState, fpr[13]) },
    { "f14", offsetof(CPUState, fpr[14]) },
    { "f15", offsetof(CPUState, fpr[15]) },
    { "f16", offsetof(CPUState, fpr[16]) },
    { "f17", offsetof(CPUState, fpr[17]) },
    { "f18", offsetof(CPUState, fpr[18]) },
    { "f19", offsetof(CPUState, fpr[19]) },
    { "f20", offsetof(CPUState, fpr[20]) },
    { "f21", offsetof(CPUState, fpr[21]) },
    { "f22", offsetof(CPUState, fpr[22]) },
    { "f23", offsetof(CPUState, fpr[23]) },
    { "f24", offsetof(CPUState, fpr[24]) },
    { "f25", offsetof(CPUState, fpr[25]) },
    { "f26", offsetof(CPUState, fpr[26]) },
    { "f27", offsetof(CPUState, fpr[27]) },
    { "f28", offsetof(CPUState, fpr[28]) },
    { "f29", offsetof(CPUState, fpr[29]) },
    { "f30", offsetof(CPUState, fpr[30]) },
    { "f31", offsetof(CPUState, fpr[31]) },
    { "fpscr", offsetof(CPUState, fpscr) },
    /* Next instruction pointer */
    { "nip|pc", offsetof(CPUState, nip) },
    { "lr", offsetof(CPUState, lr) },
    { "ctr", offsetof(CPUState, ctr) },
    { "decr", 0, &monitor_get_decr, },
    { "ccr", 0, &monitor_get_ccr, },
    /* Machine state register */
    { "msr", 0, &monitor_get_msr, },
    { "xer", 0, &monitor_get_xer, },
    { "tbu", 0, &monitor_get_tbu, },
    { "tbl", 0, &monitor_get_tbl, },
#if defined(TARGET_PPC64)
    /* Address space register */
    { "asr", offsetof(CPUState, asr) },
#endif
    /* Segment registers */
    { "sdr1", offsetof(CPUState, sdr1) },
    { "sr0", offsetof(CPUState, sr[0]) },
    { "sr1", offsetof(CPUState, sr[1]) },
    { "sr2", offsetof(CPUState, sr[2]) },
    { "sr3", offsetof(CPUState, sr[3]) },
    { "sr4", offsetof(CPUState, sr[4]) },
    { "sr5", offsetof(CPUState, sr[5]) },
    { "sr6", offsetof(CPUState, sr[6]) },
    { "sr7", offsetof(CPUState, sr[7]) },
    { "sr8", offsetof(CPUState, sr[8]) },
    { "sr9", offsetof(CPUState, sr[9]) },
    { "sr10", offsetof(CPUState, sr[10]) },
    { "sr11", offsetof(CPUState, sr[11]) },
    { "sr12", offsetof(CPUState, sr[12]) },
    { "sr13", offsetof(CPUState, sr[13]) },
    { "sr14", offsetof(CPUState, sr[14]) },
    { "sr15", offsetof(CPUState, sr[15]) },
    /* Too lazy to put BATs and SPRs ... */
#elif defined(TARGET_SPARC)
    { "g0", offsetof(CPUState, gregs[0]) },
    { "g1", offsetof(CPUState, gregs[1]) },
    { "g2", offsetof(CPUState, gregs[2]) },
    { "g3", offsetof(CPUState, gregs[3]) },
    { "g4", offsetof(CPUState, gregs[4]) },
    { "g5", offsetof(CPUState, gregs[5]) },
    { "g6", offsetof(CPUState, gregs[6]) },
    { "g7", offsetof(CPUState, gregs[7]) },
    { "o0", 0, monitor_get_reg },
    { "o1", 1, monitor_get_reg },
    { "o2", 2, monitor_get_reg },
    { "o3", 3, monitor_get_reg },
    { "o4", 4, monitor_get_reg },
    { "o5", 5, monitor_get_reg },
    { "o6", 6, monitor_get_reg },
    { "o7", 7, monitor_get_reg },
    { "l0", 8, monitor_get_reg },
    { "l1", 9, monitor_get_reg },
    { "l2", 10, monitor_get_reg },
    { "l3", 11, monitor_get_reg },
    { "l4", 12, monitor_get_reg },
    { "l5", 13, monitor_get_reg },
    { "l6", 14, monitor_get_reg },
    { "l7", 15, monitor_get_reg },
    { "i0", 16, monitor_get_reg },
    { "i1", 17, monitor_get_reg },
    { "i2", 18, monitor_get_reg },
    { "i3", 19, monitor_get_reg },
    { "i4", 20, monitor_get_reg },
    { "i5", 21, monitor_get_reg },
    { "i6", 22, monitor_get_reg },
    { "i7", 23, monitor_get_reg },
    { "pc", offsetof(CPUState, pc) },
    { "npc", offsetof(CPUState, npc) },
    { "y", offsetof(CPUState, y) },
#ifndef TARGET_SPARC64
    { "psr", 0, &monitor_get_psr, },
    { "wim", offsetof(CPUState, wim) },
#endif
    { "tbr", offsetof(CPUState, tbr) },
    { "fsr", offsetof(CPUState, fsr) },
    { "f0", offsetof(CPUState, fpr[0]) },
    { "f1", offsetof(CPUState, fpr[1]) },
    { "f2", offsetof(CPUState, fpr[2]) },
    { "f3", offsetof(CPUState, fpr[3]) },
    { "f4", offsetof(CPUState, fpr[4]) },
    { "f5", offsetof(CPUState, fpr[5]) },
    { "f6", offsetof(CPUState, fpr[6]) },
    { "f7", offsetof(CPUState, fpr[7]) },
    { "f8", offsetof(CPUState, fpr[8]) },
    { "f9", offsetof(CPUState, fpr[9]) },
    { "f10", offsetof(CPUState, fpr[10]) },
    { "f11", offsetof(CPUState, fpr[11]) },
    { "f12", offsetof(CPUState, fpr[12]) },
    { "f13", offsetof(CPUState, fpr[13]) },
    { "f14", offsetof(CPUState, fpr[14]) },
    { "f15", offsetof(CPUState, fpr[15]) },
    { "f16", offsetof(CPUState, fpr[16]) },
    { "f17", offsetof(CPUState, fpr[17]) },
    { "f18", offsetof(CPUState, fpr[18]) },
    { "f19", offsetof(CPUState, fpr[19]) },
    { "f20", offsetof(CPUState, fpr[20]) },
    { "f21", offsetof(CPUState, fpr[21]) },
    { "f22", offsetof(CPUState, fpr[22]) },
    { "f23", offsetof(CPUState, fpr[23]) },
    { "f24", offsetof(CPUState, fpr[24]) },
    { "f25", offsetof(CPUState, fpr[25]) },
    { "f26", offsetof(CPUState, fpr[26]) },
    { "f27", offsetof(CPUState, fpr[27]) },
    { "f28", offsetof(CPUState, fpr[28]) },
    { "f29", offsetof(CPUState, fpr[29]) },
    { "f30", offsetof(CPUState, fpr[30]) },
    { "f31", offsetof(CPUState, fpr[31]) },
#ifdef TARGET_SPARC64
    { "f32", offsetof(CPUState, fpr[32]) },
    { "f34", offsetof(CPUState, fpr[34]) },
    { "f36", offsetof(CPUState, fpr[36]) },
    { "f38", offsetof(CPUState, fpr[38]) },
    { "f40", offsetof(CPUState, fpr[40]) },
    { "f42", offsetof(CPUState, fpr[42]) },
    { "f44", offsetof(CPUState, fpr[44]) },
    { "f46", offsetof(CPUState, fpr[46]) },
    { "f48", offsetof(CPUState, fpr[48]) },
    { "f50", offsetof(CPUState, fpr[50]) },
    { "f52", offsetof(CPUState, fpr[52]) },
    { "f54", offsetof(CPUState, fpr[54]) },
    { "f56", offsetof(CPUState, fpr[56]) },
    { "f58", offsetof(CPUState, fpr[58]) },
    { "f60", offsetof(CPUState, fpr[60]) },
    { "f62", offsetof(CPUState, fpr[62]) },
    { "asi", offsetof(CPUState, asi) },
    { "pstate", offsetof(CPUState, pstate) },
    { "cansave", offsetof(CPUState, cansave) },
    { "canrestore", offsetof(CPUState, canrestore) },
    { "otherwin", offsetof(CPUState, otherwin) },
    { "wstate", offsetof(CPUState, wstate) },
    { "cleanwin", offsetof(CPUState, cleanwin) },
    { "fprs", offsetof(CPUState, fprs) },
#endif
#endif
    { NULL },
};

static void expr_error(Monitor *mon, const char *msg)
{
    monitor_printf(mon, "%s\n", msg);
    longjmp(expr_env, 1);
}

/* return 0 if OK, -1 if not found, -2 if no CPU defined */
static int get_monitor_def(target_long *pval, const char *name)
{
    const MonitorDef *md;
    void *ptr;

    for(md = monitor_defs; md->name != NULL; md++) {
        if (compare_cmd(name, md->name)) {
            if (md->get_value) {
                *pval = md->get_value(md, md->offset);
            } else {
                CPUState *env = mon_get_cpu();
                if (!env)
                    return -2;
                ptr = (uint8_t *)env + md->offset;
                switch(md->type) {
                case MD_I32:
                    *pval = *(int32_t *)ptr;
                    break;
                case MD_TLONG:
                    *pval = *(target_long *)ptr;
                    break;
                default:
                    *pval = 0;
                    break;
                }
            }
            return 0;
        }
    }
    return -1;
}

static void next(void)
{
    if (*pch != '\0') {
        pch++;
        while (qemu_isspace(*pch))
            pch++;
    }
}

static int64_t expr_sum(Monitor *mon);

static int64_t expr_unary(Monitor *mon)
{
    int64_t n;
    char *p;
    int ret;

    switch(*pch) {
    case '+':
        next();
        n = expr_unary(mon);
        break;
    case '-':
        next();
        n = -expr_unary(mon);
        break;
    case '~':
        next();
        n = ~expr_unary(mon);
        break;
    case '(':
        next();
        n = expr_sum(mon);
        if (*pch != ')') {
            expr_error(mon, "')' expected");
        }
        next();
        break;
    case '\'':
        pch++;
        if (*pch == '\0')
            expr_error(mon, "character constant expected");
        n = *pch;
        pch++;
        if (*pch != '\'')
            expr_error(mon, "missing terminating \' character");
        next();
        break;
    case '$':
        {
            char buf[128], *q;
            target_long reg=0;

            pch++;
            q = buf;
            while ((*pch >= 'a' && *pch <= 'z') ||
                   (*pch >= 'A' && *pch <= 'Z') ||
                   (*pch >= '0' && *pch <= '9') ||
                   *pch == '_' || *pch == '.') {
                if ((q - buf) < sizeof(buf) - 1)
                    *q++ = *pch;
                pch++;
            }
            while (qemu_isspace(*pch))
                pch++;
            *q = 0;
            ret = get_monitor_def(&reg, buf);
            if (ret == -1)
                expr_error(mon, "unknown register");
            else if (ret == -2)
                expr_error(mon, "no cpu defined");
            n = reg;
        }
        break;
    case '\0':
        expr_error(mon, "unexpected end of expression");
        n = 0;
        break;
    default:
#if TARGET_PHYS_ADDR_BITS > 32
        n = strtoull(pch, &p, 0);
#else
        n = strtoul(pch, &p, 0);
#endif
        if (pch == p) {
            expr_error(mon, "invalid char in expression");
        }
        pch = p;
        while (qemu_isspace(*pch))
            pch++;
        break;
    }
    return n;
}


static int64_t expr_prod(Monitor *mon)
{
    int64_t val, val2;
    int op;

    val = expr_unary(mon);
    for(;;) {
        op = *pch;
        if (op != '*' && op != '/' && op != '%')
            break;
        next();
        val2 = expr_unary(mon);
        switch(op) {
        default:
        case '*':
            val *= val2;
            break;
        case '/':
        case '%':
            if (val2 == 0)
                expr_error(mon, "division by zero");
            if (op == '/')
                val /= val2;
            else
                val %= val2;
            break;
        }
    }
    return val;
}

static int64_t expr_logic(Monitor *mon)
{
    int64_t val, val2;
    int op;

    val = expr_prod(mon);
    for(;;) {
        op = *pch;
        if (op != '&' && op != '|' && op != '^')
            break;
        next();
        val2 = expr_prod(mon);
        switch(op) {
        default:
        case '&':
            val &= val2;
            break;
        case '|':
            val |= val2;
            break;
        case '^':
            val ^= val2;
            break;
        }
    }
    return val;
}

static int64_t expr_sum(Monitor *mon)
{
    int64_t val, val2;
    int op;

    val = expr_logic(mon);
    for(;;) {
        op = *pch;
        if (op != '+' && op != '-')
            break;
        next();
        val2 = expr_logic(mon);
        if (op == '+')
            val += val2;
        else
            val -= val2;
    }
    return val;
}

static int get_expr(Monitor *mon, int64_t *pval, const char **pp)
{
    pch = *pp;
    if (setjmp(expr_env)) {
        *pp = pch;
        return -1;
    }
    while (qemu_isspace(*pch))
        pch++;
    *pval = expr_sum(mon);
    *pp = pch;
    return 0;
}

static int get_str(char *buf, int buf_size, const char **pp)
{
    const char *p;
    char *q;
    int c;

    q = buf;
    p = *pp;
    while (qemu_isspace(*p))
        p++;
    if (*p == '\0') {
    fail:
        *q = '\0';
        *pp = p;
        return -1;
    }
    if (*p == '\"') {
        p++;
        while (*p != '\0' && *p != '\"') {
            if (*p == '\\') {
                p++;
                c = *p++;
                switch(c) {
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case '\\':
                case '\'':
                case '\"':
                    break;
                default:
                    qemu_printf("unsupported escape code: '\\%c'\n", c);
                    goto fail;
                }
                if ((q - buf) < buf_size - 1) {
                    *q++ = c;
                }
            } else {
                if ((q - buf) < buf_size - 1) {
                    *q++ = *p;
                }
                p++;
            }
        }
        if (*p != '\"') {
            qemu_printf("unterminated string\n");
            goto fail;
        }
        p++;
    } else {
        while (*p != '\0' && !qemu_isspace(*p)) {
            if ((q - buf) < buf_size - 1) {
                *q++ = *p;
            }
            p++;
        }
    }
    *q = '\0';
    *pp = p;
    return 0;
}

/*
 * Store the command-name in cmdname, and return a pointer to
 * the remaining of the command string.
 */
static const char *get_command_name(const char *cmdline,
                                    char *cmdname, size_t nlen)
{
    size_t len;
    const char *p, *pstart;

    p = cmdline;
    while (qemu_isspace(*p))
        p++;
    if (*p == '\0')
        return NULL;
    pstart = p;
    while (*p != '\0' && *p != '/' && !qemu_isspace(*p))
        p++;
    len = p - pstart;
    if (len > nlen - 1)
        len = nlen - 1;
    memcpy(cmdname, pstart, len);
    cmdname[len] = '\0';
    return p;
}

/**
 * Read key of 'type' into 'key' and return the current
 * 'type' pointer.
 */
static char *key_get_info(const char *type, char **key)
{
    size_t len;
    char *p, *str;

    if (*type == ',')
        type++;

    p = strchr(type, ':');
    if (!p) {
        *key = NULL;
        return NULL;
    }
    len = p - type;

    str = qemu_malloc(len + 1);
    memcpy(str, type, len);
    str[len] = '\0';

    *key = str;
    return ++p;
}

static int default_fmt_format = 'x';
static int default_fmt_size = 4;

#define MAX_ARGS 16

static int is_valid_option(const char *c, const char *typestr)
{
    char option[3];
  
    option[0] = '-';
    option[1] = *c;
    option[2] = '\0';
  
    typestr = strstr(typestr, option);
    return (typestr != NULL);
}

static const mon_cmd_t *monitor_find_command(const char *cmdname)
{
    const mon_cmd_t *cmd;

    for (cmd = mon_cmds; cmd->name != NULL; cmd++) {
        if (compare_cmd(cmdname, cmd->name)) {
            return cmd;
        }
    }

    return NULL;
}

static const mon_cmd_t *monitor_parse_command(Monitor *mon,
                                              const char *cmdline,
                                              QDict *qdict)
{
    const char *p, *typestr;
    int c;
    const mon_cmd_t *cmd;
    char cmdname[256];
    char buf[1024];
    char *key;

#ifdef DEBUG
    monitor_printf(mon, "command='%s'\n", cmdline);
#endif

    /* extract the command name */
    p = get_command_name(cmdline, cmdname, sizeof(cmdname));
    if (!p)
        return NULL;

    cmd = monitor_find_command(cmdname);
    if (!cmd) {
        monitor_printf(mon, "unknown command: '%s'\n", cmdname);
        return NULL;
    }

    /* parse the parameters */
    typestr = cmd->args_type;
    for(;;) {
        typestr = key_get_info(typestr, &key);
        if (!typestr)
            break;
        c = *typestr;
        typestr++;
        switch(c) {
        case 'F':
        case 'B':
        case 's':
            {
                int ret;

                while (qemu_isspace(*p))
                    p++;
                if (*typestr == '?') {
                    typestr++;
                    if (*p == '\0') {
                        /* no optional string: NULL argument */
                        break;
                    }
                }
                ret = get_str(buf, sizeof(buf), &p);
                if (ret < 0) {
                    switch(c) {
                    case 'F':
                        monitor_printf(mon, "%s: filename expected\n",
                                       cmdname);
                        break;
                    case 'B':
                        monitor_printf(mon, "%s: block device name expected\n",
                                       cmdname);
                        break;
                    default:
                        monitor_printf(mon, "%s: string expected\n", cmdname);
                        break;
                    }
                    goto fail;
                }
                qdict_put(qdict, key, qstring_from_str(buf));
            }
            break;
        case '/':
            {
                int count, format, size;

                while (qemu_isspace(*p))
                    p++;
                if (*p == '/') {
                    /* format found */
                    p++;
                    count = 1;
                    if (qemu_isdigit(*p)) {
                        count = 0;
                        while (qemu_isdigit(*p)) {
                            count = count * 10 + (*p - '0');
                            p++;
                        }
                    }
                    size = -1;
                    format = -1;
                    for(;;) {
                        switch(*p) {
                        case 'o':
                        case 'd':
                        case 'u':
                        case 'x':
                        case 'i':
                        case 'c':
                            format = *p++;
                            break;
                        case 'b':
                            size = 1;
                            p++;
                            break;
                        case 'h':
                            size = 2;
                            p++;
                            break;
                        case 'w':
                            size = 4;
                            p++;
                            break;
                        case 'g':
                        case 'L':
                            size = 8;
                            p++;
                            break;
                        default:
                            goto next;
                        }
                    }
                next:
                    if (*p != '\0' && !qemu_isspace(*p)) {
                        monitor_printf(mon, "invalid char in format: '%c'\n",
                                       *p);
                        goto fail;
                    }
                    if (format < 0)
                        format = default_fmt_format;
                    if (format != 'i') {
                        /* for 'i', not specifying a size gives -1 as size */
                        if (size < 0)
                            size = default_fmt_size;
                        default_fmt_size = size;
                    }
                    default_fmt_format = format;
                } else {
                    count = 1;
                    format = default_fmt_format;
                    if (format != 'i') {
                        size = default_fmt_size;
                    } else {
                        size = -1;
                    }
                }
                qdict_put(qdict, "count", qint_from_int(count));
                qdict_put(qdict, "format", qint_from_int(format));
                qdict_put(qdict, "size", qint_from_int(size));
            }
            break;
        case 'i':
        case 'l':
            {
                int64_t val;

                while (qemu_isspace(*p))
                    p++;
                if (*typestr == '?' || *typestr == '.') {
                    if (*typestr == '?') {
                        if (*p == '\0') {
                            typestr++;
                            break;
                        }
                    } else {
                        if (*p == '.') {
                            p++;
                            while (qemu_isspace(*p))
                                p++;
                        } else {
                            typestr++;
                            break;
                        }
                    }
                    typestr++;
                }
                if (get_expr(mon, &val, &p))
                    goto fail;
                /* Check if 'i' is greater than 32-bit */
                if ((c == 'i') && ((val >> 32) & 0xffffffff)) {
                    monitor_printf(mon, "\'%s\' has failed: ", cmdname);
                    monitor_printf(mon, "integer is for 32-bit values\n");
                    goto fail;
                }
                qdict_put(qdict, key, qint_from_int(val));
            }
            break;
        case '-':
            {
                const char *tmp = p;
                int has_option, skip_key = 0;
                /* option */

                c = *typestr++;
                if (c == '\0')
                    goto bad_type;
                while (qemu_isspace(*p))
                    p++;
                has_option = 0;
                if (*p == '-') {
                    p++;
                    if(c != *p) {
                        if(!is_valid_option(p, typestr)) {
                  
                            monitor_printf(mon, "%s: unsupported option -%c\n",
                                           cmdname, *p);
                            goto fail;
                        } else {
                            skip_key = 1;
                        }
                    }
                    if(skip_key) {
                        p = tmp;
                    } else {
                        p++;
                        has_option = 1;
                    }
                }
                qdict_put(qdict, key, qint_from_int(has_option));
            }
            break;
        default:
        bad_type:
            monitor_printf(mon, "%s: unknown type '%c'\n", cmdname, c);
            goto fail;
        }
        qemu_free(key);
        key = NULL;
    }
    /* check that all arguments were parsed */
    while (qemu_isspace(*p))
        p++;
    if (*p != '\0') {
        monitor_printf(mon, "%s: extraneous characters at the end of line\n",
                       cmdname);
        goto fail;
    }

    return cmd;

fail:
    qemu_free(key);
    return NULL;
}

static void monitor_print_error(Monitor *mon)
{
    qerror_print(mon->error);
    QDECREF(mon->error);
    mon->error = NULL;
}

static void monitor_call_handler(Monitor *mon, const mon_cmd_t *cmd,
                                 const QDict *params)
{
    QObject *data = NULL;

    cmd->mhandler.cmd_new(mon, params, &data);

    if (monitor_ctrl_mode(mon)) {
        /* Monitor Protocol */
        monitor_protocol_emitter(mon, data);
    } else {
        /* User Protocol */
         if (data)
            cmd->user_print(mon, data);
    }

    qobject_decref(data);
}

static void handle_user_command(Monitor *mon, const char *cmdline)
{
    QDict *qdict;
    const mon_cmd_t *cmd;

    qdict = qdict_new();

    cmd = monitor_parse_command(mon, cmdline, qdict);
    if (!cmd)
        goto out;

    qemu_errors_to_mon(mon);

    if (monitor_handler_ported(cmd)) {
        monitor_call_handler(mon, cmd, qdict);
    } else {
        cmd->mhandler.cmd(mon, qdict);
    }

    if (monitor_has_error(mon))
        monitor_print_error(mon);

    qemu_errors_to_previous();

out:
    QDECREF(qdict);
}

static void cmd_completion(const char *name, const char *list)
{
    const char *p, *pstart;
    char cmd[128];
    int len;

    p = list;
    for(;;) {
        pstart = p;
        p = strchr(p, '|');
        if (!p)
            p = pstart + strlen(pstart);
        len = p - pstart;
        if (len > sizeof(cmd) - 2)
            len = sizeof(cmd) - 2;
        memcpy(cmd, pstart, len);
        cmd[len] = '\0';
        if (name[0] == '\0' || !strncmp(name, cmd, strlen(name))) {
            readline_add_completion(cur_mon->rs, cmd);
        }
        if (*p == '\0')
            break;
        p++;
    }
}

static void file_completion(const char *input)
{
    DIR *ffs;
    struct dirent *d;
    char path[1024];
    char file[1024], file_prefix[1024];
    int input_path_len;
    const char *p;

    p = strrchr(input, '/');
    if (!p) {
        input_path_len = 0;
        pstrcpy(file_prefix, sizeof(file_prefix), input);
        pstrcpy(path, sizeof(path), ".");
    } else {
        input_path_len = p - input + 1;
        memcpy(path, input, input_path_len);
        if (input_path_len > sizeof(path) - 1)
            input_path_len = sizeof(path) - 1;
        path[input_path_len] = '\0';
        pstrcpy(file_prefix, sizeof(file_prefix), p + 1);
    }
#ifdef DEBUG_COMPLETION
    monitor_printf(cur_mon, "input='%s' path='%s' prefix='%s'\n",
                   input, path, file_prefix);
#endif
    ffs = opendir(path);
    if (!ffs)
        return;
    for(;;) {
        struct stat sb;
        d = readdir(ffs);
        if (!d)
            break;
        if (strstart(d->d_name, file_prefix, NULL)) {
            memcpy(file, input, input_path_len);
            if (input_path_len < sizeof(file))
                pstrcpy(file + input_path_len, sizeof(file) - input_path_len,
                        d->d_name);
            /* stat the file to find out if it's a directory.
             * In that case add a slash to speed up typing long paths
             */
            stat(file, &sb);
            if(S_ISDIR(sb.st_mode))
                pstrcat(file, sizeof(file), "/");
            readline_add_completion(cur_mon->rs, file);
        }
    }
    closedir(ffs);
}

static void block_completion_it(void *opaque, BlockDriverState *bs)
{
    const char *name = bdrv_get_device_name(bs);
    const char *input = opaque;

    if (input[0] == '\0' ||
        !strncmp(name, (char *)input, strlen(input))) {
        readline_add_completion(cur_mon->rs, name);
    }
}

/* NOTE: this parser is an approximate form of the real command parser */
static void parse_cmdline(const char *cmdline,
                         int *pnb_args, char **args)
{
    const char *p;
    int nb_args, ret;
    char buf[1024];

    p = cmdline;
    nb_args = 0;
    for(;;) {
        while (qemu_isspace(*p))
            p++;
        if (*p == '\0')
            break;
        if (nb_args >= MAX_ARGS)
            break;
        ret = get_str(buf, sizeof(buf), &p);
        args[nb_args] = qemu_strdup(buf);
        nb_args++;
        if (ret < 0)
            break;
    }
    *pnb_args = nb_args;
}

static const char *next_arg_type(const char *typestr)
{
    const char *p = strchr(typestr, ':');
    return (p != NULL ? ++p : typestr);
}

static void monitor_find_completion(const char *cmdline)
{
    const char *cmdname;
    char *args[MAX_ARGS];
    int nb_args, i, len;
    const char *ptype, *str;
    const mon_cmd_t *cmd;
    const KeyDef *key;

    parse_cmdline(cmdline, &nb_args, args);
#ifdef DEBUG_COMPLETION
    for(i = 0; i < nb_args; i++) {
        monitor_printf(cur_mon, "arg%d = '%s'\n", i, (char *)args[i]);
    }
#endif

    /* if the line ends with a space, it means we want to complete the
       next arg */
    len = strlen(cmdline);
    if (len > 0 && qemu_isspace(cmdline[len - 1])) {
        if (nb_args >= MAX_ARGS)
            return;
        args[nb_args++] = qemu_strdup("");
    }
    if (nb_args <= 1) {
        /* command completion */
        if (nb_args == 0)
            cmdname = "";
        else
            cmdname = args[0];
        readline_set_completion_index(cur_mon->rs, strlen(cmdname));
        for(cmd = mon_cmds; cmd->name != NULL; cmd++) {
            cmd_completion(cmdname, cmd->name);
        }
    } else {
        /* find the command */
        for(cmd = mon_cmds; cmd->name != NULL; cmd++) {
            if (compare_cmd(args[0], cmd->name))
                goto found;
        }
        return;
    found:
        ptype = next_arg_type(cmd->args_type);
        for(i = 0; i < nb_args - 2; i++) {
            if (*ptype != '\0') {
                ptype = next_arg_type(ptype);
                while (*ptype == '?')
                    ptype = next_arg_type(ptype);
            }
        }
        str = args[nb_args - 1];
        if (*ptype == '-' && ptype[1] != '\0') {
            ptype += 2;
        }
        switch(*ptype) {
        case 'F':
            /* file completion */
            readline_set_completion_index(cur_mon->rs, strlen(str));
            file_completion(str);
            break;
        case 'B':
            /* block device name completion */
            readline_set_completion_index(cur_mon->rs, strlen(str));
            bdrv_iterate(block_completion_it, (void *)str);
            break;
        case 's':
            /* XXX: more generic ? */
            if (!strcmp(cmd->name, "info")) {
                readline_set_completion_index(cur_mon->rs, strlen(str));
                for(cmd = info_cmds; cmd->name != NULL; cmd++) {
                    cmd_completion(str, cmd->name);
                }
            } else if (!strcmp(cmd->name, "sendkey")) {
                char *sep = strrchr(str, '-');
                if (sep)
                    str = sep + 1;
                readline_set_completion_index(cur_mon->rs, strlen(str));
                for(key = key_defs; key->name != NULL; key++) {
                    cmd_completion(str, key->name);
                }
            } else if (!strcmp(cmd->name, "help|?")) {
                readline_set_completion_index(cur_mon->rs, strlen(str));
                for (cmd = mon_cmds; cmd->name != NULL; cmd++) {
                    cmd_completion(str, cmd->name);
                }
            }
            break;
        default:
            break;
        }
    }
    for(i = 0; i < nb_args; i++)
        qemu_free(args[i]);
}

static int monitor_can_read(void *opaque)
{
    Monitor *mon = opaque;

    return (mon->suspend_cnt == 0) ? 128 : 0;
}

typedef struct CmdArgs {
    QString *name;
    int type;
    int flag;
    int optional;
} CmdArgs;

static int check_opt(const CmdArgs *cmd_args, const char *name, QDict *args)
{
    if (!cmd_args->optional) {
        qemu_error_new(QERR_MISSING_PARAMETER, name);
        return -1;
    }

    if (cmd_args->type == '-') {
        /* handlers expect a value, they need to be changed */
        qdict_put(args, name, qint_from_int(0));
    }

    return 0;
}

static int check_arg(const CmdArgs *cmd_args, QDict *args)
{
    QObject *value;
    const char *name;

    name = qstring_get_str(cmd_args->name);

    if (!args) {
        return check_opt(cmd_args, name, args);
    }

    value = qdict_get(args, name);
    if (!value) {
        return check_opt(cmd_args, name, args);
    }

    switch (cmd_args->type) {
        case 'F':
        case 'B':
        case 's':
            if (qobject_type(value) != QTYPE_QSTRING) {
                qemu_error_new(QERR_INVALID_PARAMETER_TYPE, name, "string");
                return -1;
            }
            break;
        case '/': {
            int i;
            const char *keys[] = { "count", "format", "size", NULL };

            for (i = 0; keys[i]; i++) {
                QObject *obj = qdict_get(args, keys[i]);
                if (!obj) {
                    qemu_error_new(QERR_MISSING_PARAMETER, name);
                    return -1;
                }
                if (qobject_type(obj) != QTYPE_QINT) {
                    qemu_error_new(QERR_INVALID_PARAMETER_TYPE, name, "int");
                    return -1;
                }
            }
            break;
        }
        case 'i':
        case 'l':
            if (qobject_type(value) != QTYPE_QINT) {
                qemu_error_new(QERR_INVALID_PARAMETER_TYPE, name, "int");
                return -1;
            }
            break;
        case '-':
            if (qobject_type(value) != QTYPE_QINT &&
                qobject_type(value) != QTYPE_QBOOL) {
                qemu_error_new(QERR_INVALID_PARAMETER_TYPE, name, "bool");
                return -1;
            }
            if (qobject_type(value) == QTYPE_QBOOL) {
                /* handlers expect a QInt, they need to be changed */
                qdict_put(args, name,
                         qint_from_int(qbool_get_int(qobject_to_qbool(value))));
            }
            break;
        default:
            /* impossible */
            abort();
    }

    return 0;
}

static void cmd_args_init(CmdArgs *cmd_args)
{
    cmd_args->name = qstring_new();
    cmd_args->type = cmd_args->flag = cmd_args->optional = 0;
}

/*
 * This is not trivial, we have to parse Monitor command's argument
 * type syntax to be able to check the arguments provided by clients.
 *
 * In the near future we will be using an array for that and will be
 * able to drop all this parsing...
 */
static int monitor_check_qmp_args(const mon_cmd_t *cmd, QDict *args)
{
    int err;
    const char *p;
    CmdArgs cmd_args;

    if (cmd->args_type == NULL) {
        return (qdict_size(args) == 0 ? 0 : -1);
    }

    err = 0;
    cmd_args_init(&cmd_args);

    for (p = cmd->args_type;; p++) {
        if (*p == ':') {
            cmd_args.type = *++p;
            p++;
            if (cmd_args.type == '-') {
                cmd_args.flag = *p++;
                cmd_args.optional = 1;
            } else if (*p == '?') {
                cmd_args.optional = 1;
                p++;
            }

            assert(*p == ',' || *p == '\0');
            err = check_arg(&cmd_args, args);

            QDECREF(cmd_args.name);
            cmd_args_init(&cmd_args);

            if (err < 0) {
                break;
            }
        } else {
            qstring_append_chr(cmd_args.name, *p);
        }

        if (*p == '\0') {
            break;
        }
    }

    QDECREF(cmd_args.name);
    return err;
}

static void handle_qmp_command(JSONMessageParser *parser, QList *tokens)
{
    int err;
    QObject *obj;
    QDict *input, *args;
    const mon_cmd_t *cmd;
    Monitor *mon = cur_mon;
    const char *cmd_name, *info_item;

    args = NULL;
    qemu_errors_to_mon(mon);

    obj = json_parser_parse(tokens, NULL);
    if (!obj) {
        // FIXME: should be triggered in json_parser_parse()
        qemu_error_new(QERR_JSON_PARSING);
        goto err_out;
    } else if (qobject_type(obj) != QTYPE_QDICT) {
        qemu_error_new(QERR_QMP_BAD_INPUT_OBJECT, "object");
        qobject_decref(obj);
        goto err_out;
    }

    input = qobject_to_qdict(obj);

    mon->mc->id = qdict_get(input, "id");
    qobject_incref(mon->mc->id);

    obj = qdict_get(input, "execute");
    if (!obj) {
        qemu_error_new(QERR_QMP_BAD_INPUT_OBJECT, "execute");
        goto err_input;
    } else if (qobject_type(obj) != QTYPE_QSTRING) {
        qemu_error_new(QERR_QMP_BAD_INPUT_OBJECT, "string");
        goto err_input;
    }

    cmd_name = qstring_get_str(qobject_to_qstring(obj));

    /*
     * XXX: We need this special case until we get info handlers
     * converted into 'query-' commands
     */
    if (compare_cmd(cmd_name, "info")) {
        qemu_error_new(QERR_COMMAND_NOT_FOUND, cmd_name);
        goto err_input;
    } else if (strstart(cmd_name, "query-", &info_item)) {
        cmd = monitor_find_command("info");
        qdict_put_obj(input, "arguments",
                      qobject_from_jsonf("{ 'item': %s }", info_item));
    } else {
        cmd = monitor_find_command(cmd_name);
        if (!cmd) {
            qemu_error_new(QERR_COMMAND_NOT_FOUND, cmd_name);
            goto err_input;
        }
    }

    obj = qdict_get(input, "arguments");
    if (!obj) {
        args = qdict_new();
    } else {
        args = qobject_to_qdict(obj);
        QINCREF(args);
    }

    QDECREF(input);

    err = monitor_check_qmp_args(cmd, args);
    if (err < 0) {
        goto err_out;
    }

    monitor_call_handler(mon, cmd, args);
    goto out;

err_input:
    QDECREF(input);
err_out:
    monitor_protocol_emitter(mon, NULL);
out:
    QDECREF(args);
    qemu_errors_to_previous();
}

/**
 * monitor_control_read(): Read and handle QMP input
 */
static void monitor_control_read(void *opaque, const uint8_t *buf, int size)
{
    Monitor *old_mon = cur_mon;

    cur_mon = opaque;

    json_message_parser_feed(&cur_mon->mc->parser, (const char *) buf, size);

    cur_mon = old_mon;
}

static void monitor_read(void *opaque, const uint8_t *buf, int size)
{
    Monitor *old_mon = cur_mon;
    int i;

    cur_mon = opaque;

    if (cur_mon->rs) {
        for (i = 0; i < size; i++)
            readline_handle_byte(cur_mon->rs, buf[i]);
    } else {
        if (size == 0 || buf[size - 1] != 0)
            monitor_printf(cur_mon, "corrupted command\n");
        else
            handle_user_command(cur_mon, (char *)buf);
    }

    cur_mon = old_mon;
}

static void monitor_command_cb(Monitor *mon, const char *cmdline, void *opaque)
{
    monitor_suspend(mon);
    handle_user_command(mon, cmdline);
    monitor_resume(mon);
}

int monitor_suspend(Monitor *mon)
{
    if (!mon->rs)
        return -ENOTTY;
    mon->suspend_cnt++;
    return 0;
}

void monitor_resume(Monitor *mon)
{
    if (!mon->rs)
        return;
    if (--mon->suspend_cnt == 0)
        readline_show_prompt(mon->rs);
}

/**
 * monitor_control_event(): Print QMP gretting
 */
static void monitor_control_event(void *opaque, int event)
{
    if (event == CHR_EVENT_OPENED) {
        QObject *data;
        Monitor *mon = opaque;

        json_message_parser_init(&mon->mc->parser, handle_qmp_command);

        data = qobject_from_jsonf("{ 'QMP': { 'capabilities': [] } }");
        assert(data != NULL);

        monitor_json_emitter(mon, data);
        qobject_decref(data);
    }
}

static void monitor_event(void *opaque, int event)
{
    Monitor *mon = opaque;

    switch (event) {
    case CHR_EVENT_MUX_IN:
        mon->mux_out = 0;
        if (mon->reset_seen) {
            readline_restart(mon->rs);
            monitor_resume(mon);
            monitor_flush(mon);
        } else {
            mon->suspend_cnt = 0;
        }
        break;

    case CHR_EVENT_MUX_OUT:
        if (mon->reset_seen) {
            if (mon->suspend_cnt == 0) {
                monitor_printf(mon, "\n");
            }
            monitor_flush(mon);
            monitor_suspend(mon);
        } else {
            mon->suspend_cnt++;
        }
        mon->mux_out = 1;
        break;

    case CHR_EVENT_OPENED:
        monitor_printf(mon, "QEMU %s monitor - type 'help' for more "
                       "information\n", QEMU_VERSION);
        if (!mon->mux_out) {
            readline_show_prompt(mon->rs);
        }
        mon->reset_seen = 1;
        break;
    }
}


/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 8
 * End:
 */

void monitor_init(CharDriverState *chr, int flags)
{
    static int is_first_init = 1;
    Monitor *mon;

    if (is_first_init) {
        key_timer = qemu_new_timer(vm_clock, release_keys, NULL);
        is_first_init = 0;
    }

    mon = qemu_mallocz(sizeof(*mon));

    mon->chr = chr;
    mon->flags = flags;
    if (flags & MONITOR_USE_READLINE) {
        mon->rs = readline_init(mon, monitor_find_completion);
        monitor_read_command(mon, 0);
    }

    if (monitor_ctrl_mode(mon)) {
        mon->mc = qemu_mallocz(sizeof(MonitorControl));
        /* Control mode requires special handlers */
        qemu_chr_add_handlers(chr, monitor_can_read, monitor_control_read,
                              monitor_control_event, mon);
    } else {
        qemu_chr_add_handlers(chr, monitor_can_read, monitor_read,
                              monitor_event, mon);
    }

    QLIST_INSERT_HEAD(&mon_list, mon, entry);
    if (!cur_mon || (flags & MONITOR_IS_DEFAULT))
        cur_mon = mon;
}

static void bdrv_password_cb(Monitor *mon, const char *password, void *opaque)
{
    BlockDriverState *bs = opaque;
    int ret = 0;

    if (bdrv_set_key(bs, password) != 0) {
        monitor_printf(mon, "invalid password\n");
        ret = -EPERM;
    }
    if (mon->password_completion_cb)
        mon->password_completion_cb(mon->password_opaque, ret);

    monitor_read_command(mon, 1);
}

void monitor_read_bdrv_key_start(Monitor *mon, BlockDriverState *bs,
                                 BlockDriverCompletionFunc *completion_cb,
                                 void *opaque)
{
    int err;

    if (!bdrv_key_required(bs)) {
        if (completion_cb)
            completion_cb(opaque, 0);
        return;
    }

    if (monitor_ctrl_mode(mon)) {
        qemu_error_new(QERR_DEVICE_ENCRYPTED, bdrv_get_device_name(bs));
        return;
    }

    monitor_printf(mon, "%s (%s) is encrypted.\n", bdrv_get_device_name(bs),
                   bdrv_get_encrypted_filename(bs));

    mon->password_completion_cb = completion_cb;
    mon->password_opaque = opaque;

    err = monitor_read_password(mon, bdrv_password_cb, bs);

    if (err && completion_cb)
        completion_cb(opaque, err);
}

typedef struct QemuErrorSink QemuErrorSink;
struct QemuErrorSink {
    enum {
        ERR_SINK_FILE,
        ERR_SINK_MONITOR,
    } dest;
    union {
        FILE    *fp;
        Monitor *mon;
    };
    QemuErrorSink *previous;
};

static QemuErrorSink *qemu_error_sink;

void qemu_errors_to_file(FILE *fp)
{
    QemuErrorSink *sink;

    sink = qemu_mallocz(sizeof(*sink));
    sink->dest = ERR_SINK_FILE;
    sink->fp = fp;
    sink->previous = qemu_error_sink;
    qemu_error_sink = sink;
}

void qemu_errors_to_mon(Monitor *mon)
{
    QemuErrorSink *sink;

    sink = qemu_mallocz(sizeof(*sink));
    sink->dest = ERR_SINK_MONITOR;
    sink->mon = mon;
    sink->previous = qemu_error_sink;
    qemu_error_sink = sink;
}

void qemu_errors_to_previous(void)
{
    QemuErrorSink *sink;

    assert(qemu_error_sink != NULL);
    sink = qemu_error_sink;
    qemu_error_sink = sink->previous;
    qemu_free(sink);
}

void qemu_error(const char *fmt, ...)
{
    va_list args;

    assert(qemu_error_sink != NULL);
    switch (qemu_error_sink->dest) {
    case ERR_SINK_FILE:
        va_start(args, fmt);
        vfprintf(qemu_error_sink->fp, fmt, args);
        va_end(args);
        break;
    case ERR_SINK_MONITOR:
        va_start(args, fmt);
        monitor_vprintf(qemu_error_sink->mon, fmt, args);
        va_end(args);
        break;
    }
}

void qemu_error_internal(const char *file, int linenr, const char *func,
                         const char *fmt, ...)
{
    va_list va;
    QError *qerror;

    assert(qemu_error_sink != NULL);

    va_start(va, fmt);
    qerror = qerror_from_info(file, linenr, func, fmt, &va);
    va_end(va);

    switch (qemu_error_sink->dest) {
    case ERR_SINK_FILE:
        qerror_print(qerror);
        QDECREF(qerror);
        break;
    case ERR_SINK_MONITOR:
        assert(qemu_error_sink->mon->error == NULL);
        qemu_error_sink->mon->error = qerror;
        break;
    }
}
