/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2009 Red Hat, Inc.
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

#include "net/tap.h"
#include "net/tap-linux.h"

#include <net/if.h>
#include <sys/ioctl.h>

#include "sysemu.h"
#include "qemu-common.h"

int tap_open(char *ifname, int ifname_size, int *vnet_hdr, int vnet_hdr_required)
{
    struct ifreq ifr;
    int fd, ret;

    TFR(fd = open("/dev/net/tun", O_RDWR));
    if (fd < 0) {
        fprintf(stderr, "warning: could not open /dev/net/tun: no virtual network emulation\n");
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    if (*vnet_hdr) {
        unsigned int features;

        if (ioctl(fd, TUNGETFEATURES, &features) == 0 &&
            features & IFF_VNET_HDR) {
            *vnet_hdr = 1;
            ifr.ifr_flags |= IFF_VNET_HDR;
        } else {
            *vnet_hdr = 0;
        }

        if (vnet_hdr_required && !*vnet_hdr) {
            qemu_error("vnet_hdr=1 requested, but no kernel "
                       "support for IFF_VNET_HDR available");
            close(fd);
            return -1;
        }
    }

    if (ifname[0] != '\0')
        pstrcpy(ifr.ifr_name, IFNAMSIZ, ifname);
    else
        pstrcpy(ifr.ifr_name, IFNAMSIZ, "tap%d");
    ret = ioctl(fd, TUNSETIFF, (void *) &ifr);
    if (ret != 0) {
        fprintf(stderr, "warning: could not configure /dev/net/tun: no virtual network emulation\n");
        close(fd);
        return -1;
    }
    pstrcpy(ifname, ifname_size, ifr.ifr_name);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

/* sndbuf should be set to a value lower than the tx queue
 * capacity of any destination network interface.
 * Ethernet NICs generally have txqueuelen=1000, so 1Mb is
 * a good default, given a 1500 byte MTU.
 */
#define TAP_DEFAULT_SNDBUF 1024*1024

int tap_set_sndbuf(int fd, QemuOpts *opts)
{
    int sndbuf;

    sndbuf = qemu_opt_get_size(opts, "sndbuf", TAP_DEFAULT_SNDBUF);
    if (!sndbuf) {
        sndbuf = INT_MAX;
    }

    if (ioctl(fd, TUNSETSNDBUF, &sndbuf) == -1 && qemu_opt_get(opts, "sndbuf")) {
        qemu_error("TUNSETSNDBUF ioctl failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int tap_probe_vnet_hdr(int fd)
{
    struct ifreq ifr;

    if (ioctl(fd, TUNGETIFF, &ifr) != 0) {
        qemu_error("TUNGETIFF ioctl() failed: %s\n", strerror(errno));
        return 0;
    }

    return ifr.ifr_flags & IFF_VNET_HDR;
}

int tap_probe_has_ufo(int fd)
{
    unsigned offload;

    offload = TUN_F_CSUM | TUN_F_UFO;

    if (ioctl(fd, TUNSETOFFLOAD, offload) < 0)
        return 0;

    return 1;
}

void tap_fd_set_offload(int fd, int csum, int tso4,
                        int tso6, int ecn, int ufo)
{
    unsigned int offload = 0;

    /* Check if our kernel supports TUNSETOFFLOAD */
    if (ioctl(fd, TUNSETOFFLOAD, 0) != 0 && errno == EINVAL) {
        return;
    }

    if (csum) {
        offload |= TUN_F_CSUM;
        if (tso4)
            offload |= TUN_F_TSO4;
        if (tso6)
            offload |= TUN_F_TSO6;
        if ((tso4 || tso6) && ecn)
            offload |= TUN_F_TSO_ECN;
        if (ufo)
            offload |= TUN_F_UFO;
    }

    if (ioctl(fd, TUNSETOFFLOAD, offload) != 0) {
        offload &= ~TUN_F_UFO;
        if (ioctl(fd, TUNSETOFFLOAD, offload) != 0) {
            fprintf(stderr, "TUNSETOFFLOAD ioctl() failed: %s\n",
                    strerror(errno));
        }
    }
}
