/*****************************************************************************
 *
 *  Integration test: Ethernet over EtherCAT in the application's cyclic
 *  frames (no ecrt_master_callbacks()).
 *
 *  Without callbacks, the EoE thread only runs the EoE state machines; its
 *  datagrams go out with the application's ecrt_master_send() and come back
 *  with its ecrt_master_receive() (ec_master_queue_ext_datagrams()). The
 *  test checks:
 *
 *  - Ethernet frames of several mailbox fragments travel TAP -> slave ->
 *    echo -> TAP, all of them, with the payload intact;
 *  - the EoE thread never sends a frame itself: the simulated bus sees
 *    frames only during the application's cycle;
 *  - the process data's working counter stays complete in every cycle;
 *  - a link loss with EoE datagrams in flight (rescan in OP: the EoE
 *    handlers are cleared while the application's thread may own their
 *    datagrams) leaves a master that comes back to OP and carries EoE again.
 *
 *  Like test_sim_eoe, it needs CAP_NET_ADMIN for the TAP device, and
 *  otherwise re-executes itself in an unprivileged user+network namespace,
 *  or skips (exit 77).
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "transport_sim.h"

#include "ecrt_tool.h"

#include "test.h"

#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x5134000C

#define TAP_NAME "eoe0s0"
#define TEST_ETHERTYPE 0x88B5 /* IEEE local experimental */

#define MAX_CYCLES 15000
#define NFRAMES 20
#ifndef FRAME_LEN
#define FRAME_LEN 600 /* 5 fragments into a 128-byte mailbox */
#endif

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 10001, .alias = 0,
      .sm2_phys = 0x1800, .sm2_len = 8,
      .sm3_phys = 0x1A00, .sm3_len = 8,
      /* mailboxes of different sizes, as some slaves have: frames go to
       * the slave in fragments, the echo comes back in one */
      .mbox_out_phys = 0x1000, .mbox_out_len = 128,
      .mbox_in_phys = 0x1080, .mbox_in_len = 1024,
      .eoe = 1 },
};

static ec_pdo_entry_info_t out_entries[] = {
    { 0x7000, 0x01, 16 },
};

static ec_pdo_entry_info_t in_entries[] = {
    { 0x6000, 0x01, 16 },
};

static ec_pdo_info_t rx_pdos[] = {
    { 0x1600, 1, out_entries },
};

static ec_pdo_info_t tx_pdos[] = {
    { 0x1A00, 1, in_entries },
};

static ec_sync_info_t syncs[] = {
    { 2, EC_DIR_OUTPUT, 1, rx_pdos, EC_WD_ENABLE },
    { 3, EC_DIR_INPUT, 1, tx_pdos, EC_WD_DISABLE },
    { 0xff, EC_DIR_INVALID, 0, NULL, EC_WD_DEFAULT }
};

static sim_bus_t *bus;
static ec_master_t *master;
static ec_domain_t *domain;

/* Frames the bus saw outside the application's cycle, and cycles with an
 * incomplete working counter while checking. */
static unsigned long frames_between_cycles;
static unsigned int incomplete_cycles;
static int check_wc;

/** One application cycle, then 1 ms outside it. Frames that reach the bus
 * during the 1 ms were sent by another thread than the application's. */
static void cycle(void)
{
    ec_domain_state_t dstate;
    unsigned long before;

    ecrt_master_receive(master);
    ecrt_domain_process(domain);
    ecrt_domain_state(domain, &dstate);
    if (check_wc && dstate.wc_state != EC_WC_COMPLETE) {
        incomplete_cycles++;
    }
    ecrt_domain_queue(domain);
    ecrt_master_send(master);

    before = sim_bus_frame_count(bus);
    usleep(1000);
    frames_between_cycles += sim_bus_frame_count(bus) - before;
}

/** Bring the TAP interface up. */
static int ifup(const char *name)
{
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int ret = -1;

    if (fd < 0) {
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ret = ioctl(fd, SIOCSIFFLAGS, &ifr);
    }
    close(fd);
    return ret;
}

