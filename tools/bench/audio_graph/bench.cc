// What Audio.Graph's engine (include/stdlib/audio.h) answers and what that
// costs, for a fixed set of graphs, without building culebra: the check to
// run before the dev build when audio.h changes. `just bench-audio-graph
// <ref>` (run.sh) builds this file and the ref's own copy against their own
// headers and lays the two side by side: whether each graph renders the same
// samples, and the time a sample takes now against then.
//
// It reads the offline render only (a copy of the graph worked out from its
// start, as `render` and `play` do), not the playing engine's buffers or the
// checks bindings.h makes on what a program gives. The graphs are built with
// the calls bindings.h makes and keep their values in the ranges it allows.
//
// A graph here, once committed, never changes: a comparison matches graphs
// by name across versions, so a new shape is a new graph with a new name.
//
//   bench                 one line per graph: parts, hash, ns a sample
//   bench --time R        the same, the best of R renders
//   bench --samples DIR   each graph's samples, as raw doubles, to DIR/<name>.f64
//   bench --compare A B   per graph, whether DIR A and DIR B hold the same samples
//   bench --quick         a short render of each, failing on silence or a non-finite sample

#include <stdlib/audio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ad = culebra::_audio_detail;

constexpr double kRate = ad::kMixerRate;
// The bindings' curved segment (synth.cul's CURVE_STEPS): a curve of 20 ramps.
constexpr int kCurveSteps = 20;

// A graph as a program builds it through bindings.h's calls.
struct Build {
  ad::Graph g{kRate};
  int one = -1;

  int wave(ad::WaveKind kind, double freq, double feedback = 0, double phase = 0) {
    return g.add(ad::PartKind::Wave, {}, ad::Oscillator{kind}, freq, feedback, phase);
  }
  int noise(int64_t seed) { return g.add(ad::PartKind::Noise, {}, ad::Noise{seed}); }
  int filter(ad::FilterKind kind, std::vector<int> in, double cutoff, double q) {
    return g.add(ad::PartKind::Filter, std::move(in), ad::Svf(kind, q), cutoff);
  }
  int gain(std::vector<int> in, double gain) {
    return g.add(ad::PartKind::Gain, std::move(in), std::monostate{}, gain);
  }
  int delay(std::vector<int> in, double time) {
    return g.add(ad::PartKind::Delay, std::move(in), ad::graph_delay_line(kRate), time);
  }
  int pluck(std::vector<int> in, double freq, double ring) {
    return g.add(ad::PartKind::Pluck, std::move(in), ad::graph_string(kRate), freq, ring);
  }
  int resonate(std::vector<int> in, const std::vector<double>& modes, double dry) {
    return g.add(ad::PartKind::Resonate, std::move(in),
                 ad::Resonance(modes.data(), static_cast<int64_t>(modes.size() / 3), dry, kRate));
  }
  int drive(std::vector<int> in, double gain) { return g.add(ad::PartKind::Drive, std::move(in), ad::Drive{}, gain); }
  int limit(std::vector<int> in, double ceiling, double release) {
    return g.add(ad::PartKind::Limit, std::move(in), ad::Limiter(ceiling, release, kRate));
  }
  int impulse(double freq, double phase) { return g.add(ad::PartKind::Impulse, {}, ad::Impulse(phase), freq); }
  int dust(double density, int64_t seed) {
    return g.add(ad::PartKind::Dust, {}, ad::Dust{ad::noise_stream(seed, ad::kDustStream)}, density);
  }
  int lfnoise(ad::LfNoiseKind kind, double freq, int64_t seed) {
    return g.add(ad::PartKind::LfNoise, {}, ad::LfNoise(kind, ad::noise_stream(seed, ad::kLfNoiseStream)), freq);
  }
  int lag(std::vector<int> in, double time) { return g.add(ad::PartKind::Lag, std::move(in), ad::Lag(time, kRate)); }
  int decay(std::vector<int> in, double time) {
    return g.add(ad::PartKind::Decay, std::move(in), ad::Decay(time, kRate));
  }
  int math(ad::MathKind kind, std::vector<int> in, double lo = -1, double hi = 1) {
    return g.add(ad::PartKind::Math, std::move(in), ad::MathOp{kind, lo, hi});
  }
  int room(std::vector<int> in, double size, double damp, int side) {
    return g.add(ad::PartKind::Room, std::move(in), ad::ReverbTank(kRate, side), size, damp);
  }
  // Synth's constant: a gain of a square at 0 Hz, which answers 1.
  int level(double v) {
    if (one < 0) one = wave(ad::WaveKind::Square, 0);
    return gain({one}, v);
  }
  void connect(int from, int to, int value) {
    if (!g.connect(from, to, value)) {
      std::fprintf(stderr, "bench: a connection closed a loop with no delay\n");
      std::exit(2);
    }
  }
  void set(int part, int value, double at, double v) { g.schedule(part, value, ad::ParamEvent{at, false, v}); }
  void ramp(int part, int value, double at, double v) { g.schedule(part, value, ad::ParamEvent{at, true, v}); }
  void cancel(int part, int value, double at) { g.schedule(part, value, g.held(part, value, at)); }
  // A segment as synth.cul's `segment` sends it: a curve number c bends it,
  // as kCurveSteps ramps.
  void curve(int part, int value, double t, double l0, double l1, double len, double c) {
    for (int j = 1; j <= kCurveSteps; j++) {
      double x = j / double(kCurveSteps);
      ramp(part, value, t + len * x, l0 + (l1 - l0) * (1 - std::exp(c * x)) / (1 - std::exp(c)));
    }
  }
};

