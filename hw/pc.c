/*
 * QEMU PC System Emulator
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
#include "hw.h"
#include "pc.h"
#include "fdc.h"
#include "pci.h"
#include "vmware_vga.h"
#include "usb-uhci.h"
#include "usb-ohci.h"
#include "prep_pci.h"
#include "apb_pci.h"
#include "block.h"
#include "sysemu.h"
#include "audio/audio.h"
#include "net.h"
#include "smbus.h"
#include "boards.h"
#include "monitor.h"
#include "fw_cfg.h"
#include "hpet_emul.h"
#include "watchdog.h"
#include "smbios.h"
#include "ide.h"
#include "loader.h"
#include "elf.h"

/* output Bochs bios info messages */
//#define DEBUG_BIOS

/* Show multiboot debug output */
//#define DEBUG_MULTIBOOT

#define BIOS_FILENAME "bios.bin"

#define PC_MAX_BIOS_SIZE (4 * 1024 * 1024)

/* Leave a chunk of memory at the top of RAM for the BIOS ACPI tables.  */
#define ACPI_DATA_SIZE       0x10000
#define BIOS_CFG_IOPORT 0x510
#define FW_CFG_ACPI_TABLES (FW_CFG_ARCH_LOCAL + 0)
#define FW_CFG_SMBIOS_ENTRIES (FW_CFG_ARCH_LOCAL + 1)
#define FW_CFG_IRQ0_OVERRIDE (FW_CFG_ARCH_LOCAL + 2)

#define MAX_IDE_BUS 2

static fdctrl_t *floppy_controller;
static RTCState *rtc_state;
static PITState *pit;
static PCII440FXState *i440fx_state;

typedef struct isa_irq_state {
    qemu_irq *i8259;
    qemu_irq *ioapic;
} IsaIrqState;

static void isa_irq_handler(void *opaque, int n, int level)
{
    IsaIrqState *isa = (IsaIrqState *)opaque;

    if (n < 16) {
        qemu_set_irq(isa->i8259[n], level);
    }
    if (isa->ioapic)
        qemu_set_irq(isa->ioapic[n], level);
};

static void ioport80_write(void *opaque, uint32_t addr, uint32_t data)
{
}

/* MSDOS compatibility mode FPU exception support */
static qemu_irq ferr_irq;
/* XXX: add IGNNE support */
void cpu_set_ferr(CPUX86State *s)
{
    qemu_irq_raise(ferr_irq);
}

static void ioportF0_write(void *opaque, uint32_t addr, uint32_t data)
{
    qemu_irq_lower(ferr_irq);
}

/* TSC handling */
uint64_t cpu_get_tsc(CPUX86State *env)
{
    return cpu_get_ticks();
}

/* SMM support */
void cpu_smm_update(CPUState *env)
{
    if (i440fx_state && env == first_cpu)
        i440fx_set_smm(i440fx_state, (env->hflags >> HF_SMM_SHIFT) & 1);
}


/* IRQ handling */
int cpu_get_pic_interrupt(CPUState *env)
{
    int intno;

    intno = apic_get_interrupt(env);
    if (intno >= 0) {
        /* set irq request if a PIC irq is still pending */
        /* XXX: improve that */
        pic_update_irq(isa_pic);
        return intno;
    }
    /* read the irq from the PIC */
    if (!apic_accept_pic_intr(env))
        return -1;

    intno = pic_read_irq(isa_pic);
    return intno;
}

static void pic_irq_request(void *opaque, int irq, int level)
{
    CPUState *env = first_cpu;

    if (env->apic_state) {
        while (env) {
            if (apic_accept_pic_intr(env))
                apic_deliver_pic_intr(env, level);
            env = env->next_cpu;
        }
    } else {
        if (level)
            cpu_interrupt(env, CPU_INTERRUPT_HARD);
        else
            cpu_reset_interrupt(env, CPU_INTERRUPT_HARD);
    }
}

/* PC cmos mappings */

#define REG_EQUIPMENT_BYTE          0x14

static int cmos_get_fd_drive_type(int fd0)
{
    int val;

    switch (fd0) {
    case 0:
        /* 1.44 Mb 3"5 drive */
        val = 4;
        break;
    case 1:
        /* 2.88 Mb 3"5 drive */
        val = 5;
        break;
    case 2:
        /* 1.2 Mb 5"5 drive */
        val = 2;
        break;
    default:
        val = 0;
        break;
    }
    return val;
}

static void cmos_init_hd(int type_ofs, int info_ofs, BlockDriverState *hd)
{
    RTCState *s = rtc_state;
    int cylinders, heads, sectors;
    bdrv_get_geometry_hint(hd, &cylinders, &heads, &sectors);
    rtc_set_memory(s, type_ofs, 47);
    rtc_set_memory(s, info_ofs, cylinders);
    rtc_set_memory(s, info_ofs + 1, cylinders >> 8);
    rtc_set_memory(s, info_ofs + 2, heads);
    rtc_set_memory(s, info_ofs + 3, 0xff);
    rtc_set_memory(s, info_ofs + 4, 0xff);
    rtc_set_memory(s, info_ofs + 5, 0xc0 | ((heads > 8) << 3));
    rtc_set_memory(s, info_ofs + 6, cylinders);
    rtc_set_memory(s, info_ofs + 7, cylinders >> 8);
    rtc_set_memory(s, info_ofs + 8, sectors);
}

/* convert boot_device letter to something recognizable by the bios */
static int boot_device2nibble(char boot_device)
{
    switch(boot_device) {
    case 'a':
    case 'b':
        return 0x01; /* floppy boot */
    case 'c':
        return 0x02; /* hard drive boot */
    case 'd':
        return 0x03; /* CD-ROM boot */
    case 'n':
        return 0x04; /* Network boot */
    }
    return 0;
}

/* copy/pasted from cmos_init, should be made a general function
 and used there as well */
