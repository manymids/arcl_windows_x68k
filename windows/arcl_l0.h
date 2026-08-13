#ifndef PX68K_ARCL_L0_H
#define PX68K_ARCL_L0_H

#include <stddef.h>

#include "arcl_control.h"

typedef struct arcl_l0 arcl_l0_t;

int arcl_l0_init(arcl_l0_t **out_l0, arcl_control_t *control);
void arcl_l0_shutdown(arcl_l0_t *l0);
int arcl_l0_call(const char *name, const char *request_json, void *userdata,
                  char *result_json, size_t result_size,
                  char *error_message, size_t error_size);

#endif /* PX68K_ARCL_L0_H */
