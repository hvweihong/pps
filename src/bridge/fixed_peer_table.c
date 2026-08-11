#include "fixed_peer_table.h"

#include <string.h>

static bool node_valid(uint8_t node_id)
{
	return node_id >= 1u && node_id <= RB_FIXED_PEER_COUNT;
}

void rb_fixed_peer_table_init(struct rb_fixed_peer_table *table,
			      uint32_t lease_timeout_us)
{
	if (table == NULL) {
		return;
	}
	memset(table, 0, sizeof(*table));
	table->lease_timeout_us = lease_timeout_us;
}

enum rb_fixed_peer_event rb_fixed_peer_note_ack(
	struct rb_fixed_peer_table *table, uint8_t node_id,
	uint32_t slave_session, uint64_t now_us)
{
	struct rb_fixed_peer *peer;
	enum rb_fixed_peer_event event;

	if (table == NULL || !node_valid(node_id) || slave_session == 0u) {
		return RB_FIXED_PEER_INVALID;
	}
	peer = &table->peers[node_id - 1u];
	if (!peer->active) {
		event = RB_FIXED_PEER_ACTIVATED;
	} else if (peer->slave_session != slave_session) {
		event = RB_FIXED_PEER_SESSION_CHANGED;
	} else {
		event = RB_FIXED_PEER_REFRESHED;
	}
	peer->active = true;
	peer->slave_session = slave_session;
	peer->last_ack_us = now_us;
	return event;
}

bool rb_fixed_peer_expire(struct rb_fixed_peer_table *table, uint64_t now_us)
{
	bool changed = false;

	if (table == NULL || table->lease_timeout_us == 0u) {
		return false;
	}
	for (size_t i = 0u; i < RB_FIXED_PEER_COUNT; i++) {
		struct rb_fixed_peer *peer = &table->peers[i];

		if (!peer->active || now_us < peer->last_ack_us ||
		    now_us - peer->last_ack_us < table->lease_timeout_us) {
			continue;
		}
		memset(peer, 0, sizeof(*peer));
		changed = true;
	}
	return changed;
}

const struct rb_fixed_peer *rb_fixed_peer_get(
	const struct rb_fixed_peer_table *table, uint8_t node_id)
{
	if (table == NULL || !node_valid(node_id)) {
		return NULL;
	}
	return &table->peers[node_id - 1u];
}

uint8_t rb_fixed_peer_active_count(const struct rb_fixed_peer_table *table)
{
	uint8_t count = 0u;

	if (table == NULL) {
		return 0u;
	}
	for (size_t i = 0u; i < RB_FIXED_PEER_COUNT; i++) {
		count += table->peers[i].active ? 1u : 0u;
	}
	return count;
}

uint8_t rb_fixed_peer_active_mask(const struct rb_fixed_peer_table *table)
{
	uint8_t mask = 0u;

	if (table == NULL) {
		return 0u;
	}
	for (size_t i = 0u; i < RB_FIXED_PEER_COUNT; i++) {
		if (table->peers[i].active) {
			mask |= (uint8_t)(1u << i);
		}
	}
	return mask;
}