// About 19 parts with no loop, the block path: four sine pairs, each a
// modulator into its carrier's phase (FM as a DX7 does), their attack and
// decay envelopes retriggered twice a second, mixed, filtered and limited:
// the shape of a 19-part electric piano.
ad::Graph fm_piano() {
  Build b;
  const double ratios[] = {1.0, 2.0, 3.0, 0.5};
  std::vector<int> carriers;
  for (double r : ratios) {
    int mod = b.wave(ad::WaveKind::Sine, 220 * r * 2);
    int index = b.gain({mod}, 0);
    int car = b.wave(ad::WaveKind::Sine, 220 * r);
    b.connect(index, car, 2);
    int amp = b.gain({car}, 0);
    for (int n = 0; n < 8; n++) {
      double t = n * 0.5;
      b.set(index, 0, t, 0);
      b.ramp(index, 0, t + 0.005, 2.0);
      b.ramp(index, 0, t + 0.4, 0.3);
      b.set(amp, 0, t, 0);
      b.ramp(amp, 0, t + 0.005, 0.2);
      b.ramp(amp, 0, t + 0.45, 0);
    }
    carriers.push_back(amp);
  }
  int mix = b.gain(carriers, 1.0);
  int low = b.filter(ad::FilterKind::Low, {mix}, 3000, 0.707);
  b.g.output(b.limit({low}, 0.9, 0.2));
  return std::move(b.g);
}

// fm_piano with its output fed back, through a 1 ms delay, into every
// modulator's phase: the whole graph is one loop, worked out a sample at a
// time.
ad::Graph fm_piano_looped() {
  Build b;
  const double ratios[] = {1.0, 2.0, 3.0, 0.5};
  std::vector<int> carriers, modulators;
  for (double r : ratios) {
    int mod = b.wave(ad::WaveKind::Sine, 220 * r * 2);
    modulators.push_back(mod);
    int index = b.gain({mod}, 0);
    int car = b.wave(ad::WaveKind::Sine, 220 * r);
    b.connect(index, car, 2);
    int amp = b.gain({car}, 0);
    for (int n = 0; n < 8; n++) {
      double t = n * 0.5;
      b.set(index, 0, t, 0);
      b.ramp(index, 0, t + 0.005, 2.0);
      b.ramp(index, 0, t + 0.4, 0.3);
      b.set(amp, 0, t, 0);
      b.ramp(amp, 0, t + 0.005, 0.2);
      b.ramp(amp, 0, t + 0.45, 0);
    }
    carriers.push_back(amp);
  }
  int mix = b.gain(carriers, 1.0);
  int low = b.filter(ad::FilterKind::Low, {mix}, 3000, 0.707);
  int out = b.limit({low}, 0.9, 0.2);
  int back = b.gain({b.delay({out}, 0.001)}, 0.05);
  for (int mod : modulators) b.connect(back, mod, 2);
  b.g.output(out);
  return std::move(b.g);
}

