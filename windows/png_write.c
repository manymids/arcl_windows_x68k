/* Minimal RGB565 -> truecolor PNG encoder, built on zlib for the deflate
 * container. Used by --dump-frame and by arcl_screenshot's `path`/`inline`
 * output (windows/arcl_l0.c). */
#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* Appends one PNG chunk at *cursor and advances it. Caller guarantees the
 * destination buffer has room (size is computed exactly up front). */
static void put_chunk(uint8_t **cursor, const char type[4], const uint8_t *data, uint32_t len)
{
    uint8_t *p = *cursor;
    uLong crc;

    put_be32(p, len);
    p += 4;
    memcpy(p, type, 4);
    p += 4;
    if (len)
    {
        memcpy(p, data, len);
        p += len;
    }
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (len)
        crc = crc32(crc, data, len);
    put_be32(p, (uint32_t)crc);
    p += 4;

    *cursor = p;
}

static inline uint8_t r5_to_8(unsigned v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t g6_to_8(unsigned v) { return (uint8_t)((v << 2) | (v >> 4)); }
static inline uint8_t b5_to_8(unsigned v) { return (uint8_t)((v << 3) | (v >> 2)); }

bool px68k_encode_png_rgb565(const uint16_t *pixels, unsigned width, unsigned height,
                              unsigned stride_pixels, uint8_t **out_data, size_t *out_size)
{
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    bool ok = false;
    uint8_t *raw = NULL;
    uint8_t *deflated = NULL;
    uLongf deflated_cap;
    size_t raw_row_stride = 1 + (size_t)width * 3; /* filter byte + RGB888 */
    size_t raw_size = raw_row_stride * height;
    uint8_t ihdr[13];
    uint8_t *png = NULL;
    uint8_t *cursor;
    size_t png_size;

    if (!pixels || !out_data || !out_size || width == 0 || height == 0)
        return false;

    raw = (uint8_t *)malloc(raw_size);
    if (!raw)
        return false;

    for (unsigned y = 0; y < height; y++)
    {
        uint8_t *row = raw + (size_t)y * raw_row_stride;
        const uint16_t *src = pixels + (size_t)y * stride_pixels;
        row[0] = 0; /* filter: none */
        for (unsigned x = 0; x < width; x++)
        {
            uint16_t px = src[x];
            uint8_t *dst = row + 1 + (size_t)x * 3;
            dst[0] = r5_to_8((px >> 11) & 0x1F);
            dst[1] = g6_to_8((px >> 5) & 0x3F);
            dst[2] = b5_to_8(px & 0x1F);
        }
    }

    deflated_cap = compressBound((uLong)raw_size);
    deflated = (uint8_t *)malloc(deflated_cap);
    if (!deflated)
        goto done;

    if (compress2(deflated, &deflated_cap, raw, (uLong)raw_size, Z_BEST_SPEED) != Z_OK)
        goto done;

    put_be32(ihdr + 0, width);
    put_be32(ihdr + 4, height);
    ihdr[8]  = 8; /* bit depth */
    ihdr[9]  = 2; /* color type: truecolor */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */

    png_size = sizeof(signature)
             + (4 + 4 + sizeof(ihdr) + 4)          /* IHDR */
             + (4 + 4 + deflated_cap + 4)           /* IDAT */
             + (4 + 4 + 0 + 4);                     /* IEND */
    png = (uint8_t *)malloc(png_size);
    if (!png)
        goto done;

    cursor = png;
    memcpy(cursor, signature, sizeof(signature));
    cursor += sizeof(signature);
    put_chunk(&cursor, "IHDR", ihdr, sizeof(ihdr));
    put_chunk(&cursor, "IDAT", deflated, (uint32_t)deflated_cap);
    put_chunk(&cursor, "IEND", NULL, 0);

    *out_data = png;
    *out_size = png_size;
    ok = true;

done:
    free(deflated);
    free(raw);
    return ok;
}

bool px68k_write_png_rgb565(const char *path, const uint16_t *pixels,
                             unsigned width, unsigned height, unsigned stride_pixels)
{
    uint8_t *data = NULL;
    size_t size = 0;
    FILE *fp;
    bool ok;

    if (!path)
        return false;
    if (!px68k_encode_png_rgb565(pixels, width, height, stride_pixels, &data, &size))
        return false;

    fp = fopen(path, "wb");
    if (!fp)
    {
        free(data);
        return false;
    }
    ok = fwrite(data, 1, size, fp) == size;
    fclose(fp);
    free(data);
    return ok;
}
