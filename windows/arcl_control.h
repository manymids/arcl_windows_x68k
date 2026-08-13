#ifndef PX68K_ARCL_CONTROL_H
#define PX68K_ARCL_CONTROL_H

#include <stddef.h>

typedef struct arcl_control arcl_control_t;

int arcl_control_init(arcl_control_t **out_control);
void arcl_control_shutdown(arcl_control_t *control);
int arcl_control_call(const char *name, const char *request_json, void *userdata,
                      char *result_json, size_t result_size,
                      char *error_message, size_t error_size);

/* Shared execution path: stops any continuous execution, then advances by
 * `frames` under the control lock. Used by arcl_run and by other tool
 * modules that also advance frames (e.g. arcl_key's "tap" action), so
 * every frame-advancing tool goes through the exact same NoWaitMode/frame
 * counter bookkeeping. Returns the frame count reached. */
unsigned long long arcl_control_run_frames(arcl_control_t *control, long frames);

/* Read-only snapshot for tool modules that need to report frame/state in
 * their own response without reaching into arcl_control's internals.
 * running is 1 while arcl_resume's worker thread is active. */
void arcl_control_get_status(arcl_control_t *control, unsigned long long *frame, int *running);

/* For tool modules that restore a core-level state blob outside of
 * arcl_control's own numbered save slots (arcl_l4.c's named snapshots) and
 * need arcl_status's frame number to reflect the restored point afterward,
 * exactly like arcl_load_state does for its own slots. Caller must already
 * hold the machine paused (e.g. via arcl_control_run_frames(control, 0)). */
void arcl_control_set_frame(arcl_control_t *control, unsigned long long frame);

/* arcl_speed (L4, x68k_mcp.md 6.5/Phase4): real-time ratio of the current
 * (or, if paused, the just-ended) arcl_resume session. *out_running is 0
 * whenever nothing has been resumed yet, in which case *out_fps/*out_percent
 * are 0 rather than stale. */
void arcl_control_get_speed(arcl_control_t *control, double *out_fps, double *out_percent, int *out_running);

/* arcl_rewind (L4): owned here (not a separate module, unlike arcl_l4.c's
 * named snapshots) because the auto-capture hook has to live inside the
 * shared advance_frame() every frame-stepping tool in arcl_control.c
 * already funnels through - see arcl_control.c's struct arcl_control and
 * advance_frame() for why. action: enable/disable/status/rewind. */
int arcl_control_rewind_call(arcl_control_t *control, const char *request_json,
                              char *result_json, size_t result_size,
                              char *error_message, size_t error_size);

/* arcl_run's optional text_match stop condition (arcl_common_spec.md 4.7)
 * is an L1 concept (it reads the decoded console), but arcl_run itself is
 * owned here so every frame-advancing tool shares one execution path. L1
 * registers its matcher via this setter instead of arcl_control depending
 * on the L1 module's header. Return 1 from `fn` once the console contains
 * `text_match`; write the matching line (for the response's `stop_reason`
 * detail) into matched_line. */
typedef int (*arcl_console_match_fn)(void *userdata, const char *text_match,
                                      char *matched_line, size_t matched_line_size);
void arcl_control_set_console_matcher(arcl_control_t *control, arcl_console_match_fn fn, void *userdata);

/* Shared `,"machine":"x68k","frame":N,"state":"...")[,"frames_used":M]}`
 * suffix used to close out nearly every tool response (was duplicated,
 * near-verbatim, as a static append_status() in each of arcl_l0..l4.c).
 * Appends at out+used; pass frames_used < 0 to omit that field. Goes
 * through the same lock as arcl_control_get_status() since callers are
 * outside arcl_control's own lock. */
void arcl_control_append_status(arcl_control_t *control, char *out, size_t out_size,
                                 size_t used, long frames_used);

/* Registered by whichever module needs to know which frame is *about to
 * run* at the moment it runs, not just the completed count arcl_status
 * reports afterward - e.g. arcl_opm.c timestamping OPM register writes
 * (x68k_mcp.md 6.4's x68k_opm) so a short-lived sound-effect key-on that
 * gets overwritten before the next arcl_status/x68k_opm poll is still
 * attributable to a specific frame instead of vanishing without a trace.
 * Called from advance_frame() - the same single choke point every
 * frame-advancing tool in arcl_control.c funnels through (see
 * arcl_control_set_console_matcher() above for the same registration
 * pattern) - just before px68k_frontend_run_frame(), with the frame number
 * that will be current once this advance completes (control->frame + 1).
 * Only one sink is supported (single-purpose hook, like the console
 * matcher); registering again replaces the previous one. */
typedef void (*arcl_frame_hint_fn)(void *userdata, unsigned long long next_frame);
void arcl_control_set_frame_hint_sink(arcl_control_t *control, arcl_frame_hint_fn fn, void *userdata);

/* Serializes against arcl_resume's background frame-advance thread (which
 * holds this same lock for the duration of every advance_frame() call -
 * see resume_worker() in arcl_control.c). Any tool handler that reads or
 * writes core state *directly* (m68000_get_reg/set_reg, cpu_readmem24/
 * writemem24, or core-owned global arrays like CRTC_Regs/Pal_Regs/TVRAM/
 * GVRAM) needs this: those accessors have no synchronization of their own,
 * unlike px68k_frontend_set_key() and friends, arcl_opm_shadow_get(), and
 * the audio capture buffer, which already lock internally. Without it, a
 * concurrent arcl_resume can tear or corrupt whatever's being read or
 * written. Backed by a Windows CRITICAL_SECTION, which is reentrant for
 * the owning thread, so it's safe to call while already holding it (e.g.
 * from within arcl_control_run_frames()) - but prefer the smallest scope
 * that actually covers the unsynchronized access. */
void arcl_control_lock(arcl_control_t *control);
void arcl_control_unlock(arcl_control_t *control);

#endif /* PX68K_ARCL_CONTROL_H */
