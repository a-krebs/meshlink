/*
    net_packet.c -- Handles in- and outgoing VPN packets
    Copyright (C) 2014 Guus Sliepen <guus@meshlink.io>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "system.h"

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#include "cipher.h"
#include "conf.h"
#include "connection.h"
#include "crypto.h"
#include "digest.h"
#include "graph.h"
#include "logger.h"
#include "net.h"
#include "netutl.h"
#include "protocol.h"
#include "route.h"
#include "utils.h"
#include "xalloc.h"

int keylifetime = 0;

static void send_udppacket(node_t *, vpn_packet_t *);

unsigned replaywin = 16;
bool localdiscovery = false;
sockaddr_t localdiscovery_address;

#define MAX_SEQNO 1073741824

/* mtuprobes == 1..30: initial discovery, send bursts with 1 second interval
   mtuprobes ==    31: sleep pinginterval seconds
   mtuprobes ==    32: send 1 burst, sleep pingtimeout second
   mtuprobes ==    33: no response from other side, restart PMTU discovery process

   Probes are sent in batches of at least three, with random sizes between the
   lower and upper boundaries for the MTU thus far discovered.

   After the initial discovery, a fourth packet is added to each batch with a
   size larger than the currently known PMTU, to test if the PMTU has increased.

   In case local discovery is enabled, another packet is added to each batch,
   which will be broadcast to the local network.

*/

static void send_mtu_probe_handler(void *data) {
	node_t *n = data;
	int timeout = 1;

	n->mtuprobes++;

	if(!n->status.reachable || !n->status.validkey) {
		logger(DEBUG_TRAFFIC, LOG_INFO, "Trying to send MTU probe to unreachable or rekeying node %s (%s)", n->name, n->hostname);
		n->mtuprobes = 0;
		return;
	}

	if(n->mtuprobes > 32) {
		if(!n->minmtu) {
			n->mtuprobes = 31;
			timeout = pinginterval;
			goto end;
		}

		logger(DEBUG_TRAFFIC, LOG_INFO, "%s (%s) did not respond to UDP ping, restarting PMTU discovery", n->name, n->hostname);
		n->status.udp_confirmed = false;
		n->mtuprobes = 1;
		n->minmtu = 0;
		n->maxmtu = MTU;
	}

	if(n->mtuprobes >= 10 && n->mtuprobes < 32 && !n->minmtu) {
		logger(DEBUG_TRAFFIC, LOG_INFO, "No response to MTU probes from %s (%s)", n->name, n->hostname);
		n->mtuprobes = 31;
	}

	if(n->mtuprobes == 30 || (n->mtuprobes < 30 && n->minmtu >= n->maxmtu)) {
		if(n->minmtu > n->maxmtu)
			n->minmtu = n->maxmtu;
		else
			n->maxmtu = n->minmtu;
		n->mtu = n->minmtu;
		logger(DEBUG_TRAFFIC, LOG_INFO, "Fixing MTU of %s (%s) to %d after %d probes", n->name, n->hostname, n->mtu, n->mtuprobes);
		n->mtuprobes = 31;
	}

	if(n->mtuprobes == 31) {
		timeout = pinginterval;
		goto end;
	} else if(n->mtuprobes == 32) {
		timeout = pingtimeout;
	}

	for(int i = 0; i < 4 + localdiscovery; i++) {
		int len;

		if(i == 0) {
			if(n->mtuprobes < 30 || n->maxmtu + 8 >= MTU)
				continue;
			len = n->maxmtu + 8;
		} else if(n->maxmtu <= n->minmtu) {
			len = n->maxmtu;
		} else {
			len = n->minmtu + 1 + rand() % (n->maxmtu - n->minmtu);
		}

		if(len < 64)
			len = 64;

		vpn_packet_t packet;
		memset(packet.data, 0, 14);
		randomize(packet.data + 14, len - 14);
		packet.len = len;
		packet.priority = 0;
		n->status.broadcast = i >= 4 && n->mtuprobes <= 10 && n->prevedge;

		logger(DEBUG_TRAFFIC, LOG_INFO, "Sending MTU probe length %d to %s (%s)", len, n->name, n->hostname);

		send_udppacket(n, &packet);
	}

	n->status.broadcast = false;
	n->probe_counter = 0;
	gettimeofday(&n->probe_time, NULL);

	/* Calculate the packet loss of incoming traffic by comparing the rate of
	   packets received to the rate with which the sequence number has increased.
	 */

	if(n->received > n->prev_received)
		n->packetloss = 1.0 - (n->received - n->prev_received) / (float)(n->received_seqno - n->prev_received_seqno);
	else
		n->packetloss = n->received_seqno <= n->prev_received_seqno;

	n->prev_received_seqno = n->received_seqno;
	n->prev_received = n->received;

end:
	timeout_set(&n->mtutimeout, &(struct timeval){timeout, rand() % 100000});
}

