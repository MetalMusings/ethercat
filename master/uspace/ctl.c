/*****************************************************************************
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
 ****************************************************************************/

/**
   \file
   ec_uspace_master start | stop | status.

   Controls a userspace master that runs in the background, the way
   ethercatctl controls the kernel master:

   - start: start ec_uspace_master as a daemon with the settings in
     EC_USPACE_MASTER_CONF, and return once its socket answers and the bus
     scan is done, so that the ethercat tool can be used right away.
   - stop: stop it and wait until it has released the network interface.
   - status: tell who holds the interface.

   Who holds an interface is told by the lock file EC_USPACE_LOCK_PATH_FMT
   (an flock() held for as long as the interface is in use, with the PID
   of the holder in it). An application that runs its own master, such as
   LinuxCNC, holds the same lock and stops a running ec_uspace_master
   itself.
*/

/****************************************************************************/

#include "../ec_ioctl_data.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ctl.h"

/****************************************************************************/

/** Settings, from EC_USPACE_MASTER_CONF and the command line. */
struct ctl_settings {
    char interface[64];
    char transport[32];
    char idle_exit[16];
    char group[64];
    char cpu[16];
    char irq_cpu[16];
};

/** Read KEY=VALUE lines; unknown keys and # comments are ignored. */
static void read_conf(struct ctl_settings *s)
{
    FILE *f = fopen(EC_USPACE_MASTER_CONF, "r");
    char line[256];

    if (!f)
        return;

    while (fgets(line, sizeof(line), f)) {
        char *key = line, *value, *end;
        char *dst = NULL;
        size_t size = 0;

        while (*key == ' ' || *key == '\t')
            key++;
        if (*key == '#' || !(value = strchr(key, '=')))
            continue;
        for (end = value; end > key && (end[-1] == ' ' || end[-1] == '\t');)
            end--;
        *end = 0;
        value++;
        while (*value == ' ' || *value == '\t' || *value == '"')
            value++;
        end = value + strlen(value);
        while (end > value && strchr(" \t\r\n\"", end[-1]))
            end--;
        *end = 0;

        if (!strcmp(key, "INTERFACE")) {
            dst = s->interface; size = sizeof(s->interface);
        } else if (!strcmp(key, "TRANSPORT")) {
            dst = s->transport; size = sizeof(s->transport);
        } else if (!strcmp(key, "IDLE_EXIT")) {
            dst = s->idle_exit; size = sizeof(s->idle_exit);
        } else if (!strcmp(key, "GROUP")) {
            dst = s->group; size = sizeof(s->group);
        } else if (!strcmp(key, "CPU")) {
            dst = s->cpu; size = sizeof(s->cpu);
        } else if (!strcmp(key, "IRQ_CPU")) {
            dst = s->irq_cpu; size = sizeof(s->irq_cpu);
        }
        if (dst)
            snprintf(dst, size, "%s", value);
    }
    fclose(f);
}

/****************************************************************************/

/** PID of the process that holds the interface lock.
 *
 * \return The PID (-1 if the lock file has none), or 0 if nobody holds it.
 */
static pid_t lock_holder(const char *interface)
{
    char path[128], buf[32];
    pid_t pid = -1;
    ssize_t n;
    int fd;

    snprintf(path, sizeof(path), EC_USPACE_LOCK_PATH_FMT, interface);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    if (flock(fd, LOCK_SH | LOCK_NB) == 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return 0;
    }
    n = pread(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = 0;
        pid = (pid_t)strtol(buf, NULL, 10);
        if (pid <= 0)
            pid = -1;
    }
    close(fd);
    return pid;
}

/** Name of the program that runs as \a pid (basename of argv[0]).
 *
 * /proc/PID/cmdline is readable for any process, unlike /proc/PID/exe,
 * and not cut at 15 characters, unlike /proc/PID/comm. */
