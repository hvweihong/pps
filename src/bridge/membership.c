#include "membership.h"

#include <errno.h>
#include <stddef.h>

static bool node_id_valid(uint8_t node_id)
{
	return node_id >= 1 && node_id <= RB_MEMBERSHIP_MAX_PEERS;
}

static struct rb_membership_peer *peer_mut(struct rb_membership *membership,
					   uint8_t node_id)
{
	if (membership == NULL || !node_id_valid(node_id)) {
		return NULL;
	}
	return &membership->peers[node_id - 1];
}

static void peer_release(struct rb_membership *membership,
			 struct rb_membership_peer *peer)
{
	if (peer->state == RB_PEER_ACTIVE || peer->state == RB_PEER_SUSPECT) {
		membership->active_count--;
	}
	*peer = (struct rb_membership_peer){
		.state = RB_PEER_FREE,
	};
}

void rb_membership_init(struct rb_membership *membership,
			uint32_t master_session, uint32_t lease_timeout_us)
{
	if (membership == NULL) {
		return;
	}
	membership->master_session = master_session;
	membership->lease_timeout_us = lease_timeout_us;
	membership->active_count = 0;
	for (uint8_t i = 0; i < RB_MEMBERSHIP_MAX_PEERS; i++) {
		membership->peers[i] = (struct rb_membership_peer){
			.state = RB_PEER_FREE,
		};
	}
}

static struct rb_membership_peer *peer_by_device(
	struct rb_membership *membership, uint64_t device_id)
{
	for (uint8_t i = 0; i < RB_MEMBERSHIP_MAX_PEERS; i++) {
		if (membership->peers[i].state != RB_PEER_FREE &&
		    membership->peers[i].device_id == device_id) {
			return &membership->peers[i];
		}
	}
	return NULL;
}

int rb_membership_begin_assign(struct rb_membership *membership,
			       uint64_t device_id, uint64_t now_us,
			       uint8_t node_id, uint16_t lease_id)
{
	struct rb_membership_peer *peer;

	if (membership == NULL || membership->master_session == 0 ||
	    membership->lease_timeout_us == 0 || device_id == 0 ||
	    !node_id_valid(node_id) || lease_id == 0) {
		return -EINVAL;
	}
	peer = peer_by_device(membership, device_id);
	if (peer != NULL) {
		return peer->node_id;
	}
	if (membership->active_count >= RB_MEMBERSHIP_MAX_PEERS) {
		return -ENOSPC;
	}
	peer = peer_mut(membership, node_id);
	if (peer->state != RB_PEER_FREE) {
		return -EBUSY;
	}
	*peer = (struct rb_membership_peer){
		.state = RB_PEER_RESERVED,
		.device_id = device_id,
		.node_id = node_id,
		.lease_id = lease_id,
		.reserved_since_us = now_us,
	};
	return node_id;
}

int rb_membership_assign_acked(struct rb_membership *membership,
			       uint8_t node_id, uint64_t now_us)
{
	struct rb_membership_peer *peer = peer_mut(membership, node_id);

	if (peer == NULL || peer->state != RB_PEER_RESERVED) {
		return -EINVAL;
	}
	peer->state = RB_PEER_ACTIVE;
	peer->last_success_us = now_us;
	peer->suspect_since_us = 0;
	membership->active_count++;
	return 0;
}

int rb_membership_abort_assign(struct rb_membership *membership,
			       uint8_t node_id)
{
	struct rb_membership_peer *peer = peer_mut(membership, node_id);

	if (peer == NULL || peer->state != RB_PEER_RESERVED) {
		return -EINVAL;
	}
	peer_release(membership, peer);
	return 0;
}

int rb_membership_note_failure(struct rb_membership *membership,
			       uint8_t node_id, uint64_t now_us)
{
	struct rb_membership_peer *peer = peer_mut(membership, node_id);

	if (peer == NULL || (peer->state != RB_PEER_ACTIVE &&
			     peer->state != RB_PEER_SUSPECT)) {
		return -EINVAL;
	}
	if (peer->state == RB_PEER_ACTIVE) {
		peer->state = RB_PEER_SUSPECT;
		peer->suspect_since_us = now_us;
	}
	return 0;
}