void send_mtu_probe(node_t *n) {
	timeout_add(&n->mtutimeout, send_mtu_probe_handler, n, &(struct timeval){1, 0});
	send_mtu_probe_handler(n);
}

static void mtu_probe_h(node_t *n, vpn_packet_t *packet, length_t len) {
	logger(DEBUG_TRAFFIC, LOG_INFO, "Got MTU probe length %d from %s (%s)", packet->len, n->name, n->hostname);

	if(!packet->data[0]) {
		/* It's a probe request, send back a reply */

		packet->data[0] = 1;

		/* Temporarily set udp_confirmed, so that the reply is sent
		   back exactly the way it came in. */

		bool udp_confirmed = n->status.udp_confirmed;
		n->status.udp_confirmed = true;
		send_udppacket(n, packet);
		n->status.udp_confirmed = udp_confirmed;
	} else {
		/* It's a valid reply: now we know bidirectional communication
		   is possible using the address and socket that the reply
		   packet used. */

		n->status.udp_confirmed = true;

		/* If we haven't established the PMTU yet, restart the discovery process. */

		if(n->mtuprobes > 30) {
			if (len == n->maxmtu + 8) {
				logger(DEBUG_TRAFFIC, LOG_INFO, "Increase in PMTU to %s (%s) detected, restarting PMTU discovery", n->name, n->hostname);
				n->maxmtu = MTU;
				n->mtuprobes = 10;
				return;
			}

			if(n->minmtu)
				n->mtuprobes = 30;
			else
				n->mtuprobes = 1;
		}

		/* If applicable, raise the minimum supported MTU */

		if(len > n->maxmtu)
			len = n->maxmtu;
		if(n->minmtu < len)
			n->minmtu = len;

		/* Calculate RTT and bandwidth.
		   The RTT is the time between the MTU probe burst was sent and the first
		   reply is received. The bandwidth is measured using the time between the
		   arrival of the first and third probe reply.
		 */

		struct timeval now, diff;
		gettimeofday(&now, NULL);
		timersub(&now, &n->probe_time, &diff);
		
		n->probe_counter++;

		if(n->probe_counter == 1) {
			n->rtt = diff.tv_sec + diff.tv_usec * 1e-6;
			n->probe_time = now;
		} else if(n->probe_counter == 3) {
			n->bandwidth = 2.0 * len / (diff.tv_sec + diff.tv_usec * 1e-6);
			logger(DEBUG_TRAFFIC, LOG_DEBUG, "%s (%s) RTT %.2f ms, burst bandwidth %.3f Mbit/s, rx packet loss %.2f %%", n->name, n->hostname, n->rtt * 1e3, n->bandwidth * 8e-6, n->packetloss * 1e2);
		}
	}
}

static length_t compress_packet(uint8_t *dest, const uint8_t *source, length_t len, int level) {
	if(level == 0) {
		memcpy(dest, source, len);
		return len;
	} else if(level == 10) {
		return -1;
	} else if(level < 10) {
#ifdef HAVE_ZLIB
		unsigned long destlen = MAXSIZE;
		if(compress2(dest, &destlen, source, len, level) == Z_OK)
			return destlen;
		else
#endif
			return -1;
	} else {
		return -1;
	}

	return -1;
}

