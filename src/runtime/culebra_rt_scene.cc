// `Scene` facade — raylib backend (v2: lighting + custom meshes).
//
// v1 validated the retained-scene + fluent-node architecture with flat-colour
// shapes. v2 adds:
//   * a directional-sun + ambient lighting shader (escapes raylib's unlit
//     DrawCube), so shapes read as solid 3D — `sun()` / `ambient()`.
//   * custom triangle meshes built by scalar push (`add_mesh()` -> vertex/tri/
//     build), since wrap can't marshal arrays on the JIT path. Track surfaces.
//
// Shapes are drawn as lit Models via DrawMesh with an explicit world matrix, so
// the parent/child hierarchy composes through raymath instead of the rlgl stack.
// Node handles stay owning (shared_ptr); fluent setters stay borrowed_method.
//
// Built into the driver (and force-loaded into `culebra build` outputs that name
// Scene) with -DCULEBRA_ENABLE_SCENE=ON; raylib + SDL3 come from the vendored
// submodules and CULEBRA_SCENE_LINK. No `culebra wrap` step.

// Windows puts three of raylib's names in <windows.h> too: GDI's Rectangle()
// hides `struct Rectangle` (a class name loses to a function name in the same
// scope), and USER32's CloseWindow(HWND) / ShowCursor(BOOL) are a different
// signature for the same extern "C" symbol. raylib's own raudio.c cuts them the
// same way. This has to be here rather than in os_compat.h, which is what
// wrap.h reaches <windows.h> through: every other TU keeps the full API, and
// nothing in culebra calls into GDI or USER32 anyway (SDL and raylib do their
// own windowing).
#if defined(_WIN32)
#define NOGDI   // Rectangle() and the rest of the drawing API
#define NOUSER  // CloseWindow(), ShowCursor() and the rest of the window API
#endif

#include <interop/wrap.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "raylib.h"     // vendored: vendor/raylib/src (added to include path by CMake)
#include "raymath.h"
#include "rlgl.h"       // FBO / shader / matrix stack for shadows
#include "stdlib/keynames.h"   // key / pad names, shared with the Canvas backend

namespace gfx {

// A channel saturates at the ends of its range. The cast alone would wrap —
// 256 to black, -1 to full — turning an off-by-one brightness tweak into the
// opposite colour.
static unsigned char chan(int64_t v) {
  return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static Color col(int64_t r, int64_t g, int64_t b, int64_t a = 255) {
  return Color{chan(r), chan(g), chan(b), chan(a)};
}

// 0-255 RGB to a normalized linear-ish Vector3, scaled by `k` (light intensity).
// `k` is deliberately not clamped: intensity above 1 is a brighter light.
static Vector3 rgb01(int64_t r, int64_t g, int64_t b, double k = 1.0) {
  return Vector3{(float)(chan(r) / 255.0 * k), (float)(chan(g) / 255.0 * k),
                 (float)(chan(b) / 255.0 * k)};
}

// Bring the audio device up on first use (a View, Sound or Music) so scripts
// don't have to manage it. Idempotent; a failure is latched so a device-less
// machine doesn't re-probe (and re-log) on every Sound/Music construction.
static void ensure_audio() {
  static bool failed = false;
  if (failed || IsAudioDeviceReady()) return;
  InitAudioDevice();
  if (!IsAudioDeviceReady()) {
    failed = true;
    // Sound is decorative, so no error — but say it once, past the ctor's
    // LOG_ERROR filter (a latched silence reads as "my sounds are broken").
    std::fprintf(stderr, "Scene: no audio device -- sound stays off\n");
  }
}

// raylib reports a graphics device it could not create with LOG_FATAL, and its
// default log handler exits the process — the View ctor never gets to turn the
// failure into an error the script can see. Printing without exiting lets
// InitPlatform's failure reach InitWindow, which reports it through
// IsWindowReady() like any other (same shape as the Canvas backend's handler).
// Body identical to the Canvas backend's: raylib holds ONE process-global
// callback slot, so whichever TU installs last serves both — the bodies must
// agree, with verbosity left to each TU's SetTraceLogLevel (raylib applies it
// before the callback).
static void trace_log(int level, const char* text, va_list args) {
  if (level < LOG_WARNING) return;
  std::fprintf(stderr, "%s: ", level >= LOG_FATAL     ? "FATAL"
                               : level >= LOG_ERROR   ? "ERROR"
                                                      : "WARNING");
  std::vfprintf(stderr, text, args);
  std::fputc('\n', stderr);
}

// Minimal lit shader: one directional sun + flat ambient, Lambert diffuse.
// raylib provides mvp / matModel / matNormal / colDiffuse; we add lightDir,
// lightColor, ambient. GLSL 330 (raylib's desktop GL backend).
static const char* kVS = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
uniform vec4 uvXform;     // per-material UV scale.xy + offset.zw (tiling, flips)
out vec3 fragNormal;
out vec3 fragWorld;
out vec2 fragUV;
out vec4 fragColor;
void main() {
  fragNormal = normalize((matNormal * vec4(vertexNormal, 0.0)).xyz);
  fragWorld = (matModel * vec4(vertexPosition, 1.0)).xyz;
  fragUV = vertexTexCoord * uvXform.xy + uvXform.zw;
  fragColor = vertexColor;
  gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

static const char* kFS = R"(#version 330
in vec3 fragNormal;
in vec3 fragWorld;
in vec2 fragUV;
in vec4 fragColor;
uniform sampler2D texture0;
// Shadow cascades carried on raylib's own material-map samplers (texture1 =
// MAP_SPECULAR, texture2 = MAP_NORMAL) — the same proven binding path as
// texture0. Binding them as ad-hoc extra samplers read a zero texture on macOS.
uniform sampler2D texture1;     // near cascade (tight, crisp)
uniform sampler2D texture2;     // far cascade (wide)
// The normal map rides the ROUGHNESS map slot (texture3) — the same working
// material path as the cascades. Meshes carry no tangents, so the tangent
// frame is rebuilt per fragment from screen-space derivatives of position
// and UV (Schüler's method): exact on flat faces, good enough on curved ones.
uniform sampler2D normalMap;
uniform float normalStrength;   // 0 = no normal map
uniform mat4 lightVP0;
uniform mat4 lightVP1;
uniform vec4 colDiffuse;
uniform vec3 lightDir;
uniform vec3 lightColor;
uniform vec3 ambient;
uniform vec3 viewPos;     // camera position (specular + fog distance)
uniform vec3 fogColor;
uniform float fogStart;   // fogStart huge => fog effectively off
uniform float fogEnd;
uniform float metallic;   // 0 dielectric .. 1 metal
uniform float roughness;  // 0 glossy .. 1 matte
uniform vec3 skyTop;      // zenith colour (also drives reflections)
uniform vec3 skyBot;      // horizon colour
// Per-material (see Material): coverage, and what a surface opts out of.
uniform float opacity;    // 0..1, multiplies the texture's alpha
uniform float cutoff;     // > 0: discard where coverage is below it (leaf cards)
uniform vec3 emissive;    // added after lighting, before fog
uniform float unlit;      // 1: the base colour as is, no light or reflection
uniform float fogOn;      // 0: fog does not reach this surface
// Which pass: 0 = opaque, alpha carries depth for the post stack; 1 = the
// transparent pass, alpha is coverage and the blend leaves the destination's
// alpha (the depth) alone.
uniform float alphaMode;
out vec4 finalColor;

// Analytic environment for image-based reflections: the same gradient sky we
// draw behind the scene, evaluated in an arbitrary direction, plus a sun disc.
// The lower hemisphere fades to a dim ground tone. No cubemap needed.
vec3 envColor(vec3 dir, vec3 sunDir, vec3 sunCol) {
  vec3 ground = mix(skyBot, vec3(0.13, 0.14, 0.12), 0.6);
  float up = dir.y;
  vec3 sky = mix(skyBot, skyTop, clamp(up, 0.0, 1.0));
  vec3 env = mix(ground, sky, smoothstep(-0.15, 0.05, up));
  float s = max(dot(dir, -sunDir), 0.0);
  env += sunCol * (pow(s, 350.0) * 1.4 + pow(s, 9.0) * 0.12);   // disc + soft glow
  return env;
}

bool inRange(vec3 p) {
  return p.z <= 1.0 && p.x >= 0.0 && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0;
}
float unpackDepth(vec4 c) {
  return dot(c, vec4(1.0, 1.0 / 255.0, 1.0 / 65025.0, 1.0 / 16581375.0));
}
float pcf(sampler2D sm, vec3 proj, float bias) {
  vec2 texel = 1.0 / vec2(textureSize(sm, 0));
  float lit = 0.0;
  for (int x = -1; x <= 1; x++)
    for (int y = -1; y <= 1; y++)
      lit += (proj.z - bias > unpackDepth(texture(sm, proj.xy + vec2(x, y) * texel))) ? 0.0 : 1.0;
  return lit / 9.0;
}
void main() {
  // texture0 defaults to a 1x1 white texture, so untextured shapes are
  // unaffected (white * colour). A material can swap in a real texture.
  vec4 tex = texture(texture0, fragUV);
  float cover = opacity * tex.a;
  if (cutoff > 0.0 && cover < cutoff) discard;
  vec3 N = normalize(fragNormal);
  if (normalStrength > 0.0) {
    vec3 dp1 = dFdx(fragWorld), dp2 = dFdy(fragWorld);
    vec2 duv1 = dFdx(fragUV), duv2 = dFdy(fragUV);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    vec3 nm = texture(normalMap, fragUV).xyz * 2.0 - 1.0;
    nm.xy *= normalStrength;
    N = normalize(mat3(T * invmax, B * invmax, N) * normalize(nm));
  }
  vec3 L = -normalize(lightDir);
  float d = max(dot(N, L), 0.0);

  // Cascaded directional shadow: use the first cascade that contains the
  // fragment — near cascade for crisp close shadows, far for the rest.
  float bias = max(0.0035 * (1.0 - d), 0.0012);
  vec3 wp = fragWorld;
  float shadowLit = 1.0;
  vec4 l0 = lightVP0 * vec4(wp, 1.0);
  vec3 p0 = l0.xyz / l0.w * 0.5 + 0.5;
  if (inRange(p0)) {
    shadowLit = pcf(texture1, p0, bias);
  } else {
    vec4 l1 = lightVP1 * vec4(wp, 1.0);
    vec3 p1 = l1.xyz / l1.w * 0.5 + 0.5;
    if (inRange(p1)) shadowLit = pcf(texture2, p1, bias);
  }
  float shadow = mix(0.3, 1.0, shadowLit);

  vec3 base = fragColor.rgb * colDiffuse.rgb * tex.rgb;
  // PBR-ish: metals have little diffuse; rough surfaces have weak/broad spec.
  vec3 diffuse = base * (ambient + lightColor * d * shadow) * mix(1.0, 0.25, metallic);

  vec3 V = normalize(viewPos - fragWorld);
  vec3 H = normalize(L + V);
  float shininess = mix(8.0, 120.0, 1.0 - roughness);
  float specStr = mix(0.05, 1.0, metallic) * (1.0 - roughness);
  vec3 specTint = mix(vec3(1.0), base, metallic);   // metals tint the highlight
  vec3 spec = specTint * lightColor *
              (pow(max(dot(N, H), 0.0), shininess) * specStr * shadow * step(0.0001, d));
  vec3 col = diffuse + spec;

  // --- image-based reflection: mirror the environment off the surface -------
  // Fresnel-weighted (metals reflect by base colour, dielectrics get a grazing
  // sheen); rough surfaces blur toward the average sky and reflect less.
  vec3 R = reflect(-V, N);
  vec3 envAvg = (skyTop + skyBot) * 0.5;
  vec3 env = mix(envColor(R, lightDir, lightColor), envAvg, roughness);
  vec3 F0 = mix(vec3(0.04), base, metallic);
  float ct = max(dot(N, V), 0.0);
  vec3 F = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - ct, 5.0);
  col += env * F * mix(1.0, 0.35, roughness);
  col = mix(col, base, unlit) + emissive;

  // Distance fog toward fogColor (off when fogStart is set very large).
  float dist = length(fragWorld - viewPos);
  float fogF = clamp((dist - fogStart) / max(fogEnd - fogStart, 1.0), 0.0, 1.0);
  col = mix(col, fogColor, fogF * fogOn);

  // Opaque pass: alpha carries linear camera depth (0=near .. 1=far) for the
  // post pass's SSAO + depth-of-field — packing it here avoids a second
  // sampler on macOS. Transparent pass: alpha is the coverage the blend uses.
  finalColor = alphaMode < 0.5 ? vec4(col, clamp(dist / 3000.0, 0.0, 1.0))
                               : vec4(col, cover);
}
)";

// Depth pass shaders. We write depth into the COLOR target as a packed RGBA
// value and sample THAT in the lit pass — sampling a real depth texture as a
// plain sampler2D returns a "zero texture" on this GL path (raylib warns
// "texture unloadable"), which made the whole scene read as shadowed.
static const char* kVS_DEPTH = R"(#version 330
in vec3 vertexPosition;
uniform mat4 mvp;
void main() { gl_Position = mvp * vec4(vertexPosition, 1.0); }
)";
static const char* kFS_DEPTH = R"(#version 330
out vec4 c;
void main() {
  float d = gl_FragCoord.z;
  vec4 e = vec4(d, fract(d * 255.0), fract(d * 65025.0), fract(d * 16581375.0));
  e -= e.yzww * vec4(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 0.0);
  c = e;
}
)";

// Post-process (composite) fragment shader: cheap bloom (bright-pass bleed) +
// exposure + saturation + a soft exposure-style tonemap, for the warm "broadcast"
// look. Pairs with raylib's default 2D vertex shader (vs = nullptr).
static const char* kFS_POST = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;     // .rgb = lit scene, .a = linear camera depth
uniform float aaScale;          // supersample factor: keep screen-space radii constant
// The knobs (View::exposure and friends); the defaults are the numbers the
// look was tuned with, and a script that never touches them gets that look.
uniform float dofStrength;      // 0.85
uniform float dofRange;         // 3.5
uniform float ssaoStrength;     // 0.45
uniform float ssaoRadius;       // 3.0 (texels at aaScale)
uniform float bloomThreshold;   // 0.7
uniform float bloomStrength;    // 1.5
uniform float exposure;         // 1.35
uniform float saturation;       // 1.10
uniform float vignette;         // 0
// Colour grading: a 3D LUT as a horizontal strip of lutSize slices, each
// lutSize x lutSize (blue selects the slice, red runs across it, green down).
uniform sampler2D lutTex;
uniform float lutSize;          // 0 = no LUT
uniform float lutAmount;        // 0..1
out vec4 finalColor;
void main() {
  vec2 texel = aaScale / vec2(textureSize(texture0, 0));
  vec4 center = texture(texture0, fragTexCoord);
  float depth = center.a;
  float focus = texture(texture0, vec2(0.5, 0.5)).a;   // auto-focus on screen centre

  // --- depth of field: gentle blur by circle-of-confusion from |depth-focus|.
  // Subtle (cinematic), so the track stays readable; only the far background
  // and the very near foreground soften.
  float coc = clamp(abs(depth - focus) * dofRange, 0.0, 1.0);
  vec3 c = center.rgb;
  if (coc > 0.05 && dofStrength > 0.0) {
    vec3 acc = vec3(0.0);
    for (int i = 0; i < 8; i++) {
      float a = float(i) * 0.7853982;
      acc += texture(texture0, fragTexCoord + vec2(cos(a), sin(a)) * texel * coc * 2.0).rgb;
    }
    c = mix(c, acc / 8.0, coc * dofStrength);
  }

  // --- SSAO (depth-only): darken where neighbours sit nearer the camera ----
  // Depth lives in an 8-bit alpha channel, so its quantization step is ~1/256.
  // The occlusion bias must clear that step, else iso-depth quantization
  // contours show up as banding lines across flat ground. Real geometry edges
  // jump far more than the bias, so contact AO survives.
  float occ = 0.0;
  for (int i = 0; i < 8; i++) {
    float a = float(i) * 0.7853982;
    float nd = texture(texture0, fragTexCoord + vec2(cos(a), sin(a)) * texel * ssaoRadius).a;
    occ += step(nd, depth - 0.008);
  }
  c *= 1.0 - (occ / 8.0) * ssaoStrength;

  // --- bloom (wide bright-pass bleed for a soft glow) ---
  vec3 bloom = vec3(0.0);
  for (int x = -2; x <= 2; x++)
    for (int y = -2; y <= 2; y++)
      bloom += max(texture(texture0, fragTexCoord + vec2(x, y) * texel * 4.5).rgb - bloomThreshold, vec3(0.0));
  c += (bloom / 25.0) * bloomStrength;

  // --- tonemap + saturation + vignette ---
  c = vec3(1.0) - exp(-c * exposure);
  float l = dot(c, vec3(0.299, 0.587, 0.114));
  c = mix(vec3(l), c, saturation);
  c *= 1.0 - vignette * smoothstep(0.35, 0.85, distance(fragTexCoord, vec2(0.5)));

  // --- colour grading through the LUT, the last word on the colour ---
  if (lutSize > 0.5) {
    float n = lutSize;
    vec3 q = clamp(c, 0.0, 1.0) * (n - 1.0);
    float b0 = floor(q.b);
    float b1 = min(b0 + 1.0, n - 1.0);
    vec2 uv0 = vec2((b0 * n + q.r + 0.5) / (n * n), (q.g + 0.5) / n);
    vec2 uv1 = vec2((b1 * n + q.r + 0.5) / (n * n), (q.g + 0.5) / n);
    vec3 graded = mix(texture(lutTex, uv0).rgb, texture(lutTex, uv1).rgb, q.b - b0);
    c = mix(c, graded, lutAmount);
  }
  finalColor = vec4(c, 1.0);
}
)";

