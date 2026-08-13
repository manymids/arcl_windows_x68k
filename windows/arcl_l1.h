#ifndef PX68K_ARCL_L1_H
#define PX68K_ARCL_L1_H

#include <stddef.h>

#include "arcl_control.h"
#include "arcl_l0.h" /* handle_command decomposes into arcl_type, see arcl_common_spec.md 7.7 */

typedef struct arcl_l1 arcl_l1_t;

/* l0 must already be initialized: arcl_command's echo step calls through
 * to arcl_l0_call("arcl_type", ...) with it, so typing goes through the
 * exact same empirically-verified key mapping arcl_type itself uses
 * (x68k_mcp.md 6.1.1) rather than a second, divergent implementation. */
int arcl_l1_init(arcl_l1_t **out_l1, arcl_control_t *control, arcl_l0_t *l0);
void arcl_l1_shutdown(arcl_l1_t *l1);
int arcl_l1_call(const char *name, const char *request_json, void *userdata,
                  char *result_json, size_t result_size,
                  char *error_message, size_t error_size);

/* Registered with arcl_control as its console_match_fn (arcl_control.h) so
 * arcl_run's text_match stop condition can check the decoded console
 * without arcl_control depending on this header. */
int arcl_l1_console_match(void *userdata, const char *text_match,
                           char *matched_line, size_t matched_line_size);

#endif /* PX68K_ARCL_L1_H */
