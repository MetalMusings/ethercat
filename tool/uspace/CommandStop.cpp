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
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <iostream>
#include <sstream>
using namespace std;

#include "CommandStop.h"
#include "Daemon.h"

using namespace UspaceDaemon;

extern string binaryBaseName;

/****************************************************************************/

CommandStop::CommandStop():
    Command("stop", "Stop a free-running userspace master (ec_master).")
{
}

/****************************************************************************/

string CommandStop::helpString(const string &binaryBaseName) const
{
    stringstream str;

    str << binaryBaseName << " " << getName() << endl
        << endl
        << getBriefDescription() << endl
        << endl
        << "Stops every ec_master daemon and waits until it has released"
        << endl
        << "its network interface. A master that runs inside an" << endl
        << "application (LinuxCNC) is left alone. It is not an error if"
        << endl
        << "no master is running." << endl
        << endl
        << "Needs root: sudo " << binaryBaseName << " stop" << endl;

    return str.str();
}

/****************************************************************************/

void CommandStop::execute(const StringVector &args)
{
    vector<string> interfaces = lockedInterfaces();
    vector<string> stopping;
    vector<pid_t> pids;
    stringstream err;

    if (args.size()) {
        err << "'" << getName() << "' takes no arguments!";
        throwInvalidUsageException(err);
    }

    for (size_t i = 0; i < interfaces.size(); i++) {
        pid_t holder = lockHolder(interfaces[i]);

        if (!holder)
            continue;
        if (programOf(holder) != daemonName) {
            cout << interfaces[i] << " is used by " << programOf(holder)
                << " (pid " << holder << "), not by " << daemonName
                << "; leaving it." << endl;
            continue;
        }
        /* One daemon can hold several interfaces. */
        bool signalled = false;
        for (size_t j = 0; j < pids.size(); j++)
            signalled = signalled || pids[j] == holder;
        if (!signalled && kill(holder, SIGTERM) < 0) {
            err << "Cannot stop " << daemonName << " (pid " << holder
                << "): " << strerror(errno);
            if (errno == EPERM)
                err << endl << "Try: sudo " << binaryBaseName << " stop";
            throwCommandException(err);
        }
        stopping.push_back(interfaces[i]);
        pids.push_back(holder);
    }

    if (stopping.empty()) {
        cout << "No " << daemonName << " is running." << endl;
        return;
    }

    for (size_t i = 0; i < stopping.size(); i++) {
        int tenths;

        for (tenths = 0; tenths < 100 && lockHolder(stopping[i]); tenths++)
            usleep(100000);
        if (tenths == 100) {
            err << daemonName << " (pid " << pids[i]
                << ") did not release " << stopping[i] << ".";
            throwCommandException(err);
        }
        cout << daemonName << " (pid " << pids[i] << ") stopped, "
            << stopping[i] << " is free." << endl;
    }
}

/****************************************************************************/
