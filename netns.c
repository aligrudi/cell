#include <fcntl.h>
#include <linux/rtnetlink.h>
#include <linux/veth.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define NLMSG_TAIL(nmsg)	((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

struct nl_req {
	struct nlmsghdr n;
	struct ifinfomsg i;
	char buf[4096];
};

struct rt_req {
	struct nlmsghdr n;
	struct rtmsg rt;
	char buffer[4096];
};

static int addattr_l(struct nlmsghdr *n, int maxlen, int type, void *dat, int datlen)
{
	int attr_len = RTA_LENGTH(datlen);
	int newlen = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(attr_len);
	struct rtattr *rta;
	if (newlen > maxlen)
		return 1;
	rta = NLMSG_TAIL(n);
	rta->rta_type = type;
	rta->rta_len = attr_len;
	if (datlen)
		memcpy(RTA_DATA(rta), dat, datlen);
	n->nlmsg_len = newlen;
	return 0;
}

static struct rtattr *addattr_nest(struct nlmsghdr *n, int maxlen, int type)
{
	struct rtattr *nest = NLMSG_TAIL(n);
	addattr_l(n, maxlen, type, NULL, 0);
	return nest;
}

static void addattr_nest_end(struct nlmsghdr *n, struct rtattr *nest)
{
	nest->rta_len = (void *) NLMSG_TAIL(n) - (void *) nest;
}

static int netns_recv(int fd)
{
	char buf[2000];
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = sizeof(buf),
	};
	struct msghdr msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	int len = recvmsg(fd, &msg, 0);
	struct nlmsghdr *hdr = (struct nlmsghdr *) buf;
	int nlmsglen = hdr->nlmsg_len;
	int datalen = nlmsglen - sizeof(*hdr);
	if (len <= 0)
		return 1;
	if (datalen < 0 || nlmsglen > len) {
		if (msg.msg_flags & MSG_TRUNC)
			return 2;
		return 1;
	}
	if (hdr->nlmsg_type == NLMSG_ERROR)
		return 1;
	return 0;
}

static int netns_send(int fd, struct nlmsghdr *n)
{
	static int seq;
	struct iovec iov = {
		.iov_base = n,
		.iov_len = n->nlmsg_len
	};
	struct msghdr msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1
	};
	n->nlmsg_seq = seq++;
	if (sendmsg(fd, &msg, 0) < 0)
		return 1;
	return netns_recv(fd);
}

int netns_ifup(char *dev, unsigned addr, unsigned mask)
{
	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	struct sockaddr_in sa;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(struct ifreq));
	memset(&sa, 0, sizeof(struct sockaddr_in));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", dev);
	sa.sin_family = AF_INET;
	sa.sin_port = 0;
	sa.sin_addr.s_addr = addr;
	memcpy(&ifr.ifr_addr, &sa, sizeof(struct sockaddr));
	if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
		close(fd);
		return 1;
	}
	sa.sin_addr.s_addr = mask;
	memcpy(&ifr.ifr_addr, &sa, sizeof(struct sockaddr));
	ioctl(fd, SIOCSIFNETMASK, &ifr);
	ifr.ifr_flags |= IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
	ioctl(fd, SIOCSIFFLAGS, &ifr);
	close(fd);
	return 0;
}

/* ip link add ve1 type veth peer name ve2 */
int netns_veth(int fd, char *dev1, char *dev2)
{
	int flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
	struct nl_req req = {
		.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
		.n.nlmsg_flags = flags,
		.n.nlmsg_type = RTM_NEWLINK,
		.i.ifi_family = AF_NETLINK,
	};
	struct nlmsghdr *n = &req.n;
	int maxlen = sizeof(req);
	addattr_l(n, maxlen, IFLA_IFNAME, dev1, strlen(dev1) + 1);
	struct rtattr *linfo = addattr_nest(n, maxlen, IFLA_LINKINFO);
	addattr_l(&req.n, sizeof(req), IFLA_INFO_KIND, "veth", 5);
	struct rtattr *linfodata = addattr_nest(n, maxlen, IFLA_INFO_DATA);
	struct rtattr *peerinfo = addattr_nest(n, maxlen, VETH_INFO_PEER);
	n->nlmsg_len += sizeof(struct ifinfomsg);
	addattr_l(n, maxlen, IFLA_IFNAME, dev2, strlen(dev2) + 1);
	addattr_nest_end(n, peerinfo);
	addattr_nest_end(n, linfodata);
	addattr_nest_end(n, linfo);
	return netns_send(fd, n);
}

/* ip link set dev netns ns */
int netns_move(int fd, char *dev, int netns)
{
	struct nl_req req = {
		.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
		.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK,
		.n.nlmsg_type = RTM_NEWLINK,
		.i.ifi_family = AF_NETLINK,
	};
	addattr_l(&req.n, sizeof(req), IFLA_NET_NS_FD, &netns, 4);
	addattr_l(&req.n, sizeof(req), IFLA_IFNAME, dev, strlen(dev) + 1);
	return netns_send(fd, &req.n);
}

int netns_netlink(void)
{
	struct sockaddr_nl sa = {
		.nl_family = AF_NETLINK,
	};
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	bind(fd, (void *) &sa, sizeof(sa));
	return fd;
}

int netns_route(int fd, ...)
{
	/* initialise the request structure */
	int index = 3;
	unsigned gw = 3232236545;
	unsigned dst = 3232235776;
	struct rt_req req = {
		.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg)),
		/* set the flags that facilitates adding a route in routing table */
		.n.nlmsg_flags = NLM_F_REQUEST|NLM_F_CREATE,
		/* note that inet_rtm_newroute() is the fn in kernel which will be eventually called to add a new route in routing table */
		.n.nlmsg_type = RTM_NEWROUTE,
		.rt.rtm_family = AF_INET,
		.rt.rtm_table = RT_TABLE_MAIN,
		.rt.rtm_protocol = RTPROT_BOOT,		/* Route installed during boot*/
		.rt.rtm_scope = RT_SCOPE_UNIVERSE,
		.rt.rtm_type = RTN_UNICAST,		/* Gateway or direct route  */
	};
	/* Add routing info */
	addattr_l(&req.n, sizeof(req), RTA_GATEWAY, &gw, sizeof(gw));
	addattr_l(&req.n, sizeof(req), RTA_DST, &dst, sizeof(dst));
	addattr_l(&req.n, sizeof(req), RTA_OIF, &index, sizeof(index));
	/* For adding a route, the gateway, destination address and the interface will suffice, now the netlink packet is all set to go to the kernel */
	return netns_send(fd, &req.n);
}