// An echo whose loop holds a filter and a second, short delay: a saw into a
// mix fed back through a 0.3 s delay, a lowpass and a gain (the loop path).
ad::Graph echo_loop() {
  Build b;
  int saw = b.gain({b.wave(ad::WaveKind::Saw, 220)}, 0);
  for (int n = 0; n < 4; n++) {
    b.set(saw, 0, n * 1.0, 0.3);
    b.set(saw, 0, n * 1.0 + 0.1, 0);
  }
  int both = b.gain({saw}, 1.0);
  int line = b.delay({both}, 0.3);
  int low = b.filter(ad::FilterKind::Low, {line}, 2000, 0.9);
  int back = b.gain({low}, 0.6);
  int short_line = b.delay({back}, 37.0 / kRate);
  b.connect(short_line, both, -1);
  b.g.output(both);
  return std::move(b.g);
}

// Strings: noise bursts strike a string retuned with each note (and bent
// up through it, the tuning moving every sample), through a body of
// resonances, a drive and a limit.
ad::Graph strings() {
  Build b;
  int burst = b.gain({b.noise(7)}, 0);
  int string = b.pluck({burst}, 110, 1.5);
  const double notes[] = {110, 146.8, 164.8, 220, 196, 146.8, 130.8, 110};
  for (int n = 0; n < 16; n++) {
    double t = n * 0.25, f = notes[n % 8];
    b.set(burst, 0, t, 0.5);
    b.set(burst, 0, t + 0.004, 0);
    b.set(string, 0, t, f);
    b.ramp(string, 0, t + 0.2, f * 1.06);
  }
  const std::vector<double> body = {98, 4, 1.0, 196, 6, 0.6, 410, 12, 0.4};
  int bodied = b.resonate({string}, body, 0.5);
  b.g.output(b.limit({b.drive({bodied}, 2.0)}, 0.9, 0.2));
  return std::move(b.g);
}

// The other waves and the paths a sine alone does not take: a square and a
// triangle (their jumps smoothed), a sine with its own feedback (the serial
// path), and a resonant lowpass swept from 200 Hz to 6 kHz and back, so its
// cutoff moves, and is worked out again, every sample.
ad::Graph waves_swept() {
  Build b;
  int square = b.wave(ad::WaveKind::Square, 110);
  int triangle = b.wave(ad::WaveKind::Triangle, 165);
  int fed = b.wave(ad::WaveKind::Sine, 220, 0.8);
  int mix = b.gain({square, triangle, fed}, 0.2);
  int low = b.filter(ad::FilterKind::Low, {mix}, 200, 4.0);
  b.ramp(low, 0, 2.0, 6000);
  b.ramp(low, 0, 4.0, 200);
  b.g.output(low);
  return std::move(b.g);
}

