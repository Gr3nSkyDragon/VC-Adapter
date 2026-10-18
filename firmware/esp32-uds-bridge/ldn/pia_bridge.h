#pragma once
#include "ldn_keys.h"

/* Standalone bridge: finds the Switch's room, joins it, and relays between the
   GB-Link adapter and the Pia session entirely on-device. */

void pia_bridge_start(void);
void pia_bridge_stop(void);
bool pia_bridge_running(void);
/* Joined to a room and relaying. */
bool pia_bridge_in_session(void);
void pia_bridge_poll(void);
void pia_bridge_room(const ldn_network_t *net);
void pia_bridge_status(char *out, size_t cap);
