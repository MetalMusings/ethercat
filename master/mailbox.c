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


#include "pal.h"

#include "mailbox.h"
#include "datagram.h"
#include "master.h"

/* The kernel build keeps plain fields (EC_PAL_SHARED is empty there) and
 * reads a slave's mailbox under its own locking, so the handover between
 * readers needs no atomics. */
#ifndef EC_USPACE_MASTER
#define atomic_load(p) (*(p))
#define atomic_store(p, v) ((void) (*(p) = (v)))
#define atomic_compare_exchange_strong(p, e, v) \
    (*(p) == *(e) ? (*(p) = (v), 1) : (*(e) = *(p), 0))
#endif

/****************************************************************************/

/**
   Prepares a mailbox-send datagram.
   \return Pointer to mailbox datagram data, or ERR_PTR() code.
*/

uint8_t *ec_slave_mbox_prepare_send(const ec_slave_t *slave, /**< slave */
                                    ec_datagram_t *datagram, /**< datagram */
                                    uint8_t type, /**< mailbox protocol */
                                    size_t size /**< size of the data */
                                    )
{
    size_t total_size;
    int ret;

    if (unlikely(!slave->sii.mailbox_protocols)) {
        EC_SLAVE_ERR(slave, "Slave does not support mailbox"
                " communication!\n");
        return ERR_PTR(-EPROTONOSUPPORT);
    }

    total_size = EC_MBOX_HEADER_SIZE + size;

    if (unlikely(total_size > slave->configured_rx_mailbox_size)) {
        EC_SLAVE_ERR(slave, "Data size (%zu) does not fit in mailbox (%u)!\n",
                total_size, slave->configured_rx_mailbox_size);
        return ERR_PTR(-ENOBUFS);
    }

    ret = ec_datagram_fpwr(datagram, slave->station_address,
            slave->configured_rx_mailbox_offset,
            slave->configured_rx_mailbox_size);
    if (ret)
        return ERR_PTR(ret);

    // an answer put aside earlier is not the answer to this request
    switch (type) {
        case EC_MBOX_TYPE_COE:
            ec_slave_mbox_stash_drop(slave, EC_MBOX_USER_COE);
            break;
        case EC_MBOX_TYPE_FOE:
            ec_slave_mbox_stash_drop(slave, EC_MBOX_USER_FOE);
            break;
        case EC_MBOX_TYPE_SOE:
            ec_slave_mbox_stash_drop(slave, EC_MBOX_USER_SOE);
            break;
        default: // EoE: see ec_fsm_eoe; VoE reads without a request
            break;
    }

    EC_WRITE_U16(datagram->data,     size); // mailbox service data length
    EC_WRITE_U16(datagram->data + 2, slave->station_address); // station addr.
    EC_WRITE_U8 (datagram->data + 4, 0x00); // channel & priority
    EC_WRITE_U8 (datagram->data + 5, type); // underlying protocol type

    return datagram->data + EC_MBOX_HEADER_SIZE;
}

/****************************************************************************/

/** Time after which a reader that went to fetch the mailbox is taken to
 * have given up (an error path that never came back here). */
#define EC_MBOX_TOKEN_TIMEOUT_NS 500000000ULL

/** Address of the mailbox state register read by the check datagram. */
#define EC_MBOX_CHECK_ADDRESS 0x808

/****************************************************************************/

/** Allocates the places for mails put aside. Not in real-time context:
 * called by the bus scan when the slave is known to have a mailbox.
 *
 * Only a slave with EoE gets them: its EoE handler reads the mailbox from
 * another thread than the master's state machines, all the time. The other
 * protocols take turns in the master's thread, and such a slave's mailbox
 * is read as it always was (no places: every function here falls back).
 *
 * \return 0 on success or if the slave needs none, else < 0
 */
