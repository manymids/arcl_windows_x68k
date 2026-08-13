/* L1 tools: arcl_console_read, arcl_command, arcl_mount, arcl_host_dir.
 *
 * X68000 text VRAM has no character codes on screen - it is a 4-plane
 * bitmap (x68k/tvram.c). The only way to recover text is glyph matching
 * against the CG ROM (x68k_mcp.md 6.2). The exact glyph-address formula
 * below is not guessed: it is lifted read-only from the core's own menu
 * text renderer (px68k-libretro/libretro/windraw.c:get_font_addr), so this
 * file never has to independently reverse-engineer the CGROM.DAT layout.
 * Everything here only *reads* core globals that are already exported via
 * headers (FONT, TVRAM, CRTC_Regs, ...) - no core file is modified.
 */
#include "arcl_l1.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "frontend_core.h" /* px68k_frontend_set_key, for the RETURN keystroke */
#include "libretro.h"      /* RETROK_RETURN */
#include "mcp_json.h"

#include "crtc.h"    /* CRTC_Regs, TextDotX/Y, TextScrollX/Y */
#include "tvram.h"   /* TVRAM[] */
#include "winx68k.h" /* FONT */

#define ANK_GLYPH_BYTES 16
/* x2 because build_ank_table() stores both the normal and the "bold"
 * variant of every glyph under the same code - see its comment. */
#define ANK_TABLE_MAX 512
#define ARCL_MAX_CONSOLE_COLS 200
#define ARCL_MAX_CONSOLE_ROWS 80
#define ARCL_COMMAND_POLL_FRAMES 30
#define ARCL_COMMAND_MAX_FRAMES 6000

typedef struct {
    uint8_t code;
    uint8_t glyph[ANK_GLYPH_BYTES];
} ank_entry_t;

struct arcl_l1 {
    arcl_control_t *control;
    arcl_l0_t *l0;
    ank_entry_t ank_table[ANK_TABLE_MAX];
    int ank_count;
    int font_ready;
};

/* Human68k renders emphasized text (e.g. the "Human68k for X680x0 ..."
 * boot banner) with a classic "poor man's bold": each font row OR'd with
 * itself shifted right one bit. Confirmed by comparing an extracted TVRAM
 * glyph against the CG ROM original during implementation: font byte 0x82
 * appeared in TVRAM as 0xc3, and 0x82 | (0x82>>1) == 0xc3 exactly (same
 * for every row of the glyph). A plain font-only table therefore misses
 * every bold-rendered character while matching everything else, which is
 * a very confusing partial failure if you don't already know to expect
 * it - hence storing both variants under the same code up front. */
static void bold_transform(const uint8_t in[ANK_GLYPH_BYTES], uint8_t out[ANK_GLYPH_BYTES])
{
    int i;
    for (i = 0; i < ANK_GLYPH_BYTES; i++)
        out[i] = (uint8_t)(in[i] | (in[i] >> 1));
}

static void build_ank_table(arcl_l1_t *l1)
{
    int code;
    if (l1->font_ready)
        return;
    l1->ank_count = 0;
    /* JIS X0201: 0x20-0x7e is ASCII-compatible half-width, 0xa1-0xdf is
     * half-width katakana. get_font_addr()'s hankaku branch (windraw.c)
     * indexes the font by this single byte alone. */
    for (code = 0x20; code <= 0xdf && l1->ank_count + 1 < ANK_TABLE_MAX; code++)
    {
        uint32_t addr;
        const uint8_t *src;
        if (code > 0x7e && code < 0xa1)
            continue;
        addr = 0x3a800u + (uint32_t)code * ANK_GLYPH_BYTES;
        if (addr + ANK_GLYPH_BYTES > 0xc0000u)
            continue;
        src = &FONT[addr];
        l1->ank_table[l1->ank_count].code = (uint8_t)code;
        memcpy(l1->ank_table[l1->ank_count].glyph, src, ANK_GLYPH_BYTES);
        l1->ank_count++;
        l1->ank_table[l1->ank_count].code = (uint8_t)code;
        bold_transform(src, l1->ank_table[l1->ank_count].glyph);
        l1->ank_count++;
    }
    l1->font_ready = 1;
}

