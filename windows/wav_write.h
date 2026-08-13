#ifndef PX68K_WAV_WRITE_H
#define PX68K_WAV_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encodes interleaved stereo 16-bit PCM samples (frames = sample pairs)
 * into a standard 44-byte-header RIFF/WAVE file, in memory. On success
 * *out_data is a malloc'd buffer the caller must free(); returns false on
 * allocation failure. No compression - matches arcl_screenshot's PNG
 * being the only other file format this project writes, both
 * intentionally simple/uncompressed-or-lossless formats. */
bool px68k_encode_wav_s16_stereo(const int16_t *data, size_t frames, unsigned sample_rate,
                                  uint8_t **out_data, size_t *out_size);

/* Convenience wrapper: encode then write to a host file path. */
bool px68k_write_wav_s16_stereo(const char *path, const int16_t *data,
                                 size_t frames, unsigned sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* PX68K_WAV_WRITE_H */
