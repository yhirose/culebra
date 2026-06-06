// Http feature object for the linear-scaling AOT runtime (see
// DESIGN_linear_rt.md). Emits the single strong definition of
// culebra::http::http_request — the lone OpenSSL/zlib choke (the only
// code that instantiates httplib::Client) — which the AOT link
// force-loads only when the program uses Http, overriding the weak stub
// in the core archive. TLS/gzip symbols are referenced exclusively from
// here, so a program that never makes an Http request links neither this
// object nor OpenSSL/zlib.
//
// CULEBRA_RT_HTTP_REQUEST_STRONG makes http_request a strong, non-inline
// definition; http.h is value-neutral (no interpreter/JIT types), so this
// TU stays small.

#define CULEBRA_RT_HTTP_REQUEST_STRONG
#include <http.h>
