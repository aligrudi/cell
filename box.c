/*
 * NEATBOX - A SMALL LINUX SANDBOX
 *
 * Copyright (C) 2022-2024 Ali Gholami Rudi <ali at rudi dot ir>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>

#define NMNT		128
#define NDEV		128
#define NETH		8

int pivot_root(const char *new_root, const char *put_old);
int netns_netlink(void);
int netns_move(int fd, char *dev, int netns);
int netns_veth(int fd, char *v1, char *v2);
int netns_ifup(char *ifname, unsigned addr, unsigned mask);

static void die(char *msg)
{
	perror(msg);
	exit(1);
}

static int csplit(char **dst, char *s, int c)
{
	int n = 0;
	dst[n++] = s;
	while ((s = strchr(s, c)) != NULL) {
		*s++ = '\0';
		dst[n++] = s;
	}
	while (n < 16)
		dst[n++] = NULL;
	return n;
}

static int dupnod(char *src)
{
	struct stat st;
	if (stat(src, &st) != 0 || !S_ISCHR(st.st_mode))
		return 1;
	if (mknod(src + 1, S_IFCHR | 0666, st.st_rdev))
		return 1;
	return 0;
}

int main(int argc, char *argv[])
{
	char *init_base[4] = {"/bin/sh"};
	char *base = NULL;
	char **init = init_base;
	char *romnt[NMNT][4];
	char *rwmnt[NMNT][4];
	char *veth[NETH][4];
	unsigned veth_ip[NETH];
	int romnt_n = 0;
	int rwmnt_n = 0;
	int veth_n = 0;
	char *rlim[8];
	int rlim_n = 0;
	int mntdev = 0, mntsys = 0;
	int audio = 0, vgafb = 0;
	char *mktmp = NULL;
	int uid = 99, gid = 99;
	unsigned long cln_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC;
	unsigned long romnt_flags = MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOATIME;
	unsigned long rwmnt_flags = MS_BIND | MS_NOSUID | MS_NODEV | MS_NOATIME;
	unsigned long cap = 0;
	unsigned long base_flags = romnt_flags;
	int nsfd;
	int pid;
	int i;
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		switch (argv[i][1]) {
		case 'n':
			cln_flags |= CLONE_NEWNET;
			break;
		case 'p':
			uid = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
			break;
		case 'g':
			gid = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
			break;
		case 'R':
			base_flags &= ~MS_RDONLY;
			base = argv[i][2] ? argv[i] + 2 : argv[++i];
			break;
		case 'r':
			base = argv[i][2] ? argv[i] + 2 : argv[++i];
			break;
		case 'm':
			csplit(romnt[romnt_n++], argv[i][2] ? argv[i] + 2 : argv[++i], ':');
			break;
		case 'M':
			csplit(rwmnt[rwmnt_n++], argv[i][2] ? argv[i] + 2 : argv[++i], ':');
			break;
		case 'l':
			rlim[rlim_n++] = argv[i][2] ? argv[i] + 2 : argv[++i];
			break;
		case 'c':
			sscanf(argv[i][2] ? argv[i] + 2 : argv[++i], "%lx", &cap);
			break;
		case 'e':
			csplit(veth[veth_n++], argv[i][2] ? argv[i] + 2 : argv[++i], ':');
			break;
		case 't':
			mktmp = argv[i][2] ? argv[i] + 2 : "size=256m,nr_inodes=4k,mode=777";
			break;
		case 'i':
			init_base[0] = argv[i][2] ? argv[i] + 2 : argv[++i];
			break;
		case 'd':
			if (argv[i][2] == '\0')
				mntdev = 1;
			if (argv[i][2] == 'a')
				audio = 1;
			if (argv[i][2] == 'f')
				vgafb = 1;
			break;
		case 's':
			mntsys = 1;
			break;
		default:
			argc = 1;
			break;
		}
	}
	if (i < argc)
		init = argv + i;
	if (argc < 2) {
		printf("Usage: %s [options] init\n\n", argv[0]);
		printf("Options:\n");
		printf("  -r root        root directory (ro -r, rw -R)\n");
		printf("  -p pid         process pid (%d)\n", uid);
		printf("  -g gid         process gid (%d)\n", gid);
		printf("  -m mnt         mount directory src:dst (ro -m, rw -M)\n");
		printf("  -t             mount /tmp\n");
		printf("  -s             mount host's /sys\n");
		printf("  -d             mount host's /dev\n");
		printf("  -da            create audio devices\n");
		printf("  -df            create framebuffer devices\n");
		printf("  -l Xn          set resource limits (p: nproc, f: nofiles, d: data)\n");
		printf("  -c msk         mask of capabilities not to drop\n");
		printf("  -n             make a new network namespace\n");
		printf("  -e v1:v2:addr  add a veth pair\n");
		return 0;
	}
	/* keep the network namespace of the parent */
	for (i = 0; i < veth_n; i++)
		inet_pton(AF_INET, veth[i][2], &veth_ip);
	if (veth_n > 0 && (nsfd = open("/proc/self/ns/net", O_RDONLY)) < 0)
		die("cannot open netns");
	/* create a new namespace */
	if (unshare(cln_flags) < 0)
		die("unshare failed");
	/* set up the network namespace */
	if (cln_flags & CLONE_NEWNET)
		netns_ifup("lo", 0x0100007f, 0x000000ff);
	if (veth_n > 0) {
		int nofd = open("/proc/self/ns/net", O_RDONLY);
		int nlfd = netns_netlink();
		for (i = 0; i < veth_n; i++) {
			netns_veth(nlfd, veth[i][0], veth[i][1]);
			netns_ifup(veth[i][1], veth_ip[i] + 0x01000000, 0x00ffffff);
			netns_move(nlfd, veth[i][0], nsfd);
			setns(nsfd, CLONE_NEWNET);
			netns_ifup(veth[i][0], veth_ip[i], 0x00ffffff);
			setns(nofd, CLONE_NEWNET);
		}
		close(nofd);
		close(nsfd);
		close(nlfd);
	}
	/* make FS private */
	if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		die("mount / failed");
	if (mount(base, base, NULL, MS_BIND | MS_NOSUID, NULL) < 0)
		die("mount base failed");
	if (chdir(base) < 0)
		die("chdir base failed");
	if (mount("none", "proc", "proc", 0, NULL) < 0)
		die("mount proc failed");
	/* home directory */
	mount("foe", "foe", NULL, rwmnt_flags, NULL);
	/* read-only mounts */
	for (i = 0; i < romnt_n; i++)
		mount(romnt[i][0], romnt[i][1], NULL, romnt_flags, NULL);
	/* read-write mounts */
	for (i = 0; i < rwmnt_n; i++)
		mount(rwmnt[i][0], rwmnt[i][1], NULL, rwmnt_flags, NULL);
	/* mount /dev */
	if (mount("foe-dev", "dev", "tmpfs", MS_NOSUID | MS_NOEXEC | MS_NOATIME,
			"size=64k,nr_inodes=64,mode=755") < 0)
		die("mount dev failed");
	umask(0);
	/* base devices */
	mknod("dev/null", S_IFCHR | 0666, makedev(1, 3));
	mknod("dev/zero", S_IFCHR | 0666, makedev(1, 5));
	mknod("dev/random", S_IFCHR | 0666, makedev(1, 8));
	mknod("dev/urandom", S_IFCHR | 0666, makedev(1, 9));
	mkdir("dev/pts", 0755);
	mount("foe-pty", "dev/pts", "devpts", MS_NOSUID | MS_NOEXEC | MS_NOATIME, NULL);
	mknod("dev/ptmx", S_IFCHR | 0666, makedev(5, 2));
	mknod("dev/tty", S_IFCHR | 0666, makedev(5, 0));
	mkdir("dev/net", 0755);
	mknod("dev/net/tun", S_IFCHR | 0666, makedev(10, 200));
	mknod("dev/kvm", S_IFCHR | 0666, makedev(10, 232));
	/* audio devices */
	if (audio) {
		DIR *snd = opendir("/dev/snd");
		mkdir("dev/snd", 0755);
		dupnod("/dev/mixer");
		dupnod("/dev/mixer1");
		dupnod("/dev/mixer2");
		dupnod("/dev/dsp");
		dupnod("/dev/dsp1");
		dupnod("/dev/dsp2");
		if (snd != NULL) {
			struct dirent *dp;
			char path[128];
			while ((dp = readdir(snd)) != NULL) {
				snprintf(path, sizeof(path), "/dev/snd/%s", dp->d_name);
				dupnod(path);
			}
			closedir(snd);
		}
		dupnod("/dev/video0");
		dupnod("/dev/video1");
		dupnod("/dev/media0");
	}
	/* framebuffer devices */
	if (vgafb) {
		dupnod("/dev/fb0");
		dupnod("/dev/fb1");
	}
	/* mount /dev and /sys */
	if (mntdev)
		mount("foe-dev", "dev", "devtmpfs", MS_NOSUID | MS_NOEXEC | MS_NOATIME, NULL);
	if (mntsys)
		mount("foe-sys", "sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NOATIME, NULL);
	/* mount /tmp */
	if (mktmp != NULL && mount("foe-tmp", "tmp", "tmpfs",
			MS_NOSUID | MS_NODEV | MS_NOATIME, mktmp) < 0)
		die("mount tmp failed");
	/* switching the root */
	mkdir(".root", 0755);
	if (pivot_root(".", ".root") < 0)
		die("pivot_root failed");
	if (umount2(".root", MNT_DETACH) < 0)
		die("umount2 root failed");
	rmdir(".root");
	mount("/", "/", NULL, base_flags | MS_REMOUNT, NULL);
	if ((pid = fork()) < 0)
		die("fork failed");
	if (pid == 0) {
		gid_t groups[] = {gid};
		char *envs[] = {"USER=foe", "HOME=/foe", "TERM=linux", "PS1=> ",
			"LD_LIBRARY_PATH=/opt/lib", "EDITOR=vi",
			"PATH=/foe/bin:/opt/bin:/bin:/sbin:/usr/bin", NULL};
		if (mount("none", "proc", "proc", 0, NULL) < 0)
			die("mount proc failed");
		if (setgroups(1, groups) < 0)
			die("setgroups failed");
		for (i = 0; i < 64; i++)
			if (~cap & (1ul << i))
				prctl(PR_CAPBSET_DROP, i, 0, 0, 0);
		for (i = 0; i < rlim_n && rlim[i][0]; i++) {
			struct rlimit rl;
			rl.rlim_cur = atol(rlim[i] + 1);
			rl.rlim_max = atol(rlim[i] + 1);
			if (rlim[i][0] == 'p')
				setrlimit(RLIMIT_NPROC, &rl);
			if (rlim[i][0] == 'f')
				setrlimit(RLIMIT_NOFILE, &rl);
			if (rlim[i][0] == 'd')
				setrlimit(RLIMIT_DATA, &rl);
		}
		if (setresgid(gid, gid, gid) < 0)
			die("setresgid failed");
		if (setresuid(uid, uid, uid) < 0)
			die("setresuid failed");
		execve(init[0], init, envs);
		exit(1);
	}
	printf("foe: %d -> %d\n", getpid(), pid);
	/* wait for the child */
	while (1) {
		int cp = wait(NULL);
		printf("foe: %d exited\n", cp);
		if (cp == pid)
			break;
	}
	return 0;
}
