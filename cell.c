/*
 * CELL - A SMALL LINUX SANDBOX
 *
 * Copyright (C) 2022-2026 Ali Gholami Rudi <ali at rudi dot ir>
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
#include <libgen.h>
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
#define NENV		128
#define NETH		8

#define LEN(a)		(sizeof(a) / sizeof((a)[0]))

static char *cenv[NENV] = {
	"PATH=/foe/bin:/opt/bin:/bin:/sbin:/usr/bin:/usr/sbin",
	"TERM=linux",
	"EDITOR=vi",
	"USER=foe",
	"HOME=/foe",
	"PS1=> ",
};

int pivot_root(const char *new_root, const char *put_old);

static void die(char *msg)
{
	perror(msg);
	exit(1);
}

static int csplit(char **dst, int sz, char *s, int c)
{
	int n = 0;
	dst[n++] = s;
	while (n < sz && (s = strchr(s, c)) != NULL) {
		*s++ = '\0';
		dst[n++] = s;
	}
	while (n < sz)
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

static int writefile(char *dir, char *ent, char *dat)
{
	char path[256];
	int fd;
	snprintf(path, sizeof(path), "%s/%s", dir, ent);
	if ((fd = open(path, O_WRONLY)) < 0)
		return 1;
	write(fd, dat, strlen(dat));
	close(fd);
	return 0;
}

static int cgroup_limit(char *dir, int pid, char **opts)
{
	char ctl[256], grp[256], key[256], dat[32];
	char *ctls[] = {"memory", "cpu", "pids", "io", "cpuset"};
	int mark[LEN(ctls)] = {0};
	char *val;
	int i, j;
	for (i = 0; opts[i] != NULL; i++) {
		for (j = 0; j < LEN(ctls); j++)
			if (strncmp(ctls[j], opts[i], strlen(ctls[j])) == 0)
				if (opts[i][strlen(ctls[j])] == '.')
					mark[j] = 1;
	}
	for (i = 0; i < LEN(ctls); i++) {
		if (mark[i]) {
			strcat(ctl, " +");
			strcat(ctl, ctls[i]);
		}
	}
	snprintf(grp, sizeof(grp), "%s", dir);
	writefile(dirname(grp), "cgroup.subtree_control", ctl);
	snprintf(dat, sizeof(dat), "%d\n", pid);
	if (mkdir(dir, 0755) < 0 && errno != EEXIST)
		return 1;
	if (writefile(dir, "cgroup.procs", dat) != 0)
		return 1;
	if (writefile(dir, "cgroup.subtree_control", ctl) != 0)
		return 1;
	for (i = 0; opts[i] != NULL; i++) {
		snprintf(key, sizeof(key), "%s", opts[i]);
		if ((val = strchr(key, '=')) != NULL) {
			val[0] = '\0';
			if (writefile(dir, key, val + 1) != 0)
				return 1;
		}
	}
	return 0;
}

static int env_set(char *envs[], int env_n, char *val)
{
	char *eq = strchr(val, '=');
	int i;
	if (!eq)
		return 1;
	for (i = 0; i + 1 < env_n; i++)
		if (!envs[i] || !strncmp(envs[i], val, eq - val + 1))
			break;
	if (i + 1 < env_n)
		envs[i] = val;
	return i + 1 >= env_n;
}

static int cell_pid;

static void signalhandle(int n)
{
	kill(cell_pid, n);
}

int main(int argc, char *argv[])
{
	char *init_base[4] = {"/bin/sh"};
	char *base = NULL;
	char *base_dirs[4] = {NULL};
	char **init = init_base;
	char *romnt[NMNT][4];
	char *rwmnt[NMNT][4];
	char *devcp[NMNT];
	char *netns = NULL;
	int romnt_n = 0;
	int rwmnt_n = 0;
	int devcp_n = 0;
	char *rlim[8];
	char *cgrp[32];
	int rlim_n = 0;
	int audio = 0, vgafb = 0, kvm = 0, video = 0;
	char *mktmp = NULL;
	char *mkrun = NULL;
	char *mkshm = NULL;
	char *mkdev = "size=64k,nr_inodes=64,mode=755";
	char *mkdevfs = NULL;
	char *mksys = NULL;
	char *mkcgroup = NULL;
	int uid = 99, gid = 99;
	unsigned long cln_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC;
	unsigned long romnt_flags = MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOATIME;
	unsigned long rwmnt_flags = MS_BIND | MS_NOSUID | MS_NODEV | MS_NOATIME;
	unsigned long cap = 0, caparg;
	unsigned long base_flags = romnt_flags;
	int i;
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		switch (argv[i][1]) {
		case 'n':
			if (argv[i][2])
				netns = argv[i] + 2;
			else
				cln_flags |= CLONE_NEWNET;
			break;
		case 'u':
			uid = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
			break;
		case 'g':
			gid = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
			break;
		case 'R':
			base_flags &= ~MS_RDONLY;
			base = argv[i][2] ? argv[i] + 2 : argv[++i];
			csplit(base_dirs, 3, base, ':');
			break;
		case 'r':
			base = argv[i][2] ? argv[i] + 2 : argv[++i];
			csplit(base_dirs, 3, base, ':');
			break;
		case 'm':
			csplit(romnt[romnt_n++], 2, argv[i][2] ? argv[i] + 2 : argv[++i], ':');
			break;
		case 'M':
			csplit(rwmnt[rwmnt_n++], 2, argv[i][2] ? argv[i] + 2 : argv[++i], ':');
			break;
		case 'l':
			if (rlim_n < LEN(rlim))
				rlim[rlim_n++] = argv[i][2] ? argv[i] + 2 : argv[++i];
			break;
		case 'L':
			csplit(cgrp, LEN(cgrp), argv[i][2] ? argv[i] + 2 : argv[++i], ',');
			break;
		case 'c':
			sscanf(argv[i][2] ? argv[i] + 2 : argv[++i], "%lx", &caparg);
			cap = caparg ? cap | caparg : 0;
			break;
		case 'e':
			env_set(cenv, LEN(cenv), argv[i][2] ? argv[i] + 2 : argv[++i]);
			break;
		case 't':
			mktmp = argv[i][2] ? argv[i] + 2 : "size=256m,nr_inodes=4k,mode=777";
			break;
		case 'i':
			init_base[0] = argv[i][2] ? argv[i] + 2 : argv[++i];
			break;
		case 'd':
			if (argv[i][2] == 'm')
				mkdev = argv[i] + 3;
			if (argv[i][2] == 'M')
				mkdevfs = argv[i] + 3;
			if (argv[i][2] == '/' && devcp_n < LEN(devcp))
				devcp[devcp_n++] = argv[i] + 2;
			if (argv[i][2] == 'a')
				audio = 1;
			if (argv[i][2] == 'v')
				video = 1;
			if (argv[i][2] == 'f')
				vgafb = 1;
			if (argv[i][2] == 'k')
				kvm = 1;
			if (argv[i][2] == 's')
				mkshm = argv[i][3] ? argv[i] + 3 : "size=256m,nr_inodes=4k,mode=777";
			if (argv[i][2] == 'r')
				mkrun = argv[i][3] ? argv[i] + 3 : "size=256m,nr_inodes=4k,mode=777";
			break;
		case 's':
			if (argv[i][2] == 'm')
				mksys = argv[i] + 3;
			if (argv[i][2] == 'g')
				mkcgroup = argv[i] + 3;
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
		printf("  -r root        root directory (ro -r, rw -R, overlay root:lower:work)\n");
		printf("  -u pid         process uid (%d)\n", uid);
		printf("  -g gid         process gid (%d)\n", gid);
		printf("  -m src:dst     mount a file/directory, e.g. -m/mnt/foe:foe (ro -m, rw -M)\n");
		printf("  -e E=V         set an environment variable\n");
		printf("  -t[opts]       mount tmpfs on /tmp\n");
		printf("  -sm[opts]      mount /sys\n");
		printf("  -sg[opts]      mount cgroup2 filesystem in /sys/fs/cgroup\n");
		printf("  -dm[opts]      mount tmpfs on /dev (mounted by default)\n");
		printf("  -dM[opts]      mount devtmpfs on /dev (unsafe)\n");
		printf("  -ds[opts]      mount tmpfs on /dev/shm\n");
		printf("  -dr[opts]      mount tmpfs on /run\n");
		printf("  -d/dev/name    create a copy of /dev/name\n");
		printf("  -da            create audio devices\n");
		printf("  -dv            create video capture devices\n");
		printf("  -df            create framebuffer devices\n");
		printf("  -dk            create kvm device\n");
		printf("  -l Xn          resource limits (p: nproc, f: nofiles, d: data, c: core)\n");
		printf("  -L /grp,key=n  cgroup v2 limits (i.e., -L/sys/fs/cgroup/foe,memory.max=1000000)\n");
		printf("  -c msk         mask of additional capabilities not to drop\n");
		printf("  -c 0           drop all capabilites\n");
		printf("  -n             create a new network namespace\n");
		printf("  -nnetns        switch to the given named network namespace\n");
		return 0;
	}
	/* create a new namespace */
	if (unshare(cln_flags) < 0)
		die("unshare failed");
	/* change the network namespace */
	if (netns) {
		char path[256];
		int nsfd;
		snprintf(path, sizeof(path), "/var/run/netns/%s", netns);
		nsfd = open(path, O_RDONLY);
		if (nsfd < 0 || setns(nsfd, CLONE_NEWNET) < 0)
			die("changing netns failed");
		close(nsfd);
	}
	/* make FS private */
	if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		die("mount / failed");
	if (base_dirs[1] == NULL) {
		if (mount(base, base, NULL, MS_BIND | MS_NOSUID, NULL) < 0)
			die("mount base failed");
	} else {
		char opt[2048];
		snprintf(opt, sizeof(opt), "lowerdir=%s,upperdir=%s,workdir=%s",
			base_dirs[1], base, base_dirs[2]);
		if (mount("cell-overlay", base, "overlay", MS_NOSUID, opt) < 0)
			die("mount base overlay failed");
	}
	if (chdir(base) < 0)
		die("chdir base failed");
	/* home directory */
	mount("foe", "foe", NULL, rwmnt_flags, NULL);
	/* read-only mounts */
	for (i = 0; i < romnt_n; i++)
		mount(romnt[i][0], romnt[i][1], NULL, romnt_flags, NULL);
	/* read-write mounts */
	for (i = 0; i < rwmnt_n; i++)
		mount(rwmnt[i][0], rwmnt[i][1], NULL, rwmnt_flags, NULL);
	/* mount /dev */
	if (mkdev && mount("cell-dev", "dev", "tmpfs",
			MS_NOSUID | MS_NOEXEC | MS_NOATIME, mkdev) < 0)
		die("mount dev failed");
	umask(0);
	/* base devices */
	mknod("dev/null", S_IFCHR | 0666, makedev(1, 3));
	mknod("dev/zero", S_IFCHR | 0666, makedev(1, 5));
	mknod("dev/random", S_IFCHR | 0666, makedev(1, 8));
	mknod("dev/urandom", S_IFCHR | 0666, makedev(1, 9));
	mkdir("dev/pts", 0755);
	mount("cell-pty", "dev/pts", "devpts", MS_NOSUID | MS_NOEXEC | MS_NOATIME, NULL);
	mknod("dev/ptmx", S_IFCHR | 0666, makedev(5, 2));
	mknod("dev/tty", S_IFCHR | 0666, makedev(5, 0));
	mkdir("dev/net", 0755);
	mknod("dev/net/tun", S_IFCHR | 0666, makedev(10, 200));
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
			char path[256];
			while ((dp = readdir(snd)) != NULL) {
				snprintf(path, sizeof(path), "/dev/snd/%s", dp->d_name);
				dupnod(path);
			}
			closedir(snd);
		}
	}
	/* video capture devices */
	if (video) {
		dupnod("/dev/video0");
		dupnod("/dev/video1");
		dupnod("/dev/media0");
	}
	/* framebuffer devices */
	if (vgafb) {
		dupnod("/dev/fb0");
		dupnod("/dev/fb1");
	}
	/* kvm device */
	if (kvm)
		mknod("dev/kvm", S_IFCHR | 0666, makedev(10, 232));
	/* copy devices */
	for (i = 0; i < devcp_n; i++)
		if (devcp[i][0])
			dupnod(devcp[i]);
	/* mount /dev and /sys */
	if (mkdevfs)
		mount("cell-dev", "dev", "devtmpfs",
			MS_NOSUID | MS_NOEXEC | MS_NOATIME, mkdevfs);
	if (mksys)
		mount("cell-sys", "sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NOATIME, mksys);
	if (mkcgroup) {
		mkdir("sys/fs", 0755);
		mkdir("sys/fs/cgroup", 0755);
		mount("cell-cgroup", "sys/fs/cgroup", "cgroup2",
			MS_NOSUID | MS_NOEXEC | MS_NOATIME, mkcgroup);
	}
	/* mount /tmp */
	if (mktmp && mount("cell-tmp", "tmp", "tmpfs",
			MS_NOSUID | MS_NODEV | MS_NOATIME, mktmp) < 0)
		die("mount /tmp failed");
	if (mkshm)
		mkdir("dev/shm", 0777);
	if (mkshm && mount("cell-shm", "dev/shm", "tmpfs",
			MS_NOSUID | MS_NODEV | MS_NOATIME, mkshm) < 0)
		die("mount /dev/shm failed");
	if (mkrun && mount("cell-run", "run", "tmpfs",
			MS_NOSUID | MS_NODEV | MS_NOATIME, mkrun) < 0)
		die("mount /run failed");
	/* set cgroup limits */
	if (cgrp[0] && cgroup_limit(cgrp[0], getpid(), cgrp + 1) != 0)
		die("cannot set cgroup limits");
	/* switch to the new root */
	if (pivot_root(".", ".") < 0)
		die("pivot_root failed");
	if (umount2(".", MNT_DETACH) < 0)
		die("umount2 root failed");
	mount("/", "/", NULL, base_flags | MS_REMOUNT, NULL);
	if ((cell_pid = fork()) < 0)
		die("fork failed");
	if (cell_pid == 0) {
		gid_t groups[] = {gid};
		if (mount("none", "proc", "proc", 0, NULL) < 0)
			die("mount proc failed");
		if (setgroups(1, groups) < 0)
			die("setgroups failed");
		for (i = 0; i < 64; i++)
			if (~cap & (1ul << i))
				prctl(PR_CAPBSET_DROP, i, 0, 0, 0);
		for (i = 0; i < rlim_n && rlim[i][0]; i++) {
			struct rlimit rl;
			rl.rlim_cur = rlim[i][1] ? atol(rlim[i] + 1) : RLIM_INFINITY;
			rl.rlim_max = rlim[i][1] ? atol(rlim[i] + 1) : RLIM_INFINITY;
			if (rlim[i][0] == 'p')
				setrlimit(RLIMIT_NPROC, &rl);
			if (rlim[i][0] == 'f')
				setrlimit(RLIMIT_NOFILE, &rl);
			if (rlim[i][0] == 'd')
				setrlimit(RLIMIT_DATA, &rl);
			if (rlim[i][0] == 'c')
				setrlimit(RLIMIT_CORE, &rl);
		}
		if (setresgid(gid, gid, gid) < 0)
			die("setresgid failed");
		if (setresuid(uid, uid, uid) < 0)
			die("setresuid failed");
		execvpe(init[0], init, cenv);
		exit(1);
	}
	/* wait for the child */
	signal(SIGINT, signalhandle);
	signal(SIGTERM, signalhandle);
	signal(SIGPIPE, signalhandle);
	signal(SIGHUP, signalhandle);
	while (wait(NULL) >= 0 || errno != ECHILD)
		;
	return 0;
}
