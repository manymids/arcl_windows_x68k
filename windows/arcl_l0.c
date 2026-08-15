/* L0 tools: arcl_screenshot, arcl_key, arcl_clear_input.
 *
 * arcl_key/arcl_clear_input write into frontend_core's held-key state,
 * which core_input_state() (frontend_core.c) exposes to the emulated
 * keyboard. arcl_screenshot reads the framebuffer frontend_core keeps
 * (px68k_frontend_get_frame()) and crops/scales/grids it in-place before
 * handing it to png_write.c.
 */
#include "arcl_l0.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include "base64.h"
#include "frontend_core.h"
#include "libretro.h" /* RETROK_* */
#include "mcp_json.h"
#include "png_write.h"
#include "wav_write.h"

/* Below this peak sample magnitude (out of 32767), a captured clip counts
 * as "silent" for arcl_audio_record's `silent` field - real digital
 * silence is exactly 0, but a tiny noise floor is normal even with
 * nothing intentionally playing. */
#define PX68K_AUDIO_SILENCE_PEAK 256

#define PX68K_MAGENTA565 0xF81Fu
#define PX68K_MAX_SCALE 16u
/* A full 800x600 frame at scale=1 (this core's max resolution, libretro.c:129)
 * must always fit, so the cap can't go below that. It's set right at that
 * boundary rather than higher: crop+scale is meant to zoom into a *smaller*
 * region, not blow the whole screen up past its own pixel count. This also
 * bounds the worst-case inline base64 PNG payload mcp_server.c's response
 * buffer has to hold (see MCP_RESULT_MAX there). */
#define PX68K_MAX_SCALED_PIXELS 480000u
#define PX68K_MAX_TAP_FRAMES 3600
#define PX68K_MAX_TYPE_CHARS 256
#define PX68K_MAX_MACRO_STEPS 16
#define PX68K_MAX_MACRO_TOTAL_FRAMES 10000
/* Must be at least as generous as a standalone tool call's own result
 * buffer (mcp_server.c's MCP_RESULT_MAX) - see the comment on sub_result
 * in handle_macro() below for why. */
#define PX68K_MACRO_STEP_RESULT_MAX (3 * 1024 * 1024)

struct arcl_l0 {
    arcl_control_t *control;
};

typedef struct {
    const char *name;
    unsigned code;
    int blocked; /* core-internal side effect if pressed via MCP; see x68k_mcp.md 6.1 */
} key_entry_t;

static const key_entry_t KEY_TABLE[] = {
    { "a", RETROK_a, 0 }, { "b", RETROK_b, 0 }, { "c", RETROK_c, 0 }, { "d", RETROK_d, 0 },
    { "e", RETROK_e, 0 }, { "f", RETROK_f, 0 }, { "g", RETROK_g, 0 }, { "h", RETROK_h, 0 },
    { "i", RETROK_i, 0 }, { "j", RETROK_j, 0 }, { "k", RETROK_k, 0 }, { "l", RETROK_l, 0 },
    { "m", RETROK_m, 0 }, { "n", RETROK_n, 0 }, { "o", RETROK_o, 0 }, { "p", RETROK_p, 0 },
    { "q", RETROK_q, 0 }, { "r", RETROK_r, 0 }, { "s", RETROK_s, 0 }, { "t", RETROK_t, 0 },
    { "u", RETROK_u, 0 }, { "v", RETROK_v, 0 }, { "w", RETROK_w, 0 }, { "x", RETROK_x, 0 },
    { "y", RETROK_y, 0 }, { "z", RETROK_z, 0 },
    { "0", RETROK_0, 0 }, { "1", RETROK_1, 0 }, { "2", RETROK_2, 0 }, { "3", RETROK_3, 0 },
    { "4", RETROK_4, 0 }, { "5", RETROK_5, 0 }, { "6", RETROK_6, 0 }, { "7", RETROK_7, 0 },
    { "8", RETROK_8, 0 }, { "9", RETROK_9, 0 },
    { "space", RETROK_SPACE, 0 }, { "return", RETROK_RETURN, 0 }, { "enter", RETROK_RETURN, 0 },
    { "tab", RETROK_TAB, 0 }, { "backspace", RETROK_BACKSPACE, 0 }, { "escape", RETROK_ESCAPE, 0 },
    { "minus", RETROK_MINUS, 0 }, { "equals", RETROK_EQUALS, 0 }, { "comma", RETROK_COMMA, 0 },
    { "period", RETROK_PERIOD, 0 }, { "slash", RETROK_SLASH, 0 }, { "semicolon", RETROK_SEMICOLON, 0 },
    { "quote", RETROK_QUOTE, 0 }, { "leftbracket", RETROK_LEFTBRACKET, 0 },
    { "rightbracket", RETROK_RIGHTBRACKET, 0 }, { "backslash", RETROK_BACKSLASH, 0 },
    { "backquote", RETROK_BACKQUOTE, 0 },
    { "up", RETROK_UP, 0 }, { "down", RETROK_DOWN, 0 }, { "left", RETROK_LEFT, 0 }, { "right", RETROK_RIGHT, 0 },
    { "insert", RETROK_INSERT, 0 }, { "delete", RETROK_DELETE, 0 }, { "home", RETROK_HOME, 0 },
    { "end", RETROK_END, 0 }, { "pageup", RETROK_PAGEUP, 0 }, { "pagedown", RETROK_PAGEDOWN, 0 },
    { "f1", RETROK_F1, 0 }, { "f2", RETROK_F2, 0 }, { "f3", RETROK_F3, 0 }, { "f4", RETROK_F4, 0 },
    { "f5", RETROK_F5, 0 }, { "f6", RETROK_F6, 0 }, { "f7", RETROK_F7, 0 }, { "f8", RETROK_F8, 0 },
    { "f9", RETROK_F9, 0 }, { "f10", RETROK_F10, 0 },
    { "f12", RETROK_F12, 1 },        /* core: opens the built-in menu (WinUI_Menu), see x68k_mcp.md 6.1 */
    { "scrolllock", RETROK_SCROLLOCK, 1 }, /* core: toggles MIDI output, see x68k_mcp.md 6.1 */
    { "shift", RETROK_LSHIFT, 0 }, { "lshift", RETROK_LSHIFT, 0 }, { "rshift", RETROK_RSHIFT, 0 },
    { "ctrl", RETROK_LCTRL, 0 }, { "lctrl", RETROK_LCTRL, 0 }, { "rctrl", RETROK_RCTRL, 0 },
    { "alt", RETROK_LALT, 0 }, { "lalt", RETROK_LALT, 0 }, { "ralt", RETROK_RALT, 0 },
    { "capslock", RETROK_CAPSLOCK, 0 }, { "numlock", RETROK_NUMLOCK, 0 },
    { "kp0", RETROK_KP0, 0 }, { "kp1", RETROK_KP1, 0 }, { "kp2", RETROK_KP2, 0 }, { "kp3", RETROK_KP3, 0 },
    { "kp4", RETROK_KP4, 0 }, { "kp5", RETROK_KP5, 0 }, { "kp6", RETROK_KP6, 0 }, { "kp7", RETROK_KP7, 0 },
    { "kp8", RETROK_KP8, 0 }, { "kp9", RETROK_KP9, 0 }, { "kp_period", RETROK_KP_PERIOD, 0 },
    { "kp_enter", RETROK_KP_ENTER, 0 },
    /* Shifted-symbol keysyms, needed for arcl_type (see the char table below)
     * and available individually here too. libretro/keyboard.c's KeyTable[]
     * maps each of these to the *same physical X68000 scancode* as its
     * unshifted digit/symbol (e.g. RETROK_EXCLAIM -> the '1' key's scancode),
     * so the guest only sees the shifted character if LSHIFT is held at the
     * same time - there is no separate "send shift+1" primitive, you hold
     * both keys like a real keyboard. */
    { "exclaim", RETROK_EXCLAIM, 0 }, { "quotedbl", RETROK_QUOTEDBL, 0 }, { "hash", RETROK_HASH, 0 },
    { "dollar", RETROK_DOLLAR, 0 }, { "ampersand", RETROK_AMPERSAND, 0 },
    { "leftparen", RETROK_LEFTPAREN, 0 }, { "rightparen", RETROK_RIGHTPAREN, 0 },
    { "asterisk", RETROK_ASTERISK, 0 }, { "plus", RETROK_PLUS, 0 },
    { "colon", RETROK_COLON, 0 }, { "less", RETROK_LESS, 0 }, { "greater", RETROK_GREATER, 0 },
    { "question", RETROK_QUESTION, 0 }, { "at", RETROK_AT, 0 }, { "caret", RETROK_CARET, 0 },
    { "underscore", RETROK_UNDERSCORE, 0 }, { "leftbrace", RETROK_LEFTBRACE, 0 },
    { "bar", RETROK_BAR, 0 }, { "rightbrace", RETROK_RIGHTBRACE, 0 }, { "tilde", RETROK_TILDE, 0 },
};

