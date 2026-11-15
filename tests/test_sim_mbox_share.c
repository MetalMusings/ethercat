/*****************************************************************************
 *
 *  Integration test: several readers of one slave mailbox.
 *
 *  A slave has one mailbox for all its protocols. The master reads it from
 *  the CoE state machines and, in another thread, from the EoE handler,
 *  which polls it all the time. Whoever fetches a mail of the other one
 *  must put it aside for it (mailbox.c); before that, the EoE handler
 *  dropped CoE responses and the CoE state machine failed on an Ethernet
 *  frame. The test checks, on a slave with EoE and CoE:
 *
 *  - the slave reaches OP with several SDOs in its configuration (their
 *    responses pass the polling EoE handler);
 *  - SDO uploads succeed, every one, with the right value, while Ethernet
 *    frames are echoed through the same mailbox;
 *  - the Ethernet frames keep coming back meanwhile.
 *
 *  EoE rides in the application's cyclic frames (no
 *  ecrt_master_callbacks()), as in test_sim_mbox_share. Like that test, it
 *  needs CAP_NET_ADMIN for the TAP device, and otherwise re-executes
 *  itself in an unprivileged user+network namespace, or skips (exit 77).
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
#include <pthread.h>
#include <unistd.h>

#include "transport_sim.h"

#include "ecrt_tool.h"

#include "test.h"

#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x5134000C

#define TAP_NAME "eoe0s0"
#define TEST_ETHERTYPE 0x88B5 /* IEEE local experimental */

#define MAX_CYCLES 15000
#define FRAME_LEN 100 /* one fragment */
#define NSDO 50 /* uploads while frames are echoed */
#define NSTARTUP 10 /* SDOs in the slave's configuration */
#define SDO_INDEX 0x2000
#define SDO_VALUE 0x12345678u

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 10001, .alias = 0,
      .sm2_phys = 0x1800, .sm2_len = 8,
      .sm3_phys = 0x1A00, .sm3_len = 8,
      .mbox_out_phys = 0x1000, .mbox_out_len = 128,
      .mbox_in_phys = 0x1080, .mbox_in_len = 128,
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

static void cycle(void)
{
    ecrt_master_receive(master);
    ecrt_domain_process(domain);
    ecrt_domain_queue(domain);
    ecrt_master_send(master);
    usleep(1000);
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

/* SDO uploads from another thread, as a tool or the application's
 * non-realtime part does them; the main thread keeps cycling. */
static volatile int sdo_done;
static int sdo_ok, sdo_failed, sdo_wrong;

static void *sdo_thread(void *arg)
{
    unsigned int i;

    (void) arg;
    for (i = 0; i < NSDO; i++) {
        uint8_t buf[4];
        size_t size = 0;
        uint32_t abort_code = 0;

        if (ecrt_master_sdo_upload(master, 0, SDO_INDEX, 0, buf,
                    sizeof(buf), &size, &abort_code)) {
            sdo_failed++;
        } else if (size != 4 || EC_READ_U32(buf) != SDO_VALUE) {
            sdo_wrong++;
        } else {
            sdo_ok++;
        }
    }
    sdo_done = 1;
    return NULL;
}

int main(int argc, char **argv)
{
    ec_slave_config_t *sc;
    ec_tool_slave_sdo_entry_t entry;
    unsigned int ifindex, waited, seq = 0, echoed = 0, i;
    int off_out, off_in, sock;
    uint8_t frame[FRAME_LEN], expect[FRAME_LEN], rbuf[1600], value[4];
    struct sockaddr_ll sll;
    pthread_t thread;

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
        return test_done("test_sim_mbox_share");
    }
    EC_WRITE_U32(value, 0);
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, SDO_INDEX, 0, value, 4));

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    if (!if_nametoindex(TAP_NAME)) {
        fprintf(stderr, "test_sim_mbox_share: no %s interface (TAP"
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

    /* Several of them: with the EoE handler taking CoE responses, one
     * lost response is enough to fail the configuration. */
    for (i = 0; i < NSTARTUP; i++) {
        TEST_CHECK_EQ(0, ecrt_slave_config_sdo32(sc, SDO_INDEX, 0,
                    i + 1 < NSTARTUP ? i : SDO_VALUE));
    }

    /* No ecrt_master_callbacks(): EoE rides in the cyclic frames. */
    TEST_CHECK_EQ(0, ecrt_master_activate(master));
    TEST_CHECK(ecrt_domain_data(domain) != NULL);
    if (!ecrt_domain_data(domain)) {
        goto out_release;
    }
    TEST_CHECK(wait_op(sc));
    {
        size_t size = 0;
        const uint8_t *od = sim_bus_od_data(bus, 0, SDO_INDEX, 0, &size);

        TEST_CHECK(od != NULL && size == 4);
        if (od && size == 4) {
            TEST_CHECK_EQ(SDO_VALUE, EC_READ_U32(od));
        }
    }

    sock = open_tap_socket(&ifindex);
    TEST_CHECK(sock >= 0);
    if (sock < 0) {
        goto out_release;
    }
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = (int) ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    /* Frames into the TAP all the time while the uploads run, a few on
     * their way at a time (more than the mailbox carries would only fill
     * the slave's queue): both protocols' mails are in the slave's mailbox
     * in turn. */
    TEST_CHECK_EQ(0, pthread_create(&thread, NULL, sdo_thread, NULL));
    for (waited = 0; waited < MAX_CYCLES && !sdo_done; waited++) {
        struct sockaddr_ll from;
        socklen_t fromlen = sizeof(from);
        ssize_t n;

        if (seq - echoed < 3) {
            make_frame(frame, seq++);
            sendto(sock, frame, sizeof(frame), 0,
                    (struct sockaddr *) &sll, sizeof(sll));
        }
        cycle();
        while ((n = recvfrom(sock, rbuf, sizeof(rbuf), MSG_DONTWAIT,
                        (struct sockaddr *) &from, &fromlen)) > 0) {
            fromlen = sizeof(from);
            if (from.sll_pkttype == PACKET_OUTGOING || n != FRAME_LEN
                    || rbuf[12] != (TEST_ETHERTYPE >> 8)
                    || rbuf[13] != (TEST_ETHERTYPE & 0xFF)) {
                continue;
            }
            make_frame(expect, rbuf[14]);
            if (!memcmp(rbuf, expect, FRAME_LEN) && echoed < seq) {
                echoed++;
            }
        }
    }
    while (!sdo_done) { /* never, unless the uploads hang */
        cycle();
    }
    pthread_join(thread, NULL);
    close(sock);

    TEST_CHECK_EQ(NSDO, sdo_ok);
    TEST_CHECK_EQ(0, sdo_failed);
    TEST_CHECK_EQ(0, sdo_wrong);
    /* EoE went on meanwhile (the last frames are still on their way) */
    TEST_CHECK(echoed >= NSDO);
    TEST_CHECK(seq - echoed <= 3);
    printf("test_sim_mbox_share: %u of %u frames echoed during %d SDO"
            " uploads\n", echoed, seq, NSDO);

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_mbox_share");
}