int rb_membership_note_success(struct rb_membership *membership,
			       uint8_t node_id, uint64_t now_us)
{
	struct rb_membership_peer *peer = peer_mut(membership, node_id);

	if (peer == NULL || (peer->state != RB_PEER_ACTIVE &&
			     peer->state != RB_PEER_SUSPECT)) {
		return -EINVAL;
	}
	peer->state = RB_PEER_ACTIVE;
	peer->last_success_us = now_us;
	peer->suspect_since_us = 0;
	return 0;
}

void rb_membership_tick(struct rb_membership *membership, uint64_t now_us)
{
	if (membership == NULL || membership->lease_timeout_us == 0) {
		return;
	}
	for (uint8_t i = 0; i < RB_MEMBERSHIP_MAX_PEERS; i++) {
		struct rb_membership_peer *peer = &membership->peers[i];
		uint64_t reference;

		switch (peer->state) {
		case RB_PEER_RESERVED:
			reference = peer->reserved_since_us;
			break;
		case RB_PEER_ACTIVE:
			reference = peer->last_success_us;
			break;
		case RB_PEER_SUSPECT:
			reference = peer->suspect_since_us;
			break;
		case RB_PEER_FREE:
		default:
			continue;
		}
		/* Synthetic/native tests may feed timestamps out of order.  Treat a
		 * backwards clock observation as no elapsed time rather than allowing
		 * unsigned subtraction to wrap and expire the lease immediately. */
		if (now_us < reference) {
			continue;
		}
		if (now_us - reference >= membership->lease_timeout_us) {
			peer_release(membership, peer);
		}
	}
}

const struct rb_membership_peer *rb_membership_peer(
	const struct rb_membership *membership, uint8_t node_id)
{
	if (membership == NULL || !node_id_valid(node_id)) {
		return NULL;
	}
	return &membership->peers[node_id - 1];
}

uint8_t rb_membership_active_count(const struct rb_membership *membership)
{
	return membership == NULL ? 0 : membership->active_count;
}

uint8_t rb_membership_suspect_count(const struct rb_membership *membership)
{
	uint8_t count = 0;

	if (membership == NULL) {
		return 0;
	}
	for (uint8_t i = 0; i < RB_MEMBERSHIP_MAX_PEERS; i++) {
		if (membership->peers[i].state == RB_PEER_SUSPECT) {
			count++;
		}
	}
	return count;
}

uint8_t rb_membership_free_slots(const struct rb_membership *membership)
{
	uint8_t count = 0;

	if (membership == NULL) {
		return 0;
	}
	for (uint8_t i = 0; i < RB_MEMBERSHIP_MAX_PEERS; i++) {
		if (membership->peers[i].state == RB_PEER_FREE) {
			count++;
		}
	}
	return count;
}

void rb_slave_lease_init(struct rb_slave_lease *lease, uint32_t timeout_us)
{
	if (lease == NULL) {
		return;
	}
	*lease = (struct rb_slave_lease){
		.timeout_us = timeout_us,
	};
}

int rb_slave_lease_assign(struct rb_slave_lease *lease, uint32_t master_session,
			  uint8_t node_id, uint16_t lease_id, uint64_t now_us)
{
	if (lease == NULL || lease->timeout_us == 0 || master_session == 0 ||
	    !node_id_valid(node_id) || lease_id == 0) {
		return -EINVAL;
	}
	lease->active = true;
	lease->master_session = master_session;
	lease->node_id = node_id;
	lease->lease_id = lease_id;
	lease->last_poll_us = now_us;
	return 0;
}

int rb_slave_lease_note_poll(struct rb_slave_lease *lease,
			     uint32_t master_session, uint8_t node_id,
			     uint16_t lease_id, uint64_t now_us)
{
	if (lease == NULL || !lease->active) {
		return -EINVAL;
	}
	if (lease->master_session != master_session || lease->node_id != node_id ||
	    lease->lease_id != lease_id) {
		return -ESTALE;
	}
	lease->last_poll_us = now_us;
	return 0;
}

bool rb_slave_lease_tick(struct rb_slave_lease *lease, uint64_t now_us)
{
	if (lease == NULL || !lease->active || lease->timeout_us == 0 ||
	    now_us < lease->last_poll_us ||
	    now_us - lease->last_poll_us < lease->timeout_us) {
		return false;
	}
	lease->active = false;
	lease->master_session = 0;
	lease->node_id = 0;
	lease->lease_id = 0;
	return true;
}

bool rb_slave_lease_active(const struct rb_slave_lease *lease)
{
	return lease != NULL && lease->active;
}