static int pc_boot_set(void *opaque, const char *boot_device)
{
    Monitor *mon = cur_mon;
#define PC_MAX_BOOT_DEVICES 3
    RTCState *s = (RTCState *)opaque;
    int nbds, bds[3] = { 0, };
    int i;

    nbds = strlen(boot_device);
    if (nbds > PC_MAX_BOOT_DEVICES) {
        monitor_printf(mon, "Too many boot devices for PC\n");
        return(1);
    }
    for (i = 0; i < nbds; i++) {
        bds[i] = boot_device2nibble(boot_device[i]);
        if (bds[i] == 0) {
            monitor_printf(mon, "Invalid boot device for PC: '%c'\n",
                           boot_device[i]);
            return(1);
        }
    }
    rtc_set_memory(s, 0x3d, (bds[1] << 4) | bds[0]);
    rtc_set_memory(s, 0x38, (bds[2] << 4));
    return(0);
}

/* hd_table must contain 4 block drivers */
static void cmos_init(ram_addr_t ram_size, ram_addr_t above_4g_mem_size,
                      const char *boot_device, DriveInfo **hd_table)
{
    RTCState *s = rtc_state;
    int nbds, bds[3] = { 0, };
    int val;
    int fd0, fd1, nb;
    int i;

    /* various important CMOS locations needed by PC/Bochs bios */

    /* memory size */
    val = 640; /* base memory in K */
    rtc_set_memory(s, 0x15, val);
    rtc_set_memory(s, 0x16, val >> 8);

    val = (ram_size / 1024) - 1024;
    if (val > 65535)
        val = 65535;
    rtc_set_memory(s, 0x17, val);
    rtc_set_memory(s, 0x18, val >> 8);
    rtc_set_memory(s, 0x30, val);
    rtc_set_memory(s, 0x31, val >> 8);

    if (above_4g_mem_size) {
        rtc_set_memory(s, 0x5b, (unsigned int)above_4g_mem_size >> 16);
        rtc_set_memory(s, 0x5c, (unsigned int)above_4g_mem_size >> 24);
        rtc_set_memory(s, 0x5d, (uint64_t)above_4g_mem_size >> 32);
    }

    if (ram_size > (16 * 1024 * 1024))
        val = (ram_size / 65536) - ((16 * 1024 * 1024) / 65536);
    else
        val = 0;
    if (val > 65535)
        val = 65535;
    rtc_set_memory(s, 0x34, val);
    rtc_set_memory(s, 0x35, val >> 8);

    /* set the number of CPU */
    rtc_set_memory(s, 0x5f, smp_cpus - 1);

    /* set boot devices, and disable floppy signature check if requested */
#define PC_MAX_BOOT_DEVICES 3
    nbds = strlen(boot_device);
    if (nbds > PC_MAX_BOOT_DEVICES) {
        fprintf(stderr, "Too many boot devices for PC\n");
        exit(1);
    }
    for (i = 0; i < nbds; i++) {
        bds[i] = boot_device2nibble(boot_device[i]);
        if (bds[i] == 0) {
            fprintf(stderr, "Invalid boot device for PC: '%c'\n",
                    boot_device[i]);
            exit(1);
        }
    }
    rtc_set_memory(s, 0x3d, (bds[1] << 4) | bds[0]);
    rtc_set_memory(s, 0x38, (bds[2] << 4) | (fd_bootchk ?  0x0 : 0x1));

    /* floppy type */

    fd0 = fdctrl_get_drive_type(floppy_controller, 0);
    fd1 = fdctrl_get_drive_type(floppy_controller, 1);

    val = (cmos_get_fd_drive_type(fd0) << 4) | cmos_get_fd_drive_type(fd1);
    rtc_set_memory(s, 0x10, val);

    val = 0;
    nb = 0;
    if (fd0 < 3)
        nb++;
    if (fd1 < 3)
        nb++;
    switch (nb) {
    case 0:
        break;
    case 1:
        val |= 0x01; /* 1 drive, ready for boot */
        break;
    case 2:
        val |= 0x41; /* 2 drives, ready for boot */
        break;
    }
    val |= 0x02; /* FPU is there */
    val |= 0x04; /* PS/2 mouse installed */
    rtc_set_memory(s, REG_EQUIPMENT_BYTE, val);

    /* hard drives */

    rtc_set_memory(s, 0x12, (hd_table[0] ? 0xf0 : 0) | (hd_table[1] ? 0x0f : 0));
    if (hd_table[0])
        cmos_init_hd(0x19, 0x1b, hd_table[0]->bdrv);
    if (hd_table[1])
        cmos_init_hd(0x1a, 0x24, hd_table[1]->bdrv);

    val = 0;
    for (i = 0; i < 4; i++) {
        if (hd_table[i]) {
            int cylinders, heads, sectors, translation;
            /* NOTE: bdrv_get_geometry_hint() returns the physical
                geometry.  It is always such that: 1 <= sects <= 63, 1
                <= heads <= 16, 1 <= cylinders <= 16383. The BIOS
                geometry can be different if a translation is done. */
            translation = bdrv_get_translation_hint(hd_table[i]->bdrv);
            if (translation == BIOS_ATA_TRANSLATION_AUTO) {
                bdrv_get_geometry_hint(hd_table[i]->bdrv, &cylinders, &heads, &sectors);
                if (cylinders <= 1024 && heads <= 16 && sectors <= 63) {
                    /* No translation. */
                    translation = 0;
                } else {
                    /* LBA translation. */
                    translation = 1;
                }
            } else {
                translation--;
            }
            val |= translation << (i * 2);
        }
    }
    rtc_set_memory(s, 0x39, val);
}

void ioport_set_a20(int enable)
{
    /* XXX: send to all CPUs ? */
    cpu_x86_set_a20(first_cpu, enable);
}

int ioport_get_a20(void)
{
    return ((first_cpu->a20_mask >> 20) & 1);
}

static void ioport92_write(void *opaque, uint32_t addr, uint32_t val)
{
    ioport_set_a20((val >> 1) & 1);
    /* XXX: bit 0 is fast reset */
}

static uint32_t ioport92_read(void *opaque, uint32_t addr)
{
    return ioport_get_a20() << 1;
}

/***********************************************************/
/* Bochs BIOS debug ports */

