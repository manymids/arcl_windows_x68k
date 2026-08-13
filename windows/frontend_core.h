#ifndef PX68K_FRONTEND_CORE_H
#define PX68K_FRONTEND_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Current framebuffer as delivered by the core's video_refresh callback.
 * pixels is RGB565, row-major. stride is in pixels (may exceed width). */
typedef struct {
    const uint16_t *pixels;
    unsigned width;
    unsigned height;
    unsigned stride;
    int valid; /* 0 until the core has produced at least one frame */
} px68k_frame_t;

/* Must be called before px68k_frontend_init(). Strings are copied. */
void px68k_frontend_set_system_dir(const char *dir);
void px68k_frontend_set_save_dir(const char *dir);
void px68k_frontend_set_log_path(const char *path);

/* Must be called before px68k_frontend_init() - the core reads these once,
 * from update_variables(0) inside retro_init() (libretro.c), and the
 * frontend answers RETRO_ENVIRONMENT_GET_VARIABLE from a value formatted
 * here. Only the exact values the core's px68k_cpuspeed/px68k_ramsize core
 * options recognize are accepted (see libretro.c update_variables()); an
 * unrecognized value returns false and leaves the core's own default
 * (10MHz / 2MB) in place, same as if this were never called at all. */
bool px68k_frontend_set_clock_mhz(int mhz);   /* one of 10/16/25/33/66/100 */
bool px68k_frontend_set_ram_size_mb(int mb);  /* 1..12 */

/* Held-key state consumed by the core's keyboard polling (RETROK_* codes,
 * see libretro.h). The core only ever polls indices 0..319 (libretro.c's
 * Core_Key_State loop), so codes >= 320 are accepted but never observed by
 * the guest. Safe to call from any thread; internally synchronized with
 * the emulation thread's input_state_cb reads. */
void px68k_frontend_set_key(unsigned retro_key, int pressed);
int px68k_frontend_is_key_pressed(unsigned retro_key);
void px68k_frontend_clear_keys(void);

/* Joypad: port is 0 or 1, button is a RETRO_DEVICE_ID_JOYPAD_* id (0..15). */
void px68k_frontend_set_joypad(unsigned port, unsigned button, int pressed);
int px68k_frontend_get_joypad(unsigned port, unsigned button);

/* Mouse buttons are held state, like keys. Movement is a *relative* delta:
 * px68k_frontend_move_mouse() adds to a pending accumulator that is
 * delivered to the guest (and reset to zero) the next time a frame actually
 * runs - consistent with nothing happening without run_frame(). */
void px68k_frontend_set_mouse_left(int pressed);
void px68k_frontend_set_mouse_right(int pressed);
void px68k_frontend_move_mouse(int dx, int dy);
void px68k_frontend_get_mouse_state(int *left, int *right, int *pending_dx, int *pending_dy);

void px68k_frontend_clear_input(void); /* keys + joypad + mouse buttons + pending motion */

/* Disk swap via the core's own multi-disk interface (SET_DISK_CONTROL_
 * INTERFACE), which the core always registers in retro_init() regardless
 * of whether content was an M3U playlist. Targets whichever drive the
 * core defaults newly-added images to (FDD1/"B:" - x68k_mcp.md 6.2); there
 * is currently no way to pick the drive explicitly. */
bool px68k_frontend_mount(const char *path);
bool px68k_frontend_eject(void);
/* Path of the currently-selected disk image, if the core negotiated the
 * ext disk-control interface (it does here) and something is mounted via
 * px68k_frontend_mount(). Returns false (and leaves out untouched) if
 * neither holds. */
bool px68k_frontend_get_mount_path(char *out, size_t out_size);

/* Wraps retro_set_environment/retro_init(). Call exactly once at startup. */
bool px68k_frontend_init(void);

/* Wraps retro_load_game() with a full content path (NULL = no content). */
bool px68k_frontend_load(const char *content_path);

/* Wraps retro_run() for exactly one frame (one quantum, per arcl_common_spec.md 4.2). */
void px68k_frontend_run_frame(void);

