/******************************************************************************
 *
 *  Copyright (C) 2006-2024  Florian Pose, Ingenieurgemeinschaft IgH
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
 *****************************************************************************/

/**
 * \file
 * XDP transport implementation using AF_XDP sockets.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <linux/if_link.h>

/* After the network headers (pal.h pulls linux/if.h, which must come
 * after net/if.h): ec_log for nonblocking logging from the cyclic
 * path. */
#include "pal.h"
#include <net/ethernet.h>
#include <xdp/xsk.h>
#include <xdp/libxdp.h>
#include <bpf/libbpf.h>

#include "pal_alloc.h"
#include "ectp.h"
#include "irq_pin.h"

/* bpf_prog_get_info_by_fd() and bpf_map_get_info_by_fd() came with libbpf
 * 1.4; before that (Debian 12 has 1.1) bpf_obj_get_info_by_fd() does both. */
#include <bpf/libbpf_version.h>
#if LIBBPF_MAJOR_VERSION > 1 || (LIBBPF_MAJOR_VERSION == 1 && LIBBPF_MINOR_VERSION >= 4)
#define EC_BPF_INFO_BY_FD(fd, info, len) \
    _Generic(*(info), struct bpf_prog_info: bpf_prog_get_info_by_fd, \
             struct bpf_map_info: bpf_map_get_info_by_fd)(fd, info, len)
#else
#define EC_BPF_INFO_BY_FD(fd, info, len) bpf_obj_get_info_by_fd(fd, info, len)
#endif

/****************************************************************************/

/*
 * Frame pool and ring sizing for EtherCAT XDP transport.
 *
 * EtherCAT is a synchronous request/reply protocol: at most one TX frame and
 * a handful of RX frames are in flight per cycle.  A small UMEM pool is
 * therefore sufficient:
 *
 *   NUM_FRAMES    = 64  → 64 × 4 KiB = 256 KiB UMEM (vs. 16 MB at 4096)
 *   XDP_FQ_FILL_SIZE    = 32  → initial fill-queue population (half the pool,
 *                               leaving headroom for TX / CQ in-flight frames)
 *   XDP_RX/TX_RING_SIZE = 32  → explicit ring sizes (replacing the 2048-entry
 *                               XSK_RING_*_DEFAULT_NUM_DESCS defaults)
 *
 * Invariant: NUM_FRAMES must be at least XDP_FQ_FILL_SIZE + some headroom so
 * the free pool is never exhausted while frames are in-flight in TX/CQ/RX.
 */
#define NUM_FRAMES 64
#define FRAME_SIZE XSK_UMEM__DEFAULT_FRAME_SIZE
#define INVALID_UMEM_FRAME UINT64_MAX
#define FQ_REFILL_MAX 64
#define CQ_DRAIN_MAX 8
#define XDP_RX_RING_SIZE 32
#define XDP_TX_RING_SIZE 32
#define XDP_FQ_FILL_SIZE 32

_Static_assert(NUM_FRAMES >= XDP_FQ_FILL_SIZE * 2,
    "NUM_FRAMES must be at least twice XDP_FQ_FILL_SIZE to leave headroom");

/** Private data for XDP transport */
typedef struct {
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
    struct xsk_umem *umem;
    struct xsk_socket *xsk;
    void *umem_buffer;
    uint64_t umem_frame_addr[NUM_FRAMES];
    uint32_t umem_frame_free;

    int if_index;                      /**< Interface index */
    uint8_t mac_addr[6];               /**< Interface MAC address */
    int ioctl_sock;                    /**< Socket for ioctl operations (link state, etc.) */
    uint32_t xdp_flags;                /**< XDP mode flags used during open */
    int map_fd;                        /**< The program's XSKMAP (-1: none) */
    uint64_t fq_refill_pending[FQ_REFILL_MAX]; /**< Frames awaiting FQ refill */
    uint32_t fq_refill_count;          /**< Number of pending refill frames */
    ec_irq_set_t irqs;                 /**< Cached NIC IRQs (count 0 = not discovered) */
    uint64_t rx_foreign;               /**< Non-EtherCAT frames dropped on RX */
} ec_transport_xdp_t;

/****************************************************************************/

/**
 * Allocate a UMEM frame.
 */
static uint64_t xsk_alloc_umem_frame(ec_transport_xdp_t *xdp)
{
    uint64_t frame;
    if (xdp->umem_frame_free == 0) {
        return INVALID_UMEM_FRAME;
    }

    frame = xdp->umem_frame_addr[--xdp->umem_frame_free];
    xdp->umem_frame_addr[xdp->umem_frame_free] = INVALID_UMEM_FRAME;
    return frame;
}

/****************************************************************************/

/**
 * Free a UMEM frame.
 */
