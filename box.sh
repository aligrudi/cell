#!/bin/sh
# Make a sandbox directory and generate a script to run Neatbox

ROOT="${ROOT-/root/box}"
OPTS="-t -lp100 -lf100 -ld100000000"
BOX="/path/to/neatbox/box"

ROOTFS="/path/to/rootfs.tar.gz"
ROOTCP="/more/root/files/dir"

root="$1"
if test "$#" != "1"; then
	echo "usage: $0 root"
	exit 1
fi
if test -d "$root" -o -f $root; then
	echo "box.sh: directory $root already exists"
	exit 1
fi
# copy rootfs
mkdir $root
cd $root
tar xf $ROOTFS
test -d "$ROOTCP" && cp -r $ROOTCP/* $root/
test -d $root/foe && chown -R 99:99 $root/foe
# fill /etc
echo "foe:x:99:99:foe:/foe:/bin/mksh" >>$root/etc/passwd
echo "foo::99:" >>$root/etc/group
echo "foe:x:0::::::" >>$root/etc/shadow
echo "nameserver 4.2.2.4" >>$root/etc/resolv.conf
# neatbox options
echo '#!/bin/sh' >$root.sh
echo "root=\"\${ROOT+ -R$root -c00405f9 -p0 -g0}\"" >>$root.sh
echo "exec $BOX -r$root $OPTS \$root \"\$@\"" >>$root.sh
chmod 755 $root
chmod 700 $root.sh
