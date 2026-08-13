/* L4 tool: arcl_snapshot (x68k_mcp.md 6.5/8 Phase 3).
 *
 * Named, in-memory, no fixed slot count (up to ARCL_SNAPSHOT_MAX) - unlike
 * Control's arcl_save_state/arcl_load_state, which are the common spec's
 * numbered 0-9 slots (arcl_common_spec.md 4.7-adjacent Control contract).
 * Both ultimately go through the same retro_serialize/retro_unserialize
 * blob (frontend_core.c) - this file is just a second, name-keyed table on
 * top of the same mechanism, kept separate from arcl_control.c's slots so
 * the two don't collide or share a capacity limit. arcl_common_spec.md 6.5
 * requires reporting memory consumption in the response, since state size
 * x depth is the real constraint for machines with large save states. */
#include "arcl_l4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend_core.h"
#include "mcp_json.h"

#define ARCL_SNAPSHOT_MAX 32
#define ARCL_SNAPSHOT_NAME_MAX 64

typedef struct {
    int in_use;
    char name[ARCL_SNAPSHOT_NAME_MAX];
    unsigned char *data;
    size_t size;
    unsigned long long frame;
} snapshot_slot_t;

struct arcl_l4 {
    arcl_control_t *control;
    snapshot_slot_t slots[ARCL_SNAPSHOT_MAX];
};

static size_t total_bytes(arcl_l4_t *l4)
{
    size_t total = 0;
    int i;
    for (i = 0; i < ARCL_SNAPSHOT_MAX; i++)
        if (l4->slots[i].in_use)
            total += l4->slots[i].size;
    return total;
}

static snapshot_slot_t *find_slot(arcl_l4_t *l4, const char *name)
{
    int i;
    for (i = 0; i < ARCL_SNAPSHOT_MAX; i++)
        if (l4->slots[i].in_use && strcmp(l4->slots[i].name, name) == 0)
            return &l4->slots[i];
    return NULL;
}