// Shadow map = a normal colour render target (RGBA8 colour + depth buffer).
// The depth pass writes packed depth into the colour texture, which we then
// sample reliably as a plain sampler2D in the lit pass.
static RenderTexture2D LoadShadowmap(int w, int h) {
  return LoadRenderTexture(w, h);
}

enum class Shape { Group, Box, Sphere, Cylinder, Plane, Mesh };

// GL names belong to the context that made them, and a View owns its context:
// closing one and opening the next starts the numbering over, so a mesh built
// under the old View holds ids that now name nothing — or name one of the new
// View's meshes. Nodes outlive their View (the script may hold one), so every
// View bumps this and a mesh records the value it was uploaded under.
// IsWindowReady() alone can't tell the two apart: it is true again as soon as
// the next View opens.
static uint64_t& gl_epoch() {
  static uint64_t epoch = 0;
  return epoch;
}

// GPU-upload an Image as a repeat-wrapped texture, consuming it. Mipmapped
// trilinear for tiled materials; plain bilinear for baked canvas drawings.
static Texture2D upload(::Image im, bool mipmaps) {
  Texture2D t = LoadTextureFromImage(im);
  if (mipmaps) GenTextureMipmaps(&t);
  SetTextureFilter(t, mipmaps ? TEXTURE_FILTER_TRILINEAR
                              : TEXTURE_FILTER_BILINEAR);
  SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
  UnloadImage(im);
  return t;
}

// A texture the script holds by handle: a plain upload (checker / grain / a
// baked canvas) or, while a canvas is open, the render target being drawn
// into. Its GL names belong to the View's context (gl_epoch), so a handle kept
// past view.drop() is inert — it samples as the 1x1 white, and its destructor
// frees nothing the next context could have reused. The typed handle is what
// keeps a texture out of node.material() and a material out of
// material.texture(): each is a TypeError at the call, where an integer id
// could only degrade to "untextured" and stay silent.
class Texture : public std::enable_shared_from_this<Texture> {
 public:
  Texture2D tex{};
  RenderTexture2D rt{};   // a canvas or render target: tex is rt.texture
  bool is_rt = false;
  // A render target is stored bottom-up. Rather than bake an upright copy
  // (a GPU -> CPU -> GPU round trip, unthinkable per frame for a mirror), the
  // flip is folded into wherever the texture is read: the material's UV
  // transform (emit) and a sprite's source rectangle. One flag, every reader.
  bool flip_v = false;
  uint64_t epoch = gl_epoch();

  Texture() = default;
  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;
  ~Texture() {
    if (!live() || !IsWindowReady()) return;
    if (is_rt) UnloadRenderTexture(rt); else UnloadTexture(tex);
  }
  bool live() const { return epoch == gl_epoch(); }
  double width() const { return tex.width; }
  double height() const { return tex.height; }
  // Sampling: "point" for pixel art and a LUT's exact cells, "bilinear",
  // "trilinear" (mipmapped — built here if the upload skipped them).
  Texture& filter(std::string name) {
    if (!live()) return *this;
    if (name == "point") SetTextureFilter(tex, TEXTURE_FILTER_POINT);
    else if (name == "trilinear") { GenTextureMipmaps(&tex); SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR); }
    else SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    return *this;
  }
  // Past the edge: "repeat" (a tiled road), "clamp" (a LUT, a sprite),
  // "mirror".
  Texture& wrap(std::string name) {
    if (!live()) return *this;
    SetTextureWrap(tex, name == "clamp" ? TEXTURE_WRAP_CLAMP
                        : name == "mirror" ? TEXTURE_WRAP_MIRROR_REPEAT
                                           : TEXTURE_WRAP_REPEAT);
    return *this;
  }
};

// How a transparent surface's colour meets what is already there. Each is a
// pair of RGB blend factors; the alpha factors are always (ZERO, ONE) so the
// destination's alpha — the depth the post stack reads — is left as the
// opaque pass wrote it. That is also the right answer: glass, a mirror plate
// or a blob shadow contributes no depth, so SSAO and DoF key off the solid
// geometry behind it.
enum class Blend { Over, Add, Multiply, Screen };

// A reusable material: tint + optional texture + PBR-ish response (metallic
// 0..1, roughness 0..1), plus what the surface opts out of. Defaults are a
// matte, opaque dielectric that casts a shadow and takes fog. The setters are
// fluent, so a material is one expression: view.add_material().rgb(…).pbr(…).
// Arguments are const: a material only reads the texture it is given, so the
// call must not stale the caller's other borrows of that texture.
class Material : public std::enable_shared_from_this<Material> {
 public:
  Color color = WHITE;
  std::shared_ptr<const Texture> tex;
  float metallic = 0.0f;
  float roughness = 0.85f;
  int64_t opacity_ = 255;
  float cutoff = 0.0f;
  Vector3 emissive_{0, 0, 0};
  bool unlit_ = false, double_sided_ = false, depth_write_ = true, depth_test_ = true;
  bool casts_shadow_ = true, fog_ = true;
  Blend blend_ = Blend::Over;
  Vector4 uv_{1, 1, 0, 0};   // scale.xy, offset.zw
  std::shared_ptr<const Texture> normal;
  float normal_strength = 0.0f;

  Material& rgb(int64_t r, int64_t g, int64_t b) { color = col(r, g, b); return *this; }
  // A tangent-space normal map (Scene.Image.to_normal makes one); nil removes it.
  Material& normal_map(const Texture* t, double strength) {
    normal = t ? t->shared_from_this() : nullptr;
    normal_strength = t ? (float)strength : 0.0f;
    return *this;
  }
  Material& pbr(double m, double r) { metallic = (float)m; roughness = (float)r; return *this; }
  Material& texture(const Texture* t) { tex = t ? t->shared_from_this() : nullptr; return *this; }
  Material& opacity(int64_t a) { opacity_ = chan(a); return *this; }
  Material& cutout(double t) { cutoff = (float)t; return *this; }
  Material& emissive(int64_t r, int64_t g, int64_t b, double k) { emissive_ = rgb01(r, g, b, k); return *this; }
  Material& unlit(bool on) { unlit_ = on; return *this; }
  Material& double_sided(bool on) { double_sided_ = on; return *this; }
  Material& depth_write(bool on) { depth_write_ = on; return *this; }
  Material& depth_test(bool on) { depth_test_ = on; return *this; }
  Material& casts_shadow(bool on) { casts_shadow_ = on; return *this; }
  Material& fog(bool on) { fog_ = on; return *this; }
  Material& uv(double us, double vs, double uo, double vo) {
    uv_ = Vector4{(float)us, (float)vs, (float)uo, (float)vo}; return *this;
  }
  Material& blend(std::string name) {
    if (name == "over") blend_ = Blend::Over;
    else if (name == "add") blend_ = Blend::Add;
    else if (name == "multiply") blend_ = Blend::Multiply;
    else if (name == "screen") blend_ = Blend::Screen;
    else TraceLog(LOG_ERROR, "Scene: blend('%s') names no mode (over / add / multiply / screen) — unchanged.", name.c_str());
    return *this;
  }
  // A blend other than plain "over" is transparent even at full opacity: it
  // reads what is behind it.
  bool transparent(int64_t node_opacity) const {
    return opacity_ * node_opacity < 255 * 255 || blend_ != Blend::Over;
  }
};

// A TTF/OTF rasterized at one pixel size into a glyph atlas (raylib's rtext,
// which is already linked — the same stb_truetype Canvas.Font builds on, one
// atlas per (font, size) instead of a glyph cache). Same context rule as a
// Texture: a handle kept past the View is inert and frees nothing.
class Font {
 public:
  ::Font font{};   // raylib's
  uint64_t epoch = gl_epoch();

  Font() = default;
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;
  ~Font() {
    if (live() && IsWindowReady()) UnloadFont(font);
  }
  bool live() const { return epoch == gl_epoch(); }
  double size() const { return font.baseSize; }
  int64_t glyphs() const { return font.glyphCount; }
};

// A CPU image: the baker for the procedural textures a scene is dressed with
// (liveries, signage, road grain, lamp glows, a colour-grading LUT) and the
// way a PNG's pixels come in. rtextures is plain CPU code, so none of this
// needs a window: an image can be built, read back pixel by pixel, or written
// out before any View exists, and it is the one part of Scene a test can run
// without a display. view.texture(img) is the upload. Always RGBA8, so get()
// and to_normal() read one layout.
class Image {
 public:
  ::Image im{};   // raylib's

  Image(int64_t w, int64_t h) : im(GenImageColor((int)w, (int)h, BLANK)) {}
  explicit Image(::Image i) : im(i) { ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8); }
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  ~Image() { UnloadImage(im); }

  static std::shared_ptr<Image> from_png(std::string bytes) {
    ::Image i = LoadImageFromMemory(".png", reinterpret_cast<const unsigned char*>(bytes.data()),
                                    (int)bytes.size());
    if (i.data == nullptr) throw std::runtime_error("Scene.Image.from_png: not a PNG");
    return std::make_shared<Image>(i);
  }

  double width() const { return im.width; }
  double height() const { return im.height; }
  // One pixel, packed 0xRRGGBBAA — for a test's exact answer, or a lookup.
  int64_t get(int64_t x, int64_t y) const {
    Color c = GetImageColor(im, (int)x, (int)y);
    return ((int64_t)c.r << 24) | ((int64_t)c.g << 16) | ((int64_t)c.b << 8) | c.a;
  }
  std::shared_ptr<Image> copy() const { return std::make_shared<Image>(ImageCopy(im)); }
  bool save_png(std::string path) const {
    return ExportImage(im, std::filesystem::absolute(path).string().c_str());
  }
  std::string to_png() const {
    int n = 0;
    unsigned char* p = ExportImageToMemory(im, ".png", &n);
    std::string out(reinterpret_cast<const char*>(p), (size_t)n);
    MemFree(p);
    return out;
  }

  // --- drawing (all fluent; alpha per call, the image having no shared one) ---
  Image& fill(int64_t r, int64_t g, int64_t b, int64_t a) { ImageClearBackground(&im, col(r, g, b, a)); return *this; }
  Image& pixel(int64_t x, int64_t y, int64_t r, int64_t g, int64_t b, int64_t a) {
    ImageDrawPixel(&im, (int)x, (int)y, col(r, g, b, a)); return *this;
  }
  Image& rect(int64_t x, int64_t y, int64_t w, int64_t h, int64_t r, int64_t g, int64_t b, int64_t a) {
    ImageDrawRectangle(&im, (int)x, (int)y, (int)w, (int)h, col(r, g, b, a)); return *this;
  }
  Image& rect_line(int64_t x, int64_t y, int64_t w, int64_t h, int64_t r, int64_t g, int64_t b, int64_t a) {
    ImageDrawRectangleLines(&im, (int)x, (int)y, (int)w, (int)h, col(r, g, b, a)); return *this;
  }
  Image& circle(int64_t x, int64_t y, int64_t radius, int64_t r, int64_t g, int64_t b, int64_t a) {
    ImageDrawCircle(&im, (int)x, (int)y, (int)radius, col(r, g, b, a)); return *this;
  }
  Image& circle_line(int64_t x, int64_t y, int64_t radius, int64_t r, int64_t g, int64_t b, int64_t a) {
    ImageDrawCircleLines(&im, (int)x, (int)y, (int)radius, col(r, g, b, a)); return *this;
  }
  Image& line(double x0, double y0, double x1, double y1, int64_t thick, int64_t r, int64_t g, int64_t b, int64_t a) {
    ImageDrawLineEx(&im, Vector2{(float)x0, (float)y0}, Vector2{(float)x1, (float)y1}, (int)thick,
                    col(r, g, b, a));
    return *this;
  }
  Image& triangle(double x0, double y0, double x1, double y1, double x2, double y2,
                  int64_t r, int64_t g, int64_t b, int64_t a) {
    ImageDrawTriangle(&im, Vector2{(float)x0, (float)y0}, Vector2{(float)x1, (float)y1},
                      Vector2{(float)x2, (float)y2}, col(r, g, b, a));
    return *this;
  }
  // Text: in a Font, or raylib's built-in font — which exists only once a
  // window does, so a nil font before any View draws nothing and says so.
  Image& text(std::string s, int64_t x, int64_t y, int64_t size, int64_t r, int64_t g, int64_t b, int64_t a,
              const Font* font, double spacing) {
    Color c = col(r, g, b, a);
    if (font && font->live()) {
      ImageDrawTextEx(&im, font->font, s.c_str(), Vector2{(float)x, (float)y}, (float)size,
                      (float)spacing, c);
    } else if (IsWindowReady()) {
      ImageDrawText(&im, s.c_str(), (int)x, (int)y, (int)size, c);
    } else {
      TraceLog(LOG_ERROR, "Scene.Image.text: the built-in font needs a window; pass a Font");
    }
    return *this;
  }
  // Generators, blended over the whole image. `direction` is degrees: 0 is
  // top to bottom, 90 left to right.
  Image& gradient(int64_t r1, int64_t g1, int64_t b1, int64_t r2, int64_t g2, int64_t b2, int64_t direction) {
    return over(GenImageGradientLinear(im.width, im.height, (int)direction, col(r1, g1, b1), col(r2, g2, b2)), 255);
  }
  // centre -> rim; `density` (0..1) is how far out the inner colour holds.
  Image& gradient_radial(double density, int64_t r1, int64_t g1, int64_t b1, int64_t r2, int64_t g2, int64_t b2) {
    return over(GenImageGradientRadial(im.width, im.height, (float)density, col(r1, g1, b1), col(r2, g2, b2)), 255);
  }
  // Perlin noise (`scale` = feature size, larger is smoother) mixed in at
  // `amount` (0..255) — the grain asphalt and grass are made of.
  Image& noise(int64_t seed, double scale, int64_t amount) {
    return over(GenImagePerlinNoise(im.width, im.height, (int)seed, (int)seed, (float)scale), amount);
  }
  // Worley cells of `tile` pixels, mixed in at `amount` — gravel, stone.
  Image& cellular(int64_t tile, int64_t amount) {
    return over(GenImageCellular(im.width, im.height, (int)(tile < 1 ? 1 : tile)), amount);
  }
  Image& blit(const Image& src, int64_t x, int64_t y, int64_t r, int64_t g, int64_t b, int64_t a) {
    ImageDrawImage(&im, src.im, (int)x, (int)y, col(r, g, b, a)); return *this;
  }
  // `src` turned `rot` degrees and scaled, its centre at (x, y).
  Image& blit_rot(const Image& src, double x, double y, double rot, double scale) {
    ImageDrawImageEx(&im, src.im, Vector2{(float)x, (float)y}, (float)rot, (float)scale, WHITE);
    return *this;
  }

  // --- whole-image passes ---
  Image& blur(int64_t radius) { ImageBlurGaussian(&im, (int)radius); return *this; }
  Image& tint(int64_t r, int64_t g, int64_t b) { ImageColorTint(&im, col(r, g, b)); return *this; }
  Image& invert() { ImageColorInvert(&im); return *this; }
  Image& grayscale() { ImageColorGrayscale(&im); return *this; }
  Image& brightness(int64_t k) { ImageColorBrightness(&im, (int)k); return *this; }
  Image& flip_v() { ImageFlipVertical(&im); return *this; }
  Image& flip_h() { ImageFlipHorizontal(&im); return *this; }
  Image& rotate(int64_t degrees) { ImageRotate(&im, (int)degrees); return *this; }
  Image& resize(int64_t w, int64_t h) { ImageResize(&im, (int)w, (int)h); return *this; }
  Image& crop(int64_t x, int64_t y, int64_t w, int64_t h) {
    ImageCrop(&im, Rectangle{(float)x, (float)y, (float)w, (float)h}); return *this;
  }
  // Height (the red channel) -> a tangent-space normal map, OpenGL convention
  // (+Y up), edges wrapping so a tiled texture tiles its normals too.
  Image& to_normal(double strength) {
    const int w = im.width, h = im.height;
    auto* px = static_cast<unsigned char*>(im.data);
    std::vector<unsigned char> out((size_t)w * h * 4);
    auto height_at = [&](int x, int y) {
      x = (x + w) % w;
      y = (y + h) % h;
      return px[((size_t)y * w + x) * 4] / 255.0;
    };
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        double dx = (height_at(x + 1, y) - height_at(x - 1, y)) * strength;
        double dy = (height_at(x, y + 1) - height_at(x, y - 1)) * strength;
        // Y follows +V (down the image), the axis the shader's bitangent runs along
        Vector3 n = Vector3Normalize(Vector3{(float)-dx, (float)-dy, 1.0f});
        auto* o = &out[((size_t)y * w + x) * 4];
        o[0] = (unsigned char)((n.x * 0.5 + 0.5) * 255.0 + 0.5);
        o[1] = (unsigned char)((n.y * 0.5 + 0.5) * 255.0 + 0.5);
        o[2] = (unsigned char)((n.z * 0.5 + 0.5) * 255.0 + 0.5);
        o[3] = 255;
      }
    }
    std::memcpy(px, out.data(), out.size());
    return *this;
  }

 private:
  // Composite a generated image over this one at `amount` alpha, consuming it.
  Image& over(::Image gen, int64_t amount) {
    ImageDrawImage(&im, gen, 0, 0, col(255, 255, 255, amount));
    UnloadImage(gen);
    return *this;
  }
};

