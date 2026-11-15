/*****************************************************************************
 *
 *  Unit test: several readers of one slave mailbox (master/mailbox.c).
 *
 *  The rules by which the readers of a slave's mailbox (CoE, the EoE
 *  handler, ...) are kept apart, without a bus: who is told to fetch, what
 *  happens to a mail fetched by the wrong reader, and how it reaches the
 *  right one. tests/test_sim_mbox_share.c runs the same code with the real
 *  state machines and threads.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <stdlib.h>
#include <string.h>

#include "pal.h"

#include "master_globals.h"
#include "datagram.h"
#include "mailbox.h"
#include "master.h"
#include "slave.h"

#include "test.h"

#define STATION 0x03e9
#define TX_OFFSET 0x1080
#define TX_SIZE 128
#define MS 1000000ULL

static ec_master_t *master;
static ec_slave_t slave;

/** The address a datagram reads. */
static unsigned int address(const ec_datagram_t *dg)
{
    return EC_READ_U16(dg->address + 2);
}

/** A check datagram as it comes back: mailbox full or not, sent and
 * received at the given times (ms). */
static void checked(ec_datagram_t *dg, int full, unsigned int sent_ms)
{
    ec_slave_mbox_prepare_check(&slave, dg);
    if (full) {
        dg->data[5] |= 0x08;
    }
    dg->time_sent = sent_ms * MS;
    dg->time_received = sent_ms * MS + MS / 2;
    dg->working_counter = 1;
}

/** Fill a datagram prepared for the fetch, as it comes back with a mail
 * of the given protocol and first payload bytes. */
static void fetched(ec_datagram_t *dg, uint8_t type, uint8_t b0, uint8_t b1,
        unsigned int sent_ms)
{
    memset(dg->data, 0, dg->data_size);
    EC_WRITE_U16(dg->data, 10); /* length */
    EC_WRITE_U16(dg->data + 2, STATION);
    dg->data[5] = type;
    dg->data[EC_MBOX_HEADER_SIZE] = b0;
    dg->data[EC_MBOX_HEADER_SIZE + 1] = b1;
    dg->data[EC_MBOX_HEADER_SIZE + 9] = 0x5a;
    dg->time_sent = sent_ms * MS;
    dg->time_received = sent_ms * MS + MS / 2;
    dg->working_counter = 1;
}

