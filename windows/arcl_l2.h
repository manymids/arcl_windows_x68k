#ifndef PX68K_ARCL_L2_H
#define PX68K_ARCL_L2_H

#include <stddef.h>

#include "arcl_control.h"

typedef struct arcl_l2 arcl_l2_t;

int arcl_l2_init(arcl_l2_t **out_l2, arcl_control_t *control);
void arcl_l2_shutdown(arcl_l2_t *l2);
int arcl_l2_call(const char *name, const char *request_json, void *userdata,
                  char *result_json, size_t result_size,
                  char *error_message, size_t error_size);

#endif /* PX68K_ARCL_L2_H */
