/*****************************************************************************
 *
 *  Integration test: the host's address on an EoE slave's network device
 *  (ecrt_slave_config_eoe_host_address()).
 *
 *  The master creates the TAP device of an EoE slave at the bus scan, and
 *  anew at every rescan. With a host address in the slave's configuration
 *  the master gives the device that address and mask and brings it up. The
 *  test checks:
 *
 *  - without the call, the device has no address and stays down;
 *  - an address or mask that cannot be one is refused;
 *  - with it, the device has the address and the mask and is up once the
 *    slave is in OP;
 *  - after a link loss (rescan in OP, new device) it has them again.
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

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
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
#define HOST_IP "192.168.77.1"
#define HOST_MASK "255.255.255.0"

#define MAX_CYCLES 15000

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

static void cycle(void)
{
    ecrt_master_receive(master);
    ecrt_domain_process(domain);
    ecrt_domain_queue(domain);
    ecrt_master_send(master);
    usleep(1000);
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

/** What the TAP device has now: 1 if it has the address, the mask and is
 * up; 0 if it exists without all of that; -1 if there is no device. */
static int tap_has_host(void)
{
    struct ifreq ifr;
    struct sockaddr_in *sin = (struct sockaddr_in *) &ifr.ifr_addr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int ret = 0;

    if (fd < 0 || !if_nametoindex(TAP_NAME)) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", TAP_NAME);
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0
            && sin->sin_addr.s_addr == inet_addr(HOST_IP)
            && ioctl(fd, SIOCGIFNETMASK, &ifr) == 0
            && sin->sin_addr.s_addr == inet_addr(HOST_MASK)
            && ioctl(fd, SIOCGIFFLAGS, &ifr) == 0
            && (ifr.ifr_flags & IFF_UP)) {
        ret = 1;
    }
    close(fd);
    return ret;
}

/** One run of the master. With \a with_host the configuration gets the
 * host address. */
static void run(int with_host)
{
    ec_slave_config_t *sc;
    ec_tool_slave_sdo_entry_t entry;
    struct in_addr addr, mask, bad;
    unsigned int waited, cycles, al;
    int got;

    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (!master) {
        return;
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
    TEST_CHECK(ecrt_slave_config_reg_pdo_entry(sc, 0x7000, 0x01, domain,
                NULL) >= 0);
    TEST_CHECK(ecrt_slave_config_reg_pdo_entry(sc, 0x6000, 0x01, domain,
                NULL) >= 0);

    addr.s_addr = inet_addr(HOST_IP);
    mask.s_addr = inet_addr(HOST_MASK);
    if (with_host) {
        /* no address, no mask, a mask with a hole */
        bad.s_addr = 0;
        TEST_CHECK(ecrt_slave_config_eoe_host_address(sc, bad, mask) < 0);
        TEST_CHECK(ecrt_slave_config_eoe_host_address(sc, addr, bad) < 0);
        bad.s_addr = inet_addr("255.0.255.0");
        TEST_CHECK(ecrt_slave_config_eoe_host_address(sc, addr, bad) < 0);
        TEST_CHECK_EQ(0, ecrt_slave_config_eoe_host_address(sc, addr, mask));
    }

    /* nothing is set before the slave is configured */
    TEST_CHECK_EQ(0, tap_has_host());

    TEST_CHECK_EQ(0, ecrt_master_activate(master));
    TEST_CHECK(wait_op(sc));
    TEST_CHECK_EQ(with_host ? 1 : 0, tap_has_host());

    if (with_host) {
        /* Link loss: the rescan makes the device anew. */
        sim_bus_set_link(bus, 0);
        for (cycles = 0; cycles < 300; cycles++) {
            cycle();
        }
        TEST_CHECK_EQ(-1, tap_has_host());
        sim_bus_set_link(bus, 1);
        /* The address comes with the scan's end, before the slave is
         * configured again (a slave that stayed in OP would not be): the
         * slave is not in PREOP yet when the device first has it. */
        got = 0;
        al = 0;
        for (cycles = 0; cycles < MAX_CYCLES && got != 1; cycles++) {
            cycle();
            al = sim_bus_slave_al_state(bus, 0);
            got = tap_has_host();
        }
        TEST_CHECK_EQ(1, got);
        TEST_CHECK(al != 0x02);
        TEST_CHECK(wait_op(sc));
    }

out_release:
    ecrt_release_master(master);
}

int main(int argc, char **argv)
{
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
        return test_done("test_sim_eoe_host");
    }

    TEST_CHECK_EQ(0, ecrt_lib_init(NULL, NULL));
    master = ecrt_startup_master(0, sim_bus_transport(bus), NULL, 0, -1, -1);
    TEST_CHECK(master != NULL);
    if (master) {
        int tap = if_nametoindex(TAP_NAME) != 0;

        ecrt_release_master(master);
        if (!tap) {
            fprintf(stderr, "test_sim_eoe_host: no %s interface (TAP"
                    " creation needs CAP_NET_ADMIN) — skipping\n", TAP_NAME);
            ecrt_lib_cleanup();
            sim_bus_destroy(bus);
            return 77;
        }
        run(0);
        run(1);
    }

    ecrt_lib_cleanup();
    sim_bus_destroy(bus);

    return test_done("test_sim_eoe_host");
}
