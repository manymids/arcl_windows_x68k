#ifndef PX68K_ARCL_L3_H
#define PX68K_ARCL_L3_H

#include <stddef.h>

#include "arcl_control.h"

typedef struct arcl_l3 arcl_l3_t;

int arcl_l3_init(arcl_l3_t **out_l3, arcl_control_t *control);
void arcl_l3_shutdown(arcl_l3_t *l3);
int arcl_l3_call(const char *name, const char *request_json, void *userdata,
                  char *result_json, size_t result_size,
                  char *error_message, size_t error_size);

#endif /* PX68K_ARCL_L3_H */