static void xsk_free_umem_frame(ec_transport_xdp_t *xdp, uint64_t frame)
{
    if (xdp->umem_frame_free >= NUM_FRAMES) {
        /* Reachable from the cyclic path: ec_log is nonblocking there
         * (lock-free ring / application callback), fprintf is not. */
        ec_log(EC_LOG_WARNING,
                "XDP: UMEM frame pool overflow - frame will be leaked\n");
        return;
    }
    xdp->umem_frame_addr[xdp->umem_frame_free++] = frame;
}

/****************************************************************************/

/**
 * Process completion queue and free completed frames.
 */
static void process_completion_queue(ec_transport_xdp_t *xdp, unsigned int max_frames)
{
    uint32_t idx_cq = 0;
    unsigned int rcvd;
    unsigned int i;

    rcvd = xsk_ring_cons__peek(&xdp->cq, max_frames, &idx_cq);
    if (rcvd > 0) {
        for (i = 0; i < rcvd; i++) {
            uint64_t addr = *xsk_ring_cons__comp_addr(&xdp->cq, idx_cq++);
            xsk_free_umem_frame(xdp, addr);
        }
        xsk_ring_cons__release(&xdp->cq, rcvd);
    }
}

/****************************************************************************/

/* The master's own XDP program, kept on the interface.
 *
 * Attaching or removing an XDP program restarts the network card on
 * common drivers (igc, igb: the link goes down for 1.5-2 s while it is
 * negotiated again; igc even when one program replaces another). So the
 * program is attached once and left on the interface when a master
 * closes: the next master (the free ec_master, or an application such as
 * LinuxCNC) finds it and only binds its AF_XDP socket to it, which does
 * not touch the link. The kernel removes a closed socket from the map,
 * also when its process was killed.
 *
 * The program sends EtherCAT frames (ethertype 0x88A4) to the socket in
 * the XSKMAP at the frame's receive queue, and everything else to the
 * kernel (XDP_PASS), also EtherCAT frames while no socket is bound: a
 * program left on the card without a master is harmless, and the raw
 * transport still works with it. "sudo ip link set dev <if> xdp off"
 * removes it.
 *
 * It is written as BPF instructions, so no BPF compiler is needed to
 * build the master. Its name carries a version: change the name with the
 * code, and a master finding an older version replaces it (one restart).
 */
#define EC_XDP_PROG_NAME "ethercat_xdp1"
#define EC_XDP_MAP_NAME "ethercat_xsks"
#define EC_XDP_MAP_ENTRIES 64

#define EC_XDP_INSN(c, d, s, o, i) ((struct bpf_insn) { \
    .code = (c), .dst_reg = (d), .src_reg = (s), .off = (o), .imm = (i) })

/** Load the program with a new XSKMAP; returns the program fd. */
static int xdp_load_program(int *map_fd_out)
{
    char log[2048] = "";
    LIBBPF_OPTS(bpf_prog_load_opts, opts,
            .log_buf = log, .log_size = sizeof(log), .log_level = 0);
    int map_fd, prog_fd;

    map_fd = bpf_map_create(BPF_MAP_TYPE_XSKMAP, EC_XDP_MAP_NAME,
            sizeof(int), sizeof(int), EC_XDP_MAP_ENTRIES, NULL);
    if (map_fd < 0) {
        EC_ERR("XDP: cannot create the socket map: %s\n", strerror(errno));
        return -errno;
    }
    {
        const struct bpf_insn insns[] = {
            /* r6 = ctx; r2 = data; r3 = data_end */
            EC_XDP_INSN(BPF_ALU64 | BPF_MOV | BPF_X, 6, 1, 0, 0),
            EC_XDP_INSN(BPF_LDX | BPF_MEM | BPF_W, 2, 1, 0, 0),
            EC_XDP_INSN(BPF_LDX | BPF_MEM | BPF_W, 3, 1, 4, 0),
            /* shorter than an Ethernet header: pass */
            EC_XDP_INSN(BPF_ALU64 | BPF_MOV | BPF_X, 4, 2, 0, 0),
            EC_XDP_INSN(BPF_ALU64 | BPF_ADD | BPF_K, 4, 0, 0, ETH_HLEN),
            EC_XDP_INSN(BPF_JMP | BPF_JGT | BPF_X, 4, 3, 8, 0),
            /* ethertype (network order, read as little endian) */
            EC_XDP_INSN(BPF_LDX | BPF_MEM | BPF_H, 5, 2, 12, 0),
            EC_XDP_INSN(BPF_JMP | BPF_JNE | BPF_K, 5, 0, 6,
                    ((EC_TRANSPORT_ETHERTYPE & 0xff) << 8)
                    | (EC_TRANSPORT_ETHERTYPE >> 8)),
            /* return bpf_redirect_map(map, rx_queue_index, XDP_PASS) */
            EC_XDP_INSN(BPF_LDX | BPF_MEM | BPF_W, 2, 6, 16, 0),
            EC_XDP_INSN(BPF_LD | BPF_DW | BPF_IMM, 1, BPF_PSEUDO_MAP_FD, 0,
                    map_fd),
            EC_XDP_INSN(0, 0, 0, 0, 0),
            EC_XDP_INSN(BPF_ALU64 | BPF_MOV | BPF_K, 3, 0, 0, XDP_PASS),
            EC_XDP_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_redirect_map),
            EC_XDP_INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0),
            /* pass: */
            EC_XDP_INSN(BPF_ALU64 | BPF_MOV | BPF_K, 0, 0, 0, XDP_PASS),
            EC_XDP_INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0),
        };
        prog_fd = bpf_prog_load(BPF_PROG_TYPE_XDP, EC_XDP_PROG_NAME, "GPL",
                insns, sizeof(insns) / sizeof(insns[0]), &opts);
    }
    if (prog_fd < 0) {
        int err = -errno;
        EC_ERR("XDP: cannot load the program: %s%s%s\n", strerror(-err),
                log[0] ? "; verifier: " : "", log);
        close(map_fd);
        return err;
    }
    (void)fcntl(map_fd, F_SETFD, FD_CLOEXEC);
    (void)fcntl(prog_fd, F_SETFD, FD_CLOEXEC);
    *map_fd_out = map_fd;
    return prog_fd;
}