static void bochs_bios_write(void *opaque, uint32_t addr, uint32_t val)
{
    static const char shutdown_str[8] = "Shutdown";
    static int shutdown_index = 0;

    switch(addr) {
        /* Bochs BIOS messages */
    case 0x400:
    case 0x401:
        fprintf(stderr, "BIOS panic at rombios.c, line %d\n", val);
        exit(1);
    case 0x402:
    case 0x403:
#ifdef DEBUG_BIOS
        fprintf(stderr, "%c", val);
#endif
        break;
    case 0x8900:
        /* same as Bochs power off */
        if (val == shutdown_str[shutdown_index]) {
            shutdown_index++;
            if (shutdown_index == 8) {
                shutdown_index = 0;
                qemu_system_shutdown_request();
            }
        } else {
            shutdown_index = 0;
        }
        break;

        /* LGPL'ed VGA BIOS messages */
    case 0x501:
    case 0x502:
        fprintf(stderr, "VGA BIOS panic, line %d\n", val);
        exit(1);
    case 0x500:
    case 0x503:
#ifdef DEBUG_BIOS
        fprintf(stderr, "%c", val);
#endif
        break;
    }
}

static void *bochs_bios_init(void)
{
    void *fw_cfg;
    uint8_t *smbios_table;
    size_t smbios_len;
    uint64_t *numa_fw_cfg;
    int i, j;

    register_ioport_write(0x400, 1, 2, bochs_bios_write, NULL);
    register_ioport_write(0x401, 1, 2, bochs_bios_write, NULL);
    register_ioport_write(0x402, 1, 1, bochs_bios_write, NULL);
    register_ioport_write(0x403, 1, 1, bochs_bios_write, NULL);
    register_ioport_write(0x8900, 1, 1, bochs_bios_write, NULL);

    register_ioport_write(0x501, 1, 2, bochs_bios_write, NULL);
    register_ioport_write(0x502, 1, 2, bochs_bios_write, NULL);
    register_ioport_write(0x500, 1, 1, bochs_bios_write, NULL);
    register_ioport_write(0x503, 1, 1, bochs_bios_write, NULL);

    fw_cfg = fw_cfg_init(BIOS_CFG_IOPORT, BIOS_CFG_IOPORT + 1, 0, 0);

    fw_cfg_add_i32(fw_cfg, FW_CFG_ID, 1);
    fw_cfg_add_i64(fw_cfg, FW_CFG_RAM_SIZE, (uint64_t)ram_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_ACPI_TABLES, (uint8_t *)acpi_tables,
                     acpi_tables_len);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_IRQ0_OVERRIDE, &irq0override, 1);

    smbios_table = smbios_get_table(&smbios_len);
    if (smbios_table)
        fw_cfg_add_bytes(fw_cfg, FW_CFG_SMBIOS_ENTRIES,
                         smbios_table, smbios_len);

    /* allocate memory for the NUMA channel: one (64bit) word for the number
     * of nodes, one word for each VCPU->node and one word for each node to
     * hold the amount of memory.
     */
    numa_fw_cfg = qemu_mallocz((1 + smp_cpus + nb_numa_nodes) * 8);
    numa_fw_cfg[0] = cpu_to_le64(nb_numa_nodes);
    for (i = 0; i < smp_cpus; i++) {
        for (j = 0; j < nb_numa_nodes; j++) {
            if (node_cpumask[j] & (1 << i)) {
                numa_fw_cfg[i + 1] = cpu_to_le64(j);
                break;
            }
        }
    }
    for (i = 0; i < nb_numa_nodes; i++) {
        numa_fw_cfg[smp_cpus + 1 + i] = cpu_to_le64(node_mem[i]);
    }
    fw_cfg_add_bytes(fw_cfg, FW_CFG_NUMA, (uint8_t *)numa_fw_cfg,
                     (1 + smp_cpus + nb_numa_nodes) * 8);

    return fw_cfg;
}

static long get_file_size(FILE *f)
{
    long where, size;

    /* XXX: on Unix systems, using fstat() probably makes more sense */

    where = ftell(f);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, where, SEEK_SET);

    return size;
}

#define MULTIBOOT_STRUCT_ADDR 0x9000

#if MULTIBOOT_STRUCT_ADDR > 0xf0000
#error multiboot struct needs to fit in 16 bit real mode
#endif

