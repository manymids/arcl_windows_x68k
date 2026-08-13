/* Thin wrapper around the statically-linked px68k-libretro core.
 *
 * The core (px68k-libretro/libretro.c) is compiled unmodified into this
 * executable (see windows/CMakeLists.txt). This file supplies the five
 * retro_set_* callbacks a libretro core requires and nothing else; the
 * core owns all emulation state.
 */
#include "frontend_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mutex_lock.h"

#include "libretro.h"
#include "prop.h" /* for Config.NoWaitMode, see px68k_frontend_init() */

/* <SDL.h> is safe here (unlike real <windows.h>): it doesn't pull in
 * windows.h itself (only SDL_syswm.h/SDL_egl.h/SDL_opengl*.h do, and none
 * of those are included by the plain umbrella header), so it doesn't hit
 * the prop.h/compiler.h conflict documented in x68k_mcp.md 3.4. Needed
 * here (not just main.c) for px68k_frontend_enable_audio_output(). */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#define PX68K_PATH_MAX 1024
#define PX68K_KEY_COUNT 512 /* the core only polls indices 0..319; sized generously */

static char g_system_dir[PX68K_PATH_MAX];
static char g_save_dir[PX68K_PATH_MAX];
static char g_log_path[PX68K_PATH_MAX];

/* Formatted to exactly match the px68k_cpuspeed/px68k_ramsize core-option
 * values libretro.c's update_variables() string-compares against; empty
 * means "not set", in which case core_environment() declines the query and
 * the core keeps its own built-in default. */
static char g_clock_var_value[16];
static char g_ram_var_value[16];
static FILE *g_log_file;
static double g_fps = 61.0; /* overwritten by retro_get_system_av_info() */

/* g_frame is written by core_video_refresh() on whichever thread is
 * currently calling retro_run() (the MCP handler thread during arcl_run,
 * or the resume worker thread during arcl_resume) and read by the GUI
 * thread's render loop and by arcl_screenshot. g_keys is written by
 * arcl_key/arcl_clear_input (the MCP handler thread) and read by
 * core_input_state() on whichever thread is running retro_run(). Both are
 * small enough that a single lock for both is simplest and never a
 * contention point in practice. */
static px68k_lock_t *g_lock;
static px68k_frame_t g_frame;
static uint8_t g_keys[PX68K_KEY_COUNT];
static uint8_t g_joypad[2][16];

/* arcl_audio_record's capture buffer (frontend_core.h). Guarded by g_lock
 * like everything else here - written from core_audio_sample_batch() on
 * whichever thread is running retro_run(), read/controlled from the MCP
 * handler thread via px68k_frontend_audio_capture_*(). */
#define PX68K_AUDIO_CAP_FRAMES (PX68K_AUDIO_SAMPLE_RATE * PX68K_AUDIO_CAP_SECONDS)
static int16_t *g_audio_buf; /* interleaved stereo, capacity PX68K_AUDIO_CAP_FRAMES*2 */
static size_t g_audio_count; /* frames (sample pairs) captured so far */
static int g_audio_active;
static int g_audio_truncated;

/* Live playback device (px68k_frontend_enable/disable_audio_output()).
 * Not behind g_lock: SDL_QueueAudio() is documented thread-safe, and this
 * handle is only ever written from the interactive-mode main thread
 * before/after the loop that's the sole caller of run_frame() in that
 * mode - never concurrently with itself. */
static SDL_AudioDeviceID g_audio_device;

/* Disk control: the core registers this in retro_init() regardless of
 * whether content was an M3U playlist (disk_swap_interface_init() in
 * libretro.c always calls SET_DISK_CONTROL_INTERFACE / _EXT_INTERFACE).
 * We request the ext version (GET_DISK_CONTROL_INTERFACE_VERSION >= 1) so
 * get_image_path is available for px68k_frontend_get_mount_path(). */
