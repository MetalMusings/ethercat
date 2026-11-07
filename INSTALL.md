This is the INSTALL file of the IgH EtherCAT Master.

vim: set spelllang=en spell tw=78

# Building and installing

The complete build and installation procedure is described in the respective
section of the
[documentation](https://gitlab.com/etherlab.org/ethercat/-/jobs/artifacts/stable-1.5/raw/pdf/ethercat_doc.pdf?job=pdf).

---

For the impatient, the procedure mainly consists of calling:

```bash
./bootstrap # to create the configure script, if downloaded from the repo

./configure --sysconfdir=/etc
make all modules
```

... and as root:

```bash
make modules_install install
depmod
```

... and then customizing the appropriate configuration file:

```bash
# vi /etc/ethercat.conf      # For systemd based distro
# vi /etc/sysconfig/ethercat # For init.d based distro
```

Make sure, that the 'udev' package is installed, to automatically create the
EtherCAT character devices. The character devices will be created with mode
0660 and group root by default. If you want to give normal users reading
access, create a udev rule like this:

```bash
echo KERNEL==\"EtherCAT[0-9]*\", MODE=\"0664\" > /etc/udev/rules.d/99-EtherCAT.rules
```

Now you can start the EtherCAT master:

```bash
# systemctl start ethercat   # For systemd based distro
# /etc/init.d/ethercat start # For init.d based distro
```

Have a look at the [examples subdirectory](examples/) for some application
examples.

---

# Userspace master

To build the master as a userspace shared library instead of a kernel
module (see FEATURES.md, "Userspace Master"):

```bash
./bootstrap
./configure --enable-uspace-master
make
make install    # as root
```

This installs `libethercat.so.2`, the `ec_master` standalone daemon, the
`ethercat` command-line tool, and pkg-config/CMake package files. Note
that the userspace library's soname (`.so.2`) deliberately differs from
the kernel-mode client library's (`.so.1`): the two are not ABI
compatible and cannot be mixed up at runtime.

Run a standalone master (the tool connects over a Unix domain socket,
created with mode 0660; grant a tool group with `--socket-group`):

```bash
# ec_master -i eth0 --socket-group ethercat
$ ethercat slaves
```

Or let the tool start and stop the daemon, the way `ethercatctl` does
for the kernel master. Without arguments, `ethercat start` takes the
settings from `$(sysconfdir)/ethercat-uspace.conf` (`INTERFACE`,
`TRANSPORT`, `CPU`, `IRQ_CPU`, `GROUP`); arguments after `--` go to
`ec_master` instead:

```bash
# cat /usr/local/etc/ethercat-uspace.conf
INTERFACE=eth0
TRANSPORT=raw
# ethercat start              # returns once the slaves reached PREOP
$ ethercat slaves             # the socket belongs to GROUP, or to the sudo user's group
# ethercat stop               # waits until the interface is free
# ethercat start -- -i eth0 -t xdp-native
```

`start` and `stop` only handle a free-running `ec_master`. While it uses
an interface, the daemon holds an `flock()` on
`/run/ethercat-uspace.<interface>.lock` with its PID in it
(`EC_USPACE_LOCK_PATH_FMT` in `ecrt.h`). An application that runs its
own master on the same interface should take the same lock: it can then
send a running `ec_master` SIGTERM and wait for the lock, which is
released once the interface is closed. `ethercat stop` leaves such an
application alone, and `ethercat start` refuses an interface it holds.

Applications embedding the library should read the realtime notes at
`ecrt_lib_init()` in `ecrt.h` (memory locking and `RLIMIT_MEMLOCK`,
nonblocking log callback, thread scheduling) and
[docs/testing/rt-system-test.md](docs/testing/rt-system-test.md).

---

Have fun!

---
