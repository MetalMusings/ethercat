/*****************************************************************************
 *
 *  Unit test: the IPC server and another master's socket.
 *
 *  Two masters on two network interfaces both want the default socket.
 *  The second must not take over or remove the first one's socket (it
 *  would leave that master unreachable by the tool), but a stale file
 *  that nobody listens on is replaced. At stop, the server removes the
 *  socket file only if it is still its own.
 *
 *  The other master is played by a plain listening Unix socket.
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "uspace/cdev.h"

#include "test.h"

static char path[108];
static char keep[120];

/** A listening socket at path, as another master's IPC server. */
static int other_master(void)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    unlink(path);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0
            || listen(fd, 1) < 0) {
        perror("other master");
        exit(1);
    }
    return fd;
}

static ino_t inode(void)
{
    struct stat st;

    return stat(path, &st) == 0 ? st.st_ino : 0;
}

int main(void)
{
    int other;
    ino_t theirs;

    snprintf(path, sizeof(path), "%s/ectest-ipc-%d.sock",
            getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());

    /* Another master serves the socket: keep away from it. */
    other = other_master();
    theirs = inode();
    TEST_CHECK_EQ(-EADDRINUSE, ec_ipc_server_start(path));
    TEST_CHECK(theirs != 0 && inode() == theirs);
    ec_ipc_server_stop(); /* not started: a no-op, the file stays */
    TEST_CHECK(inode() == theirs);

    /* That master died and left its file: a stale socket is replaced. */
    close(other);
    TEST_CHECK(inode() == theirs);
    /* A second link keeps the old inode allocated: the filesystem would
     * otherwise hand its number to the new socket (seen on CI's ext4). */
    snprintf(keep, sizeof(keep), "%s.keep", path);
    unlink(keep);
    TEST_CHECK(link(path, keep) == 0);
    TEST_CHECK_EQ(0, ec_ipc_server_start(path));
    TEST_CHECK(inode() != 0 && inode() != theirs);
    unlink(keep);

    /* At stop the server removes its own socket. */
    ec_ipc_server_stop();
    TEST_CHECK(inode() == 0);

    /* ...but not one that another master has put there since. */
    TEST_CHECK_EQ(0, ec_ipc_server_start(path));
    other = other_master(); /* as an older master: unlink, bind */
    theirs = inode();
    ec_ipc_server_stop();
    TEST_CHECK(theirs != 0 && inode() == theirs);

    close(other);
    unlink(path);
    return test_done("test_ipc_socket");
}
