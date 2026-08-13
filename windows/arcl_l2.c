/* L2 tools: arcl_registers, arcl_write_registers, arcl_read_mem,
 * arcl_write_mem, arcl_breakpoint (incl. memory watchpoint), arcl_stack,
 * arcl_step, arcl_disasm (x68k_mcp.md 6.3, 0.1.1).
 *
 * Registers go through m68000_get_reg()/m68000_set_reg() (m68000/m68000.h)
 * - an existing, backend-agnostic, unmodified core API, not something this
 * project added. Memory goes through cpu_readmem24/cpu_writemem24 (and
 * their _word/_dword variants, x68k/x68kmemory.h) - also existing and
 * unmodified; these are the same
 * CPU-side entry points that ultimately reach rm_main()/wm_cnt() in
 * mem_wrap.c, so a read/write here through this file will itself trip any
 * watchpoint set on that address, same as a real CPU access would.
 * Breakpoints/watchpoints are this project's own arcl_watchpoint.c state;
 * this file is just the MCP-facing wrapper around it. */
#include "arcl_l2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "arcl_watchpoint.h"
#include "m68k_disasm.h"
#include "mcp_json.h"

#include "m68000.h"     /* M68K_* register enum, m68000_get_reg/set_reg */
#include "x68kmemory.h" /* cpu_readmem24, cpu_writemem24, and _word/_dword variants */

struct arcl_l2 {
    arcl_control_t *control;
};

typedef struct {
    const char *name;
    int regnum;
} reg_field_t;

static const reg_field_t REG_FIELDS[] = {
    { "pc", M68K_PC }, { "sr", M68K_SR }, { "sp", M68K_SP }, { "usp", M68K_USP },
    { "isp", M68K_ISP }, { "msp", M68K_MSP }, { "vbr", M68K_VBR },
    { "sfc", M68K_SFC }, { "dfc", M68K_DFC }, { "cacr", M68K_CACR }, { "caar", M68K_CAAR },
    { "pref_addr", M68K_PREF_ADDR }, { "pref_data", M68K_PREF_DATA },
    { "d0", M68K_D0 }, { "d1", M68K_D1 }, { "d2", M68K_D2 }, { "d3", M68K_D3 },
    { "d4", M68K_D4 }, { "d5", M68K_D5 }, { "d6", M68K_D6 }, { "d7", M68K_D7 },
    { "a0", M68K_A0 }, { "a1", M68K_A1 }, { "a2", M68K_A2 }, { "a3", M68K_A3 },
    { "a4", M68K_A4 }, { "a5", M68K_A5 }, { "a6", M68K_A6 }, { "a7", M68K_A7 }
};
#define REG_FIELD_COUNT (sizeof(REG_FIELDS) / sizeof(REG_FIELDS[0]))

#define ARCL_READ_MEM_MAX 65536
#define ARCL_WRITE_MEM_MAX_BYTES 8192

static const char *watch_type_to_string(int type)
{
    switch (type)
    {
    case ARCL_WATCH_EXEC: return "exec";
    case ARCL_WATCH_READ: return "read";
    case ARCL_WATCH_WRITE: return "write";
    default: return "access";
    }
}

static int watch_type_from_string(const char *s)
{
    if (strcmp(s, "exec") == 0) return ARCL_WATCH_EXEC;
    if (strcmp(s, "read") == 0) return ARCL_WATCH_READ;
    if (strcmp(s, "write") == 0) return ARCL_WATCH_WRITE;
    if (strcmp(s, "access") == 0) return ARCL_WATCH_ACCESS;
    return -1;
}

static void append_registers_json(char *out, size_t cap, size_t *used)
{
    size_t i;
    for (i = 0; i < REG_FIELD_COUNT; i++)
        mcp_json_appendf(out, cap, used, "%s\"%s\":\"0x%x\"",
                          i ? "," : "", REG_FIELDS[i].name, (unsigned)m68000_get_reg(REG_FIELDS[i].regnum));
}

static int handle_registers(arcl_l2_t *l2, char *result_json, size_t result_size)
{
    size_t used = 0;
    mcp_json_appendf(result_json, result_size, &used, "{");
    arcl_control_lock(l2->control); /* m68000_get_reg() isn't self-synchronized against arcl_resume */
    append_registers_json(result_json, result_size, &used);
    arcl_control_unlock(l2->control);
    arcl_control_append_status(l2->control, result_json, result_size, used, -1);
    return 1;
}

