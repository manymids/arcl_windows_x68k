#ifndef PX68K_MCP_JSON_H
#define PX68K_MCP_JSON_H

#include <stddef.h>
#include <stdint.h>

/* Small JSON helpers for the narrow JSON-RPC envelope this server accepts.
 * They parse JSON strings correctly and do not emit any output themselves. */
int mcp_json_get_string(const char *json, const char *key, char *out, size_t out_size);
int mcp_json_get_string_any(const char *json, const char *key, char *out, size_t out_size);
int mcp_json_get_long_any(const char *json, const char *key, long *out);
int mcp_json_get_bool_any(const char *json, const char *key, int *out);
int mcp_json_get_id(const char *json, char *out, size_t out_size);
void mcp_json_write_quoted(const char *text, char *out, size_t out_size);

/* arcl_common_spec.md 7.4: memory-ish fields accept either a JSON integer or
 * a hex string ("0x1000"); a leading "0x"/"0X" on the string selects base
 * 16, otherwise base 10. Shared by every tool module that takes an
 * address/register-value argument (was duplicated per-file before). */
int mcp_json_get_hex_or_int_any(const char *json, const char *key, uint32_t *out);

/* Copies `message` into `out` (truncating to fit), for tool handlers'
 * error_message out-parameter. Shared by every tool module (was duplicated
 * per-file before). */
void mcp_json_set_error(char *out, size_t out_size, const char *message);

/* Appends printf-style output at buf[*used], advancing *used by however
 * many bytes were actually written. If the formatted text wouldn't fit,
 * *used is clamped to buf_size instead of overshooting it (matching
 * snprintf's "would-have-written" return value would otherwise let *used
 * exceed buf_size on a later call, and the next buf+*used/buf_size-*used
 * pair would then point/size past the end of the buffer). Once *used ==
 * buf_size, further calls are no-ops. Safe replacement for the
 * `used += (size_t)snprintf(buf + used, buf_size - used, ...)` pattern used
 * throughout the arcl_l*.c tool handlers, in particular inside loops where
 * truncation would otherwise accumulate. */
void mcp_json_appendf(char *buf, size_t buf_size, size_t *used, const char *fmt, ...);

/* Finds `key`'s value in `json` (same "anywhere in the text, ignoring
 * nesting" lookup as mcp_json_get_string_any) and, if it is a JSON object
 * (`{...}`), copies the whole object (braces included) into `out`. Returns
 * 0 if the key is missing, its value isn't an object, or it doesn't fit in
 * out_size. Used by the MCP server to scope tool handlers to just
 * `params.arguments` instead of the full JSON-RPC request line. */
int mcp_json_extract_object(const char *json, const char *key, char *out, size_t out_size);

#endif /* PX68K_MCP_JSON_H */