int ec_slave_mbox_stash_alloc(ec_slave_t *slave /**< slave */)
{
    ec_mbox_stash_t *stash;
    unsigned int i, k;

    if (slave->mbox_stash) {
        return 0;
    }
    if (!(slave->sii.mailbox_protocols & EC_MBOX_EOE)) {
        return 0;
    }

    if (!(stash = ec_alloc(sizeof(*stash)))) {
        goto out_err;
    }
    stash->token = 0;
    stash->fetch_time = 0;
    stash->mem_size = EC_MAX_DATA_SIZE;
    for (i = 0; i < EC_MBOX_USER_COUNT; i++) {
        stash->slot[i].wr = 0;
        stash->slot[i].rd = 0;
        for (k = 0; k < EC_MBOX_STASH_DEPTH; k++) {
            stash->slot[i].mail[k].state = EC_MBOX_SLOT_EMPTY;
            stash->slot[i].mail[k].size = 0;
            stash->slot[i].mail[k].data = NULL;
        }
    }
    for (i = 0; i < EC_MBOX_USER_COUNT; i++) {
        for (k = 0; k < EC_MBOX_STASH_DEPTH; k++) {
            if (!(stash->slot[i].mail[k].data = ec_alloc(stash->mem_size))) {
                slave->mbox_stash = stash;
                ec_slave_mbox_stash_free(slave); // frees what there is
                goto out_err;
            }
        }
    }

    slave->mbox_stash = stash;
    return 0;

out_err:
    EC_SLAVE_ERR(slave, "Failed to allocate memory for mailbox"
            " sharing.\n");
    return -ENOMEM;
}

/****************************************************************************/

/** Frees the places for mails put aside.
 */
void ec_slave_mbox_stash_free(ec_slave_t *slave /**< slave */)
{
    unsigned int i, k;

    if (!slave->mbox_stash) {
        return;
    }
    for (i = 0; i < EC_MBOX_USER_COUNT; i++) {
        for (k = 0; k < EC_MBOX_STASH_DEPTH; k++) {
            if (slave->mbox_stash->slot[i].mail[k].data) {
                ec_free(slave->mbox_stash->slot[i].mail[k].data);
            }
        }
    }
    ec_free(slave->mbox_stash);
    slave->mbox_stash = NULL;
}

/****************************************************************************/

/** The reader a mail belongs to.
 *
 * \return The reader, or EC_MBOX_USER_COUNT if the master has none.
 */
static ec_mbox_user_t ec_mbox_user_of(
        const uint8_t *mail, /**< mail with header */
        size_t data_size /**< size without header */
        )
{
    switch (EC_READ_U8(mail + 5) & 0x0F) {
        case EC_MBOX_TYPE_COE:
            return EC_MBOX_USER_COE;
        case EC_MBOX_TYPE_FOE:
            return EC_MBOX_USER_FOE;
        case EC_MBOX_TYPE_SOE:
            return EC_MBOX_USER_SOE;
        case EC_MBOX_TYPE_VOE:
            return EC_MBOX_USER_VOE;
        case EC_MBOX_TYPE_EOE:
            // frame type 0: a fragment of an Ethernet frame
            return data_size >= 1
                && (EC_READ_U8(mail + EC_MBOX_HEADER_SIZE) & 0x0F) == 0
                ? EC_MBOX_USER_EOE_FRAME : EC_MBOX_USER_EOE_PARAM;
        default:
            return EC_MBOX_USER_COUNT;
    }
}

/****************************************************************************/

/** Whether a mail put aside is a CoE emergency message.
 */
static int ec_mbox_is_coe_emergency(const ec_mbox_mail_t *slot)
{
    return slot->size >= EC_MBOX_HEADER_SIZE + 2
        && (EC_READ_U8(slot->data + 5) & 0x0F) == EC_MBOX_TYPE_COE
        && (EC_READ_U16(slot->data + EC_MBOX_HEADER_SIZE) >> 12 & 0x0F)
        == 0x01;
}

/****************************************************************************/

/** Forgets the mail put aside for a reader that is about to send a new
 * request: it would be taken as the answer. A CoE emergency message is no
 * answer and stays.
 */
