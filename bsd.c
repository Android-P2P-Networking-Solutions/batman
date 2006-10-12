/*
 * Copyright (C) 2006 BATMAN contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 *
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/route.h>
#include <net/if.h>
#include <net/if_tun.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <err.h>

#include "os.h"
#include "batman.h"

void set_forwarding(int state)
{
	int mib[4];

	/* FreeBSD allows us to set the boolean IP forwarding
	 * sysctl to anything. Check the value for sanity. */
	if (state < 0 || state > 1) {
		errno = EINVAL;
		err(1, "set_forwarding: %i", state);
	}

	/* "net.inet.ip.forwarding" */
	mib[0] = CTL_NET;
	mib[1] = PF_INET;
	mib[2] = IPPROTO_IP;
	mib[3] = IPCTL_FORWARDING;

	if (sysctl(mib, 4, NULL, 0, (void*)&state, sizeof(state)) == -1)
		err(1, "Cannot enable packet forwarding");
}

int get_forwarding(void)
{
	int state;
	size_t len;
	int mib[4];

	/* "net.inet.ip.forwarding" */
	mib[0] = CTL_NET;
	mib[1] = PF_INET;
	mib[2] = IPPROTO_IP;
	mib[3] = IPCTL_FORWARDING;

	len = sizeof(int);

	if (sysctl(mib, 4, &state, &len, NULL, 0) == -1)
		err(1, "Cannot tell if packet forwarding is enabled");

	return state;
}

int bind_to_iface( int udp_recv_sock, char *dev )
{
	/* XXX: Is binding a socket to a specific
	 * interface possible in *BSD? */
	return 1;
}

/* Message structure used to interface the kernel routing table.
 * See route(4) for details on the message passing interface for
 * manipulating the kernel routing table.
 * We transmit at most two addresses at once: a destination host
 * and a gateway.
 */
struct rt_msg
{
	struct rt_msghdr hdr;
	struct sockaddr_in dest;
	struct sockaddr_in gateway;
};

void add_del_route(unsigned int dest, unsigned int router, int del,
		char *dev, int sock)
{
	char str1[16], str2[16];
	int rt_sock;
	static unsigned int seq = 0;
	struct rt_msg msg;
	struct sockaddr_in *so_dest, *so_gateway;
	struct sockaddr_in ifname;
	socklen_t ifname_len;
	ssize_t len;
	pid_t pid;

	so_dest = NULL;
	so_gateway = NULL;

	memset(&msg, 0, sizeof(struct rt_msg));

	inet_ntop(AF_INET, &dest, str1, sizeof (str1));
	inet_ntop(AF_INET, &router, str2, sizeof (str2));

	msg.hdr.rtm_type = del ? RTM_DELETE : RTM_ADD;
	msg.hdr.rtm_version = RTM_VERSION;
	msg.hdr.rtm_flags = RTF_STATIC | RTF_UP | RTF_HOST;
	msg.hdr.rtm_addrs = RTA_DST;

	so_dest = &msg.dest;
	so_dest->sin_family = AF_INET;
	so_dest->sin_len = sizeof(struct sockaddr_in);
	so_dest->sin_addr.s_addr = dest;

	msg.hdr.rtm_msglen = sizeof(struct rt_msghdr)
		+ (2 * sizeof(struct sockaddr_in));

	msg.hdr.rtm_flags |= RTF_GATEWAY;
	msg.hdr.rtm_addrs |= RTA_GATEWAY;

	so_gateway = &msg.gateway;
	so_gateway->sin_family = AF_INET;
	so_gateway->sin_len = sizeof(struct sockaddr_in);

	if (dest != router) {
		/* This is not a direct route; router is our gateway
		 * to the remote host.
		 * We essentially run 'route add <remote ip> <gateway ip> */
		so_gateway->sin_addr.s_addr = router;
	} else {
		/* This is a direct route to the remote host.
		 * We use our own IP address as gateway.
		 * We essentially run 'route add <remote ip> <local ip> */
		ifname_len = sizeof(struct sockaddr_in);
		if (getsockname(sock, (struct sockaddr*)&ifname, &ifname_len) == -1) {
			err(1, "Could not get name of interface %s", dev);
		}
		so_gateway->sin_addr.s_addr = ifname.sin_addr.s_addr;
	}

	output("%s route to %s via %s\n", del ? "Deleting" : "Adding", str1, str2);

	rt_sock = socket(PF_ROUTE, SOCK_RAW, AF_INET);
	if (rt_sock < 0)
		err(1, "Could not open socket to routing table");

	pid = getpid();
	len = 0;
	seq++;

	/* Send message */
	do {
		msg.hdr.rtm_seq = seq;
		len = write(rt_sock, &msg, msg.hdr.rtm_msglen);
		if (len < 0) {
			warn("Error sending routing message to kernel");
			err(1, "Cannot %s route to %s",
				del ? "delete" : "add", str1);
		}
	} while (len < msg.hdr.rtm_msglen);

	/* Get reply */
	do {
		len = read(rt_sock, &msg, sizeof(struct rt_msg));
		if (len < 0)
			err(1, "Error reading from routing socket");
	} while (len > 0 && (msg.hdr.rtm_seq != seq || msg.hdr.rtm_pid != pid));

	/* Evaluate reply */
	if (msg.hdr.rtm_version != RTM_VERSION) {
		warn("routing message version mismatch: "
		    "compiled with version %i, "
		    "but running kernel uses version %i", RTM_VERSION,
		    msg.hdr.rtm_version);
	}

	if (msg.hdr.rtm_errno) {
		errno = msg.hdr.rtm_errno;
		err(1, "Cannot %s route to %s",
			del ? "delete" : "add", str1);
	}
}