// What the depth pass needs from the View: raylib's depth material and the
// cached primitive meshes. The lit pass does not draw through this — it
// collects DrawItems and emits them sorted (View::emit).
struct RenderCtx {
  ::Material& mat;   // raylib's, not gfx::Material
  const Mesh& cube;
  const Mesh& sphere;
  const Mesh& cyl;
  const Mesh& plane;
};

class Node;

// One draw of the lit pass, gathered from the scene graph and sorted before
// anything is emitted: by order, opaque before transparent, then front-to-back
// (early depth rejection) for opaque and back-to-front (correct blending) for
// transparent. A scene that sets no order and no opacity draws exactly as
// insertion order would — stable_sort keeps that as the tiebreak.
struct DrawItem {
  const Node* node;
  Matrix world;     // children inherit this
  Matrix draw;      // own mesh, shape dimensions folded in
  const Material* mat;   // may be null: the inline tint alone
  int64_t order;
  float depth;      // distance to the eye
  bool transparent;
  unsigned char opacity;   // node x material, 0..255
};

// The six planes of a view-projection, for culling a node's bounding sphere
// before it becomes a DrawItem. Gribb/Hartmann: each plane is a sum or
// difference of the matrix's fourth row with another. raymath names a
// Matrix's fields by column-major index but declares them in row order
// (m0, m4, m8, m12 is the first row), so rows are spelled by name — indexing
// from &m0 would hand back columns.
struct Frustum {
  Vector4 plane[6];
  static Frustum from(const Matrix& m) {
    Vector4 r0{m.m0, m.m4, m.m8, m.m12};
    Vector4 r1{m.m1, m.m5, m.m9, m.m13};
    Vector4 r2{m.m2, m.m6, m.m10, m.m14};
    Vector4 r3{m.m3, m.m7, m.m11, m.m15};
    auto add = [](Vector4 a, Vector4 b, float s) {
      Vector4 p{a.x + s * b.x, a.y + s * b.y, a.z + s * b.z, a.w + s * b.w};
      float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
      return Vector4{p.x / len, p.y / len, p.z / len, p.w / len};
    };
    Frustum f;
    f.plane[0] = add(r3, r0, 1);    // left
    f.plane[1] = add(r3, r0, -1);   // right
    f.plane[2] = add(r3, r1, 1);    // bottom
    f.plane[3] = add(r3, r1, -1);   // top
    f.plane[4] = add(r3, r2, 1);    // near
    f.plane[5] = add(r3, r2, -1);   // far
    return f;
  }
  bool visible(Vector3 c, float r) const {
    for (const Vector4& p : plane)
      if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) return false;
    return true;
  }
};

class Node : public std::enable_shared_from_this<Node> {
 public:
  Shape shape = Shape::Group;
  double w = 1, h = 1, d = 1, radius = 0.5;
  double px = 0, py = 0, pz = 0;
  double ex = 0, ey = 0, ez = 0;            // euler radians, ZYX
  double ax = 0, ay = 1, az = 0, ang = 0;   // axis-angle spin (radians)
  Quaternion quat_{0, 0, 0, 1};             // replaces the euler when set (quat())
  bool use_quat_ = false;
  bool billboard_ = false;                  // face the camera (collect)
  double cull_radius_ = 0;                  // 0 = from the shape, < 0 = never culled
  double scx = 1, scy = 1, scz = 1;
  std::weak_ptr<Node> parent_;              // set by push(); a root has none
  Color color = WHITE;
  std::shared_ptr<const Material> mat;   // null = the inline tint alone
  int64_t order = 0;          // draw order: lower first (SceneKit's renderingOrder)
  int64_t opacity_ = 255;     // multiplies the material's
  bool visible = true;
  std::string name;
  std::vector<std::shared_ptr<Node>> children;

  // custom-mesh accumulation (shape == Mesh)
  std::vector<float> mv;   // vertices xyz
  std::vector<float> mn;   // normals xyz
  std::vector<float> mt;   // texcoords uv
  std::vector<long> mi;    // indices (range-checked at build())
  bool mesh_built = false;
  ::Mesh mesh_{};   // GL ids + indices — see build(); the vertex data is ours
  uint64_t mesh_epoch_ = 0;   // the GL context mesh_ was uploaded under
  float mesh_radius_ = 0;     // farthest pushed vertex, for culling (build())

  // local-transform cache: recomputed only when a setter dirties it, so a
  // static subtree's matrix is built once, not 3x/frame across the passes.
  mutable Matrix cached_local_{};
  mutable bool local_dirty_ = true;

  ~Node() {
    if (mesh_built) unload_mesh();
  }
  // The node owns GL ids and a raylib-side index array (see build()); a copy
  // would free them twice. Everything holds nodes by shared_ptr.
  Node() = default;
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  // fluent setters (transform setters dirty the cached local matrix)
  Node& move(double x, double y, double z) { px = x; py = y; pz = z; local_dirty_ = true; return *this; }
  Node& euler(double x, double y, double z) { ex = x; ey = y; ez = z; local_dirty_ = true; return *this; }
  Node& yaw(double a) { ey = a; local_dirty_ = true; return *this; }
  Node& roll(double a) { ez = a; local_dirty_ = true; return *this; }
  Node& pitch(double a) { ex = a; local_dirty_ = true; return *this; }
  Node& spin(double x, double y, double z, double a) { ax = x; ay = y; az = z; ang = a; local_dirty_ = true; return *this; }
  Node& scale(double s) { scx = scy = scz = s; local_dirty_ = true; return *this; }
  Node& scale3(double x, double y, double z) { scx = x; scy = y; scz = z; local_dirty_ = true; return *this; }
  Node& tint(int64_t r, int64_t g, int64_t b) { color = col(r, g, b); return *this; }
  Node& material(const Material* m) { mat = m ? m->shared_from_this() : nullptr; return *this; }
  Node& set_order(int64_t n) { order = n; return *this; }
  Node& opacity(int64_t a) { opacity_ = chan(a); return *this; }
  Node& set_name(std::string n) { name = std::move(n); return *this; }
  Node& hide() { visible = false; return *this; }
  Node& show() { visible = true; return *this; }
  // A quaternion orientation — what an integrator hands over, and what a car
  // going over a crest needs where euler(ZYX) would gimbal-lock. Replaces the
  // euler angles; spin() still composes on top.
  Node& quat(double x, double y, double z, double w) {
    quat_ = QuaternionNormalize(Quaternion{(float)x, (float)y, (float)z, (float)w});
    use_quat_ = true;
    local_dirty_ = true;
    return *this;
  }
  Node& billboard(bool on) { billboard_ = on; return *this; }
  Node& cull_radius(double r) { cull_radius_ = r; return *this; }

  // --- the graph as data: parents, children, names, size --------------------
  // Detach from the parent (a root is removed through view.remove()). The
  // handle stays valid; the node just draws nowhere until re-added.
  Node& remove() {
    if (auto p = parent_.lock()) p->remove_child(this);
    else TraceLog(LOG_ERROR, "Scene: node.remove() on a root — use view.remove(node).");
    return *this;
  }
  void remove_child(const Node* child) {
    for (auto it = children.begin(); it != children.end(); ++it)
      if (it->get() == child) {
        (*it)->parent_.reset();
        children.erase(it);
        return;
      }
  }
  int64_t child_count() const { return (int64_t)children.size(); }
  std::shared_ptr<Node> child_at(int64_t i) const {
    if (i < 0 || (size_t)i >= children.size())
      throw std::runtime_error("Scene: child_at(" + std::to_string(i) + ") — the node has " +
                               std::to_string(children.size()) + " children");
    return children[(size_t)i];
  }
  // Depth-first by name(), this node's subtree excluded of itself. A name
  // nothing carries is an error rather than a nil handle: has() is the test.
  std::shared_ptr<Node> find(std::string n) const {
    if (auto f = search(n)) return f;
    throw std::runtime_error("Scene: no node named '" + n + "'");
  }
  bool has(std::string n) const { return search(n) != nullptr; }
  std::shared_ptr<Node> search(const std::string& n) const {
    for (const auto& ch : children) {
      if (ch->name == n) return ch;
      if (auto f = ch->search(n)) return f;
    }
    return nullptr;
  }
  // Vertices a custom mesh holds (pushed, or uploaded) — so a builder can start
  // a new node before the 65535 cap.
  int64_t vertex_count() const { return mesh_built ? mesh_.vertexCount : (int64_t)(mv.size() / 3); }
  // Position after every ancestor's transform — where the node is in the world.
  Matrix world() const {
    auto p = parent_.lock();
    return p ? MatrixMultiply(local(), p->world()) : local();
  }
  double world_x() const { return world().m12; }
  double world_y() const { return world().m13; }
  double world_z() const { return world().m14; }

  // custom-mesh builders (scalar push — no array marshalling needed)
  Node& vertex(double x, double y, double z, double nx, double ny, double nz) {
    return vertex_uv(x, y, z, nx, ny, nz, 0.0, 0.0);   // the zero-UV case
  }
  Node& vertex_uv(double x, double y, double z, double nx, double ny, double nz,
                  double u, double v) {
    mv.push_back((float)x); mv.push_back((float)y); mv.push_back((float)z);
    mn.push_back((float)nx); mn.push_back((float)ny); mn.push_back((float)nz);
    mt.push_back((float)u); mt.push_back((float)v);
    return *this;
  }
  Node& tri(int64_t a, int64_t b, int64_t c) {
    mi.push_back(a); mi.push_back(b); mi.push_back(c);
    return *this;
  }
  Node& build() {
    if (mv.empty() || mi.empty()) return *this;   // nothing pushed → no-op (no GL upload)
    if (mv.size() / 3 > 65535) {                   // raylib mesh indices are 16-bit
      TraceLog(LOG_ERROR,
               "Scene: mesh has %zu vertices; raylib's 16-bit index buffer "
               "caps at 65535 — split it across multiple nodes.", mv.size() / 3);
      return *this;   // refuse rather than silently wrap indices into garbage
    }
    // Every index must name a vertex that was pushed. That subsumes the 16-bit
    // range check (the cap above bounds the count), so the narrowing cast below
    // is the only place a value can reach the GPU and it is provably in range.
    int64_t verts = (long)(mv.size() / 3);
    for (int64_t i : mi) {
      if (i < 0 || i >= verts) {
        TraceLog(LOG_ERROR,
                 "Scene: triangle index %ld names no vertex (%ld pushed) — "
                 "mesh not built.", i, verts);
        return *this;
      }
    }
    // Upload straight from the node's own vectors; UploadMesh reads them once
    // and they are freed below. Indices are the exception — DrawMesh reads
    // that pointer as its draw-indexed flag, so that one array stays raylib's.
    // The Mesh then keeps GL ids plus two plain-heap arrays, so a node can
    // outlive its window (see unload_mesh). No Model: render() draws the Mesh.
    // (GL33-only: the GL 1.1 draw path reads mesh.vertices per frame — moot
    // while the lit shader is #version 330.)
    ::Mesh m{};
    m.vertexCount = (int)(mv.size() / 3);
    m.triangleCount = (int)(mi.size() / 3);
    m.vertices = mv.data();
    if (!mn.empty()) m.normals = mn.data();
    if (!mt.empty()) m.texcoords = mt.data();
    m.indices = (unsigned short*)MemAlloc((unsigned)(mi.size() * sizeof(unsigned short)));
    for (size_t i = 0; i < mi.size(); i++) m.indices[i] = (unsigned short)mi[i];
    UploadMesh(&m, false);
    m.vertices = nullptr;
    m.normals = nullptr;
    m.texcoords = nullptr;
    float r2 = 0;
    for (size_t i = 0; i + 2 < mv.size(); i += 3)
      r2 = std::max(r2, mv[i] * mv[i] + mv[i + 1] * mv[i + 1] + mv[i + 2] * mv[i + 2]);
    mesh_radius_ = std::sqrt(r2);
    if (mesh_built) unload_mesh();   // rebuilt: drop the older upload
    mesh_ = m;
    mesh_epoch_ = gl_epoch();
    mesh_built = true;
    // the data now lives in the GPU mesh; release the CPU-side build buffers
    std::vector<float>().swap(mv);
    std::vector<float>().swap(mn);
    std::vector<float>().swap(mt);
    std::vector<long>().swap(mi);
    return *this;
  }

  // Shape factories — single source for each shape's defaults. Spheres and
  // cylinders use the View's fixed-tessellation meshes (no per-node segments).
  static std::shared_ptr<Node> make_box(double w, double h, double d) {
    auto n = std::make_shared<Node>(); n->shape = Shape::Box; n->w = w; n->h = h; n->d = d; return n;
  }
  static std::shared_ptr<Node> make_sphere(double r) {
    auto n = std::make_shared<Node>(); n->shape = Shape::Sphere; n->radius = r; return n;
  }
  static std::shared_ptr<Node> make_cylinder(double r, double h) {
    auto n = std::make_shared<Node>(); n->shape = Shape::Cylinder; n->radius = r; n->h = h; return n;
  }
  static std::shared_ptr<Node> make_plane(double w, double d) {
    auto n = std::make_shared<Node>(); n->shape = Shape::Plane; n->w = w; n->d = d; return n;
  }

  std::shared_ptr<Node> child() { return push(std::make_shared<Node>()); }
  std::shared_ptr<Node> add_box(double w, double h, double d) { return push(make_box(w, h, d)); }
  std::shared_ptr<Node> add_sphere(double r) { return push(make_sphere(r)); }
  std::shared_ptr<Node> add_cylinder(double r, double h) { return push(make_cylinder(r, h)); }
  std::shared_ptr<Node> add_plane(double w, double d) { return push(make_plane(w, d)); }
  std::shared_ptr<Node> add_mesh() { auto n = std::make_shared<Node>(); n->shape = Shape::Mesh; return push(n); }

  double x() const { return px; }
  double y() const { return py; }
  double z() const { return pz; }

  Matrix local() const {
    if (local_dirty_) {
      Matrix s = MatrixScale((float)scx, (float)scy, (float)scz);
      Matrix r = use_quat_ ? QuaternionToMatrix(quat_)
                           : MatrixRotateZYX(Vector3{(float)ex, (float)ey, (float)ez});
      Matrix t = MatrixTranslate((float)px, (float)py, (float)pz);
      Matrix m = MatrixMultiply(s, r);
      if (ang != 0) m = MatrixMultiply(m, MatrixRotate(Vector3{(float)ax, (float)ay, (float)az}, (float)ang));
      cached_local_ = MatrixMultiply(m, t);
      local_dirty_ = false;
    }
    return cached_local_;
  }

  // The primitive meshes are unit-sized; fold the node's shape dimensions in as
  // an innermost scale (applied to the vertex before the node transform).
  Matrix shape_scale() const {
    switch (shape) {
      case Shape::Box: return MatrixScale((float)w, (float)h, (float)d);
      case Shape::Sphere: return MatrixScale((float)radius, (float)radius, (float)radius);
      case Shape::Cylinder:  // GenMeshCylinder is base-at-origin; centre it on Y
        return MatrixMultiply(MatrixTranslate(0, -0.5f, 0),
                              MatrixScale((float)radius, (float)h, (float)radius));
      case Shape::Plane: return MatrixScale((float)w, 1.0f, (float)d);
      default: return MatrixIdentity();
    }
  }

  // The mesh this node draws with, or null for a group / a mesh that has no
  // GL side (never built, or built under a View that is gone).
  const Mesh* mesh_for(const RenderCtx& ctx) const {
    switch (shape) {
      case Shape::Box: return &ctx.cube;
      case Shape::Sphere: return &ctx.sphere;
      case Shape::Cylinder: return &ctx.cyl;
      case Shape::Plane: return &ctx.plane;
      case Shape::Mesh: return mesh_live() ? &mesh_ : nullptr;
      case Shape::Group: return nullptr;
    }
    return nullptr;
  }

  // The depth pass: draw the shadow casters, recursively, in the cheap order.
  // A material can opt out (a blob shadow, a mirror plate); a transparent one
  // casts nothing unless it is a cutout, whose shape is real.
  void render(const Matrix& parent, const RenderCtx& ctx) const {
    if (!visible) return;
    Matrix world = MatrixMultiply(local(), parent);          // children inherit this
    bool casts = !mat || (mat->casts_shadow_ &&
                          (!mat->transparent(opacity_) || mat->cutoff > 0.0f));
    if (casts)
      if (const Mesh* m = mesh_for(ctx))
        DrawMesh(*m, ctx.mat, shape == Shape::Mesh ? world : MatrixMultiply(shape_scale(), world));
    for (const auto& ch : children) ch->render(world, ctx);
  }

  // The bounding sphere's radius in local units, for culling: the shape's own
  // (a mesh's farthest vertex), unless cull_radius() says otherwise.
  double bound_radius() const {
    if (cull_radius_ != 0) return cull_radius_;
    switch (shape) {
      case Shape::Box: return 0.5 * std::sqrt(w * w + h * h + d * d);
      case Shape::Sphere: return radius;
      case Shape::Cylinder: return std::sqrt(radius * radius + h * h * 0.25);
      case Shape::Plane: return 0.5 * std::sqrt(w * w + d * d);
      case Shape::Mesh: return mesh_radius_;
      case Shape::Group: return 0;
    }
    return 0;
  }

