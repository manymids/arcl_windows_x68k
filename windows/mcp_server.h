#ifndef PX68K_MCP_SERVER_H
#define PX68K_MCP_SERVER_H

#include <stddef.h>
#include <stdio.h>

enum {
    MCP_LAYER_L0 = 1u << 0,
    MCP_LAYER_L1 = 1u << 1,
    MCP_LAYER_L2 = 1u << 2,
    MCP_LAYER_L3 = 1u << 3,
    MCP_LAYER_L4 = 1u << 4
};

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;
    unsigned layer_mask;
} mcp_tool_definition_t;

typedef int (*mcp_tool_call_fn)(const char *name, const char *request_json, void *userdata,
                                char *result_json, size_t result_size,
                                char *error_message, size_t error_size);

typedef struct {
    unsigned enabled_layers;
    const mcp_tool_definition_t *tools;
    size_t tool_count;
    mcp_tool_call_fn call_tool;
    void *userdata;
} mcp_server_config_t;

/* Parse l0,l1,... or all. NULL selects the conservative default l0. */
int mcp_server_parse_layers(const char *text, unsigned *out_layers);

/* Serve newline-delimited JSON-RPC 2.0 MCP messages until stdin closes. */
int mcp_server_run(FILE *input, FILE *output, const mcp_server_config_t *config);

#endif /* PX68K_MCP_SERVER_H */
