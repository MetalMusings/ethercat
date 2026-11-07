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

#include <errno.h>
#include <grp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <iostream>
#include <sstream>
using namespace std;

#include "CommandStart.h"
#include "Daemon.h"
#include "../MasterDevice.h"

using namespace UspaceDaemon;

extern string binaryBaseName;
extern string socketPath;

/****************************************************************************/

CommandStart::CommandStart():
    Command("start", "Start a free-running userspace master (ec_master).")
{
}

/****************************************************************************/

string CommandStart::helpString(const string &binaryBaseName) const
{
    stringstream str;

    str << binaryBaseName << " " << getName()
        << " [-- <ec_master options>]" << endl
        << endl
        << getBriefDescription() << endl
        << endl
        << "Starts the ec_master daemon and returns when the bus scan is"
        << endl
        << "done, so that the other commands can be used right away."
        << endl
        << "The daemon runs until '" << binaryBaseName << " stop', or until"
        << endl
        << "an application with its own master (LinuxCNC) takes over" << endl
        << "the interface." << endl
        << endl
        << "Needs root. The IPC socket is made accessible to the group" << endl
        << "GROUP, or else to the group of the user that ran sudo, so that"
        << endl
        << "the other commands work without sudo." << endl
        << endl
        << "Without arguments, the settings come from" << endl
        << "  " EC_USPACE_MASTER_CONF << endl
        << "(INTERFACE, TRANSPORT, CPU, IRQ_CPU, GROUP). Arguments after"
        << endl
        << "'--' are given to ec_master instead; see 'ec_master --help'."
        << endl
        << endl
        << "Examples:" << endl
        << "  sudo " << binaryBaseName << " start" << endl
        << "  sudo " << binaryBaseName
        << " start -- -i enp2s0 -t xdp-native" << endl;

    return str.str();
}

/****************************************************************************/

/** Interfaces named by -i/--interface in ec_master arguments. */
static vector<string> interfacesIn(const Command::StringVector &args)
{
    vector<string> res;

    for (size_t i = 0; i < args.size(); i++) {
        const string &a = args[i];

        if ((a == "-i" || a == "--interface") && i + 1 < args.size())
            res.push_back(args[++i]);
        else if (a.compare(0, 12, "--interface=") == 0)
            res.push_back(a.substr(12));
        else if (a.compare(0, 2, "-i") == 0 && a.size() > 2)
            res.push_back(a.substr(2));
    }
    return res;
}

static bool hasOption(const Command::StringVector &args,
        const char *shortOpt, const char *longOpt)
{
    string longEq = string(longOpt) + "=";

    for (size_t i = 0; i < args.size(); i++) {
        const string &a = args[i];

        if (a.compare(0, 2, shortOpt) == 0 || a == longOpt
                || a.compare(0, longEq.size(), longEq) == 0)
            return true;
    }
    return false;
}

/****************************************************************************/