static retro_set_eject_state_t g_disk_set_eject;
static retro_get_eject_state_t g_disk_get_eject;
static retro_set_image_index_t g_disk_set_index;
static retro_get_image_index_t g_disk_get_index;
static retro_get_num_images_t g_disk_get_num_images;
static retro_replace_image_index_t g_disk_replace_index;
static retro_add_image_index_t g_disk_add_index;
static retro_get_image_path_t g_disk_get_image_path;
static int g_mouse_left, g_mouse_right;
static int g_mouse_pending_dx, g_mouse_pending_dy; /* not yet delivered to a frame */
static int g_mouse_frame_dx, g_mouse_frame_dy;     /* this frame's delta, snapshotted in core_input_poll() */

static void core_log_printf(enum retro_log_level level, const char *fmt, ...)
{
    static const char *prefix[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    va_list ap;
    FILE *output = g_log_file ? g_log_file : stderr;
    fprintf(output, "[px68k:%s] ", (unsigned)level < 4 ? prefix[level] : "?");
    va_start(ap, fmt);
    vfprintf(output, fmt, ap);
    va_end(ap);
    fflush(output);
}

static bool core_environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log_printf;
        return true;

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = g_system_dir[0] ? g_system_dir : NULL;
        return g_system_dir[0] != '\0';

    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_save_dir[0] ? g_save_dir : NULL;
        return g_save_dir[0] != '\0';

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        /* The core only ever requests RGB565; refusing anything else would
         * make it exit(0) immediately, so this must return true here. */
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;

    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        g_fps = ((const struct retro_system_av_info *)data)->timing.fps;
        return true;

    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        /* Per-frame size comes from video_cb's width/height directly; the
         * geometry hint isn't needed for blitting so just acknowledge it. */
        return true;

    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
        *(unsigned *)data = 1; /* -> the core uses the ext interface below */
        return true;

    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    {
        const struct retro_disk_control_callback *cb = (const struct retro_disk_control_callback *)data;
        g_disk_set_eject       = cb->set_eject_state;
        g_disk_get_eject       = cb->get_eject_state;
        g_disk_set_index       = cb->set_image_index;
        g_disk_get_index       = cb->get_image_index;
        g_disk_get_num_images  = cb->get_num_images;
        g_disk_replace_index   = cb->replace_image_index;
        g_disk_add_index       = cb->add_image_index;
        g_disk_get_image_path  = NULL;
        return true;
    }

    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
    {
        const struct retro_disk_control_ext_callback *cb = (const struct retro_disk_control_ext_callback *)data;
        g_disk_set_eject       = cb->set_eject_state;
        g_disk_get_eject       = cb->get_eject_state;
        g_disk_set_index       = cb->set_image_index;
        g_disk_get_index       = cb->get_image_index;
        g_disk_get_num_images  = cb->get_num_images;
        g_disk_replace_index   = cb->replace_image_index;
        g_disk_add_index       = cb->add_image_index;
        g_disk_get_image_path  = cb->get_image_path;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        /* Only px68k_cpuspeed/px68k_ramsize are answered (from
         * px68k_frontend_set_clock_mhz()/set_ram_size_mb(), called before
         * px68k_frontend_init() from main.c's --clock/--ram args). Every
         * other core option (joystick type, MIDI, ADPCM volume, ...)
         * falls through to "declined", same as before this case existed. */
        struct retro_variable *var = (struct retro_variable *)data;
        if (!var || !var->key)
            return false;
        if (strcmp(var->key, "px68k_cpuspeed") == 0 && g_clock_var_value[0])
        {
            var->value = g_clock_var_value;
            return true;
        }
        if (strcmp(var->key, "px68k_ramsize") == 0 && g_ram_var_value[0])
        {
            var->value = g_ram_var_value;
            return true;
        }
        return false;
    }

    /* Everything else is optional plumbing (core options,
     * MIDI, rumble, input bitmasks, frame-time callback, ...). The core
     * guards every use of these behind its own defaults when the frontend
     * declines them (verified by reading libretro.c: retro_init() and
     * update_variables() both treat a false/NULL return as "keep the
     * built-in default"), so declining is safe. */
    default:
        return false;
    }
}

