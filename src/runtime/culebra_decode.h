// A recording read at the precision it was made in, with the decoders raudio
// compiles in (culebra_raudio.c): raudio's own loaders read WAV and Ogg as 16
// bits, for playing; these read every format as floats.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads `len` bytes of `fmt` (".wav", ".mp3" or ".ogg") and hands `sink` its
// rate, its channels and its frames, the channels interleaved, -1..1, before
// letting go of them. 0 when the bytes do not decode.
int culebra_decode(const char* fmt, const unsigned char* data, size_t len,
                   void (*sink)(void* ctx, unsigned rate, unsigned channels,
                                const float* frames, unsigned long long count),
                   void* ctx);

#ifdef __cplusplus
}
#endif
