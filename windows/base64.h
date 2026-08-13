#ifndef PX68K_BASE64_H
#define PX68K_BASE64_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard (RFC 4648) base64 alphabet, with padding. out_size must be at
 * least px68k_base64_encoded_size(in_size); writes a NUL terminator.
 * Returns the encoded length (excluding the terminator). */
size_t px68k_base64_encoded_size(size_t in_size);
size_t px68k_base64_encode(const unsigned char *in, size_t in_size, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* PX68K_BASE64_H */
