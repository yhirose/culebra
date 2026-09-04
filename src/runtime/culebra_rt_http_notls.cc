// The Http feature object again, built without TLS — the other half of the
// axis `culebra build --no-tls` selects. Same strong culebra::http::http_request
// as culebra_rt_http.cc, but compiled with cpp-httplib's OpenSSL support off,
// so the object references no libssl/libcrypto symbol and the AOT link appends
// no OpenSSL archive. Measured on macOS arm64: an `Http.get` binary is 1.22 MB
// this way against 5.07 MB with TLS, because OpenSSL is 82% of what Http adds
// (its precomputed EC tables alone are 512 KB, and they do not compress —
// see docs/deployment.md §1).
//
// Exactly one of the two objects is force-loaded into any binary. They define
// the same symbols from different httplib class layouts, so linking both would
// be an ODR violation; the driver only ever carries them as embedded data.
//
// CPPHTTPLIB_OPENSSL_SUPPORT is a directory-wide definition, undone for this
// target alone by -UCPPHTTPLIB_OPENSSL_SUPPORT (CMakeLists). That relies on the
// generator emitting COMPILE_OPTIONS after COMPILE_DEFINITIONS, which is why
// the guard below is here rather than a comment: if the order ever changes,
// this stops the build instead of quietly shipping a TLS-linked "no-TLS"
// archive that the size gate would then have to catch.
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#error "culebra_rt_http_notls.cc must compile without CPPHTTPLIB_OPENSSL_SUPPORT"
#endif

#define CULEBRA_RT_HTTP_REQUEST_STRONG
#include <stdlib/http.h>