static int load_multiboot(void *fw_cfg,
                          FILE *f,
                          const char *kernel_filename,
                          const char *initrd_filename,
                          const char *kernel_cmdline,
                          uint8_t *header)
{
    int i, is_multiboot = 0;
    uint32_t flags = 0;
    uint32_t mh_entry_addr;
    uint32_t mh_load_addr;
    uint32_t mb_kernel_size;
    uint32_t mmap_addr = MULTIBOOT_STRUCT_ADDR;
    uint32_t mb_bootinfo = MULTIBOOT_STRUCT_ADDR + 0x500;
    uint32_t mb_mod_end;
    uint8_t bootinfo[0x500];
    uint32_t cmdline = 0x200;
    uint8_t *mb_kernel_data;
    uint8_t *mb_bootinfo_data;

    /* Ok, let's see if it is a multiboot image.
       The header is 12x32bit long, so the latest entry may be 8192 - 48. */
    for (i = 0; i < (8192 - 48); i += 4) {
        if (ldl_p(header+i) == 0x1BADB002) {
            uint32_t checksum = ldl_p(header+i+8);
            flags = ldl_p(header+i+4);
            checksum += flags;
            checksum += (uint32_t)0x1BADB002;
            if (!checksum) {
                is_multiboot = 1;
                break;
            }
        }
    }

    if (!is_multiboot)
        return 0; /* no multiboot */

#ifdef DEBUG_MULTIBOOT
    fprintf(stderr, "qemu: I believe we found a multiboot image!\n");
#endif
    memset(bootinfo, 0, sizeof(bootinfo));

    if (flags & 0x00000004) { /* MULTIBOOT_HEADER_HAS_VBE */
        fprintf(stderr, "qemu: multiboot knows VBE. we don't.\n");
    }
    if (!(flags & 0x00010000)) { /* MULTIBOOT_HEADER_HAS_ADDR */
        uint64_t elf_entry;
        uint64_t elf_low, elf_high;
        int kernel_size;
        fclose(f);
        kernel_size = load_elf(kernel_filename, 0, &elf_entry, &elf_low, &elf_high,
                               0, ELF_MACHINE, 0);
        if (kernel_size < 0) {
            fprintf(stderr, "Error while loading elf kernel\n");
            exit(1);
        }
        mh_load_addr = elf_low;
        mb_kernel_size = elf_high - elf_low;
        mh_entry_addr = elf_entry;

        mb_kernel_data = qemu_malloc(mb_kernel_size);
        if (rom_copy(mb_kernel_data, mh_load_addr, mb_kernel_size) != mb_kernel_size) {
            fprintf(stderr, "Error while fetching elf kernel from rom\n");
            exit(1);
        }

#ifdef DEBUG_MULTIBOOT
        fprintf(stderr, "qemu: loading multiboot-elf kernel (%#x bytes) with entry %#zx\n",
                mb_kernel_size, (size_t)mh_entry_addr);
#endif
    } else {
        /* Valid if mh_flags sets MULTIBOOT_HEADER_HAS_ADDR. */
        uint32_t mh_header_addr = ldl_p(header+i+12);
        mh_load_addr = ldl_p(header+i+16);
#ifdef DEBUG_MULTIBOOT
        uint32_t mh_load_end_addr = ldl_p(header+i+20);
        uint32_t mh_bss_end_addr = ldl_p(header+i+24);
#endif
        uint32_t mb_kernel_text_offset = i - (mh_header_addr - mh_load_addr);

        mh_entry_addr = ldl_p(header+i+28);
        mb_kernel_size = get_file_size(f) - mb_kernel_text_offset;

        /* Valid if mh_flags sets MULTIBOOT_HEADER_HAS_VBE.
        uint32_t mh_mode_type = ldl_p(header+i+32);
        uint32_t mh_width = ldl_p(header+i+36);
        uint32_t mh_height = ldl_p(header+i+40);
        uint32_t mh_depth = ldl_p(header+i+44); */

#ifdef DEBUG_MULTIBOOT
        fprintf(stderr, "multiboot: mh_header_addr = %#x\n", mh_header_addr);
        fprintf(stderr, "multiboot: mh_load_addr = %#x\n", mh_load_addr);
        fprintf(stderr, "multiboot: mh_load_end_addr = %#x\n", mh_load_end_addr);
        fprintf(stderr, "multiboot: mh_bss_end_addr = %#x\n", mh_bss_end_addr);
        fprintf(stderr, "qemu: loading multiboot kernel (%#x bytes) at %#x\n",
                mb_kernel_size, mh_load_addr);
#endif

        mb_kernel_data = qemu_malloc(mb_kernel_size);
        fseek(f, mb_kernel_text_offset, SEEK_SET);
        fread(mb_kernel_data, 1, mb_kernel_size, f);
        fclose(f);
    }

    /* blob size is only the kernel for now */
    mb_mod_end = mh_load_addr + mb_kernel_size;

    /* load modules */
    stl_p(bootinfo + 20, 0x0); /* mods_count */
    if (initrd_filename) {
        uint32_t mb_mod_info = 0x100;
        uint32_t mb_mod_cmdline = 0x300;
        uint32_t mb_mod_start = mh_load_addr;
        uint32_t mb_mod_length = mb_kernel_size;
        char *next_initrd;
        char *next_space;
        int mb_mod_count = 0;

        do {
            if (mb_mod_info + 16 > mb_mod_cmdline) {
                printf("WARNING: Too many modules loaded, aborting.\n");
                break;
            }

            next_initrd = strchr(initrd_filename, ',');
            if (next_initrd)
                *next_initrd = '\0';
            /* if a space comes after the module filename, treat everything
               after that as parameters */
            pstrcpy((char*)bootinfo + mb_mod_cmdline,
                    sizeof(bootinfo) - mb_mod_cmdline,
                    initrd_filename);
            stl_p(bootinfo + mb_mod_info + 8, mb_bootinfo + mb_mod_cmdline); /* string */
            mb_mod_cmdline += strlen(initrd_filename) + 1;
            if (mb_mod_cmdline > sizeof(bootinfo)) {
                mb_mod_cmdline = sizeof(bootinfo);
                printf("WARNING: Too many module cmdlines loaded, aborting.\n");
                break;
            }
            if ((next_space = strchr(initrd_filename, ' ')))
                *next_space = '\0';
#ifdef DEBUG_MULTIBOOT
            printf("multiboot loading module: %s\n", initrd_filename);
#endif
            mb_mod_start = (mb_mod_start + mb_mod_length + (TARGET_PAGE_SIZE - 1))
                         & (TARGET_PAGE_MASK);
            mb_mod_length = get_image_size(initrd_filename);
            if (mb_mod_length < 0) {
                fprintf(stderr, "failed to get %s image size\n", initrd_filename);
                exit(1);
            }
            mb_mod_end = mb_mod_start + mb_mod_length;
            mb_mod_count++;

            /* append module data at the end of last module */
            mb_kernel_data = qemu_realloc(mb_kernel_data,
                                          mb_mod_end - mh_load_addr);
            load_image(initrd_filename,
                       mb_kernel_data + mb_mod_start - mh_load_addr);

            stl_p(bootinfo + mb_mod_info + 0, mb_mod_start);
            stl_p(bootinfo + mb_mod_info + 4, mb_mod_start + mb_mod_length);
            stl_p(bootinfo + mb_mod_info + 12, 0x0); /* reserved */
#ifdef DEBUG_MULTIBOOT
            printf("mod_start: %#x\nmod_end:   %#x\n", mb_mod_start,
                   mb_mod_start + mb_mod_length);
#endif
            initrd_filename = next_initrd+1;
            mb_mod_info += 16;
        } while (next_initrd);
        stl_p(bootinfo + 20, mb_mod_count); /* mods_count */
        stl_p(bootinfo + 24, mb_bootinfo + 0x100); /* mods_addr */
    }

    /* Commandline support */
    stl_p(bootinfo + 16, mb_bootinfo + cmdline);
    snprintf((char*)bootinfo + cmdline, 0x100, "%s %s",
             kernel_filename, kernel_cmdline);

    /* the kernel is where we want it to be now */
#define MULTIBOOT_FLAGS_MEMORY (1 << 0)
#define MULTIBOOT_FLAGS_BOOT_DEVICE (1 << 1)
#define MULTIBOOT_FLAGS_CMDLINE (1 << 2)
#define MULTIBOOT_FLAGS_MODULES (1 << 3)
#define MULTIBOOT_FLAGS_MMAP (1 << 6)
    stl_p(bootinfo, MULTIBOOT_FLAGS_MEMORY
                  | MULTIBOOT_FLAGS_BOOT_DEVICE
                  | MULTIBOOT_FLAGS_CMDLINE
                  | MULTIBOOT_FLAGS_MODULES
                  | MULTIBOOT_FLAGS_MMAP);
    stl_p(bootinfo + 4, 640); /* mem_lower */
    stl_p(bootinfo + 8, ram_size / 1024); /* mem_upper */
    stl_p(bootinfo + 12, 0x8001ffff); /* XXX: use the -boot switch? */
    stl_p(bootinfo + 48, mmap_addr); /* mmap_addr */

#ifdef DEBUG_MULTIBOOT
    fprintf(stderr, "multiboot: mh_entry_addr = %#x\n", mh_entry_addr);
#endif

    /* save bootinfo off the stack */
    mb_bootinfo_data = qemu_malloc(sizeof(bootinfo));
    memcpy(mb_bootinfo_data, bootinfo, sizeof(bootinfo));

    /* Pass variables to option rom */
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ENTRY, mh_entry_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ADDR, mh_load_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_SIZE, mb_mod_end - mh_load_addr);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_KERNEL_DATA, mb_kernel_data,
                     mb_mod_end - mh_load_addr);

    fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_ADDR, mb_bootinfo);
    fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_SIZE, sizeof(bootinfo));
    fw_cfg_add_bytes(fw_cfg, FW_CFG_INITRD_DATA, mb_bootinfo_data,
                     sizeof(bootinfo));

    option_rom[nb_option_roms] = "multiboot.bin";
    nb_option_roms++;

    return 1; /* yes, we are multiboot */
}

