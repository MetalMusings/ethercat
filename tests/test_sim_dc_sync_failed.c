/*****************************************************************************
 *
 *  Integration test: a DC slave whose clock does not synchronize.
 *
 *  The reference clock's system time difference (0x092C) stays at 1 ms,
 *  so the master's DC sync check times out after 5 s. SYNC0 is started
 *  anyway, as before, and ecrt_slave_config_state() reports
 *  dc_sync_failed, so that the application can refuse to run.
 *
 *  Based on test_sim_dc:
 *
 *  Two DC-capable slaves (the first also carries process data). Verifies
 *  that the master selects a reference clock, measures transmission
 *  delays from the synthetic port receive times, programs the DC
 *  registers configured via ecrt_slave_config_dc() (AssignActivate
 *  0x0980, cycle time 0x09A0), and that the cyclic DC path works: the
 *  application time written via ecrt_master_sync_reference_clock_to()
 *  reaches the reference clock's system time register (0x0910) and is
 *  propagated to the other slave by the FRMW sync datagram;
 *  ecrt_master_reference_clock_time() returns it (minus the ref clock's
 *  transmission delay) and the sync monitor reports the system time
 *  difference register (0x092C).
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <unistd.h>

#include "transport_sim.h"

#include "test.h"

#define SM2_PHYS 0x1100
#define SM3_PHYS 0x1400

#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x51340005

#define MAX_CYCLES 10000 /* the check gives up after 5 s */

/** EtherCAT epoch-ish application time base (2000-01-01 in ns). */
#define APP_TIME_BASE 0x0102030405060708ULL

static const sim_slave_identity_t identities[2] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 5001, .alias = 0,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8,
      .dc_supported = 1 },
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE + 1,
      .revision_number = 1, .serial_number = 5002, .alias = 0,
      .dc_supported = 1 },
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

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_domain_t *domain;
    ec_slave_config_t *sc;
    ec_slave_config_state_t sc_state;
    unsigned int cycles;
    uint64_t app_time = APP_TIME_BASE;

    bus = sim_bus_create(2, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_dc_sync_failed");
    }
    /* 1 ms off the reference time, and it stays there */
    EC_WRITE_U32(sim_bus_slave_regs(bus, 0) + 0x092C, 1000000);

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    domain = ecrt_master_create_domain(master);
    sc = ecrt_master_slave_config(master, 0, 0, VENDOR_ID, PRODUCT_CODE);
    TEST_CHECK(domain != NULL);
    TEST_CHECK(sc != NULL);
    if (!domain || !sc) {
        goto out_release;
    }
    TEST_CHECK_EQ(0, ecrt_slave_config_pdos(sc, EC_END, syncs));
    TEST_CHECK(ecrt_slave_config_reg_pdo_entry(sc, 0x7000, 0x01, domain, NULL) >= 0);
    TEST_CHECK_EQ(0, ecrt_slave_config_dc(sc, 0x0300, 1000000, 0, 0, 0));
    TEST_CHECK_EQ(0, ecrt_master_activate(master));

    for (cycles = 0; cycles < MAX_CYCLES; cycles++) {
        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        app_time += 1000000;
        ecrt_master_application_time(master, app_time);
        ecrt_master_sync_reference_clock_to(master, app_time);
        ecrt_master_sync_slave_clocks(master);
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        usleep(1000);

        ecrt_slave_config_state(sc, &sc_state);
        if (cycles < 1000) {
            /* still checking (at most 5 s): not failed yet */
            TEST_CHECK_EQ(0, sc_state.dc_sync_failed);
        }
        if (sc_state.operational) {
            break;
        }
    }
    /* configured all the same, with SYNC0 started, but reported */
    TEST_CHECK_EQ(1, sc_state.operational);
    TEST_CHECK_EQ(1, sc_state.dc_sync_failed);
    TEST_CHECK_EQ(0x0300, EC_READ_U16(sim_bus_slave_regs(bus, 0) + 0x0980));

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_dc_sync_failed");
}
