/*****************************************************************************
 *
 *  Integration test: ecrt_master_frame_stats() on the simulated bus.
 *
 *  In OP with a complete working counter no frame is lost: after a receive,
 *  tx_frames - rx_frames is zero. Three frames whose replies are lost on the
 *  wire (sim_bus_lose_frames) show as three more sent than received, also
 *  after the master has logged its per-second statistics (the counts are
 *  never reset). A structure of an older, smaller size gets only its part.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "transport_sim.h"

#include "test.h"

#define SM2_PHYS 0x1100
#define SM3_PHYS 0x1400
#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x51340004
#define MAX_CYCLES 15000

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 5001, .alias = 0,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8 },
};

static ec_pdo_entry_info_t out_entries[] = { { 0x7000, 0x01, 16 } };
static ec_pdo_entry_info_t in_entries[] = { { 0x6000, 0x01, 16 } };
static ec_pdo_info_t rx_pdos[] = { { 0x1600, 1, out_entries } };
static ec_pdo_info_t tx_pdos[] = { { 0x1A00, 1, in_entries } };
static ec_sync_info_t syncs[] = {
    { 2, EC_DIR_OUTPUT, 1, rx_pdos, EC_WD_ENABLE },
    { 3, EC_DIR_INPUT, 1, tx_pdos, EC_WD_DISABLE },
    { 0xff, EC_DIR_INVALID, 0, NULL, EC_WD_DEFAULT }
};

static void cycle(ec_master_t *master, ec_domain_t *domain)
{
    ecrt_master_receive(master);
    ecrt_domain_process(domain);
    ecrt_domain_queue(domain);
    ecrt_master_send(master);
    usleep(1000);
}

static ec_master_frame_stats_t stats(ec_master_t *master)
{
    ec_master_frame_stats_t s;

    memset(&s, 0xA5, sizeof(s));
    s.size = sizeof(s);
    TEST_CHECK_EQ(0, ecrt_master_frame_stats(master, &s));
    TEST_CHECK_EQ(sizeof(s), s.size);
    return s;
}

int main(void)
{
    sim_bus_t *bus = sim_bus_create(1, identities);
    ec_master_t *master;
    ec_domain_t *domain;
    ec_slave_config_t *sc;
    ec_slave_config_state_t sc_state;
    ec_domain_state_t dstate;
    ec_master_frame_stats_t s, before;
    unsigned int cycles;
    int reached = 0;

    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_frame_stats");
    }
    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    /* bad arguments */
    TEST_CHECK_EQ(-EINVAL, ecrt_master_frame_stats(master, NULL));
    s.size = 4;
    TEST_CHECK_EQ(-EINVAL, ecrt_master_frame_stats(master, &s));

    domain = ecrt_master_create_domain(master);
    sc = ecrt_master_slave_config(master, 0, 0, VENDOR_ID, PRODUCT_CODE);
    TEST_CHECK(domain != NULL && sc != NULL);
    if (!domain || !sc) {
        goto out_release;
    }
    TEST_CHECK_EQ(0, ecrt_slave_config_pdos(sc, EC_END, syncs));
    TEST_CHECK(ecrt_slave_config_reg_pdo_entry(sc, 0x7000, 0x01, domain, NULL) >= 0);
    TEST_CHECK_EQ(0, ecrt_master_activate(master));

    for (cycles = 0; cycles < MAX_CYCLES && reached < 50; cycles++) {
        cycle(master, domain);
        ecrt_slave_config_state(sc, &sc_state);
        ecrt_domain_state(domain, &dstate);
        reached = sc_state.operational && dstate.wc_state == EC_WC_COMPLETE ? reached + 1 : 0;
    }
    TEST_CHECK(reached >= 50);

    /* in OP, nothing lost: every frame sent came back before the receive */
    ecrt_master_receive(master);
    before = stats(master);
    TEST_CHECK(before.tx_frames > 50);
    TEST_CHECK_EQ(0, before.reserved);
    TEST_CHECK_EQ(0, before.tx_errors);
    ecrt_domain_process(domain);
    ecrt_domain_queue(domain);
    ecrt_master_send(master);
    usleep(1000);

    /* three frames lost on the wire */
    sim_bus_lose_frames(bus, 3);
    for (cycles = 0; cycles < 1500; cycles++) {   /* > the datagram timeout, and a logged second */
        cycle(master, domain);
    }
    ecrt_master_receive(master);
    s = stats(master);
    TEST_CHECK(s.tx_frames > before.tx_frames + 1000);
    TEST_CHECK_EQ(3, (s.tx_frames - s.rx_frames) - (before.tx_frames - before.rx_frames));
    TEST_CHECK(s.timeouts >= before.timeouts);   /* the application's datagrams are sent again */
    TEST_CHECK_EQ(before.corrupted, s.corrupted);

    /* an older, smaller structure: only its part, and its size back */
    {
        struct {
            ec_master_frame_stats_t s;
            uint64_t guard;
        } small;
        size_t old = offsetof(ec_master_frame_stats_t, tx_errors);

        memset(&small, 0x5A, sizeof(small));
        small.s.size = old;
        TEST_CHECK_EQ(0, ecrt_master_frame_stats(master, &small.s));
        TEST_CHECK_EQ(old, small.s.size);
        TEST_CHECK(small.s.tx_frames >= s.tx_frames);
        TEST_CHECK_EQ(0x5A5A5A5A5A5A5A5AULL, small.s.tx_errors);   /* not written */
    }

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);
    return test_done("test_sim_frame_stats");
}