static void program_of(pid_t pid, char *name, size_t size)
{
    char path[64], buf[256];
    const char *base;
    ssize_t n = -1;
    int fd;

    snprintf(name, size, "?");
    if (pid <= 0)
        return;
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
    }
    if (n <= 0)
        return;
    buf[n] = 0;
    base = strrchr(buf, '/');
    snprintf(name, size, "%s", base ? base + 1 : buf);
}

static int is_uspace_master(pid_t pid)
{
    char name[64];

    program_of(pid, name, sizeof(name));
    return !strcmp(name, "ec_uspace_master");
}

/****************************************************************************/

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;

    while (len > 0) {
        ssize_t n = recv(fd, p, len, MSG_WAITALL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int connect_socket(const char *path)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/** One request to master 0, the way the ethercat tool sends it. */
static int ipc_request(int fd, uint32_t cmd, void *data, uint32_t size)
{
    ec_ipc_request_t req = {
        .version_magic = EC_IPC_VERSION_MAGIC,
        .cmd = cmd,
        .master_index = 0,
        .data_size = size,
    };
    ec_ipc_response_t resp;
    char discard[256];

    if (send_all(fd, &req, sizeof(req)) || send_all(fd, data, size)
            || recv_all(fd, &resp, sizeof(resp)))
        return -EIO;
    if (resp.data_size > 0) {
        uint32_t keep = resp.data_size < size ? resp.data_size : size;
        uint32_t rest = resp.data_size - keep;

        if (recv_all(fd, data, keep))
            return -EIO;
        while (rest > 0) {
            uint32_t chunk = rest < sizeof(discard) ? rest : sizeof(discard);
            if (recv_all(fd, discard, chunk))
                return -EIO;
            rest -= chunk;
        }
    }
    return resp.ret;
}

/** Wait until the bus scan is done and the slaves reached PREOP (or an
 * error), as with a kernel master that has been running for a while.
 *
 * \return Number of slaves, or -1 if that took longer than 15 s. An empty
 * bus counts as done after 3 s.
 */
static int wait_for_scan(int fd)
{
    int tenths;

    for (tenths = 0; tenths < 150; tenths++, usleep(100000)) {
        ec_ioctl_master_t master;
        unsigned int pos;
        int settled = 1;

        memset(&master, 0, sizeof(master));
        if (ipc_request(fd, EC_CMD_MASTER, &master, sizeof(master)) < 0
                || master.scan_busy)
            continue;
        if (master.slave_count == 0) {
            if (tenths >= 30)
                return 0;
            continue;
        }
        for (pos = 0; pos < master.slave_count && settled; pos++) {
            ec_ioctl_slave_t slave;

            memset(&slave, 0, sizeof(slave));
            slave.position = pos;
            if (ipc_request(fd, EC_CMD_SLAVE, &slave, sizeof(slave)) < 0
                    || (!slave.error_flag
                        && (slave.al_state & 0x0f) < EC_SLAVE_STATE_PREOP))
                settled = 0;
        }
        if (settled)
            return (int)master.slave_count;
    }
    return -1;
}

/****************************************************************************/

static int ctl_start(const struct ctl_settings *s, const char *self)
{
    const char *argv[24];
    char group[64] = "";
    pid_t pid, holder;
    int argc = 0, status, fd = -1, slaves, i;

    holder = lock_holder(s->interface);
    if (holder > 0 && is_uspace_master(holder)) {
        printf("ec_uspace_master is already running on %s (pid %d).\n",
                s->interface, (int)holder);
        return 0;
    }
    if (holder) {
        char name[64];

        program_of(holder, name, sizeof(name));
        fprintf(stderr, "%s is in use by %s (pid %d).\n",
                s->interface, name, (int)holder);
        return 1;
    }
    if (access("/dev/EtherCAT0", F_OK) == 0) {
        fprintf(stderr, "The kernel master is loaded (/dev/EtherCAT0)."
                " Stop it first: sudo service ethercat stop\n");
        return 1;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "Starting a master needs root:"
                " sudo ec_uspace_master start\n");
        return 1;
    }

    /* The socket is for the group in GROUP, else for the group of the
     * user that ran sudo, so that the tool works without sudo. */
    if (s->group[0]) {
        snprintf(group, sizeof(group), "%s", s->group);
    } else if (getenv("SUDO_GID")) {
        struct group *gr =
            getgrgid((gid_t)strtoul(getenv("SUDO_GID"), NULL, 10));
        if (gr && gr->gr_gid != 0)
            snprintf(group, sizeof(group), "%s", gr->gr_name);
    }

    argv[argc++] = "ec_uspace_master";
    argv[argc++] = "-i";
    argv[argc++] = s->interface;
    argv[argc++] = "-t";
    argv[argc++] = s->transport;
    argv[argc++] = "-x";
    argv[argc++] = s->idle_exit;
    if (s->cpu[0]) {
        argv[argc++] = "-c";
        argv[argc++] = s->cpu;
    }
    if (s->irq_cpu[0]) {
        argv[argc++] = "-q";
        argv[argc++] = s->irq_cpu;
    }
    if (group[0]) {
        argv[argc++] = "-g";
        argv[argc++] = group;
    }
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        execv(self, (char **)argv);
        fprintf(stderr, "Cannot run %s: %s\n", self, strerror(errno));
        _exit(127);
    }
    /* The daemon forks away; this first process exits at once. */
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "ec_uspace_master did not start.\n");
        return 1;
    }

    for (i = 0; i < 100 && fd < 0; i++) {
        fd = connect_socket(EC_IPC_DEFAULT_SOCKET_PATH);
        if (fd < 0)
            usleep(100000);
    }
    if (fd < 0) {
        fprintf(stderr, "ec_uspace_master did not open %s"
                " (see: sudo journalctl -t ec_uspace_master).\n",
                EC_IPC_DEFAULT_SOCKET_PATH);
        return 1;
    }
    slaves = wait_for_scan(fd);
    close(fd);

    printf("ec_uspace_master started on %s (%s, pid %d)",
            s->interface, s->transport, (int)lock_holder(s->interface));
    if (slaves < 0)
        printf(", the bus scan is not done yet.\n");
    else
        printf(", %d slave(s).\n", slaves);
    if (strtoul(s->idle_exit, NULL, 10) == 0)
        printf("It runs until LinuxCNC starts or"
                " \"sudo ec_uspace_master stop\".\n");
    else
        printf("It exits after %s s without a command.\n", s->idle_exit);
    return 0;
}

