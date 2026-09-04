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

#include <cstdarg>
#include <cstdio>
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
out vec3 fragNormal;
out vec3 fragWorld;
out vec2 fragUV;
out vec4 fragColor;
void main() {
  fragNormal = normalize((matNormal * vec4(vertexNormal, 0.0)).xyz);
  fragWorld = (matModel * vec4(vertexPosition, 1.0)).xyz;
  fragUV = vertexTexCoord;
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
  vec3 N = normalize(fragNormal);
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

  // Distance fog toward fogColor (off when fogStart is set very large).
  float dist = length(fragWorld - viewPos);
  float fogF = clamp((dist - fogStart) / max(fogEnd - fogStart, 1.0), 0.0, 1.0);
  col = mix(col, fogColor, fogF);

  // Alpha carries linear camera depth (0=near .. 1=far) for the post pass's
  // SSAO + depth-of-field — packing it here avoids a second sampler on macOS.
  finalColor = vec4(col, clamp(dist / 3000.0, 0.0, 1.0));
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
out vec4 finalColor;
void main() {
  vec2 texel = aaScale / vec2(textureSize(texture0, 0));
  vec4 center = texture(texture0, fragTexCoord);
  float depth = center.a;
  float focus = texture(texture0, vec2(0.5, 0.5)).a;   // auto-focus on screen centre

  // --- depth of field: gentle blur by circle-of-confusion from |depth-focus|.
  // Subtle (cinematic), so the track stays readable; only the far background
  // and the very near foreground soften.
  float coc = clamp(abs(depth - focus) * 3.5, 0.0, 1.0);
  vec3 c = center.rgb;
  if (coc > 0.05) {
    vec3 acc = vec3(0.0);
    for (int i = 0; i < 8; i++) {
      float a = float(i) * 0.7853982;
      acc += texture(texture0, fragTexCoord + vec2(cos(a), sin(a)) * texel * coc * 2.0).rgb;
    }
    c = mix(c, acc / 8.0, coc * 0.85);
  }

  // --- SSAO (depth-only): darken where neighbours sit nearer the camera ----
  // Depth lives in an 8-bit alpha channel, so its quantization step is ~1/256.
  // The occlusion bias must clear that step, else iso-depth quantization
  // contours show up as banding lines across flat ground. Real geometry edges
  // jump far more than the bias, so contact AO survives.
  float occ = 0.0;
  for (int i = 0; i < 8; i++) {
    float a = float(i) * 0.7853982;
    float nd = texture(texture0, fragTexCoord + vec2(cos(a), sin(a)) * texel * 3.0).a;
    occ += step(nd, depth - 0.008);
  }
  c *= 1.0 - (occ / 8.0) * 0.45;

  // --- bloom (wide bright-pass bleed for a soft glow) ---
  vec3 bloom = vec3(0.0);
  for (int x = -2; x <= 2; x++)
    for (int y = -2; y <= 2; y++)
      bloom += max(texture(texture0, fragTexCoord + vec2(x, y) * texel * 4.5).rgb - 0.7, vec3(0.0));
  c += (bloom / 25.0) * 1.5;

  // --- tonemap + saturation ---
  c = vec3(1.0) - exp(-c * 1.35);
  float l = dot(c, vec3(0.299, 0.587, 0.114));
  c = mix(vec3(l), c, 1.10);
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
static Texture2D upload(Image im, bool mipmaps) {
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
  RenderTexture2D rt{};   // the open canvas's target; freed when it closes
  bool is_rt = false;
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
};

// A reusable material: tint + optional texture + PBR-ish response (metallic
// 0..1, roughness 0..1). Defaults are a matte dielectric. The setters are
// fluent, so a material is one expression: view.add_material().rgb(…).pbr(…).
// Arguments are const: a material only reads the texture it is given, so the
// call must not stale the caller's other borrows of that texture.
class Material : public std::enable_shared_from_this<Material> {
 public:
  Color color = WHITE;
  std::shared_ptr<const Texture> tex;
  float metallic = 0.0f;
  float roughness = 0.85f;

  Material& rgb(int64_t r, int64_t g, int64_t b) { color = col(r, g, b); return *this; }
  Material& pbr(double m, double r) { metallic = (float)m; roughness = (float)r; return *this; }
  Material& texture(const Texture* t) { tex = t ? t->shared_from_this() : nullptr; return *this; }
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

// Everything Node::render needs from the View that doesn't vary within a pass:
// raylib's shared material, the cached primitive meshes and the lit shader's
// per-node PBR uniform locations. `lit` is the one per-pass bit — the depth
// pass's shader reads only vertex positions, so resolving a node's
// colour/texture there is wasted work.
struct RenderCtx {
  ::Material& mat;   // raylib's, not gfx::Material
  const Mesh& cube;
  const Mesh& sphere;
  const Mesh& cyl;
  const Mesh& plane;
  Texture2D white;
  int loc_metallic;
  int loc_rough;
  bool lit;
};

class Node {
 public:
  Shape shape = Shape::Group;
  double w = 1, h = 1, d = 1, radius = 0.5;
  double px = 0, py = 0, pz = 0;
  double ex = 0, ey = 0, ez = 0;            // euler radians, ZYX
  double ax = 0, ay = 1, az = 0, ang = 0;   // axis-angle spin (radians)
  double scx = 1, scy = 1, scz = 1;
  Color color = WHITE;
  std::shared_ptr<const Material> mat;   // null = the inline tint alone
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
  Node& set_name(std::string n) { name = std::move(n); return *this; }
  Node& hide() { visible = false; return *this; }
  Node& show() { visible = true; return *this; }

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
      Matrix r = MatrixRotateZYX(Vector3{(float)ex, (float)ey, (float)ez});
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

  // Draw with the shared lit material (colour + texture set per node from the
  // material registry) and the cached primitive meshes the View passes in.
  void render(const Matrix& parent, const RenderCtx& ctx) const {
    if (!visible) return;
    Matrix world = MatrixMultiply(local(), parent);          // children inherit this
    Matrix draw = MatrixMultiply(shape_scale(), world);      // own mesh only
    if (ctx.lit) {
      Color c = color;
      Texture2D tex = ctx.white;
      float metallic = 0.0f, roughness = 0.85f;
      if (mat) {   // a material wins over the inline tint
        c = mat->color;
        metallic = mat->metallic;
        roughness = mat->roughness;
        // A texture from a closed View samples as white, the way a stale
        // mesh draws nothing (see Texture).
        if (mat->tex && mat->tex->live()) tex = mat->tex->tex;
      }
      ctx.mat.maps[MATERIAL_MAP_DIFFUSE].color = c;
      ctx.mat.maps[MATERIAL_MAP_DIFFUSE].texture = tex;
      SetShaderValue(ctx.mat.shader, ctx.loc_metallic, &metallic, SHADER_UNIFORM_FLOAT);
      SetShaderValue(ctx.mat.shader, ctx.loc_rough, &roughness, SHADER_UNIFORM_FLOAT);
    }
    switch (shape) {
      case Shape::Box: DrawMesh(ctx.cube, ctx.mat, draw); break;
      case Shape::Sphere: DrawMesh(ctx.sphere, ctx.mat, draw); break;
      case Shape::Cylinder: DrawMesh(ctx.cyl, ctx.mat, draw); break;
      case Shape::Plane: DrawMesh(ctx.plane, ctx.mat, draw); break;
      case Shape::Mesh:
        if (mesh_live()) DrawMesh(mesh_, ctx.mat, world);
        break;
      case Shape::Group: break;
    }
    for (const auto& ch : children) ch->render(world, ctx);
  }

 private:
  std::shared_ptr<Node> push(std::shared_ptr<Node> n) { children.push_back(n); return n; }

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
    sky_top_ = Color{135, 165, 205, 255};   // default reflected sky until sky() is called
    sky_bot_ = Color{182, 202, 224, 255};
    set_sky_uniform();
    set_fog_uniform();
    depth_ = LoadShaderFromMemory(kVS_DEPTH, kFS_DEPTH);
    post_ = LoadShaderFromMemory(0, kFS_POST);   // default 2D VS + our composite FS
    loc_aascale_ = GetShaderLocation(post_, "aaScale");
    set_aa_uniform();
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
    cube_ = GenMeshCube(1, 1, 1);
    sphere_ = GenMeshSphere(1, 16, 16);
    cyl_ = GenMeshCylinder(1, 1, 16);
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
  std::shared_ptr<Texture> register_texture(Image im) {
    auto t = std::make_shared<Texture>();
    t->tex = upload(std::move(im), true);
    return t;
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
    Image im = GenImageColor((int)px, (int)px, col(r, g, b));
    Image noise = GenImageWhiteNoise((int)px, (int)px, 0.5f);
    ImageColorTint(&noise, col(amt, amt, amt));
    ImageDrawImage(&im, noise, 0, 0, Color{255, 255, 255, 60});
    UnloadImage(noise);
    return register_texture(im);
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
    auto t = std::make_shared<Texture>();
    t->rt = LoadRenderTexture((int)w, (int)h);
    t->tex = t->rt.texture;
    t->is_rt = true;
    SetTextureFilter(t->tex, TEXTURE_FILTER_BILINEAR);
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
    // A render target is stored bottom-up; the present path flips it, but a
    // material samples it as-is. Bake the flip into a plain texture here —
    // which also frees the render target (and its depth buffer) early.
    Texture& t = *open_canvas_;
    Image im = LoadImageFromTexture(t.rt.texture);
    ImageFlipVertical(&im);
    UnloadRenderTexture(t.rt);
    t.rt = RenderTexture2D{};
    t.is_rt = false;
    t.tex = upload(std::move(im), false);
    open_canvas_.reset();
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
    auto ctx = pass_ctx(depth_mat_, false);
    for (const auto& n : roots_) n->render(id, ctx);
    EndMode3D();
    EndTextureMode();
    return MatrixMultiply(lv, lp);
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
    // near (low-alpha) fragments would blend away. Opaque 3D needs no blend.
    rlDisableColorBlend();
    BeginMode3D(cam_);
    // cascades are bound automatically via mat_.maps[SPECULAR/NORMAL]
    auto ctx = pass_ctx(mat_, true);
    for (const auto& n : roots_) n->render(id, ctx);
    EndMode3D();
    rlEnableColorBlend();
    EndTextureMode();

    // --- post pass at supersample resolution (SSAO/DoF/bloom/tonemap) -------
    int sw = scene_rt_.texture.width, sh = scene_rt_.texture.height;
    BeginTextureMode(post_rt_);
    BeginShaderMode(post_);
    // RenderTextures are y-flipped; a negative source height flips it upright.
    DrawTextureRec(scene_rt_.texture, Rectangle{0, 0, (float)sw, -(float)sh},
                   Vector2{0, 0}, WHITE);
    EndShaderMode();
    EndTextureMode();

    // --- downsample the supersampled result to the window (the AA step) -----
    // The 2D overlay (HUD) is drawn by the caller AFTER this, so it stays crisp.
    BeginDrawing();
    frame_open_ = true;
    DrawTexturePro(post_rt_.texture,
                   Rectangle{0, 0, (float)sw, -(float)sh},
                   Rectangle{0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()},
                   Vector2{0, 0}, 0.0f, WHITE);
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
    DrawTexturePro(tex.tex, rec(0, 0, tex.tex.width, tex.tex.height), rec(x, y, w, h),
                   Vector2{(float)ox, (float)oy}, (float)rot, col(r, g, b, alpha_));
  }
  void sprite_rec(const Texture& tex, double sx, double sy, double sw, double sh,
                  double x, double y, double w, double h, double rot, double ox, double oy) {
    if (!tex.live()) return;
    DrawTexturePro(tex.tex, rec(sx, sy, sw, sh), rec(x, y, w, h), Vector2{(float)ox, (float)oy},
                   (float)rot, col(255, 255, 255, alpha_));
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
  // Bind one pass's invariants. `m` is the pass's material (lit or depth).
  RenderCtx pass_ctx(::Material& m, bool lit) {
    return RenderCtx{.mat = m, .cube = cube_, .sphere = sphere_, .cyl = cyl_,
                     .plane = plane_, .white = white_,
                     .loc_metallic = loc_metallic_, .loc_rough = loc_rough_,
                     .lit = lit};
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
  Camera3D light_{};
  Mesh cube_{}, sphere_{}, cyl_{}, plane_{};
  int loc_dir_ = 0, loc_lcol_ = 0, loc_amb_ = 0;
  int loc_lvp0_ = 0, loc_lvp1_ = 0;
  int loc_viewpos_ = 0, loc_fogcol_ = 0, loc_fogstart_ = 0, loc_fogend_ = 0;
  int loc_skytop_ = 0, loc_skybot_ = 0;
  int loc_metallic_ = 0, loc_rough_ = 0;
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

}  // namespace gfx

namespace {
const bool registered = [] {
  culebra::wrap<gfx::Texture>("Scene", "Texture")
      .method<&gfx::Texture::width>("width")
      .method<&gfx::Texture::height>("height");

  culebra::wrap<gfx::Font>("Scene", "Font")
      .method<&gfx::Font::size>("size")
      .method<&gfx::Font::glyphs>("glyphs");

  culebra::wrap<gfx::Material>("Scene", "Material")
      .borrowed_method<&gfx::Material::rgb>("rgb", {"r", "g", "b"})
      .borrowed_method<&gfx::Material::pbr>("pbr", {"metallic", "roughness"})
      .borrowed_method<&gfx::Material::texture>("texture", {"tex"});

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
      .method<&gfx::View::checker>("checker", {"px", "checks", "r1", "g1", "b1", "r2", "g2", "b2"})
      .method<&gfx::View::grain>("grain", {"px", "r", "g", "b", "amt"})
      .method<&gfx::View::canvas>("canvas", {"w", "h"})
      .method<&gfx::View::canvas_end>("canvas_end")
      .method<&gfx::View::font>("font", {"path", "size", {"chars", ""}})
      .method<&gfx::View::font_bytes>("font_bytes", {"data", "size", {"chars", ""}})
      .method<&gfx::View::add_node>("add_node")
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
  return true;
}();
}  // namespace