typedef struct {
    const char *name;
    unsigned id; /* RETRO_DEVICE_ID_JOYPAD_* */
} joypad_entry_t;

static const joypad_entry_t JOYPAD_TABLE[] = {
    { "b", RETRO_DEVICE_ID_JOYPAD_B }, { "y", RETRO_DEVICE_ID_JOYPAD_Y },
    { "select", RETRO_DEVICE_ID_JOYPAD_SELECT }, { "start", RETRO_DEVICE_ID_JOYPAD_START },
    { "up", RETRO_DEVICE_ID_JOYPAD_UP }, { "down", RETRO_DEVICE_ID_JOYPAD_DOWN },
    { "left", RETRO_DEVICE_ID_JOYPAD_LEFT }, { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
    { "a", RETRO_DEVICE_ID_JOYPAD_A }, { "x", RETRO_DEVICE_ID_JOYPAD_X },
    { "l", RETRO_DEVICE_ID_JOYPAD_L }, { "r", RETRO_DEVICE_ID_JOYPAD_R },
    { "l2", RETRO_DEVICE_ID_JOYPAD_L2 }, { "r2", RETRO_DEVICE_ID_JOYPAD_R2 },
    { "l3", RETRO_DEVICE_ID_JOYPAD_L3 }, { "r3", RETRO_DEVICE_ID_JOYPAD_R3 },
};

/* ASCII -> (RETROK_* code, needs LSHIFT held) for arcl_type.
 *
 * This table is empirically verified against the running core, not derived
 * from SDL1.2/US-keyboard convention - that convention turned out to be
 * wrong here. libretro/keyboard.c's KeyTable[] maps each RETROK_* code to
 * an X68000 scancode, and several "shifted" keysyms (RETROK_EXCLAIM,
 * RETROK_COLON, ...) share their scancode with an unrelated "unshifted"
 * keysym (RETROK_1, RETROK_SEMICOLON, ...) rather than being independently
 * reachable. Sending the named shifted keysym directly (e.g. RETROK_EXCLAIM
 * alone) does *not* produce the shifted character; sending shift held
 * together with the *unshifted* keysym does (e.g. LSHIFT+RETROK_1 -> '!').
 * This was confirmed character-by-character via arcl_screenshot during
 * implementation (see x68k_mcp.md 6.2/tools/_quick_shift_probe*.py).
 *
 * Also empirically required: shift must be held for one full frame before
 * the base key is pressed, not the same frame - see the comment in
 * handle_type() below.
 *
 * ':' '*' '^' '_' '~' have no confirmed working key combination (pressing
 * their RETROK_* code, shifted or not, produced no visible character in
 * testing) and are intentionally left unmapped rather than guessed. */
static int char_to_retrok(unsigned char c, unsigned *code, int *shift)
{
    if (c >= 'a' && c <= 'z') { *code = RETROK_a + (c - 'a'); *shift = 0; return 1; }
    if (c >= 'A' && c <= 'Z') { *code = RETROK_a + (c - 'A'); *shift = 1; return 1; }
    if (c >= '0' && c <= '9') { *code = RETROK_0 + (c - '0'); *shift = 0; return 1; }
    switch (c)
    {
    case ' ':  *code = RETROK_SPACE;    *shift = 0; return 1;
    case '\t': *code = RETROK_TAB;      *shift = 0; return 1;
    case '\n': case '\r': *code = RETROK_RETURN; *shift = 0; return 1;
    /* digit row, shifted (confirmed for !, ", '; #,$,&,(,) follow the same
     * row and mechanism so are trusted without individual confirmation) */
    case '!':  *code = RETROK_1; *shift = 1; return 1;
    case '"':  *code = RETROK_2; *shift = 1; return 1;
    case '#':  *code = RETROK_3; *shift = 1; return 1;
    case '$':  *code = RETROK_4; *shift = 1; return 1;
    case '%':  *code = RETROK_5; *shift = 1; return 1;
    case '&':  *code = RETROK_6; *shift = 1; return 1;
    case '\'': *code = RETROK_7; *shift = 1; return 1; /* confirmed */
    case '(':  *code = RETROK_8; *shift = 1; return 1;
    case ')':  *code = RETROK_9; *shift = 1; return 1;
    case ',':  *code = RETROK_COMMA;  *shift = 0; return 1; /* confirmed */
    case '<':  *code = RETROK_COMMA;  *shift = 1; return 1; /* confirmed */
    case '-':  *code = RETROK_MINUS;  *shift = 0; return 1; /* confirmed */
    case '=':  *code = RETROK_MINUS;  *shift = 1; return 1; /* confirmed */
    case '.':  *code = RETROK_PERIOD; *shift = 0; return 1; /* confirmed */
    case '>':  *code = RETROK_PERIOD; *shift = 1; return 1; /* confirmed */
    case '/':  *code = RETROK_SLASH;  *shift = 0; return 1; /* confirmed */
    case '?':  *code = RETROK_SLASH;  *shift = 1; return 1; /* confirmed */
    case ';':  *code = RETROK_SEMICOLON; *shift = 0; return 1; /* confirmed */
    case '+':  *code = RETROK_SEMICOLON; *shift = 1; return 1; /* confirmed */
    case ':':  *code = RETROK_COLON;     *shift = 0; return 1;
    case '*':  *code = RETROK_COLON;     *shift = 1; return 1;
    case '@':  *code = RETROK_BACKQUOTE; *shift = 0; return 1; /* confirmed */
    case '`':  *code = RETROK_CARET;     *shift = 1; return 1; /* confirmed (not a typo: see note above) */
    case '[':  *code = RETROK_LEFTBRACKET;  *shift = 0; return 1; /* confirmed */
    case '{':  *code = RETROK_LEFTBRACKET;  *shift = 1; return 1; /* confirmed */
    case '\\': *code = RETROK_BACKSLASH;    *shift = 0; return 1; /* confirmed (renders as \xa5 on this font) */
    case '|':  *code = RETROK_BACKSLASH;    *shift = 1; return 1; /* confirmed */
    case ']':  *code = RETROK_RIGHTBRACKET; *shift = 0; return 1; /* confirmed */
    case '}':  *code = RETROK_RIGHTBRACKET; *shift = 1; return 1; /* confirmed */
    default:   return 0; /* includes '^' '_' '~', see comment above */
    }
}

static int lookup_key(const char *name, const key_entry_t **out)
{
    size_t i;
    for (i = 0; i < sizeof(KEY_TABLE) / sizeof(KEY_TABLE[0]); i++)
    {
        if (strcmp(KEY_TABLE[i].name, name) == 0)
        {
            *out = &KEY_TABLE[i];
            return 1;
        }
    }
    return 0;
}

static int lookup_joypad(const char *name, unsigned *id)
{
    size_t i;
    for (i = 0; i < sizeof(JOYPAD_TABLE) / sizeof(JOYPAD_TABLE[0]); i++)
    {
        if (strcmp(JOYPAD_TABLE[i].name, name) == 0)
        {
            *id = JOYPAD_TABLE[i].id;
            return 1;
        }
    }
    return 0;
}

int arcl_l0_init(arcl_l0_t **out_l0, arcl_control_t *control)
{
    arcl_l0_t *l0;
    if (!out_l0 || !control)
        return 0;
    l0 = (arcl_l0_t *)calloc(1, sizeof(*l0));
    if (!l0)
        return 0;
    l0->control = control;
    *out_l0 = l0;
    return 1;
}

void arcl_l0_shutdown(arcl_l0_t *l0)
{
    free(l0);
}

static int handle_screenshot(arcl_l0_t *l0, const char *request_json,
                              char *result_json, size_t result_size,
                              char *error_message, size_t error_size)
{
    px68k_frame_t frame = px68k_frontend_get_frame();
    long x = 0, y = 0, w = 0, h = 0, scale = 1, grid = 0;
    int have_x = mcp_json_get_long_any(request_json, "x", &x);
    int have_y = mcp_json_get_long_any(request_json, "y", &y);
    int have_w = mcp_json_get_long_any(request_json, "w", &w);
    int have_h = mcp_json_get_long_any(request_json, "h", &h);
    int have_scale = mcp_json_get_long_any(request_json, "scale", &scale);
    int have_grid = mcp_json_get_long_any(request_json, "grid", &grid);
    char path[1024];
    int have_path = mcp_json_get_string_any(request_json, "path", path, sizeof(path));
    int want_inline = 1;
    mcp_json_get_bool_any(request_json, "inline", &want_inline);

    if (!frame.valid)
    {
        mcp_json_set_error(error_message, error_size, "no frame available yet; call arcl_run first");
        return 0;
    }
    if ((have_x || have_y || have_w || have_h) && !(have_x && have_y && have_w && have_h))
    {
        mcp_json_set_error(error_message, error_size, "specify all of x,y,w,h together, or none for the full frame");
        return 0;
    }
    if (!have_x)
    {
        x = 0; y = 0; w = frame.width; h = frame.height;
    }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        (unsigned long)x + (unsigned long)w > frame.width ||
        (unsigned long)y + (unsigned long)h > frame.height)
    {
        mcp_json_set_error(error_message, error_size, "crop rectangle out of bounds of the current frame");
        return 0;
    }
    if (!have_scale)
        scale = 1;
    if (scale < 1 || (unsigned long)scale > PX68K_MAX_SCALE)
    {
        mcp_json_set_error(error_message, error_size, "scale must be between 1 and 16");
        return 0;
    }
    if (have_grid && grid > 0 && scale < 2)
    {
        mcp_json_set_error(error_message, error_size, "grid requires scale >= 2");
        return 0;
    }
    if (!have_grid || grid < 0)
        grid = 0;
    if ((unsigned long long)(w * scale) * (unsigned long long)(h * scale) > PX68K_MAX_SCALED_PIXELS)
    {
        mcp_json_set_error(error_message, error_size, "scaled output too large; crop with x/y/w/h or reduce scale");
        return 0;
    }

    {
        unsigned ow = (unsigned)(w * scale);
        unsigned oh = (unsigned)(h * scale);
        uint16_t *crop = (uint16_t *)malloc((size_t)w * (size_t)h * sizeof(uint16_t));
        uint16_t *buf = (uint16_t *)malloc((size_t)ow * oh * sizeof(uint16_t));
        uint8_t *png_data = NULL;
        size_t png_size = 0;
        size_t used;

        if (!crop || !buf)
        {
            free(crop);
            free(buf);
            mcp_json_set_error(error_message, error_size, "out of memory");
            return 0;
        }
        /* Copies the crop's actual pixel bytes out while holding the frame
         * lock (frontend_core.c), unlike reading straight through
         * frame.pixels here would: that pointer is into the core's own
         * buffer, which a concurrent arcl_resume can overwrite in place on
         * its next run_frame()/video_cb mid-copy. Also re-validates bounds
         * against whatever frame is current *now*, in case one raced in
         * between the check above and this call. */
        if (!px68k_frontend_copy_frame_rect((unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h, crop))
        {
            free(crop);
            free(buf);
            mcp_json_set_error(error_message, error_size,
                "frame changed size while preparing the screenshot; try again");
            return 0;
        }
        for (unsigned oy = 0; oy < oh; oy++)
        {
            const uint16_t *srow = crop + (size_t)(oy / (unsigned)scale) * (unsigned)w;
            uint16_t *drow = buf + (size_t)oy * ow;
            for (unsigned ox = 0; ox < ow; ox++)
                drow[ox] = srow[ox / (unsigned)scale];
        }
        free(crop);
        if (grid > 0)
        {
            for (long gx = 0; gx <= w; gx += grid)
            {
                unsigned ox = (unsigned)(gx * scale);
                if (ox >= ow) break;
                for (unsigned oy = 0; oy < oh; oy++)
                    buf[(size_t)oy * ow + ox] = (uint16_t)PX68K_MAGENTA565;
            }
            for (long gy = 0; gy <= h; gy += grid)
            {
                unsigned oy = (unsigned)(gy * scale);
                if (oy >= oh) break;
                uint16_t *drow = buf + (size_t)oy * ow;
                for (unsigned ox = 0; ox < ow; ox++)
                    drow[ox] = (uint16_t)PX68K_MAGENTA565;
            }
        }

        if (!px68k_encode_png_rgb565(buf, ow, oh, ow, &png_data, &png_size))
        {
            free(buf);
            mcp_json_set_error(error_message, error_size, "PNG encoding failed");
            return 0;
        }

        if (have_path)
        {
            FILE *fp = fopen(path, "wb");
            if (!fp || fwrite(png_data, 1, png_size, fp) != png_size)
            {
                if (fp) fclose(fp);
                free(png_data);
                free(buf);
                mcp_json_set_error(error_message, error_size, "failed to write PNG to path");
                return 0;
            }
            fclose(fp);
        }

        if (have_path)
        {
            char quoted_path[1200];
            mcp_json_write_quoted(path, quoted_path, sizeof(quoted_path));
            used = (size_t)snprintf(result_json, result_size,
                "{\"source_width\":%u,\"source_height\":%u,\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld,"
                "\"scale\":%ld,\"width\":%u,\"height\":%u,\"path\":%s",
                frame.width, frame.height, x, y, w, h, scale, ow, oh, quoted_path);
        }
        else
        {
            used = (size_t)snprintf(result_json, result_size,
                "{\"source_width\":%u,\"source_height\":%u,\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld,"
                "\"scale\":%ld,\"width\":%u,\"height\":%u",
                frame.width, frame.height, x, y, w, h, scale, ow, oh);
        }

        if (want_inline && used < result_size)
        {
            size_t b64_needed = px68k_base64_encoded_size(png_size);
            char *b64 = (char *)malloc(b64_needed);
            if (b64 && px68k_base64_encode(png_data, png_size, b64, b64_needed))
            {
                int n = snprintf(result_json + used, result_size - used, ",\"image_png_base64\":\"%s\"", b64);
                if (n > 0)
                    used += (size_t)n;
            }
            free(b64);
        }

        free(png_data);
        free(buf);

        arcl_control_append_status(l0->control, result_json, result_size, used, -1);
        return 1;
    }
}

static int handle_key(arcl_l0_t *l0, const char *request_json,
                       char *result_json, size_t result_size,
                       char *error_message, size_t error_size)
{
    char key_name[64];
    char action[16];
    const key_entry_t *entry;
    long frames = 1;
    size_t used;

    if (!mcp_json_get_string_any(request_json, "key", key_name, sizeof(key_name)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'key'");
        return 0;
    }
    if (!mcp_json_get_string_any(request_json, "action", action, sizeof(action)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'action' (press/release/tap)");
        return 0;
    }
    if (!lookup_key(key_name, &entry))
    {
        mcp_json_set_error(error_message, error_size, "unmapped key name; see x68k_mcp.md for the supported key set");
        return 0;
    }
    if (entry->blocked)
    {
        mcp_json_set_error(error_message, error_size,
                     "this key is blocked from arcl_key: it triggers a core-internal side effect "
                     "(F12 opens the built-in menu, ScrollLock toggles MIDI); see x68k_mcp.md 6.1");
        return 0;
    }

    if (strcmp(action, "press") == 0)
    {
        px68k_frontend_set_key(entry->code, 1);
        used = (size_t)snprintf(result_json, result_size, "{\"key\":\"%s\",\"action\":\"press\"", key_name);
        arcl_control_append_status(l0->control, result_json, result_size, used, -1);
        return 1;
    }
    if (strcmp(action, "release") == 0)
    {
        px68k_frontend_set_key(entry->code, 0);
        used = (size_t)snprintf(result_json, result_size, "{\"key\":\"%s\",\"action\":\"release\"", key_name);
        arcl_control_append_status(l0->control, result_json, result_size, used, -1);
        return 1;
    }
    if (strcmp(action, "tap") == 0)
    {
        mcp_json_get_long_any(request_json, "frames", &frames);
        if (frames < 1 || frames > PX68K_MAX_TAP_FRAMES)
        {
            mcp_json_set_error(error_message, error_size, "frames must be between 1 and 3600");
            return 0;
        }
        px68k_frontend_set_key(entry->code, 1);
        arcl_control_run_frames(l0->control, frames);
        px68k_frontend_set_key(entry->code, 0);
        used = (size_t)snprintf(result_json, result_size, "{\"key\":\"%s\",\"action\":\"tap\"", key_name);
        arcl_control_append_status(l0->control, result_json, result_size, used, frames);
        return 1;
    }
    mcp_json_set_error(error_message, error_size, "action must be press, release, or tap");
    return 0;
}

static int handle_clear_input(arcl_l0_t *l0, char *result_json, size_t result_size)
{
    size_t used;
    px68k_frontend_clear_input();
    used = (size_t)snprintf(result_json, result_size, "{\"cleared\":true");
    arcl_control_append_status(l0->control, result_json, result_size, used, -1);
    return 1;
}

static int handle_type(arcl_l0_t *l0, const char *request_json,
                        char *result_json, size_t result_size,
                        char *error_message, size_t error_size)
{
    char text[PX68K_MAX_TYPE_CHARS + 1];
    long hold_frames = 2;
    size_t i, len;
    size_t used;
    long frames_used = 0;

    if (!mcp_json_get_string_any(request_json, "text", text, sizeof(text)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'text'");
        return 0;
    }
    mcp_json_get_long_any(request_json, "frames", &hold_frames);
    if (hold_frames < 1 || hold_frames > PX68K_MAX_TAP_FRAMES)
    {
        mcp_json_set_error(error_message, error_size, "frames must be between 1 and 3600");
        return 0;
    }
    len = strlen(text);

    /* Validate every character up front and fail atomically: partially
     * typing a command line because character 30 of 40 turned out to be
     * unmapped would be a worse failure mode than typing nothing at all
     * (arcl_input_macro is the tool that models "ran partway, stopped"). */
    for (i = 0; i < len; i++)
    {
        unsigned code;
        int shift;
        if (!char_to_retrok((unsigned char)text[i], &code, &shift))
        {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "character '%c' (0x%02x) at position %zu has no mapped key on this keyboard model",
                     (text[i] >= 0x20 && text[i] < 0x7f) ? text[i] : '?', (unsigned char)text[i], i);
            mcp_json_set_error(error_message, error_size, msg);
            return 0;
        }
    }

    for (i = 0; i < len; i++)
    {
        unsigned code;
        int shift;
        char_to_retrok((unsigned char)text[i], &code, &shift);
        /* Empirically required (verified by screenshot): pressing shift and
         * the base key in the *same* frame snapshot does not produce an
         * uppercase/shifted character - the guest's keyboard driver reads
         * the two key-down events as a sequence within the frame and sees
         * the base key before it has registered the shift. Holding shift
         * alone for a full frame first, then pressing the base key while
         * still holding shift, matches how a physical keyboard actually
         * presents this to guest software and is what works here. */
        if (shift)
        {
            px68k_frontend_set_key(RETROK_LSHIFT, 1);
            arcl_control_run_frames(l0->control, 1);
            frames_used += 1;
        }
        px68k_frontend_set_key(code, 1);
        arcl_control_run_frames(l0->control, hold_frames);
        frames_used += hold_frames;
        px68k_frontend_set_key(code, 0);
        if (shift)
            px68k_frontend_set_key(RETROK_LSHIFT, 0);
        arcl_control_run_frames(l0->control, 1); /* gap frame: guarantees a clean up-then-down for repeats */
        frames_used += 1;
    }

    used = (size_t)snprintf(result_json, result_size, "{\"chars_typed\":%zu", len);
    arcl_control_append_status(l0->control, result_json, result_size, used, frames_used);
    return 1;
}

static int handle_mouse(arcl_l0_t *l0, const char *request_json,
                         char *result_json, size_t result_size,
                         char *error_message, size_t error_size)
{
    char action[16];
    size_t used;

    if (!mcp_json_get_string_any(request_json, "action", action, sizeof(action)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'action' (move/press/release)");
        return 0;
    }

    if (strcmp(action, "move") == 0)
    {
        long dx = 0, dy = 0;
        mcp_json_get_long_any(request_json, "dx", &dx);
        mcp_json_get_long_any(request_json, "dy", &dy);
        px68k_frontend_move_mouse((int)dx, (int)dy);
        used = (size_t)snprintf(result_json, result_size, "{\"action\":\"move\",\"dx\":%ld,\"dy\":%ld", dx, dy);
        arcl_control_append_status(l0->control, result_json, result_size, used, -1);
        return 1;
    }
    if (strcmp(action, "press") == 0 || strcmp(action, "release") == 0)
    {
        char button[8];
        int pressed = strcmp(action, "press") == 0;
        if (!mcp_json_get_string_any(request_json, "button", button, sizeof(button)))
        {
            mcp_json_set_error(error_message, error_size, "missing required string field 'button' (left/right)");
            return 0;
        }
        if (strcmp(button, "left") == 0)
            px68k_frontend_set_mouse_left(pressed);
        else if (strcmp(button, "right") == 0)
            px68k_frontend_set_mouse_right(pressed);
        else
        {
            mcp_json_set_error(error_message, error_size, "button must be left or right");
            return 0;
        }
        used = (size_t)snprintf(result_json, result_size, "{\"action\":\"%s\",\"button\":\"%s\"", action, button);
        arcl_control_append_status(l0->control, result_json, result_size, used, -1);
        return 1;
    }
    mcp_json_set_error(error_message, error_size, "action must be move, press, or release");
    return 0;
}

static int handle_joypad(arcl_l0_t *l0, const char *request_json,
                          char *result_json, size_t result_size,
                          char *error_message, size_t error_size)
{
    char button_name[16];
    char action[16];
    long port = 0;
    long frames = 1;
    unsigned id;
    size_t used;

    if (!mcp_json_get_string_any(request_json, "button", button_name, sizeof(button_name)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'button'");
        return 0;
    }
    if (!mcp_json_get_string_any(request_json, "action", action, sizeof(action)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'action' (press/release/tap)");
        return 0;
    }
    mcp_json_get_long_any(request_json, "port", &port);
    if (port != 0 && port != 1)
    {
        mcp_json_set_error(error_message, error_size, "port must be 0 or 1");
        return 0;
    }
    if (!lookup_joypad(button_name, &id))
    {
        mcp_json_set_error(error_message, error_size, "unmapped joypad button name; see x68k_mcp.md for the supported set");
        return 0;
    }

    if (strcmp(action, "press") == 0)
    {
        px68k_frontend_set_joypad((unsigned)port, id, 1);
        used = (size_t)snprintf(result_json, result_size, "{\"port\":%ld,\"button\":\"%s\",\"action\":\"press\"", port, button_name);
        arcl_control_append_status(l0->control, result_json, result_size, used, -1);
        return 1;
    }
    if (strcmp(action, "release") == 0)
    {
        px68k_frontend_set_joypad((unsigned)port, id, 0);
        used = (size_t)snprintf(result_json, result_size, "{\"port\":%ld,\"button\":\"%s\",\"action\":\"release\"", port, button_name);
        arcl_control_append_status(l0->control, result_json, result_size, used, -1);
        return 1;
    }
    if (strcmp(action, "tap") == 0)
    {
        mcp_json_get_long_any(request_json, "frames", &frames);
        if (frames < 1 || frames > PX68K_MAX_TAP_FRAMES)
        {
            mcp_json_set_error(error_message, error_size, "frames must be between 1 and 3600");
            return 0;
        }
        px68k_frontend_set_joypad((unsigned)port, id, 1);
        arcl_control_run_frames(l0->control, frames);
        px68k_frontend_set_joypad((unsigned)port, id, 0);
        used = (size_t)snprintf(result_json, result_size, "{\"port\":%ld,\"button\":\"%s\",\"action\":\"tap\"", port, button_name);
        arcl_control_append_status(l0->control, result_json, result_size, used, frames);
        return 1;
    }
    mcp_json_set_error(error_message, error_size, "action must be press, release, or tap");
    return 0;
}

static int handle_input_state(arcl_l0_t *l0, char *result_json, size_t result_size)
{
    size_t used;
    size_t i;
    int first;
    int left, right, pending_dx, pending_dy;

    used = 0;
    mcp_json_appendf(result_json, result_size, &used, "{\"keys\":[");
    first = 1;
    for (i = 0; i < sizeof(KEY_TABLE) / sizeof(KEY_TABLE[0]); i++)
    {
        if (px68k_frontend_is_key_pressed(KEY_TABLE[i].code))
        {
            mcp_json_appendf(result_json, result_size, &used, "%s\"%s\"", first ? "" : ",", KEY_TABLE[i].name);
            first = 0;
        }
    }
    mcp_json_appendf(result_json, result_size, &used, "],\"joypad\":[");
    first = 1;
    for (int port = 0; port < 2; port++)
        for (i = 0; i < sizeof(JOYPAD_TABLE) / sizeof(JOYPAD_TABLE[0]); i++)
        {
            if (px68k_frontend_get_joypad((unsigned)port, JOYPAD_TABLE[i].id))
            {
                mcp_json_appendf(result_json, result_size, &used, "%s{\"port\":%d,\"button\":\"%s\"}",
                                  first ? "" : ",", port, JOYPAD_TABLE[i].name);
                first = 0;
            }
        }
    px68k_frontend_get_mouse_state(&left, &right, &pending_dx, &pending_dy);
    mcp_json_appendf(result_json, result_size, &used,
        "],\"mouse\":{\"left\":%s,\"right\":%s,\"pending_dx\":%d,\"pending_dy\":%d}",
        left ? "true" : "false", right ? "true" : "false", pending_dx, pending_dy);

    arcl_control_append_status(l0->control, result_json, result_size, used, -1);
    return 1;
}

/* arcl_input_macro: executes each step through the same handlers the
 * single-tool calls use (arcl_common_spec.md 7.7: a composite tool must be
 * decomposable into the single tools it wraps). Stops at the first failed
 * step per 4.5 - the whole point of a macro is that input order matters,
 * so a partially-applied prefix is reported rather than silently skipping
 * or rolling back. */
static int handle_macro(arcl_l0_t *l0, const char *request_json,
                         char *result_json, size_t result_size,
                         char *error_message, size_t error_size)
{
    const char *p = request_json;
    const char *steps_start;
    long total_frames_used = 0;
    size_t step_count = 0;
    size_t used;
    int overall_ok = 1;
    char step_error[256];

    steps_start = strstr(p, "\"steps\"");
    if (!steps_start)
    {
        mcp_json_set_error(error_message, error_size, "missing required array field 'steps'");
        return 0;
    }
    steps_start = strchr(steps_start, '[');
    if (!steps_start)
    {
        mcp_json_set_error(error_message, error_size, "'steps' must be an array");
        return 0;
    }

    used = 0;
    mcp_json_appendf(result_json, result_size, &used, "{\"results\":[");

    {
        const char *cursor = steps_start + 1;
        for (;;)
        {
            char op[24];
            /* Sized like a standalone tool call's own result buffer
             * (mcp_server.c's MCP_RESULT_MAX), not some smaller ad hoc
             * constant: an "op":"screenshot" step can carry an inline
             * base64 PNG as large as any standalone arcl_screenshot
             * response (up to ~1.92MB for the largest allowed crop, see
             * PX68K_MAX_SCALED_PIXELS above). A too-small buffer here
             * wouldn't fail loudly - handle_screenshot() would just
             * silently truncate into invalid JSON, which then gets
             * embedded verbatim (unescaped) into this tool's own response
             * below, corrupting the whole tools/call reply. */
            char *sub_result;
            char sub_error[256];
            int depth;
            const char *step_start;
            const char *step_end;
            int step_ok;

            while (*cursor && (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t' || *cursor == ','))
                cursor++;
            if (*cursor == ']' || !*cursor)
                break;
            if (*cursor != '{')
            {
                mcp_json_set_error(error_message, error_size, "each step must be a JSON object");
                return 0;
            }
            if (step_count >= PX68K_MAX_MACRO_STEPS)
            {
                mcp_json_set_error(error_message, error_size, "too many steps; max is 16 per arcl_input_macro call");
                return 0;
            }

            step_start = cursor;
            depth = 0;
            do
            {
                if (*cursor == '"')
                {
                    cursor++;
                    while (*cursor && *cursor != '"')
                        cursor += (*cursor == '\\' && cursor[1]) ? 2 : 1;
                    if (*cursor) cursor++;
                    continue;
                }
                if (*cursor == '{') depth++;
                else if (*cursor == '}') depth--;
                cursor++;
            } while (depth > 0 && *cursor);
            step_end = cursor;

            {
                size_t step_len = (size_t)(step_end - step_start);
                char *step_json = (char *)malloc(step_len + 1);
                if (!step_json)
                {
                    mcp_json_set_error(error_message, error_size, "out of memory");
                    return 0;
                }
                memcpy(step_json, step_start, step_len);
                step_json[step_len] = '\0';

                if (!mcp_json_get_string_any(step_json, "op", op, sizeof(op)))
                {
                    mcp_json_set_error(error_message, error_size, "each step needs a string 'op' field");
                    free(step_json);
                    return 0;
                }

                sub_result = (char *)malloc(PX68K_MACRO_STEP_RESULT_MAX);
                if (!sub_result)
                {
                    mcp_json_set_error(error_message, error_size, "out of memory");
                    free(step_json);
                    return 0;
                }

                sub_error[0] = '\0';
                if (strcmp(op, "key") == 0)
                    step_ok = handle_key(l0, step_json, sub_result, PX68K_MACRO_STEP_RESULT_MAX, sub_error, sizeof(sub_error));
                else if (strcmp(op, "type") == 0)
                    step_ok = handle_type(l0, step_json, sub_result, PX68K_MACRO_STEP_RESULT_MAX, sub_error, sizeof(sub_error));
                else if (strcmp(op, "mouse") == 0)
                    step_ok = handle_mouse(l0, step_json, sub_result, PX68K_MACRO_STEP_RESULT_MAX, sub_error, sizeof(sub_error));
                else if (strcmp(op, "joypad") == 0)
                    step_ok = handle_joypad(l0, step_json, sub_result, PX68K_MACRO_STEP_RESULT_MAX, sub_error, sizeof(sub_error));
                else if (strcmp(op, "clear_input") == 0)
                    step_ok = handle_clear_input(l0, sub_result, PX68K_MACRO_STEP_RESULT_MAX);
                else if (strcmp(op, "screenshot") == 0)
                    step_ok = handle_screenshot(l0, step_json, sub_result, PX68K_MACRO_STEP_RESULT_MAX, sub_error, sizeof(sub_error));
                else if (strcmp(op, "run") == 0)
                {
                    long frames = 1;
                    mcp_json_get_long_any(step_json, "frames", &frames);
                    if (frames < 1 || frames > PX68K_MAX_MACRO_TOTAL_FRAMES)
                    {
                        strncpy(sub_error, "frames must be between 1 and 10000", sizeof(sub_error) - 1);
                        step_ok = 0;
                    }
                    else if (total_frames_used + frames > PX68K_MAX_MACRO_TOTAL_FRAMES)
                    {
                        strncpy(sub_error, "macro would exceed the 10000 total-frame budget", sizeof(sub_error) - 1);
                        step_ok = 0;
                    }
                    else
                    {
                        size_t n = 0;
                        arcl_control_run_frames(l0->control, frames);
                        mcp_json_appendf(sub_result, PX68K_MACRO_STEP_RESULT_MAX, &n, "{\"op\":\"run\"");
                        arcl_control_append_status(l0->control, sub_result, PX68K_MACRO_STEP_RESULT_MAX, n, frames);
                        total_frames_used += frames;
                        step_ok = 1;
                    }
                }
                else
                {
                    strncpy(sub_error, "unknown macro step op", sizeof(sub_error) - 1);
                    step_ok = 0;
                }

                free(step_json);
            }

            mcp_json_appendf(result_json, result_size, &used, "%s{\"op\":\"%s\",\"ok\":%s,\"result\":%s}",
                              step_count == 0 ? "" : ",", op, step_ok ? "true" : "false",
                              step_ok ? sub_result : "null");
            free(sub_result);
            step_count++;

            if (!step_ok)
            {
                strncpy(step_error, sub_error[0] ? sub_error : "step failed", sizeof(step_error) - 1);
                overall_ok = 0;
                break;
            }
        }
    }

    mcp_json_appendf(result_json, result_size, &used,
        "],\"steps_completed\":%zu,\"steps_ok\":%s", step_count, overall_ok ? "true" : "false");
    arcl_control_append_status(l0->control, result_json, result_size, used, -1);

    if (!overall_ok)
    {
        /* result_json is fully populated (steps_completed, results[], ...)
         * even though this returns failure: mcp_server.c's send_tool_error
         * attaches it as structuredContent alongside isError:true, so
         * 4.5's "show how far it got" requirement is met without having to
         * report a partial macro as a false success. */
        mcp_json_set_error(error_message, error_size, step_error);
        return 0;
    }
    return 1;
}

/* start/stop/status around frontend_core's independent audio capture
 * buffer (frontend_core.h) - x68k_mcp.md 6.4/Phase4. Typical use: start,
 * arcl_run some frames, stop (optionally with `path` to save a WAV).
 * Peak/RMS are computed over the whole captured clip, combining both
 * stereo channels into one flat sample stream. */
static int handle_audio_record(arcl_l0_t *l0, const char *request_json,
                                char *result_json, size_t result_size,
                                char *error_message, size_t error_size)
{
    char action[16];
    size_t used;

    if (!mcp_json_get_string_any(request_json, "action", action, sizeof(action)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'action' (start/stop/status)");
        return 0;
    }

    if (strcmp(action, "start") == 0)
    {
        px68k_frontend_audio_capture_start();
        used = (size_t)snprintf(result_json, result_size,
                                 "{\"action\":\"start\",\"sample_rate\":%d,\"cap_seconds\":%d",
                                 PX68K_AUDIO_SAMPLE_RATE, PX68K_AUDIO_CAP_SECONDS);
        arcl_control_append_status(l0->control, result_json, result_size, used, -1);
        return 1;
    }

    if (strcmp(action, "status") == 0)
    {
        int active, truncated;
        size_t frames;
        px68k_frontend_audio_capture_status(&active, &frames, &truncated);
        used = (size_t)snprintf(result_json, result_size,
            "{\"action\":\"status\",\"active\":%s,\"frames\":%zu,\"seconds\":%.3f,\"truncated\":%s",
            active ? "true" : "false", frames, (double)frames / PX68K_AUDIO_SAMPLE_RATE,
            truncated ? "true" : "false");
        arcl_control_append_status(l0->control, result_json, result_size, used, -1);
        return 1;
    }

    if (strcmp(action, "stop") == 0)
    {
        int16_t *data = NULL;
        size_t frames = 0;
        int truncated = 0;
        char path[1024];
        int have_path = mcp_json_get_string_any(request_json, "path", path, sizeof(path));
        int want_inline = 0;
        mcp_json_get_bool_any(request_json, "inline", &want_inline);

        px68k_frontend_audio_capture_stop(&data, &frames, &truncated);

        {
            size_t i;
            long peak = 0;
            double sum_sq = 0.0;
            for (i = 0; i < frames * 2; i++)
            {
                long v = data ? data[i] : 0;
                long a = v < 0 ? -v : v;
                if (a > peak) peak = a;
                sum_sq += (double)v * (double)v;
            }
            {
                double rms = frames > 0 ? sqrt(sum_sq / (double)(frames * 2)) : 0.0;
                int silent = peak < PX68K_AUDIO_SILENCE_PEAK;

                uint8_t *wav_data = NULL;
                size_t wav_size = 0;
                int have_wav = (data && frames > 0 &&
                                 px68k_encode_wav_s16_stereo(data, frames, PX68K_AUDIO_SAMPLE_RATE, &wav_data, &wav_size));

                if (have_path)
                {
                    if (!have_wav)
                    {
                        free(data);
                        mcp_json_set_error(error_message, error_size, "failed to encode WAV");
                        return 0;
                    }
                    {
                        FILE *fp = fopen(path, "wb");
                        if (!fp || fwrite(wav_data, 1, wav_size, fp) != wav_size)
                        {
                            if (fp) fclose(fp);
                            free(wav_data);
                            free(data);
                            mcp_json_set_error(error_message, error_size, "failed to write WAV to path");
                            return 0;
                        }
                        fclose(fp);
                    }
                }

                used = (size_t)snprintf(result_json, result_size,
                    "{\"action\":\"stop\",\"frames\":%zu,\"seconds\":%.3f,\"truncated\":%s,"
                    "\"peak\":%ld,\"rms\":%.2f,\"silent\":%s",
                    frames, (double)frames / PX68K_AUDIO_SAMPLE_RATE, truncated ? "true" : "false",
                    peak, rms, silent ? "true" : "false");
                if (have_path)
                {
                    char quoted_path[1200];
                    mcp_json_write_quoted(path, quoted_path, sizeof(quoted_path));
                    used += (size_t)snprintf(result_json + used, result_size - used, ",\"path\":%s", quoted_path);
                }
                if (want_inline && have_wav && used < result_size)
                {
                    size_t b64_needed = px68k_base64_encoded_size(wav_size);
                    char *b64 = (char *)malloc(b64_needed);
                    if (b64 && px68k_base64_encode(wav_data, wav_size, b64, b64_needed))
                    {
                        int n = snprintf(result_json + used, result_size - used, ",\"audio_wav_base64\":\"%s\"", b64);
                        if (n > 0) used += (size_t)n;
                    }
                    free(b64);
                }
                free(wav_data);
            }
        }
        free(data);
        arcl_control_append_status(l0->control, result_json, result_size, used, -1);
        return 1;
    }

    mcp_json_set_error(error_message, error_size, "action must be start, stop, or status");
    return 0;
}

int arcl_l0_call(const char *name, const char *request_json, void *userdata,
                  char *result_json, size_t result_size,
                  char *error_message, size_t error_size)
{
    arcl_l0_t *l0 = (arcl_l0_t *)userdata;
    if (!l0 || !name || !result_json || !error_message)
        return 0;
    error_message[0] = '\0';

    if (strcmp(name, "arcl_screenshot") == 0)
        return handle_screenshot(l0, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_key") == 0)
        return handle_key(l0, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_clear_input") == 0)
        return handle_clear_input(l0, result_json, result_size);
    if (strcmp(name, "arcl_type") == 0)
        return handle_type(l0, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_mouse") == 0)
        return handle_mouse(l0, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_joypad") == 0)
        return handle_joypad(l0, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_input_state") == 0)
        return handle_input_state(l0, result_json, result_size);
    if (strcmp(name, "arcl_input_macro") == 0)
        return handle_macro(l0, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_audio_record") == 0)
        return handle_audio_record(l0, request_json, result_json, result_size, error_message, error_size);

    mcp_json_set_error(error_message, error_size, "Unknown tool");
    return 0;
}