void px68k_frontend_reset(void);
size_t px68k_frontend_serialize_size(void);
bool px68k_frontend_serialize(void *data, size_t size);
bool px68k_frontend_unserialize(const void *data, size_t size);

/* Snapshot of the framebuffer as of the most recent run_frame() call.
 * `pixels` is a pointer into the core's own internal buffer, not a copy -
 * safe to read momentarily (e.g. to check width/height/valid before
 * allocating an output buffer), but NOT safe to keep reading from across
 * any window where a concurrent arcl_resume could call run_frame() again:
 * the core reuses/overwrites that same buffer in place on every frame, and
 * nothing here holds it stable for you. For actually reading pixel data,
 * use px68k_frontend_copy_frame_rect() below instead. */
px68k_frame_t px68k_frontend_get_frame(void);

/* Copies a w*h crop starting at (x,y) out of the *current* framebuffer into
 * dst (row-major, w*h uint16_t values, no row padding), entirely while
 * holding the frame lock - unlike px68k_frontend_get_frame() above, which
 * only snapshots the *pointer*, this guarantees the pixel bytes themselves
 * can't be concurrently overwritten mid-copy by a resume-thread's
 * run_frame()/video_cb. Re-validates bounds against whatever frame is
 * current at the moment the lock is acquired (which may differ from an
 * earlier px68k_frontend_get_frame() call if a resume was racing with the
 * caller) - returns false without touching dst if there's no valid frame
 * or the rect doesn't fit. */
bool px68k_frontend_copy_frame_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t *dst);

double px68k_frontend_get_fps(void);

/* Independent audio capture for arcl_audio_record (x68k_mcp.md 6.4/Phase4).
 * Fixed at the core's own SOUNDRATE (44100Hz stereo s16, see
 * libretro/timer.c). core_audio_sample_batch() feeds this capture buffer
 * and (if px68k_frontend_enable_audio_output() below was called) the live
 * SDL audio device in parallel - capture is unaffected by whether live
 * playback is enabled, satisfying arcl_common_spec.md's "independent of
 * the playback path" requirement. Capped at 60 seconds (2,646,000 frames);
 * further samples are dropped (not overwritten) once full, reported via
 * *out_truncated. */
#define PX68K_AUDIO_SAMPLE_RATE 44100
#define PX68K_AUDIO_CAP_SECONDS 60

void px68k_frontend_audio_capture_start(void);
/* Stops capturing. If out_data/out_frames are non-NULL, *out_data receives
 * a malloc'd copy of the captured interleaved stereo s16 samples (caller
 * frees it) and *out_frames the sample-pair count; pass NULL for both to
 * just stop without retrieving data. *out_truncated (if non-NULL) is set
 * when the cap was hit before capture was stopped. */
void px68k_frontend_audio_capture_stop(int16_t **out_data, size_t *out_frames, int *out_truncated);
void px68k_frontend_audio_capture_status(int *out_active, size_t *out_frames, int *out_truncated);

/* Live speaker playback via SDL's audio queue - separate from the capture
 * buffer above (both are fed from the same core_audio_sample_batch(), x68k
 * only has one audio stream to begin with). Interactive-mode-only by
 * convention (main.c): MCP sessions don't need a real audio device and
 * may run somewhere without one. Safe to call when SDL_INIT_AUDIO hasn't
 * been initialized yet - this initializes it. Returns false (frontend_core
 * keeps running silently) if the platform has no audio device at all. */
bool px68k_frontend_enable_audio_output(void);
void px68k_frontend_disable_audio_output(void);

/* Bytes currently sitting in the live audio device's playback queue (0 if
 * live output was never enabled, or was enabled but SDL couldn't find a
 * device). Interactive-mode frame pacing (main.c) polls this: a queue
 * that's about to run dry is the direct, measurable precursor to an
 * audible click/pop (SDL plays silence once it drains a queue with
 * nothing behind it), so pacing off of this catches the problem before it
 * makes noise instead of guessing at a fixed delay and hoping it matches
 * the core's actual frame rate. */
size_t px68k_frontend_get_audio_queued_bytes(void);

/* Wraps retro_unload_game()/retro_deinit(). */
void px68k_frontend_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PX68K_FRONTEND_CORE_H */