/** Cycle until the slave is in OP with a complete working counter. */
static int wait_op(ec_slave_config_t *sc)
{
    ec_slave_config_state_t sc_state;
    ec_domain_state_t dstate;
    unsigned int cycles;

    for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
        cycle();
        ecrt_slave_config_state(sc, &sc_state);
        ecrt_domain_state(domain, &dstate);
        if (sc_state.operational && dstate.wc_state == EC_WC_COMPLETE) {
            return 1;
        }
    }
    return 0;
}

/** Packet socket bound to the TAP device (which a rescan recreates). */
static int open_tap_socket(unsigned int *ifindex)
{
    struct sockaddr_ll sll;
    int sock;

    *ifindex = if_nametoindex(TAP_NAME);
    if (!*ifindex || ifup(TAP_NAME) != 0) {
        return -1;
    }
    sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        return -1;
    }
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = (int) *ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);
    if (bind(sock, (struct sockaddr *) &sll, sizeof(sll)) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void make_frame(uint8_t *frame, unsigned int seq)
{
    unsigned int i;

    memset(frame, 0xFF, 6); /* broadcast */
    memset(frame + 6, 0, 6);
    frame[6] = 0x02; frame[11] = 0x01; /* locally administered source */
    frame[12] = TEST_ETHERTYPE >> 8;
    frame[13] = TEST_ETHERTYPE & 0xFF;
    frame[14] = (uint8_t) seq;
    for (i = 15; i < FRAME_LEN; i++) {
        frame[i] = (uint8_t) (i * 7 + seq);
    }
}

/** Send frames first..first+count-1 into the TAP and cycle until all have
 * come back intact. Returns the number that came back. */
static unsigned int echo_frames(int sock, unsigned int ifindex,
        unsigned int first, unsigned int count)
{
    uint8_t frame[FRAME_LEN], expect[FRAME_LEN], rbuf[1600];
    struct sockaddr_ll sll;
    unsigned int seq, got = 0, cycles;
    unsigned char seen[256];

    memset(seen, 0, sizeof(seen));
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = (int) ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    for (seq = first; seq < first + count; seq++) {
        make_frame(frame, seq);
        if (sendto(sock, frame, sizeof(frame), 0,
                    (struct sockaddr *) &sll, sizeof(sll))
                != (ssize_t) sizeof(frame)) {
            return 0;
        }
    }

    for (cycles = 0; cycles < MAX_CYCLES && got < count; cycles++) {
        struct sockaddr_ll from;
        socklen_t fromlen = sizeof(from);
        ssize_t n;

        cycle();
        while ((n = recvfrom(sock, rbuf, sizeof(rbuf), MSG_DONTWAIT,
                        (struct sockaddr *) &from, &fromlen)) > 0) {
            fromlen = sizeof(from);
            if (from.sll_pkttype == PACKET_OUTGOING || n != FRAME_LEN
                    || rbuf[12] != (TEST_ETHERTYPE >> 8)
                    || rbuf[13] != (TEST_ETHERTYPE & 0xFF)) {
                continue;
            }
            seq = rbuf[14];
            make_frame(expect, seq);
            if (memcmp(rbuf, expect, FRAME_LEN) || seen[seq]) {
                continue;
            }
            seen[seq] = 1;
            got++;
        }
    }
    return got;
}

int main(int argc, char **argv)
{
    ec_slave_config_t *sc;
    ec_tool_slave_sdo_entry_t entry;
    unsigned int ifindex, waited, cycles;
    int off_out, off_in, sock;
    uint8_t frame[FRAME_LEN];
    struct sockaddr_ll sll;

    (void) argc;

    /* See test_sim_eoe.c. */
    if (!getenv("EC_TEST_EOE_WRAPPED") && geteuid() != 0
            && system("unshare -r -n true >/dev/null 2>&1") == 0) {
        setenv("EC_TEST_EOE_WRAPPED", "1", 1);
        execlp("unshare", "unshare", "-r", "-n", argv[0], (char *) NULL);
    }

    bus = sim_bus_create(1, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_eoe_cyclic");
    }

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    if (!if_nametoindex(TAP_NAME)) {
        fprintf(stderr, "test_sim_eoe_cyclic: no %s interface (TAP"
                " creation needs CAP_NET_ADMIN) — skipping\n", TAP_NAME);
        ecrt_release_master(master);
        ecrt_lib_cleanup();
        sim_bus_destroy(bus);
        return 77;
    }

    /* Let the SDO dictionary fetch finish first (see test_sim_eoe.c). */
    for (waited = 0; waited < 15000; waited += 100) {
        memset(&entry, 0, sizeof(entry));
        entry.slave_position = 0;
        entry.sdo_spec = 0x1C13;
        entry.sdo_entry_subindex = 0;
        if (ecrt_tool_get_slave_sdo_entry(master, &entry) == 0) {
            break;
        }
        usleep(100000);
    }

    domain = ecrt_master_create_domain(master);
    sc = ecrt_master_slave_config(master, 0, 0, VENDOR_ID, PRODUCT_CODE);
    TEST_CHECK(domain != NULL);
    TEST_CHECK(sc != NULL);
    if (!domain || !sc) {
        goto out_release;
    }
    TEST_CHECK_EQ(0, ecrt_slave_config_pdos(sc, EC_END, syncs));
    off_out = ecrt_slave_config_reg_pdo_entry(sc, 0x7000, 0x01, domain, NULL);
    off_in = ecrt_slave_config_reg_pdo_entry(sc, 0x6000, 0x01, domain, NULL);
    TEST_CHECK(off_out >= 0);
    TEST_CHECK(off_in >= 0);

    /* No ecrt_master_callbacks(): EoE rides in the cyclic frames. */
    TEST_CHECK_EQ(0, ecrt_master_activate(master));
    TEST_CHECK(ecrt_domain_data(domain) != NULL);
    if (!ecrt_domain_data(domain)) {
        goto out_release;
    }
    TEST_CHECK(wait_op(sc));

    sock = open_tap_socket(&ifindex);
    TEST_CHECK(sock >= 0);
    if (sock < 0) {
        goto out_release;
    }

    /* Several multi-fragment frames, only during the cycle, process data
     * complete throughout. */
    frames_between_cycles = 0;
    incomplete_cycles = 0;
    check_wc = 1;
    TEST_CHECK_EQ(NFRAMES, (int) echo_frames(sock, ifindex, 0, NFRAMES));
    check_wc = 0;
    TEST_CHECK_EQ(0, (int) frames_between_cycles);
    TEST_CHECK_EQ(0, (int) incomplete_cycles);

    /* Link loss with EoE traffic in flight: queue frames, run a few
     * cycles so that EoE datagrams are queued or sent, then pull the
     * link. The master's rescan clears the EoE handlers while their
     * datagrams may be in the application's queue. */
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = (int) ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);
    for (waited = 0; waited < 5; waited++) {
        make_frame(frame, 100 + waited);
        sendto(sock, frame, sizeof(frame), 0,
                (struct sockaddr *) &sll, sizeof(sll));
    }
    for (cycles = 0; cycles < 3; cycles++) {
        cycle();
    }
    sim_bus_set_link(bus, 0);
    for (cycles = 0; cycles < 300; cycles++) {
        cycle();
    }
    close(sock);
    sim_bus_set_link(bus, 1);
    TEST_CHECK(wait_op(sc));

    /* The rescan recreated the TAP device: EoE works again. */
    sock = -1;
    for (cycles = 0; cycles < MAX_CYCLES && sock < 0; cycles++) {
        cycle();
        sock = open_tap_socket(&ifindex);
    }
    TEST_CHECK(sock >= 0);
    if (sock >= 0) {
        TEST_CHECK_EQ(3, (int) echo_frames(sock, ifindex, 200, 3));
        close(sock);
    }

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_eoe_cyclic");
}