/** Whether program \a prog_id is this master's program.
 *
 * \return 1 and its map's fd in \a map_fd_out if it is, 0 if not,
 *         negative on error. */
static int xdp_is_ours(uint32_t prog_id, int *map_fd_out)
{
    struct bpf_prog_info info;
    struct bpf_map_info map_info;
    uint32_t len = sizeof(info), map_len = sizeof(map_info), map_id = 0;
    int prog_fd, map_fd, ret = 0;

    prog_fd = bpf_prog_get_fd_by_id(prog_id);
    if (prog_fd < 0)
        return -errno;
    memset(&info, 0, sizeof(info));
    info.nr_map_ids = 1;
    info.map_ids = (uint64_t)(unsigned long)&map_id;
    if (EC_BPF_INFO_BY_FD(prog_fd, &info, &len) == 0
            && strcmp(info.name, EC_XDP_PROG_NAME) == 0
            && info.nr_map_ids == 1) {
        map_fd = bpf_map_get_fd_by_id(map_id);
        memset(&map_info, 0, sizeof(map_info));
        if (map_fd >= 0 && EC_BPF_INFO_BY_FD(map_fd, &map_info,
                    &map_len) == 0
                && map_info.type == BPF_MAP_TYPE_XSKMAP) {
            (void)fcntl(map_fd, F_SETFD, FD_CLOEXEC);
            *map_fd_out = map_fd;
            ret = 1;
        } else if (map_fd >= 0) {
            close(map_fd);
        }
    }
    close(prog_fd);
    return ret;
}

/** Remove every XDP program from the interface (each a link restart). */
static void xdp_detach_all(int if_index, const char *interface,
        const struct bpf_xdp_query_opts *opts)
{
    int failed = 0;

    if (opts->skb_prog_id
            && bpf_xdp_detach(if_index, XDP_FLAGS_SKB_MODE, NULL) < 0)
        failed = 1;
    if (opts->drv_prog_id
            && bpf_xdp_detach(if_index, XDP_FLAGS_DRV_MODE, NULL) < 0)
        failed = 1;
    if (opts->hw_prog_id
            && bpf_xdp_detach(if_index, XDP_FLAGS_HW_MODE, NULL) < 0)
        failed = 1;
    if (failed)
        EC_ERR("XDP: could not remove the program on %s; try: sudo ip link"
                " set dev %s xdp off\n", interface, interface);
}

/**
 * Find this master's program on the interface in the wanted mode, or put
 * it there.
 *
 * The caller owns the interface (the masters share a lock per
 * interface). The program found in the wanted mode is used as it is.
 * Anything else is removed first: another program (libxdp's from an
 * older master, someone else's) or this program in the other mode
 * (the transport changed between xdp-skb and xdp-native).
 *
 * \return 0 with xdp->map_fd set, or a negative error code.
 */
