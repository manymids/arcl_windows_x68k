#ifndef ARCL_OPM_H
#define ARCL_OPM_H

#include <stddef.h>
#include <stdint.h>

/* Included from both windows/arcl_l3.c (this project's x68k_opm tool) and,
 * conditionally under ARCL_OPM_SHADOW, from the core's
 * fmgen/fmg_wrap.cpp (x68k_mcp.md 0.1.1 - same minimal-diff/#ifdef
 * constraints as windows/arcl_watchpoint.h). Kept dependency-free so it
 * never risks the <windows.h> / libretro/compiler.h conflict
 * (x68k_mcp.md 3.4). */

#ifdef __cplusplus
extern "C" {
#endif

/* Called from MyOPM::WriteIO() (fmg_wrap.cpp) on every register write -
 * the only place "what was last written" can be captured, since FM::OPM
 * is a write-only hardware model with no register readback API. reg is
 * the YM2151 register number (0-255) latched by the preceding address
 * write; value is the data byte. */
void arcl_opm_shadow_on_write(int reg, uint8_t value);

void arcl_opm_shadow_init(void);
void arcl_opm_shadow_shutdown(void);

/* Fills out_regs[256] with the last-written value of every register
 * (0 for registers never written since boot - see x68k_mcp.md 6.4's
 * documented caveat that this shadow isn't part of the save state).
 * out_key_on[8] gets each channel's current 4-bit slot key-on mask (bits
 * 0-3 = slot 1-4 enabled), tracked separately because register 0x08 is a
 * "set this channel's mask" command, not a per-channel readable slot in
 * the 0-255 register space. */
void arcl_opm_shadow_get(uint8_t out_regs[256], uint8_t out_key_on[8]);

/* Frame-correlation for the state arcl_opm_shadow_get() above returns.
 * Without this, a caller that runs several frames between polls has no
 * way to tell "this channel's key-on mask is what it is because a sound
 * effect fired mid-run" apart from "this is stale state left over from
 * long ago" - both look identical as a bare register snapshot. */

/* Registered via arcl_control_set_frame_hint_sink() (arcl_control.h) from
 * arcl_l3_init() so every arcl_opm_shadow_on_write() call after this point
 * knows which frame is currently executing, without this file depending on
 * arcl_control.h (same decoupling arcl_control_set_console_matcher() uses
 * for L1). userdata is unused; the signature matches arcl_frame_hint_fn. */
void arcl_opm_shadow_on_frame_hint(void *userdata, unsigned long long next_frame);

/* Per-channel frame number of the most recent register 0x08 (key on/off)
 * write that touched that channel, or 0 if none since boot/last shadow
 * reset. */
void arcl_opm_shadow_get_key_on_frames(unsigned long long out_frames[8]);

#define ARCL_OPM_EVENT_LOG_MAX 32

typedef struct {
    unsigned long long frame;
    uint8_t channel;
    uint8_t key_on_mask; /* bits0-3 = slot1-4 enabled by this write; 0 = key-off */
} arcl_opm_event_t;

/* Copies up to ARCL_OPM_EVENT_LOG_MAX most recent register-0x08 (key
 * on/off) edges into out, oldest first, and returns how many were copied.
 * A short-lived sound effect's key-on (and its later key-off) can happen
 * and be overwritten by other OPM traffic entirely within the frames an
 * arcl_run call advances; polling only the *current* state via
 * arcl_opm_shadow_get()/out_key_on above would miss it. This log is what
 * lets x68k_opm report "these key-on/off edges happened, on these frames"
 * even when they're no longer the live state by the time the tool is
 * called. */
size_t arcl_opm_shadow_get_key_on_events(arcl_opm_event_t out[ARCL_OPM_EVENT_LOG_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* ARCL_OPM_H */
