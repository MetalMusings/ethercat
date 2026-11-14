#!/bin/bash
#
# Build linuxcnc-ethercat-uspace_*.deb: this userspace IgH EtherCAT master,
# installed in /opt/linuxcnc-ethercat-uspace, apart from the system and from
# an installed kernel master. It is the master that the ecat HAL component
# (https://github.com/MetalMusings/ecat) runs in LinuxCNC. See README.md in
# this folder.
#
# Usage: packaging/linuxcnc/build-deb.sh [-o <dir>]
#
#   -o  directory for the .deb. Default: the current directory.
#
# The master is built from a fresh clone of this repository's committed
# state, so uncommitted changes are not packaged. The package is built for
# the Debian release this machine has.
#
# Build dependencies (Debian 12/13):
#   build-essential git autoconf automake libtool pkg-config libxdp-dev
#   libbpf-dev libexpat1-dev dpkg-dev
#
# Until version 1.0 the package was built from MetalMusings/linuxcnc-ethercat
# and also held lcec_uspace; that is frozen, and its packages stay there.

set -euo pipefail

PKG=linuxcnc-ethercat-uspace
PREFIX=/opt/$PKG
BASE_VERSION=1.3

OUTDIR=$PWD
while getopts "o:h" opt; do
    case $opt in
        o) OUTDIR=$(realpath "$OPTARG") ;;
        *) sed -n '2,/^$/s/^# \{0,1\}//p' "$0"; exit 1 ;;
    esac
done

SRC=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
HERE=$(cd "$(dirname "$0")" && pwd)
JOBS=$(nproc)
WORK=$(mktemp -d -t $PKG.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

MASTER_ROOT=$WORK/master-root  # the master's "make install"
ROOT=$WORK/pkg                 # the package's file tree

step() { echo; echo "=== $*"; }

step "Source"
if [ -n "$(git -C "$SRC" status --porcelain --untracked-files=no)" ]; then
    echo "Note: $SRC has uncommitted changes; they are not packaged."
fi
git clone -q "$SRC" "$WORK/ethercat"
git -C "$WORK/ethercat" checkout -q "$(git -C "$SRC" rev-parse HEAD)"
REV=$(git -C "$WORK/ethercat" rev-parse --short=8 HEAD)
echo "master $REV"

step "Userspace master"
(
    cd "$WORK/ethercat"
    ./bootstrap >/dev/null
    ./configure -q --enable-uspace-master --prefix=$PREFIX
    make -s -j"$JOBS"
    make -s install DESTDIR="$MASTER_ROOT"
) >"$WORK/master.log" 2>&1 || { tail -30 "$WORK/master.log"; exit 1; }
if ! LD_LIBRARY_PATH="$MASTER_ROOT$PREFIX/lib" "$MASTER_ROOT$PREFIX/bin/ec_master" -h 2>&1 \
        | grep -q xdp-native; then
    echo "The master was built without the XDP transports: install libxdp-dev." >&2
    exit 1
fi

step "Package tree"
install -d "$ROOT$PREFIX/bin" "$ROOT$PREFIX/lib" "$ROOT$PREFIX/etc" "$ROOT$PREFIX/include" \
    "$ROOT$PREFIX/share/doc" "$ROOT/usr/bin" "$ROOT/DEBIAN"
install -m 755 "$MASTER_ROOT$PREFIX/bin/ec_master" \
    "$MASTER_ROOT$PREFIX/bin/ethercat" "$ROOT$PREFIX/bin/"
cp -a "$MASTER_ROOT$PREFIX"/lib/libethercat.so.2* "$ROOT$PREFIX/lib/"
# The master's headers, so that HAL components (such as ecat) can be built
# against the package: they link libethercat.so.2 by its full name.
install -m 644 "$MASTER_ROOT$PREFIX"/include/*.h "$ROOT$PREFIX/include/"
strip --strip-unneeded "$ROOT$PREFIX"/bin/* "$ROOT$PREFIX"/lib/libethercat.so.2.*
ln -s $PREFIX/bin/ethercat "$ROOT/usr/bin/ethercat-uspace"

cat >"$ROOT$PREFIX/etc/ethercat-uspace.conf" <<EOF
# Settings for "sudo ethercat-uspace start", the free-running master that the
# ethercat-uspace tool can use before LinuxCNC starts. In LinuxCNC, the ecat
# component takes its settings from ethercat.yaml instead.
# Arguments after "--" replace these: sudo ethercat-uspace start -- -i eth0
#
# The network interface of the EtherCAT bus (see "ip link"):
#INTERFACE=enp2s0
# raw (any network card), xdp-native (driver with XDP support, such as igc):
#TRANSPORT=raw
EOF

install -m 644 "$HERE/README.md" "$ROOT$PREFIX/share/doc/"
echo "userspace EtherCAT master: $(git -C "$WORK/ethercat" remote get-url origin 2>/dev/null || echo "$SRC") $(git -C "$WORK/ethercat" rev-parse HEAD)" \
    >"$ROOT$PREFIX/share/doc/VERSIONS"
install -m 644 "$WORK/ethercat/COPYING" "$ROOT$PREFIX/share/doc/COPYING"
install -m 644 "$WORK/ethercat/COPYING.LESSER" "$ROOT$PREFIX/share/doc/COPYING.LESSER"

step "Control file"
. /etc/os-release
ARCH=$(dpkg --print-architecture)
# The date is UTC, as on the CI machines.
VERSION=$BASE_VERSION+git$(date -u +%Y%m%d).$REV~$ID$VERSION_ID
MAINTAINER="$(git -C "$WORK/ethercat" log -1 --format='%an <%ae>')"

# Library dependencies of the binaries, without libethercat.so.2 (in the
# package). No LinuxCNC: the master does not need it.
mkdir -p "$WORK/shlibs/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' $PKG $PKG \
    >"$WORK/shlibs/debian/control"
SHLIBS=$(cd "$WORK/shlibs" && dpkg-shlibdeps -O --ignore-missing-info \
    -l"$ROOT$PREFIX/lib" "$ROOT$PREFIX"/bin/* "$ROOT$PREFIX"/lib/libethercat.so.2.* 2>/dev/null \
    | sed -n 's/^shlibs:Depends=//p')

cat >"$ROOT/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINTAINER
Depends: $SHLIBS
Section: misc
Priority: optional
Homepage: https://github.com/MetalMusings/ethercat
Description: Userspace EtherCAT master for LinuxCNC (ecat), experimental
 The IgH EtherCAT master built as a library, without a kernel module, for
 the ecat HAL component (https://github.com/MetalMusings/ecat), with the
 ethercat-uspace tool and a free-running master daemon. Installed in
 $PREFIX, apart from an installed kernel master.
 Since 1.1 without lcec_uspace, which is frozen: keep 1.0 for it.
EOF
echo "$PREFIX/etc/ethercat-uspace.conf" >"$ROOT/DEBIAN/conffiles"

step "Package"
mkdir -p "$OUTDIR"
DEB=$OUTDIR/${PKG}_${VERSION}_$ARCH.deb
dpkg-deb --root-owner-group -Zxz --build "$ROOT" "$DEB" >/dev/null
dpkg-deb --info "$DEB" | sed -n '/Package:/,$p'
dpkg-deb --contents "$DEB" | awk '{print $6, $7, $8}'
echo
echo "Built $DEB"
