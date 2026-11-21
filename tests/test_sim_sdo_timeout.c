/*****************************************************************************
 *
 *  Integration test: a slave configuration's SDO response timeout
 *  (ecrt_slave_config_sdo_timeout()).
 *
 *  A slave that takes no mail (sim_bus_mbox_deaf) makes an SDO transfer
 *  fail when its response timeout runs out: after the default 1000 ms
 *  without a timeout of its own, after the configured time with one (an
 *  ESI file's Mailbox/Timeout/ResponseTimeout, 2000 ms in most of them).
 *  0 puts back the default. A slave that answers is not affected.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <string.h>
#include <time.h>

#include "transport_sim.h"

#include "test.h"

#define VENDOR_ID 0x00000E17
#define PRODUCT_CODE 0x51340002

static const sim_slave_identity_t identities[1] = {
    { .vendor_id = VENDOR_ID, .product_code = PRODUCT_CODE,
      .revision_number = 1, .serial_number = 2, .alias = 0,
      .mbox_out_phys = 0x1000, .mbox_out_len = 128,
      .mbox_in_phys = 0x1080, .mbox_in_len = 128 },
};

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* the time an upload from the deaf slave takes to fail [ms] */
static double failing_upload(ec_master_t *master)
{
    uint8_t buf[8];
    size_t size;
    uint32_t abort_code = 0;
    double t0 = now_ms();
    int ret = ecrt_master_sdo_upload(master, 0, 0x2000, 0, buf, sizeof(buf),
            &size, &abort_code);
    TEST_CHECK(ret != 0);
    return now_ms() - t0;
}

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_slave_config_t *sc;
    uint8_t buf[8];
    size_t size;
    uint32_t abort_code;
    double ms;

    bus = sim_bus_create(1, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_sdo_timeout");
    }
    EC_WRITE_U32(buf, 0x12345678);
    TEST_CHECK_EQ(0, sim_bus_od_set(bus, 0, 0x2000, 0, buf, 4));

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        goto out_cleanup;
    }
    sc = ecrt_master_slave_config(master, 0, 0, VENDOR_ID, PRODUCT_CODE);
    TEST_CHECK(sc != NULL);
    if (!sc) {
        goto out_release;
    }

    /* an answering slave: unaffected by a short timeout */
    TEST_CHECK_EQ(0, ecrt_slave_config_sdo_timeout(sc, 300));
    TEST_CHECK_EQ(0, ecrt_master_sdo_upload(master, 0, 0x2000, 0, buf,
                sizeof(buf), &size, &abort_code));
    TEST_CHECK_EQ(0x12345678u, EC_READ_U32(buf));

    TEST_CHECK_EQ(0, sim_bus_mbox_deaf(bus, 0, 1));

    /* its own timeout */
    ms = failing_upload(master);
    TEST_CHECK(ms >= 250 && ms < 900);

    /* a longer one */
    TEST_CHECK_EQ(0, ecrt_slave_config_sdo_timeout(sc, 1600));
    ms = failing_upload(master);
    TEST_CHECK(ms >= 1500 && ms < 2600);

    /* 0: the default (1000 ms) */
    TEST_CHECK_EQ(0, ecrt_slave_config_sdo_timeout(sc, 0));
    ms = failing_upload(master);
    TEST_CHECK(ms >= 900 && ms < 1500);

    TEST_CHECK_EQ(0, sim_bus_mbox_deaf(bus, 0, 0));

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_sdo_timeout");
}
