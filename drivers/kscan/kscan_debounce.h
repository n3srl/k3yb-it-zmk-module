/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct k3yb_debounce {
    bool candidate;
    uint32_t since;
};

/* Both edges must remain unchanged for the whole debounce interval.
 * Ambiguous mux samples cancel confirmation rather than counting as an edge. */
static inline bool k3yb_debounce_update(struct k3yb_debounce *state, bool stable,
                                       bool sample, bool valid, uint32_t now,
                                       uint32_t interval) {
    if (!valid) {
        state->candidate = stable;
        state->since = now;
        return stable;
    }
    if (interval == 0) {
        state->candidate = sample;
        return sample;
    }
    if (sample != state->candidate) {
        state->candidate = sample;
        state->since = now;
    }
    if (sample != stable && (uint32_t)(now - state->since) >= interval) {
        return sample;
    }
    return stable;
}
