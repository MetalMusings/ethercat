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

This installs `libethercat.so.2`, the `ec_uspace_master` standalone daemon, the
`ethercat` command-line tool, and pkg-config/CMake package files. Note
that the userspace library's soname (`.so.2`) deliberately differs from
the kernel-mode client library's (`.so.1`): the two are not ABI
compatible and cannot be mixed up at runtime.

Run a standalone master (the tool connects over a Unix domain socket,
created with mode 0660; grant a tool group with `--socket-group`):

```bash
# ec_uspace_master -i eth0 --socket-group ethercat
$ ethercat slaves
```

Or let `ec_uspace_master` start and stop itself, the way `ethercatctl`
does for the kernel master. The settings come from
`$(sysconfdir)/ethercat-uspace.conf` (`INTERFACE`, `TRANSPORT`,
`IDLE_EXIT`, `GROUP`, `CPU`, `IRQ_CPU`; `-i`, `-t` and `-x` override
them):

```bash
# cat /usr/local/etc/ethercat-uspace.conf
INTERFACE=eth0
TRANSPORT=raw
IDLE_EXIT=0          # seconds without a tool request before it exits; 0 = never
# ec_uspace_master start     # returns once the slaves reached PREOP
$ ethercat slaves            # the socket belongs to the group of the sudo user, or GROUP
$ ec_uspace_master status    # who holds the interface
# ec_uspace_master stop      # waits until the interface is free
```

While it uses an interface, the daemon holds an `flock()` on
`/run/ethercat-uspace.<interface>.lock` with its PID in it
(`EC_USPACE_LOCK_PATH_FMT` in `ecrt.h`). An application that runs its
own master on the same interface should take the same lock: it can then
send a running `ec_uspace_master` SIGTERM and wait for the lock, which
is released once the interface is closed.

Applications embedding the library should read the realtime notes at
`ecrt_lib_init()` in `ecrt.h` (memory locking and `RLIMIT_MEMLOCK`,
nonblocking log callback, thread scheduling) and
[docs/testing/rt-system-test.md](docs/testing/rt-system-test.md).

---

Have fun!

---
