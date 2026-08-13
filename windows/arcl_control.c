#include "arcl_control.h"

#include "arcl_watchpoint.h"
#include "frontend_core.h"
#include "mcp_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#define ARCL_MAX_SAVE_SLOTS 10
#define ARCL_MAX_RUN_FRAMES 36000
#define ARCL_REWIND_MAX_DEPTH 8
#define ARCL_REWIND_MIN_INTERVAL 10

typedef struct {
    unsigned char *data;
    size_t size;
    unsigned long long frame;
} arcl_save_slot_t;

/* arcl_rewind (L4, x68k_mcp.md 6.5/Phase4): a small fixed-depth ring of
 * full-state snapshots, auto-captured every `interval` frames by
 * advance_frame() below - the single place every frame-advancing tool in
 * this file (arcl_run, arcl_control_run_frames, resume_worker) already
 * funnels through, so enabling rewind covers all of them uniformly rather
 * than requiring three separate hooks. */
typedef struct {
    unsigned char *data;
    size_t size;
    unsigned long long frame;
} arcl_rewind_slot_t;

struct arcl_control {
    CRITICAL_SECTION lock;
    HANDLE resume_thread;
    int resuming;
    unsigned long long frame;
    arcl_save_slot_t saves[ARCL_MAX_SAVE_SLOTS];
    arcl_console_match_fn console_match;
    void *console_match_userdata;

    arcl_frame_hint_fn frame_hint_fn;
    void *frame_hint_userdata;

    int rewind_enabled;
    long rewind_interval;
    arcl_rewind_slot_t rewind_ring[ARCL_REWIND_MAX_DEPTH];
    int rewind_count;  /* how many ring slots are populated (<= depth) */
    int rewind_next;   /* next ring slot to write (wraps) */

    ULONGLONG speed_start_tick;
    unsigned long long speed_start_frame;
    /* arcl_speed's "just-ended" case (x68k_mcp.md/tool description): captured
     * by pause_and_join() below whenever a resume session actually stops,
     * so arcl_control_get_speed() can keep reporting it after `resuming`
     * goes back to 0 instead of collapsing to the same all-zero response as
     * "never resumed at all". */
    int have_last_speed;
    double last_speed_fps;
    double last_speed_percent;
};

static void write_state(arcl_control_t *control, char *out, size_t out_size, long frames_used)
{
    if (frames_used >= 0)
        snprintf(out, out_size,
                 "{\"machine\":\"x68k\",\"frame\":%llu,\"state\":\"%s\",\"frames_used\":%ld}",
                 control->frame, control->resuming ? "running" : "paused", frames_used);
    else
        snprintf(out, out_size,
                 "{\"machine\":\"x68k\",\"frame\":%llu,\"state\":\"%s\"}",
                 control->frame, control->resuming ? "running" : "paused");
}

/* Must be called with control->lock held. Runs one frame, advances the
 * frame counter, and - if rewind is enabled - periodically snapshots into
 * the rewind ring (struct arcl_control above). This is the single place
 * every frame-advancing tool in this file (arcl_run, resume_worker,
 * arcl_control_run_frames) funnels through, so rewind capture and any
 * future per-frame bookkeeping only needs to live here once. */
static void advance_frame(arcl_control_t *control)
{
    if (control->frame_hint_fn)
        control->frame_hint_fn(control->frame_hint_userdata, control->frame + 1);
    px68k_frontend_run_frame();
    control->frame++;

    if (control->rewind_enabled && (control->frame % (unsigned long long)control->rewind_interval) == 0)
    {
        size_t size = px68k_frontend_serialize_size();
        if (size > 0)
        {
            arcl_rewind_slot_t *slot = &control->rewind_ring[control->rewind_next];
            if (slot->size != size)
            {
                unsigned char *nd = (unsigned char *)realloc(slot->data, size);
                if (nd)
                {
                    slot->data = nd;
                    slot->size = size;
                }
            }
            if (slot->size == size && px68k_frontend_serialize(slot->data, slot->size))
            {
                slot->frame = control->frame;
                control->rewind_next = (control->rewind_next + 1) % ARCL_REWIND_MAX_DEPTH;
                if (control->rewind_count < ARCL_REWIND_MAX_DEPTH)
                    control->rewind_count++;
            }
        }
    }
}

static DWORD WINAPI resume_worker(LPVOID opaque)
{
    arcl_control_t *control = (arcl_control_t *)opaque;
    for (;;)
    {
        EnterCriticalSection(&control->lock);
        if (!control->resuming)
        {
            LeaveCriticalSection(&control->lock);
            break;
        }
        advance_frame(control);
        LeaveCriticalSection(&control->lock);
        Sleep(1); /* Keep a control request responsive while running. */
    }
    return 0;
}