static int xdp_prepare_program(ec_transport_xdp_t *xdp,
        const char *interface)
{
    LIBBPF_OPTS(bpf_xdp_query_opts, opts);
    uint32_t want_id, other_id;
    int prog_fd, map_fd = -1, ret;

    ret = bpf_xdp_query(xdp->if_index, 0, &opts);
    if (ret < 0) {
        EC_ERR("XDP: cannot query %s: %s\n", interface, strerror(-ret));
        return ret;
    }
    if (xdp->xdp_flags & XDP_FLAGS_DRV_MODE) {
        want_id = opts.drv_prog_id;
        other_id = opts.skb_prog_id | opts.hw_prog_id;
    } else {
        want_id = opts.skb_prog_id;
        other_id = opts.drv_prog_id | opts.hw_prog_id;
    }

    if (want_id && !other_id && xdp_is_ours(want_id, &map_fd) == 1) {
        EC_INFO("XDP: using the program already on %s (no link"
                " restart)\n", interface);
        xdp->map_fd = map_fd;
        return 0;
    }

    if (opts.attach_mode != XDP_ATTACHED_NONE) {
        EC_WARN("XDP: replacing the program on %s (the link restarts)\n",
                interface);
        xdp_detach_all(xdp->if_index, interface, &opts);
    }

    prog_fd = xdp_load_program(&map_fd);
    if (prog_fd < 0)
        return prog_fd;
    ret = bpf_xdp_attach(xdp->if_index, prog_fd, xdp->xdp_flags, NULL);
    close(prog_fd);  /* the interface holds the program now */
    if (ret < 0) {
        EC_ERR("XDP: cannot attach the program to %s: %s\n", interface,
                strerror(-ret));
        close(map_fd);
        return ret;
    }
    EC_INFO("XDP: program attached to %s; it stays there for the next"
            " master (sudo ip link set dev %s xdp off removes it)\n",
            interface, interface);
    xdp->map_fd = map_fd;
    return 0;
}

/****************************************************************************/
/* libbpf and libxdp messages                                               */
/*                                                                          */
/* Both libraries print to stderr by default, past the master's log and so */
/* past an application's log callback (ecrt_lib_init()). They are routed   */
/* into ec_log() instead. Only reachable while a socket is opened or the   */
/* program is loaded or removed, never from the cyclic path.               */
/****************************************************************************/

/* What libbpf and libxdp say on every start on a working system: the ELF
 * sections libbpf does not know (libxdp's metadata) and libxdp's fallback
 * to a single program when no bpffs is mounted. And what libxdp said on
 * every close by an application that has dropped its privileges (LinuxCNC)
 * while libxdp managed the program; the transport attaches its own now
 * (xdp_prepare_program()), but an older libxdp may still say it. Not
 * logged. */
static const char *const xdp_lib_quiet[] = {
    "skipping unrecognized data section",
    "Can't use dispatcher without a working bpffs",
    "Falling back to loading single prog without dispatcher",
    "Error occurred when decrementing xsk XDP prog refcount",
    NULL
};

static int ec_xdp_lib_message(int ec_level, const char *fmt, va_list ap)
{
    char msg[256];
    size_t n;
    unsigned int i;

    vsnprintf(msg, sizeof(msg), fmt, ap);
    for (i = 0; xdp_lib_quiet[i]; i++) {
        if (strstr(msg, xdp_lib_quiet[i]))
            return 0;
    }
    n = strlen(msg);
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
        msg[--n] = '\0';
    if (n == 0)
        return 0;
    ec_log(ec_level, "EtherCAT%s: %s\n",
            ec_level <= EC_LOG_ERR ? " ERROR" :
            ec_level == EC_LOG_WARNING ? " WARNING" : "", msg);
    return 0;
}

static int xdp_libbpf_print(enum libbpf_print_level level, const char *fmt,
        va_list ap)
{
    if (level == LIBBPF_DEBUG)
        return 0;
    return ec_xdp_lib_message(level == LIBBPF_WARN ? EC_LOG_WARNING
            : EC_LOG_INFO, fmt, ap);
}

static int xdp_libxdp_print(enum libxdp_print_level level, const char *fmt,
        va_list ap)
{
    if (level == LIBXDP_DEBUG)
        return 0;
    return ec_xdp_lib_message(level == LIBXDP_WARN ? EC_LOG_WARNING
            : EC_LOG_INFO, fmt, ap);
}

static void xdp_lib_log_to_master(void)
{
    libbpf_set_print(xdp_libbpf_print);
    libxdp_set_print(xdp_libxdp_print);
}

/**
 * Open XDP transport on interface.
 */
