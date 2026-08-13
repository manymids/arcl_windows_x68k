#ifndef PX68K_PNG_WRITE_H
#define PX68K_PNG_WRITE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encodes an RGB565 framebuffer region to a truecolor (24-bit) PNG file in
 * memory. stride_pixels is the number of pixels between the start of
 * consecutive rows in `pixels` (may exceed width). On success *out_data is
 * a malloc'd buffer the caller must free(); returns false on any zlib
 * failure and leaves *out_data untouched. */
bool px68k_encode_png_rgb565(const uint16_t *pixels, unsigned width, unsigned height,
                              unsigned stride_pixels, uint8_t **out_data, size_t *out_size);

/* Convenience wrapper: encode then write to a host file path. */
bool px68k_write_png_rgb565(const char *path, const uint16_t *pixels,
                             unsigned width, unsigned height, unsigned stride_pixels);

#ifdef __cplusplus
}
#endif

#endif /* PX68K_PNG_WRITE_H */
