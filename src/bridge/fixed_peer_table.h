#ifndef FIXED_PEER_TABLE_H_
#define FIXED_PEER_TABLE_H_

#include <stdbool.h>
#include <stdint.h>

#define RB_FIXED_PEER_COUNT 3u

enum rb_fixed_peer_event {
	RB_FIXED_PEER_INVALID = -1,
	RB_FIXED_PEER_REFRESHED = 0,
	RB_FIXED_PEER_ACTIVATED = 1,
	RB_FIXED_PEER_SESSION_CHANGED = 2,
};

struct rb_fixed_peer {
	bool active;
	uint32_t slave_session;
	uint64_t last_ack_us;
};

struct rb_fixed_peer_table {
	uint32_t lease_timeout_us;
	struct rb_fixed_peer peers[RB_FIXED_PEER_COUNT];
};

void rb_fixed_peer_table_init(struct rb_fixed_peer_table *table,
			      uint32_t lease_timeout_us);
enum rb_fixed_peer_event rb_fixed_peer_note_ack(
	struct rb_fixed_peer_table *table, uint8_t node_id,
	uint32_t slave_session, uint64_t now_us);
bool rb_fixed_peer_expire(struct rb_fixed_peer_table *table, uint64_t now_us);
const struct rb_fixed_peer *rb_fixed_peer_get(
	const struct rb_fixed_peer_table *table, uint8_t node_id);
uint8_t rb_fixed_peer_active_count(const struct rb_fixed_peer_table *table);
uint8_t rb_fixed_peer_active_mask(const struct rb_fixed_peer_table *table);

#endif /* FIXED_PEER_TABLE_H_ */
