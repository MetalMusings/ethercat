# linuxcnc-ethercat-uspace: the userspace EtherCAT master for LinuxCNC

This package holds the IgH EtherCAT master of this repository, built as a
library without a kernel module (`./configure --enable-uspace-master`). It
is the master that the [ecat](https://github.com/MetalMusings/ecat) HAL
component runs inside LinuxCNC. It is experimental: try it with the
machine unable to do harm.

Everything is installed in `/opt/linuxcnc-ethercat-uspace`:

| | |
|---|---|
| `lib/libethercat.so.2` | The master, which `ecat` loads. |
| `include/*.h` | Its headers, for building `ecat` and other HAL components. |
| `bin/ethercat` | The tool, also as `/usr/bin/ethercat-uspace`: `ethercat-uspace slaves`, `sdos`, `upload` ... reach the master running in LinuxCNC, or the free-running one. |
| `bin/ec_master` | The free-running master: `sudo ethercat-uspace start`, `stop`. |
| `etc/ethercat-uspace.conf` | Settings of the free-running master. |

An installed kernel master is left as it is.

Requirements: Debian 12 or 13, preferably with a PREEMPT_RT kernel. The
package does not need LinuxCNC; `ecat` does.

## Install

```
sudo apt install ./linuxcnc-ethercat-uspace_*.deb
```

Then install `ecat` from its releases.

## Stop the kernel master, if installed

The two masters cannot use the network interface at the same time:

```
sudo systemctl stop ethercat
```

`ecat` refuses to start while the kernel master has the same interface.
To start the kernel master again later: `sudo systemctl start ethercat`.

## Check that the slaves are found

Replace `enp2s0` with your EtherCAT interface (see `ip link`):

```
sudo ethercat-uspace start -- -i enp2s0
ethercat-uspace slaves
sudo ethercat-uspace stop
```

`raw` (the default transport) works with any network card; `xdp-native`
(`-t xdp-native`) gives shorter and more even cycle times, but needs a
driver with XDP support, such as `igc` (Intel i225/i226) or `igb`.

With `xdp-skb` or `xdp-native` the master attaches a small XDP program to
the card the first time, and leaves it there when it stops: attaching or
removing it restarts the network card (the link is down for about 2 s on
`igc` and `igb`), so the next master, the free one or LinuxCNC's, only
binds to it and finds the slaves at once. The program sends EtherCAT frames
to a running master and everything else to the system as usual; without a
master it passes all frames on. A reboot removes it, and so does:

```
sudo ip link set dev enp2s0 xdp off
```

LinuxCNC with `ecat` stops a free-running master on the same interface
when it starts.

## Ethernet over EtherCAT (EoE)

A slave with EoE gets a network device `eoe0s<position>` (a TAP device) on
the host when the master scans the bus. Inside LinuxCNC, the EoE traffic
rides in `ecat`'s cyclic frames, so the servo thread never waits for it
(no callbacks needed; `docs/design/userspace-master.md`, "EoE After
Activation"). Expect about one mailbox (often 128 to 1024 bytes) per EoE
slave per servo cycle in each direction, and frames that much longer.

## From version 1.0 and lcec_uspace

Up to 1.0 this package was built from
[MetalMusings/linuxcnc-ethercat](https://github.com/MetalMusings/linuxcnc-ethercat/releases)
and also held `lcec_uspace`, `lcec_conf` and `lcec-take-down`. They are
frozen, and the package no longer has them: installing 1.1 over 1.0
removes them. To keep running `lcec_uspace`, stay on 1.0 from there; to
move on, use `ecat`, which imports lcec XML files.

## Uninstall

```
sudo apt remove linuxcnc-ethercat-uspace
```

## Building

```
packaging/linuxcnc/build-deb.sh -o out
```

builds the package from this repository's committed state, for the
Debian release of the machine. The workflow
`.github/workflows/linuxcnc-deb.yml` builds it for Debian 12 and 13, and
puts it on a release for a pushed `uspace-v*` tag.