static int ctl_stop(const struct ctl_settings *s)
{
    pid_t holder = lock_holder(s->interface);
    int i;

    if (holder == 0) {
        printf("No master is running on %s.\n", s->interface);
        return 0;
    }
    if (!is_uspace_master(holder)) {
        char name[64];

        program_of(holder, name, sizeof(name));
        fprintf(stderr, "%s is in use by %s (pid %d), not by"
                " ec_uspace_master; not stopping it.\n",
                s->interface, name, (int)holder);
        return 1;
    }
    if (kill(holder, SIGTERM) < 0) {
        fprintf(stderr, "Cannot stop ec_uspace_master (pid %d): %s\n",
                (int)holder, strerror(errno));
        return 1;
    }
    for (i = 0; i < 100; i++) {
        if (lock_holder(s->interface) == 0) {
            printf("ec_uspace_master (pid %d) stopped, %s is free.\n",
                    (int)holder, s->interface);
            return 0;
        }
        usleep(100000);
    }
    fprintf(stderr, "ec_uspace_master (pid %d) did not release %s.\n",
            (int)holder, s->interface);
    return 1;
}

/** \return 0 if a master runs on the interface, 3 if not (as LSB status). */
static int ctl_status(const struct ctl_settings *s)
{
    pid_t holder = lock_holder(s->interface);
    int kernel = access("/dev/EtherCAT0", F_OK) == 0;
    char name[64];

    if (holder == 0) {
        printf("No userspace master is running on %s.\n", s->interface);
        if (kernel)
            printf("The kernel master is loaded (/dev/EtherCAT0).\n");
        return 3;
    }

    program_of(holder, name, sizeof(name));
    if (!strcmp(name, "ec_uspace_master"))
        printf("ec_uspace_master is running on %s (pid %d).\n",
                s->interface, (int)holder);
    else if (!strcmp(name, "rtapi_app"))
        printf("LinuxCNC (rtapi_app, pid %d) runs the master on %s.\n",
                (int)holder, s->interface);
    else
        printf("%s (pid %d) holds %s.\n", name, (int)holder, s->interface);

    {
        int fd = connect_socket(EC_IPC_DEFAULT_SOCKET_PATH);
        ec_ioctl_master_t master;

        memset(&master, 0, sizeof(master));
        if (fd < 0) {
            printf("Socket %s: %s\n", EC_IPC_DEFAULT_SOCKET_PATH,
                    strerror(errno));
        } else {
            if (ipc_request(fd, EC_CMD_MASTER, &master, sizeof(master)) == 0)
                printf("Socket %s: %u slave(s), %s.\n",
                        EC_IPC_DEFAULT_SOCKET_PATH, master.slave_count,
                        master.active ? "active" : "idle");
            close(fd);
        }
    }
    if (kernel)
        printf("Warning: the kernel master is loaded too (/dev/EtherCAT0).\n");
    return 0;
}

