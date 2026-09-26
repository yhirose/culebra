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
