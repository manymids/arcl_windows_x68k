#include "arcl_watchpoint.h"
#include "mutex_lock.h"

/* m68000.h pulls in libretro/common.h -> libretro/compiler.h, which conflicts
 * with <windows.h> in the same translation unit (windows/mutex_lock.h,
 * x68k_mcp.md 3.4). This file never includes <windows.h> directly - the lock
 * is the opaque px68k_lock_t - so that's safe here. */
#include "m68000.h"

#include <string.h>

typedef struct {
    int in_use;
    int id;
    int type;
    uint32_t address;
    uint32_t length;
} arcl_watch_slot_t;

static px68k_lock_t *g_lock;
static arcl_watch_slot_t g_watches[ARCL_WATCH_MAX];
static volatile int g_watch_active_count;
static int g_next_id;

static int g_hit_pending;
static int g_hit_type;
static uint32_t g_hit_pc;
static uint32_t g_hit_addr;
static uint8_t g_hit_value;
static int g_hit_is_write;
static uint32_t g_hit_length;

void arcl_watchpoint_init(void)
{
    g_lock = px68k_lock_create();
    memset(g_watches, 0, sizeof(g_watches));
    g_watch_active_count = 0;
    g_next_id = 1;
    g_hit_pending = 0;
}

void arcl_watchpoint_shutdown(void)
{
    if (g_lock) {
        px68k_lock_destroy(g_lock);
        g_lock = NULL;
    }
}

int arcl_watchpoint_add(int type, uint32_t address, uint32_t length)
{
    int i, id = -1;
    if (length == 0)
        length = 1;
    px68k_lock_enter(g_lock);
    for (i = 0; i < ARCL_WATCH_MAX; i++) {
        if (!g_watches[i].in_use) {
            g_watches[i].in_use = 1;
            g_watches[i].id = g_next_id++;
            g_watches[i].type = type;
            g_watches[i].address = address;
            g_watches[i].length = length;
            id = g_watches[i].id;
            g_watch_active_count++;
            break;
        }
    }
    px68k_lock_leave(g_lock);
    return id;
}

int arcl_watchpoint_remove(int id)
{
    int i, found = -1;
    px68k_lock_enter(g_lock);
    for (i = 0; i < ARCL_WATCH_MAX; i++) {
        if (g_watches[i].in_use && g_watches[i].id == id) {
            g_watches[i].in_use = 0;
            g_watch_active_count--;
            found = 0;
            break;
        }
    }
    px68k_lock_leave(g_lock);
    return found;
}

void arcl_watchpoint_clear(void)
{
    px68k_lock_enter(g_lock);
    memset(g_watches, 0, sizeof(g_watches));
    g_watch_active_count = 0;
    px68k_lock_leave(g_lock);
}

int arcl_watchpoint_list(arcl_watch_info_t *out, int cap)
{
    int i, n = 0;
    px68k_lock_enter(g_lock);
    for (i = 0; i < ARCL_WATCH_MAX && n < cap; i++) {
        if (g_watches[i].in_use) {
            out[n].id = g_watches[i].id;
            out[n].type = g_watches[i].type;
            out[n].address = g_watches[i].address;
            out[n].length = g_watches[i].length;
            n++;
        }
    }
    px68k_lock_leave(g_lock);
    return n;
}

static void record_hit_locked(int type, uint32_t pc, uint32_t addr, uint8_t value,
                               int is_write, uint32_t length)
{
    g_hit_pending = 1;
    g_hit_type = type;
    g_hit_pc = pc;
    g_hit_addr = addr;
    g_hit_value = value;
    g_hit_is_write = is_write;
    g_hit_length = length;
}

/* Shared by on_read/on_write. is_write is -1 for neither (not used here). */
static void check_common(uint32_t addr, uint8_t value, int is_write)
{
    int i, have_pc = 0;
    uint32_t pc = 0;

    if (g_watch_active_count == 0)
        return;

    px68k_lock_enter(g_lock);
    if (!g_hit_pending) {
        for (i = 0; i < ARCL_WATCH_MAX; i++) {
            arcl_watch_slot_t *w = &g_watches[i];
            if (!w->in_use)
                continue;

            if (w->type == ARCL_WATCH_EXEC) {
                /* Instruction fetch bypasses rm_main()/wm_cnt() on this core
                 * (x68k_mcp.md 6.3), so exec breakpoints are approximated by
                 * polling PC on every *other* bus access - frame-granular
                 * stopping either way on the C68K backend. */
                if (!have_pc) {
                    pc = m68000_get_reg(M68K_PC);
                    have_pc = 1;
                }
                if (pc >= w->address && pc < w->address + w->length) {
                    record_hit_locked(ARCL_WATCH_EXEC, pc, pc, 0, 0, w->length);
                    break;
                }
                continue;
            }

            if (addr < w->address || addr >= w->address + w->length)
                continue;
            if (w->type == ARCL_WATCH_READ && is_write)
                continue;
            if (w->type == ARCL_WATCH_WRITE && !is_write)
                continue;

            if (!have_pc) {
                pc = m68000_get_reg(M68K_PC);
                have_pc = 1;
            }
            record_hit_locked(w->type, pc, addr, value, is_write, w->length);
            break;
        }
    }
    px68k_lock_leave(g_lock);
}

void arcl_watchpoint_on_read(uint32_t addr, uint8_t value)
{
    check_common(addr, value, 0);
}

void arcl_watchpoint_on_write(uint32_t addr, uint8_t value)
{
    check_common(addr, value, 1);
}

int arcl_watchpoint_poll_hit(int *out_type, uint32_t *out_pc, uint32_t *out_addr,
                              uint8_t *out_value, int *out_is_write, uint32_t *out_length)
{
    int hit;
    px68k_lock_enter(g_lock);
    hit = g_hit_pending;
    if (hit) {
        *out_type = g_hit_type;
        *out_pc = g_hit_pc;
        *out_addr = g_hit_addr;
        *out_value = g_hit_value;
        *out_is_write = g_hit_is_write;
        *out_length = g_hit_length;
        g_hit_pending = 0;
    }
    px68k_lock_leave(g_lock);
    return hit;
}