/****************************************************************************/

int ec_uspace_ctl(int argc, char *argv[])
{
    static const struct option options[] = {
        {"interface", required_argument, 0, 'i'},
        {"transport", required_argument, 0, 't'},
        {"idle-exit", required_argument, 0, 'x'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    struct ctl_settings s;
    const char *cmd;
    char self[512];
    ssize_t n;
    int c;

    if (argc < 2 || (strcmp(argv[1], "start") && strcmp(argv[1], "stop")
                && strcmp(argv[1], "status")))
        return -1; /* not a control command: run as the daemon */
    cmd = argv[1];

    memset(&s, 0, sizeof(s));
    snprintf(s.transport, sizeof(s.transport), "raw");
    snprintf(s.idle_exit, sizeof(s.idle_exit), "0");
    read_conf(&s);

    optind = 2;
    while ((c = getopt_long(argc, argv, "i:t:x:h", options, NULL)) != -1) {
        switch (c) {
            case 'i':
                snprintf(s.interface, sizeof(s.interface), "%s", optarg);
                break;
            case 't':
                snprintf(s.transport, sizeof(s.transport), "%s", optarg);
                break;
            case 'x':
                snprintf(s.idle_exit, sizeof(s.idle_exit), "%s", optarg);
                break;
            default:
                printf("Usage: %s start|stop|status [-i <interface>]"
                        " [-t <transport>] [-x <idle-exit seconds>]\n\n"
                        "Settings come from " EC_USPACE_MASTER_CONF
                        " (INTERFACE, TRANSPORT,\nIDLE_EXIT, GROUP, CPU,"
                        " IRQ_CPU); the options override them.\n",
                        argv[0]);
                return c == 'h' ? 0 : 1;
        }
    }
    if (!s.interface[0]) {
        fprintf(stderr, "No interface: set INTERFACE in "
                EC_USPACE_MASTER_CONF " or give -i.\n");
        return 1;
    }

    if (!strcmp(cmd, "stop"))
        return ctl_stop(&s);
    if (!strcmp(cmd, "status"))
        return ctl_status(&s);

    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        perror("/proc/self/exe");
        return 1;
    }
    self[n] = 0;
    return ctl_start(&s, self);
}

/****************************************************************************/