static void pause_and_join(arcl_control_t *control)
{
    HANDLE thread;
    EnterCriticalSection(&control->lock);
    if (control->resuming)
    {
        /* Snapshot the session that's ending now, while speed_start_tick/
         * _frame still describe it - arcl_control_get_speed() has no other
         * way to recover this once `resuming` flips back to 0. */
        ULONGLONG now = GetTickCount64();
        double elapsed = (double)(now - control->speed_start_tick) / 1000.0;
        unsigned long long frames = control->frame - control->speed_start_frame;
        double nominal = px68k_frontend_get_fps();
        control->last_speed_fps = elapsed > 0.05 ? (double)frames / elapsed : 0.0;
        control->last_speed_percent = nominal > 0.0 ? control->last_speed_fps / nominal * 100.0 : 0.0;
        control->have_last_speed = 1;
    }
    control->resuming = 0;
    thread = control->resume_thread;
    control->resume_thread = NULL;
    LeaveCriticalSection(&control->lock);
    if (thread)
    {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
}

static int get_slot(const char *request_json, long *slot, char *error, size_t error_size)
{
    if (!mcp_json_get_long_any(request_json, "slot", slot))
        *slot = 0;
    if (*slot < 0 || *slot >= ARCL_MAX_SAVE_SLOTS)
    {
        mcp_json_set_error(error, error_size, "slot must be between 0 and 9");
        return 0;
    }
    return 1;
}

int arcl_control_init(arcl_control_t **out_control)
{
    arcl_control_t *control;
    if (!out_control)
        return 0;
    control = (arcl_control_t *)calloc(1, sizeof(*control));
    if (!control)
        return 0;
    InitializeCriticalSection(&control->lock);
    *out_control = control;
    return 1;
}

void arcl_control_shutdown(arcl_control_t *control)
{
    int i;
    if (!control)
        return;
    pause_and_join(control);
    for (i = 0; i < ARCL_MAX_SAVE_SLOTS; i++)
        free(control->saves[i].data);
    for (i = 0; i < ARCL_REWIND_MAX_DEPTH; i++)
        free(control->rewind_ring[i].data);
    DeleteCriticalSection(&control->lock);
    free(control);
}

void arcl_control_get_speed(arcl_control_t *control, double *out_fps, double *out_percent, int *out_running)
{
    EnterCriticalSection(&control->lock);
    if (control->resuming)
    {
        ULONGLONG now = GetTickCount64();
        double elapsed = (double)(now - control->speed_start_tick) / 1000.0;
        unsigned long long frames = control->frame - control->speed_start_frame;
        double fps = elapsed > 0.05 ? (double)frames / elapsed : 0.0;
        if (out_fps) *out_fps = fps;
        if (out_percent) *out_percent = px68k_frontend_get_fps() > 0.0 ? fps / px68k_frontend_get_fps() * 100.0 : 0.0;
        if (out_running) *out_running = 1;
    }
    else if (control->have_last_speed)
    {
        /* Just-ended session (paused after having been resumed at least
         * once) - the tool description promises this, not the same
         * all-zero response as "never resumed". */
        if (out_fps) *out_fps = control->last_speed_fps;
        if (out_percent) *out_percent = control->last_speed_percent;
        if (out_running) *out_running = 0;
    }
    else
    {
        if (out_fps) *out_fps = 0.0;
        if (out_percent) *out_percent = 0.0;
        if (out_running) *out_running = 0;
    }
    LeaveCriticalSection(&control->lock);
}

int arcl_control_rewind_call(arcl_control_t *control, const char *request_json,
                              char *result_json, size_t result_size,
                              char *error_message, size_t error_size)
{
    char action[16];
    size_t used;

    if (!mcp_json_get_string_any(request_json, "action", action, sizeof(action)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'action' (enable/disable/status/rewind)");
        return 0;
    }

    if (strcmp(action, "enable") == 0)
    {
        long interval;
        if (!mcp_json_get_long_any(request_json, "interval", &interval))
            interval = 60;
        if (interval < ARCL_REWIND_MIN_INTERVAL || interval > ARCL_MAX_RUN_FRAMES)
        {
            mcp_json_set_error(error_message, error_size, "interval must be between 10 and 36000");
            return 0;
        }
        EnterCriticalSection(&control->lock);
        control->rewind_enabled = 1;
        control->rewind_interval = interval;
        control->rewind_count = 0;
        control->rewind_next = 0;
        used = (size_t)snprintf(result_json, result_size,
                                 "{\"action\":\"enable\",\"interval\":%ld,\"depth\":%d",
                                 interval, ARCL_REWIND_MAX_DEPTH);
        used += (size_t)snprintf(result_json + used, result_size - used,
                 ",\"machine\":\"x68k\",\"frame\":%llu,\"state\":\"%s\"}",
                 control->frame, control->resuming ? "running" : "paused");
        LeaveCriticalSection(&control->lock);
        return 1;
    }

    if (strcmp(action, "disable") == 0)
    {
        int i;
        EnterCriticalSection(&control->lock);
        control->rewind_enabled = 0;
        for (i = 0; i < ARCL_REWIND_MAX_DEPTH; i++)
        {
            free(control->rewind_ring[i].data);
            control->rewind_ring[i].data = NULL;
            control->rewind_ring[i].size = 0;
        }
        control->rewind_count = 0;
        control->rewind_next = 0;
        used = (size_t)snprintf(result_json, result_size, "{\"action\":\"disable\"");
        used += (size_t)snprintf(result_json + used, result_size - used,
                 ",\"machine\":\"x68k\",\"frame\":%llu,\"state\":\"%s\"}",
                 control->frame, control->resuming ? "running" : "paused");
        LeaveCriticalSection(&control->lock);
        return 1;
    }

    if (strcmp(action, "status") == 0)
    {
        size_t i, total = 0;
        EnterCriticalSection(&control->lock);
        for (i = 0; i < ARCL_REWIND_MAX_DEPTH; i++)
            total += control->rewind_ring[i].size;
        used = (size_t)snprintf(result_json, result_size,
            "{\"action\":\"status\",\"enabled\":%s,\"interval\":%ld,\"depth\":%d,\"entries\":%d,\"total_bytes\":%zu",
            control->rewind_enabled ? "true" : "false", control->rewind_interval,
            ARCL_REWIND_MAX_DEPTH, control->rewind_count, total);
        used += (size_t)snprintf(result_json + used, result_size - used,
                 ",\"machine\":\"x68k\",\"frame\":%llu,\"state\":\"%s\"}",
                 control->frame, control->resuming ? "running" : "paused");
        LeaveCriticalSection(&control->lock);
        return 1;
    }

    if (strcmp(action, "rewind") == 0)
    {
        long steps;
        int idx;
        if (!mcp_json_get_long_any(request_json, "steps", &steps))
            steps = 1;
        pause_and_join(control);
        EnterCriticalSection(&control->lock);
        if (steps < 1 || steps > control->rewind_count)
        {
            LeaveCriticalSection(&control->lock);
            mcp_json_set_error(error_message, error_size, "not enough rewind history for that many steps");
            return 0;
        }
        /* rewind_next is the slot the *next* capture will overwrite, i.e.
         * one past the most recent entry; walk back `steps` from there. */
        idx = (control->rewind_next - (int)steps + ARCL_REWIND_MAX_DEPTH) % ARCL_REWIND_MAX_DEPTH;
        if (!px68k_frontend_unserialize(control->rewind_ring[idx].data, control->rewind_ring[idx].size))
        {
            LeaveCriticalSection(&control->lock);
            mcp_json_set_error(error_message, error_size, "core failed to restore rewind snapshot");
            return 0;
        }
        control->frame = control->rewind_ring[idx].frame;
        /* Restoring to an older point invalidates anything captured after
         * it (their `frame` values would otherwise be >= the one we just
         * restored to, which is nonsensical for a rewind history). */
        control->rewind_count -= (int)steps;
        control->rewind_next = idx;
        used = (size_t)snprintf(result_json, result_size, "{\"action\":\"rewind\",\"steps\":%ld", steps);
        used += (size_t)snprintf(result_json + used, result_size - used,
                 ",\"machine\":\"x68k\",\"frame\":%llu,\"state\":\"%s\"}",
                 control->frame, control->resuming ? "running" : "paused");
        LeaveCriticalSection(&control->lock);
        return 1;
    }

    mcp_json_set_error(error_message, error_size, "action must be enable, disable, status, or rewind");
    return 0;
}

unsigned long long arcl_control_run_frames(arcl_control_t *control, long frames)
{
    unsigned long long result;
    pause_and_join(control);
    EnterCriticalSection(&control->lock);
    for (long i = 0; i < frames; i++)
        advance_frame(control);
    result = control->frame;
    LeaveCriticalSection(&control->lock);
    return result;
}

void arcl_control_get_status(arcl_control_t *control, unsigned long long *frame, int *running)
{
    EnterCriticalSection(&control->lock);
    if (frame)
        *frame = control->frame;
    if (running)
        *running = control->resuming;
    LeaveCriticalSection(&control->lock);
}

void arcl_control_set_frame(arcl_control_t *control, unsigned long long frame)
{
    EnterCriticalSection(&control->lock);
    control->frame = frame;
    LeaveCriticalSection(&control->lock);
}

void arcl_control_set_console_matcher(arcl_control_t *control, arcl_console_match_fn fn, void *userdata)
{
    control->console_match = fn;
    control->console_match_userdata = userdata;
}

void arcl_control_set_frame_hint_sink(arcl_control_t *control, arcl_frame_hint_fn fn, void *userdata)
{
    control->frame_hint_fn = fn;
    control->frame_hint_userdata = userdata;
}

void arcl_control_lock(arcl_control_t *control)
{
    EnterCriticalSection(&control->lock);
}

void arcl_control_unlock(arcl_control_t *control)
{
    LeaveCriticalSection(&control->lock);
}

void arcl_control_append_status(arcl_control_t *control, char *out, size_t out_size,
                                 size_t used, long frames_used)
{
    unsigned long long frame;
    int running;
    if (used > out_size)
        used = out_size; /* defensive: a caller's own `used` bookkeeping could
                           * have overshot via a truncated snprintf upstream;
                           * clamp rather than let out_size-used underflow
                           * into a huge size_t and hand snprintf a bogus
                           * bounds pair. */
    arcl_control_get_status(control, &frame, &running);
    if (frames_used >= 0)
        snprintf(out + used, out_size - used,
                 ",\"machine\":\"x68k\",\"frame\":%llu,\"state\":\"%s\",\"frames_used\":%ld}",
                 frame, running ? "running" : "paused", frames_used);
    else
        snprintf(out + used, out_size - used,
                 ",\"machine\":\"x68k\",\"frame\":%llu,\"state\":\"%s\"}",
                 frame, running ? "running" : "paused");
}

int arcl_control_call(const char *name, const char *request_json, void *userdata,
                      char *result_json, size_t result_size,
                      char *error_message, size_t error_size)
{
    arcl_control_t *control = (arcl_control_t *)userdata;
    long frames;
    long slot;

    if (!control || !name || !result_json || !error_message)
        return 0;
    error_message[0] = '\0';
    if (strcmp(name, "arcl_status") == 0)
    {
        EnterCriticalSection(&control->lock);
        write_state(control, result_json, result_size, -1);
        LeaveCriticalSection(&control->lock);
        return 1;
    }
    if (strcmp(name, "arcl_pause") == 0)
    {
        pause_and_join(control);
        EnterCriticalSection(&control->lock);
        write_state(control, result_json, result_size, -1);
        LeaveCriticalSection(&control->lock);
        return 1;
    }
    if (strcmp(name, "arcl_resume") == 0)
    {
        EnterCriticalSection(&control->lock);
        if (!control->resuming)
        {
            control->resuming = 1;
            control->speed_start_tick = GetTickCount64();
            control->speed_start_frame = control->frame;
            control->resume_thread = CreateThread(NULL, 0, resume_worker, control, 0, NULL);
            if (!control->resume_thread)
            {
                control->resuming = 0;
                LeaveCriticalSection(&control->lock);
                mcp_json_set_error(error_message, error_size, "failed to start emulator thread");
                return 0;
            }
        }
        write_state(control, result_json, result_size, -1);
        LeaveCriticalSection(&control->lock);
        return 1;
    }
    if (strcmp(name, "arcl_run") == 0)
    {
        char text_match[128];
        int have_match = mcp_json_get_string_any(request_json, "text_match", text_match, sizeof(text_match));
        char matched_line[256];
        int until_break = 0;
        long i;
        const char *stop_reason = "frames";
        int hit_type = 0, hit_is_write = 0;
        uint32_t hit_pc = 0, hit_addr = 0, hit_length = 0;
        uint8_t hit_value = 0;

        mcp_json_get_bool_any(request_json, "until_break", &until_break);
        if (!mcp_json_get_long_any(request_json, "frames", &frames))
            frames = 1;
        if (frames < 1 || frames > ARCL_MAX_RUN_FRAMES)
        {
            mcp_json_set_error(error_message, error_size, "frames must be between 1 and 36000");
            return 0;
        }

        pause_and_join(control);
        EnterCriticalSection(&control->lock);
        matched_line[0] = '\0';
        for (i = 0; i < frames; i++)
        {
            advance_frame(control);
            if (until_break &&
                arcl_watchpoint_poll_hit(&hit_type, &hit_pc, &hit_addr, &hit_value, &hit_is_write, &hit_length))
            {
                i++; /* this frame counts toward frames_used */
                stop_reason = "breakpoint";
                break;
            }
            if (have_match && control->console_match &&
                control->console_match(control->console_match_userdata, text_match,
                                        matched_line, sizeof(matched_line)))
            {
                i++; /* this frame counts toward frames_used */
                stop_reason = "text_match";
                break;
            }
        }
        {
            /* arcl_common_spec.md 4.7: stop_reason is required on every
             * arcl_run response, not just early-stop ones - "frames" here
             * means "ran the full request, no earlier condition fired". */
            char quoted_line[300];
            size_t used = (size_t)snprintf(result_json, result_size,
                "{\"machine\":\"x68k\",\"frame\":%llu,\"state\":\"%s\",\"frames_used\":%ld,\"stop_reason\":\"%s\"",
                control->frame, control->resuming ? "running" : "paused", i, stop_reason);
            if (strcmp(stop_reason, "text_match") == 0 && used < result_size)
            {
                mcp_json_write_quoted(matched_line, quoted_line, sizeof(quoted_line));
                used += (size_t)snprintf(result_json + used, result_size - used,
                                          ",\"matched_line\":%s", quoted_line);
            }
            else if (strcmp(stop_reason, "breakpoint") == 0 && used < result_size)
            {
                /* x68k_mcp.md 6.3: pc is the executing instruction's PC, not
                 * a fetch pointer; exec-type hits report pc == addr since
                 * they are detected by polling pc itself (arcl_watchpoint.c). */
                static const char *const type_names[4] = { "exec", "read", "write", "access" };
                used += (size_t)snprintf(result_json + used, result_size - used,
                    ",\"breakpoint\":{\"type\":\"%s\",\"pc\":\"0x%x\",\"address\":\"0x%x\","
                    "\"value\":\"0x%x\",\"is_write\":%s,\"length\":%u}",
                    type_names[hit_type & 3], (unsigned)hit_pc, (unsigned)hit_addr,
                    (unsigned)hit_value, hit_is_write ? "true" : "false", (unsigned)hit_length);
            }
            if (used < result_size)
                snprintf(result_json + used, result_size - used, "}");
        }
        LeaveCriticalSection(&control->lock);
        return 1;
    }
    if (strcmp(name, "arcl_reset") == 0)
    {
        pause_and_join(control);
        EnterCriticalSection(&control->lock);
        px68k_frontend_reset();
        control->frame = 0;
        write_state(control, result_json, result_size, -1);
        LeaveCriticalSection(&control->lock);
        return 1;
    }
    if (strcmp(name, "arcl_save_state") == 0)
    {
        size_t size;
        arcl_save_slot_t *save;
        if (!get_slot(request_json, &slot, error_message, error_size))
            return 0;
        pause_and_join(control);
        EnterCriticalSection(&control->lock);
        size = px68k_frontend_serialize_size();
        save = &control->saves[slot];
        if (size == 0 || (save->size != size && !(save->data = (unsigned char *)realloc(save->data, size))))
        {
            LeaveCriticalSection(&control->lock);
            mcp_json_set_error(error_message, error_size, "failed to allocate save-state buffer");
            return 0;
        }
        save->size = size;
        if (!px68k_frontend_serialize(save->data, save->size))
        {
            LeaveCriticalSection(&control->lock);
            mcp_json_set_error(error_message, error_size, "core failed to serialize state");
            return 0;
        }
        save->frame = control->frame;
        snprintf(result_json, result_size,
                 "{\"machine\":\"x68k\",\"frame\":%llu,\"state\":\"paused\",\"slot\":%ld,\"bytes\":%zu}",
                 control->frame, slot, save->size);
        LeaveCriticalSection(&control->lock);
        return 1;
    }
    if (strcmp(name, "arcl_load_state") == 0)
    {
        arcl_save_slot_t *save;
        if (!get_slot(request_json, &slot, error_message, error_size))
            return 0;
        pause_and_join(control);
        EnterCriticalSection(&control->lock);
        save = &control->saves[slot];
        if (!save->data || !px68k_frontend_unserialize(save->data, save->size))
        {
            LeaveCriticalSection(&control->lock);
            mcp_json_set_error(error_message, error_size, "save-state slot is empty or incompatible");
            return 0;
        }
        control->frame = save->frame;
        snprintf(result_json, result_size,
                 "{\"machine\":\"x68k\",\"frame\":%llu,\"state\":\"paused\",\"slot\":%ld}",
                 control->frame, slot);
        LeaveCriticalSection(&control->lock);
        return 1;
    }
    mcp_json_set_error(error_message, error_size, "Unknown tool");
    return 0;
}
