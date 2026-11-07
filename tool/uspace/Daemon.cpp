/*****************************************************************************
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

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include <fstream>

#include "ecrt.h"
#include "Daemon.h"

namespace UspaceDaemon {

const char *const daemonName = "ec_master";

/****************************************************************************/

static string trim(const string &s, const char *chars)
{
    size_t b = s.find_first_not_of(chars);
    size_t e = s.find_last_not_of(chars);

    return b == string::npos ? string() : s.substr(b, e - b + 1);
}

Settings readSettings()
{
    Settings s;
    ifstream f(EC_USPACE_MASTER_CONF);
    string line;

    s.transport = "raw";
    while (getline(f, line)) {
        size_t eq;
        string key, value;

        line = trim(line, " \t\r\n");
        if (line.empty() || line[0] == '#'
                || (eq = line.find('=')) == string::npos)
            continue;
        key = trim(line.substr(0, eq), " \t");
        value = trim(line.substr(eq + 1), " \t\"'");

        if (key == "INTERFACE")
            s.interface = value;
        else if (key == "TRANSPORT")
            s.transport = value;
        else if (key == "CPU")
            s.cpu = value;
        else if (key == "IRQ_CPU")
            s.irqCpu = value;
        else if (key == "GROUP")
            s.group = value;
    }
    return s;
}

/****************************************************************************/

pid_t lockHolder(const string &interface)
{
    char path[128], buf[32];
    pid_t pid = -1;
    ssize_t n;
    int fd;

    snprintf(path, sizeof(path), EC_USPACE_LOCK_PATH_FMT, interface.c_str());
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
        pid = (pid_t) strtol(buf, NULL, 10);
        if (pid <= 0)
            pid = -1;
    }
    close(fd);
    return pid;
}

/****************************************************************************/

vector<string> lockedInterfaces()
{
    /* Split EC_USPACE_LOCK_PATH_FMT into directory, prefix and suffix. */
    string fmt(EC_USPACE_LOCK_PATH_FMT);
    size_t slash = fmt.rfind('/'), pct = fmt.find("%s");
    string dir = fmt.substr(0, slash);
    string prefix = fmt.substr(slash + 1, pct - slash - 1);
    string suffix = fmt.substr(pct + 2);
    vector<string> res;
    DIR *d = opendir(dir.c_str());
    struct dirent *e;

    if (!d)
        return res;
    while ((e = readdir(d))) {
        string name(e->d_name);

        if (name.size() > prefix.size() + suffix.size()
                && !name.compare(0, prefix.size(), prefix)
                && !name.compare(name.size() - suffix.size(),
                    suffix.size(), suffix))
            res.push_back(name.substr(prefix.size(),
                        name.size() - prefix.size() - suffix.size()));
    }
    closedir(d);
    return res;
}

/****************************************************************************/

/* /proc/PID/cmdline is readable for any process, unlike /proc/PID/exe,
 * and not cut at 15 characters, unlike /proc/PID/comm. */
string programOf(pid_t pid)
{
    char path[64], buf[256];
    ssize_t n = -1;
    int fd;

    if (pid <= 0)
        return "?";
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int) pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
    }
    if (n <= 0)
        return "?";
    buf[n] = 0;
    const char *base = strrchr(buf, '/');
    return base ? base + 1 : buf;
}

} // namespace UspaceDaemon

/****************************************************************************/
