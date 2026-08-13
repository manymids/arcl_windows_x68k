#include "base64.h"

static const char ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t px68k_base64_encoded_size(size_t in_size)
{
    return ((in_size + 2) / 3) * 4 + 1; /* +1 for the NUL terminator */
}

size_t px68k_base64_encode(const unsigned char *in, size_t in_size, char *out, size_t out_size)
{
    size_t needed = px68k_base64_encoded_size(in_size);
    size_t i, o = 0;

    if (!out || out_size < needed)
        return 0;

    for (i = 0; i + 2 < in_size; i += 3)
    {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        out[o++] = ALPHABET[(v >> 18) & 0x3F];
        out[o++] = ALPHABET[(v >> 12) & 0x3F];
        out[o++] = ALPHABET[(v >> 6) & 0x3F];
        out[o++] = ALPHABET[v & 0x3F];
    }
    if (in_size - i == 1)
    {
        unsigned v = (unsigned)in[i] << 16;
        out[o++] = ALPHABET[(v >> 18) & 0x3F];
        out[o++] = ALPHABET[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    }
    else if (in_size - i == 2)
    {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
        out[o++] = ALPHABET[(v >> 18) & 0x3F];
        out[o++] = ALPHABET[(v >> 12) & 0x3F];
        out[o++] = ALPHABET[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}
