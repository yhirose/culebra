// Microphone capture on miniaudio, compiled into culebra_raudio.c next to the
// playback module (the same miniaudio, no second copy). One capture is a
// device plus a ring the device's thread fills and the caller drains.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CulebraCapture CulebraCapture;

// Opens the capture context (idempotent); 0 when the platform has none. Every
// call below needs it open.
int culebra_capture_init(void);
// 1 when the platform reports at least one capture device.
int culebra_capture_present(void);
// A stopped capture of `rate` Hz and 1 or 2 interleaved float channels, with
// a ring of `ring_frames`; NULL when the device or the ring cannot be had.
CulebraCapture* culebra_capture_open(unsigned rate, unsigned channels,
                                     unsigned ring_frames);
void culebra_capture_close(CulebraCapture* c);
void culebra_capture_start(CulebraCapture* c);
void culebra_capture_stop(CulebraCapture* c);
int culebra_capture_running(const CulebraCapture* c);
// Frames waiting in the ring.
size_t culebra_capture_waiting(CulebraCapture* c);
// Moves up to `frames` waiting frames into `out`; answers how many.
size_t culebra_capture_read(CulebraCapture* c, float* out, size_t frames);
// Closes the context; call after every capture is closed.
void culebra_capture_shutdown(void);

#ifdef __cplusplus
}
#endif
