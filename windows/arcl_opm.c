#include "arcl_opm.h"
#include "mutex_lock.h"

#include <string.h>

static px68k_lock_t *g_lock;
static uint8_t g_regs[256];
static uint8_t g_key_on[8];
static unsigned long long g_current_frame;
static unsigned long long g_key_on_frame[8];
static arcl_opm_event_t g_event_log[ARCL_OPM_EVENT_LOG_MAX];
static unsigned long long g_event_count; /* total ever recorded; next slot is g_event_count % ARCL_OPM_EVENT_LOG_MAX */

void arcl_opm_shadow_init(void)
{
    g_lock = px68k_lock_create();
    memset(g_regs, 0, sizeof(g_regs));
    memset(g_key_on, 0, sizeof(g_key_on));
    g_current_frame = 0;
    memset(g_key_on_frame, 0, sizeof(g_key_on_frame));
    memset(g_event_log, 0, sizeof(g_event_log));
    g_event_count = 0;
}

void arcl_opm_shadow_shutdown(void)
{
    if (g_lock)
    {
        px68k_lock_destroy(g_lock);
        g_lock = NULL;
    }
}

void arcl_opm_shadow_on_frame_hint(void *userdata, unsigned long long next_frame)
{
    (void)userdata;
    px68k_lock_enter(g_lock);
    g_current_frame = next_frame;
    px68k_lock_leave(g_lock);
}

void arcl_opm_shadow_on_write(int reg, uint8_t value)
{
    if (reg < 0 || reg > 255)
        return;
    px68k_lock_enter(g_lock);
    g_regs[reg] = value;
    if (reg == 0x08) /* Key On/Off: bits0-2 = channel, bits3-6 = slot mask */
    {
        uint8_t ch = value & 7;
        uint8_t mask = (value >> 3) & 0xf;
        arcl_opm_event_t *slot = &g_event_log[g_event_count % ARCL_OPM_EVENT_LOG_MAX];

        g_key_on[ch] = mask;
        g_key_on_frame[ch] = g_current_frame;
        slot->frame = g_current_frame;
        slot->channel = ch;
        slot->key_on_mask = mask;
        g_event_count++;
    }
    px68k_lock_leave(g_lock);
}

void arcl_opm_shadow_get(uint8_t out_regs[256], uint8_t out_key_on[8])
{
    px68k_lock_enter(g_lock);
    memcpy(out_regs, g_regs, sizeof(g_regs));
    memcpy(out_key_on, g_key_on, sizeof(g_key_on));
    px68k_lock_leave(g_lock);
}

void arcl_opm_shadow_get_key_on_frames(unsigned long long out_frames[8])
{
    px68k_lock_enter(g_lock);
    memcpy(out_frames, g_key_on_frame, sizeof(g_key_on_frame));
    px68k_lock_leave(g_lock);
}

size_t arcl_opm_shadow_get_key_on_events(arcl_opm_event_t out[ARCL_OPM_EVENT_LOG_MAX])
{
    unsigned long long n, start, i;

    px68k_lock_enter(g_lock);
    n = g_event_count < ARCL_OPM_EVENT_LOG_MAX ? g_event_count : ARCL_OPM_EVENT_LOG_MAX;
    start = g_event_count - n;
    for (i = 0; i < n; i++)
        out[i] = g_event_log[(start + i) % ARCL_OPM_EVENT_LOG_MAX];
    px68k_lock_leave(g_lock);
    return (size_t)n;
}
