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
 * Helpers for "ethercat start" and "ethercat stop": the ec_master daemon,
 * its settings file and the interface locks (EC_USPACE_LOCK_PATH_FMT).
 *
 * The commands only handle a free-running ec_master daemon. A master that
 * runs inside an application (for example LinuxCNC's rtapi_app) holds the
 * same lock but is never started or stopped by the tool.
 */

#ifndef __USPACE_DAEMON_H__
#define __USPACE_DAEMON_H__

#include <sys/types.h>

#include <string>
#include <vector>
using namespace std;

namespace UspaceDaemon {

/** Settings from EC_USPACE_MASTER_CONF (KEY=VALUE lines). */
struct Settings {
    string interface;   /**< INTERFACE */
    string transport;   /**< TRANSPORT (default: raw) */
    string cpu;         /**< CPU */
    string irqCpu;      /**< IRQ_CPU */
    string group;       /**< GROUP */
};

/** Read the settings file; a missing file gives the defaults. */
Settings readSettings();

/** PID of the process that holds the lock of \a interface.
 *
 * \return The PID, -1 if it is held but has no PID in it, or 0 if it is
 *         free.
 */
pid_t lockHolder(const string &interface);

/** Interfaces that have a lock file. */
vector<string> lockedInterfaces();

/** Program name (basename of argv[0]) of \a pid, or "?". */
string programOf(pid_t pid);

/** Name of the free-running daemon. */
extern const char *const daemonName;

} // namespace UspaceDaemon

#endif
