#ifndef MEMBERSHIP_H_
#define MEMBERSHIP_H_

#include <stdbool.h>
#include <stdint.h>

#define RB_MEMBERSHIP_MAX_PEERS 3u

enum rb_peer_state {
	RB_PEER_FREE,
	RB_PEER_RESERVED,
	RB_PEER_ACTIVE,
	RB_PEER_SUSPECT,
};

struct rb_membership_peer {
	enum rb_peer_state state;
	uint64_t device_id;
	uint8_t node_id;
	uint16_t lease_id;
	uint64_t reserved_since_us;
	uint64_t last_success_us;
	uint64_t suspect_since_us;
};

struct rb_membership {
	uint32_t master_session;
	uint32_t lease_timeout_us;
	struct rb_membership_peer peers[RB_MEMBERSHIP_MAX_PEERS];
	uint8_t active_count;
};

struct rb_slave_lease {
	bool active;
	uint32_t master_session;
	uint8_t node_id;
	uint16_t lease_id;
	uint32_t timeout_us;
	uint64_t last_poll_us;
};

void rb_membership_init(struct rb_membership *membership,
			uint32_t master_session, uint32_t lease_timeout_us);
int rb_membership_begin_assign(struct rb_membership *membership,
			       uint64_t device_id, uint64_t now_us,
			       uint8_t node_id, uint16_t lease_id);
int rb_membership_assign_acked(struct rb_membership *membership,
			       uint8_t node_id, uint64_t now_us);
int rb_membership_abort_assign(struct rb_membership *membership,
			       uint8_t node_id);
int rb_membership_note_failure(struct rb_membership *membership,
			       uint8_t node_id, uint64_t now_us);
int rb_membership_note_success(struct rb_membership *membership,
			       uint8_t node_id, uint64_t now_us);
void rb_membership_tick(struct rb_membership *membership, uint64_t now_us);
const struct rb_membership_peer *rb_membership_peer(
	const struct rb_membership *membership, uint8_t node_id);
uint8_t rb_membership_active_count(const struct rb_membership *membership);
uint8_t rb_membership_suspect_count(const struct rb_membership *membership);
uint8_t rb_membership_free_slots(const struct rb_membership *membership);

void rb_slave_lease_init(struct rb_slave_lease *lease, uint32_t timeout_us);
int rb_slave_lease_assign(struct rb_slave_lease *lease, uint32_t master_session,
			  uint8_t node_id, uint16_t lease_id, uint64_t now_us);
int rb_slave_lease_note_poll(struct rb_slave_lease *lease,
			     uint32_t master_session, uint8_t node_id,
			     uint16_t lease_id, uint64_t now_us);
bool rb_slave_lease_tick(struct rb_slave_lease *lease, uint64_t now_us);
bool rb_slave_lease_active(const struct rb_slave_lease *lease);

#endif /* MEMBERSHIP_H_ */