static int xdp_open(ec_transport_t *transport, const char *interface,
    uint32_t xdp_flags, uint16_t bind_flags)
{
    ec_transport_xdp_t *xdp;
    struct ifreq ifr;
    struct xsk_socket_config cfg;
    uint32_t idx;
    uint64_t addr;
    int ret;
    int sock_fd;
    int i;

    /* Allocate private data */
    xdp = calloc(1, sizeof(ec_transport_xdp_t));
    if (!xdp) {
        return -ENOMEM;
    }

    xdp->ioctl_sock = -1;
    xdp->map_fd = -1;
    xdp->xdp_flags = xdp_flags;
    transport->priv = xdp;

    /* Create temporary socket to get interface info */
    sock_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock_fd < 0) {
        ret = -errno;
        EC_ERR("XDP: Failed to create socket: %s\n", strerror(errno));
        goto err_free;
    }

    /* Get interface index */
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(sock_fd, SIOCGIFINDEX, &ifr) < 0) {
        ret = -errno;
        EC_ERR("XDP: Failed to get interface index for %s: %s\n",
                interface, strerror(errno));
        close(sock_fd);
        goto err_free;
    }
    xdp->if_index = ifr.ifr_ifindex;

    /* Get MAC address */
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(sock_fd, SIOCGIFHWADDR, &ifr) < 0) {
        ret = -errno;
        EC_ERR("XDP: Failed to get MAC address for %s: %s\n",
                interface, strerror(errno));
        close(sock_fd);
        goto err_free;
    }
    memcpy(xdp->mac_addr, ifr.ifr_hwaddr.sa_data, 6);

    /* Store socket for later ioctl operations */
    xdp->ioctl_sock = sock_fd;

    /* Allocate UMEM buffer: page-aligned, prefaulted, and mlocked so that
     * no page faults or swapping occur on the RT cyclic TX/RX path. */
    xdp->umem_buffer = ec_rt_alloc_aligned(getpagesize(), NUM_FRAMES * FRAME_SIZE);
    if (!xdp->umem_buffer) {
        EC_ERR("XDP: Failed to allocate UMEM buffer\n");
        ret = -ENOMEM;
        goto err_free;
    }

    /* Initialize UMEM frame pool */
    for (i = 0; i < NUM_FRAMES; i++) {
        xdp->umem_frame_addr[i] = i * FRAME_SIZE;
    }
    xdp->umem_frame_free = NUM_FRAMES;

    /* Create UMEM */
    ret = xsk_umem__create(&xdp->umem, xdp->umem_buffer, NUM_FRAMES * FRAME_SIZE,
                          &xdp->fq, &xdp->cq, NULL);
    if (ret) {
        EC_ERR("XDP: Failed to create UMEM: %s\n", strerror(-ret));
        goto err_free_buffer;
    }

    /* Configure socket
     * Use SKB mode and copy mode for maximum compatibility across different
     * network drivers and kernel versions. While native XDP mode with zero-copy
     * would provide better performance, SKB mode ensures the transport works
     * on all network interfaces without requiring driver-specific XDP support.
     */
    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_size = XDP_RX_RING_SIZE;
    cfg.tx_size = XDP_TX_RING_SIZE;
    cfg.xdp_flags = xdp_flags;
    cfg.bind_flags = bind_flags;
    /* The program is this transport's own (xdp_prepare_program()), not
     * libxdp's: libxdp would attach and detach one per socket. */
    cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;

    ret = xdp_prepare_program(xdp, interface);
    if (ret) {
        goto err_free_umem;
    }

    /* Create XSK socket (queue 0). The kernel frees the queue of a socket
     * that was just closed (the previous master's) a little later, and
     * until then the bind fails with EBUSY: try again for up to 2 s. */
    for (i = 0; ; i++) {
        ret = xsk_socket__create(&xdp->xsk, interface, 0, xdp->umem,
                                &xdp->rx, &xdp->tx, &cfg);
        if (ret != -EBUSY || i >= 200) {
            break;
        }
        usleep(10000);
    }
    if (ret) {
        EC_ERR("XDP: Failed to create XSK socket: %s\n", strerror(-ret));
        goto err_free_umem;
    }

    /* Bind the socket to the program: frames on queue 0 come here. */
    ret = xsk_socket__update_xskmap(xdp->xsk, xdp->map_fd);
    if (ret) {
        EC_ERR("XDP: cannot add the socket to the program's map: %s\n",
                strerror(-ret));
        goto err_free_socket;
    }

    /* libxdp creates its fds without CLOEXEC. A forked child (a [FILTER]
     * converter, a user M-code script) must not inherit handles that can
     * inject raw frames onto the fieldbus. */
    (void)fcntl(xsk_socket__fd(xdp->xsk), F_SETFD, FD_CLOEXEC);
    (void)fcntl(xsk_umem__fd(xdp->umem), F_SETFD, FD_CLOEXEC);

    /* Populate fill queue */
    ret = xsk_ring_prod__reserve(&xdp->fq, XDP_FQ_FILL_SIZE, &idx);
    if (ret != XDP_FQ_FILL_SIZE) {
        EC_ERR("XDP: Failed to reserve fill queue\n");
        goto err_free_socket;
    }

    for (i = 0; i < XDP_FQ_FILL_SIZE; i++) {
        addr = xsk_alloc_umem_frame(xdp);
        *xsk_ring_prod__fill_addr(&xdp->fq, idx++) = addr;
    }

    xsk_ring_prod__submit(&xdp->fq, XDP_FQ_FILL_SIZE);

    /* Discover NIC IRQs for affinity pinning (best-effort, non-fatal) */
    ec_irq_discover(interface, &xdp->irqs);

    return 0;

err_free_socket:
    xsk_socket__delete(xdp->xsk);
err_free_umem:
    xsk_umem__delete(xdp->umem);
err_free_buffer:
    ec_rt_free(xdp->umem_buffer, NUM_FRAMES * FRAME_SIZE);