static length_t uncompress_packet(uint8_t *dest, const uint8_t *source, length_t len, int level) {
	if(level == 0) {
		memcpy(dest, source, len);
		return len;
	} else if(level > 9) {
			return -1;
	}
#ifdef HAVE_ZLIB
	else {
		unsigned long destlen = MAXSIZE;
		if(uncompress(dest, &destlen, source, len) == Z_OK)
			return destlen;
		else
			return -1;
	}
#endif

	return -1;
}

/* VPN packet I/O */

static void receive_packet(node_t *n, vpn_packet_t *packet) {
	logger(DEBUG_TRAFFIC, LOG_DEBUG, "Received packet of %d bytes from %s (%s)",
			   packet->len, n->name, n->hostname);

	n->in_packets++;
	n->in_bytes += packet->len;

	route(n, packet);
}

static bool try_mac(node_t *n, const vpn_packet_t *inpkt) {
	return sptps_verify_datagram(&n->sptps, (char *)&inpkt->seqno, inpkt->len);
}

static void receive_udppacket(node_t *n, vpn_packet_t *inpkt) {
	vpn_packet_t pkt1, pkt2;
	vpn_packet_t *pkt[] = { &pkt1, &pkt2, &pkt1, &pkt2 };
	int nextpkt = 0;
	vpn_packet_t *outpkt = pkt[0];
	size_t outlen;

	if(!n->sptps.state) {
		if(!n->status.waitingforkey) {
			logger(DEBUG_TRAFFIC, LOG_DEBUG, "Got packet from %s (%s) but we haven't exchanged keys yet", n->name, n->hostname);
			send_req_key(n);
		} else {
			logger(DEBUG_TRAFFIC, LOG_DEBUG, "Got packet from %s (%s) but he hasn't got our key yet", n->name, n->hostname);
		}
		return;
	}
	sptps_receive_data(&n->sptps, (char *)&inpkt->seqno, inpkt->len);
}

void receive_tcppacket(connection_t *c, const char *buffer, int len) {
	vpn_packet_t outpkt;

	if(len > sizeof outpkt.data)
		return;

	outpkt.len = len;
	if(c->options & OPTION_TCPONLY)
		outpkt.priority = 0;
	else
		outpkt.priority = -1;
	memcpy(outpkt.data, buffer, len);

	receive_packet(c->node, &outpkt);
}

static void send_sptps_packet(node_t *n, vpn_packet_t *origpkt) {
	if(!n->status.validkey) {
		logger(DEBUG_TRAFFIC, LOG_INFO, "No valid key known yet for %s (%s)", n->name, n->hostname);
		if(!n->status.waitingforkey)
			send_req_key(n);
		else if(n->last_req_key + 10 < now.tv_sec) {
			logger(DEBUG_ALWAYS, LOG_DEBUG, "No key from %s after 10 seconds, restarting SPTPS", n->name);
			sptps_stop(&n->sptps);
			n->status.waitingforkey = false;
			send_req_key(n);
		}
		return;
	}

	uint8_t type = 0;
	int offset = 0;

	if(!(origpkt->data[12] | origpkt->data[13])) {
		sptps_send_record(&n->sptps, PKT_PROBE, (char *)origpkt->data, origpkt->len);
		return;
	}

	if(routing_mode == RMODE_ROUTER)
		offset = 14;
	else
		type = PKT_MAC;

	if(origpkt->len < offset)
		return;

	vpn_packet_t outpkt;

	if(n->outcompression) {
		int len = compress_packet(outpkt.data + offset, origpkt->data + offset, origpkt->len - offset, n->outcompression);
		if(len < 0) {
			logger(DEBUG_TRAFFIC, LOG_ERR, "Error while compressing packet to %s (%s)", n->name, n->hostname);
		} else if(len < origpkt->len - offset) {
			outpkt.len = len + offset;
			origpkt = &outpkt;
			type |= PKT_COMPRESSED;
		}
	}

	sptps_send_record(&n->sptps, type, (char *)origpkt->data + offset, origpkt->len - offset);
	return;
}

