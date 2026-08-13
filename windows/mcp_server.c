#include "mcp_server.h"

#include "mcp_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MCP_LINE_MAX 65536

/* Generously sized for arcl_screenshot's inline base64 PNG (worst case is
 * the 480,000-pixel cap in arcl_l0.c: ~1.44MB raw -> ~1.44MB deflated
 * worst-case (incompressible) -> ~1.92MB base64), with headroom for the
 * rest of the JSON envelope. Heap-allocated per call rather than a stack
 * array; one malloc/free per tools/call is negligible next to the frame
 * this responds about. */
#define MCP_RESULT_MAX (3 * 1024 * 1024)

static void send_result(FILE *output, const char *id, const char *result)
{
    fprintf(output, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\n", id, result);
    fflush(output);
}

static void send_error(FILE *output, const char *id, int code, const char *message)
{
    char quoted[512];
    mcp_json_write_quoted(message, quoted, sizeof(quoted));
    fprintf(output, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":%s}}\n",
            id, code, quoted);
    fflush(output);
}

static int layers_from_name(const char *name, unsigned *layer)
{
    if (strcmp(name, "l0") == 0) *layer = MCP_LAYER_L0;
    else if (strcmp(name, "l1") == 0) *layer = MCP_LAYER_L1;
    else if (strcmp(name, "l2") == 0) *layer = MCP_LAYER_L2;
    else if (strcmp(name, "l3") == 0) *layer = MCP_LAYER_L3;
    else if (strcmp(name, "l4") == 0) *layer = MCP_LAYER_L4;
    else return 0;
    return 1;
}

int mcp_server_parse_layers(const char *text, unsigned *out_layers)
{
    char copy[64];
    char *token;
    char *comma;
    unsigned layers = 0;

    if (!out_layers)
        return 0;
    if (!text || !*text)
    {
        *out_layers = MCP_LAYER_L0 | MCP_LAYER_L1;
        return 1;
    }
    if (strcmp(text, "all") == 0)
    {
        *out_layers = MCP_LAYER_L0 | MCP_LAYER_L1 | MCP_LAYER_L2 | MCP_LAYER_L3 | MCP_LAYER_L4;
        return 1;
    }
    if (strlen(text) >= sizeof(copy))
        return 0;
    strcpy(copy, text);
    token = copy;
    for (;;)
    {
        unsigned layer;
        comma = strchr(token, ',');
        if (comma)
            *comma = '\0';
        if (!*token || !layers_from_name(token, &layer) || (layers & layer))
            return 0;
        layers |= layer;
        if (!comma)
            break;
        token = comma + 1;
    }
    if (!layers)
        return 0;
    *out_layers = layers;
    return 1;
}

static void send_initialize(FILE *output, const char *id)
{
    send_result(output, id,
        "{\"protocolVersion\":\"2025-06-18\","
        "\"capabilities\":{\"tools\":{\"listChanged\":false}},"
        "\"serverInfo\":{\"name\":\"px68k-arcl\",\"version\":\"0.1.0\"},"
        "\"instructions\":\"PX68K ARCL MCP server. Use tools/list to inspect currently implemented capabilities.\"}");
}

static void send_tools_list(FILE *output, const char *id, const mcp_server_config_t *config)
{
    size_t i;
    int first = 1;
    fputs("{\"jsonrpc\":\"2.0\",\"id\":", output);
    fputs(id, output);
    fputs(",\"result\":{\"tools\":[", output);
    for (i = 0; i < config->tool_count; i++)
    {
        const mcp_tool_definition_t *tool = &config->tools[i];
        char name[256];
        char description[2048];
        /* Control tools use layer_mask == 0 and are always visible. */
        if (tool->layer_mask && !(tool->layer_mask & config->enabled_layers))
            continue;
        mcp_json_write_quoted(tool->name, name, sizeof(name));
        mcp_json_write_quoted(tool->description, description, sizeof(description));
        fprintf(output, "%s{\"name\":%s,\"description\":%s,\"inputSchema\":%s}",
                first ? "" : ",", name, description, tool->input_schema_json);
        first = 0;
    }
    fputs("]}}\n", output);
    fflush(output);
}

static void send_tool_result(FILE *output, const char *id, const char *structured_json)
{
    /* The escaped copy can only be larger than the source for '"'/'\\'/control
     * characters (each becomes 2-6 bytes); doubling plus slack is always
     * enough headroom regardless of how big structured_json is (e.g. an
     * arcl_screenshot response carrying an inline base64 PNG). */
    size_t cap = strlen(structured_json) * 2 + 64;
    char *text = (char *)malloc(cap);
    if (!text)
    {
        fprintf(output, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32000,\"message\":\"out of memory\"}}\n", id);
        fflush(output);
        return;
    }
    mcp_json_write_quoted(structured_json, text, cap);
    fprintf(output, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":%s}],\"structuredContent\":%s}}\n",
            id, text, structured_json);
    fflush(output);
    free(text);
}

/* structured_json is optional (pass "" or NULL when there's nothing to
 * attach): most tool errors are a plain rejection (bad argument, unmapped
 * key) with no partial state worth reporting. arcl_input_macro is the
 * exception - a macro that fails partway still has to report exactly how
 * far it got (arcl_common_spec.md 4.5), so it populates result_json even
 * on failure and this carries it through as structuredContent alongside
 * isError:true. */
static void send_tool_error(FILE *output, const char *id, const char *message, const char *structured_json)
{
    char text[1024];
    mcp_json_write_quoted(message, text, sizeof(text));
    if (structured_json && structured_json[0])
        fprintf(output,
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":%s}],"
            "\"isError\":true,\"structuredContent\":%s}}\n",
            id, text, structured_json);
    else
        fprintf(output, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":%s}],\"isError\":true}}\n",
                id, text);
    fflush(output);
}

int mcp_server_run(FILE *input, FILE *output, const mcp_server_config_t *config)
{
    char line[MCP_LINE_MAX];
    char method[128];
    char tool_name[256];
    char id[128];

    if (!input || !output || !config)
        return 1;
    while (fgets(line, sizeof(line), input))
    {
        int has_id = mcp_json_get_id(line, id, sizeof(id));
        if (!mcp_json_get_string(line, "method", method, sizeof(method)))
        {
            if (has_id)
                send_error(output, id, -32600, "Invalid Request");
            continue;
        }
        if (strcmp(method, "notifications/initialized") == 0 ||
            strcmp(method, "notifications/cancelled") == 0)
            continue;
        if (!has_id)
            continue;
        if (strcmp(method, "initialize") == 0)
            send_initialize(output, id);
        else if (strcmp(method, "tools/list") == 0)
            send_tools_list(output, id, config);
        else if (strcmp(method, "tools/call") == 0)
        {
            char *result = (char *)malloc(MCP_RESULT_MAX);
            /* Scope tool handlers to just params.arguments instead of the
             * raw JSON-RPC line: mcp_json_get_*_any() looks up a key
             * anywhere in the text, ignoring nesting, so handlers that saw
             * the full envelope could accidentally read the request's own
             * top-level "id" or params.name (the tool name) instead of an
             * argument the caller intended - the documented reason
             * arcl_breakpoint's remove uses "bp_id" instead of "id" and
             * arcl_snapshot uses "snapshot_name" instead of "name" (see
             * arcl_l2.c/arcl_l4.c). Scoping down here removes that whole
             * collision class for every tool, existing or future; the
             * already-renamed fields are left as-is since they're already
             * the documented, depended-upon argument names. Allocated at
             * MCP_LINE_MAX since arguments is a substring of `line`. */
            char *args = (char *)malloc(MCP_LINE_MAX);
            char error[512];
            if (!result || !args)
                send_error(output, id, -32000, "out of memory");
            else
            {
                result[0] = '\0'; /* handlers that fail without partial state leave this empty */
                if (!mcp_json_extract_object(line, "arguments", args, MCP_LINE_MAX))
                    strcpy(args, "{}");
                if (!config->call_tool || !mcp_json_get_string_any(line, "name", tool_name, sizeof(tool_name)))
                    send_error(output, id, -32602, "Invalid tool request");
                else if (config->call_tool(tool_name, args, config->userdata,
                                           result, MCP_RESULT_MAX, error, sizeof(error)))
                    send_tool_result(output, id, result);
                else
                    send_tool_error(output, id, error[0] ? error : "Tool call failed", result);
            }
            free(result);
            free(args);
        }
        else if (strcmp(method, "ping") == 0)
            send_result(output, id, "{}");
        else
            send_error(output, id, -32601, "Method not found");
    }
    return ferror(input) ? 1 : 0;
}
