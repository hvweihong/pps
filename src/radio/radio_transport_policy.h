#ifndef RADIO_TRANSPORT_POLICY_H_
#define RADIO_TRANSPORT_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

int rb_radio_pipe_mask(bool master, uint8_t node_id, uint8_t *mask);

#endif /* RADIO_TRANSPORT_POLICY_H_ */