err_free:
    if (xdp->map_fd >= 0) {
        close(xdp->map_fd);
    }
    if (xdp->ioctl_sock >= 0) {
        close(xdp->ioctl_sock);
    }
    free(xdp);
    transport->priv = NULL;
    return ret;
}

/**
 * Open XDP transport on interface (SKB mode).
 */
static int xdp_open_skb(ec_transport_t *transport, const char *interface)
{
    xdp_lib_log_to_master();
    return xdp_open(transport, interface, XDP_FLAGS_SKB_MODE,
                    XDP_COPY | XDP_USE_NEED_WAKEUP);
}

/**
 * Open XDP transport on interface (Native mode).
 */
static int xdp_open_native(ec_transport_t *transport, const char *interface)
{
    xdp_lib_log_to_master();
    return xdp_open(transport, interface, XDP_FLAGS_DRV_MODE,
                    XDP_COPY | XDP_USE_NEED_WAKEUP);
}

/****************************************************************************/

/**
 * Close XDP transport.
 */
static void xdp_close(ec_transport_t *transport)
{
    ec_transport_xdp_t *xdp = transport->priv;

    if (xdp) {
        /* Closing the socket takes it out of the program's map. The
         * program stays on the interface for the next master: removing
         * it would restart the link (see EC_XDP_PROG_NAME). Nothing here
         * needs a privilege, so an application that has dropped its
         * privileges closes the same way. */
        if (xdp->xsk) {
            xsk_socket__delete(xdp->xsk);
            xdp->xsk = NULL;
        }
        if (xdp->map_fd >= 0) {
            close(xdp->map_fd);
        }

        if (xdp->umem) {
            xsk_umem__delete(xdp->umem);
        }
        if (xdp->umem_buffer) {
            ec_rt_free(xdp->umem_buffer, NUM_FRAMES * FRAME_SIZE);
        }
        if (xdp->ioctl_sock >= 0) {
            close(xdp->ioctl_sock);
        }
        free(xdp);
        transport->priv = NULL;
    }
}

/****************************************************************************/

/**
 * Get TX buffer pointer.
 *
 * Always returns transport->tx_buffer so the Ethernet header written by
 * ec_device_open() at positions 0..ETH_HLEN-1 is preserved across every TX
 * cycle.  xdp_send() copies the full frame from this buffer into a fresh UMEM
 * frame, so there is no risk of sending a frame with a stale or
 * uninitialized header caused by UMEM frame pool churn.
 */
static uint8_t *xdp_get_tx_buffer(ec_transport_t *transport)
        ECRT_RT_ATTR;
static uint8_t *xdp_get_tx_buffer(ec_transport_t *transport)
{
    ec_transport_xdp_t *xdp = transport->priv;

    if (!xdp) {
        return transport->tx_buffer;
    }

    /* Drain the completion queue so UMEM frames are reclaimed promptly.
     * This keeps the pool healthy for xdp_send() allocations. */
    process_completion_queue(xdp, CQ_DRAIN_MAX);

    /* Return the stable, pre-initialised fixed buffer.  The Ethernet header
     * (dst MAC, src MAC, ethertype) was written here once by ec_device_open()
     * and must not be overwritten by frame-pool churn. */
    return transport->tx_buffer;
}

/****************************************************************************/

/**
 * Send frame.
 *
 * Allocates a fresh UMEM frame for every send and copies the full frame from
 * transport->tx_buffer (which includes the stable Ethernet header).  This
 * avoids the previous bug where a pre-allocated UMEM frame was reused across
 * cycles: if the completion queue had not yet returned the frame from the
 * previous cycle a new, uninitialised frame was allocated, resulting in a
 * garbage Ethernet header (random dst/src MAC, random ethertype) being
 * transmitted.  EtherCAT slaves forwarded those frames back, and the NIC's
 * MAC filter rejected or counted them as alignment errors.
 */
/* TRUSTED: the TX ring kick is sendto(MSG_DONTWAIT) and the ring
 * operations are lock-free; the function-effects analysis cannot
 * see that the syscall does not block. */
static int xdp_send(ec_transport_t *transport, size_t size)
        ECRT_RT_ATTR;