static int handle_write_registers(arcl_l2_t *l2, const char *request_json,
                                   char *result_json, size_t result_size,
                                   char *error_message, size_t error_size)
{
    size_t i;
    int wrote_any = 0;
    size_t used = 0;
    uint32_t values[REG_FIELD_COUNT];
    int have_value[REG_FIELD_COUNT];

    for (i = 0; i < REG_FIELD_COUNT; i++)
        have_value[i] = mcp_json_get_hex_or_int_any(request_json, REG_FIELDS[i].name, &values[i]);

    mcp_json_appendf(result_json, result_size, &used, "{");
    /* Held across both the writes and the read-back so the response
     * reflects a consistent snapshot, not one torn by a concurrent
     * arcl_resume frame landing in between. */
    arcl_control_lock(l2->control);
    for (i = 0; i < REG_FIELD_COUNT; i++)
    {
        if (have_value[i])
        {
            m68000_set_reg(REG_FIELDS[i].regnum, values[i]);
            wrote_any = 1;
        }
    }
    if (wrote_any)
        append_registers_json(result_json, result_size, &used);
    arcl_control_unlock(l2->control);

    if (!wrote_any)
    {
        mcp_json_set_error(error_message, error_size,
                    "no recognized register fields in request (e.g. \"pc\", \"d0\", \"a7\")");
        return 0;
    }
    arcl_control_append_status(l2->control, result_json, result_size, used, -1);
    return 1;
}

static int handle_read_mem(arcl_l2_t *l2, const char *request_json,
                            char *result_json, size_t result_size,
                            char *error_message, size_t error_size)
{
    uint32_t address;
    long length;
    char *hexbuf;
    size_t i, used;

    if (!mcp_json_get_hex_or_int_any(request_json, "address", &address))
    {
        mcp_json_set_error(error_message, error_size, "missing required field 'address' (integer or hex string)");
        return 0;
    }
    if (!mcp_json_get_long_any(request_json, "length", &length))
        length = 16;
    if (length < 1 || length > ARCL_READ_MEM_MAX)
    {
        mcp_json_set_error(error_message, error_size, "length must be between 1 and 65536");
        return 0;
    }

    hexbuf = (char *)malloc((size_t)length * 2 + 1);
    if (!hexbuf)
    {
        mcp_json_set_error(error_message, error_size, "out of memory");
        return 0;
    }
    arcl_control_lock(l2->control); /* cpu_readmem24() isn't self-synchronized against arcl_resume */
    for (i = 0; i < (size_t)length; i++)
    {
        uint8_t v = (uint8_t)cpu_readmem24(address + (uint32_t)i);
        snprintf(hexbuf + i * 2, 3, "%02x", v);
    }
    arcl_control_unlock(l2->control);
    used = (size_t)snprintf(result_json, result_size, "{\"address\":\"0x%x\",\"length\":%ld,\"data\":\"%s\"",
                             (unsigned)address, length, hexbuf);
    free(hexbuf);
    arcl_control_append_status(l2->control, result_json, result_size, used, -1);
    return 1;
}