/* Mirrors the plane-decode TVRAM_Write()/TVRAM_RCUpdate() perform in
 * x68k/tvram.c, but reading instead of writing, and reading straight from
 * the exported TVRAM[] array instead of the core's private per-pixel
 * cache (TextDrawWork is `static` in tvram.c; duplicating the decode here
 * avoids having to export it, i.e. avoids touching the core at all). */
static uint8_t tvram_pixel(unsigned x, unsigned y)
{
    unsigned row = y & 0x3ffu;
    unsigned col_byte = (x & 0x3ffu) >> 3;
    unsigned bit = 7u - (x & 7u);
    uint32_t byte_addr = (uint32_t)((row * 128u + col_byte) ^ 1u);
    uint8_t v = 0;
    if (TVRAM[byte_addr] & (1u << bit))            v |= 1;
    if (TVRAM[byte_addr + 0x20000u] & (1u << bit)) v |= 2;
    if (TVRAM[byte_addr + 0x40000u] & (1u << bit)) v |= 4;
    if (TVRAM[byte_addr + 0x60000u] & (1u << bit)) v |= 8;
    return v;
}

static void extract_cell(unsigned col, unsigned row, uint8_t out_glyph[ANK_GLYPH_BYTES])
{
    unsigned screen_x0 = col * 8;
    unsigned screen_y0 = row * 16;
    unsigned gy, gx;
    for (gy = 0; gy < 16; gy++)
    {
        unsigned tv_y = (TextScrollY + screen_y0 + gy) & 0x3ffu;
        uint8_t byte = 0;
        for (gx = 0; gx < 8; gx++)
        {
            unsigned tv_x = (TextScrollX + screen_x0 + gx) & 0x3ffu;
            if (tvram_pixel(tv_x, tv_y))
                byte |= (uint8_t)(0x80u >> gx);
        }
        out_glyph[gy] = byte;
    }
}

static int match_ank(arcl_l1_t *l1, const uint8_t glyph[ANK_GLYPH_BYTES])
{
    int i;
    for (i = 0; i < l1->ank_count; i++)
        if (memcmp(glyph, l1->ank_table[i].glyph, ANK_GLYPH_BYTES) == 0)
            return l1->ank_table[i].code;
    return -1;
}

static size_t append_utf8_for_ank(char *out, size_t pos, size_t cap, int code)
{
    if (code < 0)
        return pos; /* unmatched glyph: caller appends '?' or ' ' itself */
    if (code >= 0x20 && code <= 0x7e)
    {
        if (pos + 1 >= cap) return pos;
        out[pos++] = (char)code;
        return pos;
    }
    if (code == 0xa0)
    {
        if (pos + 1 >= cap) return pos;
        out[pos++] = ' ';
        return pos;
    }
    if (code >= 0xa1 && code <= 0xdf)
    {
        unsigned cp = 0xFF61u + (unsigned)(code - 0xA1);
        if (pos + 3 >= cap) return pos;
        out[pos++] = (char)(0xE0 | (cp >> 12));
        out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[pos++] = (char)(0x80 | (cp & 0x3F));
        return pos;
    }
    return pos;
}

/* Decodes the full visible text screen into UTF-8, one line per row
 * (newline-separated, trailing spaces trimmed per line). Unmatched glyphs
 * (full-width Kanji, corrupted cells, non-ANK content) become '?' -
 * this build only matches the half-width 8x16 font (x68k_mcp.md 6.2: 6.1.1
 * covers the empirically-verified keyboard side of this same limitation;
 * full-width glyph matching is future work). Returns cols/rows via
 * out params and the number of bytes written (excluding the NUL). */
