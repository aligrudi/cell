#!/bin/sh
# A wrapper script around Neatbox
ROOT="${ROOT-/root/box}"
OPTS="-t -lp200 -lf500 -ld320000000"
BOX="/path/to/neatbox/box"
# for mk command
ROOTFS="/path/to/rootfs.tar.gz"
ROOTCP="/more/root/files/dir"

box_mk() {
	root="$1"
	if test -z "$root"; then
		echo "box: sandbox root not specified"
		exit 1
	fi
	if test -d "$root" -o -f $root; then
		echo "box: directory $root already exists"
		exit 1
	fi
	# copy rootfs
	mkdir $root
	cd $root
	tar xf $ROOTFS
	test -d "$ROOTCP" && cp -r $ROOTCP $root/
	test -d $root/foe && chown -R 99:99 $root/foe
	# fill /etc
	echo "foe:x:99:99:foe:/foe:/bin/mksh" >>$root/etc/passwd
	echo "foo::99:" >>$root/etc/group
	echo "nameserver 4.2.2.4" >>$root/etc/resolv.conf
	# neatbox options
	echo $OPTS >$root.box
}

box_opts() {
	root=$(echo "$@" | sed -En 's/^.*-[rR] ([^ ]+).*$/\1/p')
	test -f $root.box && cat $root.box || echo -r $ROOT $OPTS
}

box_go() {
	exec $BOX $(box_opts "$@") "$@"
}

box_root() {
	exec $BOX -c80425fb -p0 -g0 $(box_opts "$@") "$@"
}

if test -z "$1"; then
	echo "box.sh command [opts]"
	echo
	echo "commands:"
	echo "  mk root     fill sandbox root directory"
	echo "  go opts     execute neatbox"
	echo "  root opts   execute neatbox as root"
	exit
fi

box_"$@"