  // The lit pass, first half: one DrawItem per drawable node, depth-first, so
  // View::emit can sort them (see DrawItem). `cam_rot` is the camera's rotation
  // for billboards; `fr` the frustum to cull against, or null for none.
  void collect(const Matrix& parent, const RenderCtx& ctx, const Camera3D& cam, const Matrix& cam_rot,
               const Frustum* fr, std::vector<DrawItem>& out) const {
    if (!visible) return;
    Matrix world = MatrixMultiply(local(), parent);
    if (billboard_) {   // the node's rotation replaced by the camera's, in the world
      Matrix s = MatrixScale((float)scx, (float)scy, (float)scz);
      Matrix t = MatrixTranslate(world.m12, world.m13, world.m14);
      world = MatrixMultiply(MatrixMultiply(s, cam_rot), t);
    }
    if (mesh_for(ctx)) {
      Vector3 pos{world.m12, world.m13, world.m14};
      bool culled = false;
      if (fr && cull_radius_ >= 0) {
        // The world matrix's largest axis scale, so a scaled node's sphere grows.
        float sx = Vector3Length(Vector3{world.m0, world.m1, world.m2});
        float sy = Vector3Length(Vector3{world.m4, world.m5, world.m6});
        float sz = Vector3Length(Vector3{world.m8, world.m9, world.m10});
        culled = !fr->visible(pos, (float)bound_radius() * std::max({sx, sy, sz}));
      }
      if (!culled) {
        int64_t op = mat ? opacity_ * mat->opacity_ / 255 : opacity_;
        out.push_back(DrawItem{
            .node = this,
            .world = world,
            .draw = shape == Shape::Mesh ? world : MatrixMultiply(shape_scale(), world),
            .mat = mat.get(),
            .order = order,
            .depth = Vector3Distance(pos, cam.position),
            .transparent = mat ? mat->transparent(opacity_) : op < 255,
            .opacity = (unsigned char)op});
      }
    }
    for (const auto& ch : children) ch->collect(world, ctx, cam, cam_rot, fr, out);
  }

 private:
  std::shared_ptr<Node> push(std::shared_ptr<Node> n) {
    n->parent_ = shared_from_this();
    children.push_back(n);
    return n;
  }

  // Has this mesh's GL side survived? It dies with the View that uploaded it
  // (see gl_epoch), and drawing with ids the current context reused would draw
  // some other node's geometry. A stale mesh draws nothing, like one whose
  // build() was refused.
  bool mesh_live() const { return mesh_built && mesh_epoch_ == gl_epoch(); }

  // Only the GL ids need the window — and only the context that made them, or
  // the delete lands on whatever the new one has since given those ids to. The
  // vboId and index arrays are plain heap and are reclaimed either way — the
  // same split ~View makes for a material's maps. The vertex data was never
  // raylib's (build() uploads from this node's own vectors), so a node
  // outliving its View leaks nothing.
  void unload_mesh() {
    if (mesh_live() && IsWindowReady()) {
      UnloadMesh(mesh_);
    } else {
      MemFree(mesh_.vboId);
      MemFree(mesh_.indices);
    }
    mesh_ = ::Mesh{};
    mesh_built = false;
  }
};

// One texture contract for every surface: (0, 0) is an image's top-left, as
// in 2D, and a primitive shows the image upright with its top toward +Y.
// raylib's cube puts V = 0 at the geometric bottom (an image came out upside
// down); its par_shapes sphere and cylinder write (pole-to-pole, around) where
// a texture expects (around, top-down), the sphere also keeping its poles on
// Z; a plane already reads upright from +Z. GenMesh* has uploaded, so the
// changed buffers go back to the GPU (slot 0 vertices, 1 texcoords, 2 normals).
static Mesh push_buffer(Mesh m, int slot, float* data, int comps) {
  UpdateMeshBuffer(m, slot, data, m.vertexCount * comps * (int)sizeof(float), 0);
  return m;
}

static Mesh remap_uv(Mesh m, bool swap, bool flip_u, bool flip_v) {
  for (int i = 0; i < m.vertexCount; i++) {
    float* uv = &m.texcoords[i * 2];
    if (swap) std::swap(uv[0], uv[1]);
    if (flip_u) uv[0] = 1.0f - uv[0];
    if (flip_v) uv[1] = 1.0f - uv[1];
  }
  return push_buffer(m, 1, m.texcoords, 2);
}

static Mesh poles_to_y(Mesh m) {   // -90 degrees about X: (x, y, z) -> (x, z, -y)
  for (float* p : {m.vertices, m.normals})
    for (int i = 0; i < m.vertexCount; i++) {
      float y = p[i * 3 + 1];
      p[i * 3 + 1] = p[i * 3 + 2];
      p[i * 3 + 2] = -y;
    }
  push_buffer(m, 0, m.vertices, 3);
  return push_buffer(m, 2, m.normals, 3);
}

class View {
 public:
  View(int64_t w, int64_t h, std::string title) {
    // Quiet raylib's INFO/WARNING chatter (verbose GL/asset logs, and the
    // per-call gamepad-vibration "not available" warning on backends without
    // haptics). Keep errors only.
    SetTraceLogLevel(LOG_ERROR);
    SetTraceLogCallback(trace_log);  // keep a FATAL from exiting (see there)
    InitWindow((int)w, (int)h, title.c_str());
    // No window, no Scene: unlike Canvas (whose meaning lives in the CPU
    // framebuffer and which has a declared headless mode), every observable
    // thing a View does needs the GPU. wrap maps this to a RuntimeError the
    // script can catch — the Webview ctor's shape.
    if (!IsWindowReady())
      throw std::runtime_error(
          "Scene: cannot open a window (no usable display/GL)");
    ++gl_epoch();   // a fresh context: nothing an earlier View uploaded survives
    // raylib's own default: Esc alone force-closes the window, one frame too
    // late for a script to ask "are you sure?" — a pause menu cannot exist.
    // Esc is an ordinary key here (view.key("escape")); quit() is the exit.
    SetExitKey(KEY_NULL);
    ensure_audio();
    cam_.up = Vector3{0, 1, 0};
    cam_.fovy = 55;
    cam_.projection = CAMERA_PERSPECTIVE;
    lit_ = LoadShaderFromMemory(kVS, kFS);
    loc_dir_ = GetShaderLocation(lit_, "lightDir");
    loc_lcol_ = GetShaderLocation(lit_, "lightColor");
    loc_amb_ = GetShaderLocation(lit_, "ambient");
    loc_lvp0_ = GetShaderLocation(lit_, "lightVP0");
    loc_lvp1_ = GetShaderLocation(lit_, "lightVP1");
    loc_viewpos_ = GetShaderLocation(lit_, "viewPos");
    loc_fogcol_ = GetShaderLocation(lit_, "fogColor");
    loc_fogstart_ = GetShaderLocation(lit_, "fogStart");
    loc_fogend_ = GetShaderLocation(lit_, "fogEnd");
    loc_metallic_ = GetShaderLocation(lit_, "metallic");
    loc_rough_ = GetShaderLocation(lit_, "roughness");
    loc_skytop_ = GetShaderLocation(lit_, "skyTop");
    loc_skybot_ = GetShaderLocation(lit_, "skyBot");
    loc_opacity_ = GetShaderLocation(lit_, "opacity");
    loc_cutoff_ = GetShaderLocation(lit_, "cutoff");
    loc_emissive_ = GetShaderLocation(lit_, "emissive");
    loc_unlit_ = GetShaderLocation(lit_, "unlit");
    loc_fogon_ = GetShaderLocation(lit_, "fogOn");
    loc_alphamode_ = GetShaderLocation(lit_, "alphaMode");
    loc_uv_ = GetShaderLocation(lit_, "uvXform");
    loc_nstrength_ = GetShaderLocation(lit_, "normalStrength");
    // DrawMesh binds maps[i] to shader.locs[MAP_DIFFUSE + i]; raylib fills the
    // first three (texture0..2), so the normal map's slot is named here.
    lit_.locs[SHADER_LOC_MAP_ROUGHNESS] = GetShaderLocation(lit_, "normalMap");
    sky_top_ = Color{135, 165, 205, 255};   // default reflected sky until sky() is called
    sky_bot_ = Color{182, 202, 224, 255};
    set_sky_uniform();
    set_fog_uniform();
    depth_ = LoadShaderFromMemory(kVS_DEPTH, kFS_DEPTH);
    post_ = LoadShaderFromMemory(0, kFS_POST);   // default 2D VS + our composite FS
    loc_aascale_ = GetShaderLocation(post_, "aaScale");
    set_aa_uniform();
    loc_lut_ = GetShaderLocation(post_, "lutTex");
    loc_lutsize_ = GetShaderLocation(post_, "lutSize");
    loc_lutamount_ = GetShaderLocation(post_, "lutAmount");
    set_post_uniforms();
    alloc_targets((int)w * ss_, (int)h * ss_);
    shadowmap0_ = LoadShadowmap(2048, 2048);
    shadowmap1_ = LoadShadowmap(2048, 2048);
    mat_ = LoadMaterialDefault();
    mat_.shader = lit_;
    white_ = mat_.maps[MATERIAL_MAP_DIFFUSE].texture;   // default 1x1 white
    // Carry the cascades on the SPECULAR / NORMAL map slots (shader texture1 /
    // texture2) so raylib binds them through its working material path.
    mat_.maps[MATERIAL_MAP_SPECULAR].texture = shadowmap0_.texture;
    mat_.maps[MATERIAL_MAP_NORMAL].texture = shadowmap1_.texture;
    depth_mat_ = LoadMaterialDefault();
    depth_mat_.shader = depth_;
    light_.up = Vector3{0, 1, 0};
    light_.projection = CAMERA_ORTHOGRAPHIC;
    cube_ = remap_uv(GenMeshCube(1, 1, 1), false, false, true);
    sphere_ = remap_uv(poles_to_y(GenMeshSphere(1, 16, 16)), true, false, false);
    cyl_ = remap_uv(GenMeshCylinder(1, 1, 16), true, true, true);
    plane_ = GenMeshPlane(1, 1, 1, 1);
    set_sun_uniform();
  }
  ~View() {
    if (IsWindowReady()) {
      if (open_canvas_) EndTextureMode();  // no frame to finish, just unbind
      open_canvas_.reset();
      // Drop the scene while the window is up: ~Node frees each mesh, and a
      // material's texture with it when nothing else holds the handle. One the
      // script still holds outlives this View, and its destructor then sees the
      // epoch change and frees nothing.
      roots_.clear();
      UnloadRenderTexture(shadowmap0_);
      UnloadRenderTexture(shadowmap1_);
      UnloadRenderTexture(scene_rt_);
      UnloadRenderTexture(post_rt_);
      UnloadMesh(cube_); UnloadMesh(sphere_); UnloadMesh(cyl_); UnloadMesh(plane_);
      UnloadShader(lit_);
      UnloadShader(depth_);
      UnloadShader(post_);
      // NB: do NOT CloseAudioDevice() here. The device is a process-global
      // singleton (ensure_audio) shared with Sound/Music, which may outlive this
      // View; closing it would silence and leak still-live handles. It is
      // reclaimed at process exit. Sound/Music unload their own buffers.
      CloseWindow();
    }
    // Plain heap (raylib allocates one MaterialMap array per material), so it
    // is reclaimed even when the window is already gone. UnloadMaterial would
    // be wrong here: it also unloads the shader and every non-default texture,
    // including the shadowmaps parked in the SPECULAR/NORMAL slots that the
    // block above owns and has already unloaded.
    MemFree(mat_.maps);
    MemFree(depth_mat_.maps);
  }

  void target_fps(int64_t fps) { SetTargetFPS((int)fps); }
  // The close box, or the script's own quit() — Esc is no longer a third way.
  bool closing() const { return quit_ || WindowShouldClose(); }
  void quit() { quit_ = true; }
  double dt() const { return GetFrameTime(); }
  double width() const { return GetScreenWidth(); }
  double height() const { return GetScreenHeight(); }
  int64_t fps() const { return GetFPS(); }
  double time() const { return GetTime(); }

  // --- the window ---------------------------------------------------------
  // SDL applies these live; the render targets follow the new size at the next
  // render_3d() (sync_targets), so a fullscreen or dragged-out frame is drawn
  // at its own resolution rather than stretched from the old one.
  void fullscreen(bool on) { if (on != IsWindowFullscreen()) ToggleFullscreen(); }
  bool is_fullscreen() const { return IsWindowFullscreen(); }
  void resizable(bool on) {
    if (on) SetWindowState(FLAG_WINDOW_RESIZABLE); else ClearWindowState(FLAG_WINDOW_RESIZABLE);
  }
  bool resized() const { return IsWindowResized(); }   // one-frame edge
  void size(int64_t w, int64_t h) { SetWindowSize((int)w, (int)h); }
  void title(std::string s) { SetWindowTitle(s.c_str()); }
  void vsync(bool on) {
    if (on) SetWindowState(FLAG_VSYNC_HINT); else ClearWindowState(FLAG_VSYNC_HINT);
  }
  void cursor(bool on) { if (on) ShowCursor(); else HideCursor(); }
  // Captured: hidden and locked to the window, mouse_dx/dy still report motion
  // (a chase camera's mouse look).
  void mouse_capture(bool on) { if (on) DisableCursor(); else EnableCursor(); }
  std::string clipboard() const { const char* s = GetClipboardText(); return s ? s : ""; }
  void set_clipboard(std::string s) { SetClipboardText(s.c_str()); }

  // --- the post stack: each pass's strength, and a colour-grading LUT ------
  // The defaults are the look the shader was tuned with; 0 turns a pass off.
  void post(bool on) { post_on_ = on; }
  void exposure(double k) { look_.exposure = (float)k; set_post_uniforms(); }
  void saturation(double k) { look_.saturation = (float)k; set_post_uniforms(); }
  void bloom(double threshold, double strength) {
    look_.bloom_threshold = (float)threshold; look_.bloom_strength = (float)strength; set_post_uniforms();
  }
  void dof(double strength, double range) {
    look_.dof_strength = (float)strength; look_.dof_range = (float)range; set_post_uniforms();
  }
  void ssao(double strength, double radius) {
    look_.ssao_strength = (float)strength; look_.ssao_radius = (float)radius; set_post_uniforms();
  }
  void vignette(double k) { look_.vignette = (float)k; set_post_uniforms(); }
  // A 3D LUT as a horizontal strip: n slices of n x n, n = the texture's
  // height (a 4096 x 64 strip is 64 slices). Blue picks the slice, red runs
  // across it, green down. Built with Scene.Image, uploaded with mipmaps off;
  // the post pass samples it with point filtering so a cell is a cell.
  // `amount` blends the graded colour in; nil turns grading off.
  void lut(const Texture* tex, double amount) {
    lut_ = tex ? tex->shared_from_this() : nullptr;
    look_.lut_amount = (float)amount;
    set_post_uniforms();
  }

  // Supersample factor (1 = off, 2 = default); the whole scene and post pass
  // render at this multiple of the window and box-filter down. The single
  // largest performance knob for a scene-heavy frame.
  void supersample(int64_t n) {
    ss_ = (int)(n < 1 ? 1 : n > 4 ? 4 : n);
    set_aa_uniform();
    sync_targets();
  }
  // Near / far clip planes of the 3D pass (metres). The near plane is far out
  // by default: depth precision is what keeps coplanar road layers from
  // z-fighting at range, and a chase camera never sits within metres of
  // geometry. A cockpit camera does, and pulls it in here.
  void clip_planes(double near, double far) { near_ = (float)near; far_ = (float)far; }

  // --- the mouse: window points, buttons by name ----------------------------
  double mouse_x() const { return GetMousePosition().x; }
  double mouse_y() const { return GetMousePosition().y; }
  double mouse_dx() const { return GetMouseDelta().x; }
  double mouse_dy() const { return GetMouseDelta().y; }
  double mouse_wheel() const { return GetMouseWheelMove(); }
  bool mouse(std::string button) const {
    int b = culebra::keynames::mouse_button_of(button);
    return b >= 0 && IsMouseButtonDown(b);
  }
  bool mouse_pressed(std::string button) const {
    int b = culebra::keynames::mouse_button_of(button);
    return b >= 0 && IsMouseButtonPressed(b);
  }