static size_t decode_console_text(arcl_l1_t *l1, char *out, size_t cap, unsigned *out_cols, unsigned *out_rows)
{
    unsigned cols = TextDotX / 8;
    unsigned rows = TextDotY / 16;
    unsigned r, c;
    size_t pos = 0;

    if (cols > ARCL_MAX_CONSOLE_COLS) cols = ARCL_MAX_CONSOLE_COLS;
    if (rows > ARCL_MAX_CONSOLE_ROWS) rows = ARCL_MAX_CONSOLE_ROWS;
    build_ank_table(l1);

    for (r = 0; r < rows; r++)
    {
        size_t line_start = pos;
        for (c = 0; c < cols; c++)
        {
            uint8_t glyph[ANK_GLYPH_BYTES];
            int code;
            unsigned k;
            int blank;

            extract_cell(c, r, glyph);
            code = match_ank(l1, glyph);
            if (code >= 0)
            {
                pos = append_utf8_for_ank(out, pos, cap, code);
                continue;
            }
            blank = 1;
            for (k = 0; k < ANK_GLYPH_BYTES; k++)
                if (glyph[k]) { blank = 0; break; }
            if (pos + 1 < cap)
                out[pos++] = blank ? ' ' : '?';
        }
        while (pos > line_start && out[pos - 1] == ' ')
            pos--;
        if (pos + 1 < cap)
            out[pos++] = '\n';
    }
    if (pos < cap)
        out[pos] = '\0';
    else if (cap > 0)
        out[cap - 1] = '\0';

    if (out_cols) *out_cols = cols;
    if (out_rows) *out_rows = rows;
    return pos;
}

/* The bottom row is the X68000's fixed function-key label bar (always
 * non-empty - x68k_mcp.md 4.4), not part of Human68k's scrolling text
 * area. Above that, Human68k does *not* keep the cursor pinned to a fixed
 * row: after a command, the prompt sits wherever output stopped, with
 * blank rows below it until the screen actually fills and scrolls (a
 * fixed "second from bottom" row is blank far more often than not, as
 * originally assumed and then observed to be wrong: "A>" printed at row
 * 27 of 32 left three blank rows before the row-30 guess). So instead:
 * take every decoded line, and return the last non-blank one above the
 * function-key bar - that is Human68k's current line regardless of how
 * full the screen is. */
static void get_current_line(arcl_l1_t *l1, char *out, size_t cap)
{
    char full[ARCL_MAX_CONSOLE_COLS * ARCL_MAX_CONSOLE_ROWS * 3 + ARCL_MAX_CONSOLE_ROWS + 1];
    unsigned cols, rows;
    const char *line_starts[ARCL_MAX_CONSOLE_ROWS];
    size_t line_lens[ARCL_MAX_CONSOLE_ROWS];
    unsigned n = 0;
    const char *p = full;
    int r;

    decode_console_text(l1, full, sizeof(full), &cols, &rows);
    out[0] = '\0';
    while (*p && n < ARCL_MAX_CONSOLE_ROWS)
    {
        const char *nl = strchr(p, '\n');
        line_starts[n] = p;
        line_lens[n] = nl ? (size_t)(nl - p) : strlen(p);
        n++;
        if (!nl) break;
        p = nl + 1;
    }
    if (n < 2)
        return; /* nothing above the function-key bar to report */

    for (r = (int)n - 2; r >= 0; r--)
    {
        if (line_lens[r] > 0)
        {
            size_t len = line_lens[r];
            if (len >= cap) len = cap - 1;
            memcpy(out, line_starts[r], len);
            out[len] = '\0';
            return;
        }
    }
}

int arcl_l1_console_match(void *userdata, const char *text_match, char *matched_line, size_t matched_line_size)
{
    arcl_l1_t *l1 = (arcl_l1_t *)userdata;
    char full[ARCL_MAX_CONSOLE_COLS * ARCL_MAX_CONSOLE_ROWS * 3 + ARCL_MAX_CONSOLE_ROWS + 1];
    unsigned cols, rows;
    const char *found;

    if (!l1 || !text_match || !*text_match)
        return 0;
    decode_console_text(l1, full, sizeof(full), &cols, &rows);
    found = strstr(full, text_match);
    if (!found)
        return 0;
    if (matched_line && matched_line_size)
    {
        const char *line_start = found;
        const char *line_end = strchr(found, '\n');
        size_t len;
        while (line_start > full && line_start[-1] != '\n')
            line_start--;
        len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        if (len >= matched_line_size) len = matched_line_size - 1;
        memcpy(matched_line, line_start, len);
        matched_line[len] = '\0';
    }
    return 1;
}