static void choose_udp_address(const node_t *n, const sockaddr_t **sa, int *sock) {
	/* Latest guess */
	*sa = &n->address;
	*sock = n->sock;

	/* If the UDP address is confirmed, use it. */
	if(n->status.udp_confirmed)
		return;

	/* Send every third packet to n->address; that could be set
	   to the node's reflexive UDP address discovered during key
	   exchange. */

	static int x = 0;
	if(++x >= 3) {
		x = 0;
		return;
	}

	/* Otherwise, address are found in edges to this node.
	   So we pick a random edge and a random socket. */

	int i = 0;
	int j = rand() % n->edge_tree->count;
	edge_t *candidate = NULL;

	for splay_each(edge_t, e, n->edge_tree) {
		if(i++ == j) {
			candidate = e->reverse;
			break;
		}
	}

	if(candidate) {
		*sa = &candidate->address;
		*sock = rand() % listen_sockets;
	}

	/* Make sure we have a suitable socket for the chosen address */
	if(listen_socket[*sock].sa.sa.sa_family != (*sa)->sa.sa_family) {
		for(int i = 0; i < listen_sockets; i++) {
			if(listen_socket[i].sa.sa.sa_family == (*sa)->sa.sa_family) {
				*sock = i;
				break;
			}
		}
	}
}

static void choose_broadcast_address(const node_t *n, const sockaddr_t **sa, int *sock) {
	static sockaddr_t broadcast_ipv4 = {
		.in = {
			.sin_family = AF_INET,
			.sin_addr.s_addr = -1,
		}
	};

	static sockaddr_t broadcast_ipv6 = {
		.in6 = {
			.sin6_family = AF_INET6,
			.sin6_addr.s6_addr[0x0] = 0xff,
			.sin6_addr.s6_addr[0x1] = 0x02,
			.sin6_addr.s6_addr[0xf] = 0x01,
		}
	};

	*sock = rand() % listen_sockets;

	if(listen_socket[*sock].sa.sa.sa_family == AF_INET6) {
		if(localdiscovery_address.sa.sa_family == AF_INET6) {
			localdiscovery_address.in6.sin6_port = n->prevedge->address.in.sin_port;
			*sa = &localdiscovery_address;
		} else {
			broadcast_ipv6.in6.sin6_port = n->prevedge->address.in.sin_port;
			broadcast_ipv6.in6.sin6_scope_id = listen_socket[*sock].sa.in6.sin6_scope_id;
			*sa = &broadcast_ipv6;
		}
	} else {
		if(localdiscovery_address.sa.sa_family == AF_INET) {
			localdiscovery_address.in.sin_port = n->prevedge->address.in.sin_port;
			*sa = &localdiscovery_address;
		} else {
			broadcast_ipv4.in.sin_port = n->prevedge->address.in.sin_port;
			*sa = &broadcast_ipv4;
		}
	}
}

static void send_udppacket(node_t *n, vpn_packet_t *origpkt) {
	vpn_packet_t pkt1, pkt2;
	vpn_packet_t *pkt[] = { &pkt1, &pkt2, &pkt1, &pkt2 };
	vpn_packet_t *inpkt = origpkt;
	int nextpkt = 0;
	vpn_packet_t *outpkt;
	int origlen = origpkt->len;
	size_t outlen;
#if defined(SOL_IP) && defined(IP_TOS)
	static int priority = 0;
#endif
	int origpriority = origpkt->priority;

	if(!n->status.reachable) {
		logger(DEBUG_TRAFFIC, LOG_INFO, "Trying to send UDP packet to unreachable node %s (%s)", n->name, n->hostname);
		return;
	}

	return send_sptps_packet(n, origpkt);
}