// Eight voices built as Synth's Voices builds them (synth.cul: `_built`,
// `lower`, `feed`, `pitch_part`, `apply_note`, `segment`): each a saw whose
// freq a pitch level made after it is connected into, through a lowpass and
// an envelope gain, and a gate gain, all mixed; notes on and off as Voices
// sends them (cancels, a gate, the pitch, curved attack, decay and release),
// eight a second, each on the first voice free, as Voices chooses.
ad::Graph voices() {
  Build b;
  struct Voice {
    int pitch, env, gate;
    double free = 0;
  };
  std::vector<Voice> vs;
  std::vector<int> gates;
  for (int v = 0; v < 8; v++) {
    int saw = b.wave(ad::WaveKind::Saw, 0);
    int pitch = b.level(440);
    b.connect(pitch, saw, 0);
    int low = b.filter(ad::FilterKind::Low, {saw}, 1500, 0.707);
    int env = b.gain({low}, 0);
    int gate = b.gain({env}, 0);
    vs.push_back({pitch, env, gate});
    gates.push_back(gate);
  }
  b.g.output(b.gain(gates, 1.0));
  // adsr(0.01, 0.1, 0.6, 0.2, peak 0.15), curve -4: a note on runs the attack
  // and the decay, a note off the release from the sustain.
  for (int n = 0; n < 32; n++) {
    double on = n * 0.125, off = on + 0.6;
    Voice& v = *std::find_if(vs.begin(), vs.end(), [on](const Voice& x) { return x.free <= on; });
    v.free = off + 0.2;
    double freq = 220 * std::exp2((n * 5 % 24) / 12.0);
    b.cancel(v.gate, 0, on);
    b.cancel(v.pitch, 0, on);
    b.cancel(v.env, 0, on);
    b.set(v.gate, 0, on, 1);
    b.set(v.pitch, 0, on, freq);
    b.set(v.env, 0, on, 0);
    b.curve(v.env, 0, on, 0, 0.15, 0.01, -4);
    b.curve(v.env, 0, on + 0.01, 0.15, 0.09, 0.1, -4);
    b.cancel(v.env, 0, off);
    b.curve(v.env, 0, off, 0.09, 0, 0.2, -4);
  }
  return std::move(b.g);
}

// One strike and then silence: a string, a body and an echo decaying to
// nothing a little after a second, and from there on the states each part
// lets go once they are too small to hear (`settled`), for the rest of the
// render.
ad::Graph decay_tail() {
  Build b;
  int burst = b.gain({b.noise(3)}, 0);
  b.set(burst, 0, 0.0, 0.6);
  b.set(burst, 0, 0.005, 0);
  int string = b.pluck({burst}, 196, 0.05);
  const std::vector<double> body = {196, 60, 1.0, 392, 80, 0.5};
  int bodied = b.resonate({string}, body, 0.3);
  int both = b.gain({bodied}, 1.0);
  int back = b.gain({b.delay({both}, 0.05)}, 0.01);
  b.connect(back, both, -1);
  b.g.output(both);
  return std::move(b.g);
}

// The sources and the shaping parts as Synth builds them (synth.cul):
// impulses through decay2 (two decays, one taken off the other) as the level
// of a sine whose freq a cubic lfnoise moves (`.range`), dust through a
// bandpass, a pulse whose width a linear lfnoise moves (two saws, one's phase
// moved by the width, and the step the width makes), a stepped lfnoise
// lagged, taken through `.exprange` (`exp`), and a sine's size and its clip.
ad::Graph sources() {
  Build b;
  int ticks = b.impulse(4.0, 0.0);
  int env = b.gain({b.decay({ticks}, 0.3), b.gain({b.decay({ticks}, 0.01)}, -1.0)}, 1.0);
  int sweep = b.lfnoise(ad::LfNoiseKind::Cubic, 8.0, 1);
  int sine = b.wave(ad::WaveKind::Sine, 0);
  b.connect(b.gain({sweep}, 300.0), sine, 0);
  b.connect(b.level(600.0), sine, 0);
  int ticked = b.gain({sine}, 0);
  b.connect(env, ticked, 0);
  int dust = b.filter(ad::FilterKind::Band, {b.dust(20.0, 1)}, 2000, 4.0);
  int width = b.gain({b.lfnoise(ad::LfNoiseKind::Linear, 0.5, 1)}, 0.4);
  int moved = b.level(0.5);
  int w = b.gain({width, moved}, 1.0);
  int early = b.wave(ad::WaveKind::Saw, 110);
  b.connect(b.gain({w}, -2.0 * std::numbers::pi), early, 2);
  int late = b.wave(ad::WaveKind::Saw, 110);
  int pulse = b.gain({early, b.gain({late}, -1.0), b.gain({w}, 2.0), b.level(-1.0)}, 0.1);
  int glide = b.lag({b.lfnoise(ad::LfNoiseKind::Step, 2.0, 2)}, 0.1);
  int curved = b.gain({b.math(ad::MathKind::Exp, {b.gain({b.gain({glide}, 0.5), b.level(0.5)}, std::log(8.0))})}, 0.05);
  int sized = b.math(ad::MathKind::Clip, {b.math(ad::MathKind::Abs, {b.wave(ad::WaveKind::Sine, 3.0)})}, 0.0, 0.7);
  int lifted = b.gain({curved}, 0);
  b.connect(sized, lifted, 0);
  b.g.output(b.limit({b.gain({ticked, dust, pulse, lifted}, 0.5)}, 0.9, 0.2));
  return std::move(b.g);
}