int arcl_l1_init(arcl_l1_t **out_l1, arcl_control_t *control, arcl_l0_t *l0)
{
    arcl_l1_t *l1;
    if (!out_l1 || !control || !l0)
        return 0;
    l1 = (arcl_l1_t *)calloc(1, sizeof(*l1));
    if (!l1)
        return 0;
    l1->control = control;
    l1->l0 = l0;
    *out_l1 = l1;
    arcl_control_set_console_matcher(control, arcl_l1_console_match, l1);
    return 1;
}

void arcl_l1_shutdown(arcl_l1_t *l1)
{
    if (!l1)
        return;
    arcl_control_set_console_matcher(l1->control, NULL, NULL);
    free(l1);
}

static int handle_console_read(arcl_l1_t *l1, char *result_json, size_t result_size,
                                char *error_message, size_t error_size)
{
    char text[ARCL_MAX_CONSOLE_COLS * ARCL_MAX_CONSOLE_ROWS * 3 + ARCL_MAX_CONSOLE_ROWS + 1];
    char *quoted;
    unsigned cols, rows;
    size_t len, used;

    /* decode_console_text() reads TVRAM[]/CRTC regs directly, not
     * self-synchronized against arcl_resume. */
    arcl_control_lock(l1->control);
    len = decode_console_text(l1, text, sizeof(text), &cols, &rows);
    arcl_control_unlock(l1->control);
    quoted = (char *)malloc(len * 2 + 64);
    if (!quoted)
    {
        mcp_json_set_error(error_message, error_size, "out of memory");
        return 0;
    }
    mcp_json_write_quoted(text, quoted, len * 2 + 64);
    used = (size_t)snprintf(result_json, result_size, "{\"cols\":%u,\"rows\":%u,\"text\":%s", cols, rows, quoted);
    free(quoted);
    arcl_control_append_status(l1->control, result_json, result_size, used, -1);
    return 1;
}

/* type -> confirm the echoed prompt -> RETURN -> wait for the same prompt
 * to reappear on the current line (x68k_mcp.md 4.4). Polls in small
 * batches rather than one huge arcl_control_run_frames() call so a
 * pathological hang doesn't block indefinitely - ARCL_COMMAND_MAX_FRAMES
 * is a hard ceiling, reported as a timeout error rather than hanging the
 * MCP connection. */
