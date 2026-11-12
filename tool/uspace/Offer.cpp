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

/** \file
 * See Offer.h. The user always confirms: the tool prints the full commands
 * and runs them only on "y". sudo asks for a password itself when it needs
 * one (there is a terminal). Nothing else is passed on: the interface and
 * the transport come from the settings file, as with "start" by hand.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <vector>
using namespace std;

#include "Daemon.h"
#include "Offer.h"

namespace UspaceDaemon {

/****************************************************************************/

/** Run \a args (args[0] looked up in PATH) and wait; true if it exited 0. */
static bool runCommand(const vector<string> &args)
{
    vector<char *> av;
    pid_t pid;
    int status;

    for (size_t i = 0; i < args.size(); i++)
        av.push_back(const_cast<char *>(args[i].c_str()));
    av.push_back(NULL);

    pid = fork();
    if (pid < 0) {
        cerr << "fork(): " << strerror(errno) << endl;
        return false;
    }
    if (pid == 0) {
        execvp(av[0], &av[0]);
        cerr << "Cannot run " << av[0] << ": " << strerror(errno) << endl;
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/****************************************************************************/

/** True if a program other than the free-running daemon (LinuxCNC) holds
 * one of the interface locks: it has the bus, no master is to be started. */
static bool otherMasterRuns()
{
    vector<string> interfaces = lockedInterfaces();

    for (size_t i = 0; i < interfaces.size(); i++) {
        pid_t holder = lockHolder(interfaces[i]);

        if (holder && programOf(holder) != daemonName)
            return true;
    }
    return false;
}

/****************************************************************************/

void offerStart(char **argv, const string &command)
{
    const char *noPrompt = getenv("ETHERCAT_USPACE_NO_PROMPT");
    char self[4096];
    ssize_t n;
    vector<string> startCmd, stopKernel;
    string line;

    if (command == "start" || command == "stop" || command == "version")
        return;
    if (noPrompt && *noPrompt && strcmp(noPrompt, "0"))
        return;
    if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO) || geteuid() == 0)
        return;
    if (otherMasterRuns())
        return;

    /* By absolute path: sudo's PATH may not hold this tool. */
    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0)
        return;
    self[n] = 0;

    startCmd.push_back("sudo");
    startCmd.push_back(self);
    startCmd.push_back("start");
    stopKernel.push_back("sudo");
    stopKernel.push_back("systemctl");
    stopKernel.push_back("stop");
    stopKernel.push_back("ethercat");
    bool kernel = access("/dev/EtherCAT0", F_OK) == 0;

    cerr << "No userspace master is running. To start one:" << endl;
    if (kernel)
        cerr << "  The kernel master is loaded (/dev/EtherCAT0). It is stopped"
            << endl << "  first: it shares the network card, and may be in use"
            << " on purpose." << endl
            << "  sudo systemctl stop ethercat" << endl;
    cerr << "  sudo " << self << " start" << endl
        << "Run it now? [y/N] " << flush;
    if (!getline(cin, line) || (line != "y" && line != "Y" && line != "yes"))
        return;

    if (kernel && !runCommand(stopKernel)) {
        cerr << "Could not stop the kernel master." << endl;
        return;
    }
    if (!runCommand(startCmd)) {
        cerr << "The master did not start." << endl;
        return;
    }

    /* Run the original command again, without asking a second time. */
    setenv("ETHERCAT_USPACE_NO_PROMPT", "1", 1);
    execv(self, argv);
    cerr << "Cannot run " << self << ": " << strerror(errno) << endl;
}

} // namespace UspaceDaemon

/****************************************************************************/