void ec_slave_mbox_stash_drop(
        const ec_slave_t *slave, /**< slave */
        ec_mbox_user_t user /**< reader */
        )
{
    ec_mbox_stash_t *stash = slave->mbox_stash;
    ec_mbox_slot_t *slot;
    unsigned int i, wr;

    if (!stash || user >= EC_MBOX_USER_COUNT) {
        return;
    }

    slot = &stash->slot[user];
    wr = atomic_load(&slot->wr);
    for (i = atomic_load(&slot->rd); i != wr; i++) {
        ec_mbox_mail_t *mail = &slot->mail[i % EC_MBOX_STASH_DEPTH];
        int state = EC_MBOX_SLOT_FULL;

        if (!atomic_compare_exchange_strong(&mail->state, &state,
                    EC_MBOX_SLOT_READING)) {
            continue;
        }
        if (user == EC_MBOX_USER_COE && ec_mbox_is_coe_emergency(mail)) {
            atomic_store(&mail->state, EC_MBOX_SLOT_FULL);
            continue;
        }
        EC_SLAVE_DBG(slave, 1, "Dropped an old mailbox response (protocol"
                " 0x%02X) before a new request.\n",
                EC_READ_U8(mail->data + 5) & 0x0F);
        // an empty place between the others: passed over when taking
        atomic_store(&mail->state, EC_MBOX_SLOT_EMPTY);
    }
}

/****************************************************************************/

/** Puts a fetched mail aside for the reader it belongs to, behind those
 * already there. With all places taken the mail is lost.
 */
static void ec_mbox_stash_put(
        const ec_slave_t *slave, /**< slave */
        ec_mbox_user_t owner, /**< reader the mail belongs to */
        const uint8_t *mail, /**< mail with header */
        size_t size /**< size with header */
        )
{
    ec_mbox_slot_t *slot = &slave->mbox_stash->slot[owner];
    unsigned int wr = atomic_load(&slot->wr);
    ec_mbox_mail_t *place = &slot->mail[wr % EC_MBOX_STASH_DEPTH];
    int state = EC_MBOX_SLOT_EMPTY;

    // Only the reader holding the token fetches: one writer at a time.
    // A place the readers have passed (rd) is empty.
    if (wr - atomic_load(&slot->rd) >= EC_MBOX_STASH_DEPTH
            || !atomic_compare_exchange_strong(&place->state, &state,
                EC_MBOX_SLOT_WRITING)) {
        if (ec_log_ratelimit()) {
            EC_SLAVE_WARN(slave, "Mailbox response (protocol 0x%02X) lost:"
                    " %u before it are not read yet.\n",
                    EC_READ_U8(mail + 5) & 0x0F, EC_MBOX_STASH_DEPTH);
        }
        return;
    }

    memcpy(place->data, mail, size);
    place->size = size;
    atomic_store(&place->state, EC_MBOX_SLOT_FULL);
    atomic_store(&slot->wr, wr + 1);
}

/****************************************************************************/

/** The oldest mail put aside for a reader, places emptied by
 * ec_slave_mbox_stash_drop() passed over.
 *
 * \return The mail, or NULL if there is none.
 */
static ec_mbox_mail_t *ec_mbox_stash_first(ec_mbox_slot_t *slot)
{
    for (;;) {
        unsigned int rd = atomic_load(&slot->rd);
        ec_mbox_mail_t *mail;

        if (rd == atomic_load(&slot->wr)) {
            return NULL;
        }
        mail = &slot->mail[rd % EC_MBOX_STASH_DEPTH];
        if (atomic_load(&mail->state) != EC_MBOX_SLOT_EMPTY) {
            return mail;
        }
        atomic_compare_exchange_strong(&slot->rd, &rd, rd + 1);
    }
}

/****************************************************************************/

/** Takes the oldest mail put aside for a reader into the reader's datagram.
 *
 * \return 1 if there was one, else 0.
 */
static int ec_mbox_stash_take(
        const ec_slave_t *slave, /**< slave */
        ec_mbox_user_t user, /**< reader */
        ec_datagram_t *datagram /**< datagram with memory for a mail */
        )
{
    ec_mbox_slot_t *slot = &slave->mbox_stash->slot[user];
    ec_mbox_mail_t *mail = ec_mbox_stash_first(slot);
    unsigned int rd = atomic_load(&slot->rd);
    int state = EC_MBOX_SLOT_FULL;

    if (!mail || mail != &slot->mail[rd % EC_MBOX_STASH_DEPTH]
            || !atomic_compare_exchange_strong(&mail->state, &state,
                EC_MBOX_SLOT_READING)) {
        return 0;
    }

    if (mail->size > datagram->mem_size) { // never: see prepare_fetch
        atomic_store(&mail->state, EC_MBOX_SLOT_FULL);
        return 0;
    }

    memcpy(datagram->data, mail->data, mail->size);
    atomic_store(&mail->state, EC_MBOX_SLOT_EMPTY);
    atomic_compare_exchange_strong(&slot->rd, &rd, rd + 1);
    return 1;
}