static void load_linux(void *fw_cfg,
                       const char *kernel_filename,
		       const char *initrd_filename,
		       const char *kernel_cmdline,
                       target_phys_addr_t max_ram_size)
{
    uint16_t protocol;
    int setup_size, kernel_size, initrd_size = 0, cmdline_size;
    uint32_t initrd_max;
    uint8_t header[8192], *setup, *kernel, *initrd_data;
    target_phys_addr_t real_addr, prot_addr, cmdline_addr, initrd_addr = 0;
    FILE *f;
    char *vmode;

    /* Align to 16 bytes as a paranoia measure */
    cmdline_size = (strlen(kernel_cmdline)+16) & ~15;

    /* load the kernel header */
    f = fopen(kernel_filename, "rb");
    if (!f || !(kernel_size = get_file_size(f)) ||
	fread(header, 1, MIN(ARRAY_SIZE(header), kernel_size), f) !=
	MIN(ARRAY_SIZE(header), kernel_size)) {
	fprintf(stderr, "qemu: could not load kernel '%s': %s\n",
		kernel_filename, strerror(errno));
	exit(1);
    }

    /* kernel protocol version */
#if 0
    fprintf(stderr, "header magic: %#x\n", ldl_p(header+0x202));
#endif
    if (ldl_p(header+0x202) == 0x53726448)
	protocol = lduw_p(header+0x206);
    else {
	/* This looks like a multiboot kernel. If it is, let's stop
	   treating it like a Linux kernel. */
	if (load_multiboot(fw_cfg, f, kernel_filename,
                           initrd_filename, kernel_cmdline, header))
            return;
	protocol = 0;
    }

    if (protocol < 0x200 || !(header[0x211] & 0x01)) {
	/* Low kernel */
	real_addr    = 0x90000;
	cmdline_addr = 0x9a000 - cmdline_size;
	prot_addr    = 0x10000;
    } else if (protocol < 0x202) {
	/* High but ancient kernel */
	real_addr    = 0x90000;
	cmdline_addr = 0x9a000 - cmdline_size;
	prot_addr    = 0x100000;
    } else {
	/* High and recent kernel */
	real_addr    = 0x10000;
	cmdline_addr = 0x20000;
	prot_addr    = 0x100000;
    }

#if 0
    fprintf(stderr,
	    "qemu: real_addr     = 0x" TARGET_FMT_plx "\n"
	    "qemu: cmdline_addr  = 0x" TARGET_FMT_plx "\n"
	    "qemu: prot_addr     = 0x" TARGET_FMT_plx "\n",
	    real_addr,
	    cmdline_addr,
	    prot_addr);
#endif

    /* highest address for loading the initrd */
    if (protocol >= 0x203)
	initrd_max = ldl_p(header+0x22c);
    else
	initrd_max = 0x37ffffff;

    if (initrd_max >= max_ram_size-ACPI_DATA_SIZE)
    	initrd_max = max_ram_size-ACPI_DATA_SIZE-1;

    fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_ADDR, cmdline_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_SIZE, strlen(kernel_cmdline)+1);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_CMDLINE_DATA,
                     (uint8_t*)strdup(kernel_cmdline),
                     strlen(kernel_cmdline)+1);

    if (protocol >= 0x202) {
	stl_p(header+0x228, cmdline_addr);
    } else {
	stw_p(header+0x20, 0xA33F);
	stw_p(header+0x22, cmdline_addr-real_addr);
    }

    /* handle vga= parameter */
    vmode = strstr(kernel_cmdline, "vga=");
    if (vmode) {
        unsigned int video_mode;
        /* skip "vga=" */
        vmode += 4;
        if (!strncmp(vmode, "normal", 6)) {
            video_mode = 0xffff;
        } else if (!strncmp(vmode, "ext", 3)) {
            video_mode = 0xfffe;
        } else if (!strncmp(vmode, "ask", 3)) {
            video_mode = 0xfffd;
        } else {
            video_mode = strtol(vmode, NULL, 0);
        }
        stw_p(header+0x1fa, video_mode);
    }

    /* loader type */
    /* High nybble = B reserved for Qemu; low nybble is revision number.
       If this code is substantially changed, you may want to consider
       incrementing the revision. */
    if (protocol >= 0x200)
	header[0x210] = 0xB0;

    /* heap */
    if (protocol >= 0x201) {
	header[0x211] |= 0x80;	/* CAN_USE_HEAP */
	stw_p(header+0x224, cmdline_addr-real_addr-0x200);
    }

    /* load initrd */
    if (initrd_filename) {
	if (protocol < 0x200) {
	    fprintf(stderr, "qemu: linux kernel too old to load a ram disk\n");
	    exit(1);
	}

	initrd_size = get_image_size(initrd_filename);
        initrd_addr = (initrd_max-initrd_size) & ~4095;

        initrd_data = qemu_malloc(initrd_size);
        load_image(initrd_filename, initrd_data);

        fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_ADDR, initrd_addr);
        fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_SIZE, initrd_size);
        fw_cfg_add_bytes(fw_cfg, FW_CFG_INITRD_DATA, initrd_data, initrd_size);

	stl_p(header+0x218, initrd_addr);
	stl_p(header+0x21c, initrd_size);
    }

    /* load kernel and setup */
    setup_size = header[0x1f1];
    if (setup_size == 0)
	setup_size = 4;
    setup_size = (setup_size+1)*512;
    kernel_size -= setup_size;

    setup  = qemu_malloc(setup_size);
    kernel = qemu_malloc(kernel_size);
    fseek(f, 0, SEEK_SET);
    fread(setup, 1, setup_size, f);
    fread(kernel, 1, kernel_size, f);
    fclose(f);
    memcpy(setup, header, MIN(sizeof(header), setup_size));

    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ADDR, prot_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_SIZE, kernel_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_KERNEL_DATA, kernel, kernel_size);

    fw_cfg_add_i32(fw_cfg, FW_CFG_SETUP_ADDR, real_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_SETUP_SIZE, setup_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_SETUP_DATA, setup, setup_size);

    option_rom[nb_option_roms] = "linuxboot.bin";
    nb_option_roms++;
}