#if defined(__OpenBSD__)
int open_tun_any(void)
{
	int i;
	int fd;
	char tundev[12]; /* "/dev/tunxxx\0" */

	for (i = 0; i < sizeof(tundev); i++)
		tundev[i] = '\0';

	/* Try opening tun device /dev/tun[0..255] */
	for (i = 0; i < 256; i++) {
		snprintf(tundev, sizeof(tundev), "/dev/tun%i", i);
		if ((fd = open(tundev, O_RDWR)) >= 0) {
			printf("Using /dev/tun%i\n", i);
			return fd;
		}
	}
	return -1;
}
#elif defined(__FreeBSD__)
int open_tun_any(void)
{
	int fd;
	struct stat buf;

	/* Open lowest unused tun device */
	if ((fd = open("/dev/tun", O_RDWR)) >= 0) {
		fstat(fd, &buf);
		printf("Using %s\n", devname(buf.st_rdev, S_IFCHR));
		return fd;
	}
	return -1;
}
#endif

/* Probe for tun interface availability */
int probe_tun()
{
	int fd;
	fd = open_tun_any();
	if (fd > 0)
		close(fd);
	return fd;
}

int del_dev_tun(int fd)
{
	return close(fd);
}

int add_dev_tun(struct batman_if *batman_if, unsigned int tun_addr, char *tun_dev, int *fd)
{
	struct ifreq ifr_tun, ifr_if;
	struct tuninfo ti;
	struct sockaddr_in addr;

	/* set up tunnel device */
	memset(&ifr_tun, 0, sizeof(ifr_tun));
	memset(&ifr_if, 0, sizeof(ifr_if));
	memset(&ti, 0, sizeof(ti));

	if ((*fd = open_tun_any()) < 0) {
		perror("Could not open tun device");
		return -1;
	}

	if (ioctl(*fd, TUNGIFINFO, &ti) < 0) {
		perror("TUNGIFINFO");
		del_dev_tun(*fd);
		return -1;
	}

	/* set ip of this end point of tunnel */
	memset(&addr, 0, sizeof(addr));
	addr.sin_addr.s_addr = tun_addr;
	addr.sin_family = AF_INET;
	memcpy(&ifr_tun.ifr_addr, &addr, sizeof(struct sockaddr));

	if (ioctl(*fd, SIOCSIFADDR, &ifr_tun) < 0) {
		perror("SIOCSIFADDR");
		del_dev_tun(*fd);
		return -1;
	}

	if (ioctl(*fd, SIOCGIFFLAGS, &ifr_tun) < 0) {
		perror("SIOCGIFFLAGS");
		del_dev_tun(*fd);
		return -1;
	}

	ifr_tun.ifr_flags |= IFF_UP;
	ifr_tun.ifr_flags |= IFF_RUNNING;

	if (ioctl(*fd, SIOCSIFFLAGS, &ifr_tun) < 0) {
		perror("SIOCSIFFLAGS");
		del_dev_tun(*fd);
		return -1;
	}

	/* get MTU from real interface */
	strncpy(ifr_if.ifr_name, batman_if->dev, IFNAMSIZ - 1);
	if (ioctl(*fd, SIOCGIFMTU, &ifr_if) < 0) {
		perror("SIOCGIFMTU");
		del_dev_tun(*fd);
		return -1;
	}

	/* set MTU of tun interface: real MTU - 28 */
	if (ifr_if.ifr_mtu < 100) {
		fprintf(stderr, "Warning: MTU smaller than 100 - cannot reduce MTU anymore\n" );
	} else {
		ti.mtu = ifr_if.ifr_mtu - 28;
		if (ioctl(*fd, TUNSIFINFO, &ti) < 0) {
			perror("TUNSIFINFO");
			del_dev_tun(*fd);
			return -1;
		}
	}

	strncpy(tun_dev, ifr_tun.ifr_name, IFNAMSIZ - 1);
	return 1;
}