/****************************************************************************/

/** The token value of a reader that goes to fetch at a time.
 */
static uint64_t ec_mbox_token(ec_mbox_user_t user, uint64_t time)
{
    return (time & ~0xFFULL) | (user + 1);
}

/****************************************************************************/

/**
   Prepares a datagram for checking the mailbox state.
   \todo Determine sync manager used for receive mailbox
   \return 0 in case of success, else < 0
*/

int ec_slave_mbox_prepare_check(const ec_slave_t *slave, /**< slave */
                                ec_datagram_t *datagram /**< datagram */
                                )
{
    int ret = ec_datagram_fprd(datagram, slave->station_address,
            EC_MBOX_CHECK_ADDRESS, 8);
    if (ret)
        return ret;

    ec_datagram_zero(datagram);
    return 0;
}

/****************************************************************************/

/** Processes a mailbox state checking datagram.
 *
 * A reader that gets 1 must go on with ec_slave_mbox_prepare_fetch() and
 * ec_slave_mbox_fetch(). With several readers of one mailbox, only one at
 * a time is told to fetch; the others get 0 and check again, as with an
 * empty mailbox.
 *
 * \return 1 if there is a mail to fetch for the reader, else 0
 */
int ec_slave_mbox_check(const ec_slave_t *slave, /**< slave */
                        const ec_datagram_t *datagram, /**< datagram */
                        ec_mbox_user_t user /**< reader */
                        )
{
    ec_mbox_stash_t *stash = slave->mbox_stash;
    int full = EC_READ_U8(datagram->data + 5) & 8 ? 1 : 0;
    uint64_t token, now = datagram->time_received;

    if (!stash || user >= EC_MBOX_USER_COUNT) {
        return full;
    }

    // a mail another reader fetched and put aside
    if (ec_mbox_stash_first(&stash->slot[user])) {
        return 1;
    }

    token = atomic_load(&stash->token);
    if ((token & 0xFF) == (uint64_t) user + 1) {
        // this reader went to fetch and never finished: start again
        if (atomic_compare_exchange_strong(&stash->token, &token, 0)) {
            token = 0;
        }
    }

    if (!full) {
        return 0;
    }

    // The mailbox was full when this datagram passed. If a fetch came
    // back since, it was that mail.
    if (datagram->time_sent <= atomic_load(&stash->fetch_time)) {
        return 0;
    }

    // Another reader is fetching, unless it went long ago. (Its check may
    // have come back after this one: not long ago.)
    if (token && (now < (token & ~0xFFULL)
                || now - (token & ~0xFFULL) < EC_MBOX_TOKEN_TIMEOUT_NS)) {
        return 0;
    }

    return atomic_compare_exchange_strong(&stash->token, &token,
            ec_mbox_token(user, now)) ? 1 : 0;
}

/****************************************************************************/

/** Prepares a datagram to fetch mailbox data.
 *
 * If the reader's mail was put aside by another reader, there is nothing
 * to fetch from the slave: the datagram reads the mailbox state instead,
 * which the reader's checks of the datagram accept as well.
 *
 * \return 0 in case of success, else < 0
 */
int ec_slave_mbox_prepare_fetch(const ec_slave_t *slave, /**< slave */
                                ec_datagram_t *datagram, /**< datagram */
                                ec_mbox_user_t user /**< reader */
                                )
{
    ec_mbox_stash_t *stash = slave->mbox_stash;
    int ret;

    if (stash && user < EC_MBOX_USER_COUNT
            && (atomic_load(&stash->token) & 0xFF) != (uint64_t) user + 1) {
        // not told to fetch: the mail is put aside
        ret = ec_datagram_prealloc(datagram, stash->mem_size);
        if (ret)
            return ret;
        return ec_slave_mbox_prepare_check(slave, datagram);
    }

    ret = ec_datagram_fprd(datagram, slave->station_address,
            slave->configured_tx_mailbox_offset,
            slave->configured_tx_mailbox_size);
    if (ret)
        return ret;

    ec_datagram_zero(datagram);
    return 0;
}

