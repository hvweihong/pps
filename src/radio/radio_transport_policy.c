#include "radio_transport_policy.h"

#include <errno.h>
#include <stddef.h>

#include <zephyr/sys/util.h>

int rb_radio_pipe_mask(bool master, uint8_t node_id, uint8_t *mask)
{
	if (mask == NULL || (master && node_id != 0u) ||
	    (!master && (node_id == 0u || node_id > 3u))) {
		return -EINVAL;
	}
	*mask = master ? BIT_MASK(4) : BIT(0) | BIT(node_id);
	return 0;
}