bool send_sptps_data(void *handle, uint8_t type, const char *data, size_t len) {
	node_t *to = handle;

	/* Send it via TCP if it is a handshake packet, TCPOnly is in use, or this packet is larger than the MTU. */

	if(type >= SPTPS_HANDSHAKE || ((myself->options | to->options) & OPTION_TCPONLY) || (type != PKT_PROBE && len > to->minmtu)) {
		char buf[len * 4 / 3 + 5];
		b64encode(data, buf, len);
		/* If no valid key is known yet, send the packets using ANS_KEY requests,
		   to ensure we get to learn the reflexive UDP address. */
		if(!to->status.validkey) {
			to->incompression = myself->incompression;
			return send_request(to->nexthop->connection, "%d %s %s %s -1 -1 -1 %d", ANS_KEY, myself->name, to->name, buf, to->incompression);
		} else {
			return send_request(to->nexthop->connection, "%d %s %s %d %s", REQ_KEY, myself->name, to->name, REQ_SPTPS, buf);
		}
	}

	/* Otherwise, send the packet via UDP */

	const sockaddr_t *sa;
	int sock;

	if(to->status.broadcast)
		choose_broadcast_address(to, &sa, &sock);
	else
		choose_udp_address(to, &sa, &sock);

	if(sendto(listen_socket[sock].udp.fd, data, len, 0, &sa->sa, SALEN(sa->sa)) < 0 && !sockwouldblock(sockerrno)) {
		if(sockmsgsize(sockerrno)) {
			if(to->maxmtu >= len)
				to->maxmtu = len - 1;
			if(to->mtu >= len)
				to->mtu = len - 1;
		} else {
			logger(DEBUG_TRAFFIC, LOG_WARNING, "Error sending UDP SPTPS packet to %s (%s): %s", to->name, to->hostname, sockstrerror(sockerrno));
			return false;
		}
	}

	return true;
}

bool receive_sptps_record(void *handle, uint8_t type, const char *data, uint16_t len) {
	node_t *from = handle;

	if(type == SPTPS_HANDSHAKE) {
		if(!from->status.validkey) {
			from->status.validkey = true;
			from->status.waitingforkey = false;
			logger(DEBUG_META, LOG_INFO, "SPTPS key exchange with %s (%s) succesful", from->name, from->hostname);
		}
		return true;
	}

	if(len > MTU) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Packet from %s (%s) larger than maximum supported size (%d > %d)", from->name, from->hostname, len, MTU);
		return false;
	}

	vpn_packet_t inpkt;

	if(type == PKT_PROBE) {
		inpkt.len = len;
		memcpy(inpkt.data, data, len);
		mtu_probe_h(from, &inpkt, len);
		return true;
	}

	if(type & ~(PKT_COMPRESSED | PKT_MAC)) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Unexpected SPTPS record type %d len %d from %s (%s)", type, len, from->name, from->hostname);
		return false;
	}

	/* Check if we have the headers we need */
	if(routing_mode != RMODE_ROUTER && !(type & PKT_MAC)) {
		logger(DEBUG_TRAFFIC, LOG_ERR, "Received packet from %s (%s) without MAC header (maybe Mode is not set correctly)", from->name, from->hostname);
		return false;
	} else if(routing_mode == RMODE_ROUTER && (type & PKT_MAC)) {
		logger(DEBUG_TRAFFIC, LOG_WARNING, "Received packet from %s (%s) with MAC header (maybe Mode is not set correctly)", from->name, from->hostname);
	}

	int offset = (type & PKT_MAC) ? 0 : 14;
	if(type & PKT_COMPRESSED) {
		length_t ulen = uncompress_packet(inpkt.data + offset, (const uint8_t *)data, len, from->incompression);
		if(ulen < 0) {
			return false;
		} else {
			inpkt.len = ulen + offset;
		}
		if(inpkt.len > MAXSIZE)
			abort();
	} else {
		memcpy(inpkt.data + offset, data, len);
		inpkt.len = len + offset;
	}

	/* Generate the Ethernet packet type if necessary */
	if(offset) {
		switch(inpkt.data[14] >> 4) {
			case 4:
				inpkt.data[12] = 0x08;
				inpkt.data[13] = 0x00;
				break;
			case 6:
				inpkt.data[12] = 0x86;
				inpkt.data[13] = 0xDD;
				break;
			default:
				logger(DEBUG_TRAFFIC, LOG_ERR,
						   "Unknown IP version %d while reading packet from %s (%s)",
						   inpkt.data[14] >> 4, from->name, from->hostname);
				return false;
		}
	}

	receive_packet(from, &inpkt);
	return true;
}