// Plucks in a room as `.reverb` builds one (synth.cul): both sides take in
// the dry sound, the size moved by a slow lfnoise, and each side's echoes
// mixed with the dry sound.
ad::Graph room() {
  Build b;
  int burst = b.gain({b.noise(5)}, 0);
  for (int k = 0; k < 4; k++) {
    b.set(burst, 0, 0.5 * k, 0.5);
    b.set(burst, 0, 0.5 * k + 0.004, 0);
  }
  int dry = b.pluck({burst}, 220, 0.8);
  int left = b.room({dry}, 0.5, 0.5, 0), right = b.room({dry}, 0.5, 0.5, 1);
  int moved = b.gain({b.lfnoise(ad::LfNoiseKind::Linear, 0.5, 3)}, 0.3);
  for (int side : {left, right}) b.connect(moved, side, 0);
  b.g.output(b.gain({b.gain({dry}, 0.67), b.gain({left}, 0.33), b.gain({right}, 0.33)}, 1.0));
  return std::move(b.g);
}

struct Case {
  const char* name;
  ad::Graph (*make)();
};
const Case kCases[] = {
    {"fm_piano", fm_piano},       {"fm_piano_looped", fm_piano_looped}, {"echo_loop", echo_loop},
    {"strings", strings},         {"waves_swept", waves_swept},         {"voices", voices},
    {"decay_tail", decay_tail},   {"sources", sources},                 {"room", room},
};

constexpr double kSeconds = 4.0;

std::vector<double> render(const ad::Graph& def, double seconds) {
  ad::Graph g = def;
  std::vector<double> out(static_cast<size_t>(seconds * kRate));
  g.render(out.data(), static_cast<int64_t>(out.size()));
  return out;
}

