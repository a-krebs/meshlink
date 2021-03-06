/*
    route.c -- routing
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
#include "xalloc.h"

#include "logger.h"
#include "meshlink_internal.h"
#include "net.h"
#include "route.h"
#include "utils.h"

bool decrement_ttl = false;

static bool checklength(node_t *source, vpn_packet_t *packet, uint16_t length) {
	if(packet->len < length) {
		logger(source->mesh, MESHLINK_WARNING, "Got too short packet from %s (%s)", source->name, source->hostname);
		return false;
	} else
		return true;
}

// @return the sockerrno, 0 on success, -1 on other errors
int route(meshlink_handle_t *mesh, node_t *source, vpn_packet_t *packet) {
	// TODO: route on name or key

	node_t *owner = NULL;
	node_t *via = NULL;
	meshlink_packethdr_t *hdr = (meshlink_packethdr_t *) packet->data;
	owner = lookup_node(mesh, (char *)hdr->destination);
	logger(mesh, MESHLINK_DEBUG, "Routing packet from \"%s\" to \"%s\"\n", hdr->source, hdr->destination);

	//Check Lenght
	if(!checklength(source, packet, sizeof *hdr))
		return -1;

	if(owner == NULL) {
		//Lookup failed
		logger(mesh, MESHLINK_WARNING, "Cant lookup the owner of a packet in the route() function. This should never happen!\n");
		logger(mesh, MESHLINK_WARNING, "Destination was: %s\n", hdr->destination);
		return -1;
	}

	if(owner == mesh->self) {
		const void *payload = packet->data + sizeof *hdr;
		size_t len = packet->len - sizeof *hdr;

		// check log level before calling bin2hex since that's an expensive call
		if(mesh->log_level <= MESHLINK_DEBUG_PACKETDATA) {
			char* hex = xzalloc(len * 2 + 1);
			bin2hex(payload, hex, len);
			logger(mesh, MESHLINK_DEBUG, "I received a packet for me with payload: %s\n", hex);
			free(hex);
		}

		if(mesh->receive_cb)
			mesh->receive_cb(mesh, (meshlink_node_t *)source, payload, len);
		return 0;
	}

	if(!owner->status.reachable) {
		//TODO: check what to do here, not just print a warning
		logger(mesh, MESHLINK_WARNING, "The owner of a packet in the route() function is unreachable. Dropping packet.\n");
		return -1;
	}

	via = (owner->via == mesh->self) ? owner->nexthop : owner->via;
	if(via == source) {
		logger(mesh, MESHLINK_ERROR, "Routing loop for packet from %s (%s)!", source->name, source->hostname);
		return -1;
	}

	return send_packet(mesh, owner, packet);
}
