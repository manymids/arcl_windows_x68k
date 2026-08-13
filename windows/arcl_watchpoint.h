#ifndef ARCL_WATCHPOINT_H
#define ARCL_WATCHPOINT_H

#include <stdint.h>

/* This header is included from both windows/arcl_l2.c (this project's L2
 * tool implementation) and, conditionally under ARCL_WATCHPOINT, from the
 * core's x68k/mem_wrap.c (x68k_mcp.md 0.1.1). Keep it dependency-free (no
 * <windows.h>, no other core headers) so it never risks the <windows.h> /
 * libretro/compiler.h conflict documented in x68k_mcp.md 3.4. */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ARCL_WATCH_EXEC = 0,
    ARCL_WATCH_READ = 1,
    ARCL_WATCH_WRITE = 2,
    ARCL_WATCH_ACCESS = 3 /* read or write */
};

#define ARCL_WATCH_MAX 8

/* Called from mem_wrap.c's rm_main()/wm_cnt() (the single choke point for
 * every CPU and DMA access - x68k_mcp.md 0.1.1) on every guest memory
 * access, watchpoints active or not. Must stay cheap when idle. */
void arcl_watchpoint_on_read(uint32_t addr, uint8_t value);
void arcl_watchpoint_on_write(uint32_t addr, uint8_t value);

void arcl_watchpoint_init(void);
void arcl_watchpoint_shutdown(void);

/* add returns the new watchpoint's slot id (>=0), or -1 if full.
 * remove/clear operate on that id. */
int arcl_watchpoint_add(int type, uint32_t address, uint32_t length);
int arcl_watchpoint_remove(int id);
void arcl_watchpoint_clear(void);
/* Fills out[] (capacity ARCL_WATCH_MAX) with active watchpoints, each as
 * {id, type, address, length}; returns the count written. */
typedef struct {
    int id;
    int type;
    uint32_t address;
    uint32_t length;
} arcl_watch_info_t;
int arcl_watchpoint_list(arcl_watch_info_t *out, int cap);

/* Exec breakpoints share the same slot table (type ARCL_WATCH_EXEC) and
 * the same id space, per arcl_common_spec.md 7.3's arcl_breakpoint being
 * one tool for both. Checked opportunistically inside the read/write hooks
 * (x68k_mcp.md 6.3: instruction fetch bypasses rm_main/wm_cnt on this
 * core, so exact-PC exec breakpoints can only be approximated by polling
 * the current PC on every *other* bus access - still frame-granular
 * either way on the C68K backend). */

/* Call once per frame, after retro_run() returns. Returns 1 if a
 * watchpoint or exec breakpoint fired during that frame and fills the
 * hit details (clearing the pending flag); 0 otherwise. */
int arcl_watchpoint_poll_hit(int *out_type, uint32_t *out_pc, uint32_t *out_addr,
                              uint8_t *out_value, int *out_is_write, uint32_t *out_length);

#ifdef __cplusplus
}
#endif

#endif /* ARCL_WATCHPOINT_H */