uint64_t hash(const std::vector<double>& xs) {
  uint64_t h = 1469598103934665603ull;
  for (double x : xs) {
    uint64_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    for (int i = 0; i < 8; i++) h = (h ^ ((bits >> (8 * i)) & 0xff)) * 1099511628211ull;
  }
  return h;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// The best of `repeats` renders into `out` (a fresh copy each, the copy
// outside the timing, so each renders the same samples), in ns a sample, and
// the best copy, in us.
std::pair<double, double> best_times(const ad::Graph& def, int repeats, std::vector<double>& out) {
  double render_best = 1e300, copy_best = 1e300;
  size_t n = out.size();
  for (int r = 0; r < repeats; r++) {
    auto t0 = std::chrono::steady_clock::now();
    ad::Graph g = def;
    copy_best = std::min(copy_best, seconds_since(t0));
    auto t1 = std::chrono::steady_clock::now();
    g.render(out.data(), static_cast<int64_t>(n));
    render_best = std::min(render_best, seconds_since(t1));
  }
  return {render_best / double(n) * 1e9, copy_best * 1e6};
}

int time_all(int repeats) {
  std::printf("graph\tparts\thash\tns_a_sample\tns_a_part_sample\tcopy_us\n");
  for (const Case& c : kCases) {
    ad::Graph def = c.make();
    std::vector<double> out(static_cast<size_t>(kSeconds * kRate));
    auto [ns, copy_us] = best_times(def, repeats, out);
    std::printf("%s\t%zu\t%016llx\t%.1f\t%.2f\t%.1f\n", c.name, def.size(),
                static_cast<unsigned long long>(hash(out)), ns, ns / double(def.size()), copy_us);
  }
  return 0;
}

int write_samples(const std::string& dir) {
  for (const Case& c : kCases) {
    auto xs = render(c.make(), kSeconds);
    std::ofstream f(dir + "/" + c.name + ".f64", std::ios::binary);
    f.write(reinterpret_cast<const char*>(xs.data()), static_cast<std::streamsize>(xs.size() * sizeof(double)));
    if (!f) {
      std::fprintf(stderr, "bench: cannot write %s/%s.f64\n", dir.c_str(), c.name);
      return 2;
    }
  }
  return 0;
}

std::vector<double> read_samples(const std::string& path, bool& found) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  found = static_cast<bool>(f);
  if (!found) return {};
  std::vector<double> xs(static_cast<size_t>(f.tellg()) / sizeof(double));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(xs.data()), static_cast<std::streamsize>(xs.size() * sizeof(double)));
  return xs;
}

// Per graph of this version: "same", or how far the samples in `b` stray
// from those in `a` (the largest difference, NaN when one side is, and the
// first sample that has one), or "only in one" for a graph one side lacks.
int compare(const std::string& a, const std::string& b) {
  std::printf("graph\tsamples\n");
  for (const Case& c : kCases) {
    bool found_a, found_b;
    auto xs = read_samples(a + "/" + c.name + ".f64", found_a);
    auto ys = read_samples(b + "/" + c.name + ".f64", found_b);
    if (!found_a || !found_b) {
      std::printf("%s\tonly in one\n", c.name);
      continue;
    }
    if (xs.size() != ys.size()) {
      std::printf("%s\tdiffer: %zu samples against %zu\n", c.name, xs.size(), ys.size());
      continue;
    }
    double largest = 0;
    size_t first = xs.size();
    for (size_t i = 0; i < xs.size(); i++) {
      bool differ = std::memcmp(&xs[i], &ys[i], sizeof(double)) != 0;
      if (differ && first == xs.size()) first = i;
      double d = std::abs(xs[i] - ys[i]);
      largest = std::isnan(d) ? d : std::max(largest, d);
    }
    if (first == xs.size())
      std::printf("%s\tsame\n", c.name);
    else
      std::printf("%s\tdiffer: largest %.3g, first at sample %zu\n", c.name, largest, first);
  }
  return 0;
}

// Each graph renders briefly, to finite samples that are not all silence.
int quick() {
  int bad = 0;
  for (const Case& c : kCases) {
    auto xs = render(c.make(), 0.25);
    bool finite = std::all_of(xs.begin(), xs.end(), [](double x) { return std::isfinite(x); });
    bool heard = std::any_of(xs.begin(), xs.end(), [](double x) { return x != 0; });
    if (!finite || !heard) {
      std::fprintf(stderr, "bench: %s renders %s\n", c.name, finite ? "silence" : "a non-finite sample");
      bad++;
    }
  }
  if (bad == 0) std::printf("bench-audio-graph OK (%zu graphs)\n", std::size(kCases));
  return bad == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) return time_all(5);
  if (args[0] == "--time" && args.size() == 2) return time_all(std::max(1, std::atoi(args[1].c_str())));
  if (args[0] == "--samples" && args.size() == 2) return write_samples(args[1]);
  if (args[0] == "--compare" && args.size() == 3) return compare(args[1], args[2]);
  if (args[0] == "--quick" && args.size() == 1) return quick();
  std::fprintf(stderr, "usage: bench [--time R | --samples DIR | --compare A B | --quick]\n");
  return 2;
}