static void core_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (!data)
        return; /* duplicate-frame marker; not used by this core */

    px68k_lock_enter(g_lock);
    g_frame.pixels = (const uint16_t *)data;
    g_frame.width  = width;
    g_frame.height = height;
    g_frame.stride = (unsigned)(pitch / sizeof(uint16_t));
    g_frame.valid  = 1;
    px68k_lock_leave(g_lock);
}

static void core_audio_sample(int16_t left, int16_t right)
{
    (void)left;
    (void)right;
}

static size_t core_audio_sample_batch(const int16_t *data, size_t frames)
{
    px68k_lock_enter(g_lock);
    if (g_audio_active && data && g_audio_buf)
    {
        size_t room = PX68K_AUDIO_CAP_FRAMES - g_audio_count;
        size_t take = frames < room ? frames : room;
        if (take > 0)
        {
            memcpy(g_audio_buf + g_audio_count * 2, data, take * 2 * sizeof(int16_t));
            g_audio_count += take;
        }
        if (take < frames)
            g_audio_truncated = 1;
    }
    px68k_lock_leave(g_lock);
    /* SDL_QueueAudio() is documented safe to call without holding the
     * device paused/locked, and independently of the capture path above
     * (arcl_audio_record works the same whether or not live playback is
     * enabled). g_audio_device is 0 whenever output hasn't been enabled
     * (interactive mode only - see px68k_frontend_enable_audio_output()). */
    if (g_audio_device && data)
        SDL_QueueAudio(g_audio_device, data, (Uint32)(frames * 2 * sizeof(int16_t)));
    return frames;
}

static void core_input_poll(void)
{
    /* Called once per retro_run(), before the core's per-id input_state_cb
     * queries for that frame. Snapshotting the pending mouse delta here
     * (then zeroing it) means every input_state_cb(..., MOUSE_X/Y) call
     * within this same frame sees the same value, and a fresh arcl_mouse
     * "move" issued before the *next* run_frame() accumulates cleanly on
     * top of an empty pending delta rather than the one just delivered. */
    px68k_lock_enter(g_lock);
    g_mouse_frame_dx   = g_mouse_pending_dx;
    g_mouse_frame_dy   = g_mouse_pending_dy;
    g_mouse_pending_dx = 0;
    g_mouse_pending_dy = 0;
    px68k_lock_leave(g_lock);
}

static int16_t core_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    int16_t value = 0;
    (void)index;

    if (port > 1)
        return 0;

    if (device == RETRO_DEVICE_KEYBOARD)
    {
        if (port != 0 || id >= PX68K_KEY_COUNT)
            return 0;
        px68k_lock_enter(g_lock);
        value = g_keys[id] ? 1 : 0;
        px68k_lock_leave(g_lock);
        return value;
    }

    if (device == RETRO_DEVICE_JOYPAD)
    {
        if (id >= 16)
            return 0; /* RETRO_DEVICE_ID_JOYPAD_MASK (256) and friends: unsupported, see core_environment() */
        px68k_lock_enter(g_lock);
        value = g_joypad[port][id] ? 1 : 0;
        px68k_lock_leave(g_lock);
        return value;
    }

    if (device == RETRO_DEVICE_MOUSE && port == 0)
    {
        px68k_lock_enter(g_lock);
        switch (id)
        {
        case RETRO_DEVICE_ID_MOUSE_X:     value = (int16_t)g_mouse_frame_dx; break;
        case RETRO_DEVICE_ID_MOUSE_Y:     value = (int16_t)g_mouse_frame_dy; break;
        case RETRO_DEVICE_ID_MOUSE_LEFT:  value = g_mouse_left ? 1 : 0; break;
        case RETRO_DEVICE_ID_MOUSE_RIGHT: value = g_mouse_right ? 1 : 0; break;
        default: value = 0; break;
        }
        px68k_lock_leave(g_lock);
        return value;
    }

    return 0; /* RETRO_DEVICE_ANALOG (Cyberstick) etc.: not wired up, default JOY_TYPE is PAD_2BUTTON */
}