  // Keys by name (stdlib/keynames.h — the vocabulary Canvas.key and
  // Term.read_key share); an unknown name is a key that is never pressed.
  bool key(std::string name) const {
    int c = culebra::keynames::key_code_of(name);
    return c != 0 && IsKeyDown(c);
  }
  bool key_pressed(std::string name) const {
    int c = culebra::keynames::key_code_of(name);
    return c != 0 && IsKeyPressed(c);
  }
  bool key_released(std::string name) const {
    int c = culebra::keynames::key_code_of(name);
    return c != 0 && IsKeyReleased(c);
  }
  // Gamepads by slot (0-3), buttons and axes by name (keynames.h); SDL's
  // mapping DB normalizes Xbox / DualSense / others to one layout.
  bool pad_available(int64_t index) const { return IsGamepadAvailable((int)index); }
  double pad_axis(std::string name, int64_t index) const {
    int a = culebra::keynames::pad_axis_of(name);
    return a < 0 ? 0.0 : (double)GetGamepadAxisMovement((int)index, a);
  }
  bool pad(std::string name, int64_t index) const {
    int b = culebra::keynames::pad_button_of(name);
    return b != GAMEPAD_BUTTON_UNKNOWN && IsGamepadButtonDown((int)index, b);
  }
  bool pad_pressed(std::string name, int64_t index) const {
    int b = culebra::keynames::pad_button_of(name);
    return b != GAMEPAD_BUTTON_UNKNOWN && IsGamepadButtonPressed((int)index, b);
  }
  std::string pad_name(int64_t index) const {
    const char* n = GetGamepadName((int)index);
    return n ? n : "";
  }
  // Load SDL_GameControllerDB mapping lines for pads the bundled DB lacks
  // (newer controllers). Returns 1 on success.
  int64_t gamepad_mappings(std::string db) { return SetGamepadMappings(db.c_str()); }
  // Rumble both motors at [0..1] for `sec` seconds. Works where the platform's
  // gamepad backend drives haptics (Windows/Linux XInput, Sony pads on macOS).
  // NOTE: Xbox controllers on macOS cannot rumble — no macOS API (SDL or Apple's
  // own GameController/CoreHaptics) drives their motors, so this is a no-op there.
  void rumble(double left, double right, double sec, int64_t index) {
    SetGamepadVibration((int)index, (float)left, (float)right, (float)sec);
  }
  void background(int64_t r, int64_t g, int64_t b) { bg_ = col(r, g, b); }
  // Vertical sky gradient (top -> horizon), drawn behind the 3D scene.
  void sky(int64_t tr, int64_t tg, int64_t tb, int64_t br, int64_t bg, int64_t bb) {
    sky_top_ = col(tr, tg, tb);
    sky_bot_ = col(br, bg, bb);
    has_sky_ = true;
    set_sky_uniform();
  }
  // Distance fog: surfaces fade to (r,g,b) between `start` and `end` metres.
  void fog(double start, double end, int64_t r, int64_t g, int64_t b) {
    fogcol_ = rgb01(r, g, b);
    fogstart_ = (float)start;
    fogend_ = (float)end;
    set_fog_uniform();
  }
  // Save the frame being drawn: call it between render_3d() / begin_2d() and
  // present(). After present() the buffer that was shown is gone — swapped
  // out, and on a double- or triple-buffered display what glReadPixels then
  // sees is a frame or two behind (the first one black) — so a shot taken
  // there is stale, not the frame the script just drew. raylib resolves a
  // relative path against its own base directory (beside the binary); a script
  // means the working directory, as FS does.
  void screenshot(std::string path) {
    if (!frame_open_)
      TraceLog(LOG_ERROR,
               "Scene: screenshot() outside a frame saves a stale buffer — "
               "call it after render_3d() / begin_2d() and before present().");
    rlDrawRenderBatchActive();   // the overlay drawn so far is still batched
    TakeScreenshot(std::filesystem::absolute(path).string().c_str());
  }

  // lighting
  void sun(double dx, double dy, double dz, double intensity, int64_t r, int64_t g, int64_t b) {
    Vector3 d = Vector3Normalize(Vector3{(float)dx, (float)dy, (float)dz});
    // Vector3Normalize leaves a zero vector alone, but the shader normalizes
    // it again and 0/0 unlights the whole scene. Refuse the call rather than
    // hand the GPU a NaN.
    if (d.x == 0 && d.y == 0 && d.z == 0) {
      TraceLog(LOG_ERROR,
               "Scene: sun() needs a direction to shine from; (0, 0, 0) names "
               "none — sun unchanged.");
      return;
    }
    dir_ = d;
    lcol_ = rgb01(r, g, b, intensity);
    set_sun_uniform();
  }
  void ambient(double intensity, int64_t r, int64_t g, int64_t b) {
    amb_ = rgb01(r, g, b, intensity);
    set_sun_uniform();
  }

  // --- materials + procedural textures --------------------------------------
  // Both are handles the script owns (see Texture / Material): a View hands
  // them out and never keeps them, so nothing here has to be looked up by id.
  std::shared_ptr<Material> add_material() { return std::make_shared<Material>(); }

