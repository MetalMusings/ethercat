/*****************************************************************************
 *
 *  Test: the XDP transports keep their program on the interface.
 *
 *  Attaching or removing an XDP program restarts the link on common
 *  drivers, so a closing master leaves the program attached and the next
 *  one only binds its socket to it (transport_xdp.c, EC_XDP_PROG_NAME).
 *  Runs on a veth pair in a private network namespace: needs root (BPF
 *  programs cannot be loaded in an unprivileged user namespace) and skips
 *  (exit 77) without it.
 *
 *  Checked: EtherCAT frames reach the transport and other frames the
 *  kernel; frames sent by the transport arrive at the peer; the program
 *  stays after close and the next open reuses it (same program id); a
 *  change between xdp-native and xdp-skb, and a foreign program, are
 *  replaced.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_packet.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "ecrt.h"
#include "ectp.h"

#include "test.h"

#define IF_A "ecxdp0"   /* the transport's side */
#define IF_B "ecxdp1"   /* the peer */
#define ETHERTYPE_LOCAL 0x88B5  /* IEEE local experimental */

static int packet_socket(const char *ifname)
{
    struct sockaddr_ll sll;
    int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));

    if (fd < 0) {
        return -1;
    }
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = if_nametoindex(ifname);
    if (bind(fd, (struct sockaddr *) &sll, sizeof(sll)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void make_frame(uint8_t *f, uint16_t ethertype, uint8_t tag)
{
    memset(f, 0, 60);
    memset(f, 0xff, 6);
    f[6] = 0x02; f[11] = tag;
    f[12] = ethertype >> 8;
    f[13] = ethertype & 0xff;
    f[14] = tag;
}

/* Wait up to ms for a frame with this ethertype and tag on fd. */
static int packet_seen(int fd, uint16_t ethertype, uint8_t tag, int ms)
{
    uint8_t buf[1600];
    struct pollfd p = { .fd = fd, .events = POLLIN };
    ssize_t n;

    while (poll(&p, 1, ms) > 0) {
        n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n >= 15 && buf[12] == (ethertype >> 8)
                && buf[13] == (ethertype & 0xff) && buf[14] == tag) {
            return 1;
        }
    }
    return 0;
}

static int transport_seen(ec_transport_t *t, uint8_t tag, int ms)
{
    uint8_t buf[1600];
    int i, n;

    for (i = 0; i < ms; i++) {
        n = ec_transport_receive(t, buf, sizeof(buf));
        if (n >= 15 && buf[14] == tag) {
            return 1;
        }
        usleep(1000);
    }
    return 0;
}

static void prog_ids(uint32_t *drv, uint32_t *skb)
{
    LIBBPF_OPTS(bpf_xdp_query_opts, opts);

    *drv = *skb = 0;
    if (bpf_xdp_query(if_nametoindex(IF_A), 0, &opts) == 0) {
        *drv = opts.drv_prog_id;
        *skb = opts.skb_prog_id;
    }
}

/* One open/receive/send round on IF_A with the given transport. */
static ec_transport_t *open_and_check(const char *name, int peer, int own,
        uint8_t tag)
{
    ec_transport_t *t = ec_transport_create_by_name(name, IF_A);
    uint8_t f[60];

    TEST_CHECK(t != NULL);
    if (!t) {
        return NULL;
    }
    TEST_CHECK_EQ(0, ec_transport_open(t));

    /* EtherCAT frame from the peer: to the transport, not the kernel. */
    make_frame(f, 0x88A4, tag);
    TEST_CHECK(send(peer, f, sizeof(f), 0) == sizeof(f));
    TEST_CHECK(transport_seen(t, tag, 1000));
    TEST_CHECK(!packet_seen(own, 0x88A4, tag, 100));

    /* Another ethertype: to the kernel, not the transport. */
    make_frame(f, ETHERTYPE_LOCAL, tag);
    TEST_CHECK(send(peer, f, sizeof(f), 0) == sizeof(f));
    TEST_CHECK(packet_seen(own, ETHERTYPE_LOCAL, tag, 1000));
    TEST_CHECK(!transport_seen(t, tag, 100));

    /* Sent by the transport: arrives at the peer. */
    make_frame(ec_transport_get_tx_buffer(t), 0x88A4, tag + 1);
    TEST_CHECK_EQ(0, ec_transport_send(t, 60));
    TEST_CHECK(packet_seen(peer, 0x88A4, tag + 1, 1000));
    return t;
}

static void close_transport(ec_transport_t *t)
{
    if (t) {
        ec_transport_close(t);
        ec_transport_destroy(t);
    }
}

int main(void)
{
    uint32_t drv, skb, first, again;
    ec_transport_t *t;
    int peer, own, pass_fd;

    if (geteuid() != 0) {
        fprintf(stderr, "test_xdp_keep: needs root (BPF programs) -"
                " skipping\n");
        return 77;
    }
    if (unshare(CLONE_NEWNET) < 0
            || system("ip link set lo up && ip link add " IF_A
                " type veth peer name " IF_B " && ip link set " IF_A
                " up && ip link set " IF_B " up") != 0) {
        fprintf(stderr, "test_xdp_keep: no network namespace or veth -"
                " skipping\n");
        return 77;
    }
    usleep(200000);
    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    peer = packet_socket(IF_B);
    own = packet_socket(IF_A);
    TEST_CHECK(peer >= 0 && own >= 0);

    /* First open attaches the program. */
    t = open_and_check("xdp-native", peer, own, 0x10);
    prog_ids(&drv, &skb);
    first = drv;
    TEST_CHECK(first != 0);
    TEST_CHECK_EQ(0, skb);

    /* Close keeps it; the next open reuses it. */
    close_transport(t);
    prog_ids(&drv, &skb);
    TEST_CHECK_EQ(first, drv);
    t = open_and_check("xdp-native", peer, own, 0x20);
    prog_ids(&again, &skb);
    TEST_CHECK_EQ(first, again);
    close_transport(t);

    /* Without a socket the program passes EtherCAT frames to the kernel
     * (once the kernel has released the closed socket: a little later). */
    {
        uint8_t f[60];
        int i, seen = 0;
        make_frame(f, 0x88A4, 0x30);
        for (i = 0; i < 20 && !seen; i++) {
            TEST_CHECK(send(peer, f, sizeof(f), 0) == sizeof(f));
            seen = packet_seen(own, 0x88A4, 0x30, 100);
        }
        TEST_CHECK(seen);
    }

    /* Another mode: the native program is replaced by a generic one. */
    t = open_and_check("xdp-skb", peer, own, 0x40);
    prog_ids(&drv, &skb);
    TEST_CHECK_EQ(0, drv);
    TEST_CHECK(skb != 0);
    close_transport(t);

    /* A foreign program is replaced. */
    bpf_xdp_detach(if_nametoindex(IF_A), XDP_FLAGS_SKB_MODE, NULL);
    {
        const struct bpf_insn pass[] = {
            { .code = BPF_ALU64 | BPF_MOV | BPF_K, .imm = XDP_PASS },
            { .code = BPF_JMP | BPF_EXIT },
        };
        pass_fd = bpf_prog_load(BPF_PROG_TYPE_XDP, "foreign", "GPL", pass,
                2, NULL);
        TEST_CHECK(pass_fd >= 0);
        TEST_CHECK_EQ(0, bpf_xdp_attach(if_nametoindex(IF_A), pass_fd,
                    XDP_FLAGS_DRV_MODE, NULL));
    }
    prog_ids(&first, &skb);
    t = open_and_check("xdp-native", peer, own, 0x50);
    prog_ids(&drv, &skb);
    TEST_CHECK(drv != 0 && drv != first);
    close_transport(t);
    close(pass_fd);

    close(peer);
    close(own);
    ecrt_lib_cleanup();
    return test_done("test_xdp_keep");
}
