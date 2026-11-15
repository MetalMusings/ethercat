/*****************************************************************************
 *
 *  Copyright (C) 2006-2008  Florian Pose, Ingenieurgemeinschaft IgH
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 *  Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 ****************************************************************************/

/**
   \file
   Mailbox functionality.
*/

/****************************************************************************/

#ifndef __EC_MAILBOX_H__
#define __EC_MAILBOX_H__

#include "slave.h"

/****************************************************************************/

/** Size of the mailbox header.
 */
#define EC_MBOX_HEADER_SIZE 6

/** Mailbox types.
 *
 * These are used in the 'Type' field of the mailbox header.
 */
enum {
    EC_MBOX_TYPE_EOE = 0x02,
    EC_MBOX_TYPE_COE = 0x03,
    EC_MBOX_TYPE_FOE = 0x04,
    EC_MBOX_TYPE_SOE = 0x05,
    EC_MBOX_TYPE_VOE = 0x0f,
};

/****************************************************************************/

/** Readers of a slave's mailbox.
 *
 * A slave has one mailbox for all its protocols, and the master has several
 * state machines reading it (CoE, the EoE handler in its own thread, ...).
 * Whoever fetches a mail that belongs to another reader puts it aside for
 * that reader (see ec_slave_mbox_fetch()). Every reader has one place.
 */
typedef enum {
    EC_MBOX_USER_COE,
    EC_MBOX_USER_FOE,
    EC_MBOX_USER_SOE,
    EC_MBOX_USER_VOE,
    EC_MBOX_USER_EOE_FRAME, /**< EoE handler: Ethernet frame fragments. */
    EC_MBOX_USER_EOE_PARAM, /**< EoE state machine: everything else. */
    EC_MBOX_USER_COUNT,
    EC_MBOX_USER_ANY = EC_MBOX_USER_COUNT /**< Takes any mail, as it is in
                                            the mailbox (bus scan). */
} ec_mbox_user_t;

/** States of a place for a mail put aside. */
enum {
    EC_MBOX_SLOT_EMPTY,
    EC_MBOX_SLOT_WRITING,
    EC_MBOX_SLOT_FULL,
    EC_MBOX_SLOT_READING
};

/** A mail put aside for one reader. */
typedef struct {
    EC_PAL_SHARED int state; /**< EC_MBOX_SLOT_*. */
    size_t size; /**< Size of the mail with its header. */
    uint8_t *data; /**< The mail (ec_mbox_stash_t::mem_size bytes). */
} ec_mbox_slot_t;

/** A slave's mails put aside, and who is fetching from its mailbox. */
struct ec_mbox_stash {
    EC_PAL_SHARED uint64_t token; /**< Zero, or the time (ns, the lowest
                                     byte replaced by the reader + 1) at
                                     which a reader saw a full mailbox and
                                     went to fetch it. */
    EC_PAL_SHARED uint64_t fetch_time; /**< When the last fetch datagram
                                          came back. */
    size_t mem_size; /**< Size of every slot's memory. */
    ec_mbox_slot_t slot[EC_MBOX_USER_COUNT];
};

/****************************************************************************/

int ec_slave_mbox_stash_alloc(ec_slave_t *);
void ec_slave_mbox_stash_free(ec_slave_t *);
void ec_slave_mbox_stash_drop(const ec_slave_t *, ec_mbox_user_t);

uint8_t *ec_slave_mbox_prepare_send(const ec_slave_t *, ec_datagram_t *,
                                    uint8_t, size_t);
int      ec_slave_mbox_prepare_check(const ec_slave_t *, ec_datagram_t *);
int      ec_slave_mbox_check(const ec_slave_t *, const ec_datagram_t *,
                             ec_mbox_user_t);
int      ec_slave_mbox_prepare_fetch(const ec_slave_t *, ec_datagram_t *,
                                     ec_mbox_user_t);
uint8_t *ec_slave_mbox_fetch(const ec_slave_t *, ec_datagram_t *,
                             ec_mbox_user_t, uint8_t *, size_t *);

/****************************************************************************/

#endif