void px68k_frontend_set_system_dir(const char *dir)
{
    strncpy(g_system_dir, dir, sizeof(g_system_dir) - 1);
    g_system_dir[sizeof(g_system_dir) - 1] = '\0';
}

void px68k_frontend_set_save_dir(const char *dir)
{
    strncpy(g_save_dir, dir, sizeof(g_save_dir) - 1);
    g_save_dir[sizeof(g_save_dir) - 1] = '\0';
}

void px68k_frontend_set_log_path(const char *path)
{
    strncpy(g_log_path, path, sizeof(g_log_path) - 1);
    g_log_path[sizeof(g_log_path) - 1] = '\0';
}

bool px68k_frontend_set_clock_mhz(int mhz)
{
    /* Must match libretro.c update_variables()'s px68k_cpuspeed strcmp()
     * table exactly - these are display strings, not just numbers. */
    switch (mhz)
    {
    case 10:  strcpy(g_clock_var_value, "10Mhz");       return true;
    case 16:  strcpy(g_clock_var_value, "16Mhz");       return true;
    case 25:  strcpy(g_clock_var_value, "25Mhz");       return true;
    case 33:  strcpy(g_clock_var_value, "33Mhz (OC)");  return true;
    case 66:  strcpy(g_clock_var_value, "66Mhz (OC)");  return true;
    case 100: strcpy(g_clock_var_value, "100Mhz (OC)"); return true;
    default:  return false;
    }
}

bool px68k_frontend_set_ram_size_mb(int mb)
{
    if (mb < 1 || mb > 12)
        return false;
    /* update_variables()'s px68k_ramsize table is "1MB".."12MB", no
     * leading zero, no space. */
    snprintf(g_ram_var_value, sizeof(g_ram_var_value), "%dMB", mb);
    return true;
}

void px68k_frontend_set_key(unsigned retro_key, int pressed)
{
    if (retro_key >= PX68K_KEY_COUNT)
        return;
    px68k_lock_enter(g_lock);
    g_keys[retro_key] = pressed ? 1 : 0;
    px68k_lock_leave(g_lock);
}

int px68k_frontend_is_key_pressed(unsigned retro_key)
{
    int pressed;
    if (retro_key >= PX68K_KEY_COUNT)
        return 0;
    px68k_lock_enter(g_lock);
    pressed = g_keys[retro_key] ? 1 : 0;
    px68k_lock_leave(g_lock);
    return pressed;
}

void px68k_frontend_clear_keys(void)
{
    px68k_lock_enter(g_lock);
    memset(g_keys, 0, sizeof(g_keys));
    px68k_lock_leave(g_lock);
}

void px68k_frontend_set_joypad(unsigned port, unsigned button, int pressed)
{
    if (port > 1 || button >= 16)
        return;
    px68k_lock_enter(g_lock);
    g_joypad[port][button] = pressed ? 1 : 0;
    px68k_lock_leave(g_lock);
}

int px68k_frontend_get_joypad(unsigned port, unsigned button)
{
    int pressed;
    if (port > 1 || button >= 16)
        return 0;
    px68k_lock_enter(g_lock);
    pressed = g_joypad[port][button] ? 1 : 0;
    px68k_lock_leave(g_lock);
    return pressed;
}

void px68k_frontend_set_mouse_left(int pressed)
{
    px68k_lock_enter(g_lock);
    g_mouse_left = pressed ? 1 : 0;
    px68k_lock_leave(g_lock);
}

void px68k_frontend_set_mouse_right(int pressed)
{
    px68k_lock_enter(g_lock);
    g_mouse_right = pressed ? 1 : 0;
    px68k_lock_leave(g_lock);
}

void px68k_frontend_move_mouse(int dx, int dy)
{
    px68k_lock_enter(g_lock);
    g_mouse_pending_dx += dx;
    g_mouse_pending_dy += dy;
    px68k_lock_leave(g_lock);
}