/*
  send a packet to the given vpn ip.
*/
void send_packet(node_t *n, vpn_packet_t *packet) {
	node_t *via;

	if(n == myself) {
		n->out_packets++;
		n->out_bytes += packet->len;
		// TODO: send to application
		return;
	}

	logger(DEBUG_TRAFFIC, LOG_ERR, "Sending packet of %d bytes to %s (%s)",
			   packet->len, n->name, n->hostname);

	if(!n->status.reachable) {
		logger(DEBUG_TRAFFIC, LOG_INFO, "Node %s (%s) is not reachable",
				   n->name, n->hostname);
		return;
	}

	n->out_packets++;
	n->out_bytes += packet->len;

	send_sptps_packet(n, packet);
	return;
}

/* Broadcast a packet using the minimum spanning tree */

void broadcast_packet(const node_t *from, vpn_packet_t *packet) {
	// Always give ourself a copy of the packet.
	if(from != myself)
		send_packet(myself, packet);

	logger(DEBUG_TRAFFIC, LOG_INFO, "Broadcasting packet of %d bytes from %s (%s)",
			   packet->len, from->name, from->hostname);

	switch(broadcast_mode) {
		// In MST mode, broadcast packets travel via the Minimum Spanning Tree.
		// This guarantees all nodes receive the broadcast packet, and
		// usually distributes the sending of broadcast packets over all nodes.
		case BMODE_MST:
			for list_each(connection_t, c, connection_list)
				if(c->status.active && c->status.mst && c != from->nexthop->connection)
					send_packet(c->node, packet);
			break;

		// In direct mode, we send copies to each node we know of.
		// However, this only reaches nodes that can be reached in a single hop.
		// We don't have enough information to forward broadcast packets in this case.
		case BMODE_DIRECT:
			if(from != myself)
				break;

			for splay_each(node_t, n, node_tree)
				if(n->status.reachable && n != myself && ((n->via == myself && n->nexthop == n) || n->via == n))
					send_packet(n, packet);
			break;

		default:
			break;
	}
}

static node_t *try_harder(const sockaddr_t *from, const vpn_packet_t *pkt) {
	node_t *n = NULL;
	bool hard = false;
	static time_t last_hard_try = 0;

	for splay_each(edge_t, e, edge_weight_tree) {
		if(!e->to->status.reachable || e->to == myself)
			continue;

		if(sockaddrcmp_noport(from, &e->address)) {
			if(last_hard_try == now.tv_sec)
				continue;
			hard = true;
		}

		if(!try_mac(e->to, pkt))
			continue;

		n = e->to;
		break;
	}

	if(hard)
		last_hard_try = now.tv_sec;

	last_hard_try = now.tv_sec;
	return n;
}

void handle_incoming_vpn_data(void *data, int flags) {
	listen_socket_t *ls = data;
	vpn_packet_t pkt;
	char *hostname;
	sockaddr_t from = {{0}};
	socklen_t fromlen = sizeof from;
	node_t *n;
	int len;

	len = recvfrom(ls->udp.fd, (char *) &pkt.seqno, MAXSIZE, 0, &from.sa, &fromlen);

	if(len <= 0 || len > MAXSIZE) {
		if(!sockwouldblock(sockerrno))
			logger(DEBUG_ALWAYS, LOG_ERR, "Receiving packet failed: %s", sockstrerror(sockerrno));
		return;
	}

	pkt.len = len;

	sockaddrunmap(&from); /* Some braindead IPv6 implementations do stupid things. */

	n = lookup_node_udp(&from);

	if(!n) {
		n = try_harder(&from, &pkt);
		if(n)
			update_node_udp(n, &from);
		else if(debug_level >= DEBUG_PROTOCOL) {
			hostname = sockaddr2hostname(&from);
			logger(DEBUG_PROTOCOL, LOG_WARNING, "Received UDP packet from unknown source %s", hostname);
			free(hostname);
			return;
		}
		else
			return;
	}

	n->sock = ls - listen_socket;

	receive_udppacket(n, &pkt);
}
