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
 * The offer to start a free-running master when a command finds none.
 */

#ifndef __USPACE_OFFER_H__
#define __USPACE_OFFER_H__

#include <string>

namespace UspaceDaemon {

/** A command found no master. On a terminal, offer to run
 * "sudo <this tool> start" (and "sudo systemctl stop ethercat" first when
 * the kernel master is loaded), after the user has answered yes. When it
 * has started a master it runs the original command again and does not
 * return. It returns, doing nothing more, when there is no offer
 * (no terminal, ETHERCAT_USPACE_NO_PROMPT set, root, LinuxCNC has the
 * interface, or \a command is start, stop or version), or the answer is
 * no, or a command failed.
 *
 * \param argv    The tool's own argv, to run the command again.
 * \param command Name of the command that failed.
 */
void offerStart(char **argv, const std::string &command);

} // namespace UspaceDaemon

#endif