  // Upload a CPU image as a mipmapped, repeat-wrapped texture. Mipmaps +
  // trilinear keep tiled high-frequency textures (e.g. the checker) from
  // crawling/shimmering when minified under motion. Consumes `im`.
  std::shared_ptr<Texture> register_texture(::Image im) {
    auto t = std::make_shared<Texture>();
    t->tex = upload(std::move(im), true);
    return t;
  }
  // Upload a CPU image the script drew (Scene.Image). Mipmapped + repeating by
  // default, for a tiled material; a sprite or a LUT turns both off.
  std::shared_ptr<Texture> texture(const Image& img, bool mipmaps, bool repeat) {
    auto t = std::make_shared<Texture>();
    t->tex = LoadTextureFromImage(img.im);
    if (mipmaps) GenTextureMipmaps(&t->tex);
    SetTextureFilter(t->tex, mipmaps ? TEXTURE_FILTER_TRILINEAR : TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(t->tex, repeat ? TEXTURE_WRAP_REPEAT : TEXTURE_WRAP_CLAMP);
    return t;
  }
  // A PNG's bytes (FS.read, or an Embed.dir asset) straight to a texture.
  std::shared_ptr<Texture> texture_png(std::string bytes) {
    return texture(*Image::from_png(std::move(bytes)), true, true);
  }
  // a checkerboard texture (px square, `checks` cells per side)
  std::shared_ptr<Texture> checker(int64_t px, int64_t checks, int64_t r1, int64_t g1, int64_t b1,
                                   int64_t r2, int64_t g2, int64_t b2) {
    int64_t n = checks > 0 ? checks : 1;          // guard: avoid div-by-zero / 0-size cells
    int cell = (int)(px / n);
    if (cell < 1) cell = 1;
    return register_texture(GenImageChecked((int)px, (int)px, cell, cell,
                                            col(r1, g1, b1), col(r2, g2, b2)));
  }
  // a flat colour with white-noise grain mixed in (asphalt / grass grain)
  std::shared_ptr<Texture> grain(int64_t px, int64_t r, int64_t g, int64_t b, int64_t amt) {
    ::Image im = GenImageColor((int)px, (int)px, col(r, g, b));
    ::Image noise = GenImageWhiteNoise((int)px, (int)px, 0.5f);
    ImageColorTint(&noise, col(amt, amt, amt));
    ImageDrawImage(&im, noise, 0, 0, Color{255, 255, 255, 60});
    UnloadImage(noise);
    return register_texture(im);
  }

  // A texture the scene is rendered into (render_to): the same GL object a
  // canvas is, kept for as long as the handle lives, so a mirror redraws into
  // it every frame. Clamped, not repeating — a target's edge pixels have no
  // business wrapping round.
  std::shared_ptr<Texture> render_target(int64_t w, int64_t h) {
    auto t = make_target(w, h);
    SetTextureWrap(t->tex, TEXTURE_WRAP_CLAMP);
    return t;
  }

  // Arbitrary texture drawn by the script with the 2D primitives: canvas(w,h)
  // opens an off-screen target (returns its texture); the following text/
  // rect/circle/line calls draw INTO it; canvas_end() finalises it. This is the
  // "generate a texture procedurally" path (liveries, signage) — like SceneKit
  // drawing textures with Core Graphics. One canvas at a time: a second one
  // would nest render targets, and there is no valid texture to hand back for
  // it, so it is the error the id era only logged.
  std::shared_ptr<Texture> canvas(int64_t w, int64_t h) {
    if (open_canvas_)
      throw std::runtime_error(
          "Scene: canvas() while a canvas is still open — call canvas_end() first");
    auto t = make_target(w, h);
    SetTextureWrap(t->tex, TEXTURE_WRAP_REPEAT);
    open_canvas_ = t;
    BeginTextureMode(t->rt);
    ClearBackground(WHITE);
    return t;
  }
  void canvas_end() {
    if (!open_canvas_) {
      TraceLog(LOG_ERROR, "Scene: canvas_end() with no canvas open. Ignored.");
      return;
    }
    EndTextureMode();
    open_canvas_.reset();   // the texture stays a target; its readers flip it (Texture::flip_v)
  }

  // Render the scene from another camera into a render_target — the lit pass
  // of render_3d() without the post stack, into `tex`. A rear-view mirror:
  // call it before render_3d() each frame, since the plate that samples the
  // texture is drawn there. Shadows are the cascades the last render_3d()
  // fitted around the main camera, which a mirror looking back down the same
  // road shares — its own two depth passes per frame would cost more than the
  // mirror is worth. `tex` is const: rendering into it changes GL state, not
  // the handle, and a mutable parameter would stale the caller's borrows.
  void render_to(const Texture& tex, double px, double py, double pz, double tx, double ty, double tz,
                 double ux, double uy, double uz, double fov) {
    if (!tex.is_rt || !tex.live()) {
      TraceLog(LOG_ERROR, "Scene: render_to() needs a render_target() of this view. Ignored.");
      return;
    }
    close_stray_canvas("render_to");
    if (frame_open_)
      TraceLog(LOG_ERROR,
               "Scene: render_to() inside a frame shows in the next one — call it before render_3d().");
    Camera3D cam{};
    cam.position = Vector3{(float)px, (float)py, (float)pz};
    cam.target = Vector3{(float)tx, (float)ty, (float)tz};
    cam.up = Vector3{(float)ux, (float)uy, (float)uz};
    cam.fovy = (float)fov;
    cam.projection = CAMERA_PERSPECTIVE;
    // The eye drives specular and fog in the lit shader; set for this pass,
    // put back for the main one (render_3d sets it too, belt and braces).
    SetShaderValue(lit_, loc_viewpos_, &cam.position, SHADER_UNIFORM_VEC3);
    rlSetClipPlanes(near_, far_);
    BeginTextureMode(tex.rt);
    ClearBackground(bg_);
    if (has_sky_)
      DrawRectangleGradientV(0, 0, tex.rt.texture.width, tex.rt.texture.height, sky_top_, sky_bot_);
    rlDisableColorBlend();
    BeginMode3D(cam);
    // Alpha is coverage here, not depth: no post pass reads this target, and a
    // sprite() of it (or anything else blending by alpha) must see the image
    // as opaque, not as 0.4% of itself.
    lit_pass(cam, /*depth_in_alpha=*/false);
    EndMode3D();
    rlEnableColorBlend();
    EndTextureMode();
    SetShaderValue(lit_, loc_viewpos_, &cam_.position, SHADER_UNIFORM_VEC3);
  }

  // --- fonts ------------------------------------------------------------------
  // A TTF/OTF at one pixel size. `chars` names the glyphs to rasterize ("" =
  // printable ASCII): a HUD that draws digits and a few words lists them and
  // gets a small atlas; one with Japanese lists the characters it uses. The
  // bytes form is for an embedded asset (Embed.dir), so a one-binary game
  // ships its font inside itself.
  std::shared_ptr<Font> font(std::string path, int64_t size, std::string chars) {
    return adopt_font([&](int* cps, int n) {
      return LoadFontEx(path.c_str(), (int)size, cps, n);
    }, chars, "font '" + path + "'");
  }
  std::shared_ptr<Font> font_bytes(std::string data, int64_t size, std::string chars) {
    return adopt_font([&](int* cps, int n) {
      return LoadFontFromMemory(".ttf", reinterpret_cast<const unsigned char*>(data.data()),
                                (int)data.size(), (int)size, cps, n);
    }, chars, "font from bytes");
  }

  std::shared_ptr<Node> add_node() { return push(std::make_shared<Node>()); }
  // Take a node out of the scene — a root, or a child through its parent. The
  // handle stays valid; the node just draws nowhere until re-added.
  void remove(const Node& n) {
    for (auto it = roots_.begin(); it != roots_.end(); ++it)
      if (it->get() == &n) { roots_.erase(it); return; }
    if (auto p = n.parent_.lock()) { p->remove_child(&n); return; }
    TraceLog(LOG_ERROR, "Scene: view.remove(): the node is not in this scene. Ignored.");
  }
  std::shared_ptr<Node> find(std::string name) const {
    for (const auto& r : roots_) {
      if (r->name == name) return r;
      if (auto f = r->search(name)) return f;
    }
    throw std::runtime_error("Scene: no node named '" + name + "'");
  }
  bool has(std::string name) const {
    for (const auto& r : roots_)
      if (r->name == name || r->search(name)) return true;
    return false;
  }
  // Frustum culling of nodes whose bounding sphere is off screen (default
  // on). Off is for measuring what it saves, or a shape whose bound is wrong.
  void culling(bool on) { culling_ = on; }
  std::shared_ptr<Node> add_box(double w, double h, double d) { return push(Node::make_box(w, h, d)); }
  std::shared_ptr<Node> add_sphere(double r) { return push(Node::make_sphere(r)); }
  std::shared_ptr<Node> add_cylinder(double r, double h) { return push(Node::make_cylinder(r, h)); }
  std::shared_ptr<Node> add_plane(double w, double d) { return push(Node::make_plane(w, d)); }
  std::shared_ptr<Node> add_mesh() {
    auto n = std::make_shared<Node>(); n->shape = Shape::Mesh; return push(n);
  }

  void camera(double px, double py, double pz, double tx, double ty, double tz,
              double ux, double uy, double uz, double fov) {
    cam_.position = Vector3{(float)px, (float)py, (float)pz};
    cam_.target = Vector3{(float)tx, (float)ty, (float)tz};
    cam_.up = Vector3{(float)ux, (float)uy, (float)uz};
    cam_.fovy = (float)fov;
  }

  // One shadow cascade: render scene depth from the sun over a square area
  // (metres) centred on `focus`, into `sm`. Returns the light view-projection.
  Matrix shadow_cascade(RenderTexture2D& sm, float area, Vector3 focus, Matrix id) {
    // Snap the cascade centre to whole shadow-texel steps. Without this the
    // ortho frustum slides by sub-texel amounts every frame as the camera
    // moves, and the shadow edges crawl/shimmer.
    float texel = area / (float)sm.texture.width;
    focus.x = roundf(focus.x / texel) * texel;
    focus.z = roundf(focus.z / texel) * texel;
    light_.fovy = area;
    light_.position = Vector3Subtract(focus, Vector3Scale(dir_, 300.0f));
    light_.target = focus;
    BeginTextureMode(sm);
    ClearBackground(WHITE);
    BeginMode3D(light_);
    Matrix lv = rlGetMatrixModelview();
    Matrix lp = rlGetMatrixProjection();
    auto ctx = pass_ctx(depth_mat_);
    for (const auto& n : roots_) n->render(id, ctx);
    EndMode3D();
    EndTextureMode();
    return MatrixMultiply(lv, lp);
  }

  // The lit pass, inside a BeginMode3D: collect from the camera's point of
  // view, then emit. Shared by render_3d() (whose target the post pass reads,
  // so opaque alpha carries depth) and render_to() (whose target is an image,
  // so alpha is coverage). The frustum and the camera's rotation come from the
  // matrices BeginMode3D just installed.
  void lit_pass(const Camera3D& cam, bool depth_in_alpha) {
    items_.clear();
    auto ctx = pass_ctx(mat_);
    Matrix id = MatrixIdentity();
    Matrix view = rlGetMatrixModelview();
    Frustum fr = Frustum::from(MatrixMultiply(view, rlGetMatrixProjection()));
    // The view's rotation, transposed: what a billboard turns by to face the eye.
    Matrix cam_rot = MatrixTranspose(view);
    cam_rot.m12 = cam_rot.m13 = cam_rot.m14 = 0;
    cam_rot.m3 = cam_rot.m7 = cam_rot.m11 = 0;
    for (const auto& n : roots_) n->collect(id, ctx, cam, cam_rot, culling_ ? &fr : nullptr, items_);
    emit(items_, depth_in_alpha);
  }

  // The lit pass, second half: sort the collected items (see DrawItem) and
  // draw them, setting only the GL state and uniforms that change between one
  // item and the next. DrawMesh is unbatched on GL 3.3, so a state change
  // between two draws lands exactly between them.
  void emit(std::vector<DrawItem>& items, bool depth_in_alpha) {
    std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
      if (a.order != b.order) return a.order < b.order;
      if (a.transparent != b.transparent) return !a.transparent;
      return a.transparent ? a.depth > b.depth : a.depth < b.depth;
    });
    auto ctx = pass_ctx(mat_);
    // The state the pass starts in (BeginMode3D's defaults + our blend-off).
    bool blending = false, depth_write = true, depth_test = true, culling = true;
    Blend blend = Blend::Over;
    for (const DrawItem& it : items) {
      const Material* m = it.mat;
      bool want_dw = m ? m->depth_write_ : true;
      bool want_dt = m ? m->depth_test_ : true;
      bool want_cull = !(m && m->double_sided_);
      Blend want_blend = m ? m->blend_ : Blend::Over;
      if (it.transparent != blending) {
        if (it.transparent) rlEnableColorBlend(); else rlDisableColorBlend();
        blending = it.transparent;
      }
      if (blending && (want_blend != blend || !blend_set_)) set_blend(want_blend), blend = want_blend;
      if (want_dw != depth_write) { if (want_dw) rlEnableDepthMask(); else rlDisableDepthMask(); depth_write = want_dw; }
      if (want_dt != depth_test) { if (want_dt) rlEnableDepthTest(); else rlDisableDepthTest(); depth_test = want_dt; }
      if (want_cull != culling) { if (want_cull) rlEnableBackfaceCulling(); else rlDisableBackfaceCulling(); culling = want_cull; }

      // Per-item uniforms and the diffuse map. A texture from a closed View
      // samples as white, the way a stale mesh draws nothing (see Texture).
      Color c = m ? m->color : it.node->color;
      Texture2D tex = white_;
      if (m && m->tex && m->tex->live()) tex = m->tex->tex;
      float metallic = m ? m->metallic : 0.0f, roughness = m ? m->roughness : 0.85f;
      float opacity = it.opacity / 255.0f, cutoff = m ? m->cutoff : 0.0f;
      float unlit = m && m->unlit_ ? 1.0f : 0.0f, fogon = m && !m->fog_ ? 0.0f : 1.0f;
      float alpha_mode = (it.transparent || !depth_in_alpha) ? 1.0f : 0.0f;
      Vector3 emissive = m ? m->emissive_ : Vector3{0, 0, 0};
      Vector4 uv = m ? m->uv_ : Vector4{1, 1, 0, 0};
      // A bottom-up texture (a canvas, a render target) reads upright through
      // v' = 1 - (v * vs + vo) = v * -vs + (1 - vo): the flip composes with the
      // material's own transform, so a mirror's uv(-1, 1, 1, 0) still mirrors.
      if (m && m->tex && m->tex->live() && m->tex->flip_v) { uv.y = -uv.y; uv.w = 1.0f - uv.w; }
      bool nm = m && m->normal && m->normal->live();
      float nstrength = nm ? m->normal_strength : 0.0f;
      mat_.maps[MATERIAL_MAP_DIFFUSE].color = c;
      mat_.maps[MATERIAL_MAP_DIFFUSE].texture = tex;
      mat_.maps[MATERIAL_MAP_ROUGHNESS].texture = nm ? m->normal->tex : white_;
      SetShaderValue(lit_, loc_nstrength_, &nstrength, SHADER_UNIFORM_FLOAT);
      SetShaderValue(lit_, loc_metallic_, &metallic, SHADER_UNIFORM_FLOAT);
      SetShaderValue(lit_, loc_rough_, &roughness, SHADER_UNIFORM_FLOAT);
      SetShaderValue(lit_, loc_opacity_, &opacity, SHADER_UNIFORM_FLOAT);
      SetShaderValue(lit_, loc_cutoff_, &cutoff, SHADER_UNIFORM_FLOAT);
      SetShaderValue(lit_, loc_emissive_, &emissive, SHADER_UNIFORM_VEC3);
      SetShaderValue(lit_, loc_unlit_, &unlit, SHADER_UNIFORM_FLOAT);
      SetShaderValue(lit_, loc_fogon_, &fogon, SHADER_UNIFORM_FLOAT);
      SetShaderValue(lit_, loc_alphamode_, &alpha_mode, SHADER_UNIFORM_FLOAT);
      SetShaderValue(lit_, loc_uv_, &uv, SHADER_UNIFORM_VEC4);
      DrawMesh(*it.node->mesh_for(ctx), mat_, it.draw);
    }
    // Leave the state as the pass found it, for the 2D overlay and the next pass.
    if (!depth_write) rlEnableDepthMask();
    if (!depth_test) rlEnableDepthTest();
    if (!culling) rlEnableBackfaceCulling();
    if (blending) { rlSetBlendMode(RL_BLEND_ALPHA); rlDisableColorBlend(); }
  }
  // RGB blend factors for a mode; alpha always (ZERO, ONE) — see Blend.
  void set_blend(Blend b) {
    int src = RL_SRC_ALPHA, dst = RL_ONE_MINUS_SRC_ALPHA;
    switch (b) {
      case Blend::Over: break;
      case Blend::Add: src = RL_SRC_ALPHA; dst = RL_ONE; break;
      case Blend::Multiply: src = RL_DST_COLOR; dst = RL_ONE_MINUS_SRC_ALPHA; break;
      case Blend::Screen: src = RL_ONE; dst = RL_ONE_MINUS_SRC_COLOR; break;
    }
    rlSetBlendFactorsSeparate(src, dst, RL_ZERO, RL_ONE, RL_FUNC_ADD, RL_FUNC_ADD);
    rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);
    blend_set_ = true;
  }

  void render_3d() {
    close_stray_canvas("render_3d");
    sync_targets();
    Matrix id = MatrixIdentity();
    Vector3 focus = cam_.target;

    // --- shadow passes: two cascades (near crisp, far wide) ----------------
    // Tight ortho clip range — the light sits ~300 m back and the scene fits
    // within a few hundred metres, so keep the depth range small for precision.
    rlSetClipPlanes(1.0, 1200.0);
    Matrix vp0 = shadow_cascade(shadowmap0_, 90.0f, focus, id);
    Matrix vp1 = shadow_cascade(shadowmap1_, 420.0f, focus, id);
    SetShaderValueMatrix(lit_, loc_lvp0_, vp0);
    SetShaderValueMatrix(lit_, loc_lvp1_, vp1);

    // camera position drives specular + fog distance in the lit shader
    Vector3 vp = cam_.position;
    SetShaderValue(lit_, loc_viewpos_, &vp, SHADER_UNIFORM_VEC3);

    // --- lit pass: render the scene into an off-screen target -------------
    // The near plane sits far out by default (see clip_planes): raylib's 0.01
    // crushes depth precision so coplanar layers (road / white lines / kerbs /
    // gravel) z-fight and flicker at range.
    rlSetClipPlanes(near_, far_);
    BeginTextureMode(scene_rt_);
    ClearBackground(bg_);
    if (has_sky_)   // gradient sky behind the scene (3D draws over it via depth)
      DrawRectangleGradientV(0, 0, scene_rt_.texture.width, scene_rt_.texture.height,
                             sky_top_, sky_bot_);
    // The lit shader writes linear depth into alpha; with alpha blending on,
    // near (low-alpha) fragments would blend away. Opaque items draw with no
    // blend; the transparent ones re-enable it with factors that leave the
    // destination's alpha alone (emit / set_blend).
    rlDisableColorBlend();
    BeginMode3D(cam_);
    // cascades are bound automatically via mat_.maps[SPECULAR/NORMAL]
    lit_pass(cam_, /*depth_in_alpha=*/true);
    EndMode3D();
    rlEnableColorBlend();
    EndTextureMode();

    // --- post pass at supersample resolution (SSAO/DoF/bloom/tonemap/LUT) ---
    int sw = scene_rt_.texture.width, sh = scene_rt_.texture.height;
    if (post_on_) {
      BeginTextureMode(post_rt_);
      BeginShaderMode(post_);
      // The LUT rides a second sampler, bound while the post shader is active.
      bool lut_live = lut_ && lut_->live();
      float lut_size = lut_live ? (float)lut_->tex.height : 0.0f;
      SetShaderValue(post_, loc_lutsize_, &lut_size, SHADER_UNIFORM_FLOAT);
      if (lut_live) SetShaderValueTexture(post_, loc_lut_, lut_->tex);
      // RenderTextures are y-flipped; a negative source height flips it upright.
      DrawTextureRec(scene_rt_.texture, Rectangle{0, 0, (float)sw, -(float)sh},
                     Vector2{0, 0}, WHITE);
      EndShaderMode();
      EndTextureMode();
    }

    // --- downsample the supersampled result to the window (the AA step) -----
    // The 2D overlay (HUD) is drawn by the caller AFTER this, so it stays crisp.
    BeginDrawing();
    frame_open_ = true;
    // With the post pass off the scene target is shown as is — with blending
    // off, since its alpha is depth, not coverage. The batch is flushed before
    // blending comes back so the state change lands after this draw.
    if (!post_on_) rlDisableColorBlend();
    DrawTexturePro((post_on_ ? post_rt_ : scene_rt_).texture,
                   Rectangle{0, 0, (float)sw, -(float)sh},
                   Rectangle{0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()},
                   Vector2{0, 0}, 0.0f, WHITE);
    if (!post_on_) { rlDrawRenderBatchActive(); rlEnableColorBlend(); }
  }
  // Open a frame for 2D-only screens (menus / pause), with no 3D or shadow
  // passes — pair with present(). (render_3d() opens the frame for 3D scenes.)
  void begin_2d() {
    close_stray_canvas("begin_2d");
    BeginDrawing();
    frame_open_ = true;
    ClearBackground(bg_);
  }
  void present() {
    EndDrawing();
    frame_open_ = false;
  }

  // --- 2D: the overlay after render_3d(), a whole frame after begin_2d(), or
  // a canvas() texture ---------------------------------------------------------
  // Alpha for subsequent 2D draws (0..255). The RGBA contract: 2D colours take
  // r,g,b and this shared alpha, so HUD fades / translucent panels are possible
  // without a 4-arg variant of every draw call. Reset to 255 when done.
  void alpha(int64_t a) { alpha_ = chan(a); }

  // Text in a Font, or raylib's built-in bitmap font when `font` is nil. The
  // no-font, no-spacing, no-rotation call is DrawText itself, so what a script
  // drew before fonts existed still lands on the same pixels.
  void text(std::string s, double x, double y, int64_t size, int64_t r, int64_t g, int64_t b,
            const Font* font, double spacing, double rot) {
    Color c = col(r, g, b, alpha_);
    if (!font_live(font) && spacing == 0.0 && rot == 0.0) {
      DrawText(s.c_str(), (int)x, (int)y, (int)size, c);
      return;
    }
    DrawTextPro(font_of(font), s.c_str(), Vector2{(float)x, (float)y}, Vector2{0, 0},
                (float)rot, (float)size, (float)spacing, c);
  }
  // The box `text` would cover — for right-aligning and centring a HUD. The
  // built-in font measures with the spacing DrawText applies (size / 10).
  double text_width(std::string s, int64_t size, const Font* font, double spacing) const {
    return measure(s, size, font, spacing).x;
  }
  double text_height(std::string s, int64_t size, const Font* font, double spacing) const {
    return measure(s, size, font, spacing).y;
  }

  void rect(double x, double y, double w, double h, int64_t r, int64_t g, int64_t b) {
    DrawRectangle((int)x, (int)y, (int)w, (int)h, col(r, g, b, alpha_));
  }
  void rect_line(double x, double y, double w, double h, double thick, int64_t r, int64_t g, int64_t b) {
    DrawRectangleLinesEx(rec(x, y, w, h), (float)thick, col(r, g, b, alpha_));
  }
  // `roundness` is 0 (square) .. 1 (the short side fully rounded).
  void rect_round(double x, double y, double w, double h, double roundness, int64_t r, int64_t g, int64_t b) {
    DrawRectangleRounded(rec(x, y, w, h), (float)roundness, 8, col(r, g, b, alpha_));
  }
  void rect_round_line(double x, double y, double w, double h, double roundness, double thick,
                       int64_t r, int64_t g, int64_t b) {
    DrawRectangleRoundedLinesEx(rec(x, y, w, h), (float)roundness, 8, (float)thick, col(r, g, b, alpha_));
  }
  // top -> bottom, or left -> right with `horizontal`.
  void rect_gradient(double x, double y, double w, double h, int64_t r1, int64_t g1, int64_t b1,
                     int64_t r2, int64_t g2, int64_t b2, bool horizontal) {
    Color a = col(r1, g1, b1, alpha_), z = col(r2, g2, b2, alpha_);
    if (horizontal) DrawRectangleGradientH((int)x, (int)y, (int)w, (int)h, a, z);
    else DrawRectangleGradientV((int)x, (int)y, (int)w, (int)h, a, z);
  }
  void circle(double x, double y, double radius, int64_t r, int64_t g, int64_t b) {
    DrawCircle((int)x, (int)y, (float)radius, col(r, g, b, alpha_));
  }
  void circle_line(double x, double y, double radius, int64_t r, int64_t g, int64_t b) {
    DrawCircleLines((int)x, (int)y, (float)radius, col(r, g, b, alpha_));
  }
  // centre -> rim: the radial gradient a lamp or a bulb texture is made of.
  void circle_gradient(double x, double y, double radius, int64_t r1, int64_t g1, int64_t b1,
                       int64_t r2, int64_t g2, int64_t b2) {
    DrawCircleGradient(Vector2{(float)x, (float)y}, (float)radius, col(r1, g1, b1, alpha_),
                       col(r2, g2, b2, alpha_));
  }
  // An arc band between two radii, from angle a0 to a1 in degrees (0 = +x,
  // clockwise on screen) — a tachometer sweep, a lap-progress dial.
  void ring(double x, double y, double r_in, double r_out, double a0, double a1,
            int64_t r, int64_t g, int64_t b) {
    DrawRing(Vector2{(float)x, (float)y}, (float)r_in, (float)r_out, (float)a0, (float)a1, 36,
             col(r, g, b, alpha_));
  }
  void line(double x0, double y0, double x1, double y1, double thick, int64_t r, int64_t g, int64_t b) {
    DrawLineEx(Vector2{(float)x0, (float)y0}, Vector2{(float)x1, (float)y1}, (float)thick, col(r, g, b, alpha_));
  }
  // Either winding: a HUD's flag triangle should not vanish for being spelled
  // clockwise, which is what the 3D pass's back-face culling would do to it.
  void triangle(double x0, double y0, double x1, double y1, double x2, double y2,
                int64_t r, int64_t g, int64_t b) {
    Vector2 pts[3] = {{(float)x0, (float)y0}, {(float)x1, (float)y1}, {(float)x2, (float)y2}};
    both_windings([&] { DrawTriangle(pts[0], pts[1], pts[2], col(r, g, b, alpha_)); });
  }
  // A regular polygon: `sides` around (x, y), `rot` in degrees.
  void poly(double x, double y, int64_t sides, double radius, double rot, int64_t r, int64_t g, int64_t b) {
    DrawPoly(Vector2{(float)x, (float)y}, (int)sides, (float)radius, (float)rot, col(r, g, b, alpha_));
  }

  // A texture as a 2D image: the whole of it into the w x h box at (x, y),
  // turned `rot` degrees about (ox, oy) inside that box, tinted (white = as
  // is); or a sub-rectangle of it, for an atlas. Both take the shared alpha.
  void sprite(const Texture& tex, double x, double y, double w, double h, double rot,
              double ox, double oy, int64_t r, int64_t g, int64_t b) {
    if (!tex.live()) return;
    DrawTexturePro(tex.tex, source_rec(tex, 0, 0, tex.tex.width, tex.tex.height), rec(x, y, w, h),
                   Vector2{(float)ox, (float)oy}, (float)rot, col(r, g, b, alpha_));
  }
  void sprite_rec(const Texture& tex, double sx, double sy, double sw, double sh,
                  double x, double y, double w, double h, double rot, double ox, double oy) {
    if (!tex.live()) return;
    DrawTexturePro(tex.tex, source_rec(tex, sx, sy, sw, sh), rec(x, y, w, h),
                   Vector2{(float)ox, (float)oy}, (float)rot, col(255, 255, 255, alpha_));
  }

  // Clip subsequent 2D draws to a rectangle (a scrolling list, a minimap
  // window) until clip_end().
  void clip(double x, double y, double w, double h) { BeginScissorMode((int)x, (int)y, (int)w, (int)h); }
  void clip_end() { EndScissorMode(); }

  // Paths by scalar push (wrap marshals no arrays): path_begin(), then path_to
  // per vertex, then one terminal call that draws and keeps the points for
  // another. path_fill is a fan from the first vertex, so it fills convex
  // shapes; a ribbon (a minimap's track, laid out as left/right pairs) is
  // path_strip; an outline is path_stroke; a smooth curve through the points is
  // path_spline.
  void path_begin() { path_.clear(); }
  void path_to(double x, double y) { path_.push_back(Vector2{(float)x, (float)y}); }
  void path_close() { if (!path_.empty()) path_.push_back(path_.front()); }
  void path_fill(int64_t r, int64_t g, int64_t b) {
    if (path_.size() < 3) return;
    both_windings([&] { DrawTriangleFan(path_.data(), (int)path_.size(), col(r, g, b, alpha_)); });
  }
  void path_strip(int64_t r, int64_t g, int64_t b) {
    if (path_.size() < 3) return;
    both_windings([&] { DrawTriangleStrip(path_.data(), (int)path_.size(), col(r, g, b, alpha_)); });
  }
  void path_stroke(double thick, int64_t r, int64_t g, int64_t b) {
    Color c = col(r, g, b, alpha_);
    for (size_t i = 1; i < path_.size(); i++) DrawLineEx(path_[i - 1], path_[i], (float)thick, c);
  }
  void path_spline(double thick, int64_t r, int64_t g, int64_t b) {
    if (path_.size() < 4) return;   // Catmull-Rom needs a point past each end
    DrawSplineCatmullRom(path_.data(), (int)path_.size(), (float)thick, col(r, g, b, alpha_));
  }

 private:
  // A canvas left open would nest render targets: the frame would be drawn
  // into the canvas instead of the screen, with nothing said about it. Finish
  // the canvas — it is the frame the caller is asking for that must proceed.
  void close_stray_canvas(const char* opener) {
    if (!open_canvas_) return;
    TraceLog(LOG_ERROR,
             "Scene: %s() with a canvas still open — ending the canvas first. "
             "Call canvas_end() when the texture is drawn.", opener);
    canvas_end();
  }
  std::shared_ptr<Node> push(std::shared_ptr<Node> n) { roots_.push_back(n); return n; }

  static Rectangle rec(double x, double y, double w, double h) {
    return Rectangle{(float)x, (float)y, (float)w, (float)h};
  }
  // A sprite's source rectangle in the texture's own storage: for a bottom-up
  // one, the region the caller means from the top sits at height - y - h
  // from the bottom, and a negative height tells DrawTexturePro to flip it.
  static Rectangle source_rec(const Texture& tex, double x, double y, double w, double h) {
    if (!tex.flip_v) return rec(x, y, w, h);
    return rec(x, tex.tex.height - y - h, w, -h);
  }
  // A render target sized w x h: colour + depth, bilinear, bottom-up (flip_v).
  static std::shared_ptr<Texture> make_target(int64_t w, int64_t h) {
    auto t = std::make_shared<Texture>();
    t->rt = LoadRenderTexture((int)w, (int)h);
    t->tex = t->rt.texture;
    t->is_rt = true;
    t->flip_v = true;
    SetTextureFilter(t->tex, TEXTURE_FILTER_BILINEAR);
    return t;
  }
  // A 2D fill drawn with back-face culling off, so its vertex order does not
  // matter. The batch is flushed around the toggle: rlgl queues geometry and
  // applies the GL state at the flush, so a toggle without one would take
  // effect on whatever was queued before it.
  template <class F>
  static void both_windings(F draw) {
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    draw();
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
  }
  static bool font_live(const Font* f) { return f && f->live(); }
  static ::Font font_of(const Font* f) { return font_live(f) ? f->font : GetFontDefault(); }
  static Vector2 measure(const std::string& s, int64_t size, const Font* font, double spacing) {
    // DrawText's own spacing rule for the built-in font, so the box matches.
    if (!font_live(font) && spacing == 0.0) spacing = (double)size / 10.0;
    return MeasureTextEx(font_of(font), s.c_str(), (float)size, (float)spacing);
  }
  // Load a font through `load(codepoints, count)`: `chars` empty = raylib's
  // printable-ASCII default. A failed load in raylib is not an error but a
  // font that is not the one asked for — the built-in one, or an empty
  // struct — which would silently give a HUD the wrong metrics. So both
  // shapes of "not ours" are the error here.
  template <class L>
  std::shared_ptr<Font> adopt_font(L load, const std::string& chars, const std::string& what) {
    int n = 0;
    int* cps = chars.empty() ? nullptr : LoadCodepoints(chars.c_str(), &n);
    auto f = std::make_shared<Font>();
    f->font = load(cps, n);
    if (cps) UnloadCodepoints(cps);
    if (f->font.texture.id == 0 || f->font.glyphCount == 0 ||
        f->font.texture.id == GetFontDefault().texture.id) {
      f->font = ::Font{};   // not ours to unload
      throw std::runtime_error("Scene: cannot load " + what);
    }
    return f;
  }
  // Supersampled targets: the scene + post render at ss_x the window, then
  // box-downsample to it for cheap, high-quality antialiasing.
  void alloc_targets(int w, int h) {
    scene_rt_ = LoadRenderTexture(w, h);
    post_rt_ = LoadRenderTexture(w, h);
    SetTextureFilter(post_rt_.texture, TEXTURE_FILTER_BILINEAR);   // averages on downscale
  }
  // Re-fit the targets when the window (or ss_) has changed since they were
  // made — the frame after a resize or a fullscreen toggle.
  void sync_targets() {
    int w = GetScreenWidth() * ss_, h = GetScreenHeight() * ss_;
    if (w == scene_rt_.texture.width && h == scene_rt_.texture.height) return;
    UnloadRenderTexture(scene_rt_);
    UnloadRenderTexture(post_rt_);
    alloc_targets(w, h);
  }
  void set_aa_uniform() {
    float aa = (float)ss_;
    SetShaderValue(post_, loc_aascale_, &aa, SHADER_UNIFORM_FLOAT);
  }
  // The post knobs are set on change, not per frame: they are uniforms of one
  // shader that nothing else touches. The LUT sampler is the exception (bound
  // in render_3d, while the shader is active).
  void set_post_uniforms() {
    auto set = [&](const char* name, float v) {
      SetShaderValue(post_, GetShaderLocation(post_, name), &v, SHADER_UNIFORM_FLOAT);
    };
    set("dofStrength", look_.dof_strength);
    set("dofRange", look_.dof_range);
    set("ssaoStrength", look_.ssao_strength);
    set("ssaoRadius", look_.ssao_radius);
    set("bloomThreshold", look_.bloom_threshold);
    set("bloomStrength", look_.bloom_strength);
    set("exposure", look_.exposure);
    set("saturation", look_.saturation);
    set("vignette", look_.vignette);
    SetShaderValue(post_, loc_lutamount_, &look_.lut_amount, SHADER_UNIFORM_FLOAT);
  }
  // Bind one pass's invariants. `m` is the pass's material (lit or depth).
  RenderCtx pass_ctx(::Material& m) {
    return RenderCtx{.mat = m, .cube = cube_, .sphere = sphere_, .cyl = cyl_, .plane = plane_};
  }
  void set_sun_uniform() {
    SetShaderValue(lit_, loc_dir_, &dir_, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit_, loc_lcol_, &lcol_, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit_, loc_amb_, &amb_, SHADER_UNIFORM_VEC3);
  }
  void set_fog_uniform() {
    SetShaderValue(lit_, loc_fogcol_, &fogcol_, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit_, loc_fogstart_, &fogstart_, SHADER_UNIFORM_FLOAT);
    SetShaderValue(lit_, loc_fogend_, &fogend_, SHADER_UNIFORM_FLOAT);
  }
  void set_sky_uniform() {
    Vector3 t{sky_top_.r / 255.0f, sky_top_.g / 255.0f, sky_top_.b / 255.0f};
    Vector3 b{sky_bot_.r / 255.0f, sky_bot_.g / 255.0f, sky_bot_.b / 255.0f};
    SetShaderValue(lit_, loc_skytop_, &t, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit_, loc_skybot_, &b, SHADER_UNIFORM_VEC3);
  }

  Camera3D cam_{};
  Color bg_ = SKYBLUE;
  Color sky_top_{}, sky_bot_{};
  bool has_sky_ = false;
  int64_t alpha_ = 255;        // shared alpha for 2D draws
  std::vector<std::shared_ptr<Node>> roots_;
  std::shared_ptr<Texture> open_canvas_;   // the canvas being drawn into, while open
  bool frame_open_ = false;   // between render_3d()/begin_2d() and present()
  bool quit_ = false;
  std::vector<Vector2> path_;   // the 2D path being built (path_begin / path_to)
  Texture2D white_{};
  Shader lit_{}, depth_{}, post_{};
  ::Material mat_{}, depth_mat_{};   // raylib's (gfx::Material is the script's)
  RenderTexture2D shadowmap0_{}, shadowmap1_{}, scene_rt_{}, post_rt_{};
  int ss_ = 2;              // supersample factor for antialiasing
  float near_ = 2.0f, far_ = 8000.0f;   // 3D clip planes, metres
  int loc_aascale_ = 0;
  int loc_lut_ = 0, loc_lutsize_ = 0, loc_lutamount_ = 0;
  bool post_on_ = true;
  struct Post {   // the look's tuned defaults (see kFS_POST)
    float dof_strength = 0.85f, dof_range = 3.5f;
    float ssao_strength = 0.45f, ssao_radius = 3.0f;
    float bloom_threshold = 0.7f, bloom_strength = 1.5f;
    float exposure = 1.35f, saturation = 1.10f, vignette = 0.0f;
    float lut_amount = 0.0f;
  } look_;
  std::shared_ptr<const Texture> lut_;
  Camera3D light_{};
  Mesh cube_{}, sphere_{}, cyl_{}, plane_{};
  int loc_dir_ = 0, loc_lcol_ = 0, loc_amb_ = 0;
  int loc_lvp0_ = 0, loc_lvp1_ = 0;
  int loc_viewpos_ = 0, loc_fogcol_ = 0, loc_fogstart_ = 0, loc_fogend_ = 0;
  int loc_skytop_ = 0, loc_skybot_ = 0;
  int loc_metallic_ = 0, loc_rough_ = 0;
  int loc_opacity_ = 0, loc_cutoff_ = 0, loc_emissive_ = 0, loc_unlit_ = 0, loc_fogon_ = 0;
  int loc_alphamode_ = 0, loc_uv_ = 0, loc_nstrength_ = 0;
  bool culling_ = true;
  std::vector<DrawItem> items_;   // the lit pass's draw list, reused each frame
  bool blend_set_ = false;        // set_blend has programmed the custom factors once
  Vector3 dir_ = Vector3{0.5f, -1.0f, -0.6f};
  Vector3 lcol_ = Vector3{1.0f, 0.98f, 0.94f};
  Vector3 amb_ = Vector3{0.35f, 0.38f, 0.42f};
  Vector3 fogcol_ = Vector3{0.7f, 0.8f, 0.9f};
  float fogstart_ = 1.0e9f;   // fog off until fog() is called
  float fogend_ = 2.0e9f;
};