static const int ide_iobase[2] = { 0x1f0, 0x170 };
static const int ide_iobase2[2] = { 0x3f6, 0x376 };
static const int ide_irq[2] = { 14, 15 };

#define NE2000_NB_MAX 6

static const int ne2000_io[NE2000_NB_MAX] = { 0x300, 0x320, 0x340, 0x360,
                                              0x280, 0x380 };
static const int ne2000_irq[NE2000_NB_MAX] = { 9, 10, 11, 3, 4, 5 };

static const int parallel_io[MAX_PARALLEL_PORTS] = { 0x378, 0x278, 0x3bc };
static const int parallel_irq[MAX_PARALLEL_PORTS] = { 7, 7, 7 };

#ifdef HAS_AUDIO
static void audio_init (PCIBus *pci_bus, qemu_irq *pic)
{
    struct soundhw *c;

    for (c = soundhw; c->name; ++c) {
        if (c->enabled) {
            if (c->isa) {
                c->init.init_isa(pic);
            } else {
                if (pci_bus) {
                    c->init.init_pci(pci_bus);
                }
            }
        }
    }
}
#endif

static void pc_init_ne2k_isa(NICInfo *nd)
{
    static int nb_ne2k = 0;

    if (nb_ne2k == NE2000_NB_MAX)
        return;
    isa_ne2000_init(ne2000_io[nb_ne2k],
                    ne2000_irq[nb_ne2k], nd);
    nb_ne2k++;
}

int cpu_is_bsp(CPUState *env)
{
    return env->cpuid_apic_id == 0;
}

static CPUState *pc_new_cpu(const char *cpu_model)
{
    CPUState *env;

    env = cpu_init(cpu_model);
    if (!env) {
        fprintf(stderr, "Unable to find x86 CPU definition\n");
        exit(1);
    }
    if ((env->cpuid_features & CPUID_APIC) || smp_cpus > 1) {
        env->cpuid_apic_id = env->cpu_index;
        /* APIC reset callback resets cpu */
        apic_init(env);
    } else {
        qemu_register_reset((QEMUResetHandler*)cpu_reset, env);
    }
    return env;
}

