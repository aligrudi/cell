#!/bin/sh
# Make a sandbox directory and generate a script to run cell

# cell program path
CELL="/path/to/cell/cell"
# rootfs archive to extract
ROOTFS="/path/to/rootfs.tar.gz"
# rootfs files to copy
ROOTCP="/more/root/files/dir"

# additional options to pass to cell
OPTS="-t -lp100 -lf100 -ld100000000"

root="$1"
lower="$2"
if test -z "$root"; then
	echo "usage: $0 root [lower]"
	exit 1
fi

# prepare root directory
if ! mkdir "$root"; then
	echo "$0: cannot create directory $root"
	exit 1
fi
chmod 755 "$root"

# fill root directory
if test -z "$lower"; then
	test -f $ROOTFS && (cd $root && tar xf $ROOTFS)
	test -d $ROOTCP && cp -r $ROOTCP/* $root/
	test -d $root/foe && chown -R 99:99 $root/foe
	echo "foe:x:99:99:foe:/foe:/bin/mksh" >>$root/etc/passwd
	echo "foo::99:" >>$root/etc/group
	echo "foe:x:0::::::" >>$root/etc/shadow
	echo "nameserver 4.2.2.4" >>$root/etc/resolv.conf
else
	mkdir $root.tmp
fi

# generate cell invocation script
echo '#!/bin/sh' >$root.sh
if test -z "$lower"; then
	echo "base=\"$root\"" >>$root.sh
else
	echo "base=\"$root:$lower:$root.tmp\"" >>$root.sh
fi
echo "root=\"\${ROOT+ -R\$base -c00405fb -p0 -g0}\"" >>$root.sh
echo "exec $CELL -r\$base $OPTS \$root \"\$@\"" >>$root.sh
chmod 700 $root.sh