// A short fully-decoded sound effect (WAV/OGG/MP3/FLAC) for one-shot playback —
// collisions, kerb strikes, UI blips. Cheap to fire repeatedly.
class SoundFx {
 public:
  SoundFx(std::string path) { ensure_audio(); snd_ = LoadSound(path.c_str()); }
  ~SoundFx() { if (IsAudioDeviceReady()) UnloadSound(snd_); }
  void play() { PlaySound(snd_); }
  void stop() { StopSound(snd_); }
  bool playing() const { return IsSoundPlaying(snd_); }
  void volume(double v) { SetSoundVolume(snd_, (float)v); }       // 0..1
  void pitch(double p) { SetSoundPitch(snd_, (float)p); }         // 1.0 = normal
  void pan(double p) { SetSoundPan(snd_, (float)p); }             // 0 left .. 1 right (0.5 center)
 private:
  Sound snd_{};
};

// A streamed audio track for int64_t / looping audio — engine note, ambience, BGM.
// Needs update() called each frame to keep the stream fed. Pitch drives the
// engine RPM effect; looping by default.
class MusicTrack {
 public:
  MusicTrack(std::string path) {
    ensure_audio();
    mus_ = LoadMusicStream(path.c_str());
    mus_.looping = true;
  }
  ~MusicTrack() { if (IsAudioDeviceReady()) UnloadMusicStream(mus_); }
  void play() { PlayMusicStream(mus_); }
  void stop() { StopMusicStream(mus_); }
  void pause() { PauseMusicStream(mus_); }
  void resume() { ResumeMusicStream(mus_); }
  void update() { UpdateMusicStream(mus_); }     // call once per frame
  bool playing() const { return IsMusicStreamPlaying(mus_); }
  void looping(bool on) { mus_.looping = on; }
  void volume(double v) { SetMusicVolume(mus_, (float)v); }       // 0..1
  void pitch(double p) { SetMusicPitch(mus_, (float)p); }         // 1.0 = normal (RPM)
  void pan(double p) { SetMusicPan(mus_, (float)p); }
 private:
  Music mus_{};
};

// A PCM stream the script synthesises itself: an engine note that follows
// the revs, brake noise, a beep — the sound of a game with no sound files.
//
// The script produces samples on the main thread and hands them over a
// block at a time (push / submit); the audio thread only ever copies a
// finished block. A script callback on that thread was never an option:
// wrap.h marshals no callables, the runtime is not reentrant from a foreign
// thread, and — had both been solved — a collector's pause inside a ~10 ms
// audio period is an audible dropout. raylib's own double-buffered stream
// is the right shape for this: needed() says when a block has drained.
//
// A machine with no audio device (CI) constructs one all the same: ready()
// is false and every call is a no-op, so a game runs silent rather than not.
class Audio {
 public:
  Audio(int64_t rate, int64_t channels, int64_t buffer)
      : rate_((int)(rate < 1 ? 1 : rate)),
        channels_(channels == 2 ? 2 : 1),
        buffer_((int)(buffer < 1 ? 1 : buffer)) {
    ensure_audio();
    if (!IsAudioDeviceReady()) return;
    // The device period (~10 ms) is the floor raylib clamps a sub-buffer up
    // to; below it, needed() would understate what a block holds and the
    // remainder would play as silence — hence the documented ~512 minimum.
    SetAudioStreamBufferSizeDefault(buffer_);
    stream_ = LoadAudioStream((unsigned)rate_, 32, (unsigned)channels_);   // 32-bit float
    ready_ = IsAudioStreamValid(stream_);
  }
  ~Audio() { if (ready_ && IsAudioDeviceReady()) UnloadAudioStream(stream_); }
  Audio(const Audio&) = delete;
  Audio& operator=(const Audio&) = delete;

  bool ready() const { return ready_; }
  // Frames the stream can take now: a whole block once one has drained.
  int64_t needed() const { return ready_ && IsAudioStreamProcessed(stream_) ? buffer_ : 0; }
  void push(double s) { add(s); }
  void push2(double l, double r) { add(l); if (channels_ == 2) add(r); }
  int64_t pending() const { return (int64_t)pend_.size() / channels_; }
  // Hand the pending block over. Nothing to hand, or nowhere to put it yet
  // (both blocks still full): 0, and the samples wait for the next call.
  int64_t submit() {
    int frames = (int)(pend_.size() / channels_);
    if (frames == 0) return 0;
    if (!ready_) { pend_.clear(); return 0; }
    if (!IsAudioStreamProcessed(stream_)) return 0;
    UpdateAudioStream(stream_, pend_.data(), frames);
    pend_.clear();
    return frames;
  }
  int64_t dropped() const { return dropped_; }
  double latency() const { return 2.0 * buffer_ / rate_; }

  void play() { if (ready_) PlayAudioStream(stream_); }
  void stop() { if (ready_) StopAudioStream(stream_); }
  void pause() { if (ready_) PauseAudioStream(stream_); }
  void resume() { if (ready_) ResumeAudioStream(stream_); }
  bool playing() const { return ready_ && IsAudioStreamPlaying(stream_); }
  void volume(double v) { if (ready_) SetAudioStreamVolume(stream_, (float)v); }
  void pitch(double p) { if (ready_) SetAudioStreamPitch(stream_, (float)p); }
  void pan(double p) { if (ready_) SetAudioStreamPan(stream_, (float)p); }

 private:
  // The pending block is capped at one block: a script that produces more
  // than the stream can take loses the surplus and can see that it did.
  void add(double s) {
    if ((int64_t)pend_.size() >= (int64_t)buffer_ * channels_) { dropped_++; return; }
    pend_.push_back((float)(s < -1.0 ? -1.0 : s > 1.0 ? 1.0 : s));
  }
  int rate_, channels_, buffer_;
  AudioStream stream_{};
  bool ready_ = false;
  std::vector<float> pend_;
  int64_t dropped_ = 0;
};

}  // namespace gfx