/* PC hardware initialisation */
static void pc_init1(ram_addr_t ram_size,
                     const char *boot_device,
                     const char *kernel_filename,
                     const char *kernel_cmdline,
                     const char *initrd_filename,
                     const char *cpu_model,
                     int pci_enabled)
{
    char *filename;
    int ret, linux_boot, i;
    ram_addr_t ram_addr, bios_offset, option_rom_offset;
    ram_addr_t below_4g_mem_size, above_4g_mem_size = 0;
    int bios_size, isa_bios_size;
    PCIBus *pci_bus;
    ISADevice *isa_dev;
    int piix3_devfn = -1;
    CPUState *env;
    qemu_irq *cpu_irq;
    qemu_irq *isa_irq;
    qemu_irq *i8259;
    IsaIrqState *isa_irq_state;
    DriveInfo *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    DriveInfo *fd[MAX_FD];
    void *fw_cfg;

    if (ram_size >= 0xe0000000 ) {
        above_4g_mem_size = ram_size - 0xe0000000;
        below_4g_mem_size = 0xe0000000;
    } else {
        below_4g_mem_size = ram_size;
    }

    linux_boot = (kernel_filename != NULL);

    /* init CPUs */
    if (cpu_model == NULL) {
#ifdef TARGET_X86_64
        cpu_model = "qemu64";
#else
        cpu_model = "qemu32";
#endif
    }

    for (i = 0; i < smp_cpus; i++) {
        env = pc_new_cpu(cpu_model);
    }

    vmport_init();

    /* allocate RAM */
    ram_addr = qemu_ram_alloc(0xa0000);
    cpu_register_physical_memory(0, 0xa0000, ram_addr);

    /* Allocate, even though we won't register, so we don't break the
     * phys_ram_base + PA assumption. This range includes vga (0xa0000 - 0xc0000),
     * and some bios areas, which will be registered later
     */
    ram_addr = qemu_ram_alloc(0x100000 - 0xa0000);
    ram_addr = qemu_ram_alloc(below_4g_mem_size - 0x100000);
    cpu_register_physical_memory(0x100000,
                 below_4g_mem_size - 0x100000,
                 ram_addr);

    /* above 4giga memory allocation */
    if (above_4g_mem_size > 0) {
#if TARGET_PHYS_ADDR_BITS == 32
        hw_error("To much RAM for 32-bit physical address");
#else
        ram_addr = qemu_ram_alloc(above_4g_mem_size);
        cpu_register_physical_memory(0x100000000ULL,
                                     above_4g_mem_size,
                                     ram_addr);
#endif
    }


    /* BIOS load */
    if (bios_name == NULL)
        bios_name = BIOS_FILENAME;
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = get_image_size(filename);
    } else {
        bios_size = -1;
    }
    if (bios_size <= 0 ||
        (bios_size % 65536) != 0) {
        goto bios_error;
    }
    bios_offset = qemu_ram_alloc(bios_size);
    ret = rom_add_file_fixed(bios_name, (uint32_t)(-bios_size));
    if (ret != 0) {
    bios_error:
        fprintf(stderr, "qemu: could not load PC BIOS '%s'\n", bios_name);
        exit(1);
    }
    if (filename) {
        qemu_free(filename);
    }
    /* map the last 128KB of the BIOS in ISA space */
    isa_bios_size = bios_size;
    if (isa_bios_size > (128 * 1024))
        isa_bios_size = 128 * 1024;
    cpu_register_physical_memory(0x100000 - isa_bios_size,
                                 isa_bios_size,
                                 (bios_offset + bios_size - isa_bios_size) | IO_MEM_ROM);



    rom_enable_driver_roms = 1;
    option_rom_offset = qemu_ram_alloc(PC_ROM_SIZE);
    cpu_register_physical_memory(PC_ROM_MIN_VGA, PC_ROM_SIZE, option_rom_offset);

    /* map all the bios at the top of memory */
    cpu_register_physical_memory((uint32_t)(-bios_size),
                                 bios_size, bios_offset | IO_MEM_ROM);

    fw_cfg = bochs_bios_init();
    rom_set_fw(fw_cfg);

    if (linux_boot) {
        load_linux(fw_cfg, kernel_filename, initrd_filename, kernel_cmdline, below_4g_mem_size);
    }

    for (i = 0; i < nb_option_roms; i++) {
        rom_add_option(option_rom[i]);
    }

    cpu_irq = qemu_allocate_irqs(pic_irq_request, NULL, 1);
    i8259 = i8259_init(cpu_irq[0]);
    isa_irq_state = qemu_mallocz(sizeof(*isa_irq_state));
    isa_irq_state->i8259 = i8259;
    isa_irq = qemu_allocate_irqs(isa_irq_handler, isa_irq_state, 24);

    if (pci_enabled) {
        pci_bus = i440fx_init(&i440fx_state, &piix3_devfn, isa_irq);
    } else {
        pci_bus = NULL;
        isa_bus_new(NULL);
    }
    isa_bus_irqs(isa_irq);

    ferr_irq = isa_reserve_irq(13);

    /* init basic PC hardware */
    register_ioport_write(0x80, 1, 1, ioport80_write, NULL);

    register_ioport_write(0xf0, 1, 1, ioportF0_write, NULL);

    if (cirrus_vga_enabled) {
        if (pci_enabled) {
            pci_cirrus_vga_init(pci_bus);
        } else {
            isa_cirrus_vga_init();
        }
    } else if (vmsvga_enabled) {
        if (pci_enabled)
            pci_vmsvga_init(pci_bus);
        else
            fprintf(stderr, "%s: vmware_vga: no PCI bus\n", __FUNCTION__);
    } else if (std_vga_enabled) {
        if (pci_enabled) {
            pci_vga_init(pci_bus, 0, 0);
        } else {
            isa_vga_init();
        }
    }

    rtc_state = rtc_init(2000);

    qemu_register_boot_set(pc_boot_set, rtc_state);

    register_ioport_read(0x92, 1, 1, ioport92_read, NULL);
    register_ioport_write(0x92, 1, 1, ioport92_write, NULL);

    if (pci_enabled) {
        isa_irq_state->ioapic = ioapic_init();
    }
    pit = pit_init(0x40, isa_reserve_irq(0));
    pcspk_init(pit);
    if (!no_hpet) {
        hpet_init(isa_irq);
    }

    for(i = 0; i < MAX_SERIAL_PORTS; i++) {
        if (serial_hds[i]) {
            serial_isa_init(i, serial_hds[i]);
        }
    }

    for(i = 0; i < MAX_PARALLEL_PORTS; i++) {
        if (parallel_hds[i]) {
            parallel_init(i, parallel_hds[i]);
        }
    }

    for(i = 0; i < nb_nics; i++) {
        NICInfo *nd = &nd_table[i];

        if (!pci_enabled || (nd->model && strcmp(nd->model, "ne2k_isa") == 0))
            pc_init_ne2k_isa(nd);
        else
            pci_nic_init_nofail(nd, "e1000", NULL);
    }

    if (drive_get_max_bus(IF_IDE) >= MAX_IDE_BUS) {
        fprintf(stderr, "qemu: too many IDE bus\n");
        exit(1);
    }

    for(i = 0; i < MAX_IDE_BUS * MAX_IDE_DEVS; i++) {
        hd[i] = drive_get(IF_IDE, i / MAX_IDE_DEVS, i % MAX_IDE_DEVS);
    }

    if (pci_enabled) {
        pci_piix3_ide_init(pci_bus, hd, piix3_devfn + 1);
    } else {
        for(i = 0; i < MAX_IDE_BUS; i++) {
            isa_ide_init(ide_iobase[i], ide_iobase2[i], ide_irq[i],
	                 hd[MAX_IDE_DEVS * i], hd[MAX_IDE_DEVS * i + 1]);
        }
    }

    isa_dev = isa_create_simple("i8042");
    DMA_init(0);