void CommandStart::execute(const StringVector &cmdArgs)
{
    Settings s = readSettings();
    StringVector args(cmdArgs);
    vector<string> interfaces;
    stringstream err;
    pid_t pid;
    int status, slaves;

    if (args.empty()) {
        if (s.interface.empty()) {
            err << "No interface: set INTERFACE in "
                EC_USPACE_MASTER_CONF << endl
                << "or give it: " << binaryBaseName
                << " start -- -i <interface>";
            throwCommandException(err);
        }
        args.push_back("-i");
        args.push_back(s.interface);
        args.push_back("-t");
        args.push_back(s.transport);
        if (!s.cpu.empty()) {
            args.push_back("-c");
            args.push_back(s.cpu);
        }
        if (!s.irqCpu.empty()) {
            args.push_back("-q");
            args.push_back(s.irqCpu);
        }
    }
    interfaces = interfacesIn(args);
    if (interfaces.empty()) {
        err << "No interface given (-i <interface>).";
        throwCommandException(err);
    }

    /* One master per interface. */
    for (size_t i = 0; i < interfaces.size(); i++) {
        pid_t holder = lockHolder(interfaces[i]);

        if (!holder)
            continue;
        if (programOf(holder) == daemonName) {
            cout << daemonName << " is already running on " << interfaces[i]
                << " (pid " << holder << ")." << endl;
            return;
        }
        err << interfaces[i] << " is in use by " << programOf(holder)
            << " (pid " << holder << ").";
        throwCommandException(err);
    }
    if (access("/dev/EtherCAT0", F_OK) == 0) {
        err << "The kernel master is loaded (/dev/EtherCAT0)." << endl
            << "Stop it first: sudo systemctl stop ethercat";
        throwCommandException(err);
    }
    if (geteuid() != 0) {
        err << "Starting a master needs root: sudo " << binaryBaseName
            << " start";
        throwCommandException(err);
    }

    if (!socketPath.empty() && !hasOption(args, "-s", "--socket")) {
        args.push_back("-s");
        args.push_back(socketPath);
    }

    /* The socket is for the group in GROUP, else for the group of the
     * user that ran sudo, so that the tool works without sudo. */
    if (!hasOption(args, "-g", "--socket-group")) {
        string group = s.group;

        if (group.empty() && getenv("SUDO_GID")) {
            struct group *gr =
                getgrgid((gid_t) strtoul(getenv("SUDO_GID"), NULL, 10));
            if (gr && gr->gr_gid != 0)
                group = gr->gr_name;
        }
        if (!group.empty()) {
            args.push_back("-g");
            args.push_back(group);
        }
    }

    vector<char *> argv;
    argv.push_back(const_cast<char *>(daemonName));
    for (size_t i = 0; i < args.size(); i++)
        argv.push_back(const_cast<char *>(args[i].c_str()));
    argv.push_back(NULL);

    pid = fork();
    if (pid < 0) {
        err << "fork(): " << strerror(errno);
        throwCommandException(err);
    }
    if (pid == 0) {
        execv(EC_MASTER_DAEMON, &argv[0]);
        cerr << "Cannot run " EC_MASTER_DAEMON ": " << strerror(errno)
            << endl;
        _exit(127);
    }
    /* The daemon forks away; this first process exits at once. */
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        err << daemonName << " did not start.";
        throwCommandException(err);
    }

    slaves = waitForScan();
    if (slaves == -2) {
        err << daemonName << " did not open its socket"
            << " (see: sudo journalctl -t " << daemonName << ").";
        throwCommandException(err);
    }

    cout << daemonName << " started on " << interfaces[0] << " (pid "
        << lockHolder(interfaces[0]) << ")";
    if (slaves < 0)
        cout << ", the bus scan is not done yet." << endl;
    else
        cout << ", " << slaves << " slave(s)." << endl;
    cout << "It runs until LinuxCNC starts or 'sudo " << binaryBaseName
        << " stop'." << endl;
}

/****************************************************************************/

/** Wait until the socket answers, the bus scan is done and the slaves
 * reached PREOP (or an error), as with a kernel master that has been
 * running for a while.
 *
 * \return Number of slaves, -1 if the scan took longer than 15 s, or -2 if
 *         the socket did not answer within 10 s. An empty bus counts as
 *         done after 3 s.
 */
int CommandStart::waitForScan()
{
    MasterDevice m(0);
    int tenths;

    for (tenths = 0; ; tenths++, usleep(100000)) {
        if (tenths >= 100)
            return -2;
        try {
            m.open(MasterDevice::Read);
            break;
        } catch (MasterDeviceException &e) {
            m.close();
        }
    }

    for (tenths = 0; tenths < 150; tenths++, usleep(100000)) {
        ec_ioctl_master_t master;
        bool settled = true;

        try {
            m.getMaster(&master);
            if (master.scan_busy)
                continue;
            if (master.slave_count == 0) {
                if (tenths >= 30)
                    return 0;
                continue;
            }
            for (unsigned int pos = 0;
                    pos < master.slave_count && settled; pos++) {
                ec_ioctl_slave_t slave;

                m.getSlave(&slave, pos);
                if (!slave.error_flag
                        && (slave.al_state & 0x0f) < EC_SLAVE_STATE_PREOP)
                    settled = false;
            }
        } catch (MasterDeviceException &e) {
            continue;
        }
        if (settled)
            return (int) master.slave_count;
    }
    return -1;
}

/****************************************************************************/
