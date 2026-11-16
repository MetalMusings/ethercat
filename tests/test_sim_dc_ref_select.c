/*****************************************************************************
 *
 *  Integration test: the reference clock the application selects.
 *
 *  ETG.1020 (21.1): "The first device that should be synchronized is used
 *  as the reference clock". The master's own choice is the first
 *  DC-capable slave on the bus, also one the application does not
 *  configure; and the reference clock is always requested to OP, which an
 *  unconfigured drive refuses. So the application selects the clock with
 *  ecrt_master_select_reference_clock(). It does so after creating its
 *  slave configurations, which is after the bus scan, where the master
 *  finds the reference clock: the selection must count at once, not only
 *  from the next scan.
 *
 *  Two DC-capable slaves; the application configures only the second (with
 *  DC) and selects it as the reference clock. The first must stay in PREOP
 *  (no OP request), and the application time must reach the second's
 *  system time (0x0910) by the reference clock datagram, and not the
 *  first's: it lies before the clock, and the FRMW only writes onwards.
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

#define MAX_CYCLES 3000

#define APP_TIME_BASE 0x0102030405060708ULL

static const sim_slave_identity_t identities[2] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 6001, .alias = 0,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8,
      .dc_supported = 1 },
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE + 1,
      .revision_number = 1, .serial_number = 6002, .alias = 0,
      .sm2_phys = SM2_PHYS, .sm2_len = 8,
      .sm3_phys = SM3_PHYS, .sm3_len = 8,
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
        return test_done("test_sim_dc_ref_select");
    }

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }

    domain = ecrt_master_create_domain(master);
    sc = ecrt_master_slave_config(master, 0, 1, VENDOR_ID, PRODUCT_CODE + 1);
    TEST_CHECK(domain != NULL);
    TEST_CHECK(sc != NULL);
    if (!domain || !sc) {
        goto out_release;
    }
    TEST_CHECK_EQ(0, ecrt_slave_config_pdos(sc, EC_END, syncs));
    TEST_CHECK(ecrt_slave_config_reg_pdo_entry(sc, 0x7000, 0x01, domain, NULL) >= 0);
    TEST_CHECK_EQ(0, ecrt_slave_config_dc(sc, 0x0300, 1000000, 0, 0, 0));
    TEST_CHECK_EQ(0, ecrt_master_select_reference_clock(master, sc));
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
        if (sc_state.operational && cycles > 200) {
            break;
        }
    }
    TEST_CHECK_EQ(1, sc_state.operational);
    /* the unconfigured first slave got no OP request: still PREOP */
    TEST_CHECK_EQ(0x02, EC_READ_U16(sim_bus_slave_regs(bus, 0) + 0x0130) & 0x0F);
    /* the application time went to the selected clock, slave 1 */
    /* the application time (32 bit) went to the selected clock, slave 1;
     * slave 0 lies before it and the FRMW only writes onwards: nothing */
    {
        uint32_t t1 = EC_READ_U32(sim_bus_slave_regs(bus, 1) + 0x0910);
        TEST_CHECK(t1 == (uint32_t) app_time
                || t1 == (uint32_t) (app_time - 1000000));
        TEST_CHECK_EQ(0, EC_READ_U32(sim_bus_slave_regs(bus, 0) + 0x0910));
    }

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_dc_ref_select");
}