static int handle_snapshot(arcl_l4_t *l4, const char *request_json,
                            char *result_json, size_t result_size,
                            char *error_message, size_t error_size)
{
    char action[16];
    char name[ARCL_SNAPSHOT_NAME_MAX];
    size_t used;

    if (!mcp_json_get_string_any(request_json, "action", action, sizeof(action)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'action' (create/restore/delete/list)");
        return 0;
    }

    if (strcmp(action, "list") == 0)
    {
        int i, first = 1;
        used = 0;
        mcp_json_appendf(result_json, result_size, &used, "{\"action\":\"list\",\"snapshots\":[");
        for (i = 0; i < ARCL_SNAPSHOT_MAX; i++)
        {
            snapshot_slot_t *s = &l4->slots[i];
            char quoted_name[ARCL_SNAPSHOT_NAME_MAX + 8];
            if (!s->in_use)
                continue;
            mcp_json_write_quoted(s->name, quoted_name, sizeof(quoted_name));
            mcp_json_appendf(result_json, result_size, &used,
                              "%s{\"name\":%s,\"frame\":%llu,\"bytes\":%zu}",
                              first ? "" : ",", quoted_name, s->frame, s->size);
            first = 0;
        }
        mcp_json_appendf(result_json, result_size, &used, "],\"total_bytes\":%zu", total_bytes(l4));
        arcl_control_append_status(l4->control, result_json, result_size, used, -1);
        return 1;
    }

    /* Request field is "snapshot_name", not "name" - historically because
     * every tools/call request already has params.name = the tool name
     * itself ("arcl_snapshot"), and mcp_json_get_string_any() scanned the
     * whole raw JSON-RPC line (mcp_server.c passed it through unparsed),
     * so a plain "name" argument would silently read the tool name instead.
     * mcp_server.c now scopes tool handlers to just params.arguments, which
     * removes that particular collision - but "snapshot_name" is kept as
     * the field name since it's already documented (main.c's tool schema)
     * and existing callers depend on it. */
    if (!mcp_json_get_string_any(request_json, "snapshot_name", name, sizeof(name)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'snapshot_name'");
        return 0;
    }

    if (strcmp(action, "create") == 0)
    {
        snapshot_slot_t *s = find_slot(l4, name);
        size_t size;
        unsigned long long frame;
        int running;

        if (!s)
        {
            int i;
            for (i = 0; i < ARCL_SNAPSHOT_MAX; i++)
                if (!l4->slots[i].in_use) { s = &l4->slots[i]; break; }
            if (!s)
            {
                mcp_json_set_error(error_message, error_size, "snapshot table is full (max 32) - delete one first");
                return 0;
            }
        }

        arcl_control_run_frames(l4->control, 0); /* ensure paused, consistent state */
        size = px68k_frontend_serialize_size();
        if (size == 0 || (s->size != size && !(s->data = (unsigned char *)realloc(s->data, size))))
        {
            mcp_json_set_error(error_message, error_size, "failed to allocate snapshot buffer");
            return 0;
        }
        s->size = size;
        if (!px68k_frontend_serialize(s->data, s->size))
        {
            mcp_json_set_error(error_message, error_size, "core failed to serialize state");
            return 0;
        }
        arcl_control_get_status(l4->control, &frame, &running);
        s->frame = frame;
        strncpy(s->name, name, sizeof(s->name) - 1);
        s->name[sizeof(s->name) - 1] = '\0';
        s->in_use = 1;

        {
            /* name is caller-controlled (arcl_common_spec.md places no
             * charset restriction on snapshot_name) - embedding it with a
             * bare %s let a name containing '"' break the response's JSON
             * structure, same class of bug the `list` action above already
             * avoided by quoting. */
            char quoted_name[ARCL_SNAPSHOT_NAME_MAX + 8];
            mcp_json_write_quoted(s->name, quoted_name, sizeof(quoted_name));
            used = (size_t)snprintf(result_json, result_size,
                                     "{\"action\":\"create\",\"name\":%s,\"bytes\":%zu,\"total_bytes\":%zu",
                                     quoted_name, s->size, total_bytes(l4));
        }
        arcl_control_append_status(l4->control, result_json, result_size, used, -1);
        return 1;
    }

    if (strcmp(action, "restore") == 0)
    {
        snapshot_slot_t *s = find_slot(l4, name);
        char quoted_name[ARCL_SNAPSHOT_NAME_MAX + 8];
        if (!s)
        {
            mcp_json_set_error(error_message, error_size, "no snapshot with that name");
            return 0;
        }
        arcl_control_run_frames(l4->control, 0);
        if (!px68k_frontend_unserialize(s->data, s->size))
        {
            mcp_json_set_error(error_message, error_size, "core failed to restore state (incompatible snapshot?)");
            return 0;
        }
        arcl_control_set_frame(l4->control, s->frame);
        mcp_json_write_quoted(s->name, quoted_name, sizeof(quoted_name));
        used = (size_t)snprintf(result_json, result_size, "{\"action\":\"restore\",\"name\":%s", quoted_name);
        arcl_control_append_status(l4->control, result_json, result_size, used, -1);
        return 1;
    }

    if (strcmp(action, "delete") == 0)
    {
        snapshot_slot_t *s = find_slot(l4, name);
        char quoted_name[ARCL_SNAPSHOT_NAME_MAX + 8];
        if (!s)
        {
            mcp_json_set_error(error_message, error_size, "no snapshot with that name");
            return 0;
        }
        free(s->data);
        memset(s, 0, sizeof(*s));
        mcp_json_write_quoted(name, quoted_name, sizeof(quoted_name));
        used = (size_t)snprintf(result_json, result_size,
                                 "{\"action\":\"delete\",\"name\":%s,\"total_bytes\":%zu", quoted_name, total_bytes(l4));
        arcl_control_append_status(l4->control, result_json, result_size, used, -1);
        return 1;
    }

    mcp_json_set_error(error_message, error_size, "action must be create, restore, delete, or list");
    return 0;
}

int arcl_l4_init(arcl_l4_t **out_l4, arcl_control_t *control)
{
    arcl_l4_t *l4;
    if (!out_l4 || !control)
        return 0;
    l4 = (arcl_l4_t *)calloc(1, sizeof(*l4));
    if (!l4)
        return 0;
    l4->control = control;
    *out_l4 = l4;
    return 1;
}

void arcl_l4_shutdown(arcl_l4_t *l4)
{
    int i;
    if (!l4)
        return;
    for (i = 0; i < ARCL_SNAPSHOT_MAX; i++)
        free(l4->slots[i].data);
    free(l4);
}

int arcl_l4_call(const char *name, const char *request_json, void *userdata,
                  char *result_json, size_t result_size,
                  char *error_message, size_t error_size)
{
    arcl_l4_t *l4 = (arcl_l4_t *)userdata;
    if (!l4 || !name || !result_json || !error_message)
        return 0;
    error_message[0] = '\0';

    if (strcmp(name, "arcl_snapshot") == 0)
        return handle_snapshot(l4, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_rewind") == 0)
        return arcl_control_rewind_call(l4->control, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "arcl_speed") == 0)
    {
        double fps, percent;
        int running;
        size_t used;
        arcl_control_get_speed(l4->control, &fps, &percent, &running);
        used = (size_t)snprintf(result_json, result_size,
                                 "{\"running\":%s,\"fps\":%.2f,\"nominal_fps\":%.2f,\"speed_percent\":%.1f",
                                 running ? "true" : "false", fps, px68k_frontend_get_fps(), percent);
        arcl_control_append_status(l4->control, result_json, result_size, used, -1);
        return 1;
    }

    mcp_json_set_error(error_message, error_size, "Unknown tool");
    return 0;
}
