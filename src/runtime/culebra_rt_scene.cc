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

#include <wrap.h>

#include <memory>
#include <string>
#include <vector>

#include "raylib.h"     // vendored: vendor/raylib/src (added to include path by CMake)
#include "raymath.h"
#include "rlgl.h"       // FBO / shader / matrix stack for shadows

namespace gfx {

static Color col(long r, long g, long b, long a = 255) {
  return Color{(unsigned char)r, (unsigned char)g, (unsigned char)b,
               (unsigned char)a};
}

// 0-255 RGB to a normalized linear-ish Vector3, scaled by `k` (light intensity).
static Vector3 rgb01(long r, long g, long b, double k = 1.0) {
  return Vector3{(float)(r / 255.0 * k), (float)(g / 255.0 * k), (float)(b / 255.0 * k)};
}

// Bring the audio device up on first use (a View, Sound or Music) so scripts
// don't have to manage it. Idempotent.
static void ensure_audio() {
  if (!IsAudioDeviceReady()) InitAudioDevice();
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

// A reusable material: tint + optional texture + PBR-ish response (metallic 0..1,
// roughness 0..1). Defaults are a matte dielectric.
struct MatDesc { Color color; int tex = -1; float metallic = 0.0f; float roughness = 0.85f; };

// Lit-shader uniform locations for the per-node PBR params (set once at View
// construction; one window/shader per process for these examples).
static int g_loc_metallic = 0;
static int g_loc_rough = 0;

// A registry texture: either a plain Texture2D (checker/grain) or a render
// target the script drew into (canvas) — tracked so it's freed the right way.
struct TexEntry { Texture2D tex; RenderTexture2D rt{}; bool is_rt = false; };

// Texture ids are offset into a disjoint range from material ids, so that
// accidentally passing a texture id to node.material() (or vice versa) lands
// out of range and degrades to "untextured" instead of silently picking the
// wrong material/texture.
static constexpr long TEX_BASE = 1000000;

// Shadow map = a normal colour render target (RGBA8 colour + depth buffer).
// The depth pass writes packed depth into the colour texture, which we then
// sample reliably as a plain sampler2D in the lit pass.
static RenderTexture2D LoadShadowmap(int w, int h) {
  return LoadRenderTexture(w, h);
}

enum class Shape { Group, Box, Sphere, Cylinder, Plane, Mesh };

// Everything Node::render needs from the View that doesn't vary within a pass:
// the shared material, the cached primitive meshes and the material / texture
// registries. `lit` is the one per-pass bit — the depth pass's shader reads only
// vertex positions, so resolving a node's colour/texture there is wasted work.
struct RenderCtx {
  Material& mat;
  const Mesh& cube;
  const Mesh& sphere;
  const Mesh& cyl;
  const Mesh& plane;
  const std::vector<MatDesc>& mats;
  const std::vector<TexEntry>& texs;
  Texture2D white;
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
  long mat_id = -1;        // index into the View's material registry (-1 = inline color)
  bool visible = true;
  std::string name;
  std::vector<std::shared_ptr<Node>> children;

  // custom-mesh accumulation (shape == Mesh)
  std::vector<float> mv;   // vertices xyz
  std::vector<float> mn;   // normals xyz
  std::vector<float> mt;   // texcoords uv
  std::vector<long> mi;    // indices (range-checked at build())
  bool mesh_built = false;
  ::Model mesh_model{};

  // local-transform cache: recomputed only when a setter dirties it, so a
  // static subtree's matrix is built once, not 3x/frame across the passes.
  mutable Matrix cached_local_{};
  mutable bool local_dirty_ = true;

  ~Node() {
    if (mesh_built && IsWindowReady()) UnloadModel(mesh_model);
  }

  // fluent setters (transform setters dirty the cached local matrix)
  Node& move(double x, double y, double z) { px = x; py = y; pz = z; local_dirty_ = true; return *this; }
  Node& euler(double x, double y, double z) { ex = x; ey = y; ez = z; local_dirty_ = true; return *this; }
  Node& yaw(double a) { ey = a; local_dirty_ = true; return *this; }
  Node& roll(double a) { ez = a; local_dirty_ = true; return *this; }
  Node& pitch(double a) { ex = a; local_dirty_ = true; return *this; }
  Node& spin(double x, double y, double z, double a) { ax = x; ay = y; az = z; ang = a; local_dirty_ = true; return *this; }
  Node& scale(double s) { scx = scy = scz = s; local_dirty_ = true; return *this; }
  Node& scale3(double x, double y, double z) { scx = x; scy = y; scz = z; local_dirty_ = true; return *this; }
  Node& tint(long r, long g, long b) { color = col(r, g, b); return *this; }
  Node& material(long id) { mat_id = id; return *this; }
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
  Node& tri(long a, long b, long c) {
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
    long verts = (long)(mv.size() / 3);
    for (long i : mi) {
      if (i < 0 || i >= verts) {
        TraceLog(LOG_ERROR,
                 "Scene: triangle index %ld names no vertex (%ld pushed) — "
                 "mesh not built.", i, verts);
        return *this;
      }
    }
    ::Mesh m{};
    m.vertexCount = (int)(mv.size() / 3);
    m.triangleCount = (int)(mi.size() / 3);
    m.vertices = (float*)MemAlloc((unsigned)(mv.size() * sizeof(float)));
    for (size_t i = 0; i < mv.size(); i++) m.vertices[i] = mv[i];
    if (!mn.empty()) {
      m.normals = (float*)MemAlloc((unsigned)(mn.size() * sizeof(float)));
      for (size_t i = 0; i < mn.size(); i++) m.normals[i] = mn[i];
    }
    if (!mt.empty()) {
      m.texcoords = (float*)MemAlloc((unsigned)(mt.size() * sizeof(float)));
      for (size_t i = 0; i < mt.size(); i++) m.texcoords[i] = mt[i];
    }
    m.indices = (unsigned short*)MemAlloc((unsigned)(mi.size() * sizeof(unsigned short)));
    for (size_t i = 0; i < mi.size(); i++) m.indices[i] = (unsigned short)mi[i];
    UploadMesh(&m, false);
    if (mesh_built) UnloadModel(mesh_model);   // rebuilt: drop the older upload
    mesh_model = LoadModelFromMesh(m);
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
      if (mat_id >= 0 && (size_t)mat_id < ctx.mats.size()) {  // reusable material wins
        const MatDesc& md = ctx.mats[mat_id];
        c = md.color;
        metallic = md.metallic;
        roughness = md.roughness;
        if (md.tex >= 0 && (size_t)md.tex < ctx.texs.size())
          tex = ctx.texs[md.tex].tex;
      }
      ctx.mat.maps[MATERIAL_MAP_DIFFUSE].color = c;
      ctx.mat.maps[MATERIAL_MAP_DIFFUSE].texture = tex;
      SetShaderValue(ctx.mat.shader, g_loc_metallic, &metallic, SHADER_UNIFORM_FLOAT);
      SetShaderValue(ctx.mat.shader, g_loc_rough, &roughness, SHADER_UNIFORM_FLOAT);
    }
    switch (shape) {
      case Shape::Box: DrawMesh(ctx.cube, ctx.mat, draw); break;
      case Shape::Sphere: DrawMesh(ctx.sphere, ctx.mat, draw); break;
      case Shape::Cylinder: DrawMesh(ctx.cyl, ctx.mat, draw); break;
      case Shape::Plane: DrawMesh(ctx.plane, ctx.mat, draw); break;
      case Shape::Mesh:
        if (mesh_built) DrawMesh(mesh_model.meshes[0], ctx.mat, world);
        break;
      case Shape::Group: break;
    }
    for (const auto& ch : children) ch->render(world, ctx);
  }

 private:
  std::shared_ptr<Node> push(std::shared_ptr<Node> n) { children.push_back(n); return n; }
};

class View {
 public:
  View(long w, long h, std::string title) {
    // Quiet raylib's INFO/WARNING chatter (verbose GL/asset logs, and the
    // per-call gamepad-vibration "not available" warning on backends without
    // haptics). Keep errors only.
    SetTraceLogLevel(LOG_ERROR);
    InitWindow((int)w, (int)h, title.c_str());
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
    g_loc_metallic = GetShaderLocation(lit_, "metallic");
    g_loc_rough = GetShaderLocation(lit_, "roughness");
    loc_skytop_ = GetShaderLocation(lit_, "skyTop");
    loc_skybot_ = GetShaderLocation(lit_, "skyBot");
    sky_top_ = Color{135, 165, 205, 255};   // default reflected sky until sky() is called
    sky_bot_ = Color{182, 202, 224, 255};
    set_sky_uniform();
    set_fog_uniform();
    depth_ = LoadShaderFromMemory(kVS_DEPTH, kFS_DEPTH);
    post_ = LoadShaderFromMemory(0, kFS_POST);   // default 2D VS + our composite FS
    loc_aascale_ = GetShaderLocation(post_, "aaScale");
    float aa = (float)ss_;
    SetShaderValue(post_, loc_aascale_, &aa, SHADER_UNIFORM_FLOAT);
    // Supersampled targets: render the scene + post at ss_x resolution, then
    // box-downsample to the window for cheap, high-quality antialiasing.
    scene_rt_ = LoadRenderTexture((int)w * ss_, (int)h * ss_);
    post_rt_ = LoadRenderTexture((int)w * ss_, (int)h * ss_);
    SetTextureFilter(post_rt_.texture, TEXTURE_FILTER_BILINEAR);   // averages on downscale
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
      roots_.clear();                       // drop scene → ~Node unloads each mesh (window still up)
      UnloadRenderTexture(shadowmap0_);
      UnloadRenderTexture(shadowmap1_);
      UnloadRenderTexture(scene_rt_);
      UnloadRenderTexture(post_rt_);
      for (auto& e : texs_) { if (e.is_rt) UnloadRenderTexture(e.rt); else UnloadTexture(e.tex); }
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

  void target_fps(long fps) { SetTargetFPS((int)fps); }
  bool closing() const { return WindowShouldClose(); }
  double dt() const { return GetFrameTime(); }
  double width() const { return GetScreenWidth(); }
  double height() const { return GetScreenHeight(); }
  bool held(long key) const { return IsKeyDown((int)key); }
  bool pressed(long key) const { return IsKeyPressed((int)key); }
  // Gamepad 0 (Xbox / DualSense, mapped by SDL_GameControllerDB).
  bool pad_available() const { return IsGamepadAvailable(0); }
  double pad_axis(long axis) const { return (double)GetGamepadAxisMovement(0, (int)axis); }
  bool pad_button(long b) const { return IsGamepadButtonDown(0, (int)b); }
  bool pad_pressed(long b) const { return IsGamepadButtonPressed(0, (int)b); }
  std::string pad_name() const { const char* n = GetGamepadName(0); return n ? n : ""; }
  // Load SDL_GameControllerDB mapping lines for pads the bundled DB lacks
  // (newer controllers). Returns 1 on success.
  long gamepad_mappings(std::string db) { return SetGamepadMappings(db.c_str()); }
  // Rumble both motors at [0..1] for `sec` seconds. Works where the platform's
  // gamepad backend drives haptics (Windows/Linux XInput, Sony pads on macOS).
  // NOTE: Xbox controllers on macOS cannot rumble — no macOS API (SDL or Apple's
  // own GameController/CoreHaptics) drives their motors, so this is a no-op there.
  void rumble(double left, double right, double sec) {
    SetGamepadVibration(0, (float)left, (float)right, (float)sec);
  }
  void background(long r, long g, long b) { bg_ = col(r, g, b); }
  // Vertical sky gradient (top -> horizon), drawn behind the 3D scene.
  void sky(long tr, long tg, long tb, long br, long bg, long bb) {
    sky_top_ = col(tr, tg, tb);
    sky_bot_ = col(br, bg, bb);
    has_sky_ = true;
    set_sky_uniform();
  }
  // Distance fog: surfaces fade to (r,g,b) between `start` and `end` metres.
  void fog(double start, double end, long r, long g, long b) {
    fogcol_ = rgb01(r, g, b);
    fogstart_ = (float)start;
    fogend_ = (float)end;
    set_fog_uniform();
  }
  void screenshot(std::string path) { TakeScreenshot(path.c_str()); }

  // lighting
  void sun(double dx, double dy, double dz, double intensity, long r, long g, long b) {
    Vector3 d = Vector3Normalize(Vector3{(float)dx, (float)dy, (float)dz});
    dir_ = d;
    lcol_ = rgb01(r, g, b, intensity);
    set_sun_uniform();
  }
  void ambient(double intensity, long r, long g, long b) {
    amb_ = rgb01(r, g, b, intensity);
    set_sun_uniform();
  }

  // --- materials (reusable) + procedural textures -------------------------
  // The four script-facing entries differ only in which of tex / metallic /
  // roughness they expose; MatDesc's defaults cover the rest (matte dielectric,
  // untextured).
  long material(long r, long g, long b) { return add_material({col(r, g, b), -1}); }
  long material_tex(long tex, long r, long g, long b) {
    return add_material({col(r, g, b), tex_index(tex)});
  }
  // PBR variants: metallic 0..1, roughness 0..1 (glossy paint, matte rubber, metal).
  long material_pbr(long r, long g, long b, double metallic, double roughness) {
    return add_material({col(r, g, b), -1, (float)metallic, (float)roughness});
  }
  long material_tex_pbr(long tex, long r, long g, long b, double metallic, double roughness) {
    return add_material(
        {col(r, g, b), tex_index(tex), (float)metallic, (float)roughness});
  }
  // Upload a CPU image as a mipmapped, repeat-wrapped texture; register + return
  // its id. Mipmaps + trilinear keep tiled high-frequency textures (e.g. the
  // checker) from crawling/shimmering when minified under motion. Consumes `im`.
  long register_texture(Image im) {
    Texture2D t = LoadTextureFromImage(im);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    UnloadImage(im);
    texs_.push_back({t});
    return (long)texs_.size() - 1 + TEX_BASE;
  }
  // a checkerboard texture (px square, `checks` cells per side)
  long checker(long px, long checks, long r1, long g1, long b1, long r2, long g2, long b2) {
    long n = checks > 0 ? checks : 1;          // guard: avoid div-by-zero / 0-size cells
    int cell = (int)(px / n);
    if (cell < 1) cell = 1;
    return register_texture(GenImageChecked((int)px, (int)px, cell, cell,
                                            col(r1, g1, b1), col(r2, g2, b2)));
  }
  // a flat colour with white-noise grain mixed in (asphalt / grass grain)
  long grain(long px, long r, long g, long b, long amt) {
    Image im = GenImageColor((int)px, (int)px, col(r, g, b));
    Image noise = GenImageWhiteNoise((int)px, (int)px, 0.5f);
    ImageColorTint(&noise, col(amt, amt, amt));
    ImageDrawImage(&im, noise, 0, 0, Color{255, 255, 255, 60});
    UnloadImage(noise);
    return register_texture(im);
  }

  // Arbitrary texture drawn by the script with the 2D primitives: canvas(w,h)
  // opens an off-screen target (returns its texture id); the following text/
  // rect/circle/line calls draw INTO it; canvas_end() finalises it. This is the
  // "generate a texture procedurally" path (liveries, signage) — like SceneKit
  // drawing textures with Core Graphics.
  long canvas(long w, long h) {
    RenderTexture2D rt = LoadRenderTexture((int)w, (int)h);
    SetTextureFilter(rt.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(rt.texture, TEXTURE_WRAP_REPEAT);
    texs_.push_back({rt.texture, rt, true});
    BeginTextureMode(rt);
    ClearBackground(WHITE);
    return (long)texs_.size() - 1 + TEX_BASE;
  }
  void canvas_end() { EndTextureMode(); }

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
    // Push the near plane far out: the chase camera never sits within metres of
    // geometry, and raylib's 0.01 default crushes depth precision so coplanar
    // layers (road / white lines / kerbs / gravel) z-fight and flicker at range.
    rlSetClipPlanes(2.0, 8000.0);
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
    DrawTexturePro(post_rt_.texture,
                   Rectangle{0, 0, (float)sw, -(float)sh},
                   Rectangle{0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()},
                   Vector2{0, 0}, 0.0f, WHITE);
  }
  // Open a frame for 2D-only screens (menus / pause), with no 3D or shadow
  // passes — pair with present(). (render_3d() opens the frame for 3D scenes.)
  void begin2d() {
    BeginDrawing();
    ClearBackground(bg_);
  }
  void present() { EndDrawing(); }

  // Alpha for subsequent 2D draws (0..255). The RGBA contract: 2D colours take
  // r,g,b and this shared alpha, so HUD fades / translucent panels are possible
  // without a 4-arg variant of every draw call. Reset to 255 when done.
  void alpha(long a) { alpha_ = (a < 0 ? 0 : (a > 255 ? 255 : a)); }

  void text(std::string s, double x, double y, long size, long r, long g, long b) {
    DrawText(s.c_str(), (int)x, (int)y, (int)size, col(r, g, b, alpha_));
  }
  void rect(double x, double y, double w, double h, long r, long g, long b) {
    DrawRectangle((int)x, (int)y, (int)w, (int)h, col(r, g, b, alpha_));
  }
  void circle(double x, double y, double radius, long r, long g, long b) {
    DrawCircle((int)x, (int)y, (float)radius, col(r, g, b, alpha_));
  }
  void line(double x0, double y0, double x1, double y1, double thick, long r, long g, long b) {
    DrawLineEx(Vector2{(float)x0, (float)y0}, Vector2{(float)x1, (float)y1}, (float)thick, col(r, g, b, alpha_));
  }

 private:
  std::shared_ptr<Node> push(std::shared_ptr<Node> n) { roots_.push_back(n); return n; }
  long add_material(MatDesc m) {
    mats_.push_back(m);
    return (long)mats_.size() - 1;   // the id IS the index into mats_
  }
  // Texture ids are offset by TEX_BASE (see there); undo it to index texs_.
  static int tex_index(long tex) { return (int)(tex - TEX_BASE); }
  // Bind one pass's invariants. `m` is the pass's material (lit or depth).
  RenderCtx pass_ctx(Material& m, bool lit) {
    return RenderCtx{m, cube_, sphere_, cyl_, plane_,
                     mats_, texs_, white_, lit};
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
  long alpha_ = 255;        // shared alpha for 2D draws
  std::vector<std::shared_ptr<Node>> roots_;
  std::vector<MatDesc> mats_;
  std::vector<TexEntry> texs_;
  Texture2D white_{};
  Shader lit_{}, depth_{}, post_{};
  Material mat_{}, depth_mat_{};
  RenderTexture2D shadowmap0_{}, shadowmap1_{}, scene_rt_{}, post_rt_{};
  int ss_ = 2;              // supersample factor for antialiasing
  int loc_aascale_ = 0;
  Camera3D light_{};
  Mesh cube_{}, sphere_{}, cyl_{}, plane_{};
  int loc_dir_ = 0, loc_lcol_ = 0, loc_amb_ = 0;
  int loc_lvp0_ = 0, loc_lvp1_ = 0;
  int loc_viewpos_ = 0, loc_fogcol_ = 0, loc_fogstart_ = 0, loc_fogend_ = 0;
  int loc_skytop_ = 0, loc_skybot_ = 0;
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

// A streamed audio track for long / looping audio — engine note, ambience, BGM.
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
      .borrowed_method<&gfx::Node::material>("material", {"id"})
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
      .method<&gfx::View::held>("held", {"key"})
      .method<&gfx::View::pressed>("pressed", {"key"})
      .method<&gfx::View::pad_available>("pad_available")
      .method<&gfx::View::pad_axis>("pad_axis", {"axis"})
      .method<&gfx::View::pad_button>("pad_button", {"button"})
      .method<&gfx::View::pad_pressed>("pad_pressed", {"button"})
      .method<&gfx::View::pad_name>("pad_name")
      .method<&gfx::View::gamepad_mappings>("gamepad_mappings", {"db"})
      .method<&gfx::View::rumble>("rumble", {"left", "right", "sec"})
      .method<&gfx::View::background>("background", {"r", "g", "b"})
      .method<&gfx::View::sky>("sky", {"tr", "tg", "tb", "br", "bg", "bb"})
      .method<&gfx::View::fog>("fog", {"start", "end", "r", "g", "b"})
      .method<&gfx::View::screenshot>("screenshot", {"path"})
      .method<&gfx::View::sun>("sun", {"dx", "dy", "dz", "intensity", "r", "g", "b"})
      .method<&gfx::View::ambient>("ambient", {"intensity", "r", "g", "b"})
      .method<&gfx::View::material>("material", {"r", "g", "b"})
      .method<&gfx::View::material_tex>("material_tex", {"tex", "r", "g", "b"})
      .method<&gfx::View::material_pbr>("material_pbr", {"r", "g", "b", "metallic", "roughness"})
      .method<&gfx::View::material_tex_pbr>("material_tex_pbr", {"tex", "r", "g", "b", "metallic", "roughness"})
      .method<&gfx::View::checker>("checker", {"px", "checks", "r1", "g1", "b1", "r2", "g2", "b2"})
      .method<&gfx::View::grain>("grain", {"px", "r", "g", "b", "amt"})
      .method<&gfx::View::canvas>("canvas", {"w", "h"})
      .method<&gfx::View::canvas_end>("canvas_end")
      .method<&gfx::View::add_node>("add_node")
      .method<&gfx::View::add_box>("add_box", {"w", "h", "d"})
      .method<&gfx::View::add_sphere>("add_sphere", {"r"})
      .method<&gfx::View::add_cylinder>("add_cylinder", {"r", "h"})
      .method<&gfx::View::add_plane>("add_plane", {"w", "d"})
      .method<&gfx::View::add_mesh>("add_mesh")
      .method<&gfx::View::camera>(
          "camera", {"px", "py", "pz", "tx", "ty", "tz", "ux", "uy", "uz", "fov"})
      .method<&gfx::View::render_3d>("render_3d")
      .method<&gfx::View::begin2d>("begin2d")
      .method<&gfx::View::present>("present")
      .method<&gfx::View::alpha>("alpha", {"a"})
      .method<&gfx::View::text>("text", {"s", "x", "y", "size", "r", "g", "b"})
      .method<&gfx::View::rect>("rect", {"x", "y", "w", "h", "r", "g", "b"})
      .method<&gfx::View::circle>("circle", {"x", "y", "radius", "r", "g", "b"})
      .method<&gfx::View::line>("line", {"x0", "y0", "x1", "y1", "thick", "r", "g", "b"});

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
