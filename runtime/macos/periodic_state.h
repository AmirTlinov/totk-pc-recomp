// Presentation publication identities. These are not closure certificates or
// evidence that a temporal state matches a reference render.
#pragma once
#include <stdint.h>
uint64_t periodic_anchor_publish(uint64_t anchor_id);
uint64_t periodic_phase_publish(void);
uint64_t periodic_latest_anchor(void);
uint64_t periodic_anchor_generation(uint64_t anchor_id);
