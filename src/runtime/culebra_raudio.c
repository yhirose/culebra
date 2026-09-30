// raylib's audio module as culebra compiles it: standalone, so the Audio
// namespace links neither raylib's core nor SDL (raylib itself is built with
// SUPPORT_MODULE_RAUDIO off, and this is the only copy of the module). The
// options live here, not in CMake, for the reason culebra_sqlite3.c gives.
// raudio.c includes "raudio.h" (src/runtime/raudio.h) in this mode.

#define RAUDIO_STANDALONE
#define SUPPORT_MODULE_RAUDIO 1
#define SUPPORT_FILEFORMAT_WAV 1
#define SUPPORT_FILEFORMAT_OGG 1
#define SUPPORT_FILEFORMAT_MP3 1
#define SUPPORT_FILEFORMAT_QOA 0
#define SUPPORT_FILEFORMAT_FLAC 0
#define SUPPORT_FILEFORMAT_XM 0
#define SUPPORT_FILEFORMAT_MOD 0

#include <stdarg.h>
#include <stdio.h>

// Standalone raudio logs through printf, onto the program's stdout. Only
// warnings and worse are worth a line, and they go to stderr, spelled the
// way raylib's own logger spells them (4 is raudio's LOG_WARNING).
static void culebra_raudio_trace(int level, const char* fmt, ...) {
  if (level < 4) return;
  va_list ap;
  va_start(ap, fmt);
  fputs(level >= 6 ? "FATAL: " : level >= 5 ? "ERROR: " : "WARNING: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}
#define TRACELOG(level, ...) culebra_raudio_trace(level, __VA_ARGS__)

#include "../../vendor/raylib/src/raudio.c"

// --- microphone capture (culebra_capture.h) ---------------------------------
//
// Playback keeps raudio's own context; capture opens a context of its own, so
// a program that only listens never touches the output device. The device
// thread writes into a ring and the caller reads from it: miniaudio's ring is
// single-producer single-consumer and lock-free, which is this shape exactly.

#include "culebra_capture.h"

struct CulebraCapture {
  ma_device device;
  ma_pcm_rb ring;
  unsigned channels;
};

static ma_context g_capture_ctx;

int culebra_capture_init(void) {
  ma_context_config cfg = ma_context_config_init();
  return ma_context_init(NULL, 0, &cfg, &g_capture_ctx) == MA_SUCCESS;
}

int culebra_capture_present(void) {
  ma_device_info* playback;
  ma_uint32 playback_count;
  ma_device_info* capture;
  ma_uint32 capture_count = 0;
  if (ma_context_get_devices(&g_capture_ctx, &playback, &playback_count,
                             &capture, &capture_count) != MA_SUCCESS)
    return 0;
  return capture_count > 0;
}

static void capture_callback(ma_device* device, void* out, const void* in,
                             ma_uint32 frames) {
  CulebraCapture* c = (CulebraCapture*)device->pUserData;
  const float* src = (const float*)in;
  (void)out;
  while (frames > 0) {
    ma_uint32 n = frames;
    void* dst = NULL;
    if (ma_pcm_rb_acquire_write(&c->ring, &n, &dst) != MA_SUCCESS || n == 0) {
      return;  // the reader fell behind: the newest frames are the ones lost
    }
    memcpy(dst, src, (size_t)n * c->channels * sizeof(float));
    ma_pcm_rb_commit_write(&c->ring, n);
    src += (size_t)n * c->channels;
    frames -= n;
  }
}

CulebraCapture* culebra_capture_open(unsigned rate, unsigned channels,
                                     unsigned ring_frames) {
  CulebraCapture* c = (CulebraCapture*)RL_CALLOC(1, sizeof(CulebraCapture));
  if (c == NULL) return NULL;
  c->channels = channels;
  if (ma_pcm_rb_init(ma_format_f32, channels, ring_frames, NULL, NULL,
                     &c->ring) != MA_SUCCESS) {
    RL_FREE(c);
    return NULL;
  }
  ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
  cfg.capture.format = ma_format_f32;
  cfg.capture.channels = channels;
  cfg.sampleRate = rate;
  cfg.dataCallback = capture_callback;
  cfg.pUserData = c;
  if (ma_device_init(&g_capture_ctx, &cfg, &c->device) != MA_SUCCESS) {
    ma_pcm_rb_uninit(&c->ring);
    RL_FREE(c);
    return NULL;
  }
  return c;
}

void culebra_capture_close(CulebraCapture* c) {
  if (c == NULL) return;
  ma_device_uninit(&c->device);  // stops the thread before the ring goes
  ma_pcm_rb_uninit(&c->ring);
  RL_FREE(c);
}

// Both are no-ops on a device already in that state.
void culebra_capture_start(CulebraCapture* c) { ma_device_start(&c->device); }
void culebra_capture_stop(CulebraCapture* c) { ma_device_stop(&c->device); }

int culebra_capture_running(const CulebraCapture* c) {
  return ma_device_is_started(&c->device);
}

size_t culebra_capture_waiting(CulebraCapture* c) {
  return ma_pcm_rb_available_read(&c->ring);
}

size_t culebra_capture_read(CulebraCapture* c, float* out, size_t frames) {
  size_t got = 0;
  while (got < frames) {
    ma_uint32 n = (ma_uint32)(frames - got);
    void* src = NULL;
    if (ma_pcm_rb_acquire_read(&c->ring, &n, &src) != MA_SUCCESS || n == 0)
      break;
    memcpy(out + got * c->channels, src, (size_t)n * c->channels * sizeof(float));
    ma_pcm_rb_commit_read(&c->ring, n);
    got += n;
  }
  return got;
}

void culebra_capture_shutdown(void) { ma_context_uninit(&g_capture_ctx); }