namespace {
const bool registered = [] {
  // raylib logs to stdout by default, and Scene.Image runs before any View has
  // had the chance to redirect it — an INFO line from a PNG decode would land
  // in a script's output. Route it from the start: errors only, to stderr.
  SetTraceLogLevel(LOG_ERROR);
  SetTraceLogCallback(gfx::trace_log);

  culebra::wrap<gfx::Texture>("Scene", "Texture")
      .method<&gfx::Texture::width>("width")
      .method<&gfx::Texture::height>("height")
      .borrowed_method<&gfx::Texture::filter>("filter", {"name"})
      .borrowed_method<&gfx::Texture::wrap>("wrap", {"name"});

  culebra::wrap<gfx::Image>("Scene", "Image")
      .ctor<long, long>({"w", "h"})
      .static_method<&gfx::Image::from_png>("from_png", {"bytes"})
      .method<&gfx::Image::width>("width")
      .method<&gfx::Image::height>("height")
      .method<&gfx::Image::get>("get", {"x", "y"})
      .method<&gfx::Image::copy>("copy")
      .method<&gfx::Image::save_png>("save_png", {"path"})
      .method<&gfx::Image::to_png>("to_png")
      .borrowed_method<&gfx::Image::fill>("fill", {"r", "g", "b", {"a", 255}})
      .borrowed_method<&gfx::Image::pixel>("pixel", {"x", "y", "r", "g", "b", {"a", 255}})
      .borrowed_method<&gfx::Image::rect>("rect", {"x", "y", "w", "h", "r", "g", "b", {"a", 255}})
      .borrowed_method<&gfx::Image::rect_line>("rect_line", {"x", "y", "w", "h", "r", "g", "b", {"a", 255}})
      .borrowed_method<&gfx::Image::circle>("circle", {"x", "y", "radius", "r", "g", "b", {"a", 255}})
      .borrowed_method<&gfx::Image::circle_line>("circle_line", {"x", "y", "radius", "r", "g", "b", {"a", 255}})
      .borrowed_method<&gfx::Image::line>("line", {"x0", "y0", "x1", "y1", "thick", "r", "g", "b", {"a", 255}})
      .borrowed_method<&gfx::Image::triangle>("triangle",
                                              {"x0", "y0", "x1", "y1", "x2", "y2", "r", "g", "b", {"a", 255}})
      .borrowed_method<&gfx::Image::text>("text", {"s", "x", "y", "size", "r", "g", "b", {"a", 255},
                                                   {"font", nullptr}, {"spacing", 0.0}})
      .borrowed_method<&gfx::Image::gradient>("gradient", {"r1", "g1", "b1", "r2", "g2", "b2", {"direction", 0}})
      .borrowed_method<&gfx::Image::gradient_radial>("gradient_radial",
                                                     {"density", "r1", "g1", "b1", "r2", "g2", "b2"})
      .borrowed_method<&gfx::Image::noise>("noise", {"seed", "scale", {"amount", 255}})
      .borrowed_method<&gfx::Image::cellular>("cellular", {"tile", {"amount", 255}})
      .borrowed_method<&gfx::Image::blit>("blit", {"src", "x", "y", {"r", 255}, {"g", 255}, {"b", 255}, {"a", 255}})
      .borrowed_method<&gfx::Image::blit_rot>("blit_rot", {"src", "x", "y", {"rot", 0.0}, {"scale", 1.0}})
      .borrowed_method<&gfx::Image::blur>("blur", {"radius"})
      .borrowed_method<&gfx::Image::tint>("tint", {"r", "g", "b"})
      .borrowed_method<&gfx::Image::invert>("invert")
      .borrowed_method<&gfx::Image::grayscale>("grayscale")
      .borrowed_method<&gfx::Image::brightness>("brightness", {"k"})
      .borrowed_method<&gfx::Image::flip_v>("flip_v")
      .borrowed_method<&gfx::Image::flip_h>("flip_h")
      .borrowed_method<&gfx::Image::rotate>("rotate", {"degrees"})
      .borrowed_method<&gfx::Image::resize>("resize", {"w", "h"})
      .borrowed_method<&gfx::Image::crop>("crop", {"x", "y", "w", "h"})
      .borrowed_method<&gfx::Image::to_normal>("to_normal", {{"strength", 1.0}});

  culebra::wrap<gfx::Font>("Scene", "Font")
      .method<&gfx::Font::size>("size")
      .method<&gfx::Font::glyphs>("glyphs");

  culebra::wrap<gfx::Material>("Scene", "Material")
      .borrowed_method<&gfx::Material::rgb>("rgb", {"r", "g", "b"})
      .borrowed_method<&gfx::Material::pbr>("pbr", {"metallic", "roughness"})
      .borrowed_method<&gfx::Material::texture>("texture", {"tex"})
      .borrowed_method<&gfx::Material::opacity>("opacity", {"a"})
      .borrowed_method<&gfx::Material::cutout>("cutout", {"threshold"})
      .borrowed_method<&gfx::Material::emissive>("emissive", {"r", "g", "b", {"k", 1.0}})
      .borrowed_method<&gfx::Material::unlit>("unlit", {{"on", true}})
      .borrowed_method<&gfx::Material::double_sided>("double_sided", {{"on", true}})
      .borrowed_method<&gfx::Material::depth_write>("depth_write", {"on"})
      .borrowed_method<&gfx::Material::depth_test>("depth_test", {"on"})
      .borrowed_method<&gfx::Material::casts_shadow>("casts_shadow", {"on"})
      .borrowed_method<&gfx::Material::fog>("fog", {"on"})
      .borrowed_method<&gfx::Material::blend>("blend", {"name"})
      .borrowed_method<&gfx::Material::uv>("uv", {"us", "vs", {"uo", 0.0}, {"vo", 0.0}})
      .borrowed_method<&gfx::Material::normal_map>("normal_map", {"tex", {"strength", 1.0}});

  culebra::wrap<gfx::Node>("Scene", "Node")
      .borrowed_method<&gfx::Node::move>("move", {"x", "y", "z"})
      .borrowed_method<&gfx::Node::euler>("euler", {"x", "y", "z"})
      .borrowed_method<&gfx::Node::yaw>("yaw", {"a"})
      .borrowed_method<&gfx::Node::roll>("roll", {"a"})
      .borrowed_method<&gfx::Node::pitch>("pitch", {"a"})
      .borrowed_method<&gfx::Node::spin>("spin", {"x", "y", "z", "a"})
      .borrowed_method<&gfx::Node::scale>("scale", {"s"})
      .borrowed_method<&gfx::Node::scale3>("scale3", {"x", "y", "z"})
      .borrowed_method<&gfx::Node::tint>("tint", {"r", "g", "b"})
      .borrowed_method<&gfx::Node::material>("material", {"m"})
      .borrowed_method<&gfx::Node::set_order>("order", {"n"})
      .borrowed_method<&gfx::Node::opacity>("opacity", {"a"})
      .borrowed_method<&gfx::Node::quat>("quat", {"x", "y", "z", "w"})
      .borrowed_method<&gfx::Node::billboard>("billboard", {{"on", true}})
      .borrowed_method<&gfx::Node::cull_radius>("cull_radius", {"r"})
      .borrowed_method<&gfx::Node::remove>("remove")
      .method<&gfx::Node::child_count>("child_count")
      .method<&gfx::Node::child_at>("child_at", {"i"})
      .method<&gfx::Node::find>("find", {"name"})
      .method<&gfx::Node::has>("has", {"name"})
      .method<&gfx::Node::vertex_count>("vertex_count")
      .method<&gfx::Node::world_x>("world_x")
      .method<&gfx::Node::world_y>("world_y")
      .method<&gfx::Node::world_z>("world_z")
      .borrowed_method<&gfx::Node::set_name>("name", {"n"})
      .borrowed_method<&gfx::Node::hide>("hide")
      .borrowed_method<&gfx::Node::show>("show")
      .borrowed_method<&gfx::Node::vertex>("vertex", {"x", "y", "z", "nx", "ny", "nz"})
      .borrowed_method<&gfx::Node::vertex_uv>("vertex_uv", {"x", "y", "z", "nx", "ny", "nz", "u", "v"})
      .borrowed_method<&gfx::Node::tri>("tri", {"a", "b", "c"})
      .borrowed_method<&gfx::Node::build>("build")
      .method<&gfx::Node::child>("add_node")
      .method<&gfx::Node::add_box>("add_box", {"w", "h", "d"})
      .method<&gfx::Node::add_sphere>("add_sphere", {"r"})
      .method<&gfx::Node::add_cylinder>("add_cylinder", {"r", "h"})
      .method<&gfx::Node::add_plane>("add_plane", {"w", "d"})
      .method<&gfx::Node::add_mesh>("add_mesh")
      .method<&gfx::Node::x>("x")
      .method<&gfx::Node::y>("y")
      .method<&gfx::Node::z>("z");

  culebra::wrap<gfx::View>("Scene", "View")
      .ctor<long, long, std::string>({"w", "h", "title"})
      .method<&gfx::View::target_fps>("target_fps", {"fps"})
      .method<&gfx::View::closing>("closing")
      .method<&gfx::View::dt>("dt")
      .method<&gfx::View::width>("width")
      .method<&gfx::View::height>("height")
      .method<&gfx::View::fps>("fps")
      .method<&gfx::View::time>("time")
      .method<&gfx::View::fullscreen>("fullscreen", {"on"})
      .method<&gfx::View::is_fullscreen>("is_fullscreen")
      .method<&gfx::View::resizable>("resizable", {"on"})
      .method<&gfx::View::resized>("resized")
      .method<&gfx::View::size>("size", {"w", "h"})
      .method<&gfx::View::title>("title", {"s"})
      .method<&gfx::View::vsync>("vsync", {"on"})
      .method<&gfx::View::cursor>("cursor", {"on"})
      .method<&gfx::View::mouse_capture>("mouse_capture", {"on"})
      .method<&gfx::View::clipboard>("clipboard")
      .method<&gfx::View::set_clipboard>("set_clipboard", {"s"})
      .method<&gfx::View::post>("post", {"on"})
      .method<&gfx::View::exposure>("exposure", {"k"})
      .method<&gfx::View::saturation>("saturation", {"k"})
      .method<&gfx::View::bloom>("bloom", {"threshold", "strength"})
      .method<&gfx::View::dof>("dof", {"strength", "range"})
      .method<&gfx::View::ssao>("ssao", {"strength", "radius"})
      .method<&gfx::View::vignette>("vignette", {"k"})
      .method<&gfx::View::lut>("lut", {"tex", {"amount", 1.0}})
      .method<&gfx::View::supersample>("supersample", {"n"})
      .method<&gfx::View::clip_planes>("clip_planes", {"near", "far"})
      .method<&gfx::View::mouse_x>("mouse_x")
      .method<&gfx::View::mouse_y>("mouse_y")
      .method<&gfx::View::mouse_dx>("mouse_dx")
      .method<&gfx::View::mouse_dy>("mouse_dy")
      .method<&gfx::View::mouse_wheel>("mouse_wheel")
      .method<&gfx::View::mouse>("mouse", {"button"})
      .method<&gfx::View::mouse_pressed>("mouse_pressed", {"button"})
      .method<&gfx::View::quit>("quit")
      .method<&gfx::View::key>("key", {"name"})
      .method<&gfx::View::key_pressed>("key_pressed", {"name"})
      .method<&gfx::View::key_released>("key_released", {"name"})
      .method<&gfx::View::pad_available>("pad_available", {{"index", 0}})
      .method<&gfx::View::pad_axis>("pad_axis", {"name", {"index", 0}})
      .method<&gfx::View::pad>("pad", {"name", {"index", 0}})
      .method<&gfx::View::pad_pressed>("pad_pressed", {"name", {"index", 0}})
      .method<&gfx::View::pad_name>("pad_name", {{"index", 0}})
      .method<&gfx::View::gamepad_mappings>("gamepad_mappings", {"db"})
      .method<&gfx::View::rumble>("rumble", {"left", "right", "sec", {"index", 0}})
      .method<&gfx::View::background>("background", {"r", "g", "b"})
      .method<&gfx::View::sky>("sky", {"tr", "tg", "tb", "br", "bg", "bb"})
      .method<&gfx::View::fog>("fog", {"start", "end", "r", "g", "b"})
      .method<&gfx::View::screenshot>("screenshot", {"path"})
      .method<&gfx::View::sun>("sun", {"dx", "dy", "dz", "intensity", "r", "g", "b"})
      .method<&gfx::View::ambient>("ambient", {"intensity", "r", "g", "b"})
      .method<&gfx::View::add_material>("add_material")
      .method<&gfx::View::texture>("texture", {"img", {"mipmaps", true}, {"repeat", true}})
      .method<&gfx::View::texture_png>("texture_png", {"bytes"})
      .method<&gfx::View::checker>("checker", {"px", "checks", "r1", "g1", "b1", "r2", "g2", "b2"})
      .method<&gfx::View::grain>("grain", {"px", "r", "g", "b", "amt"})
      .method<&gfx::View::canvas>("canvas", {"w", "h"})
      .method<&gfx::View::canvas_end>("canvas_end")
      .method<&gfx::View::render_target>("render_target", {"w", "h"})
      .method<&gfx::View::render_to>(
          "render_to", {"tex", "px", "py", "pz", "tx", "ty", "tz", "ux", "uy", "uz", "fov"})
      .method<&gfx::View::font>("font", {"path", "size", {"chars", ""}})
      .method<&gfx::View::font_bytes>("font_bytes", {"data", "size", {"chars", ""}})
      .method<&gfx::View::add_node>("add_node")
      .method<&gfx::View::remove>("remove", {"node"})
      .method<&gfx::View::find>("find", {"name"})
      .method<&gfx::View::has>("has", {"name"})
      .method<&gfx::View::culling>("culling", {"on"})
      .method<&gfx::View::add_box>("add_box", {"w", "h", "d"})
      .method<&gfx::View::add_sphere>("add_sphere", {"r"})
      .method<&gfx::View::add_cylinder>("add_cylinder", {"r", "h"})
      .method<&gfx::View::add_plane>("add_plane", {"w", "d"})
      .method<&gfx::View::add_mesh>("add_mesh")
      .method<&gfx::View::camera>(
          "camera", {"px", "py", "pz", "tx", "ty", "tz", "ux", "uy", "uz", "fov"})
      .method<&gfx::View::render_3d>("render_3d")
      .method<&gfx::View::begin_2d>("begin_2d")
      .method<&gfx::View::present>("present")
      .method<&gfx::View::alpha>("alpha", {"a"})
      .method<&gfx::View::text>("text", {"s", "x", "y", "size", "r", "g", "b", {"font", nullptr},
                                         {"spacing", 0.0}, {"rot", 0.0}})
      .method<&gfx::View::text_width>("text_width", {"s", "size", {"font", nullptr}, {"spacing", 0.0}})
      .method<&gfx::View::text_height>("text_height", {"s", "size", {"font", nullptr}, {"spacing", 0.0}})
      .method<&gfx::View::rect>("rect", {"x", "y", "w", "h", "r", "g", "b"})
      .method<&gfx::View::rect_line>("rect_line", {"x", "y", "w", "h", "thick", "r", "g", "b"})
      .method<&gfx::View::rect_round>("rect_round", {"x", "y", "w", "h", "roundness", "r", "g", "b"})
      .method<&gfx::View::rect_round_line>("rect_round_line",
                                           {"x", "y", "w", "h", "roundness", "thick", "r", "g", "b"})
      .method<&gfx::View::rect_gradient>("rect_gradient", {"x", "y", "w", "h", "r1", "g1", "b1", "r2", "g2", "b2",
                                                           {"horizontal", false}})
      .method<&gfx::View::circle>("circle", {"x", "y", "radius", "r", "g", "b"})
      .method<&gfx::View::circle_line>("circle_line", {"x", "y", "radius", "r", "g", "b"})
      .method<&gfx::View::circle_gradient>("circle_gradient", {"x", "y", "radius", "r1", "g1", "b1", "r2", "g2", "b2"})
      .method<&gfx::View::ring>("ring", {"x", "y", "r_in", "r_out", "a0", "a1", "r", "g", "b"})
      .method<&gfx::View::line>("line", {"x0", "y0", "x1", "y1", "thick", "r", "g", "b"})
      .method<&gfx::View::triangle>("triangle", {"x0", "y0", "x1", "y1", "x2", "y2", "r", "g", "b"})
      .method<&gfx::View::poly>("poly", {"x", "y", "sides", "radius", "rot", "r", "g", "b"})
      .method<&gfx::View::sprite>("sprite", {"tex", "x", "y", "w", "h", {"rot", 0.0}, {"ox", 0.0}, {"oy", 0.0},
                                             {"r", 255}, {"g", 255}, {"b", 255}})
      .method<&gfx::View::sprite_rec>("sprite_rec", {"tex", "sx", "sy", "sw", "sh", "x", "y", "w", "h",
                                                     {"rot", 0.0}, {"ox", 0.0}, {"oy", 0.0}})
      .method<&gfx::View::clip>("clip", {"x", "y", "w", "h"})
      .method<&gfx::View::clip_end>("clip_end")
      .method<&gfx::View::path_begin>("path_begin")
      .method<&gfx::View::path_to>("path_to", {"x", "y"})
      .method<&gfx::View::path_close>("path_close")
      .method<&gfx::View::path_fill>("path_fill", {"r", "g", "b"})
      .method<&gfx::View::path_strip>("path_strip", {"r", "g", "b"})
      .method<&gfx::View::path_stroke>("path_stroke", {"thick", "r", "g", "b"})
      .method<&gfx::View::path_spline>("path_spline", {"thick", "r", "g", "b"});

  culebra::wrap<gfx::SoundFx>("Scene", "Sound")
      .ctor<std::string>({"path"})
      .method<&gfx::SoundFx::play>("play")
      .method<&gfx::SoundFx::stop>("stop")
      .method<&gfx::SoundFx::playing>("playing")
      .method<&gfx::SoundFx::volume>("volume", {"v"})
      .method<&gfx::SoundFx::pitch>("pitch", {"p"})
      .method<&gfx::SoundFx::pan>("pan", {"p"});

  culebra::wrap<gfx::MusicTrack>("Scene", "Music")
      .ctor<std::string>({"path"})
      .method<&gfx::MusicTrack::play>("play")
      .method<&gfx::MusicTrack::stop>("stop")
      .method<&gfx::MusicTrack::pause>("pause")
      .method<&gfx::MusicTrack::resume>("resume")
      .method<&gfx::MusicTrack::update>("update")
      .method<&gfx::MusicTrack::playing>("playing")
      .method<&gfx::MusicTrack::looping>("looping", {"on"})
      .method<&gfx::MusicTrack::volume>("volume", {"v"})
      .method<&gfx::MusicTrack::pitch>("pitch", {"p"})
      .method<&gfx::MusicTrack::pan>("pan", {"p"});

  culebra::wrap<gfx::Audio>("Scene", "Audio")
      .ctor<long, long, long>({"rate", "channels", "buffer"})
      .method<&gfx::Audio::ready>("ready")
      .method<&gfx::Audio::needed>("needed")
      .method<&gfx::Audio::push>("push", {"s"})
      .method<&gfx::Audio::push2>("push2", {"l", "r"})
      .method<&gfx::Audio::pending>("pending")
      .method<&gfx::Audio::submit>("submit")
      .method<&gfx::Audio::dropped>("dropped")
      .method<&gfx::Audio::latency>("latency")
      .method<&gfx::Audio::play>("play")
      .method<&gfx::Audio::stop>("stop")
      .method<&gfx::Audio::pause>("pause")
      .method<&gfx::Audio::resume>("resume")
      .method<&gfx::Audio::playing>("playing")
      .method<&gfx::Audio::volume>("volume", {"v"})
      .method<&gfx::Audio::pitch>("pitch", {"p"})
      .method<&gfx::Audio::pan>("pan", {"p"});
  return true;
}();
}  // namespace
