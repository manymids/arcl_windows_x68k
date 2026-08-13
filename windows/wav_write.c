#include "wav_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

bool px68k_encode_wav_s16_stereo(const int16_t *data, size_t frames, unsigned sample_rate,
                                  uint8_t **out_data, size_t *out_size)
{
    uint32_t data_bytes = (uint32_t)(frames * 2 * sizeof(int16_t));
    uint32_t byte_rate = sample_rate * 2 * (uint32_t)sizeof(int16_t);
    size_t total = 44 + (size_t)data_bytes;
    uint8_t *buf = (uint8_t *)malloc(total);
    uint8_t *p;

    if (!buf)
        return false;
    p = buf;

    memcpy(p, "RIFF", 4); p += 4;
    put_u32le(p, 36 + data_bytes); p += 4;
    memcpy(p, "WAVE", 4); p += 4;

    memcpy(p, "fmt ", 4); p += 4;
    put_u32le(p, 16); p += 4;               /* fmt chunk size */
    put_u16le(p, 1); p += 2;                /* PCM */
    put_u16le(p, 2); p += 2;                /* stereo */
    put_u32le(p, sample_rate); p += 4;
    put_u32le(p, byte_rate); p += 4;
    put_u16le(p, 2 * (uint16_t)sizeof(int16_t)); p += 2; /* block align */
    put_u16le(p, 16); p += 2;               /* bits per sample */

    memcpy(p, "data", 4); p += 4;
    put_u32le(p, data_bytes); p += 4;
    if (data_bytes > 0 && data)
        memcpy(p, data, data_bytes);

    *out_data = buf;
    *out_size = total;
    return true;
}

bool px68k_write_wav_s16_stereo(const char *path, const int16_t *data,
                                 size_t frames, unsigned sample_rate)
{
    uint8_t *buf;
    size_t size;
    FILE *f;
    bool ok;

    if (!px68k_encode_wav_s16_stereo(data, frames, sample_rate, &buf, &size))
        return false;

    f = fopen(path, "wb");
    if (!f)
    {
        free(buf);
        return false;
    }
    ok = (fwrite(buf, 1, size, f) == size);
    fclose(f);
    free(buf);
    return ok;
}