static int handle_command(arcl_l1_t *l1, const char *request_json,
                           char *result_json, size_t result_size,
                           char *error_message, size_t error_size)
{
    char command[200];
    char before_line[256];
    char prompt[256];
    long total_frames = 0;
    size_t used;
    int matched;

    if (!mcp_json_get_string_any(request_json, "command", command, sizeof(command)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'command'");
        return 0;
    }

    arcl_control_lock(l1->control); /* get_current_line() reads TVRAM[] directly, not self-synchronized against arcl_resume */
    get_current_line(l1, before_line, sizeof(before_line));
    arcl_control_unlock(l1->control);

    {
        /* Reuse arcl_type itself (through the same call arcl_l0_call
         * dispatches tools/call to) so the command is echoed with the
         * exact same empirically-verified key mapping arcl_type uses
         * (arcl_common_spec.md 7.7: arcl_command decomposes into
         * arcl_type + arcl_run + arcl_console_read). */
        char type_req[256];
        char type_result[512];
        char type_err[128];
        char quoted_cmd[220];
        mcp_json_write_quoted(command, quoted_cmd, sizeof(quoted_cmd));
        snprintf(type_req, sizeof(type_req), "{\"text\":%s}", quoted_cmd);
        if (!arcl_l0_call("arcl_type", type_req, l1->l0, type_result, sizeof(type_result), type_err, sizeof(type_err)))
        {
            mcp_json_set_error(error_message, error_size, type_err[0] ? type_err : "arcl_type failed while echoing the command");
            return 0;
        }
    }

    {
        char after_line[256];
        size_t cmd_len = strlen(command);
        size_t after_len;
        arcl_control_lock(l1->control);
        get_current_line(l1, after_line, sizeof(after_line));
        arcl_control_unlock(l1->control);
        after_len = strlen(after_line);
        if (after_len >= cmd_len && strcmp(after_line + (after_len - cmd_len), command) == 0)
        {
            memcpy(prompt, after_line, after_len - cmd_len);
            prompt[after_len - cmd_len] = '\0';
        }
        else
        {
            /* Echo didn't land where expected; fall back to the pre-type
             * line so completion detection still has something to compare
             * against instead of silently matching everything. */
            strncpy(prompt, before_line, sizeof(prompt) - 1);
            prompt[sizeof(prompt) - 1] = '\0';
        }
    }

    px68k_frontend_set_key(RETROK_RETURN, 1);
    arcl_control_run_frames(l1->control, 2);
    px68k_frontend_set_key(RETROK_RETURN, 0);
    arcl_control_run_frames(l1->control, 1);
    total_frames += 3;

    matched = 0;
    while (total_frames < ARCL_COMMAND_MAX_FRAMES)
    {
        char line[256];
        size_t prompt_len = strlen(prompt);
        arcl_control_run_frames(l1->control, ARCL_COMMAND_POLL_FRAMES);
        total_frames += ARCL_COMMAND_POLL_FRAMES;
        arcl_control_lock(l1->control);
        get_current_line(l1, line, sizeof(line));
        arcl_control_unlock(l1->control);
        /* Prefix match with a 1-character tolerance rather than exact
         * equality: the blinking text cursor occupies the cell right after
         * the prompt and decodes as an unmatched glyph ('?', since there is
         * no font entry for a solid cursor block), so "A>" reads back as
         * "A>?" on roughly half of all polls depending on blink phase. An
         * exact-match check would miss every other poll and could time out
         * on an unlucky cadence. */
        if (strncmp(line, prompt, prompt_len) == 0 && strlen(line) <= prompt_len + 1)
        {
            matched = 1;
            break;
        }
    }

    if (!matched)
    {
        mcp_json_set_error(error_message, error_size, "timed out waiting for the prompt to return (6000 frames)");
        return 0;
    }

    {
        char text[ARCL_MAX_CONSOLE_COLS * ARCL_MAX_CONSOLE_ROWS * 3 + ARCL_MAX_CONSOLE_ROWS + 1];
        char *quoted;
        char quoted_prompt[300];
        char quoted_command[220];
        unsigned cols, rows;
        size_t len;

        arcl_control_lock(l1->control);
        len = decode_console_text(l1, text, sizeof(text), &cols, &rows);
        arcl_control_unlock(l1->control);
        quoted = (char *)malloc(len * 2 + 64);
        if (!quoted)
        {
            mcp_json_set_error(error_message, error_size, "out of memory");
            return 0;
        }
        mcp_json_write_quoted(text, quoted, len * 2 + 64);
        mcp_json_write_quoted(prompt, quoted_prompt, sizeof(quoted_prompt));
        mcp_json_write_quoted(command, quoted_command, sizeof(quoted_command));
        used = (size_t)snprintf(result_json, result_size,
            "{\"command\":%s,\"prompt\":%s,\"cols\":%u,\"rows\":%u,\"console\":%s",
            quoted_command, quoted_prompt, cols, rows, quoted);
        free(quoted);
    }
    arcl_control_append_status(l1->control, result_json, result_size, used, total_frames);
    return 1;
}

/* Targets whichever drive the core defaults newly-added disk images to
 * (FDD1/"B:"; there is no drive-selection argument in this MVP - see
 * x68k_mcp.md 6.2 and frontend_core.h). */
static int handle_mount(arcl_l1_t *l1, const char *request_json,
                         char *result_json, size_t result_size,
                         char *error_message, size_t error_size)
{
    char action[16];
    size_t used;

    if (!mcp_json_get_string_any(request_json, "action", action, sizeof(action)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'action' (insert/eject)");
        return 0;
    }
    if (strcmp(action, "insert") == 0)
    {
        char path[1024];
        char quoted_path[1100];
        if (!mcp_json_get_string_any(request_json, "path", path, sizeof(path)))
        {
            mcp_json_set_error(error_message, error_size, "insert requires a string field 'path'");
            return 0;
        }
        if (!px68k_frontend_mount(path))
        {
            mcp_json_set_error(error_message, error_size, "the core rejected the disk image (bad path or unsupported format)");
            return 0;
        }
        mcp_json_write_quoted(path, quoted_path, sizeof(quoted_path));
        used = (size_t)snprintf(result_json, result_size, "{\"action\":\"insert\",\"path\":%s", quoted_path);
        arcl_control_append_status(l1->control, result_json, result_size, used, -1);
        return 1;
    }
    if (strcmp(action, "eject") == 0)
    {
        if (!px68k_frontend_eject())
        {
            mcp_json_set_error(error_message, error_size, "no disk control interface available");
            return 0;
        }
        used = (size_t)snprintf(result_json, result_size, "{\"action\":\"eject\"");
        arcl_control_append_status(l1->control, result_json, result_size, used, -1);
        return 1;
    }
    mcp_json_set_error(error_message, error_size, "action must be insert or eject");
    return 0;
}

/* Weak by design (x68k_mcp.md 6.2): this core has no WindrvXM-equivalent
 * host share, so this just lists a host directory's contents at whatever
 * permission the arcl_windows_x68k.exe process itself has - there is no root
 * restriction yet (arcl_common_spec.md 5.4 flags that as required
 * hardening; tracked as future work, not implemented here). */
static int handle_host_dir(arcl_l1_t *l1, const char *request_json,
                            char *result_json, size_t result_size,
                            char *error_message, size_t error_size)
{
    char path[1024];
    DIR *dir;
    struct dirent *ent;
    size_t used = 0;
    int first = 1;

    if (!mcp_json_get_string_any(request_json, "path", path, sizeof(path)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'path'");
        return 0;
    }
    if (strstr(path, ".."))
    {
        mcp_json_set_error(error_message, error_size, "path must not contain '..'");
        return 0;
    }
    dir = opendir(path);
    if (!dir)
    {
        mcp_json_set_error(error_message, error_size, "failed to open directory (does it exist?)");
        return 0;
    }

    mcp_json_appendf(result_json, result_size, &used, "{\"entries\":[");
    while ((ent = readdir(dir)) != NULL)
    {
        char full[2048];
        struct stat st;
        int is_dir = 0;
        long long size = 0;
        char quoted_name[600];

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        if (stat(full, &st) == 0)
        {
            is_dir = (st.st_mode & S_IFDIR) ? 1 : 0;
            size = (long long)st.st_size;
        }
        mcp_json_write_quoted(ent->d_name, quoted_name, sizeof(quoted_name));
        mcp_json_appendf(result_json, result_size, &used,
                          "%s{\"name\":%s,\"is_dir\":%s,\"size\":%lld}",
                          first ? "" : ",", quoted_name, is_dir ? "true" : "false", size);
        first = 0;
        if (used >= result_size)
            break;
    }
    closedir(dir);

    {
        char quoted_path[1100];
        mcp_json_write_quoted(path, quoted_path, sizeof(quoted_path));
        mcp_json_appendf(result_json, result_size, &used, "],\"path\":%s", quoted_path);
    }
    arcl_control_append_status(l1->control, result_json, result_size, used, -1);
    return 1;
}

int arcl_l1_call(const char *name, const char *request_json, void *userdata,
                  char *result_json, size_t result_size,
                  char *error_message, size_t error_size)
{
    arcl_l1_t *l1 = (arcl_l1_t *)userdata;
    if (!l1 || !name || !result_json || !error_message)
        return 0;
    error_message[0] = '\0';

    if (strcmp(name, "arcl_console_read") == 0)
        return handle_console_read(l1, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_command") == 0)
        return handle_command(l1, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_mount") == 0)
        return handle_mount(l1, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_host_dir") == 0)
        return handle_host_dir(l1, request_json, result_json, result_size, error_message, error_size);

    mcp_json_set_error(error_message, error_size, "Unknown tool");
    return 0;
}