int main(void)
{
    ec_datagram_t coe, eoe;
    uint8_t type, *data;
    size_t size;

    master = calloc(1, sizeof(*master));
    TEST_CHECK(master != NULL);
    if (!master) {
        return test_done("test_mbox_share");
    }
    memset(&slave, 0, sizeof(slave));
    slave.master = master;
    slave.station_address = STATION;
    slave.configured_tx_mailbox_offset = TX_OFFSET;
    slave.configured_tx_mailbox_size = TX_SIZE;
    slave.configured_rx_mailbox_offset = 0x1000;
    slave.configured_rx_mailbox_size = TX_SIZE;
    ec_datagram_init(&coe);
    ec_datagram_init(&eoe);

    /* A slave without EoE gets no places: its mailbox is read as it
     * always was. A mail of another protocol is handed to the caller. */
    slave.sii.mailbox_protocols = EC_MBOX_COE | EC_MBOX_FOE;
    TEST_CHECK_EQ(0, ec_slave_mbox_stash_alloc(&slave));
    TEST_CHECK(slave.mbox_stash == NULL);
    checked(&coe, 1, 1);
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));
    ec_slave_mbox_prepare_fetch(&slave, &coe, EC_MBOX_USER_COE);
    TEST_CHECK_EQ(TX_OFFSET, address(&coe));
    fetched(&coe, EC_MBOX_TYPE_FOE, 0x11, 0x22, 2);
    data = ec_slave_mbox_fetch(&slave, &coe, EC_MBOX_USER_COE, &type, &size);
    TEST_CHECK(data != NULL && !IS_ERR(data));
    TEST_CHECK_EQ(EC_MBOX_TYPE_FOE, type);

    slave.sii.mailbox_protocols = EC_MBOX_COE | EC_MBOX_EOE;

    /* Without the places (no mailbox found by the scan, or no memory):
     * as it always was, every reader sees the mailbox itself. */
    checked(&coe, 1, 1);
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));
    checked(&eoe, 1, 1);
    TEST_CHECK_EQ(1,
            ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_EOE_FRAME));

    TEST_CHECK_EQ(0, ec_slave_mbox_stash_alloc(&slave));
    TEST_CHECK(slave.mbox_stash != NULL);
    if (!slave.mbox_stash) {
        return test_done("test_mbox_share");
    }

    /* An empty mailbox: nothing for anyone. */
    checked(&coe, 0, 10);
    TEST_CHECK_EQ(0, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));

    /* Both see the full mailbox in the same cycle: one is told to fetch. */
    checked(&coe, 1, 20);
    checked(&eoe, 1, 20);
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));
    TEST_CHECK_EQ(0,
            ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_EOE_FRAME));

    /* Also when the other one's check came back earlier than the check
     * of the one that was told. */
    checked(&eoe, 1, 19);
    TEST_CHECK_EQ(0,
            ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_EOE_FRAME));

    /* The one told to fetch reads the mailbox, and gets its mail. */
    TEST_CHECK_EQ(0,
            ec_slave_mbox_prepare_fetch(&slave, &coe, EC_MBOX_USER_COE));
    TEST_CHECK_EQ(TX_OFFSET, address(&coe));
    TEST_CHECK_EQ(TX_SIZE, coe.data_size);
    fetched(&coe, EC_MBOX_TYPE_COE, 0x00, 0x30, 21);
    data = ec_slave_mbox_fetch(&slave, &coe, EC_MBOX_USER_COE, &type, &size);
    TEST_CHECK(data == coe.data + EC_MBOX_HEADER_SIZE);
    TEST_CHECK_EQ(EC_MBOX_TYPE_COE, type);
    TEST_CHECK_EQ(10, size);

    /* The other reader's check from that same cycle (sent before the
     * fetch came back) saw the mail that is gone now: not a mail. */
    checked(&eoe, 1, 21);
    TEST_CHECK_EQ(0,
            ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_EOE_FRAME));
    /* A later check that sees a full mailbox is a new mail. */
    checked(&eoe, 1, 22);
    TEST_CHECK_EQ(1,
            ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_EOE_FRAME));

    /* It is a CoE mail: put aside, the EoE handler gets nothing. */
    TEST_CHECK_EQ(0, ec_slave_mbox_prepare_fetch(&slave, &eoe,
                EC_MBOX_USER_EOE_FRAME));
    TEST_CHECK_EQ(TX_OFFSET, address(&eoe));
    fetched(&eoe, EC_MBOX_TYPE_COE, 0x11, 0x30, 23);
    data = ec_slave_mbox_fetch(&slave, &eoe, EC_MBOX_USER_EOE_FRAME, &type,
            &size);
    TEST_CHECK(data == NULL);

    /* CoE finds it with the mailbox empty, does not read the mailbox for
     * it (the datagram reads the status register), and gets the mail. */
    checked(&coe, 0, 24);
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));
    TEST_CHECK_EQ(0,
            ec_slave_mbox_prepare_fetch(&slave, &coe, EC_MBOX_USER_COE));
    TEST_CHECK_EQ(0x808, address(&coe));
    coe.working_counter = 1;
    coe.time_sent = 25 * MS;
    coe.time_received = 25 * MS + MS / 2;
    data = ec_slave_mbox_fetch(&slave, &coe, EC_MBOX_USER_COE, &type, &size);
    TEST_CHECK(data != NULL && !IS_ERR(data));
    if (data && !IS_ERR(data)) {
        TEST_CHECK_EQ(EC_MBOX_TYPE_COE, type);
        TEST_CHECK_EQ(10, size);
        TEST_CHECK_EQ(0x11, data[0]);
        TEST_CHECK_EQ(0x5a, data[9]);
    }
    /* Once. */
    checked(&coe, 0, 26);
    TEST_CHECK_EQ(0, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));

    /* An Ethernet frame fragment fetched by CoE goes to the EoE handler,
     * another EoE mail (set IP response, frame type 3) to the EoE state
     * machine. */
    checked(&coe, 1, 30);
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));
    ec_slave_mbox_prepare_fetch(&slave, &coe, EC_MBOX_USER_COE);
    fetched(&coe, EC_MBOX_TYPE_EOE, 0x00, 0x00, 31);
    TEST_CHECK(ec_slave_mbox_fetch(&slave, &coe, EC_MBOX_USER_COE, &type,
                &size) == NULL);
    checked(&coe, 1, 32);
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));
    ec_slave_mbox_prepare_fetch(&slave, &coe, EC_MBOX_USER_COE);
    fetched(&coe, EC_MBOX_TYPE_EOE, 0x03, 0x00, 33);
    TEST_CHECK(ec_slave_mbox_fetch(&slave, &coe, EC_MBOX_USER_COE, &type,
                &size) == NULL);
    checked(&eoe, 0, 34);
    TEST_CHECK_EQ(1,
            ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_EOE_PARAM));
    ec_slave_mbox_prepare_fetch(&slave, &eoe, EC_MBOX_USER_EOE_PARAM);
    eoe.working_counter = 1;
    data = ec_slave_mbox_fetch(&slave, &eoe, EC_MBOX_USER_EOE_PARAM, &type,
            &size);
    TEST_CHECK(data != NULL && !IS_ERR(data));
    if (data && !IS_ERR(data)) {
        TEST_CHECK_EQ(0x03, data[0]);
    }
    checked(&eoe, 0, 35);
    TEST_CHECK_EQ(1,
            ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_EOE_FRAME));
    ec_slave_mbox_prepare_fetch(&slave, &eoe, EC_MBOX_USER_EOE_FRAME);
    eoe.working_counter = 1;
    data = ec_slave_mbox_fetch(&slave, &eoe, EC_MBOX_USER_EOE_FRAME, &type,
            &size);
    TEST_CHECK(data != NULL && !IS_ERR(data));
    if (data && !IS_ERR(data)) {
        TEST_CHECK_EQ(EC_MBOX_TYPE_EOE, type);
        TEST_CHECK_EQ(0x00, data[0]);
    }

    /* A new request forgets an old response put aside (it would be taken
     * as the answer), but not an emergency message. */
    checked(&eoe, 1, 40);
    TEST_CHECK_EQ(1,
            ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_EOE_FRAME));
    ec_slave_mbox_prepare_fetch(&slave, &eoe, EC_MBOX_USER_EOE_FRAME);
    fetched(&eoe, EC_MBOX_TYPE_COE, 0x00, 0x30, 41); /* SDO response */
    TEST_CHECK(ec_slave_mbox_fetch(&slave, &eoe, EC_MBOX_USER_EOE_FRAME,
                &type, &size) == NULL);
    data = ec_slave_mbox_prepare_send(&slave, &coe, EC_MBOX_TYPE_COE, 10);
    TEST_CHECK(data != NULL && !IS_ERR(data));
    checked(&coe, 0, 42);
    TEST_CHECK_EQ(0, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));

    checked(&eoe, 1, 43);
    TEST_CHECK_EQ(1,
            ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_EOE_FRAME));
    ec_slave_mbox_prepare_fetch(&slave, &eoe, EC_MBOX_USER_EOE_FRAME);
    fetched(&eoe, EC_MBOX_TYPE_COE, 0x00, 0x10, 44); /* emergency */
    TEST_CHECK(ec_slave_mbox_fetch(&slave, &eoe, EC_MBOX_USER_EOE_FRAME,
                &type, &size) == NULL);
    ec_slave_mbox_prepare_send(&slave, &coe, EC_MBOX_TYPE_COE, 10);
    checked(&coe, 0, 45);
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));
    ec_slave_mbox_prepare_fetch(&slave, &coe, EC_MBOX_USER_COE);
    coe.working_counter = 1;
    data = ec_slave_mbox_fetch(&slave, &coe, EC_MBOX_USER_COE, &type, &size);
    TEST_CHECK(data != NULL && !IS_ERR(data));

    /* A reader told to fetch that never did (its datagram failed) starts
     * again with its next check. */
    checked(&coe, 1, 50);
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));
    checked(&coe, 1, 51);
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));
    /* If it never comes back, the others wait half a second, not for
     * ever. */
    checked(&eoe, 1, 400);
    TEST_CHECK_EQ(0,
            ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_EOE_FRAME));
    checked(&eoe, 1, 600);
    TEST_CHECK_EQ(1,
            ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_EOE_FRAME));
    ec_slave_mbox_prepare_fetch(&slave, &eoe, EC_MBOX_USER_EOE_FRAME);
    fetched(&eoe, EC_MBOX_TYPE_EOE, 0x00, 0x00, 601);
    data = ec_slave_mbox_fetch(&slave, &eoe, EC_MBOX_USER_EOE_FRAME, &type,
            &size);
    TEST_CHECK(data == eoe.data + EC_MBOX_HEADER_SIZE);

    /* A mailbox error mail (type 0) is an error for whoever fetched it. */
    checked(&coe, 1, 700);
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_COE));
    ec_slave_mbox_prepare_fetch(&slave, &coe, EC_MBOX_USER_COE);
    fetched(&coe, 0x00, 0x01, 0x00, 701);
    data = ec_slave_mbox_fetch(&slave, &coe, EC_MBOX_USER_COE, &type, &size);
    TEST_CHECK(IS_ERR(data));

    /* The bus scan reads the mailbox as it is. */
    checked(&coe, 1, 800);
    checked(&eoe, 1, 800);
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &coe, EC_MBOX_USER_ANY));
    TEST_CHECK_EQ(1, ec_slave_mbox_check(&slave, &eoe, EC_MBOX_USER_ANY));
    ec_slave_mbox_prepare_fetch(&slave, &coe, EC_MBOX_USER_ANY);
    TEST_CHECK_EQ(TX_OFFSET, address(&coe));

    ec_datagram_clear(&coe);
    ec_datagram_clear(&eoe);
    ec_slave_mbox_stash_free(&slave);
    TEST_CHECK(slave.mbox_stash == NULL);
    free(master);

    return test_done("test_mbox_share");
}
