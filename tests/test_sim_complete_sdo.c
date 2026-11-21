/*****************************************************************************
 *
 *  Integration test: configured SDOs with complete access from subindex 0
 *  and from subindex 1 (ecrt_slave_config_complete_sdo_from()).
 *
 *  An ESI file's InitCmd with CompleteAccess and SubIndex 1 gives the data of
 *  subindexes 1.. without subindex 0. The request's subindex byte must say
 *  where the data starts: the simulated slave keeps a complete-access
 *  download at the subindex it starts at. Subindexes other than 0 and 1 are
 *  refused.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <string.h>
#include <unistd.h>

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

static const uint8_t from1[6] = {0x00, 0x00, 0x00, 0x03, 0x03, 0x00};
static const uint8_t from0[3] = {0x02, 0x11, 0x22};

int main(void)
{
    sim_bus_t *bus;
    ec_master_t *master;
    ec_slave_config_t *sc;
    const uint8_t *od;
    size_t size;
    int i;

    bus = sim_bus_create(1, identities);
    TEST_CHECK(bus != NULL);
    if (!bus) {
        return test_done("test_sim_complete_sdo");
    }
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
    TEST_CHECK_EQ(0, ecrt_slave_config_complete_sdo_from(sc, 0x8080, 1, from1, sizeof(from1)));
    TEST_CHECK_EQ(0, ecrt_slave_config_complete_sdo(sc, 0x8090, from0, sizeof(from0)));
    TEST_CHECK(ecrt_slave_config_complete_sdo_from(sc, 0x80A0, 2, from0, 1) < 0);

    /* the master writes the configured SDOs when it configures the slave */
    TEST_CHECK_EQ(0, ecrt_master_activate(master));
    for (i = 0; i < 2000; i++) {
        ecrt_master_receive(master);
        ecrt_master_send(master);
        if (sim_bus_od_download_count(bus, 0) >= 2) {
            break;
        }
        usleep(1000);
    }
    TEST_CHECK(sim_bus_od_download_count(bus, 0) >= 2);

    od = sim_bus_od_data(bus, 0, 0x8080, 1, &size);    /* from subindex 1 */
    TEST_CHECK(od != NULL);
    if (od) {
        TEST_CHECK_EQ(sizeof(from1), size);
        TEST_CHECK_EQ(0, memcmp(od, from1, sizeof(from1)));
    }
    TEST_CHECK(sim_bus_od_data(bus, 0, 0x8080, 0, &size) == NULL);
    od = sim_bus_od_data(bus, 0, 0x8090, 0, &size);    /* from subindex 0, as before */
    TEST_CHECK(od != NULL);
    if (od) {
        TEST_CHECK_EQ(sizeof(from0), size);
    }

out_release:
    ecrt_release_master(master);
out_cleanup:
    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_complete_sdo");
}
