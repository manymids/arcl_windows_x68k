#ifndef PX68K_ARCL_L4_H
#define PX68K_ARCL_L4_H

#include <stddef.h>

#include "arcl_control.h"

typedef struct arcl_l4 arcl_l4_t;

int arcl_l4_init(arcl_l4_t **out_l4, arcl_control_t *control);
void arcl_l4_shutdown(arcl_l4_t *l4);
int arcl_l4_call(const char *name, const char *request_json, void *userdata,
                  char *result_json, size_t result_size,
                  char *error_message, size_t error_size);

#endif /* PX68K_ARCL_L4_H */