/****************************************************************************/

/**
   Mailbox error codes.
*/

const ec_code_msg_t mbox_error_messages[] = {
    {0x00000001, "MBXERR_SYNTAX"},
    {0x00000002, "MBXERR_UNSUPPORTEDPROTOCOL"},
    {0x00000003, "MBXERR_INVAILDCHANNEL"},
    {0x00000004, "MBXERR_SERVICENOTSUPPORTED"},
    {0x00000005, "MBXERR_INVALIDHEADER"},
    {0x00000006, "MBXERR_SIZETOOSHORT"},
    {0x00000007, "MBXERR_NOMOREMEMORY"},
    {0x00000008, "MBXERR_INVALIDSIZE"},
    {}
};

/****************************************************************************/

/** Processes received mailbox data.
 *
 * A mail that belongs to another reader is put aside for it; the caller
 * then gets NULL and has to check the mailbox again, as if its mail had not
 * come yet.
 *
 * \return Pointer to the received data, NULL if there was no mail for the
 * reader, or ERR_PTR() code.
 */
uint8_t *ec_slave_mbox_fetch(const ec_slave_t *slave, /**< slave */
                             ec_datagram_t *datagram, /**< datagram */
                             ec_mbox_user_t user, /**< reader */
                             uint8_t *type, /**< mailbox protocol */
                             size_t *size /**< size of the received data */
                             )
{
    ec_mbox_stash_t *stash = slave->mbox_stash;
    size_t data_size;

    if (stash && user < EC_MBOX_USER_COUNT) {
        if (EC_READ_U16(datagram->address + 2) == EC_MBOX_CHECK_ADDRESS) {
            // nothing was fetched: the mail is put aside (or was, and is
            // being replaced right now)
            if (!ec_mbox_stash_take(slave, user, datagram)) {
                return NULL;
            }
        } else {
            uint64_t token = atomic_load(&stash->token);

            atomic_store(&stash->fetch_time, datagram->time_received);
            if ((token & 0xFF) == (uint64_t) user + 1) {
                atomic_compare_exchange_strong(&stash->token, &token, 0);
            }
        }
    }

    data_size = EC_READ_U16(datagram->data);

    if (data_size + EC_MBOX_HEADER_SIZE > slave->configured_tx_mailbox_size) {
        EC_SLAVE_ERR(slave, "Corrupt mailbox response received!\n");
        ec_print_data(datagram->data, slave->configured_tx_mailbox_size);
        return ERR_PTR(-EPROTO);
    }

    *type = EC_READ_U8(datagram->data + 5) & 0x0F;
    *size = data_size;

    if (stash && user < EC_MBOX_USER_COUNT && *type != 0x00) {
        ec_mbox_user_t owner = ec_mbox_user_of(datagram->data, data_size);

        if (owner != user) {
            if (owner < EC_MBOX_USER_COUNT) {
                ec_mbox_stash_put(slave, owner, datagram->data,
                        data_size + EC_MBOX_HEADER_SIZE);
            } else {
                EC_SLAVE_WARN(slave, "Dropped a mailbox response of the"
                        " unsupported protocol 0x%02X.\n", *type);
            }
            return NULL;
        }
    }

    if (*type == 0x00) {
        const ec_code_msg_t *mbox_msg;
        uint16_t code = EC_READ_U16(datagram->data + 8);

        {
            const char *err_msg = NULL;
            for (mbox_msg = mbox_error_messages; mbox_msg->code; mbox_msg++) {
                if (mbox_msg->code == code) {
                    err_msg = mbox_msg->message;
                    break;
                }
            }
            if (err_msg) {
                EC_SLAVE_ERR(slave, "Mailbox error response received -"
                        " Code 0x%04X: \"%s\".\n", code, err_msg);
            } else {
                EC_SLAVE_ERR(slave, "Mailbox error response received -"
                        " Unknown error reply code 0x%04X.\n", code);
            }
        }

        if (slave->master->debug_level)
            ec_print_data(datagram->data + EC_MBOX_HEADER_SIZE, data_size);

        return ERR_PTR(-EPROTO);
    }

    return datagram->data + EC_MBOX_HEADER_SIZE;
}

/****************************************************************************/