#ifdef HAS_AUDIO
    audio_init(pci_enabled ? pci_bus : NULL, isa_irq);
#endif

    for(i = 0; i < MAX_FD; i++) {
        fd[i] = drive_get(IF_FLOPPY, 0, i);
    }
    floppy_controller = fdctrl_init_isa(fd);

    cmos_init(below_4g_mem_size, above_4g_mem_size, boot_device, hd);

    if (pci_enabled && usb_enabled) {
        usb_uhci_piix3_init(pci_bus, piix3_devfn + 2);
    }

    if (pci_enabled && acpi_enabled) {
        uint8_t *eeprom_buf = qemu_mallocz(8 * 256); /* XXX: make this persistent */
        i2c_bus *smbus;

        /* TODO: Populate SPD eeprom data.  */
        smbus = piix4_pm_init(pci_bus, piix3_devfn + 3, 0xb100,
                              isa_reserve_irq(9));
        for (i = 0; i < 8; i++) {
            DeviceState *eeprom;
            eeprom = qdev_create((BusState *)smbus, "smbus-eeprom");
            qdev_prop_set_uint8(eeprom, "address", 0x50 + i);
            qdev_prop_set_ptr(eeprom, "data", eeprom_buf + (i * 256));
            qdev_init_nofail(eeprom);
        }
        piix4_acpi_system_hot_add_init(pci_bus);
    }

    if (i440fx_state) {
        i440fx_init_memory_mappings(i440fx_state);
    }

    if (pci_enabled) {
	int max_bus;
        int bus;

        max_bus = drive_get_max_bus(IF_SCSI);
	for (bus = 0; bus <= max_bus; bus++) {
            pci_create_simple(pci_bus, -1, "lsi53c895a");
        }
    }

    /* Add virtio console devices */
    if (pci_enabled) {
        for(i = 0; i < MAX_VIRTIO_CONSOLES; i++) {
            if (virtcon_hds[i]) {
                pci_create_simple(pci_bus, -1, "virtio-console-pci");
            }
        }
    }
}

static void pc_init_pci(ram_addr_t ram_size,
                        const char *boot_device,
                        const char *kernel_filename,
                        const char *kernel_cmdline,
                        const char *initrd_filename,
                        const char *cpu_model)
{
    pc_init1(ram_size, boot_device,
             kernel_filename, kernel_cmdline,
             initrd_filename, cpu_model, 1);
}

static void pc_init_isa(ram_addr_t ram_size,
                        const char *boot_device,
                        const char *kernel_filename,
                        const char *kernel_cmdline,
                        const char *initrd_filename,
                        const char *cpu_model)
{
    if (cpu_model == NULL)
        cpu_model = "486";
    pc_init1(ram_size, boot_device,
             kernel_filename, kernel_cmdline,
             initrd_filename, cpu_model, 0);
}

/* set CMOS shutdown status register (index 0xF) as S3_resume(0xFE)
   BIOS will read it and start S3 resume at POST Entry */
void cmos_set_s3_resume(void)
{
    if (rtc_state)
        rtc_set_memory(rtc_state, 0xF, 0xFE);
}

static QEMUMachine pc_machine = {
    .name = "pc-0.12",
    .alias = "pc",
    .desc = "Standard PC",
    .init = pc_init_pci,
    .max_cpus = 255,
    .is_default = 1,
};

static QEMUMachine pc_machine_v0_11 = {
    .name = "pc-0.11",
    .desc = "Standard PC, qemu 0.11",
    .init = pc_init_pci,
    .max_cpus = 255,
    .compat_props = (GlobalProperty[]) {
        {
            .driver   = "virtio-blk-pci",
            .property = "vectors",
            .value    = stringify(0),
        },{
            .driver   = "ide-drive",
            .property = "ver",
            .value    = "0.11",
        },{
            .driver   = "scsi-disk",
            .property = "ver",
            .value    = "0.11",
        },{
            .driver   = "PCI",
            .property = "rombar",
            .value    = stringify(0),
        },
        { /* end of list */ }
    }
};

static QEMUMachine pc_machine_v0_10 = {
    .name = "pc-0.10",
    .desc = "Standard PC, qemu 0.10",
    .init = pc_init_pci,
    .max_cpus = 255,
    .compat_props = (GlobalProperty[]) {
        {
            .driver   = "virtio-blk-pci",
            .property = "class",
            .value    = stringify(PCI_CLASS_STORAGE_OTHER),
        },{
            .driver   = "virtio-console-pci",
            .property = "class",
            .value    = stringify(PCI_CLASS_DISPLAY_OTHER),
        },{
            .driver   = "virtio-net-pci",
            .property = "vectors",
            .value    = stringify(0),
        },{
            .driver   = "virtio-blk-pci",
            .property = "vectors",
            .value    = stringify(0),
        },{
            .driver   = "ide-drive",
            .property = "ver",
            .value    = "0.10",
        },{
            .driver   = "scsi-disk",
            .property = "ver",
            .value    = "0.10",
        },{
            .driver   = "PCI",
            .property = "rombar",
            .value    = stringify(0),
        },
        { /* end of list */ }
    },
};

static QEMUMachine isapc_machine = {
    .name = "isapc",
    .desc = "ISA-only PC",
    .init = pc_init_isa,
    .max_cpus = 1,
};

static void pc_machine_init(void)
{
    qemu_register_machine(&pc_machine);
    qemu_register_machine(&pc_machine_v0_11);
    qemu_register_machine(&pc_machine_v0_10);
    qemu_register_machine(&isapc_machine);
}

machine_init(pc_machine_init);