void px68k_frontend_get_mouse_state(int *left, int *right, int *pending_dx, int *pending_dy)
{
    px68k_lock_enter(g_lock);
    if (left) *left = g_mouse_left;
    if (right) *right = g_mouse_right;
    if (pending_dx) *pending_dx = g_mouse_pending_dx;
    if (pending_dy) *pending_dy = g_mouse_pending_dy;
    px68k_lock_leave(g_lock);
}

void px68k_frontend_clear_input(void)
{
    px68k_lock_enter(g_lock);
    memset(g_keys, 0, sizeof(g_keys));
    memset(g_joypad, 0, sizeof(g_joypad));
    g_mouse_left = g_mouse_right = 0;
    g_mouse_pending_dx = g_mouse_pending_dy = 0;
    px68k_lock_leave(g_lock);
}

bool px68k_frontend_mount(const char *path)
{
    struct retro_game_info info;
    unsigned index;

    if (!g_disk_set_eject || !g_disk_add_index || !g_disk_replace_index || !g_disk_set_index)
        return false;

    index = g_disk_get_num_images ? g_disk_get_num_images() : 0;
    if (!g_disk_set_eject(true))
        return false;
    if (!g_disk_add_index())
        return false;

    memset(&info, 0, sizeof(info));
    info.path = path;
    if (!g_disk_replace_index(index, &info))
        return false;
    if (!g_disk_set_index(index))
        return false;
    return g_disk_set_eject(false);
}

bool px68k_frontend_eject(void)
{
    if (!g_disk_set_eject)
        return false;
    return g_disk_set_eject(true);
}

bool px68k_frontend_get_mount_path(char *out, size_t out_size)
{
    unsigned index;
    if (!g_disk_get_image_path || !g_disk_get_index || out_size == 0)
        return false;
    index = g_disk_get_index();
    return g_disk_get_image_path(index, out, out_size);
}

bool px68k_frontend_init(void)
{
    g_lock = px68k_lock_create();
    memset(&g_frame, 0, sizeof(g_frame));
    memset(g_keys, 0, sizeof(g_keys));
    memset(g_joypad, 0, sizeof(g_joypad));
    g_mouse_left = g_mouse_right = 0;
    g_mouse_pending_dx = g_mouse_pending_dy = 0;
    g_mouse_frame_dx = g_mouse_frame_dy = 0;
    if (g_log_path[0])
        g_log_file = fopen(g_log_path, "a");

    retro_set_environment(core_environment);
    retro_set_video_refresh(core_video_refresh);
    retro_set_audio_sample(core_audio_sample);
    retro_set_audio_sample_batch(core_audio_sample_batch);
    retro_set_input_poll(core_input_poll);
    retro_set_input_state(core_input_state);

    retro_init();

    {
        struct retro_system_av_info avi;
        retro_get_system_av_info(&avi);
        g_fps = avi.timing.fps;
    }

    return true;
}

bool px68k_frontend_load(const char *content_path)
{
    struct retro_game_info info;
    memset(&info, 0, sizeof(info));

    if (!content_path)
        return retro_load_game(NULL);

    info.path = content_path;
    return retro_load_game(&info);
}

void px68k_frontend_run_frame(void)
{
    /* retro_run()'s per-frame execution is gated by Timer_GetCount()
     * (libretro/timer.c), which falls back to real wall-clock time
     * (timeGetTime()) whenever the frontend hasn't wired up
     * RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK (we decline it, see
     * core_environment() above). Left alone, calling retro_run()
     * back-to-back would advance the emulated machine by however much
     * *real* time happened to elapse while we were calling it - not by one
     * frame per call. That breaks the "one quantum = one retro_run()"
     * mapping arcl_run depends on and the reproducibility guarantee in
     * arcl_common_spec.md 4.8 (same state + same inputs must replay
     * identically regardless of host speed).
     *
     * Config.NoWaitMode bypasses that gate so every retro_run() call
     * executes exactly one frame, unconditionally. It has to be re-asserted
     * on every call, not just once at startup: the core's own LoadConfig()
     * (libretro/prop.c) resets it to 0 the first time retro_run() reaches
     * pre_main(), so a one-shot assignment right after retro_init() gets
     * silently clobbered on frame 1. */
    Config.NoWaitMode = 1;
    retro_run();
}