static int handle_write_mem(arcl_l2_t *l2, const char *request_json,
                             char *result_json, size_t result_size,
                             char *error_message, size_t error_size)
{
    uint32_t address;
    char hexdata[ARCL_WRITE_MEM_MAX_BYTES * 2 + 1];
    size_t hexlen, nbytes, i, used;

    if (!mcp_json_get_hex_or_int_any(request_json, "address", &address))
    {
        mcp_json_set_error(error_message, error_size, "missing required field 'address' (integer or hex string)");
        return 0;
    }
    if (!mcp_json_get_string_any(request_json, "data", hexdata, sizeof(hexdata)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'data' (hex bytes, e.g. \"deadbeef\")");
        return 0;
    }
    hexlen = strlen(hexdata);
    if (hexlen == 0 || (hexlen % 2) != 0)
    {
        mcp_json_set_error(error_message, error_size, "'data' must be a non-empty even-length hex string");
        return 0;
    }
    for (i = 0; i < hexlen; i++)
    {
        if (!isxdigit((unsigned char)hexdata[i]))
        {
            mcp_json_set_error(error_message, error_size, "'data' contains a non-hex-digit character");
            return 0;
        }
    }

    nbytes = hexlen / 2;
    arcl_control_lock(l2->control); /* cpu_writemem24() isn't self-synchronized against arcl_resume */
    for (i = 0; i < nbytes; i++)
    {
        unsigned byte;
        sscanf(hexdata + i * 2, "%2x", &byte);
        cpu_writemem24(address + (uint32_t)i, (uint32_t)byte);
    }
    arcl_control_unlock(l2->control);
    used = (size_t)snprintf(result_json, result_size, "{\"address\":\"0x%x\",\"length\":%zu",
                             (unsigned)address, nbytes);
    arcl_control_append_status(l2->control, result_json, result_size, used, -1);
    return 1;
}

static int handle_breakpoint(arcl_l2_t *l2, const char *request_json,
                              char *result_json, size_t result_size,
                              char *error_message, size_t error_size)
{
    char action[16];
    size_t used;

    if (!mcp_json_get_string_any(request_json, "action", action, sizeof(action)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'action' (add/remove/clear/list)");
        return 0;
    }

    if (strcmp(action, "add") == 0)
    {
        char typestr[16];
        int type;
        uint32_t address;
        long length;
        int id;

        if (!mcp_json_get_string_any(request_json, "type", typestr, sizeof(typestr)))
        {
            mcp_json_set_error(error_message, error_size, "add requires string field 'type' (exec/read/write/access)");
            return 0;
        }
        type = watch_type_from_string(typestr);
        if (type < 0)
        {
            mcp_json_set_error(error_message, error_size, "'type' must be exec, read, write, or access");
            return 0;
        }
        if (!mcp_json_get_hex_or_int_any(request_json, "address", &address))
        {
            mcp_json_set_error(error_message, error_size, "add requires field 'address' (integer or hex string)");
            return 0;
        }
        if (!mcp_json_get_long_any(request_json, "length", &length))
            length = 1;
        if (length < 1 || length > 0x1000000L)
        {
            mcp_json_set_error(error_message, error_size, "length must be between 1 and 16777216");
            return 0;
        }
        id = arcl_watchpoint_add(type, address, (uint32_t)length);
        if (id < 0)
        {
            mcp_json_set_error(error_message, error_size, "breakpoint table is full (max 8 - remove or clear one first)");
            return 0;
        }
        used = (size_t)snprintf(result_json, result_size,
                                 "{\"action\":\"add\",\"id\":%d,\"type\":\"%s\",\"address\":\"0x%x\",\"length\":%ld",
                                 id, watch_type_to_string(type), (unsigned)address, length);
        arcl_control_append_status(l2->control, result_json, result_size, used, -1);
        return 1;
    }

    if (strcmp(action, "remove") == 0)
    {
        /* Request field is "bp_id", not "id" - historically because
         * mcp_json_get_long_any() scanned the whole raw JSON-RPC line
         * (mcp_server.c passed it through unparsed), and every request
         * already had its own top-level "id" (the JSON-RPC message id).
         * mcp_server.c now scopes tool handlers to just params.arguments,
         * so a plain "id" argument would no longer collide - but "bp_id"
         * is kept as-is since it's already the documented field name
         * (main.c's tool schema) and existing callers depend on it. */
        long id;
        if (!mcp_json_get_long_any(request_json, "bp_id", &id))
        {
            mcp_json_set_error(error_message, error_size, "remove requires integer field 'bp_id'");
            return 0;
        }
        if (arcl_watchpoint_remove((int)id) != 0)
        {
            mcp_json_set_error(error_message, error_size, "no breakpoint with that id");
            return 0;
        }
        used = (size_t)snprintf(result_json, result_size, "{\"action\":\"remove\",\"id\":%ld", id);
        arcl_control_append_status(l2->control, result_json, result_size, used, -1);
        return 1;
    }

    if (strcmp(action, "clear") == 0)
    {
        arcl_watchpoint_clear();
        used = (size_t)snprintf(result_json, result_size, "{\"action\":\"clear\"");
        arcl_control_append_status(l2->control, result_json, result_size, used, -1);
        return 1;
    }

    if (strcmp(action, "list") == 0)
    {
        arcl_watch_info_t list[ARCL_WATCH_MAX];
        int n = arcl_watchpoint_list(list, ARCL_WATCH_MAX);
        int i;
        used = 0;
        mcp_json_appendf(result_json, result_size, &used, "{\"action\":\"list\",\"breakpoints\":[");
        for (i = 0; i < n; i++)
            mcp_json_appendf(result_json, result_size, &used,
                              "%s{\"id\":%d,\"type\":\"%s\",\"address\":\"0x%x\",\"length\":%u}",
                              i ? "," : "", list[i].id, watch_type_to_string(list[i].type),
                              (unsigned)list[i].address, (unsigned)list[i].length);
        mcp_json_appendf(result_json, result_size, &used, "]");
        arcl_control_append_status(l2->control, result_json, result_size, used, -1);
        return 1;
    }

    mcp_json_set_error(error_message, error_size, "action must be add, remove, clear, or list");
    return 0;
}

static int handle_stack(arcl_l2_t *l2, const char *request_json,
                         char *result_json, size_t result_size,
                         char *error_message, size_t error_size)
{
    long count, i;
    uint32_t sp;
    size_t used;

    if (!mcp_json_get_long_any(request_json, "count", &count))
        count = 16;
    if (count < 1 || count > 256)
    {
        mcp_json_set_error(error_message, error_size, "count must be between 1 and 256");
        return 0;
    }
    /* M68K_SP is unimplemented on this project's C68K backend (falls
     * through to m68000.c's "default: return 0"); M68K_A7 is what's
     * actually live there - on the 68000, A7 *is* the current stack
     * pointer (USP or SSP, transparently swapped by supervisor mode), not
     * a separate register, so this is the correct value either way, not a
     * workaround. */
    used = 0;
    arcl_control_lock(l2->control); /* m68000_get_reg()/cpu_readmem24_dword() aren't self-synchronized against arcl_resume */
    sp = m68000_get_reg(M68K_A7);
    mcp_json_appendf(result_json, result_size, &used, "{\"sp\":\"0x%x\",\"words\":[", (unsigned)sp);
    for (i = 0; i < count; i++)
    {
        uint32_t a = sp + (uint32_t)(i * 4);
        uint32_t v = cpu_readmem24_dword(a);
        mcp_json_appendf(result_json, result_size, &used,
                          "%s{\"address\":\"0x%x\",\"value\":\"0x%x\"}",
                          i ? "," : "", (unsigned)a, (unsigned)v);
    }
    arcl_control_unlock(l2->control);
    mcp_json_appendf(result_json, result_size, &used, "]");
    arcl_control_append_status(l2->control, result_json, result_size, used, -1);
    return 1;
}

static int handle_disasm(arcl_l2_t *l2, const char *request_json,
                          char *result_json, size_t result_size,
                          char *error_message, size_t error_size)
{
    uint32_t address;
    long count, i;
    size_t used;

    if (!mcp_json_get_hex_or_int_any(request_json, "address", &address))
    {
        mcp_json_set_error(error_message, error_size, "missing required field 'address' (integer or hex string)");
        return 0;
    }
    if (!mcp_json_get_long_any(request_json, "count", &count))
        count = 8;
    if (count < 1 || count > 256)
    {
        mcp_json_set_error(error_message, error_size, "count must be between 1 and 256");
        return 0;
    }
    used = 0;
    mcp_json_appendf(result_json, result_size, &used, "{\"instructions\":[");
    arcl_control_lock(l2->control); /* m68k_disasm_one() reads live memory, not self-synchronized against arcl_resume */
    for (i = 0; i < count; i++)
    {
        char text[64];
        char quoted[140];
        uint32_t len = m68k_disasm_one(address, text, sizeof(text));
        mcp_json_write_quoted(text, quoted, sizeof(quoted));
        mcp_json_appendf(result_json, result_size, &used,
                          "%s{\"address\":\"0x%x\",\"length\":%u,\"text\":%s}",
                          i ? "," : "", (unsigned)address, (unsigned)len, quoted);
        address += len;
    }
    arcl_control_unlock(l2->control);
    mcp_json_appendf(result_json, result_size, &used, "]");
    arcl_control_append_status(l2->control, result_json, result_size, used, -1);
    return 1;
}

/* arcl_watchpoint_init()/_shutdown() are NOT called here - the watchpoint
 * hook in mem_wrap.c is compiled into the core unconditionally and fires
 * in interactive (non-MCP) launches too, so its lifecycle is owned by
 * main() (right after px68k_frontend_init(), before either mode branches)
 * rather than being tied to whether L2 tools happen to be reachable. */
int arcl_l2_init(arcl_l2_t **out_l2, arcl_control_t *control)
{
    arcl_l2_t *l2;
    if (!out_l2 || !control)
        return 0;
    l2 = (arcl_l2_t *)calloc(1, sizeof(*l2));
    if (!l2)
        return 0;
    l2->control = control;
    *out_l2 = l2;
    return 1;
}

void arcl_l2_shutdown(arcl_l2_t *l2)
{
    free(l2);
}

int arcl_l2_call(const char *name, const char *request_json, void *userdata,
                  char *result_json, size_t result_size,
                  char *error_message, size_t error_size)
{
    arcl_l2_t *l2 = (arcl_l2_t *)userdata;
    if (!l2 || !name || !result_json || !error_message)
        return 0;
    error_message[0] = '\0';

    if (strcmp(name, "arcl_registers") == 0)
        return handle_registers(l2, result_json, result_size);
    if (strcmp(name, "arcl_write_registers") == 0)
        return handle_write_registers(l2, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_read_mem") == 0)
        return handle_read_mem(l2, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_write_mem") == 0)
        return handle_write_mem(l2, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_breakpoint") == 0)
        return handle_breakpoint(l2, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_stack") == 0)
        return handle_stack(l2, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_disasm") == 0)
        return handle_disasm(l2, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_step") == 0)
    {
        mcp_json_set_error(error_message, error_size,
                    "not supported: the C68K backend this project uses cannot stop mid-instruction, only "
                    "between frames (x68k_mcp.md 6.3) - use arcl_run with frames=1, or an exec breakpoint "
                    "via arcl_breakpoint with arcl_run's until_break, instead of per-instruction stepping");
        return 0;
    }

    mcp_json_set_error(error_message, error_size, "Unknown tool");
    return 0;
}