ECRT_RT_TRUSTED_BEGIN
static int xdp_send(ec_transport_t *transport, size_t size)
{
    ec_transport_xdp_t *xdp = transport->priv;
    uint64_t addr;
    uint32_t idx;
    int ret;

    if (!xdp || !xdp->xsk) {
        return -ENODEV;
    }

    if (size > EC_TRANSPORT_MAX_FRAME_SIZE) {
        return -EINVAL;
    }

    /* Allocate a UMEM frame for this transmission. */
    addr = xsk_alloc_umem_frame(xdp);
    if (addr == INVALID_UMEM_FRAME) {
        return -ENOMEM;
    }

    /* Copy the full frame (Ethernet header + EtherCAT payload) from the
     * stable transport->tx_buffer into the UMEM frame. */
    memcpy(xsk_umem__get_data(xdp->umem_buffer, addr),
           transport->tx_buffer, size);

    /* Reserve TX slot */
    ret = xsk_ring_prod__reserve(&xdp->tx, 1, &idx);
    if (ret != 1) {
        /* TX ring is full - return the frame to the pool and report busy. */
        xsk_free_umem_frame(xdp, addr);
        return -EBUSY;
    }

    /* Submit TX descriptor */
    xsk_ring_prod__tx_desc(&xdp->tx, idx)->addr = addr;
    xsk_ring_prod__tx_desc(&xdp->tx, idx)->len = size;
    xsk_ring_prod__submit(&xdp->tx, 1);

    /* Trigger send - critical for immediate transmission in EtherCAT */
    ret = sendto(xsk_socket__fd(xdp->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
    if (ret < 0) {
        /* In copy mode the kernel only transmits inside this syscall.
         * EAGAIN/ENOBUFS mean the descriptor was NOT transmitted (driver
         * busy, completion queue full, ...); it stays queued in the TX
         * ring and goes out with the next cycle's kick.  Report the
         * failure so it lands in the device tx_errors statistic instead
         * of being silently absorbed as a mystery lost cycle. */
        return -errno;
    }

    return 0;
}
ECRT_RT_TRUSTED_END

/****************************************************************************/

/**
 * Try to refill the fill queue with deferred frames.
 */
static void xdp_drain_deferred_refills(ec_transport_xdp_t *xdp)
{
    uint32_t idx_fq = 0;
    uint32_t i;
    uint32_t remaining;
    int ret;

    if (xdp->fq_refill_count == 0) {
        return;
    }

    ret = xsk_ring_prod__reserve(&xdp->fq, xdp->fq_refill_count, &idx_fq);
    if (ret > 0) {
        /* May get fewer slots than requested */
        for (i = 0; i < (uint32_t)ret; i++) {
            *xsk_ring_prod__fill_addr(&xdp->fq, idx_fq++) =
                xdp->fq_refill_pending[i];
        }
        xsk_ring_prod__submit(&xdp->fq, (uint32_t)ret);

        /* Shift remaining entries using memmove */
        remaining = xdp->fq_refill_count - (uint32_t)ret;
        if (remaining > 0) {
            memmove(xdp->fq_refill_pending,
                    &xdp->fq_refill_pending[(uint32_t)ret],
                    remaining * sizeof(uint64_t));
        }
        xdp->fq_refill_count = remaining;
    }
}

/****************************************************************************/

/**
 * Return a consumed RX frame to the fill queue (deferring if it is full).
 */
static void xdp_refill_fq(ec_transport_xdp_t *xdp, uint64_t addr)
{
    uint32_t idx_fq = 0;

    /* Try to drain any previously deferred refills first */
    xdp_drain_deferred_refills(xdp);

    if (xsk_ring_prod__reserve(&xdp->fq, 1, &idx_fq) == 1) {
        *xsk_ring_prod__fill_addr(&xdp->fq, idx_fq) = addr;
        xsk_ring_prod__submit(&xdp->fq, 1);
    } else {
        /* FQ full — defer this frame for later refill */
        if (xdp->fq_refill_count < FQ_REFILL_MAX) {
            xdp->fq_refill_pending[xdp->fq_refill_count++] = addr;
        } else {
            /* Overflow — should not happen with proper sizing, last resort */
            xsk_free_umem_frame(xdp, addr);
        }
    }
}

/****************************************************************************/

/**
 * Receive frame (non-blocking).
 */
/* TRUSTED: ring peek/release are lock-free and the optional wakeup
 * is recvfrom(MSG_DONTWAIT); the function-effects analysis cannot
 * see that the syscall does not block. */
static int xdp_receive(ec_transport_t *transport, uint8_t *buffer,
        size_t max_size) ECRT_RT_ATTR;
ECRT_RT_TRUSTED_BEGIN
static int xdp_receive(ec_transport_t *transport, uint8_t *buffer, size_t max_size)
{
    ec_transport_xdp_t *xdp = transport->priv;
    uint32_t idx_rx = 0;
    uint64_t addr;
    uint32_t len;
    const uint8_t *frame;
    unsigned int rcvd;
    int result = 0;

    if (!xdp || !xdp->xsk) {
        return -ENODEV;
    }

    for (;;) {
        /* Check for received packets */
        rcvd = xsk_ring_cons__peek(&xdp->rx, 1, &idx_rx);
        if (!rcvd) {
            break;  /* No data available */
        }

        /* Get received packet */
        addr = xsk_ring_cons__rx_desc(&xdp->rx, idx_rx)->addr;
        len = xsk_ring_cons__rx_desc(&xdp->rx, idx_rx)->len;
        frame = xsk_umem__get_data(xdp->umem_buffer, addr);

        /* The program redirects only EtherCAT frames, but a program
         * attached by something else (or libxdp's default one, which
         * redirects ALL traffic on the queue) would bring whatever the
         * kernel stack emits here (IPv6 ND/MLD, LLDP, ...). Passing a
         * non-EtherCAT frame up would be miscounted as a corrupted frame
         * by the master, so filter on the ethertype like the raw
         * transport's protocol-bound socket does. */
        if (len < ETH_HLEN
                || ((frame[12] << 8) | frame[13]) != EC_TRANSPORT_ETHERTYPE) {
            xdp->rx_foreign++;
            /* Log with exponential backoff (1st, 2nd, 4th, ... occurrence)
             * so a stray-traffic misconfiguration is visible without
             * flooding from the cyclic path. */
            if ((xdp->rx_foreign & (xdp->rx_foreign - 1)) == 0) {
                ec_log(EC_LOG_WARNING,
                        "XDP: dropped foreign frame (ethertype 0x%02x%02x,"
                        " %llu total) - non-EtherCAT traffic on the"
                        " EtherCAT interface\n",
                        len >= ETH_HLEN ? frame[12] : 0,
                        len >= ETH_HLEN ? frame[13] : 0,
                        (unsigned long long)xdp->rx_foreign);
            }
            xsk_ring_cons__release(&xdp->rx, 1);
            xdp_refill_fq(xdp, addr);
            continue;
        }

        if (len > max_size) {
            len = max_size;
        }

        /* Copy data from UMEM */
        memcpy(buffer, frame, len);

        /* Release RX descriptor and refill */
        xsk_ring_cons__release(&xdp->rx, 1);
        xdp_refill_fq(xdp, addr);

        result = (int)len;
        break;
    }

    /* Wakeup kernel if needed (XDP_USE_NEED_WAKEUP) */
    if (xsk_ring_prod__needs_wakeup(&xdp->fq)) {
        (void)recvfrom(xsk_socket__fd(xdp->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
    }

    return result;
}
ECRT_RT_TRUSTED_END

/****************************************************************************/

/**
 * Get link state.
 */
static int xdp_get_link_state(ec_transport_t *transport)
{
    ec_transport_xdp_t *xdp = transport->priv;
    struct ifreq ifr;

    if (!xdp || xdp->ioctl_sock < 0) {
        return -ENODEV;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%.*s", IFNAMSIZ - 1, transport->interface);

    if (ioctl(xdp->ioctl_sock, SIOCGIFFLAGS, &ifr) < 0) {
        return -errno;
    }

    /* Check if interface is up and running */
    if ((ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING)) {
        return 1;  /* Link up */
    }

    return 0;  /* Link down */
}

/****************************************************************************/

/**
 * Get MAC address.
 */
static int xdp_get_mac(ec_transport_t *transport, uint8_t mac[6])
{
    ec_transport_xdp_t *xdp = transport->priv;

    if (!xdp) {
        return -ENODEV;
    }

    memcpy(mac, xdp->mac_addr, 6);
    return 0;
}

/****************************************************************************/

/**
 * Get file descriptor for polling.
 */
static int xdp_get_fd(ec_transport_t *transport)
{
    ec_transport_xdp_t *xdp = transport->priv;

    if (!xdp || !xdp->xsk) {
        return -1;
    }

    return xsk_socket__fd(xdp->xsk);
}

/****************************************************************************/

/**
 * Set CPU affinity for all NIC IRQs.
 */
static int xdp_set_cpu_affinity(ec_transport_t *transport, int cpu)
{
    ec_transport_xdp_t *xdp = transport->priv;

    if (!xdp || xdp->irqs.count <= 0) {
        return -ENODEV;
    }

    return ec_irq_set_affinity(&xdp->irqs, cpu);
}

/****************************************************************************/

/** XDP transport operations */
const ec_transport_ops_t ec_transport_xdp_skb_ops = {
    .name = "xdp-skb",
    .open = xdp_open_skb,
    .close = xdp_close,
    .get_tx_buffer = xdp_get_tx_buffer,
    .send = xdp_send,
    .receive = xdp_receive,
    .get_link_state = xdp_get_link_state,
    .get_mac = xdp_get_mac,
    .get_fd = xdp_get_fd,
    .set_cpu_affinity = xdp_set_cpu_affinity,
};

const ec_transport_ops_t ec_transport_xdp_native_ops = {
    .name = "xdp-native",
    .open = xdp_open_native,
    .close = xdp_close,
    .get_tx_buffer = xdp_get_tx_buffer,
    .send = xdp_send,
    .receive = xdp_receive,
    .get_link_state = xdp_get_link_state,
    .get_mac = xdp_get_mac,
    .get_fd = xdp_get_fd,
    .set_cpu_affinity = xdp_set_cpu_affinity,
};

/****************************************************************************/