void px68k_frontend_reset(void)
{
    retro_reset();
}

size_t px68k_frontend_serialize_size(void)
{
    return retro_serialize_size();
}

bool px68k_frontend_serialize(void *data, size_t size)
{
    return retro_serialize(data, size);
}

bool px68k_frontend_unserialize(const void *data, size_t size)
{
    return retro_unserialize(data, size);
}

px68k_frame_t px68k_frontend_get_frame(void)
{
    px68k_frame_t copy;
    px68k_lock_enter(g_lock);
    copy = g_frame;
    px68k_lock_leave(g_lock);
    return copy;
}

bool px68k_frontend_copy_frame_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t *dst)
{
    bool ok = false;
    px68k_lock_enter(g_lock);
    if (g_frame.valid && (unsigned long)x + w <= g_frame.width && (unsigned long)y + h <= g_frame.height)
    {
        unsigned row;
        for (row = 0; row < h; row++)
            memcpy(dst + (size_t)row * w,
                   g_frame.pixels + (size_t)(y + row) * g_frame.stride + x,
                   (size_t)w * sizeof(uint16_t));
        ok = true;
    }
    px68k_lock_leave(g_lock);
    return ok;
}

double px68k_frontend_get_fps(void)
{
    return g_fps;
}

void px68k_frontend_audio_capture_start(void)
{
    px68k_lock_enter(g_lock);
    if (!g_audio_buf)
        g_audio_buf = (int16_t *)malloc((size_t)PX68K_AUDIO_CAP_FRAMES * 2 * sizeof(int16_t));
    g_audio_count = 0;
    g_audio_truncated = 0;
    g_audio_active = (g_audio_buf != NULL);
    px68k_lock_leave(g_lock);
}

void px68k_frontend_audio_capture_stop(int16_t **out_data, size_t *out_frames, int *out_truncated)
{
    px68k_lock_enter(g_lock);
    g_audio_active = 0;
    if (out_data)
        *out_data = NULL;
    if (out_data && out_frames && g_audio_count > 0 && g_audio_buf)
    {
        *out_data = (int16_t *)malloc(g_audio_count * 2 * sizeof(int16_t));
        if (*out_data)
            memcpy(*out_data, g_audio_buf, g_audio_count * 2 * sizeof(int16_t));
    }
    if (out_frames)
        *out_frames = g_audio_count;
    if (out_truncated)
        *out_truncated = g_audio_truncated;
    px68k_lock_leave(g_lock);
}

void px68k_frontend_audio_capture_status(int *out_active, size_t *out_frames, int *out_truncated)
{
    px68k_lock_enter(g_lock);
    if (out_active)
        *out_active = g_audio_active;
    if (out_frames)
        *out_frames = g_audio_count;
    if (out_truncated)
        *out_truncated = g_audio_truncated;
    px68k_lock_leave(g_lock);
}

bool px68k_frontend_enable_audio_output(void)
{
    SDL_AudioSpec want, have;
    if (g_audio_device)
        return true;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return false;
    memset(&want, 0, sizeof(want));
    want.freq = PX68K_AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_audio_device)
        return false;
    SDL_PauseAudioDevice(g_audio_device, 0);
    return true;
}

void px68k_frontend_disable_audio_output(void)
{
    if (g_audio_device)
    {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
}

size_t px68k_frontend_get_audio_queued_bytes(void)
{
    return g_audio_device ? (size_t)SDL_GetQueuedAudioSize(g_audio_device) : 0;
}

void px68k_frontend_shutdown(void)
{
    retro_unload_game();
    retro_deinit();
    if (g_log_file)
    {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    free(g_audio_buf);
    g_audio_buf = NULL;
    px68k_frontend_disable_audio_output();
    px68k_lock_destroy(g_lock); g_lock = NULL;
}
